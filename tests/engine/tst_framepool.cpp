#include "engine/FrameAllocator.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <cstring>

using namespace scvp;

namespace {

bool aligned(const void* p) { return reinterpret_cast<uintptr_t>(p) % FramePool::kAlign == 0; }

// Writes every byte the planes are supposed to cover; ASan builds catch any undersized plane.
void touchAllPlanes(AVFrame* f)
{
    size_t sizes[4] = {};
    ptrdiff_t ls[4] = {f->linesize[0], f->linesize[1], f->linesize[2], f->linesize[3]};
    QVERIFY(av_image_fill_plane_sizes(sizes, AVPixelFormat(f->format), f->height, ls) >= 0);
    for (int i = 0; i < 4; ++i)
        if (sizes[i])
            std::memset(f->data[i], 0x5a, sizes[i]);
}

struct DecodeResult {
    int frames = 0;
    int mismatches = 0;
    QString firstError;
};

// Plain FFmpeg decode loop with the pool installed as get_buffer2 and frame threading on.
DecodeResult decodeAll(const test::Fixture& fx, FramePool& pool, const std::vector<std::string>& ref,
                       size_t* liveDuringDecode)
{
    DecodeResult r;
    AVFormatContext* rawFmt = nullptr;
    if (avformat_open_input(&rawFmt, fx.path.toUtf8().constData(), nullptr, nullptr) < 0) {
        r.firstError = QStringLiteral("open failed");
        return r;
    }
    ff::FormatContextPtr fmt(rawFmt);
    avformat_find_stream_info(fmt.get(), nullptr);
    const int si = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) {
        r.firstError = QStringLiteral("no video stream");
        return r;
    }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id);
    ff::CodecContextPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), fmt->streams[si]->codecpar);
    ctx->pkt_timebase = fmt->streams[si]->time_base;
    ctx->thread_count = 0;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    pool.attachTo(ctx.get());
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        r.firstError = QStringLiteral("codec open failed");
        return r;
    }

    auto pkt = ff::makePacket();
    auto frame = ff::makeFrame();
    std::vector<ff::FramePtr> held;  // keep a few frames alive to exercise accounting
    auto drain = [&]() {
        while (avcodec_receive_frame(ctx.get(), frame.get()) == 0) {
            const std::string h = test::frameMd5(frame.get());
            if (r.frames >= int(ref.size()) || h != ref[size_t(r.frames)]) {
                if (!r.mismatches)
                    r.firstError = QStringLiteral("frame %1 hash mismatch").arg(r.frames);
                ++r.mismatches;
            }
            ++r.frames;
            if (held.size() < 8) {
                held.emplace_back(av_frame_clone(frame.get()));
                *liveDuringDecode = std::max(*liveDuringDecode, pool.liveBytes());
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == si) {
            if (avcodec_send_packet(ctx.get(), pkt.get()) == AVERROR(EAGAIN)) {
                drain();
                avcodec_send_packet(ctx.get(), pkt.get());
            }
            drain();
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(ctx.get(), nullptr);
    drain();
    return r;
}

} // namespace

class TstFramePool : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void allocLayout_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        const struct {
            AVPixelFormat f;
            const char* n;
        } fmts[] = {{AV_PIX_FMT_YUV420P, "yuv420p"},     {AV_PIX_FMT_NV12, "nv12"},
                    {AV_PIX_FMT_P010LE, "p010"},         {AV_PIX_FMT_YUV422P10LE, "yuv422p10"},
                    {AV_PIX_FMT_YUV444P16LE, "yuv444p16"}, {AV_PIX_FMT_RGB24, "rgb24"},
                    {AV_PIX_FMT_RGBA, "rgba"},           {AV_PIX_FMT_GRAY8, "gray"},
                    {AV_PIX_FMT_UYVY422, "uyvy"},        {AV_PIX_FMT_GBRP12LE, "gbrp12"}};
        const int sizes[][2] = {{1, 1}, {3, 3}, {321, 241}, {1920, 1080}, {3840, 2160}};
        for (const auto& f : fmts)
            for (const auto& s : sizes)
                QTest::newRow(qPrintable(QStringLiteral("%1 %2x%3").arg(f.n).arg(s[0]).arg(s[1])))
                    << int(f.f) << s[0] << s[1];
    }

    void allocLayout()
    {
        QFETCH(int, format);
        QFETCH(int, width);
        QFETCH(int, height);
        FramePool pool;
        AVFrame* f = pool.allocFrame(AVPixelFormat(format), width, height);
        QVERIFY(f);
        QCOMPARE(f->width, width);
        QCOMPARE(f->height, height);
        int minLines[4] = {};
        QVERIFY(av_image_fill_linesizes(minLines, AVPixelFormat(format), width) >= 0);
        for (int i = 0; i < 4; ++i) {
            if (!minLines[i]) {
                QVERIFY(!f->data[i]);
                continue;
            }
            QVERIFY(aligned(f->data[i]));
            QCOMPARE(f->linesize[i] % int(FramePool::kAlign), 0);
            QVERIFY(f->linesize[i] >= minLines[i]);
        }
        touchAllPlanes(f);
        QVERIFY(pool.liveBytes() >= size_t(av_image_get_buffer_size(AVPixelFormat(format), width, height, 1)));
        av_frame_free(&f);
        QCOMPARE(pool.liveBytes(), size_t(0));
    }

    void recyclesBlocks()
    {
        FramePool pool;
        AVFrame* a = pool.allocFrame(AV_PIX_FMT_YUV420P, 1920, 1080);
        QVERIFY(a);
        uint8_t* first = a->data[0];
        av_frame_free(&a);
        QVERIFY(pool.cachedBytes() > 0);
        const size_t allocs = pool.allocationCount();
        AVFrame* b = pool.allocFrame(AV_PIX_FMT_YUV420P, 1920, 1080);
        QCOMPARE(b->data[0], first);  // same block back from the free list
        QCOMPARE(pool.allocationCount(), allocs);
        QCOMPARE(pool.cachedBytes(), size_t(0));
        av_frame_free(&b);
        pool.trim();
        QCOMPARE(pool.cachedBytes(), size_t(0));
        QCOMPARE(pool.liveBytes(), size_t(0));
    }

    void cacheLimitBoundsFreeList()
    {
        FramePool pool;
        const int frameBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 1920, 1080, 1);
        pool.setCacheLimit(size_t(frameBytes) * 3);
        std::vector<AVFrame*> frames;
        for (int i = 0; i < 10; ++i)
            frames.push_back(pool.allocFrame(AV_PIX_FMT_YUV420P, 1920, 1080));
        const size_t peak = pool.liveBytes();
        QVERIFY(peak >= size_t(frameBytes) * 10);
        for (AVFrame*& f : frames)
            av_frame_free(&f);
        QCOMPARE(pool.liveBytes(), size_t(0));
        QVERIFY(pool.cachedBytes() <= size_t(frameBytes) * 3 + 3 * 8192);
        QVERIFY(pool.peakLiveBytes() >= peak);
    }

    void frameOutlivesPool()
    {
        AVFrame* f = nullptr;
        {
            FramePool pool;
            f = pool.allocFrame(AV_PIX_FMT_NV12, 640, 480);
            QVERIFY(f);
        }
        touchAllPlanes(f);  // memory still valid after the pool object is gone
        AVFrame* clone = av_frame_clone(f);
        av_frame_free(&f);
        av_frame_free(&clone);  // last reference releases the arena (ASan/LSan verify)
    }

    void decodeThroughPool_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& fx : test::fixtures({QStringLiteral("container"), QStringLiteral("rawes")}))
            QTest::newRow(qPrintable(fx.name)) << fx.name;
    }

    void decodeThroughPool()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        const auto fx = test::fixture(name);
        QVERIFY(fx);
        const auto ref = test::referenceHashes(*fx);
        QVERIFY(!ref.empty());
        FramePool pool(name.toStdString());
        size_t live = 0;
        const DecodeResult r = decodeAll(*fx, pool, ref, &live);
        QVERIFY2(r.mismatches == 0, qPrintable(r.firstError));
        QCOMPARE(r.frames, int(ref.size()));
        QCOMPARE(pool.liveBytes(), size_t(0));  // nothing leaked once decoder and frames are gone
    }
};

QTEST_APPLESS_MAIN(TstFramePool)
#include "tst_framepool.moc"
