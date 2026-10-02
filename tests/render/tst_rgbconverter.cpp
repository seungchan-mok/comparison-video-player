#include "engine/FrameAllocator.h"
#include "render/CodeReader.h"
#include "render/RgbConverter.h"
#include "support/GlTestContext.h"

#include <QtTest>

#include <random>

using namespace scvp;
using namespace scvp::render;

namespace {

// A frame with random bytes in every plane (both GPU and CPU paths apply the same shift/mask).
std::shared_ptr<Frame> randomFrame(FramePool& pool, AVPixelFormat fmt, int w, int h, uint32_t seed)
{
    auto f = std::make_shared<Frame>();
    f->av = pool.allocFrame(fmt, w, h);
    std::mt19937 rng(seed);
    size_t sizes[4] = {};
    ptrdiff_t ls[4] = {f->av->linesize[0], f->av->linesize[1], f->av->linesize[2], f->av->linesize[3]};
    av_image_fill_plane_sizes(sizes, fmt, h, ls);
    for (int i = 0; i < 4; ++i)
        for (size_t b = 0; b < sizes[i]; ++b)
            f->av->data[i][b] = uint8_t(rng());
    return f;
}

struct Diff {
    int64_t mismatches = 0;
    int maxDelta = 0;
    int64_t codeMismatches = 0;
};

Diff compare(GL* gl, const RgbConverter& conv, const AVFrame* av, const PixelLayout& l, const ColorParams& c)
{
    Diff d;
    const auto rgb = conv.readRgb(gl);
    const auto codes = conv.readCodes(gl);
    for (int y = 0; y < av->height; ++y)
        for (int x = 0; x < av->width; ++x) {
            const auto k = readCodes(av, l, x, y);
            const auto ref = c.toRgb(k[0], k[1], k[2]);
            const size_t i = (size_t(y) * size_t(av->width) + size_t(x)) * 3;
            bool bad = false;
            for (int ch = 0; ch < 3; ++ch) {
                const int delta = std::abs(int(rgb[i + size_t(ch)]) - ref[size_t(ch)]);
                d.maxDelta = std::max(d.maxDelta, delta);
                bad |= delta != 0;
                if (!codes.empty() && codes[i + size_t(ch)] != k[size_t(ch)])
                    ++d.codeMismatches;
            }
            d.mismatches += bad ? 1 : 0;
        }
    return d;
}

} // namespace

class TstRgbConverter : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;
    ShaderLibrary m_shaders;
    CpuUploadSource m_upload;
    RgbConverter m_conv;
    FramePool m_pool;

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context (needs an X display for the offscreen platform)");
        qInfo("GL renderer: %s", qPrintable(m_gl.renderer()));
    }

    void cleanupTestCase()
    {
        if (m_gl.gl()) {
            m_upload.release(m_gl.gl());
            m_conv.release(m_gl.gl());
        }
    }

    void matchesCpuReference_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<int>("matrix");
        QTest::addColumn<int>("range");
        const AVPixelFormat yuv[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV410P,
                                     AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV444P12LE,
                                     AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_NV12, AV_PIX_FMT_NV21, AV_PIX_FMT_NV16,
                                     AV_PIX_FMT_NV24, AV_PIX_FMT_P010LE, AV_PIX_FMT_P016LE, AV_PIX_FMT_YUYV422,
                                     AV_PIX_FMT_UYVY422, AV_PIX_FMT_YVYU422};
        const AVColorSpace matrices[] = {AVCOL_SPC_BT470BG, AVCOL_SPC_BT709, AVCOL_SPC_BT2020_NCL};
        const AVColorRange ranges[] = {AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG};
        for (AVPixelFormat f : yuv)
            for (AVColorSpace m : matrices)
                for (AVColorRange r : ranges)
                    QTest::newRow(qPrintable(QStringLiteral("%1 %2 %3").arg(av_get_pix_fmt_name(f), ColorParams::matrixName(m),
                                                                             r == AVCOL_RANGE_MPEG ? "limited" : "full")))
                        << int(f) << int(m) << int(r);
        const AVPixelFormat rgb[] = {AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP10LE, AV_PIX_FMT_GBRP16LE, AV_PIX_FMT_RGB24,
                                     AV_PIX_FMT_BGR24, AV_PIX_FMT_RGBA, AV_PIX_FMT_BGRA, AV_PIX_FMT_ARGB,
                                     AV_PIX_FMT_RGB0, AV_PIX_FMT_RGB48LE, AV_PIX_FMT_RGBA64LE, AV_PIX_FMT_GRAY8,
                                     AV_PIX_FMT_GRAY10LE, AV_PIX_FMT_GRAY16LE};
        for (AVPixelFormat f : rgb)
            for (AVColorRange r : ranges)
                QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(av_get_pix_fmt_name(f), r == AVCOL_RANGE_MPEG ? "limited" : "full")))
                    << int(f) << int(AVCOL_SPC_RGB) << int(r);
    }

    void matchesCpuReference()
    {
        QFETCH(int, format);
        QFETCH(int, matrix);
        QFETCH(int, range);
        GL* gl = m_gl.gl();
        const auto fmt = AVPixelFormat(format);
        const auto l = *PixelLayout::of(fmt);
        const ColorParams c = ColorParams::resolve(l, AVColorSpace(matrix), AVColorRange(range), 1080);
        for (const auto& size : {std::pair{37, 23}, std::pair{1, 1}, std::pair{64, 2}}) {
            auto f = randomFrame(m_pool, fmt, size.first, size.second, uint32_t(format * 31 + matrix * 7 + range));
            QVERIFY(m_upload.update(gl, *f));
            QVERIFY2(m_conv.convert(gl, m_shaders, m_upload, c, true), qPrintable(m_conv.error()));
            const Diff d = compare(gl, m_conv, f->av, l, c);
            QCOMPARE(d.codeMismatches, int64_t(0));
            const bool identity = !l.isYuv() && range == AVCOL_RANGE_JPEG;
            if (identity) {
                QCOMPARE(d.mismatches, int64_t(0));
            } else {
                QVERIFY2(d.maxDelta <= 1, qPrintable(QStringLiteral("max delta %1").arg(d.maxDelta)));
                QVERIFY2(d.mismatches <= std::max<int64_t>(2, int64_t(size.first) * size.second / 1000),
                         qPrintable(QStringLiteral("%1 pixels off by one").arg(d.mismatches)));
            }
        }
        QCOMPARE(int(gl->glGetError()), int(GL_NO_ERROR));
    }

    void negativeLinesizeIsFlipped()
    {
        GL* gl = m_gl.gl();
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        const ColorParams c = ColorParams::resolve(l, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080);
        auto f = randomFrame(m_pool, AV_PIX_FMT_YUV420P, 30, 20, 99);
        // A view of the same memory read bottom-up.
        auto view = std::make_shared<Frame>();
        view->av = av_frame_clone(f->av);
        for (int i = 0; i < 3; ++i) {
            const int h = l.planeHeight(i, 20);
            view->av->data[i] = f->av->data[i] + ptrdiff_t(h - 1) * f->av->linesize[i];
            view->av->linesize[i] = -f->av->linesize[i];
        }
        QVERIFY(m_upload.update(gl, *view));
        QVERIFY(m_upload.flipY());
        QVERIFY(m_conv.convert(gl, m_shaders, m_upload, c, true));
        const Diff d = compare(gl, m_conv, view->av, l, c);
        QCOMPARE(d.codeMismatches, int64_t(0));
        QVERIFY(d.maxDelta <= 1);
    }

    void exhaustive8BitYuv_data()
    {
        QTest::addColumn<int>("matrix");
        QTest::addColumn<int>("range");
        QTest::newRow("709 limited") << int(AVCOL_SPC_BT709) << int(AVCOL_RANGE_MPEG);
        QTest::newRow("601 full") << int(AVCOL_SPC_BT470BG) << int(AVCOL_RANGE_JPEG);
    }

    // Every (Y, Cb, Cr) combination once: 4096 x 4096 pixels of yuv444p.
    void exhaustive8BitYuv()
    {
        QFETCH(int, matrix);
        QFETCH(int, range);
        GL* gl = m_gl.gl();
        const int w = 4096;
        const int h = 4096;
        auto f = std::make_shared<Frame>();
        f->av = m_pool.allocFrame(AV_PIX_FMT_YUV444P, w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint32_t i = uint32_t(y) * uint32_t(w) + uint32_t(x);
                f->av->data[0][size_t(y) * size_t(f->av->linesize[0]) + size_t(x)] = uint8_t(i >> 16);
                f->av->data[1][size_t(y) * size_t(f->av->linesize[1]) + size_t(x)] = uint8_t(i >> 8);
                f->av->data[2][size_t(y) * size_t(f->av->linesize[2]) + size_t(x)] = uint8_t(i);
            }
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV444P);
        const ColorParams c = ColorParams::resolve(l, AVColorSpace(matrix), AVColorRange(range), 1080);
        QVERIFY(m_upload.update(gl, *f));
        QVERIFY(m_conv.convert(gl, m_shaders, m_upload, c, false));
        const auto rgb = m_conv.readRgb(gl);
        int64_t off = 0;
        int maxDelta = 0;
        for (uint32_t i = 0; i < uint32_t(w) * uint32_t(h); ++i) {
            const auto ref = c.toRgb(i >> 16, (i >> 8) & 255, i & 255);
            bool bad = false;
            for (int ch = 0; ch < 3; ++ch) {
                const int delta = std::abs(int(rgb[size_t(i) * 3 + size_t(ch)]) - ref[size_t(ch)]);
                maxDelta = std::max(maxDelta, delta);
                bad |= delta != 0;
            }
            off += bad ? 1 : 0;
        }
        qInfo("%lld of %d combinations differ from the CPU reference (max %d LSB)", (long long)off, w * h, maxDelta);
        QVERIFY(maxDelta <= 1);
        QVERIFY(off <= int64_t(w) * h / 10000);  // <= 0.01 %
    }
};

QTEST_MAIN(TstRgbConverter)
#include "tst_rgbconverter.moc"
