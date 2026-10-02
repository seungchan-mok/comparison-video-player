#include "engine/PlaybackController.h"

#include <QtTest>

#include <map>
#include <set>

using namespace scvp;

namespace {

class FakeBackend : public PlaybackBackend {
public:
    int32_t frames = 100;
    bool final = true;
    int32_t partial = 0;
    int32_t estimate = 0;
    double fps = 30;
    bool open[2] = {true, false};
    std::set<int32_t> available;
    std::vector<PlaybackIntent> intents;
    mutable std::map<int32_t, FramePtr> made;

    bool isOpen(int slot) const override { return open[slot]; }
    int32_t frameCount(int) const override { return final ? frames : partial; }
    bool frameCountFinal(int) const override { return final; }
    int32_t frameCountEstimate(int) const override { return estimate; }
    double frameRate(int) const override { return fps; }
    FramePtr frame(int, int32_t n) const override
    {
        if (!available.count(n))
            return nullptr;
        FramePtr& f = made[n];
        if (!f) {
            auto nf = std::make_shared<Frame>();
            nf->pres = n;
            f = nf;
        }
        return f;
    }
    void request(int, const PlaybackIntent& in) override { intents.push_back(in); }
    void all()
    {
        for (int32_t i = 0; i < frames; ++i)
            available.insert(i);
    }
};

} // namespace

class TstPlaybackController : public QObject {
    Q_OBJECT

private slots:
    void playFollowsTheClock()
    {
        FakeBackend b;
        b.all();
        PlaybackController c(b);
        c.mediaChanged();
        c.play();
        QVERIFY(c.playing());
        QCOMPARE(c.tick(10.0).position, int64_t(0));
        QCOMPARE(c.tick(10.0 + 1.0 / 60).position, int64_t(0));  // 60 Hz ticks, 30 fps video
        QCOMPARE(c.tick(10.0 + 2.0 / 60).position, int64_t(1));
        QCOMPARE(c.tick(10.5).position, int64_t(15));
        QVERIFY(c.tick(10.5).frames[0].exact);
    }

    void speedScalesTheClock()
    {
        FakeBackend b;
        b.all();
        PlaybackController c(b);
        c.mediaChanged();
        c.setSpeed(2.0);
        c.play();
        c.tick(0);
        QCOMPARE(c.tick(0.5).position, int64_t(30));
    }

    void stallsAtMissingFrameAndResumesWithoutBurst()
    {
        FakeBackend b;
        for (int32_t i = 0; i < 100; ++i)
            if (i != 5)
                b.available.insert(i);
        PlaybackController c(b);
        c.mediaChanged();
        c.play();
        c.tick(0);
        TickResult r = c.tick(0.3);  // due 9, but 5 is missing
        QCOMPARE(r.position, int64_t(4));
        r = c.tick(0.4);  // first tick that cannot advance: the stall starts here
        QVERIFY(r.stalled);
        QCOMPARE(r.stalledSeconds, 0.0);
        r = c.tick(0.5);
        QVERIFY(r.stalled);
        QVERIFY(r.stalledSeconds > 0.09);
        b.available.insert(5);
        r = c.tick(0.55);
        QCOMPARE(r.position, int64_t(5));  // resumes one frame at a time
        QVERIFY(!r.stalled);
        QCOMPARE(c.tick(0.55 + 1.0 / 30 + 1e-4).position, int64_t(6));
    }

    void reachesTheEndAndStops()
    {
        FakeBackend b;
        b.frames = 10;
        b.all();
        PlaybackController c(b);
        c.mediaChanged();
        c.play();
        c.tick(0);
        QCOMPARE(c.tick(5.0).position, int64_t(9));
        QVERIFY(!c.playing());
        c.play();  // playing again starts over
        QCOMPARE(c.position(), int64_t(0));
    }

    void loopWraps()
    {
        FakeBackend b;
        b.all();
        PlaybackController c(b);
        c.mediaChanged();
        c.setLoop(10, 19);
        c.seek(10);
        c.play();
        c.tick(0);
        QCOMPARE(c.tick(9.5 / 30).position, int64_t(19));
        QCOMPARE(c.tick(10.5 / 30).position, int64_t(10));
        QCOMPARE(c.tick(10.5 / 30 + 3.0 / 30).position, int64_t(13));
        QVERIFY(c.playing());
        QVERIFY(b.intents.back().loopActive);
        QCOMPARE(b.intents.back().loop.first, 10);
        c.toggleLoop();
        QVERIFY(!c.loopActive());
        c.clearLoop();
        QVERIFY(!c.hasLoop());
    }

    void loopPointsFromPosition()
    {
        FakeBackend b;
        b.all();
        PlaybackController c(b);
        c.mediaChanged();
        c.seek(30);
        c.setLoopStart();
        c.seek(60);
        c.setLoopEnd();
        QCOMPARE(c.loopFirst(), int64_t(30));
        QCOMPARE(c.loopLast(), int64_t(60));
        QVERIFY(c.loopActive());
    }

    void stepsAccumulateAndRecordDirection()
    {
        FakeBackend b;
        b.available = {0, 1, 2, 3};
        PlaybackController c(b);
        c.mediaChanged();
        c.tick(0);
        c.step(1);
        c.step(1);
        c.step(1);
        QCOMPARE(c.position(), int64_t(3));
        QCOMPARE(b.intents.back().direction, 1);
        c.step(-1);
        QCOMPARE(c.position(), int64_t(2));
        QCOMPARE(b.intents.back().direction, -1);
        QCOMPARE(b.intents.back().playhead, 2);
    }

    void keepsPreviousFrameUntilTheRequestedOneArrives()
    {
        FakeBackend b;
        b.available = {0};
        PlaybackController c(b);
        c.mediaChanged();
        QVERIFY(c.tick(0).frames[0].exact);
        QVERIFY(c.positionShown());
        c.step(1);
        TickResult r = c.tick(0.01);
        QCOMPARE(r.frames[0].number, 0);
        QVERIFY(!r.frames[0].exact);
        QVERIFY(!c.positionShown());
        b.available.insert(1);
        r = c.tick(0.02);
        QCOMPARE(r.frames[0].number, 1);
        QVERIFY(r.frames[0].exact && r.changed);
        QVERIFY(c.positionShown());
    }

    void jumpsUseSecondsOrFrames()
    {
        FakeBackend b;
        b.frames = 1000;
        b.all();
        PlaybackController c(b);
        c.mediaChanged();
        QCOMPARE(c.jumpFrames(false), 150);  // 5 s at 30 fps
        QCOMPARE(c.jumpFrames(true), 900);
        c.jump(+1, false);
        QCOMPARE(c.position(), int64_t(150));
        c.jump(+1, true);
        QCOMPARE(c.position(), int64_t(999));  // clamped
        c.jump(-1, true);
        QCOMPARE(c.position(), int64_t(99));
        JumpSettings j;
        j.inFrames = true;
        j.small = 10;
        j.large = 100;
        c.setJumpSettings(j);
        c.jump(-1, false);
        QCOMPARE(c.position(), int64_t(89));
        QCOMPARE(b.intents.back().jumpDistances, (std::vector<int32_t>{10, 100}));
    }

    void seeksBeforeTheIndexIsCompleteAreKept()
    {
        FakeBackend b;
        b.all();
        b.final = false;
        b.partial = 20;
        b.estimate = 120;  // from duration x fps
        PlaybackController c(b);
        c.mediaChanged();
        c.seek(100);
        QCOMPARE(c.position(), int64_t(100));
        QCOMPARE(c.length(), int64_t(120));
        b.final = true;  // the scan finished: 100 frames
        c.mediaChanged();
        QCOMPARE(c.position(), int64_t(99));
    }

    void pauseStaysOnScreenFrame()
    {
        FakeBackend b;
        b.available = {0, 1, 2};
        PlaybackController c(b);
        c.mediaChanged();
        c.play();
        c.tick(0);
        c.tick(1.0);  // stalls at 2
        c.pause();
        QCOMPARE(c.position(), int64_t(2));
        QVERIFY(!c.playing());
    }
};

QTEST_APPLESS_MAIN(TstPlaybackController)
#include "tst_playbackcontroller.moc"
