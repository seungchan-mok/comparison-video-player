#include "engine/TimelineMapper.h"

#include <QtTest>

using namespace scvp;

namespace {

TimelineMapper make(int64_t na, int64_t nb, int64_t offset, double fa = 30, double fb = 30,
                    SyncMode mode = SyncMode::Frame)
{
    TimelineMapper m;
    m.setSlot(0, {na > 0, na, fa, nullptr});
    m.setSlot(1, {nb > 0, nb, fb, nullptr});
    m.setOffset(offset);
    m.setMode(mode);
    return m;
}

} // namespace

class TstTimelineMapper : public QObject {
    Q_OBJECT

private slots:
    void singleVideoIsIdentity()
    {
        const TimelineMapper m = make(100, 0, 7);
        QCOMPARE(m.length(), int64_t(100));
        QCOMPARE(m.master(), 0);
        QCOMPARE(m.local(0, 42).frame, 42);
        QVERIFY(!m.local(0, 42).hold);
        QCOMPARE(m.local(1, 42).frame, -1);
        const TimelineMapper onlyB = make(0, 50, 7);
        QCOMPARE(onlyB.master(), 1);
        QCOMPARE(onlyB.length(), int64_t(50));
        QCOMPARE(onlyB.local(1, 10).frame, 10);  // the offset only matters between two videos
    }

    void positiveOffsetStartsWithB()
    {
        // B frame a+3 matches A frame a: B's frames 0-2 come before A starts (A holds frame 0).
        const TimelineMapper m = make(100, 97, 3);
        QCOMPARE(m.length(), int64_t(103));
        QCOMPARE(m.local(1, 0).frame, 0);
        QVERIFY(!m.local(1, 0).hold);
        QCOMPARE(m.local(0, 0).frame, 0);
        QVERIFY(m.local(0, 0).hold);
        QCOMPARE(m.local(0, 3).frame, 0);
        QVERIFY(!m.local(0, 3).hold);
        QCOMPARE(m.local(1, 3).frame, 3);
        QCOMPARE(m.local(1, 96).frame, 96);
        QVERIFY(!m.local(1, 96).hold);
        QCOMPARE(m.local(1, 97).frame, 96);  // past B's end: its last frame is held
        QVERIFY(m.local(1, 97).hold);
        QCOMPARE(m.local(0, 102).frame, 99);
        QCOMPARE(m.positionOf(0, 0), int64_t(3));
        QCOMPARE(m.positionOf(1, 3), int64_t(3));
        QCOMPARE(m.shift(0), int64_t(-3));
        QCOMPARE(m.shift(1), int64_t(0));
    }

    void negativeOffsetHoldsBAtTheStart()
    {
        const TimelineMapper m = make(100, 100, -5);
        QCOMPARE(m.length(), int64_t(105));
        QCOMPARE(m.local(0, 0).frame, 0);
        QVERIFY(!m.local(0, 0).hold);
        QCOMPARE(m.local(1, 0).frame, 0);
        QVERIFY(m.local(1, 0).hold);  // B has not started yet
        QCOMPARE(m.local(1, 5).frame, 0);
        QVERIFY(!m.local(1, 5).hold);
        QCOMPARE(m.local(0, 104).frame, 99);
        QVERIFY(m.local(0, 104).hold);
        QCOMPARE(m.local(1, 104).frame, 99);
        QCOMPARE(m.positionOf(1, 0), int64_t(5));
    }

    void longerBExtendsTheEnd()
    {
        const TimelineMapper m = make(50, 80, -10);
        QCOMPARE(m.length(), int64_t(90));
        QCOMPARE(m.local(1, 89).frame, 79);
        QVERIFY(m.local(0, 89).hold);
        QCOMPARE(m.local(0, 89).frame, 49);
    }

    void everyPositionPairsConsistently()
    {
        for (int64_t off : {-30, -1, 0, 1, 17, 44, 60, 200}) {
            const TimelineMapper m = make(60, 45, off);
            const bool overlap = off < 45 && off > -60;
            for (int64_t p = 0; p < m.length(); ++p) {
                const auto a = m.local(0, p), b = m.local(1, p);
                if (overlap)
                    QVERIFY(!(a.hold && b.hold));  // no gap when the videos overlap
                if (!a.hold && !b.hold)
                    QCOMPARE(int64_t(b.frame), int64_t(a.frame) + off);
                if (!a.hold)
                    QCOMPARE(m.positionOf(0, a.frame), p);
                if (!b.hold)
                    QCOMPARE(m.positionOf(1, b.frame), p);
            }
            // Both videos are fully on the timeline.
            QCOMPARE(m.local(0, m.positionOf(0, 0)).frame, 0);
            QCOMPARE(m.local(0, m.positionOf(0, 59)).frame, 59);
            QCOMPARE(m.local(1, m.positionOf(1, 0)).frame, 0);
            QCOMPARE(m.local(1, m.positionOf(1, 44)).frame, 44);
            QVERIFY(m.positionOf(0, 0) >= 0 && m.positionOf(1, 0) >= 0);
            QVERIFY(m.positionOf(0, 59) < m.length() && m.positionOf(1, 44) < m.length());
        }
    }

    void timestampModeMatchesTimesAcrossRates()
    {
        // A at 60 fps, B at 24 fps: each B frame covers 2.5 A frames.
        const TimelineMapper m = make(120, 48, 0, 60, 24, SyncMode::Timestamp);
        QCOMPARE(m.fps(), 60.0);
        QCOMPARE(m.length(), int64_t(120));
        QCOMPARE(m.local(1, 0).frame, 0);
        QCOMPARE(m.local(1, 2).frame, 0);
        QCOMPARE(m.local(1, 3).frame, 1);
        QCOMPARE(m.local(1, 5).frame, 2);
        QCOMPARE(m.local(1, 119).frame, 47);
        // Offset in B frames: B frame 2 at A frame 0.
        const TimelineMapper o = make(120, 48, 2, 60, 24, SyncMode::Timestamp);
        QCOMPARE(o.length(), int64_t(125));  // B's first two frames come 5 A frames early
        QCOMPARE(o.local(1, 0).frame, 0);
        QVERIFY(o.local(0, 0).hold);
        QCOMPARE(o.local(1, 5).frame, 2);
        QCOMPARE(o.local(0, 5).frame, 0);
        QVERIFY(o.local(1, 124).hold);
    }
};

QTEST_APPLESS_MAIN(TstTimelineMapper)
#include "tst_timelinemapper.moc"
