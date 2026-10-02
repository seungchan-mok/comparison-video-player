#include "ui/OpenRawDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryFile>
#include <QtTest>

using namespace scvp;
using namespace scvp::ui;

class TstOpenRawDialog : public QObject {
    Q_OBJECT

    // A file of `bytes` zero bytes.
    static QString makeFile(QTemporaryFile& f, qint64 bytes)
    {
        f.open();
        f.write(QByteArray(int(bytes), '\0'));
        f.flush();
        return f.fileName();
    }

private slots:
    void prefillsAndCountsFrames()
    {
        QTemporaryFile file;
        const QString path = makeFile(file, 320 * 240 * 3 / 2 * 5);  // five yuv420p frames
        RawVideoParams init;
        init.width = 320;
        init.height = 240;
        init.format = AV_PIX_FMT_YUV420P;
        init.frameRate = 25;
        OpenRawDialog d(path, init, nullptr);
        const RawVideoParams p = d.params();
        QCOMPARE(p.width, 320);
        QCOMPARE(p.height, 240);
        QCOMPARE(int(p.format), int(AV_PIX_FMT_YUV420P));
        QCOMPARE(p.frameRate, 25.0);
        QVERIFY2(d.summary().contains(QLatin1String("115200 bytes per frame: 5 frames")), qPrintable(d.summary()));
        QVERIFY(!d.summary().contains(QLatin1String("left over")));
        QVERIFY(d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
    }

    void warnsAboutPartialFramesAndRejectsBadInput()
    {
        QTemporaryFile file;
        const QString path = makeFile(file, 320 * 240 * 2 + 10);  // one nv16-sized frame + 10
        OpenRawDialog d(path, RawVideoParams{}, nullptr);
        auto spins = d.findChildren<QSpinBox*>();
        QVERIFY(spins.size() >= 3);
        spins[0]->setValue(320);
        spins[1]->setValue(240);
        auto* fmt = d.findChild<QComboBox*>();
        fmt->setCurrentText(QStringLiteral("uyvy422"));
        QVERIFY2(d.summary().contains(QLatin1String("1 frames")) && d.summary().contains(QLatin1String("10 bytes left over")),
                 qPrintable(d.summary()));
        auto* ok = d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QVERIFY(ok->isEnabled());
        fmt->setCurrentText(QStringLiteral("not_a_format"));
        QVERIFY(!ok->isEnabled());
        fmt->setCurrentText(QStringLiteral("rgb48le"));  // 460800 bytes: bigger than the file
        QVERIFY(!ok->isEnabled());
    }

    void defaultsWithoutHints()
    {
        QTemporaryFile file;
        const QString path = makeFile(file, 100);
        OpenRawDialog d(path, RawVideoParams{}, nullptr);
        QCOMPARE(d.params().width, 1920);
        QCOMPARE(d.params().height, 1080);
        QCOMPARE(int(d.params().format), int(AV_PIX_FMT_YUV420P));
        QCOMPARE(d.params().frameRate, 30.0);
    }
};

QTEST_MAIN(TstOpenRawDialog)
#include "tst_openrawdialog.moc"
