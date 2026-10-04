#include "engine/FFmpegSource.h"

#include "engine/ImageSequenceSource.h"
#include "engine/RawVideoSource.h"

#include "engine/FrameCompare.h"
#include "engine/GpuFrame.h"
#include "engine/HwDevice.h"
#include "engine/Log.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <future>
#include <set>
#include <thread>

namespace scvp {
namespace {

// Frame threads add pipeline latency and a per-flush cost that dominates for small pictures
// (16 threads: ~17 ms to the first frame after a seek on 320x240 VP9 vs ~2 ms with one).
// About one thread per 0.2 MPixel, capped at FFmpeg's automatic maximum.
int autoDecoderThreads(int width, int height)
{
    const double mpix = double(std::max(width, 1)) * double(std::max(height, 1)) / 1e6;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(int(mpix / 0.2 + 0.5), 1, int(std::min(16u, hw)));
}

// Decoder + demuxer pair positioned in the stream. Every packet sent to the decoder carries its
// decode-order index in AVPacket.opaque; with AV_CODEC_FLAG_COPY_OPAQUE the decoder hands it
// back on the frame, so each output frame is mapped to its frame number through the index even
// when timestamps are missing, repeated or discontinuous, and even when the decoder drops
// frames (leading pictures after a seek, edit-list pre-roll).
class FFmpegDecodeSession final : public IDecodeSession {
public:
    FFmpegDecodeSession(const FFmpegSource& source, std::string path, InputOptions opts, SessionConfig cfg)
        : m_source(source)
        , m_path(std::move(path))
        , m_opts(std::move(opts))
        , m_cfg(std::move(cfg))
    {
    }

    bool open(std::string* error)
    {
        auto demux = openDemuxer(m_path, m_opts, error);
        if (!demux)
            return false;
        m_demux = std::move(*demux);
        m_byteSeek = prefersByteSeek(m_demux.fmt.get());
        // Raw demuxers make timestamps up (counting from wherever reading started).
        m_ptsReliable = !(m_demux.fmt->iformat->flags & AVFMT_NOTIMESTAMPS);
        if (!openCodec(m_cfg.hw.policy != HwPolicy::Off, error))
            return false;
        m_pkt = ff::makePacket();
        m_frame = ff::makeFrame();
        return true;
    }

    // Opens the decoder, with a hardware device when the policy allows and the codec and device
    // support it; otherwise in software.
    bool openCodec(bool tryHardware, std::string* error)
    {
        const AVStream* st = m_demux.videoStream();
        const AVCodecID id = st->codecpar->codec_id;
        m_hwType = AV_HWDEVICE_TYPE_NONE;
        m_hwPixFmt = AV_PIX_FMT_NONE;
        m_hwFailed = false;
        m_hwRejected = false;
        m_hwErrors = 0;
        m_verifyPending = false;
        const AVCodec* codec = nullptr;
        AVBufferRef* device = nullptr;
        if (tryHardware && m_cfg.hw.policy == HwPolicy::GpuFirst && !HwDevices::bitExactCodec(id))
            tryHardware = false;
        if (tryHardware) {
            std::vector<AVHWDeviceType> types;
            if (m_cfg.hw.policy == HwPolicy::Device)
                types.push_back(m_cfg.hw.device);
            else
                types = HwDevices::platformOrder();
            for (AVHWDeviceType t : types) {
                const AVCodec* c = chooseDecoder(id, true);
                const AVPixelFormat hwFmt = c ? hardwareFormat(c, t) : AV_PIX_FMT_NONE;
                if (hwFmt == AV_PIX_FMT_NONE)
                    continue;
                const HwVerdict v = HwDevices::instance().verdict(t, id, st->codecpar->format);
                if (m_cfg.hw.policy == HwPolicy::GpuFirst && v == HwVerdict::Differs)
                    continue;
                device = HwDevices::instance().get(t);
                if (!device)
                    continue;
                codec = c;
                m_hwType = t;
                m_hwPixFmt = hwFmt;
                m_verifyPending = m_cfg.hw.policy == HwPolicy::GpuFirst && v == HwVerdict::Unknown;
                break;
            }
        }
        if (!codec)
            codec = chooseDecoder(id, false);
        if (!codec) {
            if (error)
                *error = std::string("no decoder for ") + avcodec_get_name(id);
            return false;
        }
        m_ctx.reset(avcodec_alloc_context3(codec));
        if (!m_ctx || avcodec_parameters_to_context(m_ctx.get(), st->codecpar) < 0) {
            av_buffer_unref(&device);
            if (error)
                *error = "cannot configure decoder";
            return false;
        }
        m_ctx->pkt_timebase = st->time_base;
        m_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        m_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        m_ctx->opaque = this;
        if (m_cfg.pool)
            m_ctx->get_buffer2 = &FFmpegDecodeSession::getBuffer2;
        if (m_hwType != AV_HWDEVICE_TYPE_NONE) {
            m_ctx->hw_device_ctx = device;  // takes the reference
            m_ctx->get_format = &FFmpegDecodeSession::getFormat;
            m_ctx->extra_hw_frames = 4;
            m_ctx->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
            m_ctx->thread_count = 2;  // measured best with NVDEC; more only adds surfaces in flight
        } else {
            m_ctx->thread_count = m_cfg.threads > 0 ? m_cfg.threads : autoDecoderThreads(st->codecpar->width, st->codecpar->height);
        }
        const int err = avcodec_open2(m_ctx.get(), codec, nullptr);
        if (err < 0) {
            if (m_hwType != AV_HWDEVICE_TYPE_NONE) {
                SCVP_INFO("%s: hardware decoder open failed (%s), using software", m_path.c_str(), ff::errorString(err).c_str());
                return openCodec(false, error);
            }
            if (error)
                *error = "avcodec_open2: " + ff::errorString(err);
            return false;
        }
        m_ctxClean = true;
        return true;
    }

    static AVPixelFormat hardwareFormat(const AVCodec* codec, AVHWDeviceType type)
    {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                return AV_PIX_FMT_NONE;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type)
                return cfg->pix_fmt;
        }
    }

    // Offered the hardware format (device initialised for this stream): take it. Called again
    // without it when hardware initialisation failed: note the failure; next() then reopens the
    // decoder in software (the native AV1 decoder, for one, outputs nothing without hardware).
    static AVPixelFormat getFormat(AVCodecContext* ctx, const AVPixelFormat* fmts)
    {
        auto* self = static_cast<FFmpegDecodeSession*>(ctx->opaque);
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == self->m_hwPixFmt && !self->m_cfg.hw.simulateFailure)
                return *p;
        self->m_hwFailed = true;
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
                return *p;
        }
        return AV_PIX_FMT_NONE;
    }

    // Reopens the decoder in software and restarts from the last seek point.
    bool fallBackToSoftware()
    {
        if (!m_hwRejected)
            SCVP_WARN("%s: hardware decoding (%s) failed, switching to software", m_path.c_str(), HwDevices::name(m_hwType));
        std::string err;
        m_ctx.reset();
        if (!openCodec(false, &err)) {
            SCVP_ERROR("%s: %s", m_path.c_str(), err.c_str());
            return false;
        }
        const SeekPoint sp = m_lastSeek;
        return seekTo(sp) == DecodeStatus::Ok;
    }

    std::string hardwareDevice() const override
    {
        return m_hwType != AV_HWDEVICE_TYPE_NONE && !m_hwFailed ? HwDevices::name(m_hwType) : std::string();
    }

    DecodeStatus seekTo(const SeekPoint& sp) override
    {
        resetDecoder();
        m_lastSeek = sp;
        if (!sp.valid())
            return rewindToStart() ? DecodeStatus::Ok : DecodeStatus::Error;

        FrameIndexPtr idx = m_source.index();
        SeekPoint target = sp;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const LandResult r = seekAndLand(target, attempt);
            if (r == LandResult::Landed)
                return DecodeStatus::Ok;
            if (r == LandResult::Error)
                break;
            // Overshot: the demuxer could not get us to the key. Try from an earlier key.
            SeekPoint earlier = idx ? idx->seekPointBefore(target) : SeekPoint{};
            if (!earlier.valid())
                break;
            SCVP_DEBUG("seek overshoot at dec %d, retrying from dec %d", target.dec, earlier.dec);
            target = earlier;
        }
        SCVP_WARN("%s: seek to packet %d failed, decoding from the start", m_path.c_str(), sp.dec);
        resetDecoder();
        return rewindToStart() ? DecodeStatus::Ok : DecodeStatus::Error;
    }

    DecodeStatus next(FramePtr& out, const DecodeHints& hints) override
    {
        for (;;) {
            if (hints.cancelled())
                return DecodeStatus::Cancelled;
            if (m_hwFailed && m_hwType != AV_HWDEVICE_TYPE_NONE && !fallBackToSoftware())
                return DecodeStatus::Error;
            int ret = avcodec_receive_frame(m_ctx.get(), m_frame.get());
            if (ret == 0) {
                const bool hwFrame = m_frame->format == m_hwPixFmt && m_hwPixFmt != AV_PIX_FMT_NONE;
                const Label lb = label();
                if (lb.pres < 0) {
                    av_frame_unref(m_frame.get());
                    continue;  // a frame that is never shown
                }
                if (hwFrame && m_verifyPending) {
                    // A hardware picture before any key picture could be checked (the stream
                    // starts mid-GOP, say): GPU-first never shows unchecked pixels.
                    SCVP_INFO("%s: %s output could not be checked yet, decoding in software", m_path.c_str(),
                              HwDevices::name(m_hwType));
                    av_frame_unref(m_frame.get());
                    m_hwRejected = true;
                    m_hwFailed = true;
                    continue;
                }
                if (m_gpuBackoff > 0)
                    --m_gpuBackoff;
                if (hwFrame && hints.keepOnGpu && m_hwType == AV_HWDEVICE_TYPE_CUDA && m_gpuBackoff == 0 &&
                    (*hints.keepOnGpu)(lb.pres)) {
                    if (FramePtr g = keepOnDevice(lb)) {
                        m_hwErrors = 0;
                        out = std::move(g);
                        return DecodeStatus::Ok;
                    }
                    m_gpuBackoff = 64;  // device memory is short: download for a while
                }
                if (hwFrame && !download()) {
                    av_frame_unref(m_frame.get());
                    if (++m_hwErrors >= 3)
                        m_hwFailed = true;
                    continue;
                }
                m_hwErrors = 0;
                out = finish(lb);
                return DecodeStatus::Ok;
            }
            if (ret == AVERROR_EOF) {
                m_eof = true;
                return DecodeStatus::Eof;
            }
            if (ret != AVERROR(EAGAIN)) {
                SCVP_WARN("%s: decode error %s", m_path.c_str(), ff::errorString(ret).c_str());
                if (m_hwType != AV_HWDEVICE_TYPE_NONE && m_hwFailed)
                    continue;
                if (m_hwType != AV_HWDEVICE_TYPE_NONE && !m_hwFailed && ++m_hwErrors >= 3) {
                    m_hwFailed = true;  // persistent hardware errors: retry in software
                    continue;
                }
                return DecodeStatus::Error;
            }
            if (!feedOne(hints)) {
                if (m_hwFailed && m_hwType != AV_HWDEVICE_TYPE_NONE)
                    continue;  // the decoder rejects input once its hardware is gone: fall back
                return DecodeStatus::Error;
            }
        }
    }

    Position position() const override { return {m_nextDec, m_lastPres, m_eof}; }
    bool wasSkipped(int32_t dec) const override { return m_skipped.count(dec) != 0; }
    int indexMismatches() const override { return m_mismatches; }

private:
    enum class LandResult { Landed, Overshot, Error };

    static int getBuffer2(AVCodecContext* ctx, AVFrame* frame, int flags)
    {
        auto* self = static_cast<FFmpegDecodeSession*>(ctx->opaque);
        return self->m_cfg.pool->getBuffer2(ctx, frame, flags);
    }

    void resetDecoder()
    {
        avcodec_flush_buffers(m_ctx.get());
        m_ctxClean = true;
        av_packet_unref(m_pkt.get());
        m_hasPending = false;
        m_inputEof = false;
        m_eof = false;
        m_lastPres = -1;
        m_nextDec = -1;
        m_skipped.clear();
    }

    bool rewindToStart()
    {
        // Reopening is the one way back to the very first packet that works for every demuxer.
        std::string err;
        auto demux = openDemuxer(m_path, m_opts, &err);
        if (!demux) {
            SCVP_ERROR("%s: reopen failed: %s", m_path.c_str(), err.c_str());
            return false;
        }
        m_demux = std::move(*demux);
        m_nextDec = 0;
        return true;
    }

    LandResult seekAndLand(const SeekPoint& sp, int attempt)
    {
        AVFormatContext* fmt = m_demux.fmt.get();
        const AVStream* st = m_demux.videoStream();
        int ret;
        if (m_byteSeek && sp.pos >= 0) {
            ret = av_seek_frame(fmt, -1, sp.pos, AVSEEK_FLAG_BYTE);
        } else {
            int64_t ts = sp.dts != AV_NOPTS_VALUE ? sp.dts : sp.pts;
            if (sp.pts != AV_NOPTS_VALUE && sp.dts != AV_NOPTS_VALUE)
                ts = std::min(sp.pts, sp.dts);
            if (ts == AV_NOPTS_VALUE)
                return LandResult::Error;
            // Later attempts aim further back in case the demuxer's index is coarse.
            ts -= av_rescale_q(attempt * 2, AVRational{1, 1}, st->time_base);
            ret = av_seek_frame(fmt, m_demux.stream, ts, AVSEEK_FLAG_BACKWARD);
        }
        if (ret < 0) {
            SCVP_DEBUG("av_seek_frame failed: %s", ff::errorString(ret).c_str());
            return LandResult::Error;
        }
        // Read forward until the exact key packet; anything before it is not needed.
        for (int reads = 0; reads < 100000; ++reads) {
            ret = av_read_frame(fmt, m_pkt.get());
            if (ret < 0)
                return LandResult::Overshot;
            if (m_pkt->stream_index != m_demux.stream) {
                av_packet_unref(m_pkt.get());
                continue;
            }
            if (matches(*m_pkt, sp)) {
                m_hasPending = true;
                m_pendingDec = sp.dec;
                m_nextDec = sp.dec + 1;
                return LandResult::Landed;
            }
            const bool passed = sp.pos >= 0 && m_pkt->pos > sp.pos;
            av_packet_unref(m_pkt.get());
            if (passed)
                return LandResult::Overshot;
        }
        return LandResult::Overshot;
    }

    static bool matches(const AVPacket& pkt, const SeekPoint& sp)
    {
        if (sp.pos >= 0 && pkt.pos >= 0)
            return pkt.pos == sp.pos && pkt.size == sp.size;
        return pkt.size == sp.size && pkt.pts == sp.pts && pkt.dts == sp.dts;
    }

    // Reads one video packet (or signals end of input) and sends it to the decoder.
    bool feedOne(const DecodeHints& hints)
    {
        if (m_inputEof)
            return false;  // flushed already; receive_frame must return EOF
        if (!m_hasPending) {
            for (;;) {
                const int ret = av_read_frame(m_demux.fmt.get(), m_pkt.get());
                if (ret < 0) {
                    avcodec_send_packet(m_ctx.get(), nullptr);  // drain
                    m_ctxClean = false;
                    m_inputEof = true;
                    return true;
                }
                if (m_pkt->stream_index == m_demux.stream)
                    break;
                av_packet_unref(m_pkt.get());
            }
            m_pendingDec = verifyDecodeIndex(*m_pkt, m_nextDec);
            m_nextDec = m_pendingDec >= 0 ? m_pendingDec + 1 : -1;
            m_hasPending = true;
        }
        m_pkt->opaque = reinterpret_cast<void*>(intptr_t(m_pendingDec) + 1);
        if (m_verifyPending && m_ctxClean && (m_pkt->flags & AV_PKT_FLAG_KEY)) {
            verifyHardware();
            if (m_hwFailed)
                return true;  // next() reopens in software and restarts from the last seek point
        }
        m_ctx->skip_frame = skipFor(m_pendingDec, hints);
        if (m_ctx->skip_frame != AVDISCARD_DEFAULT && m_pendingDec >= 0) {
            m_skipped.insert(m_pendingDec);
            // Only recent packets matter for "can continuing still produce frame X".
            while (!m_skipped.empty() && *m_skipped.begin() < m_pendingDec - 4096)
                m_skipped.erase(m_skipped.begin());
        }
        const int ret = avcodec_send_packet(m_ctx.get(), m_pkt.get());
        m_ctxClean = false;
        if (ret == AVERROR(EAGAIN))
            return true;  // keep the packet; output must be drained first
        av_packet_unref(m_pkt.get());
        m_hasPending = false;
        if (ret < 0 && ret != AVERROR_INVALIDDATA) {
            SCVP_WARN("%s: avcodec_send_packet: %s", m_path.c_str(), ff::errorString(ret).c_str());
            return false;
        }
        return true;
    }

    // The decode index the packet should have; corrected through the index when the stream
    // and the expectation disagree (packets lost by the demuxer, scan still behind, ...).
    int32_t verifyDecodeIndex(const AVPacket& pkt, int32_t expected)
    {
        FrameIndexPtr idx = m_source.index();
        if (!idx)
            return expected;
        if (expected >= 0 && expected < idx->packetCount()) {
            const IndexPacket& ip = idx->packet(expected);
            if ((ip.pos < 0 || pkt.pos < 0 || ip.pos == pkt.pos) && ip.size == pkt.size)
                return expected;
        } else if (expected >= idx->packetCount()) {
            return expected;  // beyond the scanned part; trust the running count
        }
        const int32_t byPos = idx->decodeIndexForPos(pkt.pos);
        if (byPos >= 0)
            return byPos;
        return expected >= 0 && expected >= idx->packetCount() ? expected : -1;
    }

    AVDiscard skipFor(int32_t dec, const DecodeHints& hints) const
    {
        if ((hints.skipNonRefBelow < 0 && !hints.frameNeeded) || dec < 0)
            return AVDISCARD_DEFAULT;
        FrameIndexPtr idx = m_source.index();
        if (!idx || dec >= idx->packetCount())
            return AVDISCARD_DEFAULT;
        const IndexPacket& ip = idx->packet(dec);
        if (ip.isKey())
            return AVDISCARD_DEFAULT;
        bool unneeded = !ip.shown();
        if (!unneeded && ip.pres >= 0)
            unneeded = hints.frameNeeded ? !(*hints.frameNeeded)(ip.pres) : ip.pres < hints.skipNonRefBelow;
        return unneeded ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    }

    // GPU-first promises the pixels software decoding gives. The codecs it uses guarantee that
    // for conforming decoders, but not every device conforms (VideoToolbox in GitHub's macOS VMs
    // gets 8-bit HEVC chroma wrong while luma and 10-bit are exact). So before the first hardware
    // frame of a (device, codec, pixel format) is shown, the key picture about to be sent is
    // decoded both ways and compared; the verdict holds for the rest of the process. Only a key
    // picture decodes on its own, and only a clean decoder (just opened or flushed) can be
    // drained for it and flushed again without losing its place in the stream.
    void verifyHardware()
    {
        const AVStream* st = m_demux.videoStream();
        const AVCodecID id = st->codecpar->codec_id;
        // An edit list's pre-roll key packet is flagged for discarding: decode its picture anyway.
        ff::PacketPtr key = ff::makePacket();
        if (!key || av_packet_ref(key.get(), m_pkt.get()) < 0)
            return;
        key->flags &= ~AV_PKT_FLAG_DISCARD;
        // The software decode (12 ms for a 4K HEVC picture) runs while the device decodes. The
        // future's destructor waits for it on every return path.
        auto software = std::async(std::launch::async, [this, &key] { return decodeInSoftware(key.get()); });
        ff::FramePtr hw = decodeAlone(m_ctx.get(), key.get());
        if (m_hwFailed || !hw || hw->format != m_hwPixFmt)
            return;  // hardware set-up failed (the usual fallback follows) or no picture: next key
        ff::FramePtr got = ff::makeFrame();
        if (av_hwframe_transfer_data(got.get(), hw.get(), 0) < 0 || av_frame_copy_props(got.get(), hw.get()) < 0)
            return;
        if (got->crop_top || got->crop_left)
            av_frame_apply_cropping(got.get(), AV_FRAME_CROP_UNALIGNED);
        ff::FramePtr want = software.get();
        if (!want)
            return;
        if (m_cfg.hw.simulateMismatch && av_frame_make_writable(got.get()) >= 0)
            got->data[0][0] ^= 1;
        const bool same = samePixelValues(got.get(), want.get());
        HwDevices::instance().setVerdict(m_hwType, id, st->codecpar->format, same ? HwVerdict::Exact : HwVerdict::Differs);
        m_verifyPending = false;
        const char* fmt = av_get_pix_fmt_name(static_cast<AVPixelFormat>(st->codecpar->format));
        if (same) {
            SCVP_INFO("%s: %s decodes %s %s exactly like software", m_path.c_str(), HwDevices::name(m_hwType),
                      avcodec_get_name(id), fmt ? fmt : "?");
        } else {
            SCVP_WARN("%s: %s decodes %s %s differently from software: using software for these streams",
                      m_path.c_str(), HwDevices::name(m_hwType), avcodec_get_name(id), fmt ? fmt : "?");
            m_hwRejected = true;
            m_hwFailed = true;
        }
    }

    // The first picture `ctx` outputs for `pkt` alone (sent, drained); leaves `ctx` flushed.
    static ff::FramePtr decodeAlone(AVCodecContext* ctx, const AVPacket* pkt)
    {
        ff::FramePtr out;
        ctx->skip_frame = AVDISCARD_DEFAULT;
        if (avcodec_send_packet(ctx, pkt) >= 0 && avcodec_send_packet(ctx, nullptr) >= 0) {
            for (ff::FramePtr f = ff::makeFrame(); avcodec_receive_frame(ctx, f.get()) >= 0; f = ff::makeFrame())
                if (!out)
                    out = std::move(f);
        }
        avcodec_flush_buffers(ctx);
        return out;
    }

    ff::FramePtr decodeInSoftware(const AVPacket* pkt) const
    {
        const AVStream* st = m_demux.videoStream();
        const AVCodec* codec = chooseDecoder(st->codecpar->codec_id, false);
        ff::CodecContextPtr ctx(codec ? avcodec_alloc_context3(codec) : nullptr);
        if (!ctx || avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0)
            return nullptr;
        ctx->pkt_timebase = st->time_base;
        ctx->thread_type = FF_THREAD_SLICE;  // one picture: frame threads cannot help
        ctx->thread_count = autoDecoderThreads(st->codecpar->width, st->codecpar->height);
        if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
            return nullptr;
        return decodeAlone(ctx.get(), pkt);
    }

    // Copies a hardware surface into a pool frame in system memory (NV12/P010/...), so the
    // surface goes straight back to the decoder's fixed-size pool.
    bool download()
    {
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(m_frame->hw_frames_ctx->data);
        AVFrame* sw = m_cfg.pool ? m_cfg.pool->allocFrame(frames->sw_format, m_frame->width, m_frame->height) : av_frame_alloc();
        if (!sw)
            return false;
        if (!m_cfg.pool)
            sw->format = frames->sw_format;
        int err = av_hwframe_transfer_data(sw, m_frame.get(), 0);
        if (err >= 0)
            err = av_frame_copy_props(sw, m_frame.get());
        if (err < 0) {
            SCVP_WARN("%s: hardware frame download failed: %s", m_path.c_str(), ff::errorString(err).c_str());
            av_frame_free(&sw);
            return false;
        }
        if (sw->crop_top || sw->crop_left)
            av_frame_apply_cropping(sw, AV_FRAME_CROP_UNALIGNED);
        av_frame_unref(m_frame.get());
        av_frame_move_ref(m_frame.get(), sw);
        av_frame_free(&sw);
        m_downloaded = true;
        return true;
    }

    struct Label {
        int32_t dec = -1;
        int32_t pres = -1;
        bool provisional = false;
    };

    // The frame number of m_frame, from its packet tag (cross-checked with pts) and the index.
    Label label()
    {
        Label lb;
        const intptr_t tag = reinterpret_cast<intptr_t>(m_frame->opaque);
        int32_t dec = tag > 0 ? int32_t(tag - 1) : -1;
        FrameIndexPtr idx = m_source.index();
        if (idx && dec >= 0 && dec < idx->packetCount() && m_frame->pts != AV_NOPTS_VALUE && m_ptsReliable &&
            idx->ordering() == FrameIndex::Ordering::Pts) {
            // Some decoders re-output an earlier picture with that picture's packet tag (AV1
            // show_existing_frame through hwaccel) while the pts is right: trust the pts then.
            const IndexPacket& ip = idx->packet(dec);
            if (ip.pts != m_frame->pts) {
                const int32_t byPts = idx->presForPts(m_frame->pts, ip.pres);
                if (byPts >= 0)
                    dec = idx->decodeFromPres(byPts);
            }
        }
        int32_t pres = -1;
        bool provisional = false;
        if (idx && dec >= 0 && dec < idx->packetCount()) {
            const IndexPacket& ip = idx->packet(dec);
            pres = ip.pres;
            if (pres < 0 && ip.shown()) {
                provisional = true;  // not yet final in a partial index
                pres = idx->estimatePres(ip.pts);
            } else if (pres < 0) {
                ++m_mismatches;  // the index says this packet is never shown
                SCVP_DEBUG("%s: decoder output a frame for hidden packet %d", m_path.c_str(), dec);
                return lb;
            }
        } else {
            provisional = true;
            const int64_t pts = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : m_frame->best_effort_timestamp;
            pres = idx ? idx->estimatePres(pts) : -1;
        }
        if (pres < 0)
            return lb;
        if (!provisional && m_lastPres >= 0 && pres != m_lastPres + 1) {
            ++m_mismatches;
            SCVP_DEBUG("%s: frame %d followed %d", m_path.c_str(), pres, m_lastPres);
        }
        m_lastPres = pres;
        lb.dec = dec;
        lb.pres = pres;
        lb.provisional = provisional;
        return lb;
    }

    void stamp(Frame& f, const Label& lb, const AVFrame* src) const
    {
        f.pres = lb.pres;
        f.dec = lb.dec;
        f.pts = src->pts;
        f.key = ff::frameIsKey(src);
        f.pictType = av_get_picture_type_char(src->pict_type);
        f.provisional = lb.provisional;
    }

    // System-memory frame from m_frame (software output or a downloaded surface).
    FramePtr finish(const Label& lb)
    {
        auto f = std::make_shared<Frame>();
        f->av = av_frame_alloc();
        av_frame_move_ref(f->av, m_frame.get());
        stamp(*f, lb, f->av);
        f->hardware = m_downloaded;
        m_downloaded = false;
        f->bytes = ff::frameBufferBytes(f->av);
        return f;
    }

    // Device-memory frame: the decoder surface copied into a VRAM block (D2D), then released.
    FramePtr keepOnDevice(const Label& lb)
    {
        if (!m_gpuPool) {
            AVBufferRef* dev = m_ctx->hw_device_ctx;
            m_gpuPool = GpuFramePool::forDevice(dev);
            if (!m_gpuPool)
                return nullptr;
        }
        auto block = m_gpuPool->copyFrom(m_frame.get());
        if (!block)
            return nullptr;
        auto f = makeGpuFrame(std::move(block), m_frame.get());
        stamp(*f, lb, m_frame.get());
        f->hardware = true;
        av_frame_unref(m_frame.get());
        return f;
    }

    const FFmpegSource& m_source;
    std::string m_path;
    InputOptions m_opts;
    SessionConfig m_cfg;
    Demuxer m_demux;
    ff::CodecContextPtr m_ctx;
    ff::PacketPtr m_pkt;
    ff::FramePtr m_frame;
    bool m_byteSeek = false;
    bool m_ptsReliable = true;
    bool m_hasPending = false;
    bool m_inputEof = false;
    bool m_eof = false;
    int32_t m_pendingDec = -1;
    int32_t m_nextDec = 0;
    int32_t m_lastPres = -1;
    int m_mismatches = 0;
    std::set<int32_t> m_skipped;  // decode indices fed with NONREF since the last seek
    SeekPoint m_lastSeek;
    AVHWDeviceType m_hwType = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    std::atomic<bool> m_hwFailed{false};
    bool m_hwRejected = false;  // left on purpose (output differs or is unchecked), already logged
    int m_hwErrors = 0;
    bool m_verifyPending = false;  // GPU-first hardware whose verdict is still unknown
    bool m_ctxClean = true;        // nothing sent to m_ctx since it was opened or flushed
    bool m_downloaded = false;
    std::shared_ptr<GpuFramePool> m_gpuPool;
    int m_gpuBackoff = 0;
};

VideoInfo describe(const std::string& path, const Demuxer& d)
{
    VideoInfo info;
    const AVFormatContext* fmt = d.fmt.get();
    const AVStream* st = d.videoStream();
    const AVCodecParameters* par = st->codecpar;
    info.path = path;
    info.formatName = fmt->iformat ? fmt->iformat->name : "";
    info.codecName = avcodec_get_name(par->codec_id);
    info.width = par->width;
    info.height = par->height;
    info.pixelFormat = static_cast<AVPixelFormat>(par->format);
    info.timeBase = st->time_base;
    info.sampleAspect = av_guess_sample_aspect_ratio(const_cast<AVFormatContext*>(fmt), const_cast<AVStream*>(st), nullptr);
    if (info.sampleAspect.num <= 0 || info.sampleAspect.den <= 0)
        info.sampleAspect = AVRational{1, 1};
    const AVRational fr = av_guess_frame_rate(const_cast<AVFormatContext*>(fmt), const_cast<AVStream*>(st), nullptr);
    info.nominalFrameRate = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        info.durationSeconds = double(st->duration) * av_q2d(st->time_base);
    else if (fmt->duration > 0)
        info.durationSeconds = double(fmt->duration) / AV_TIME_BASE;
    info.rotation = ff::streamRotation(st);
    info.bitRate = par->bit_rate > 0 ? par->bit_rate : fmt->bit_rate;
    info.colorRange = par->color_range;
    info.colorSpace = par->color_space;
    info.colorPrimaries = par->color_primaries;
    info.colorTransfer = par->color_trc;
    info.chromaLocation = par->chroma_location;
    return info;
}

} // namespace

std::unique_ptr<FFmpegSource> FFmpegSource::open(const std::string& path, const InputOptions& opts, std::string* error)
{
    auto demux = openDemuxer(path, opts, error);
    if (!demux)
        return nullptr;
    std::unique_ptr<FFmpegSource> src(new FFmpegSource);
    src->m_path = path;
    src->m_opts = opts;
    src->m_info = describe(path, *demux);
    demux.reset();
    src->m_scanner = std::make_unique<IndexScanner>(path, opts, [s = src.get()](FrameIndexPtr idx) { s->onSnapshot(std::move(idx)); });
    src->m_scanner->start();
    return src;
}

FFmpegSource::~FFmpegSource()
{
    if (m_scanner)
        m_scanner->stop();
}

void FFmpegSource::onSnapshot(FrameIndexPtr idx)
{
    std::function<void()> listener;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_index = std::move(idx);
        listener = m_listener;
    }
    m_indexChanged.notify_all();
    if (listener)
        listener();
}

void FFmpegSource::setIndexListener(std::function<void()> listener)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = std::move(listener);
}

FrameIndexPtr FFmpegSource::index() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index;
}

bool FFmpegSource::indexComplete() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index && m_index->complete();
}

double FFmpegSource::indexProgress() const { return m_scanner ? m_scanner->progress() : 1.0; }

bool FFmpegSource::waitForIndex(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_indexChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return (m_index && m_index->complete()) || (m_scanner && m_scanner->failed());
    }) && m_index && m_index->complete();
}

std::unique_ptr<IDecodeSession> FFmpegSource::openSession(const SessionConfig& cfg, std::string* error)
{
    auto s = std::make_unique<FFmpegDecodeSession>(*this, m_path, m_opts, cfg);
    if (!s->open(error))
        return nullptr;
    return s;
}

std::unique_ptr<IVideoSource> openVideoSource(const std::string& path, const InputOptions& opts, std::string* error)
{
    if (opts.raw)
        return RawVideoSource::open(path, *opts.raw, error);
    if (isDirectory(path) || isImageExtension(path))
        return ImageSequenceSource::open(path, opts.frameRate, error);
    if (isRawVideoExtension(path)) {
        const RawVideoParams p = parseRawVideoName(path);
        if (p.valid())
            return RawVideoSource::open(path, p, error);
        if (error)
            *error = "raw video: give the frame size and pixel format (e.g. name it clip_1920x1080_nv12.yuv)";
        return nullptr;
    }
    return FFmpegSource::open(path, opts, error);
}

} // namespace scvp
