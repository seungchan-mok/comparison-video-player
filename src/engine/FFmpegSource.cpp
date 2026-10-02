#include "engine/FFmpegSource.h"

#include "engine/Log.h"

#include <algorithm>
#include <chrono>
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
        const AVStream* st = m_demux.videoStream();
        const AVCodec* codec = chooseDecoder(st->codecpar->codec_id, false);
        if (!codec) {
            if (error)
                *error = std::string("no decoder for ") + avcodec_get_name(st->codecpar->codec_id);
            return false;
        }
        m_ctx.reset(avcodec_alloc_context3(codec));
        if (!m_ctx || avcodec_parameters_to_context(m_ctx.get(), st->codecpar) < 0) {
            if (error)
                *error = "cannot configure decoder";
            return false;
        }
        m_ctx->pkt_timebase = st->time_base;
        m_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        m_ctx->thread_count = m_cfg.threads > 0 ? m_cfg.threads : autoDecoderThreads(st->codecpar->width, st->codecpar->height);
        m_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        m_ctx->opaque = this;
        if (m_cfg.pool)
            m_ctx->get_buffer2 = &FFmpegDecodeSession::getBuffer2;
        const int err = avcodec_open2(m_ctx.get(), codec, nullptr);
        if (err < 0) {
            if (error)
                *error = "avcodec_open2: " + ff::errorString(err);
            return false;
        }
        m_pkt = ff::makePacket();
        m_frame = ff::makeFrame();
        return true;
    }

    DecodeStatus seekTo(const SeekPoint& sp) override
    {
        resetDecoder();
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
            int ret = avcodec_receive_frame(m_ctx.get(), m_frame.get());
            if (ret == 0) {
                FramePtr f = labelFrame();
                if (!f)
                    continue;  // a frame that is never shown
                out = std::move(f);
                return DecodeStatus::Ok;
            }
            if (ret == AVERROR_EOF) {
                m_eof = true;
                return DecodeStatus::Eof;
            }
            if (ret != AVERROR(EAGAIN)) {
                SCVP_WARN("%s: decode error %s", m_path.c_str(), ff::errorString(ret).c_str());
                return DecodeStatus::Error;
            }
            if (!feedOne(hints))
                return DecodeStatus::Error;
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
        m_ctx->skip_frame = skipFor(m_pendingDec, hints);
        if (m_ctx->skip_frame != AVDISCARD_DEFAULT && m_pendingDec >= 0) {
            m_skipped.insert(m_pendingDec);
            // Only recent packets matter for "can continuing still produce frame X".
            while (!m_skipped.empty() && *m_skipped.begin() < m_pendingDec - 4096)
                m_skipped.erase(m_skipped.begin());
        }
        const int ret = avcodec_send_packet(m_ctx.get(), m_pkt.get());
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

    FramePtr labelFrame()
    {
        const intptr_t tag = reinterpret_cast<intptr_t>(m_frame->opaque);
        const int32_t dec = tag > 0 ? int32_t(tag - 1) : -1;
        FrameIndexPtr idx = m_source.index();
        int32_t pres = -1;
        bool provisional = false;
        if (idx && dec >= 0 && dec < idx->packetCount()) {
            const IndexPacket& ip = idx->packet(dec);
            pres = ip.pres;
            if (pres < 0 && ip.shown()) {
                // Not yet final in a partial index.
                provisional = true;
                pres = idx->estimatePres(ip.pts);
            } else if (pres < 0) {
                ++m_mismatches;  // the index says this packet is never shown
                SCVP_DEBUG("%s: decoder output a frame for hidden packet %d", m_path.c_str(), dec);
                av_frame_unref(m_frame.get());
                return nullptr;
            }
        } else {
            provisional = true;
            const int64_t pts = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : m_frame->best_effort_timestamp;
            pres = idx ? idx->estimatePres(pts) : -1;
        }
        if (pres < 0) {
            av_frame_unref(m_frame.get());
            return nullptr;
        }
        if (!provisional && m_lastPres >= 0 && pres != m_lastPres + 1) {
            ++m_mismatches;
            SCVP_DEBUG("%s: frame %d followed %d", m_path.c_str(), pres, m_lastPres);
        }
        m_lastPres = pres;

        auto f = std::make_shared<Frame>();
        f->av = av_frame_alloc();
        av_frame_move_ref(f->av, m_frame.get());
        f->pres = pres;
        f->dec = dec;
        f->pts = f->av->pts;
        f->key = ff::frameIsKey(f->av);
        f->pictType = av_get_picture_type_char(f->av->pict_type);
        f->provisional = provisional;
        f->bytes = ff::frameBufferBytes(f->av);
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
    bool m_hasPending = false;
    bool m_inputEof = false;
    bool m_eof = false;
    int32_t m_pendingDec = -1;
    int32_t m_nextDec = 0;
    int32_t m_lastPres = -1;
    int m_mismatches = 0;
    std::set<int32_t> m_skipped;  // decode indices fed with NONREF since the last seek
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
    return FFmpegSource::open(path, opts, error);
}

} // namespace scvp
