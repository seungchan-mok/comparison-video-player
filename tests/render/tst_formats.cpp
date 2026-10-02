#include "engine/FrameAllocator.h"
#include "render/Renderer.h"
#include "support/GlTestContext.h"

#include <QtTest>

#include <random>

using namespace scvp;
using namespace scvp::render;

namespace {

FramePtr randomFrame(AVPixelFormat fmt, int w, int h, uint32_t seed)
{
    auto f = std::make_shared<Frame>();
    f->av = av_frame_alloc();
    f->av->format = fmt;
    f->av->width = w;
    f->av->height = h;
    av_frame_get_buffer(f->av, 64);
    std::mt19937 rng(seed);
    size_t sizes[4] = {};
    ptrdiff_t ls[4] = {f->av->linesize[0], f->av->linesize[1], f->av->linesize[2], f->av->linesize[3]};
    av_image_fill_plane_sizes(sizes, fmt, h, ls);
    for (int i = 0; i < 4; ++i)
        for (size_t b = 0; b < sizes[i]; ++b)
            f->av->data[i][b] = uint8_t(rng());
    if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt); d->flags & AV_PIX_FMT_FLAG_PAL)
        for (int i = 0; i < 256 * 4; ++i)
            f->av->data[1][i] = uint8_t(rng());
    f->av->color_range = AVCOL_RANGE_MPEG;
    f->av->colorspace = AVCOL_SPC_BT709;
    return f;
}

// The same pixels in another format, converted exactly by swscale (the reference path).
FramePtr convertExact(const FramePtr& src, AVPixelFormat to)
{
    auto f = std::make_shared<Frame>();
    f->av = av_frame_alloc();
    f->av->format = to;
    f->av->width = src->width();
    f->av->height = src->height();
    av_frame_get_buffer(f->av, 64);
    SwsContext* sws = sws_getContext(src->width(), src->height(), src->format(), src->width(), src->height(), to,
                                     SWS_POINT | SWS_BITEXACT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP, nullptr,
                                     nullptr, nullptr);
    sws_scale_frame(sws, f->av, src->av);
    sws_freeContext(sws);
    av_frame_copy_props(f->av, src->av);
    return f;
}

// CPU bilinear demosaic matching convert.frag.
std::array<double, 3> demosaic(const AVFrame* f, int pattern, int bits, int x, int y)
{
    const int w = f->width, h = f->height;
    auto at = [&](int qx, int qy) {
        qx = std::abs(qx);
        qy = std::abs(qy);
        qx = std::min(qx, 2 * (w - 1) - qx);
        qy = std::min(qy, 2 * (h - 1) - qy);
        const uint8_t* row = f->data[0] + ptrdiff_t(qy) * f->linesize[0];
        if (bits > 8) {
            uint16_t v;
            std::memcpy(&v, row + 2 * qx, 2);
            return double(v);
        }
        return double(row[qx]);
    };
    const int rx = pattern == 0 || pattern == 3 ? 0 : 1;
    const int ry = pattern == 0 || pattern == 2 ? 0 : 1;
    const int cx = x & 1, cy = y & 1;
    const double v = at(x, y);
    const double cross = (at(x + 1, y) + at(x - 1, y) + at(x, y + 1) + at(x, y - 1)) / 4;
    const double diag = (at(x + 1, y + 1) + at(x - 1, y - 1) + at(x + 1, y - 1) + at(x - 1, y + 1)) / 4;
    const double horiz = (at(x + 1, y) + at(x - 1, y)) / 2, vert = (at(x, y + 1) + at(x, y - 1)) / 2;
    if (cx == rx && cy == ry)
        return {v, cross, diag};
    if (cx != rx && cy != ry)
        return {diag, cross, v};
    return cy == ry ? std::array<double, 3>{horiz, v, vert} : std::array<double, 3>{vert, v, horiz};
}

} // namespace

class TstFormats : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;
    Renderer m_renderer;
    test::RenderTarget m_target;

    std::vector<uint16_t> convert(const FramePtr& f)
    {
        m_target.create(m_gl.gl(), 8, 8);
        RenderState s;
        s.frames = {f, nullptr};
        s.framebufferSize = QSize(8, 8);
        s.view.setImageSize(QSizeF(f->width(), f->height()));
        s.view.setViewportSize(QSizeF(8, 8));
        m_renderer.render(s, m_target.fbo);
        return m_renderer.converter(0).readRgb(m_gl.gl());
    }

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
        QVERIFY(m_renderer.initialize(m_gl.gl()));
    }

    void cleanupTestCase()
    {
        if (m_gl.gl()) {
            m_target.destroy(m_gl.gl());
            m_renderer.release();
        }
    }

    void bayerDemosaic_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<int>("pattern");
        const struct {
            AVPixelFormat f;
            int p;
        } fmts[] = {{AV_PIX_FMT_BAYER_RGGB8, 0},     {AV_PIX_FMT_BAYER_BGGR8, 1},     {AV_PIX_FMT_BAYER_GRBG8, 2},
                    {AV_PIX_FMT_BAYER_GBRG8, 3},     {AV_PIX_FMT_BAYER_RGGB16LE, 0}, {AV_PIX_FMT_BAYER_BGGR16LE, 1},
                    {AV_PIX_FMT_BAYER_GRBG16LE, 2}, {AV_PIX_FMT_BAYER_GBRG16LE, 3}};
        for (const auto& f : fmts)
            QTest::newRow(av_get_pix_fmt_name(f.f)) << int(f.f) << f.p;
    }

    void bayerDemosaic()
    {
        QFETCH(int, format);
        QFETCH(int, pattern);
        const auto fmt = AVPixelFormat(format);
        const auto layout = PixelLayout::of(fmt);
        QVERIFY(layout && layout->layout == Layout::Bayer);
        const int bits = layout->bitDepth;
        const FramePtr f = randomFrame(fmt, 37, 23, uint32_t(format));
        const auto rgb = convert(f);
        QCOMPARE(int(rgb.size()), 37 * 23 * 3);
        QCOMPARE(m_renderer.converter(0).bitDepth(), bits);
        int worst = 0;
        for (int y = 0; y < 23; ++y)
            for (int x = 0; x < 37; ++x) {
                const auto ref = demosaic(f->av, pattern, bits, x, y);
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(int(rgb[size_t((y * 37 + x) * 3 + c)]) - int(std::floor(ref[size_t(c)] + 0.5))));
            }
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
    }

    // Formats without a shader path render exactly like their supported equivalent.
    void fallbackMatchesEquivalent_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<int>("equivalent");
        QTest::newRow("yuv420p10be") << int(AV_PIX_FMT_YUV420P10BE) << int(AV_PIX_FMT_YUV420P10LE);
        QTest::newRow("gbrp12be") << int(AV_PIX_FMT_GBRP12BE) << int(AV_PIX_FMT_GBRP12LE);
        QTest::newRow("gray16be") << int(AV_PIX_FMT_GRAY16BE) << int(AV_PIX_FMT_GRAY16LE);
        QTest::newRow("pal8") << int(AV_PIX_FMT_PAL8) << int(AV_PIX_FMT_RGB24);
        QTest::newRow("ya8") << int(AV_PIX_FMT_YA8) << int(AV_PIX_FMT_GRAY8);
        QTest::newRow("yuv440p") << int(AV_PIX_FMT_YUV440P) << int(AV_PIX_FMT_YUV440P);
        QTest::newRow("y210le") << int(AV_PIX_FMT_Y210LE) << int(AV_PIX_FMT_YUV422P10LE);
        QTest::newRow("rgb565le") << int(AV_PIX_FMT_RGB565LE) << int(AV_PIX_FMT_RGB24);
    }

    void fallbackMatchesEquivalent()
    {
        QFETCH(int, format);
        QFETCH(int, equivalent);
        const auto fmt = AVPixelFormat(format);
        const AVPixelFormat target = PixelLayout::fallbackFor(fmt);
        QVERIFY2(target != AV_PIX_FMT_NONE, av_get_pix_fmt_name(fmt));
        QVERIFY(PixelLayout::of(target));
        const FramePtr f = randomFrame(fmt, 33, 18, uint32_t(format * 3));
        const auto got = convert(f);
        QVERIFY2(!got.empty(), qPrintable(m_renderer.error()));
        const auto ref = convert(convertExact(f, AVPixelFormat(equivalent)));
        QCOMPARE(got.size(), ref.size());
        int worst = 0;
        for (size_t i = 0; i < got.size(); ++i)
            worst = std::max(worst, std::abs(int(got[i]) - int(ref[i])));
        QVERIFY2(worst <= 1, qPrintable(QStringLiteral("%1 -> %2: %3").arg(av_get_pix_fmt_name(fmt), av_get_pix_fmt_name(target)).arg(worst)));
    }

    void choosesSameDepthAndSubsampling()
    {
        QCOMPARE(PixelLayout::fallbackFor(AV_PIX_FMT_YUV420P10BE), AV_PIX_FMT_YUV420P10LE);
        QCOMPARE(PixelLayout::fallbackFor(AV_PIX_FMT_PAL8), AV_PIX_FMT_RGB24);
        QCOMPARE(PixelLayout::fallbackFor(AV_PIX_FMT_GBRP12BE), AV_PIX_FMT_GBRP12LE);
        QCOMPARE(PixelLayout::fallbackFor(AV_PIX_FMT_NV12), AV_PIX_FMT_NV12);  // supported: unchanged
        QCOMPARE(PixelLayout::fallbackFor(AV_PIX_FMT_CUDA), AV_PIX_FMT_NONE);
    }
};

QTEST_MAIN(TstFormats)
#include "tst_formats.moc"
