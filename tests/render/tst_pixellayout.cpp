#include "render/ColorParams.h"
#include "render/PixelLayout.h"

#include <QtTest>

using namespace scvp::render;

namespace {

std::array<int, 3> rgb(int r, int g, int b) { return {r, g, b}; }

bool near(const std::array<int, 3>& a, const std::array<int, 3>& b, int tol = 1)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(a[size_t(i)] - b[size_t(i)]) > tol)
            return false;
    return true;
}

QString str(const std::array<int, 3>& v) { return QStringLiteral("(%1,%2,%3)").arg(v[0]).arg(v[1]).arg(v[2]); }

} // namespace

class TstPixelLayout : public QObject {
    Q_OBJECT

private slots:
    void supportedFormats_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<int>("layout");
        QTest::addColumn<int>("planes");
        QTest::addColumn<int>("depth");
        const struct {
            AVPixelFormat f;
            Layout l;
            int planes, depth;
        } rows[] = {
            {AV_PIX_FMT_YUV420P, Layout::PlanarYuv, 3, 8},     {AV_PIX_FMT_YUVJ420P, Layout::PlanarYuv, 3, 8},
            {AV_PIX_FMT_YUV422P, Layout::PlanarYuv, 3, 8},     {AV_PIX_FMT_YUV444P, Layout::PlanarYuv, 3, 8},
            {AV_PIX_FMT_YUV410P, Layout::PlanarYuv, 3, 8},     {AV_PIX_FMT_YUVA420P, Layout::PlanarYuv, 3, 8},
            {AV_PIX_FMT_YUV420P10LE, Layout::PlanarYuv, 3, 10}, {AV_PIX_FMT_YUV422P10LE, Layout::PlanarYuv, 3, 10},
            {AV_PIX_FMT_YUV444P12LE, Layout::PlanarYuv, 3, 12}, {AV_PIX_FMT_YUV444P16LE, Layout::PlanarYuv, 3, 16},
            {AV_PIX_FMT_NV12, Layout::SemiYuv, 2, 8},           {AV_PIX_FMT_NV21, Layout::SemiYuv, 2, 8},
            {AV_PIX_FMT_NV16, Layout::SemiYuv, 2, 8},           {AV_PIX_FMT_NV24, Layout::SemiYuv, 2, 8},
            {AV_PIX_FMT_P010LE, Layout::SemiYuv, 2, 10},        {AV_PIX_FMT_P016LE, Layout::SemiYuv, 2, 16},
            {AV_PIX_FMT_YUYV422, Layout::Packed422, 1, 8},      {AV_PIX_FMT_UYVY422, Layout::Packed422, 1, 8},
            {AV_PIX_FMT_YVYU422, Layout::Packed422, 1, 8},      {AV_PIX_FMT_GBRP, Layout::PlanarRgb, 3, 8},
            {AV_PIX_FMT_GBRP10LE, Layout::PlanarRgb, 3, 10},    {AV_PIX_FMT_GBRAP, Layout::PlanarRgb, 3, 8},
            {AV_PIX_FMT_RGB24, Layout::PackedRgb, 1, 8},        {AV_PIX_FMT_BGR24, Layout::PackedRgb, 1, 8},
            {AV_PIX_FMT_RGBA, Layout::PackedRgb, 1, 8},         {AV_PIX_FMT_BGRA, Layout::PackedRgb, 1, 8},
            {AV_PIX_FMT_ARGB, Layout::PackedRgb, 1, 8},         {AV_PIX_FMT_RGB0, Layout::PackedRgb, 1, 8},
            {AV_PIX_FMT_RGB48LE, Layout::PackedRgb, 1, 16},     {AV_PIX_FMT_RGBA64LE, Layout::PackedRgb, 1, 16},
            {AV_PIX_FMT_GRAY8, Layout::Gray, 1, 8},             {AV_PIX_FMT_GRAY10LE, Layout::Gray, 1, 10},
            {AV_PIX_FMT_GRAY16LE, Layout::Gray, 1, 16},
        };
        for (const auto& r : rows)
            QTest::newRow(av_get_pix_fmt_name(r.f)) << int(r.f) << int(r.l) << r.planes << r.depth;
    }

    void supportedFormats()
    {
        QFETCH(int, format);
        QFETCH(int, layout);
        QFETCH(int, planes);
        QFETCH(int, depth);
        const auto l = PixelLayout::of(AVPixelFormat(format));
        QVERIFY(l);
        QCOMPARE(int(l->layout), layout);
        QCOMPARE(l->planes, planes);
        QCOMPARE(l->bitDepth, depth);
        QCOMPARE(l->mask, (1u << depth) - 1u);
    }

    void unsupportedFormats()
    {
        for (AVPixelFormat f : {AV_PIX_FMT_PAL8, AV_PIX_FMT_CUDA, AV_PIX_FMT_RGB565LE, AV_PIX_FMT_BAYER_RGGB8,
                                AV_PIX_FMT_YUV420P10BE, AV_PIX_FMT_RGB48BE, AV_PIX_FMT_MONOBLACK, AV_PIX_FMT_YA8,
                                AV_PIX_FMT_GRAYF32LE})
            QVERIFY2(!PixelLayout::of(f), av_get_pix_fmt_name(f));
    }

    void swizzles()
    {
        QCOMPARE(PixelLayout::of(AV_PIX_FMT_NV12)->swizzle[0], 0);
        QCOMPARE(PixelLayout::of(AV_PIX_FMT_NV21)->swizzle[0], 1);
        QCOMPARE(PixelLayout::of(AV_PIX_FMT_P010LE)->shift, 6);
        QCOMPARE(PixelLayout::of(AV_PIX_FMT_P010LE)->swizzle[1], 1);
        const auto yuyv = *PixelLayout::of(AV_PIX_FMT_YUYV422);
        QVERIFY((yuyv.swizzle == std::array<int, 4>{0, 2, 1, 3}));
        const auto uyvy = *PixelLayout::of(AV_PIX_FMT_UYVY422);
        QVERIFY((uyvy.swizzle == std::array<int, 4>{1, 3, 0, 2}));
        QVERIFY((PixelLayout::of(AV_PIX_FMT_BGR24)->swizzle[0] == 2 && PixelLayout::of(AV_PIX_FMT_BGR24)->swizzle[2] == 0));
        QCOMPARE(PixelLayout::of(AV_PIX_FMT_ARGB)->swizzle[0], 1);
    }

    void planeSizes()
    {
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        QCOMPARE(l.planeWidth(0, 3), 3);
        QCOMPARE(l.planeWidth(1, 3), 2);
        QCOMPARE(l.planeHeight(1, 3), 2);
        const auto y410 = *PixelLayout::of(AV_PIX_FMT_YUV410P);
        QCOMPARE(y410.planeWidth(1, 5), 2);
        const auto uyvy = *PixelLayout::of(AV_PIX_FMT_UYVY422);
        QCOMPARE(uyvy.planeWidth(0, 5), 3);
        QCOMPARE(uyvy.planeHeight(0, 5), 5);
    }

    void limitedRangeExtremes()
    {
        const auto l8 = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        const auto c8 = ColorParams::resolve(l8, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080);
        QCOMPARE(c8.toRgb(235, 128, 128), rgb(255, 255, 255));
        QCOMPARE(c8.toRgb(16, 128, 128), rgb(0, 0, 0));
        QCOMPARE(c8.toRgb(255, 128, 128), rgb(255, 255, 255));  // super-white clamps
        const auto l10 = *PixelLayout::of(AV_PIX_FMT_YUV420P10LE);
        const auto c10 = ColorParams::resolve(l10, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080);
        QCOMPARE(c10.outMax, 1023.0f);
        QCOMPARE(c10.toRgb(940, 512, 512), rgb(1023, 1023, 1023));
        QCOMPARE(c10.toRgb(64, 512, 512), rgb(0, 0, 0));
        // P010 shares the 10-bit numbers once the codes are shifted down.
        const auto p010 = *PixelLayout::of(AV_PIX_FMT_P010LE);
        const auto cp = ColorParams::resolve(p010, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080);
        QCOMPARE(cp.toRgb(60160u >> 6, 32768u >> 6, 32768u >> 6), rgb(1023, 1023, 1023));
    }

    void bt709Red()
    {
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        const auto c = ColorParams::resolve(l, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080);
        const auto v = c.toRgb(63, 102, 240);
        QVERIFY2(near(v, rgb(255, 0, 0)), qPrintable(str(v)));
    }

    void bt601VsBt709Differ()
    {
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        const auto c601 = ColorParams::resolve(l, AVCOL_SPC_BT470BG, AVCOL_RANGE_MPEG, 480);
        const auto c709 = ColorParams::resolve(l, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080);
        QVERIFY(c601.toRgb(81, 90, 240) != c709.toRgb(81, 90, 240));
        // 601 red: Y=81 Cb=90 Cr=240
        QVERIFY2(near(c601.toRgb(81, 90, 240), rgb(255, 0, 0), 2), qPrintable(str(c601.toRgb(81, 90, 240))));
    }

    void defaultsFromMetadata()
    {
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        QCOMPARE(int(ColorParams::resolve(l, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, 1080).matrixUsed), int(AVCOL_SPC_BT709));
        QCOMPARE(int(ColorParams::resolve(l, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, 480).matrixUsed), int(AVCOL_SPC_BT470BG));
        QCOMPARE(int(ColorParams::resolve(l, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, 480).rangeUsed), int(AVCOL_RANGE_MPEG));
        const auto lj = *PixelLayout::of(AV_PIX_FMT_YUVJ420P);
        QCOMPARE(int(ColorParams::resolve(lj, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, 480).rangeUsed), int(AVCOL_RANGE_JPEG));
        const auto lr = *PixelLayout::of(AV_PIX_FMT_RGB24);
        QCOMPARE(int(ColorParams::resolve(lr, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, 480).rangeUsed), int(AVCOL_RANGE_JPEG));
    }

    void overridesWin()
    {
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUV420P);
        ColorOverrides o;
        o.matrix = AVCOL_SPC_BT2020_NCL;
        o.range = AVCOL_RANGE_JPEG;
        const auto c = ColorParams::resolve(l, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 1080, o);
        QCOMPARE(int(c.matrixUsed), int(AVCOL_SPC_BT2020_NCL));
        QCOMPARE(int(c.rangeUsed), int(AVCOL_RANGE_JPEG));
        QCOMPARE(c.toRgb(255, 128, 128), rgb(255, 255, 255));
    }

    void fullRangeYuv()
    {
        const auto l = *PixelLayout::of(AV_PIX_FMT_YUVJ420P);
        const auto c = ColorParams::resolve(l, AVCOL_SPC_BT470BG, AVCOL_RANGE_JPEG, 480);
        QCOMPARE(c.toRgb(255, 128, 128), rgb(255, 255, 255));
        QCOMPARE(c.toRgb(0, 128, 128), rgb(0, 0, 0));
        QCOMPARE(c.toRgb(128, 128, 128), rgb(128, 128, 128));
    }

    void rgbAndGrayAreIdentity()
    {
        const auto c = ColorParams::resolve(*PixelLayout::of(AV_PIX_FMT_RGB24), AVCOL_SPC_RGB, AVCOL_RANGE_JPEG, 480);
        QCOMPARE(c.toRgb(12, 34, 56), rgb(12, 34, 56));
        const auto g = ColorParams::resolve(*PixelLayout::of(AV_PIX_FMT_GRAY10LE), AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, 480);
        QCOMPARE(g.toRgb(700, 0, 0), rgb(700, 700, 700));
        const auto g16 = ColorParams::resolve(*PixelLayout::of(AV_PIX_FMT_RGB48LE), AVCOL_SPC_RGB, AVCOL_RANGE_JPEG, 480);
        QCOMPARE(g16.toRgb(65535, 1, 40000), rgb(65535, 1, 40000));
    }

    void exhaustiveIdentityRgb8()
    {
        const auto c = ColorParams::resolve(*PixelLayout::of(AV_PIX_FMT_RGB24), AVCOL_SPC_RGB, AVCOL_RANGE_JPEG, 480);
        for (int v = 0; v < 256; ++v)
            QCOMPARE(c.toRgb(uint32_t(v), uint32_t(255 - v), uint32_t(v / 2)), rgb(v, 255 - v, v / 2));
    }
};

QTEST_APPLESS_MAIN(TstPixelLayout)
#include "tst_pixellayout.moc"
