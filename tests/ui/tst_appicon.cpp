#include "ui/AppIcon.h"

#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp::ui;

class TstAppIcon : public QObject {
    Q_OBJECT

private slots:
    // The Windows icon is read by the shell, not by Qt: check the ICO structure byte by byte and
    // that every entry is a PNG of the size its entry announces.
    void icoHoldsEverySize()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("scvp.ico"));
        QVERIFY(exportAppIco(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray ico = f.readAll();
        QCOMPARE(ico, appIconIco());

        QDataStream in(ico);
        in.setByteOrder(QDataStream::LittleEndian);
        quint16 reserved = 1, type = 0, count = 0;
        in >> reserved >> type >> count;
        QCOMPARE(reserved, quint16(0));
        QCOMPARE(type, quint16(1));  // icon, not cursor
        QCOMPARE(count, quint16(7));

        QList<int> sides;
        quint32 next = 6 + 16 * count;
        for (int i = 0; i < count; ++i) {
            quint8 w = 0, h = 0, colors = 1, zero = 1;
            quint16 planes = 0, bits = 0;
            quint32 bytes = 0, offset = 0;
            in >> w >> h >> colors >> zero >> planes >> bits >> bytes >> offset;
            QCOMPARE(w, h);
            QCOMPARE(colors, quint8(0));
            QCOMPARE(zero, quint8(0));
            QCOMPARE(planes, quint16(1));
            QCOMPARE(bits, quint16(32));
            QCOMPARE(offset, next);  // pictures follow each other, in entry order
            next += bytes;
            QVERIFY(qint64(offset) + bytes <= ico.size());

            const QByteArray png = ico.mid(offset, bytes);
            QVERIFY(png.startsWith("\x89PNG"));
            const QImage img = QImage::fromData(png, "PNG");
            const int side = w == 0 ? 256 : w;
            QCOMPARE(img.size(), QSize(side, side));
            QVERIFY(img.hasAlphaChannel());
            sides.append(side);
        }
        QCOMPARE(qint64(next), ico.size());
        QCOMPARE(sides, (QList<int>{16, 24, 32, 48, 64, 128, 256}));
    }

    void unwritablePathFails() { QVERIFY(!exportAppIco(QStringLiteral("/nonexistent-dir/scvp.ico"))); }
};

QTEST_GUILESS_MAIN(TstAppIcon)
#include "tst_appicon.moc"
