#include "engine/FFmpegSource.h"
#include "engine/GpuFrame.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <random>

using namespace scvp;

namespace {

AVFrame* randomCpuFrame(FramePool& pool, AVPixelFormat fmt, int w, int h, uint32_t seed)
{
    AVFrame* f = pool.allocFrame(fmt, w, h);
    std::mt19937 rng(seed);
    size_t sizes[4] = {};
    ptrdiff_t ls[4] = {f->linesize[0], f->linesize[1], f->linesize[2], f->linesize[3]};
    av_image_fill_plane_sizes(sizes, fmt, h, ls);
    for (int i = 0; i < 4; ++i)
        for (size_t b = 0; b < sizes[i]; ++b)
            f->data[i][b] = uint8_t(rng());
    return f;
}

} // namespace

class TstGpuFrames : public QObject {
    Q_OBJECT
    std::shared_ptr<GpuFramePool> m_gpu;

private slots:
    void initTestCase()
    {
        ff::installLogCallback(AV_LOG_QUIET);
        AVBufferRef* dev = HwDevices::instance().get(AV_HWDEVICE_TYPE_CUDA);
        m_gpu = GpuFramePool::create(dev);
        av_buffer_unref(&dev);
        if (!m_gpu)
            QSKIP("no CUDA device");
    }

    void uploadDownloadRoundTrip_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::newRow("nv12 odd") << int(AV_PIX_FMT_NV12) << 321 << 241;
        QTest::newRow("p010 4k") << int(AV_PIX_FMT_P010LE) << 3840 << 2160;
        QTest::newRow("yuv444p") << int(AV_PIX_FMT_YUV444P) << 100 << 50;
        QTest::newRow("yuv444p16") << int(AV_PIX_FMT_YUV444P16LE) << 64 << 64;
    }

    void uploadDownloadRoundTrip()
    {
        QFETCH(int, format);
        QFETCH(int, w);
        QFETCH(int, h);
        FramePool cpu;
        AVFrame* src = randomCpuFrame(cpu, AVPixelFormat(format), w, h, uint32_t(format + w));
        auto block = m_gpu->upload(src);
        QVERIFY(block);
        QCOMPARE(block->width, w);
        QVERIFY(block->bytes() >= size_t(av_image_get_buffer_size(AVPixelFormat(format), w, h, 1)));
        AVFrame* back = m_gpu->download(*block, cpu);
        QVERIFY(back);
        QCOMPARE(test::frameMd5(back), test::frameMd5(src));
        av_frame_free(&src);
        av_frame_free(&back);
    }

    void blocksAreRecycledAndAccounted()
    {
        const size_t before = m_gpu->liveBytes();
        CUdeviceptr first = 0;
        {
            auto a = m_gpu->allocate(AV_PIX_FMT_NV12, 1920, 1080);
            QVERIFY(a);
            first = a->base;
            QVERIFY(m_gpu->liveBytes() >= before + size_t(1920) * 1620);
        }
        QCOMPARE(m_gpu->liveBytes(), before);
        QVERIFY(m_gpu->cachedBytes() > 0);
        auto b = m_gpu->allocate(AV_PIX_FMT_NV12, 1920, 1080);
        QCOMPARE(b->base, first);
        b.reset();
        m_gpu->trim();
        QCOMPARE(m_gpu->cachedBytes(), size_t(0));
        size_t freeB = 0, total = 0;
        QVERIFY(m_gpu->memoryInfo(&freeB, &total));
        QVERIFY(total > freeB && freeB > 0);
    }

    // A decoder surface copied device-to-device keeps exactly the decoded pixels.
    void copiesDecoderSurfaces()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("hevc10"));
        const auto ref = test::referenceHashes(*fx);
        std::string err;
        auto demux = openDemuxer(fx->path.toStdString(), InputOptions{}, &err);
        QVERIFY(demux);
        const AVStream* st = demux->videoStream();
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        ff::CodecContextPtr ctx(avcodec_alloc_context3(c));
        avcodec_parameters_to_context(ctx.get(), st->codecpar);
        ctx->hw_device_ctx = HwDevices::instance().get(AV_HWDEVICE_TYPE_CUDA);
        ctx->get_format = [](AVCodecContext*, const AVPixelFormat* f) {
            for (; *f != AV_PIX_FMT_NONE; ++f)
                if (*f == AV_PIX_FMT_CUDA)
                    return *f;
            return AV_PIX_FMT_NONE;
        };
        QVERIFY(avcodec_open2(ctx.get(), c, nullptr) >= 0);
        auto pkt = ff::makePacket();
        auto frame = ff::makeFrame();
        FramePool cpu;
        int n = 0;
        while (n < 20 && av_read_frame(demux->fmt.get(), pkt.get()) >= 0) {
            if (pkt->stream_index == demux->stream)
                avcodec_send_packet(ctx.get(), pkt.get());
            av_packet_unref(pkt.get());
            while (avcodec_receive_frame(ctx.get(), frame.get()) == 0) {
                QCOMPARE(frame->format, int(AV_PIX_FMT_CUDA));
                auto block = m_gpu->copyFrom(frame.get());
                av_frame_unref(frame.get());  // the surface goes back to the decoder right away
                QVERIFY(block);
                QCOMPARE(int(block->format), int(AV_PIX_FMT_P010LE));
                AVFrame* sw = m_gpu->download(*block, cpu);
                QCOMPARE(test::frameMd5AsSoftware(sw), ref[size_t(n)]);
                av_frame_free(&sw);
                ++n;
            }
        }
        QCOMPARE(n, 20);
    }
};

QTEST_GUILESS_MAIN(TstGpuFrames)
#include "tst_gpuframes.moc"
