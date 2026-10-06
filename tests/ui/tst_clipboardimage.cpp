#include "ui/ClipboardImage.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QSignalSpy>
#include <QtTest>

using namespace scvp::ui;

class TstClipboardImage : public QObject {
    Q_OBJECT

private slots:
    // The PNG on the clipboard is the image, losslessly, with its resolution; the image itself is
    // offered too (for the formats other than PNG).
    void copiesLosslessPng()
    {
        QImage img(321, 203, QImage::Format_RGB32);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                img.setPixel(x, y, qRgb(x & 255, y & 255, (x * y) & 255));
        img.setDotsPerMeterX(7559);  // 192 dpi
        img.setDotsPerMeterY(7559);
        ClipboardImage clip;
        QSignalSpy spy(&clip, &ClipboardImage::copied);
        clip.copy(img);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toSize(), img.size());
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        QVERIFY(data && data->hasImage());
        const QImage png = QImage::fromData(data->data(QStringLiteral("image/png")), "png");
        QCOMPARE(png.convertToFormat(QImage::Format_RGB32), img);
        QCOMPARE(png.dotsPerMeterX(), 7559);
    }

    // Copies in quick succession: only the newest reaches the clipboard.
    void newestCopyWins()
    {
        QImage red(640, 480, QImage::Format_RGB32);
        red.fill(qRgb(255, 0, 0));
        QImage blue(640, 480, QImage::Format_RGB32);
        blue.fill(qRgb(0, 0, 255));
        ClipboardImage clip;
        QSignalSpy spy(&clip, &ClipboardImage::copied);
        clip.copy(red);
        clip.copy(blue);
        QVERIFY(spy.wait(5000));
        QTest::qWait(300);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QGuiApplication::clipboard()->image().pixel(0, 0), qRgb(0, 0, 255));
    }
};

QTEST_MAIN(TstClipboardImage)
#include "tst_clipboardimage.moc"
