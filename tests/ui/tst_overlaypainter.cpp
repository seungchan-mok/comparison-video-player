#include "ui/OverlayPainter.h"

#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QtTest>

using namespace scvp;
using namespace scvp::ui;

namespace {

// Two side-by-side panes of a 2 * 300 + 2 by 200 picture, drawn at `dpr` device px per logical px.
OverlayScene splitScene(double dpr)
{
    OverlayScene s;
    s.compare.mode = render::CompareMode::Split;
    s.compare.orientation = render::SplitOrientation::SideBySide;
    s.layout = render::CompareLayout::compute(s.compare, QSize(int(602 * dpr), int(200 * dpr)), QSizeF(300, 200),
                                              true, true, int(2 * dpr));
    s.dpr = dpr;
    s.open = {true, true};
    s.names = {QStringLiteral("left.mp4"), QStringLiteral("right.mp4")};
    s.font = QFont();
    return s;
}

QImage paint(const OverlayScene& s, bool legend, double dpr)
{
    QImage img(int(602 * dpr), int(200 * dpr), QImage::Format_RGB32);
    img.fill(qRgb(128, 128, 128));
    img.setDevicePixelRatio(dpr);
    QPainter p(&img);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (legend)
        overlay::paintLegend(p, s, QRect());
    else
        overlay::paintPaneLabels(p, s, QFontMetrics(mono), false);
    p.end();
    img.setDevicePixelRatio(1);
    return img;
}

int count(const QImage& img, QRect r, QRgb colour)
{
    int n = 0;
    r &= img.rect();
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            n += (img.pixel(x, y) & 0xffffff) == (colour & 0xffffff);
    return n;
}

} // namespace

class TstOverlayPainter : public QObject {
    Q_OBJECT

private slots:
    // Each pane's legend entry starts at its top left with that video's chip colour.
    void legendChipsInTheirPanes_data()
    {
        QTest::addColumn<double>("dpr");
        QTest::newRow("1x") << 1.0;
        QTest::newRow("2.5x") << 2.5;
    }
    void legendChipsInTheirPanes()
    {
        QFETCH(double, dpr);
        const OverlayScene s = splitScene(dpr);
        const QImage img = paint(s, true, dpr);
        const QRect left(0, 0, int(80 * dpr), int(60 * dpr));
        const QRect right(int(302 * dpr), 0, int(80 * dpr), int(60 * dpr));
        const QRgb a = overlay::slotColor(0).rgb(), b = overlay::slotColor(1).rgb();
        QVERIFY(count(img, left, a) > 20 * dpr * dpr);
        QCOMPARE(count(img, left, b), 0);
        QVERIFY(count(img, right, b) > 20 * dpr * dpr);
        QCOMPARE(count(img, right, a), 0);
    }

    // A video holding its end frame says so in amber.
    void holdIsMarked()
    {
        OverlayScene s = splitScene(1.0);
        const QImage plain = paint(s, true, 1.0);
        s.hold = {false, true};
        const QImage hold = paint(s, true, 1.0);
        const QRgb amber = qRgb(255, 200, 90);
        QCOMPARE(count(plain, plain.rect(), amber), 0);
        QCOMPARE(count(hold, QRect(0, 0, 301, 200), amber), 0);
        QVERIFY(count(hold, QRect(302, 0, 300, 200), amber) > 10);
    }

    // Labels sit at the top right of each pane; one video alone gets none.
    void labelsAtTheTopRight()
    {
        OverlayScene s = splitScene(1.0);
        const QImage img = paint(s, false, 1.0);
        auto darkened = [&](QRect r) {
            int n = 0;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    n += qRed(img.pixel(x, y)) < 100;
            return n;
        };
        QVERIFY(darkened(QRect(260, 6, 36, 14)) > 100);  // pane A, top right
        QVERIFY(darkened(QRect(562, 6, 36, 14)) > 100);  // pane B, top right
        QCOMPARE(darkened(QRect(0, 40, 600, 160)), 0);
        s.open = {true, false};
        const QImage one = paint(s, false, 1.0);
        QCOMPARE(count(one, one.rect(), qRgb(128, 128, 128)), one.width() * one.height());
    }

    void frameInfoAtTheBottomLeft()
    {
        QImage img(400, 200, QImage::Format_RGB32);
        img.fill(qRgb(128, 128, 128));
        {
            QPainter p(&img);
            overlay::paintFrameInfo(p, img.size(), QStringLiteral("#12  A 12  B 9  00:00:00.400"),
                                    QFontDatabase::systemFont(QFontDatabase::FixedFont));
        }
        QVERIFY(qRed(img.pixel(12, 190)) < 100);
        QCOMPARE(img.pixel(12, 10), qRgb(128, 128, 128));
        QCOMPARE(img.pixel(390, 190), qRgb(128, 128, 128));
    }
};

QTEST_MAIN(TstOverlayPainter)
#include "tst_overlaypainter.moc"
