#include "ui/TimelineWidget.h"

#include <QtTest>

using namespace scvp;
using namespace scvp::ui;

namespace {

struct FakeModel : TimelineModel {
    int64_t len = 1000;
    int64_t pos = 0;
    bool loop = false;
    int64_t lf = -1, ll = -1;
    bool play = false;
    QStringList calls;

    int64_t length() const override { return len; }
    int64_t indexedFrames() const override { return len; }
    int64_t position() const override { return pos; }
    double frameRate() const override { return 25; }
    bool playing() const override { return play; }
    bool slotOpen(int slot) const override { return slot == 0; }
    RangeSetPtr cachedRanges(int) const override
    {
        auto r = std::make_shared<RangeSet>();
        r->ranges = {{100, 200}};
        return r;
    }
    int64_t slotOffset(int) const override { return 0; }
    bool hasLoop() const override { return loop; }
    bool loopActive() const override { return loop; }
    int64_t loopFirst() const override { return lf; }
    int64_t loopLast() const override { return ll; }
    void scrubBegin() override { calls << QStringLiteral("begin"); }
    void scrubTo(int64_t f) override
    {
        pos = f;
        calls << QStringLiteral("to %1").arg(f);
    }
    void scrubEnd(int64_t f) override { calls << QStringLiteral("end %1").arg(f); }
    void setLoop(int64_t a, int64_t b) override
    {
        loop = true;
        lf = a;
        ll = b;
        calls << QStringLiteral("loop %1 %2").arg(a).arg(b);
    }
};

} // namespace

class TstTimeline : public QObject {
    Q_OBJECT

private slots:
    void mapping()
    {
        FakeModel m;
        TimelineWidget w(&m);
        w.resize(1016, 30);  // 1000 px of track plus 8 px padding each side
        QCOMPARE(w.frameAt(8.0), int64_t(0));
        QCOMPARE(w.frameAt(8.0 + 500.5), int64_t(500));
        QCOMPARE(w.frameAt(-50), int64_t(0));
        QCOMPARE(w.frameAt(5000), int64_t(999));
        for (int64_t f : {0, 1, 250, 999})
            QCOMPARE(w.frameAt(w.xAt(f)), f);
    }

    void clickAndDragScrub()
    {
        FakeModel m;
        TimelineWidget w(&m);
        w.resize(1016, 30);
        w.show();
        QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 300, 15));
        QTest::mouseMove(&w, QPoint(8 + 400, 15));
        QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 400, 15));
        QCOMPARE(m.calls.first(), QStringLiteral("begin"));
        QVERIFY(m.calls.contains(QStringLiteral("to 300")));
        QVERIFY(m.calls.contains(QStringLiteral("to 400")));
        QCOMPARE(m.calls.last(), QStringLiteral("end 400"));
    }

    void shiftDragIsOneFramePerPixel()
    {
        FakeModel m;
        m.len = 100000;
        TimelineWidget w(&m);
        w.resize(1016, 30);
        w.show();
        QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 500, 15));
        const int64_t start = m.pos;
        QMouseEvent move(QEvent::MouseMove, QPointF(8 + 510, 15), w.mapToGlobal(QPointF(8 + 510, 15)), Qt::NoButton,
                         Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(&w, &move);
        QCOMPARE(m.pos, start + 10);
        QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 510, 15));
    }

    void loopEdgesDrag()
    {
        FakeModel m;
        m.loop = true;
        m.lf = 200;
        m.ll = 299;
        TimelineWidget w(&m);
        w.resize(1016, 30);
        w.show();
        // The start edge sits at the left boundary of frame 200.
        QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 200, 15));
        QTest::mouseMove(&w, QPoint(8 + 150, 15));
        QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 150, 15));
        QVERIFY2(m.calls.contains(QStringLiteral("loop 150 299")), qPrintable(m.calls.join(QLatin1Char('|'))));
        QVERIFY(!m.calls.contains(QStringLiteral("begin")));  // no scrubbing while moving an edge
        m.calls.clear();
        QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 300, 15));
        QTest::mouseMove(&w, QPoint(8 + 350, 15));
        QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 350, 15));
        QVERIFY2(m.calls.contains(QStringLiteral("loop 150 350")), qPrintable(m.calls.join(QLatin1Char('|'))));
    }
};

QTEST_MAIN(TstTimeline)
#include "tst_timeline.moc"
