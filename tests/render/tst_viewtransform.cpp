#include "render/ViewTransform.h"

#include <QtTest>

#include <cmath>

using namespace scvp::render;

class TstViewTransform : public QObject {
    Q_OBJECT

private slots:
    void fitUsesWholeMultiples()
    {
        ViewTransform v;
        v.setViewportSize({1000, 800});
        v.setImageSize({320, 240});
        QVERIFY(v.isFit());
        QCOMPARE(v.scale(), 3.0);
        QCOMPARE(v.translation(), QPointF(20, 40));
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
        QCOMPARE(v.scale(), 3.0);
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
};

QTEST_APPLESS_MAIN(TstViewTransform)
#include "tst_viewtransform.moc"
