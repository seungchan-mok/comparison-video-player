#include "engine/FrameCache.h"
#include "engine/FrameAllocator.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <atomic>
#include <set>
#include <thread>

using namespace scvp;

namespace {

struct Fx {
    FramePool pool;
    size_t frameBytes = 0;

    FramePtr make(int32_t n, bool provisional = false)
    {
        auto f = std::make_shared<Frame>();
        f->av = pool.allocFrame(AV_PIX_FMT_GRAY8, 64, 64);
        f->pres = n;
        f->bytes = ff::frameBufferBytes(f->av);
        f->provisional = provisional;
        if (!frameBytes)  // set once, before any concurrent use
            frameBytes = f->bytes;
        return f;
    }
    size_t budget(int frames)
    {
        if (!frameBytes)
            make(0);
        return frameBytes * size_t(frames);
    }
};

std::set<int32_t> contents(const FrameCache& c, int32_t lo, int32_t hi)
{
    std::set<int32_t> s;
    for (int32_t i = lo; i <= hi; ++i)
        if (c.contains(i))
            s.insert(i);
    return s;
}

std::set<int32_t> span(int32_t a, int32_t b)
{
    std::set<int32_t> s;
    for (int32_t i = a; i <= b; ++i)
        s.insert(i);
    return s;
}

} // namespace

class TstFrameCache : public QObject {
    Q_OBJECT

private slots:
    void offerAndGet()
    {
        Fx fx;
        FrameCache cache(fx.budget(4));
        QVERIFY(cache.offer(fx.make(3)));
        QVERIFY(cache.contains(3));
        QCOMPARE(cache.get(3)->pres, 3);
        QVERIFY(!cache.get(4));
        QCOMPARE(cache.frames(), size_t(1));
        QCOMPARE(cache.bytes(), fx.frameBytes);
        QVERIFY(!cache.offer(nullptr));
    }

    void keepsFramesClosestToPlayhead()
    {
        Fx fx;
        FrameCache cache(fx.budget(10));
        CachePolicy pol;
        pol.playhead = 50;
        pol.behindWeight = 1.1;
        cache.setPolicy(pol);
        for (int32_t i = 40; i < 60; ++i)
            cache.offer(fx.make(i));
        QCOMPARE(contents(cache, 0, 100), span(46, 55));
        QCOMPARE(cache.frames(), size_t(10));
        QVERIFY(cache.bytes() <= cache.budget());
    }

    void playingWeightsBehindFrames()
    {
        Fx fx;
        FrameCache cache(fx.budget(10));
        CachePolicy pol;
        pol.playhead = 50;
        pol.behindWeight = 4.5;
        cache.setPolicy(pol);
        for (int32_t i = 59; i >= 40; --i)  // insertion order must not matter
            cache.offer(fx.make(i));
        QCOMPARE(contents(cache, 0, 100), span(49, 58));
    }

    void backwardDirectionMirrorsCosts()
    {
        Fx fx;
        FrameCache cache(fx.budget(6));
        CachePolicy pol;
        pol.playhead = 50;
        pol.direction = -1;
        pol.behindWeight = 4.5;  // "behind" is now the frames after the playhead
        cache.setPolicy(pol);
        for (int32_t i = 40; i < 60; ++i)
            cache.offer(fx.make(i));
        // 46..50 lie ahead (cost 4..0); 51 is one step behind (cost 4.5) and beats 45 (cost 5).
        QCOMPARE(contents(cache, 0, 100), span(46, 51));
    }

    void loopKeepsLoopFramesAndWraps()
    {
        Fx fx;
        FrameCache cache(fx.budget(20));
        CachePolicy pol;
        pol.playhead = 110;
        pol.loop = {100, 119};
        pol.loopWraps = true;
        cache.setPolicy(pol);
        for (int32_t i = 90; i < 130; ++i)
            cache.offer(fx.make(i));
        QCOMPARE(contents(cache, 0, 200), span(100, 119));
        cache.setBudget(fx.budget(15));
        // The frames just behind the playhead are reached last after wrapping around.
        std::set<int32_t> expect = span(110, 119);
        for (int32_t i = 100; i <= 104; ++i)
            expect.insert(i);
        QCOMPARE(contents(cache, 0, 200), expect);
    }

    void protectedFramesSurvive()
    {
        Fx fx;
        FrameCache cache(fx.budget(3));
        CachePolicy pol;
        pol.playhead = 0;
        pol.protect = {{90, 90}};
        cache.setPolicy(pol);
        QVERIFY(cache.offer(fx.make(90)));
        for (int32_t i = 0; i < 10; ++i)
            cache.offer(fx.make(i));
        QVERIFY(cache.contains(90));
        QVERIFY(cache.contains(0) && cache.contains(1));
        QVERIFY(!cache.contains(2));
        // Even a budget smaller than one frame keeps the protected frame.
        cache.setBudget(1);
        QVERIFY(cache.contains(90));
        QCOMPARE(cache.frames(), size_t(1));
        QVERIFY(!cache.offer(fx.make(1)));
    }

    void rejectsLessNeededFramesWhenFull()
    {
        Fx fx;
        FrameCache cache(fx.budget(3));
        CachePolicy pol;
        pol.playhead = 0;
        cache.setPolicy(pol);
        QVERIFY(cache.offer(fx.make(0)));
        QVERIFY(cache.offer(fx.make(1)));
        QVERIFY(cache.offer(fx.make(2)));
        QVERIFY(!cache.offer(fx.make(10)));
        QVERIFY(cache.offer(fx.make(1)));  // already cached: accepted, unchanged
        QCOMPARE(cache.frames(), size_t(3));
        QCOMPARE(cache.worstCost(), 2.0);
    }

    void definitiveReplacesProvisional()
    {
        Fx fx;
        FrameCache cache(fx.budget(4));
        auto prov = fx.make(5, true);
        QVERIFY(cache.offer(prov));
        auto real = fx.make(5, false);
        QVERIFY(cache.offer(real));
        QCOMPARE(cache.get(5).get(), real.get());
        QVERIFY(cache.offer(fx.make(5, true)));
        QCOMPARE(cache.get(5).get(), real.get());
        QVERIFY(cache.offer(fx.make(6, true)));
        cache.dropProvisional();
        QVERIFY(cache.contains(5));
        QVERIFY(!cache.contains(6));
    }

    void missingFrameQueries()
    {
        Fx fx;
        FrameCache cache(fx.budget(100));
        for (int32_t i : {1, 2, 3, 7, 8})
            cache.offer(fx.make(i));
        QCOMPARE(cache.firstMissing(0, 10), 0);
        QCOMPARE(cache.firstMissing(1, 10), 4);
        QCOMPARE(cache.firstMissing(1, 3), -1);
        QCOMPARE(cache.lastMissing(0, 8), 6);
        QCOMPARE(cache.lastMissing(7, 8), -1);
        QCOMPARE(cache.lastMissing(0, 12), 12);
        QCOMPARE(cache.missingRunEnd(4, 100), 6);
        QCOMPARE(cache.missingRunEnd(9, 20), 20);
        QCOMPARE(cache.missingRunEnd(7, 20), 6);
        QCOMPARE(cache.countCached(0, 10), 5);
        QCOMPARE(cache.nearest(5, 2)->pres, 3);
        QVERIFY(!cache.nearest(5, 1));
        QCOMPARE(cache.nearest(9, 5)->pres, 8);
    }

    void rangeSnapshots()
    {
        Fx fx;
        FrameCache cache(fx.budget(100));
        for (int32_t i : {1, 2, 3, 7, 8})
            cache.offer(fx.make(i));
        RangeSetPtr a = cache.ranges();
        QCOMPARE(int(a->ranges.size()), 2);
        QVERIFY((a->ranges[0] == FrameRange{1, 3}));
        QVERIFY((a->ranges[1] == FrameRange{7, 8}));
        QCOMPARE(a->frames, size_t(5));
        QCOMPARE(cache.ranges().get(), a.get());  // unchanged content: same snapshot
        const uint64_t v = cache.version();
        cache.offer(fx.make(4));
        QVERIFY(cache.version() != v);
        RangeSetPtr b = cache.ranges();
        QVERIFY((b->ranges[0] == FrameRange{1, 4}));
        cache.erase(2);
        QCOMPARE(int(cache.ranges()->ranges.size()), 3);
    }

    void evictionReleasesMemory()
    {
        Fx fx;
        fx.make(0);  // learn the frame size
        const size_t before = fx.pool.liveBytes();
        {
            FrameCache cache(fx.budget(5));
            for (int32_t i = 0; i < 50; ++i)
                cache.offer(fx.make(i));
            QCOMPARE(cache.frames(), size_t(5));
            QVERIFY(fx.pool.liveBytes() <= before + fx.budget(6) + 5 * 8192);
            cache.clear();
            QVERIFY(fx.pool.liveBytes() <= before + 8192);
        }
    }

    void concurrentOffersAndLookups()
    {
        Fx fx;
        fx.make(0);
        FrameCache cache(fx.budget(64));
        std::atomic<bool> stop{false};
        std::atomic<int> hits{0};
        std::thread reader([&] {
            while (!stop)
                for (int32_t i = 0; i < 256; ++i)
                    if (cache.get(i))
                        ++hits;
        });
        std::vector<std::thread> writers;
        for (int w = 0; w < 2; ++w)
            writers.emplace_back([&, w] {
                for (int round = 0; round < 20; ++round) {
                    CachePolicy pol;
                    pol.playhead = (round * 13 + w * 7) % 256;
                    cache.setPolicy(pol);
                    for (int32_t i = 0; i < 256; ++i)
                        cache.offer(fx.make((i + round) % 256));
                }
            });
        for (auto& t : writers)
            t.join();
        stop = true;
        reader.join();
        QVERIFY(cache.bytes() <= cache.budget());
        QVERIFY(hits.load() > 0);
    }
};

QTEST_APPLESS_MAIN(TstFrameCache)
#include "tst_framecache.moc"
