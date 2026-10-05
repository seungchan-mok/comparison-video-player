#include "ui/LegendDialog.h"

#include <QLineEdit>
#include <QtTest>

using namespace scvp::ui;

class TstLegendDialog : public QObject {
    Q_OBJECT

private slots:
    // The fields start from the current names; an empty one shows the file name it stands for.
    void prefillsNamesAndFileNames()
    {
        LegendDialog d({LegendDialog::Entry{QStringLiteral("baseline"), QStringLiteral("a.mp4"), true},
                        LegendDialog::Entry{QString(), QStringLiteral("b.mp4"), true}});
        auto* a = d.findChild<QLineEdit*>(QStringLiteral("legendA"));
        auto* b = d.findChild<QLineEdit*>(QStringLiteral("legendB"));
        QVERIFY(a && b);
        QCOMPARE(a->text(), QStringLiteral("baseline"));
        QCOMPARE(a->placeholderText(), QStringLiteral("a.mp4"));
        QCOMPARE(b->text(), QString());
        QCOMPARE(b->placeholderText(), QStringLiteral("b.mp4"));
        QVERIFY(a->isEnabled() && b->isEnabled());
        QCOMPARE(d.texts(), (LegendDialog::Texts{QStringLiteral("baseline"), QString()}));
    }

    void trimsWhatWasTyped()
    {
        LegendDialog d({LegendDialog::Entry{QString(), QStringLiteral("a.mp4"), true},
                        LegendDialog::Entry{QStringLiteral("old"), QStringLiteral("b.mp4"), true}});
        d.findChild<QLineEdit*>(QStringLiteral("legendA"))->setText(QStringLiteral("  새 인코더 v2  "));
        d.findChild<QLineEdit*>(QStringLiteral("legendB"))->setText(QStringLiteral("   "));
        QCOMPARE(d.texts(), (LegendDialog::Texts{QStringLiteral("새 인코더 v2"), QString()}));
    }

    // Only an open video can be named; a closed slot's stale name is not offered.
    void closedSlotCannotBeNamed()
    {
        LegendDialog d({LegendDialog::Entry{QString(), QStringLiteral("a.mp4"), true},
                        LegendDialog::Entry{QStringLiteral("stale"), QString(), false}});
        auto* b = d.findChild<QLineEdit*>(QStringLiteral("legendB"));
        QVERIFY(!b->isEnabled());
        QCOMPARE(b->text(), QString());
        QVERIFY(d.findChild<QLineEdit*>(QStringLiteral("legendA"))->isEnabled());
    }
};

QTEST_MAIN(TstLegendDialog)
#include "tst_legenddialog.moc"
