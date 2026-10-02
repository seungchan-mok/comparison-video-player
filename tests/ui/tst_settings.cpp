#include "ui/Settings.h"
#include "ui/SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp::ui;

class TstSettings : public QObject {
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

    void roundTrip()
    {
        AppSettings a;
        a.cacheMb = 4096;
        a.jumps.small = 2.5;
        a.jumps.large = 120;
        a.jumps.inFrames = true;
        a.keys.setKeys(Action::StepForward, {QKeyCombination(Qt::NoModifier, Qt::Key_K)});
        a.lastDir = QStringLiteral("/tmp/videos");
        a.save();
        const AppSettings b = AppSettings::load();
        QCOMPARE(b.cacheMb, size_t(4096));
        QCOMPARE(b.cacheBytes(), size_t(4096) << 20);
        QCOMPARE(b.jumps.small, 2.5);
        QCOMPARE(b.jumps.large, 120.0);
        QVERIFY(b.jumps.inFrames);
        QCOMPARE(b.keys.match(Qt::Key_K, Qt::NoModifier), std::optional<Action>(Action::StepForward));
        QCOMPARE(b.lastDir, QStringLiteral("/tmp/videos"));
    }

    void dialogEditsAndReportsBudget()
    {
        AppSettings a;
        a.cacheMb = 1024;
        // 25 MB frames with 250-frame GOPs: 1024 MB is ~40 frames, well under two GOPs.
        SettingsDialog dlg(a, [] { return std::pair<size_t, int>{size_t(25) << 20, 250}; });
        auto* autoBox = dlg.findChild<QCheckBox*>();
        auto* mb = dlg.findChild<QSpinBox*>();
        QVERIFY(autoBox && mb);
        QVERIFY(!autoBox->isChecked());
        QCOMPARE(mb->value(), 1024);
        bool warned = false;
        for (QLabel* l : dlg.findChildren<QLabel*>())
            warned |= l->text().contains(QLatin1String("Below 2 GOPs"));
        QVERIFY(warned);
        mb->setValue(20480);
        const auto spins = dlg.findChildren<QDoubleSpinBox*>();
        QCOMPARE(spins.size(), 2);
        spins[0]->setValue(1.5);
        auto* table = dlg.findChild<QTableWidget*>();
        auto* edit = static_cast<QKeySequenceEdit*>(table->cellWidget(int(Action::PlayPause), 1));
        edit->setKeySequence(QKeySequence(Qt::Key_K));
        const AppSettings r = dlg.result();
        QCOMPARE(r.cacheMb, size_t(20480));
        QCOMPARE(r.jumps.small, 1.5);
        QCOMPARE(r.keys.match(Qt::Key_K, Qt::NoModifier), std::optional<Action>(Action::PlayPause));
        autoBox->setChecked(true);
        QCOMPARE(dlg.result().cacheMb, size_t(0));
    }
};

QTEST_MAIN(TstSettings)
#include "tst_settings.moc"
