#include "engine/Engine.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <chrono>
#include <random>
#include <thread>

using namespace scvp;

namespace {

FramePtr waitFrame(const Engine& e, int slot, int32_t n, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (FramePtr f = e.frame(slot, n))
            return f;
        if (std::chrono::steady_clock::now() > deadline)
            return nullptr;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

PlaybackIntent paused(int32_t p, int dir = 1)
{
    PlaybackIntent in;
    in.playhead = p;
    in.direction = dir;
    return in;
}

struct OpenedEngine {
    Engine engine;
    std::vector<std::string> ref;
    int32_t frames = 0;
};

bool openFixture(OpenedEngine& o, int slot, const QString& name)
{
    const auto fx = test::fixture(name);
    if (!fx)
        return false;
    std::string err;
    if (!o.engine.open(slot, fx->path.toStdString(), InputOptions{}, &err))
        return false;
    o.ref = test::referenceHashes(*fx);
    o.frames = fx->frames;
    // Wait for the complete index so frame numbers are final.
    for (int i = 0; i < 3000 && !o.engine.stats(slot).indexComplete; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return o.engine.stats(slot).indexComplete;
}

size_t frameBytesFor(Engine& e, int slot)
{
    // Decode frame 0 so the real frame size is known.
    e.request(slot, paused(0));
    FramePtr f = waitFrame(e, slot, 0);
    return f ? f->bytes : 0;
}

} // namespace

class TstEngine : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void showsRequestedFrame()
    {
        SCVP_REQUIRE_FIXTURES();
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("h264_b")));
        o.engine.request(0, paused(100));
        FramePtr f = waitFrame(o.engine, 0, 100);
        QVERIFY(f);
        QCOMPARE(test::frameMd5(f->av), o.ref[100]);
        // The neighbourhood fills in without further requests.
        QVERIFY(waitFrame(o.engine, 0, 101));
        QVERIFY(waitFrame(o.engine, 0, 99));
    }

    void sequentialPlayback()
    {
        SCVP_REQUIRE_FIXTURES();
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("hevc_opengop")));
        PlaybackIntent in;
        in.playing = true;
        for (int32_t p = 0; p < o.frames; ++p) {
            in.playhead = p;
            o.engine.request(0, in);
            FramePtr f = waitFrame(o.engine, 0, p);
            QVERIFY2(f, qPrintable(QStringLiteral("frame %1 never arrived").arg(p)));
            QCOMPARE(test::frameMd5(f->av), o.ref[size_t(p)]);
        }
        QVERIFY(o.engine.stats(0).seeks <= 2);
    }

    void backwardStepping_data()
    {
        QTest::addColumn<int>("capacityGops");  // capacity in GOPs x 4 (8 = two GOPs, 2 = half a GOP)
        QTest::newRow("ample") << 40;
        QTest::newRow("two GOPs") << 9;
        QTest::newRow("one GOP") << 4;
        QTest::newRow("half GOP") << 2;
    }

    void backwardStepping()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(int, capacityGops);
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("h264_b")));  // 180 frames, GOP 48
        const size_t fb = frameBytesFor(o.engine, 0);
        QVERIFY(fb > 0);
        const int32_t gop = 48;
        const int32_t wantCapacity = gop * capacityGops / 4;
        o.engine.setMemoryBudget(size_t(wantCapacity + 26) * fb);
        const int32_t cap = o.engine.capacityFrames(0);
        QVERIFY(cap >= wantCapacity - 2);
        const uint64_t decodedBefore = o.engine.stats(0).framesDecoded;
        int32_t steps = 0;
        for (int32_t p = o.frames - 1; p >= 0; --p, ++steps) {
            o.engine.request(0, paused(p, -1));
            FramePtr f = waitFrame(o.engine, 0, p);
            QVERIFY2(f, qPrintable(QStringLiteral("frame %1 never arrived (capacity %2)\n%3")
                                       .arg(p)
                                       .arg(cap)
                                       .arg(QString::fromStdString(o.engine.debugState(0)))));
            QCOMPARE(test::frameMd5(f->av), o.ref[size_t(p)]);
            const SlotStats st = o.engine.stats(0);
            QVERIFY(st.cachedBytes <= st.budgetBytes + fb);
        }
        const double perStep = double(o.engine.stats(0).framesDecoded - decodedBefore) / steps;
        qInfo("capacity %d frames: %.2f decoded frames per backward step, %llu seeks", cap, perStep,
              (unsigned long long)o.engine.stats(0).seeks);
        if (cap >= 2 * gop + 8)
            QVERIFY(perStep < 3.0);
        else if (cap >= gop)
            QVERIFY(perStep < 8.0);
        else
            QVERIFY(perStep < double(gop));
    }

    void randomJumps()
    {
        SCVP_REQUIRE_FIXTURES();
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("vp9")));
        std::mt19937 rng(7);
        for (int i = 0; i < 40; ++i) {
            const int32_t t = int32_t(rng() % uint32_t(o.frames));
            o.engine.request(0, paused(t));
            FramePtr f = waitFrame(o.engine, 0, t);
            QVERIFY(f);
            QCOMPARE(test::frameMd5(f->av), o.ref[size_t(t)]);
        }
    }

    void shrinkingBudgetReleasesMemory()
    {
        SCVP_REQUIRE_FIXTURES();
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("h264_b")));
        const size_t fb = frameBytesFor(o.engine, 0);
        o.engine.setMemoryBudget(size_t(400) * fb);
        o.engine.request(0, paused(90));
        QTRY_VERIFY_WITH_TIMEOUT(o.engine.stats(0).cachedFrames >= size_t(o.frames - 2), 5000);
        o.engine.setMemoryBudget(size_t(60) * fb);
        const SlotStats st = o.engine.stats(0);
        QVERIFY(st.cachedBytes <= st.budgetBytes + fb);
        QTRY_VERIFY_WITH_TIMEOUT(o.engine.stats(0).poolLiveBytes <= size_t(60 + 30) * fb, 2000);
        QVERIFY(o.engine.frame(0, 90));  // the playhead frame stays
    }

    void twoVideosShareTheBudget()
    {
        SCVP_REQUIRE_FIXTURES();
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("h264_b")));
        const auto refA = o.ref;
        QVERIFY(openFixture(o, 1, QStringLiteral("hevc10")));  // 10-bit: twice the bytes per frame
        const auto refB = o.ref;
        o.engine.setMemoryBudget(size_t(64) << 20);
        o.engine.request(0, paused(50));
        o.engine.request(1, paused(50));
        FramePtr a = waitFrame(o.engine, 0, 50);
        FramePtr b = waitFrame(o.engine, 1, 50);
        QVERIFY(a && b);
        QCOMPARE(test::frameMd5(a->av), refA[50]);
        QCOMPARE(test::frameMd5(b->av), refB[50]);
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(o.engine.capacityFrames(0) - o.engine.capacityFrames(1)) <= 2, 2000);
        QVERIFY(o.engine.stats(1).budgetBytes > o.engine.stats(0).budgetBytes);
    }

    void closeWhileDecoding()
    {
        SCVP_REQUIRE_FIXTURES();
        for (int i = 0; i < 5; ++i) {
            OpenedEngine o;
            const auto fx = test::fixture(QStringLiteral("hevc_opengop"));
            std::string err;
            QVERIFY(o.engine.open(0, fx->path.toStdString(), InputOptions{}, &err));
            PlaybackIntent in;
            in.playing = true;
            o.engine.request(0, in);
            std::this_thread::sleep_for(std::chrono::milliseconds(i * 3));
            o.engine.close(0);
            QVERIFY(!o.engine.isOpen(0));
        }
    }
};

QTEST_GUILESS_MAIN(TstEngine)
#include "tst_engine.moc"
