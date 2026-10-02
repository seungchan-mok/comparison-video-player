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
    int m_fed = 0;
    bool m_done = false;
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
    const int64_t fileSize = fmt->pb ? avio_size(fmt->pb) : -1;
    auto pkt = ff::makePacket();
    auto lastPublish = Clock::now();
    size_t publishedAt = 0;
    bool earlyPublished = false;

    auto publish = [&](bool complete) {
        builder.setOrdering(builder.missingPts() ? FrameIndex::Ordering::Decode : FrameIndex::Ordering::Pts);
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
            builder.add(*pkt);
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
    if (probe.active()) {
        probe.drain();
        builder.setFirstOutputPts(probe.firstPts());
    }
    if (progress)
        progress->store(1.0);
    return publish(true);
}

} // namespace scvp
