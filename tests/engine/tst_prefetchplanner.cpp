#include "engine/PrefetchPlanner.h"

#include <QtTest>

#include <random>
#include <set>

using namespace scvp;

namespace {

std::set<int32_t> wanted(const WantList& w, bool includeIslands = false)
{
    std::set<int32_t> s;
    for (const Want& want : w.wants) {
        if (want.cls == WantClass::Island && !includeIslands)
            continue;
        for (int32_t i = want.range.first; i <= want.range.last; ++i)
            s.insert(i);
    }
    return s;
}

const Want* find(const WantList& w, WantClass cls, int nth = 0)
{
    for (const Want& want : w.wants)
        if (want.cls == cls && nth-- == 0)
            return &want;
    return nullptr;
}

PlannerInput base()
{
    PlannerInput in;
    in.playhead = 500;
    in.frameCount = 2000;
    in.capacity = 100;
    in.fps = 30;
    in.gop = 30;
    return in;
}

} // namespace

class TstPrefetchPlanner : public QObject {
    Q_OBJECT

private slots:
    void targetComesFirst()
    {
        const WantList w = planPrefetch(base());
        QVERIFY(!w.wants.empty());
        QCOMPARE(int(w.wants[0].cls), int(WantClass::Target));
        QVERIFY((w.wants[0].range == FrameRange{500, 500}));
        QCOMPARE(w.target, 500);
        QVERIFY(w.policy.isProtected(500));
    }

    void pausedSplitsSixtyForty()
    {
        PlannerInput in = base();
        in.gop = 0;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        QCOMPARE(int(s.size()), 100);
        QCOMPARE(*s.begin(), 500 - 40);  // 40 % behind, the rest ahead (the target takes a slot)
        QCOMPARE(*s.rbegin(), 500 + 59);
        // Near frames in the move direction come before the window.
        const Want* nearFwd = find(w, WantClass::Near, 0);
        QVERIFY(nearFwd && nearFwd->range.first == 501 && nearFwd->fill == 1);
        const Want* nearBack = find(w, WantClass::Near, 1);
        QVERIFY(nearBack && nearBack->range.last == 499 && nearBack->fill == -1);
        QCOMPARE(w.policy.behindWeight, 59.0 / 40.0);
    }

    void pausedBackwardMirrors()
    {
        PlannerInput in = base();
        in.gop = 0;
        in.direction = -1;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        QCOMPARE(*s.begin(), 500 - 59);
        QCOMPARE(*s.rbegin(), 500 + 40);
        QCOMPARE(find(w, WantClass::Near, 0)->fill, -1);
    }

    void pausedKeepsPreviousGop()
    {
        PlannerInput in = base();
        in.capacity = 600;
        in.gop = 250;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        QVERIFY(500 - *s.begin() >= 258);
        QVERIFY(int(s.size()) <= 600);
    }

    void playingLooksAhead()
    {
        PlannerInput in = base();
        in.playing = true;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        QCOMPARE(*s.begin(), 500 - 20);
        QCOMPARE(*s.rbegin(), 500 + 79);
        const Want* nearW = find(w, WantClass::Near);
        QVERIFY(nearW && nearW->range.first == 501 && nearW->range.last == 560);
        QCOMPARE(w.policy.behindWeight, 79.0 / 20.0);
        QVERIFY(!find(w, WantClass::Island));  // no islands while playing
    }

    void boundaryCapacityMovesToOtherSide()
    {
        PlannerInput in = base();
        in.playhead = 0;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        QCOMPARE(*s.begin(), 0);
        QVERIFY(int(s.size()) >= 95);
        in.playhead = 1999;
        const auto e = wanted(planPrefetch(in));
        QCOMPARE(*e.rbegin(), 1999);
        QVERIFY(int(e.size()) >= 95);
    }

    void pinnedLoopIsTheWindow()
    {
        PlannerInput in = base();
        in.loopActive = true;
        in.loop = {450, 529};  // 80 frames <= 0.9 * 100
        in.playing = true;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        for (int32_t i = 450; i <= 529; ++i)
            QVERIFY(s.count(i));
        QVERIFY(w.policy.isProtected(450) && w.policy.isProtected(529));
        QVERIFY(w.policy.loopWraps);
        QVERIFY(*s.rbegin() <= 560);
    }

    void largeLoopWrapsWindow()
    {
        PlannerInput in = base();
        in.loopActive = true;
        in.loop = {400, 599};  // 200 frames: does not fit
        in.playhead = 580;
        in.playing = true;
        const WantList w = planPrefetch(in);
        const auto s = wanted(w);
        QVERIFY(s.count(599));
        QVERIFY(s.count(400));   // wrapped
        QVERIFY(!s.count(600));  // never beyond the loop end
        QVERIFY(!w.policy.isProtected(450));
    }

    void islandsAtJumpTargets()
    {
        PlannerInput in = base();
        in.capacity = 400;
        in.jumpDistances = {150, 900};
        const WantList w = planPrefetch(in);
        const auto withIslands = wanted(w, true);
        QVERIFY(withIslands.count(500 + 900));
        QVERIFY(!withIslands.count(500 - 900));  // before the start: none
        // +150 / -150 lie inside the window already, so no island is needed there.
        int islands = 0;
        for (const Want& want : w.wants)
            if (want.cls == WantClass::Island) {
                ++islands;
                QVERIFY(w.policy.isProtected(want.range.first));
            }
        QCOMPARE(islands, 1);
        QVERIFY(int(wanted(w).size()) + 4 * islands <= in.capacity);
    }

    void aheadLimitClampsLeader()
    {
        PlannerInput in = base();
        in.playing = true;
        in.aheadLimit = 520;
        const auto s = wanted(planPrefetch(in));
        QVERIFY(*s.rbegin() <= 560);  // near frames may still exceed the limit slightly
        const WantList w = planPrefetch(in);
        for (const Want& want : w.wants)
            if (want.cls == WantClass::Window && want.fill > 0)
                QVERIFY(want.range.last <= 520);
    }

    // The plan must be exactly what the cache keeps: every planned frame is at least as cheap
    // under the returned policy as every frame left out (otherwise cache and plan thrash).
    void planMatchesCachePolicy()
    {
        std::mt19937 rng(42);
        for (int trial = 0; trial < 500; ++trial) {
            PlannerInput in;
            in.frameCount = 50 + int(rng() % 3000);
            in.playhead = int(rng() % uint32_t(in.frameCount));
            in.capacity = 4 + int(rng() % 400);
            in.direction = (rng() & 1) ? 1 : -1;
            in.playing = (rng() % 3) == 0;
            in.gop = int(rng() % 120);
            in.fps = 30;
            const WantList w = planPrefetch(in);
            const auto s = wanted(w);
            QVERIFY(int(s.size()) <= in.capacity);
            double maxIn = 0;
            for (int32_t i : s)
                maxIn = std::max(maxIn, w.policy.cost(i));
            double minOut = 1e30;
            for (int32_t i = 0; i < in.frameCount; ++i)
                if (!s.count(i))
                    minOut = std::min(minOut, w.policy.cost(i));
            QVERIFY2(maxIn <= minOut + w.policy.behindWeight + 1e-9,
                     qPrintable(QStringLiteral("trial %1: max planned cost %2 > min unplanned %3").arg(trial).arg(maxIn).arg(minOut)));
        }
    }
};

QTEST_APPLESS_MAIN(TstPrefetchPlanner)
#include "tst_prefetchplanner.moc"
