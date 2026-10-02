#include "ui/KeyMap.h"
#include "ui/KeyRouter.h"

#include <QAction>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

#include <set>

using namespace scvp::ui;

Q_DECLARE_METATYPE(scvp::ui::Action)

class TstKeyRouter : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<scvp::ui::Action>(); }

    void symbolKeysIgnoreShift()
    {
        const KeyMap m = KeyMap::defaults();
        QCOMPARE(m.match(Qt::Key_Comma, Qt::NoModifier), std::optional<Action>(Action::StepBack));
        QCOMPARE(m.match(Qt::Key_Less, Qt::ShiftModifier), std::optional<Action>(Action::OffsetMinus));
        QCOMPARE(m.match(Qt::Key_Greater, Qt::ShiftModifier), std::optional<Action>(Action::OffsetPlus));
        QCOMPARE(m.match(Qt::Key_L, Qt::NoModifier), std::optional<Action>(Action::Legend));
        QCOMPARE(m.match(Qt::Key_L, Qt::ControlModifier), std::optional<Action>(Action::LoopToggle));
        QCOMPARE(m.match(Qt::Key_L, Qt::ShiftModifier), std::optional<Action>(Action::LoopClear));
        QCOMPARE(m.match(Qt::Key_Left, Qt::KeypadModifier), std::optional<Action>(Action::JumpBack));
        QCOMPARE(m.match(Qt::Key_Plus, Qt::ShiftModifier), std::optional<Action>(Action::ZoomIn));
        QVERIFY(!m.match(Qt::Key_Q, Qt::NoModifier));
        QVERIFY(!m.match(Qt::Key_Left, Qt::AltModifier));
    }

    void defaultsHaveNoConflicts()
    {
        const KeyMap m = KeyMap::defaults();
        std::set<int> seen;
        for (int a = 0; a < int(Action::Count); ++a)
            for (const QKeyCombination& k : m.keys(Action(a))) {
                QVERIFY2(seen.insert(k.toCombined()).second, KeyMap::info(Action(a)).id);
                QCOMPARE(m.match(k.key(), k.keyboardModifiers()), std::optional<Action>(Action(a)));
            }
        for (int a = 0; a < int(Action::Count); ++a)
            QVERIFY2(!m.keys(Action(a)).isEmpty(), KeyMap::info(Action(a)).id);
    }

    void settingsRoundTrip()
    {
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("k.ini")), QSettings::IniFormat);
        KeyMap m = KeyMap::defaults();
        m.setKeys(Action::StepForward, {QKeyCombination(Qt::NoModifier, Qt::Key_D)});
        m.setKeys(Action::ViewDifference, {QKeyCombination(Qt::ControlModifier, Qt::Key_D)});
        m.save(s);
        KeyMap back = KeyMap::defaults();
        back.load(s);
        QCOMPARE(back.keys(Action::StepForward), m.keys(Action::StepForward));
        QCOMPARE(back.match(Qt::Key_D, Qt::ControlModifier), std::optional<Action>(Action::ViewDifference));
        QCOMPARE(back.keys(Action::PlayPause), m.keys(Action::PlayPause));
    }

    // Settings saved before L became the legend still hold loop_toggle=L: that old default moves
    // to the new one, while a binding the user chose stays.
    void outdatedDefaultsMove()
    {
        QTemporaryDir dir;
        {
            QSettings s(dir.filePath(QStringLiteral("old.ini")), QSettings::IniFormat);
            s.setValue(QStringLiteral("keys/loop_toggle"), QStringLiteral("L"));
            s.setValue(QStringLiteral("keys/zoom_fit"), QStringLiteral("Z"));
            KeyMap m = KeyMap::defaults();
            m.load(s);
            QCOMPARE(m.match(Qt::Key_L, Qt::NoModifier), std::optional<Action>(Action::Legend));
            QCOMPARE(m.match(Qt::Key_L, Qt::ControlModifier), std::optional<Action>(Action::LoopToggle));
            QCOMPARE(m.match(Qt::Key_Z, Qt::NoModifier), std::optional<Action>(Action::ZoomFit));
        }
        {
            // Saved by this version: loop_toggle=L is the user's own choice now.
            QSettings s(dir.filePath(QStringLiteral("new.ini")), QSettings::IniFormat);
            KeyMap saved = KeyMap::defaults();
            saved.setKeys(Action::Legend, {});
            saved.setKeys(Action::LoopToggle, {QKeyCombination(Qt::NoModifier, Qt::Key_L)});
            saved.save(s);
            KeyMap m = KeyMap::defaults();
            m.load(s);
            QCOMPARE(m.match(Qt::Key_L, Qt::NoModifier), std::optional<Action>(Action::LoopToggle));
        }
    }

    void routesKeysAndGatesRepeats()
    {
        QWidget window;
        auto* view = new QWidget(&window);
        view->setFocusPolicy(Qt::StrongFocus);
        auto* spin = new QSpinBox(&window);
        spin->setRange(0, 100);
        auto* layout = new QVBoxLayout(&window);
        layout->addWidget(view);
        layout->addWidget(spin);
        // A QAction with the same shortcut must not fire: the router claims the key.
        auto* act = new QAction(&window);
        act->setShortcut(Qt::Key_Space);
        window.addAction(act);
        QSignalSpy actionSpy(act, &QAction::triggered);

        bool allowRepeat = false;
        KeyRouter router(&window, view, KeyMap::defaults(), [&] { return allowRepeat; });
        QSignalSpy spy(&router, &KeyRouter::triggered);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        view->setFocus();
        QTRY_VERIFY(view->hasFocus());

        QTest::keyClick(view, Qt::Key_Period);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<Action>(), Action::StepForward);

        QTest::keyClick(view, Qt::Key_Space);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<Action>(), Action::PlayPause);
        QCOMPARE(actionSpy.count(), 0);

        // Gated auto-repeat: dropped until the previous step is shown.
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Period, Qt::NoModifier, QStringLiteral("."), true);
        QApplication::sendEvent(view, &repeat);
        QCOMPARE(spy.count(), 0);
        allowRepeat = true;
        QKeyEvent repeat2(QEvent::KeyPress, Qt::Key_Period, Qt::NoModifier, QStringLiteral("."), true);
        QApplication::sendEvent(view, &repeat2);
        QCOMPARE(spy.count(), 1);
        spy.clear();
        // Toggles never repeat.
        QKeyEvent repeatSpace(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "), true);
        QApplication::sendEvent(view, &repeatSpace);
        QCOMPARE(spy.count(), 0);

        // Text input keeps its editing keys; Escape returns focus to the view.
        spin->setFocus();
        QTRY_VERIFY(spin->hasFocus());
        QTest::keyClick(spin, Qt::Key_Up);
        QCOMPARE(spin->value(), 1);
        QTest::keyClick(spin, Qt::Key_Period);
        QCOMPARE(spy.count(), 0);
        QTest::keyClick(spin, Qt::Key_Escape);
        QTRY_VERIFY(view->hasFocus());
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(TstKeyRouter)
#include "tst_keyrouter.moc"
