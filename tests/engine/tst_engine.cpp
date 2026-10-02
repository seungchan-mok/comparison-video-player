#include "engine/Engine.h"
#include "engine/GpuFrame.h"
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
    o.engine.setHardware(HwConfig{HwPolicy::Off});  // hashes are compared with software references
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

    // Two videos played to the end with a cache smaller than the file: every frame decodes about
    // once. (A capacity planned from an estimated frame size used to exceed what the cache held;
    // near the end the full-size plan never fit and evicted frames were re-decoded GOP by GOP.)
    void playingToTheEndDecodesEachFrameOnce()
    {
        SCVP_REQUIRE_FIXTURES();
        OpenedEngine o;
        QVERIFY(openFixture(o, 0, QStringLiteral("h264_b")));
        QVERIFY(openFixture(o, 1, QStringLiteral("h264_b")));
        const size_t fb = frameBytesFor(o.engine, 0);
        QVERIFY(fb > 0);
        o.engine.setMemoryBudget(fb * 220);  // ~90 frames per video plus decoder reserve
        QVERIFY(o.engine.capacityFrames(0) < o.frames);
        PlaybackIntent in;
        in.playing = true;
        for (int32_t p = 0; p < o.frames; ++p) {
            in.playhead = p;
            o.engine.request(0, in);
            o.engine.request(1, in);
            QVERIFY2(waitFrame(o.engine, 0, p) && waitFrame(o.engine, 1, p), qPrintable(QString::number(p)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // a refill loop would keep going
        for (int slot = 0; slot < 2; ++slot) {
            const SlotStats st = o.engine.stats(slot);
            QVERIFY2(st.framesDecoded <= uint64_t(o.frames) * 11 / 10,
                     qPrintable(QStringLiteral("slot %1: %2 decoded, %3 seeks").arg(slot).arg(st.framesDecoded).arg(st.seeks)));
        }
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
        // The bounds below are what a regression in the backward-fill logic breaks first.
        const uint64_t seeks = o.engine.stats(0).seeks;
        if (cap >= o.frames) {
            // Everything fits: ideally each GOP is decoded once with one seek per GOP (1.0 / 4 on
            // an idle machine). Heavy CPU load can make the lane re-plan a little more; the bounds
            // still catch the restart-at-key regression (6.5 frames per step, 13+ seeks).
            QVERIFY2(perStep < 1.5, qPrintable(QString::number(perStep)));
            QVERIFY2(seeks <= 10, qPrintable(QString::number(seeks)));
        } else if (cap >= 2 * gop + 8)
            QVERIFY(perStep < 1.5);
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

    // NVDEC + VRAM tier: the frames around the playhead stay on the GPU, the budget holds,
    // frames pushed out are spilled to system memory, and every frame is still exact.
    void gpuTier()
    {
        SCVP_REQUIRE_FIXTURES();
        if (Engine::defaultGpuMemoryBudget() == 0)
            QSKIP("no CUDA device");
        const auto fx = test::fixture(QStringLiteral("h264_b"));
        Engine e;
        e.setHardware(HwConfig{});
        e.setMemoryBudget(size_t(64) << 20);
        e.setGpuMemoryBudget(size_t(40) * 320 * 240 * 3 / 2);  // ~40 frames
        std::string err;
        QVERIFY(e.open(0, fx->path.toStdString(), InputOptions{}, &err));
        const auto ref = test::referenceHashes(*fx);
        QTRY_VERIFY_WITH_TIMEOUT(e.stats(0).indexComplete, 5000);
        for (int32_t p = 0; p < 180; p += 3) {
            e.request(0, paused(p));
            FramePtr f = waitFrame(e, 0, p);
            QVERIFY(f);
            std::string h;
            if (f->location == FrameLocation::Cuda) {
                FramePool tmp;
                AVFrame* cpu = f->gpu->pool->download(*f->gpu, tmp);
                h = test::frameMd5AsSoftware(cpu);
                av_frame_free(&cpu);
            } else {
                h = test::frameMd5AsSoftware(f->av);
            }
            QCOMPARE(h, ref[size_t(p)]);
            const SlotStats st = e.stats(0);
            QVERIFY(st.gpuBytes <= st.gpuBudgetBytes + 320 * 240 * 2);
        }
        const SlotStats st = e.stats(0);
        QCOMPARE(st.hardwareDevice, std::string("cuda"));
        QVERIFY(st.gpuFrames > 0);
        QVERIFY(st.spills > 0);
    }

    // A jump into a region nothing has decoded yet: the target is decoded on the GPU and stays
    // there (frames that went to system memory earlier are not moved up: no gain over upload).
    void jumpTargetStaysOnGpu()
    {
        SCVP_REQUIRE_FIXTURES();
        if (Engine::defaultGpuMemoryBudget() == 0)
            QSKIP("no CUDA device");
        const auto fx = test::fixture(QStringLiteral("hevc_opengop"));
        Engine e;
        e.setHardware(HwConfig{});
        e.setMemoryBudget(size_t(30) * 320 * 240 * 3 / 2 + (size_t(4) << 20));
        e.setGpuMemoryBudget(size_t(40) * 320 * 240 * 3 / 2);
        std::string err;
        QVERIFY(e.open(0, fx->path.toStdString(), InputOptions{}, &err));
        QTRY_VERIFY_WITH_TIMEOUT(e.stats(0).indexComplete, 5000);
        e.request(0, paused(0));
        QVERIFY(waitFrame(e, 0, 0));
        e.request(0, paused(170));
        FramePtr f = waitFrame(e, 0, 170);
        QVERIFY(f);
        QCOMPARE(f->location, FrameLocation::Cuda);
    }

    void closeWhileDecoding()
    {
        SCVP_REQUIRE_FIXTURES();
        for (int i = 0; i < 5; ++i) {
            OpenedEngine o;
            const auto fx = test::fixture(QStringLiteral("hevc_opengop"));
            std::string err;
            o.engine.setHardware(HwConfig{HwPolicy::Off});
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
