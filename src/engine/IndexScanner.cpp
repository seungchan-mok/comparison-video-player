#include "engine/IndexScanner.h"

#include "engine/Log.h"
#include "engine/Platform.h"

#include <chrono>

namespace scvp {
namespace {

// Decodes from the start of the stream until the first frame appears, to learn which leading
// frames the decoder drops (open-GOP B-frames referencing a GOP that is not in the file).
class FirstOutputProbe {
public:
    explicit FirstOutputProbe(const AVStream* st)
    {
        const AVCodec* codec = chooseDecoder(st->codecpar->codec_id, false);
        if (!codec)
            return;
        m_ctx.reset(avcodec_alloc_context3(codec));
        if (!m_ctx || avcodec_parameters_to_context(m_ctx.get(), st->codecpar) < 0) {
            m_ctx.reset();
            return;
        }
        m_ctx->pkt_timebase = st->time_base;
        m_ctx->thread_count = 1;
        if (avcodec_open2(m_ctx.get(), codec, nullptr) < 0)
            m_ctx.reset();
        m_frame = ff::makeFrame();
    }

    bool active() const { return m_ctx && !m_done; }
    bool done() const { return m_done; }
    int64_t firstPts() const { return m_firstPts; }
    AVRational frameRate() const { return m_rate; }  // from the bitstream (VUI), 0/1 if none

    void feed(const AVPacket* pkt)
    {
        if (!active())
            return;
        int ret = avcodec_send_packet(m_ctx.get(), pkt);
        if (ret == AVERROR(EAGAIN)) {
            receive();
            if (m_done)
                return;
            ret = avcodec_send_packet(m_ctx.get(), pkt);
        }
        receive();
        if (++m_fed > 600 && !m_done)  // no output after this many packets: give up
            finish(AV_NOPTS_VALUE);
    }

    void drain()
    {
        if (!active())
            return;
        avcodec_send_packet(m_ctx.get(), nullptr);
        receive();
        if (!m_done)
            finish(AV_NOPTS_VALUE);
    }

private:
    void receive()
    {
        while (!m_done && avcodec_receive_frame(m_ctx.get(), m_frame.get()) == 0) {
            const int64_t pts = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : m_frame->best_effort_timestamp;
            m_rate = m_ctx->framerate;
            finish(pts);
        }
    }

    void finish(int64_t pts)
    {
        m_firstPts = pts;
        m_done = true;
        m_frame.reset();
        m_ctx.reset();
    }

    ff::CodecContextPtr m_ctx;
    ff::FramePtr m_frame;
    int64_t m_firstPts = AV_NOPTS_VALUE;
    AVRational m_rate{0, 1};
    int m_fed = 0;
    bool m_done = false;
};

// Raw H.264 / HEVC elementary streams have no timestamps: frame order comes from the picture
// order count, which FFmpeg's parser computes per access unit. POC restarts at IDRs (a key whose
// POC is not above everything before it); those epochs are chained into one increasing count
// that stands in for pts, so the ordinary pts machinery (sorting, open-GOP leading pictures,
// the first-output probe) applies. Container dts (AVI) are kept for seeking.
class PocTimestamps {
public:
    PocTimestamps(const AVStream* st)
    {
        const AVCodecID id = st->codecpar->codec_id;
        if (id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_HEVC)
            return;
        m_parser.reset(av_parser_init(id));
        m_ctx.reset(avcodec_alloc_context3(nullptr));
        if (!m_parser || !m_ctx || avcodec_parameters_to_context(m_ctx.get(), st->codecpar) < 0) {
            m_parser.reset();
            return;
        }
        m_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    }

    // Decided on the first packet: only streams without any timestamps.
    bool decide(const AVPacket& pkt)
    {
        if (!m_decided) {
            m_decided = true;
            m_active = m_parser && pkt.pts == AV_NOPTS_VALUE;  // raw ES; AVI has dts only
            if (!m_active)
                m_parser.reset();
        }
        return m_active;
    }
    bool active() const { return m_active; }

    void assign(AVPacket& pkt)
    {
        uint8_t* out = nullptr;
        int outSize = 0;
        av_parser_parse2(m_parser.get(), m_ctx.get(), &out, &outSize, pkt.data, pkt.size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, pkt.pos);
        const int64_t poc = m_parser->output_picture_number;
        const bool key = (pkt.flags & AV_PKT_FLAG_KEY) || m_parser->key_frame == 1;
        if (m_any && key && poc <= m_epochMax) {
            // The new epoch starts after the old one, with room for leading pictures that follow
            // the key in decode order but precede it (lower POC). Gaps do not matter: Poc
            // ordering times frames arithmetically.
            m_base = m_maxPts + 64 * m_step - poc;
            m_epochMax = INT64_MIN;
        }
        // Smallest POC step seen (2 for frame-coded H.264, 1 for HEVC): the gap between epochs.
        if (m_any && poc != m_lastPoc)
            m_step = std::min<int64_t>(m_step, std::llabs(poc - m_lastPoc));
        m_lastPoc = poc;
        m_any = true;
        m_epochMax = std::max(m_epochMax, poc);
        pkt.pts = m_base + poc;
        m_maxPts = std::max(m_maxPts, pkt.pts);
    }
    // A frame whose count is this far below the largest seen cannot be preceded by a later
    // packet any more: H.264/HEVC reorder at most 16 frames.
    int64_t reorderMargin() const { return 32 * std::min<int64_t>(m_step, 2); }

private:
    struct ParserDel {
        void operator()(AVCodecParserContext* p) const { av_parser_close(p); }
    };
    std::unique_ptr<AVCodecParserContext, ParserDel> m_parser;
    ff::CodecContextPtr m_ctx;
    bool m_decided = false;
    bool m_active = false;
    bool m_any = false;
    int64_t m_base = 0;
    int64_t m_maxPts = INT64_MIN;
    int64_t m_epochMax = INT64_MIN;
    int64_t m_lastPoc = 0;
    int64_t m_step = 1 << 20;
};

// MPEG-1/2/4 Part 2 in AVI or raw: demuxers give B-frames a pts but anchors (I/P) none. With
// these codecs' one-frame reordering an anchor is shown right before the next anchor, so its pts
// is the next anchor's dts (the last one: one frame after the last dts). Until that next anchor
// arrives the anchor sorts last and is not final.
class AnchorTimestamps {
public:
    static constexpr int64_t kPending = INT64_MAX / 4;

    explicit AnchorTimestamps(const AVStream* st)
    {
        const AVCodecID id = st->codecpar->codec_id;
        if (id != AV_CODEC_ID_MPEG1VIDEO && id != AV_CODEC_ID_MPEG2VIDEO && id != AV_CODEC_ID_MPEG4)
            return;
        m_parser.reset(av_parser_init(id));
        m_ctx.reset(avcodec_alloc_context3(nullptr));
        if (!m_parser || !m_ctx || avcodec_parameters_to_context(m_ctx.get(), st->codecpar) < 0)
            m_parser.reset();
        else
            m_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    }

    bool decide(const AVPacket& pkt)
    {
        if (!m_decided) {
            m_decided = true;
            m_active = m_parser && pkt.pts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE;
            if (!m_active)
                m_parser.reset();
        }
        return m_active;
    }
    bool active() const { return m_active; }

    // Before builder.add(): B-frames get pts = dts, anchors the pending marker. Resolves the
    // previous anchor in the builder.
    void assign(AVPacket& pkt, int32_t dec, FrameIndexBuilder& builder)
    {
        uint8_t* out = nullptr;
        int outSize = 0;
        av_parser_parse2(m_parser.get(), m_ctx.get(), &out, &outSize, pkt.data, pkt.size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, pkt.pos);
        if (m_lastDts != AV_NOPTS_VALUE && pkt.dts > m_lastDts)
            m_frameDuration = pkt.dts - m_lastDts;
        m_lastDts = pkt.dts;
        if (m_parser->pict_type == AV_PICTURE_TYPE_B) {
            if (pkt.pts == AV_NOPTS_VALUE)
                pkt.pts = pkt.dts;
            return;
        }
        if (m_pendingDec >= 0)
            builder.setPts(m_pendingDec, pkt.dts);
        m_pendingDec = dec;
        pkt.pts = kPending;
    }

    void finish(FrameIndexBuilder& builder)
    {
        if (m_pendingDec >= 0)
            builder.setPts(m_pendingDec, m_lastDts + m_frameDuration);
        m_pendingDec = -1;
    }

private:
    struct ParserDel {
        void operator()(AVCodecParserContext* p) const { av_parser_close(p); }
    };
    std::unique_ptr<AVCodecParserContext, ParserDel> m_parser;
    ff::CodecContextPtr m_ctx;
    bool m_decided = false;
    bool m_active = false;
    int32_t m_pendingDec = -1;
    int64_t m_lastDts = AV_NOPTS_VALUE;
    int64_t m_frameDuration = 1;
};

} // namespace

IndexScanner::IndexScanner(std::string path, InputOptions opts, SnapshotFn onSnapshot)
    : m_path(std::move(path))
    , m_opts(std::move(opts))
    , m_onSnapshot(std::move(onSnapshot))
{
}

IndexScanner::~IndexScanner() { stop(); }

void IndexScanner::start()
{
    if (m_thread.joinable())
        return;
    m_thread = std::thread([this] { run(); });
}

void IndexScanner::stop()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

std::string IndexScanner::error() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
}

void IndexScanner::run()
{
    platform::setCurrentThreadName("scvp-index");
    platform::lowerCurrentThreadPriority();
    std::string err;
    FrameIndexPtr idx = scan(m_path, m_opts, &err, m_onSnapshot, &m_cancel, &m_progress);
    if (!idx) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = err;
        m_failed = true;
        return;
    }
    m_finished = idx->complete();
}

FrameIndexPtr IndexScanner::scan(const std::string& path, const InputOptions& opts, std::string* error,
                                 const SnapshotFn& onSnapshot, const std::atomic<bool>* cancel,
                                 std::atomic<double>* progress)
{
    using Clock = std::chrono::steady_clock;
    auto demux = openDemuxer(path, opts, error);
    if (!demux)
        return nullptr;
    AVFormatContext* fmt = demux->fmt.get();
    AVStream* st = demux->videoStream();
    const AVRational guessed = av_guess_frame_rate(fmt, st, nullptr);
    FrameIndexBuilder builder(st->time_base, guessed.num > 0 && guessed.den > 0 ? av_q2d(guessed) : 0.0);

    FirstOutputProbe probe(st);
    PocTimestamps poc(st);
    AnchorTimestamps anchors(st);
    const int64_t fileSize = fmt->pb ? avio_size(fmt->pb) : -1;
    auto pkt = ff::makePacket();
    auto lastPublish = Clock::now();
    size_t publishedAt = 0;
    bool earlyPublished = false;

    auto publish = [&](bool complete) {
        if (poc.active()) {
            AVRational rate = probe.frameRate();
            if (opts.frameRate > 0)
                rate = av_d2q(opts.frameRate, 1001000);
            else if (rate.num <= 0 || rate.den <= 0)
                rate = AVRational{30, 1};  // no timing in the bitstream
            builder.setPocTiming(rate, poc.reorderMargin());
        } else {
            const bool ptsOrder = anchors.active() || !builder.missingPts();
            builder.setOrdering(ptsOrder ? FrameIndex::Ordering::Pts : FrameIndex::Ordering::Decode);
        }
        auto snapshot = builder.build(complete);
        publishedAt = builder.size();
        lastPublish = Clock::now();
        if (onSnapshot)
            onSnapshot(snapshot);
        return snapshot;
    };

    for (;;) {
        if (cancel && cancel->load())
            return nullptr;
        const int ret = av_read_frame(fmt, pkt.get());
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                SCVP_WARN("index scan of %s stopped early: %s", path.c_str(), ff::errorString(ret).c_str());
            break;
        }
        if (pkt->stream_index == demux->stream) {
            if (poc.decide(*pkt))
                poc.assign(*pkt);
            else if (anchors.decide(*pkt))
                anchors.assign(*pkt, int32_t(builder.size()), builder);
            builder.add(*pkt);
            if (probe.active() && anchors.active())
                probe.drain();  // pending anchor pts are markers, not times: no open-GOP probe
            if (probe.active()) {
                probe.feed(pkt.get());
                if (probe.done())
                    builder.setFirstOutputPts(probe.firstPts());
            }
        }
        av_packet_unref(pkt.get());

        if (progress && fileSize > 0 && fmt->pb)
            progress->store(std::min(1.0, double(avio_tell(fmt->pb)) / double(fileSize)));

        // Early snapshot as soon as frame numbering at the start is known, then at most four
        // per second (and only when the packet count grew noticeably).
        const bool probeKnown = !probe.active();
        if (!earlyPublished && probeKnown && builder.size() >= 32) {
            publish(false);
            earlyPublished = true;
        } else if (earlyPublished && Clock::now() - lastPublish > std::chrono::milliseconds(250) &&
                   builder.size() > publishedAt + publishedAt / 8 + 16) {
            publish(false);
        }
    }
    anchors.finish(builder);
    if (probe.active()) {
        probe.drain();
        builder.setFirstOutputPts(probe.firstPts());
    }
    if (progress)
        progress->store(1.0);
    return publish(true);
}

} // namespace scvp
