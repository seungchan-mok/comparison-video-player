#include "engine/FrameAllocator.h"
#include "render/Renderer.h"
#include "render/ViewTransform.h"

#include <QtTest>

#include <cmath>

using namespace scvp;
using namespace scvp::render;

class TstViewTransform : public QObject {
    Q_OBJECT

private slots:
    void fitFillsTheViewport()
    {
        ViewTransform v;
        v.setViewportSize({1000, 800});
        v.setImageSize({320, 240});
        QVERIFY(v.isFit());
        QCOMPARE(v.scale(), 1000.0 / 320.0);  // exact, not rounded down to 3x
        QCOMPARE(v.translation(), QPointF(0, 25));
        // The next zoom step lands on the integer ladder again.
        v.zoomSteps(1, {500, 400});
        QCOMPARE(v.scale(), 4.0);
        v.fit();
        v.zoomSteps(-1, {500, 400});
        QCOMPARE(v.scale(), 3.0);
        v.fit();
        v.setViewportSize({1280, 720});
        v.setImageSize({1920, 1080});
        QCOMPARE(v.scale(), 1280.0 / 1920.0);
        QCOMPARE(v.translation(), QPointF(0, 0));
    }

    void ladder()
    {
        QCOMPARE(ViewTransform::snapUp(1.0), 2.0);
        QCOMPARE(ViewTransform::snapUp(2.0), 3.0);
        QCOMPARE(ViewTransform::snapUp(6.0), 8.0);
        QCOMPARE(ViewTransform::snapUp(255.0), 256.0);
        QCOMPARE(ViewTransform::snapUp(256.0), 256.0);
        QCOMPARE(ViewTransform::snapDown(1.0), std::pow(2.0, -0.25));
        QCOMPARE(ViewTransform::snapDown(2.5), 2.0);
        QCOMPARE(ViewTransform::snapUp(0.37), std::pow(2.0, -1.25));
        QCOMPARE(ViewTransform::snapDown(0.37), std::pow(2.0, -1.5));
        QCOMPARE(ViewTransform::snapDown(1.0 / 32.0), 1.0 / 32.0);
    }

    void zoomKeepsAnchor()
    {
        ViewTransform v;
        v.setViewportSize({1280, 720});
        v.setImageSize({1920, 1080});
        const QPointF anchor(500, 300);
        const QPointF before = v.deviceToImage(anchor);
        v.zoomSteps(3, anchor);
        QVERIFY(!v.isFit());
        QCOMPARE(v.scale(), 1.0);  // 0.667 -> 0.707 -> 0.841 -> 1
        const QPointF after = v.deviceToImage(anchor);
        QVERIFY(std::abs(after.x() - before.x()) < 1.0);
        QVERIFY(std::abs(after.y() - before.y()) < 1.0);
        v.zoomTo(8, anchor);
        const QPointF zoomed = v.deviceToImage(anchor);
        QVERIFY(std::abs(zoomed.x() - before.x()) < 1.0);
        QVERIFY(std::abs(zoomed.y() - before.y()) < 1.0);
    }

    void oneToOneAndBack()
    {
        ViewTransform v;
        v.setViewportSize({800, 600});
        v.setImageSize({3840, 2160});
        v.oneToOne({400, 300});
        QCOMPARE(v.scale(), 1.0);
        v.fit();
        QVERIFY(v.isFit());
        QCOMPARE(v.scale(), 800.0 / 3840.0);
    }

    void panIsClamped()
    {
        ViewTransform v;
        v.setViewportSize({800, 600});
        v.setImageSize({400, 300});
        v.zoomTo(2, {400, 300});
        v.panBy({100000, 100000});
        QVERIFY(v.translation().x() <= 800 - 64);
        QVERIFY(v.translation().y() <= 600 - 64);
        v.panBy({-1e6, -1e6});
        QVERIFY(v.translation().x() + 2 * 400 >= 64);
        QVERIFY(v.translation().y() + 2 * 300 >= 64);
    }

    void resizeKeepsCentreInManualMode()
    {
        ViewTransform v;
        v.setViewportSize({800, 600});
        v.setImageSize({1920, 1080});
        v.zoomTo(4, {123, 456});
        const QPointF centre = v.deviceToImage({399.5, 299.5});
        v.setViewportSize({1600, 1000});
        const QPointF after = v.deviceToImage({799.5, 499.5});
        QVERIFY(std::abs(after.x() - centre.x()) <= 0.5);
        QVERIFY(std::abs(after.y() - centre.y()) <= 0.5);
        QCOMPARE(v.scale(), 4.0);
    }

    void fitModeRefitsOnResize()
    {
        ViewTransform v;
        v.setViewportSize({640, 480});
        v.setImageSize({320, 240});
        QCOMPARE(v.scale(), 2.0);
        v.setViewportSize({1000, 1000});
        QCOMPARE(v.scale(), 1000.0 / 320.0);
        QCOMPARE(v.translation(), QPointF(0, 125));
    }

    void devicePixelRatioChange()
    {
        ViewTransform v;
        v.setViewportSize({800, 600});
        v.setImageSize({1920, 1080});
        v.zoomTo(2, {100, 100});
        const QPointF t = v.translation();
        v.scaleDevice(2.0);
        QCOMPARE(v.scale(), 4.0);
        QCOMPARE(v.viewportSize(), QSizeF(1600, 1200));
        QCOMPARE(v.translation(), t * 2.0);
    }

    void hitTestingRoundTrips()
    {
        ViewTransform v;
        v.setViewportSize({1000, 800});
        v.setImageSize({320, 240});
        v.zoomTo(5, {333, 222});
        for (int x : {0, 17, 100, 319})
            for (int y : {0, 9, 239}) {
                const QPointF d = v.imageToDevice(QPointF(x + 0.5, y + 0.5));
                const QPointF back = v.deviceToImage(QPointF(std::floor(d.x()), std::floor(d.y())));
                QCOMPARE(int(std::floor(back.x())), x);
                QCOMPARE(int(std::floor(back.y())), y);
            }
    }

    // A capture's transform: the same view, exactly `factor` times larger. A fitted 0.36x
    // becomes exactly 1:1 (not 0.9999...), so the capture samples exact pixels.
    void scaledForCaptures()
    {
        ViewTransform v;
        v.setViewportSize({700, 500});
        v.setImageSize({1920, 1080});
        const ViewTransform one = v.scaled(1.0 / v.scale());
        QVERIFY(one.scale() == 1.0);
        QCOMPARE(one.translation(), v.renderTranslation() / v.scale());
        QCOMPARE(one.viewportSize(), v.viewportSize() / v.scale());
        QVERIFY(!one.isFit());
        // Manual zoom and pan scale as drawn: no refit, no clamping.
        v.zoomTo(2.0, {100, 100});
        v.panBy({13.5, -7.25});
        QVERIFY(v.translation() != v.renderTranslation());
        const ViewTransform big = v.scaled(2.5);
        QVERIFY(big.scale() == 5.0);
        QCOMPARE(big.translation(), v.renderTranslation() * 2.5);
        QCOMPARE(big.imageSize(), v.imageSize());
    }

    // Captures render minified videos at 1:1, magnified ones at the screen's size, within limits.
    void captureFactorShowsEveryPixel()
    {
        FramePool pool;
        auto frame = [&](int w, int h) {
            auto f = std::make_shared<Frame>();
            f->av = pool.allocFrame(AV_PIX_FMT_RGB24, w, h);
            return f;
        };
        RenderState s;
        s.frames[0] = frame(64, 48);
        s.framebufferSize = QSize(200, 150);
        s.view.setViewportSize({200, 150});
        s.view.setImageSize({64, 48});
        QCOMPARE(Renderer::captureFactor(s, 16384, 40'000'000), 1.0);  // magnified (fit 3.125)
        s.view.zoomTo(0.25, {0, 0});
        QCOMPARE(Renderer::captureFactor(s, 16384, 40'000'000), 4.0);
        QCOMPARE(Renderer::captureFactor(s, 400, 40'000'000), 2.0);  // 200 px x 2 = the side limit
        QCOMPARE(Renderer::captureFactor(s, 16384, 200 * 150 * 9), 3.0);
        QCOMPARE(Renderer::captureFactor(s, 100, 40'000'000), 1.0);  // never below the screen
        // A larger B counts while it is on screen.
        s.view.zoomTo(1.0, {0, 0});
        s.frames[1] = frame(128, 96);
        QCOMPARE(Renderer::captureFactor(s, 16384, 40'000'000), 2.0);
        s.compare.mode = CompareMode::Flip;
        QCOMPARE(Renderer::captureFactor(s, 16384, 40'000'000), 1.0);
        s.compare.flipShowsB = true;
        QCOMPARE(Renderer::captureFactor(s, 16384, 40'000'000), 2.0);
    }
};

QTEST_APPLESS_MAIN(TstViewTransform)
#include "tst_viewtransform.moc"
