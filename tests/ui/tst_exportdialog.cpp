#include "ui/ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp;
using namespace scvp::ui;

namespace {

ExportTimeline timeline(bool final = true)
{
    ExportTimeline t;
    t.length = 300;
    t.final = final;
    t.indexProgress = final ? 1.0 : 0.4;
    t.hasLoop = true;
    t.loopFirst = 10;
    t.loopLast = 20;
    t.fps = 30;
    return t;
}

// Two 640x480 videos side by side at the chosen size.
QSize sizeOf(const ExportOptions& o) { return QSize((2 * 640 + 2) * o.scale / 100, 480 * o.scale / 100); }

ExportOptions initial()
{
    ExportOptions o;
    o.path = QStringLiteral("/tmp/a_vs_b.mp4");
    return o;
}

} // namespace

class TstExportDialog : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("scvp-test"));
        QCoreApplication::setApplicationName(QStringLiteral("scvp-test"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_dir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    }

    // With a loop set, the loop is the range; the summary has frames, size, rate and length.
    void loopIsTheDefaultRange()
    {
        ExportDialog d(initial(), [] { return timeline(); }, sizeOf);
        const ExportOptions o = d.options();
        QCOMPARE(o.first, 10);
        QCOMPARE(o.last, 20);
        QVERIFY2(d.summary().contains(QLatin1String("11 frames")), qPrintable(d.summary()));
        QVERIFY(d.summary().contains(QLatin1String("1282 x 480")));
        QVERIFY(d.summary().contains(QLatin1String("30 fps")));
        QVERIFY(d.canExport());
        // Whole timeline / custom frames.
        auto* range = d.findChild<QComboBox*>(QStringLiteral("range"));
        range->setCurrentIndex(0);
        QCOMPARE(d.options().first, 0);
        QCOMPARE(d.options().last, 299);
        QVERIFY(!d.findChild<QSpinBox*>(QStringLiteral("first"))->isEnabled());
        range->setCurrentIndex(2);
        d.findChild<QSpinBox*>(QStringLiteral("first"))->setValue(50);
        d.findChild<QSpinBox*>(QStringLiteral("last"))->setValue(40);
        QVERIFY(!d.canExport());  // last before first
        d.findChild<QSpinBox*>(QStringLiteral("last"))->setValue(149);
        QVERIFY(d.summary().contains(QLatin1String("100 frames")));
        QVERIFY(d.canExport());
    }

    // The format sets the file's extension and whether quality applies; the size follows.
    void formatAndSize()
    {
        ExportDialog d(initial(), [] { return timeline(); }, sizeOf);
        auto* format = d.findChild<QComboBox*>(QStringLiteral("format"));
        format->setCurrentIndex(format->findData(int(OutputFormat::Ffv1)));
        QCOMPARE(d.options().path, QStringLiteral("/tmp/a_vs_b.mkv"));
        QVERIFY(!d.findChild<QComboBox*>(QStringLiteral("quality"))->isEnabled());
        format->setCurrentIndex(format->findData(int(OutputFormat::PngSequence)));
        QCOMPARE(d.options().path, QStringLiteral("/tmp/a_vs_b.png"));
        format->setCurrentIndex(format->findData(int(OutputFormat::Hevc)));
        QCOMPARE(d.options().path, QStringLiteral("/tmp/a_vs_b.mp4"));
        QVERIFY(d.findChild<QComboBox*>(QStringLiteral("quality"))->isEnabled());
        auto* scale = d.findChild<QComboBox*>(QStringLiteral("scale"));
        scale->setCurrentIndex(scale->findData(50));
        QCOMPARE(d.options().scale, 50);
        QVERIFY(d.summary().contains(QLatin1String("641 x 240")));
        d.findChild<QLineEdit*>(QStringLiteral("path"))->setText(QString());
        QVERIFY(!d.canExport());
    }

    // Too large for the GPU: not offered.
    void tooLargeIsRefused()
    {
        ExportDialog d(initial(), [] { return timeline(); }, [](const ExportOptions&) { return QSize(20000, 100); });
        QVERIFY(!d.canExport());
        QVERIFY(d.summary().contains(QLatin1String("too large")));
    }

    // While indexing, Export waits; it becomes available when the index is complete.
    void waitsForTheIndex()
    {
        bool final = false;
        ExportDialog d(initial(), [&final] { return timeline(final); }, sizeOf);
        QVERIFY(!d.canExport());
        QVERIFY2(d.summary().contains(QLatin1String("Indexing")), qPrintable(d.summary()));
        final = true;
        QTRY_VERIFY(d.canExport());
        QCOMPARE(d.options().first, 10);  // the loop, now that the frame numbers are final
    }

    // The encoders fill in (after probing) with Automatic first.
    void encodersFillIn()
    {
        ExportDialog d(initial(), [] { return timeline(); }, sizeOf);
        auto* encoder = d.findChild<QComboBox*>(QStringLiteral("encoder"));
        QTRY_VERIFY(!encoder->itemText(0).contains(QLatin1String("checking")));
        QCOMPARE(encoder->itemData(0).toString(), QString());
        QCOMPARE(encoder->count(), int(VideoEncoder::available(OutputFormat::H264).size()) + 1);
        QCOMPARE(d.options().encoder, QString());
    }

    void optionsAreRemembered()
    {
        ExportOptions o;
        o.format = OutputFormat::Ffv1;
        o.encoder = QStringLiteral("ffv1");
        o.quality = OutputQuality::Small;
        o.picture = ExportPicture::Screen;
        o.scale = 50;
        o.labels = false;
        o.frameInfo = true;
        // A real absolute path: "/tmp/..." gains the current drive on Windows.
        o.path = m_dir.filePath(QStringLiteral("exports/x.mkv"));
        o.save();
        const ExportOptions l = ExportOptions::load();
        QCOMPARE(int(l.format), int(OutputFormat::Ffv1));
        QCOMPARE(l.encoder, QStringLiteral("ffv1"));
        QCOMPARE(int(l.quality), int(OutputQuality::Small));
        QCOMPARE(int(l.picture), int(ExportPicture::Screen));
        QCOMPARE(l.scale, 50);
        QVERIFY(!l.labels);
        QVERIFY(l.frameInfo);
        QCOMPARE(l.lastDir(), m_dir.filePath(QStringLiteral("exports")));
    }
};

QTEST_MAIN(TstExportDialog)
#include "tst_exportdialog.moc"
