#include "ui/ControlBar.h"
#include "ui/Player.h"
#include "ui/Stage.h"

#include <QtTest>

using namespace scvp::ui;

class TstStage : public QObject {
    Q_OBJECT

private slots:
    void windowedAndFullscreenLayouts()
    {
        QWidget window;
        auto* video = new QWidget;  // stands in for the GL view
        Player player(nullptr);
        auto* bar = new ControlBar(&player);
        auto* stage = new Stage(video, bar, &window);
        stage->setIdleInterval(50);
        window.resize(800, 600);
        stage->resize(800, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const int barH = bar->sizeHint().height();
        QCOMPARE(video->geometry(), QRect(0, 0, 800, 600 - barH));
        QCOMPARE(bar->geometry(), QRect(0, 600 - barH, 800, barH));

        stage->setFullscreenMode(true);
        QCOMPARE(video->geometry(), QRect(0, 0, 800, 600));  // the video uses everything
        QCOMPARE(bar->geometry(), QRect(0, 600 - barH, 800, barH));
        QVERIFY(bar->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 2000);  // auto-hides when idle
        QCOMPARE(video->cursor().shape(), Qt::BlankCursor);

        QMouseEvent move(QEvent::MouseMove, QPointF(10, 10), video->mapToGlobal(QPointF(10, 10)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(video, &move);
        QVERIFY(bar->isVisible());
        QVERIFY(video->cursor().shape() != Qt::BlankCursor);

        stage->setFullscreenMode(false);
        QVERIFY(bar->isVisible());
        QCOMPARE(video->geometry(), QRect(0, 0, 800, 600 - barH));
    }
};

QTEST_MAIN(TstStage)
#include "tst_stage.moc"
