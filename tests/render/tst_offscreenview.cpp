#include "engine/FrameAllocator.h"
#include "engine/VideoEncoder.h"
#include "render/OffscreenView.h"
#include "support/GlTestContext.h"

#include <QPainter>
#include <QtTest>

#include <random>

using namespace scvp;
using namespace scvp::render;

namespace {

FramePtr randomRgb(FramePool& pool, int w, int h, unsigned seed)
{
    auto f = std::make_shared<Frame>();
    f->av = pool.allocFrame(AV_PIX_FMT_RGB24, w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w * 3; ++x)
            f->av->data[0][size_t(y) * size_t(f->av->linesize[0]) + size_t(x)] = uint8_t(rng());
    return f;
}

std::array<int, 3> src(const AVFrame* f, int x, int y)
{
    const uint8_t* p = f->data[0] + ptrdiff_t(y) * f->linesize[0] + x * 3;
    return {p[0], p[1], p[2]};
}

// R, G, B at (x, y) (top-left origin) of a read-back picture: BGRX rows, bottom row first.
std::array<int, 3> at(const std::vector<uint8_t>& px, QSize size, int x, int y)
{
    const uint8_t* p = &px[(size_t(size.height() - 1 - y) * size_t(size.width()) + size_t(x)) * 4];
    return {p[2], p[1], p[0]};
}

std::array<int, 3> rgb(const QImage& img, int x, int y)
{
    const QRgb p = img.pixel(x, y);
    return {qRed(p), qGreen(p), qBlue(p)};
}

} // namespace

class TstOffscreenView : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;
    Renderer m_renderer;
    FramePool m_pool;
    FramePtr m_a;
    FramePtr m_b;

    RenderState base(CompareSettings c) const
    {
        RenderState s;
        s.frames = {m_a, m_b};
        s.background = QColor(10, 20, 30);
        s.divider = QColor(200, 100, 50);
        s.compare = c;
        return s;
    }

    std::vector<uint8_t> draw(OffscreenView& v, const RenderState& s)
    {
        std::vector<uint8_t> px;
        if (!v.render(m_renderer, s) || !v.take(px, true))
            return {};
        return px;
    }

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
        QVERIFY(m_renderer.initialize(m_gl.gl()));
        m_a = randomRgb(m_pool, 64, 48, 5);
        m_b = randomRgb(m_pool, 64, 48, 77);
    }

    void cleanupTestCase()
    {
        if (m_gl.gl())
            m_renderer.release();
    }

    // Each video 1:1 in its pane, the divider between split panes; one picture for the rest.
    void fullFramesAreOneToOne_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("orientation");
        QTest::addColumn<QSize>("size");
        QTest::newRow("split auto") << int(CompareMode::Split) << int(SplitOrientation::Auto) << QSize(130, 48);
        QTest::newRow("split top/bottom") << int(CompareMode::Split) << int(SplitOrientation::TopBottom) << QSize(64, 98);
        QTest::newRow("wipe") << int(CompareMode::Wipe) << int(SplitOrientation::Auto) << QSize(64, 48);
        QTest::newRow("flip") << int(CompareMode::Flip) << int(SplitOrientation::Auto) << QSize(64, 48);
        QTest::newRow("difference") << int(CompareMode::Difference) << int(SplitOrientation::Auto) << QSize(64, 48);
    }
    void fullFramesAreOneToOne()
    {
        QFETCH(int, mode);
        QFETCH(int, orientation);
        QFETCH(QSize, size);
        CompareSettings c;
        c.mode = CompareMode(mode);
        c.orientation = SplitOrientation(orientation);
        c.wipe = 0.25;
        c.flipShowsB = true;
        const RenderState s = OffscreenView::fullFrameState(base(c), QSizeF(64, 48), true, true);
        QCOMPARE(s.framebufferSize, size);
        QCOMPARE(s.view.scale(), 1.0);
        OffscreenView v;
        QString err;
        QVERIFY2(v.create(m_gl.gl(), size, &err), qPrintable(err));
        const auto px = draw(v, s);
        QCOMPARE(px.size(), size_t(size.width()) * size.height() * 4);
        const std::array<int, 3> line{200, 100, 50};
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                std::array<int, 3> want;
                switch (c.mode) {
                case CompareMode::Split:
                    if (size.width() > 64)
                        want = x < 64 ? src(m_a->av, x, y) : x < 66 ? line : src(m_b->av, x - 66, y);
                    else
                        want = y < 48 ? src(m_a->av, x, y) : y < 50 ? line : src(m_b->av, x, y - 50);
                    break;
                case CompareMode::Wipe: want = x < 16 ? src(m_a->av, x, y) : x < 17 ? line : src(m_b->av, x, y); break;
                case CompareMode::Flip: want = src(m_b->av, x, y); break;
                case CompareMode::Difference: {
                    const auto p = src(m_a->av, x, y), q = src(m_b->av, x, y);
                    want = {std::abs(p[0] - q[0]), std::abs(p[1] - q[1]), std::abs(p[2] - q[2])};
                    break;
                }
                }
                if (at(px, size, x, y) != want)
                    QFAIL(qPrintable(QStringLiteral("pixel %1,%2").arg(x).arg(y)));
            }
        v.release();
    }

    // Smaller: half the size in each direction, divider kept.
    void fullFramesScaled()
    {
        CompareSettings c;
        const RenderState s = OffscreenView::fullFrameState(base(c), QSizeF(64, 48), true, true, 0.5);
        QCOMPARE(s.framebufferSize, QSize(66, 24));
        QCOMPARE(s.view.scale(), 0.5);
        const RenderState one = OffscreenView::fullFrameState(base(c), QSizeF(64, 48), true, false, 0.5);
        QCOMPARE(one.framebufferSize, QSize(32, 24));
    }

    // The same picture as a capture of the same state (here a fractional, smoothed view).
    void sameAsCapture()
    {
        CompareSettings c;
        c.mode = CompareMode::Wipe;
        c.wipe = 0.37;
        RenderState s = base(c);
        s.framebufferSize = QSize(100, 60);
        s.view.setImageSize(QSizeF(64, 48));
        s.view.setViewportSize(QSizeF(100, 60));
        QCOMPARE(s.view.scale(), 1.25);
        const QImage img = m_renderer.capture(s, 1.0);
        OffscreenView v;
        QVERIFY(v.create(m_gl.gl(), s.framebufferSize, nullptr));
        const auto px = draw(v, s);
        QVERIFY(!px.empty());
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 100; ++x)
                QCOMPARE(at(px, s.framebufferSize, x, y), rgb(img, x, y));
        v.release();
    }

    // Layers: transparent parts leave the view, opaque ones replace it, the rest is premultiplied
    // "source over" (as QPainter composes); a later layer covers an earlier one; removed layers
    // are gone; parts outside the picture are clipped.
    void layersBlendOverTheView()
    {
        CompareSettings c;
        c.mode = CompareMode::Flip;  // A alone
        const RenderState s = OffscreenView::fullFrameState(base(c), QSizeF(64, 48), true, true);
        OffscreenView v;
        QVERIFY(v.create(m_gl.gl(), s.framebufferSize, nullptr));
        QImage layer(20, 10, QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        for (int x = 0; x < 10; ++x)
            layer.setPixel(x, 0, qPremultiply(qRgba(255, 0, 0, 255)));
        for (int x = 0; x < 10; ++x)
            layer.setPixel(x, 1, qPremultiply(qRgba(0, 0, 255, 128)));
        v.setLayer(0, layer, QPoint(5, 7));
        QImage top(4, 4, QImage::Format_ARGB32_Premultiplied);
        top.fill(qRgba(0, 255, 0, 255));
        v.setLayer(1, top, QPoint(62, 46));  // half outside
        auto px = draw(v, s);
        QCOMPARE(at(px, s.framebufferSize, 5, 7), (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(at(px, s.framebufferSize, 14, 7), (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(at(px, s.framebufferSize, 15, 7), src(m_a->av, 15, 7));  // transparent
        QCOMPARE(at(px, s.framebufferSize, 4, 7), src(m_a->av, 4, 7));
        const QRgb half = qPremultiply(qRgba(0, 0, 255, 128));
        for (int x = 5; x < 15; ++x) {
            const auto d = src(m_a->av, x, 8);
            const auto got = at(px, s.framebufferSize, x, 8);
            const double k = 1.0 - qAlpha(half) / 255.0;
            const std::array<double, 3> want{qRed(half) + d[0] * k, qGreen(half) + d[1] * k, qBlue(half) + d[2] * k};
            for (int i = 0; i < 3; ++i)
                QVERIFY2(std::abs(got[size_t(i)] - want[size_t(i)]) <= 1.0, qPrintable(QStringLiteral("x %1").arg(x)));
        }
        QCOMPARE(at(px, s.framebufferSize, 63, 47), (std::array<int, 3>{0, 255, 0}));
        QCOMPARE(at(px, s.framebufferSize, 61, 47), src(m_a->av, 61, 47));

        // A new image for layer 0 replaces it; a null image removes layer 1.
        layer.fill(Qt::transparent);
        layer.setPixel(0, 0, qRgba(255, 255, 255, 255));
        v.setLayer(0, layer, QPoint(0, 0));
        v.setLayer(1, QImage(), QPoint());
        px = draw(v, s);
        QCOMPARE(at(px, s.framebufferSize, 0, 0), (std::array<int, 3>{255, 255, 255}));
        QCOMPARE(at(px, s.framebufferSize, 5, 7), src(m_a->av, 5, 7));
        QCOMPARE(at(px, s.framebufferSize, 63, 47), src(m_a->av, 63, 47));
        v.release();
    }

    // Pictures come back in the order they were drawn; at most kInFlight wait at a time.
    void readBackInOrder()
    {
        CompareSettings c;
        c.mode = CompareMode::Flip;
        RenderState s = OffscreenView::fullFrameState(base(c), QSizeF(64, 48), true, false);
        OffscreenView v;
        QVERIFY(v.create(m_gl.gl(), s.framebufferSize, nullptr));
        std::vector<FramePtr> frames;
        for (int i = 0; i < OffscreenView::kInFlight; ++i) {
            frames.push_back(randomRgb(m_pool, 64, 48, 100 + unsigned(i)));
            s.frames = {frames.back(), nullptr};
            QVERIFY(v.render(m_renderer, s));
        }
        QCOMPARE(v.pending(), OffscreenView::kInFlight);
        QVERIFY(!v.render(m_renderer, s));  // full: take one first
        std::vector<uint8_t> px;
        for (int i = 0; i < OffscreenView::kInFlight; ++i) {
            QVERIFY(v.take(px, true));
            QCOMPARE(at(px, s.framebufferSize, 3, 4), src(frames[size_t(i)]->av, 3, 4));
            QCOMPARE(at(px, s.framebufferSize, 60, 40), src(frames[size_t(i)]->av, 60, 40));
        }
        QCOMPARE(v.pending(), 0);
        QVERIFY(!v.take(px, true));
        v.release();
    }

    // On the GPU, the 4:2:0 planes an encoder takes are exactly what the encoder's own CPU
    // conversion makes of the BGRX picture (odd sizes: last column/row repeated), with the
    // overlay in them.
    void yuvPlanesAreTheEncoderConversion_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<QSize>("size");
        QTest::newRow("nv12") << int(PlaneLayout::Nv12) << QSize(64, 48);
        QTest::newRow("i420") << int(PlaneLayout::I420) << QSize(64, 48);
        QTest::newRow("nv12 odd") << int(PlaneLayout::Nv12) << QSize(63, 47);
        QTest::newRow("i420 odd") << int(PlaneLayout::I420) << QSize(63, 47);
    }
    void yuvPlanesAreTheEncoderConversion()
    {
        QFETCH(int, layout);
        QFETCH(QSize, size);
        CompareSettings c;
        c.mode = CompareMode::Flip;
        const FramePtr frame = randomRgb(m_pool, size.width(), size.height(), 9);
        RenderState s = OffscreenView::fullFrameState(base(c), QSizeF(size), true, false);
        s.frames = {frame, nullptr};
        QImage layer(10, 6, QImage::Format_ARGB32_Premultiplied);
        layer.fill(qPremultiply(qRgba(250, 20, 90, 160)));

        OffscreenView rgb;
        QVERIFY(rgb.create(m_gl.gl(), size, nullptr));
        rgb.setLayer(0, layer, QPoint(size.width() - 6, 2));
        const auto bgrx = draw(rgb, s);
        QVERIFY(!bgrx.empty());
        OffscreenView yuv;
        QString err;
        QVERIFY2(yuv.create(m_gl.gl(), size, &err, PlaneLayout(layout)), qPrintable(err));
        yuv.setLayer(0, layer, QPoint(size.width() - 6, 2));
        const QSize planes((size.width() + 1) & ~1, (size.height() + 1) & ~1);
        QCOMPARE(yuv.planeSize(), planes);
        QCOMPARE(yuv.pictureBytes(), size_t(planes.width() * planes.height() * 3 / 2));

        const AVPixelFormat fmt = PlaneLayout(layout) == PlaneLayout::Nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        AVFrame* want = av_frame_alloc();
        want->format = fmt;
        want->width = planes.width();
        want->height = planes.height();
        QVERIFY(av_frame_get_buffer(want, 0) >= 0);
        const int stride = size.width() * 4;
        bgrxToYuv420(&bgrx[size_t(size.height() - 1) * size_t(stride)], -stride, size.width(), size.height(), want);
        AVFrame* got = av_frame_alloc();
        got->format = fmt;
        got->width = planes.width();
        got->height = planes.height();
        QVERIFY(av_frame_get_buffer(got, 0) >= 0);
        QVERIFY(yuv.render(m_renderer, s));
        QVERIFY(yuv.take(got, true));
        auto samePlane = [&](int plane, int rowBytes, int rows) {
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < rowBytes; ++x) {
                    const int a = got->data[plane][y * got->linesize[plane] + x];
                    const int b = want->data[plane][y * want->linesize[plane] + x];
                    if (a != b) {
                        qWarning("plane %d at %d,%d: GPU %d, CPU %d", plane, x, y, a, b);
                        return false;
                    }
                }
            return true;
        };
        QVERIFY(samePlane(0, planes.width(), planes.height()));
        if (fmt == AV_PIX_FMT_NV12) {
            QVERIFY(samePlane(1, planes.width(), planes.height() / 2));
        } else {
            QVERIFY(samePlane(1, planes.width() / 2, planes.height() / 2));
            QVERIFY(samePlane(2, planes.width() / 2, planes.height() / 2));
        }
        // The packed read-back: the same bytes, plane after plane.
        std::vector<uint8_t> packed;
        QVERIFY(yuv.render(m_renderer, s) && yuv.take(packed, true));
        QCOMPARE(packed.size(), yuv.pictureBytes());
        QCOMPARE(int(packed[size_t(planes.width()) * 3 + 5]), int(want->data[0][3 * want->linesize[0] + 5]));
        QCOMPARE(int(packed[size_t(planes.width()) * planes.height() + 1]), int(want->data[1][1]));
        av_frame_free(&want);
        av_frame_free(&got);
        rgb.release();
        yuv.release();
    }

    void tooLargeFails()
    {
        OffscreenView v;
        QString err;
        QVERIFY(!v.create(m_gl.gl(), QSize(1 << 20, 16), &err));
        QVERIFY(!err.isEmpty());
    }
};

QTEST_MAIN(TstOffscreenView)
#include "tst_offscreenview.moc"
