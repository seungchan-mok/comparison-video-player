#include "engine/FFmpegSource.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <random>
#include <set>

using namespace scvp;

namespace {

struct Opened {
    std::unique_ptr<IVideoSource> source;
    std::shared_ptr<FramePool> pool;
    std::unique_ptr<IDecodeSession> session;
    FrameIndexPtr index;
    std::vector<std::string> ref;
};

Opened openFixture(const QString& name)
{
    Opened o;
    const auto fx = test::fixture(name);
    if (!fx)
        return o;
    std::string err;
    o.source = openVideoSource(fx->path.toStdString(), InputOptions{}, &err);
    if (!o.source || !o.source->waitForIndex(30000))
        return o;
    o.pool = std::make_shared<FramePool>(name.toStdString());
    o.session = o.source->openSession(SessionConfig{o.pool, 0}, &err);
    o.index = o.source->index();
    o.ref = test::referenceHashes(*fx);
    return o;
}

// Fixtures whose frame numbering needs POC ordering arrive with the raw-stream milestone.
bool supportedNow(const test::Fixture& fx) { return fx.name != QLatin1String("h264_b_avi"); }

void addFixtureRows()
{
    QTest::addColumn<QString>("name");
    for (const auto& fx : test::fixtures({QStringLiteral("container")}))
        if (supportedNow(fx))
            QTest::newRow(qPrintable(fx.name)) << fx.name;
}

// Seeks to the key for `target` and decodes until it; every frame on the way must be correct.
QString decodeTo(Opened& o, int32_t target, const DecodeHints& hints, int* framesOut = nullptr)
{
    const SeekPoint sp = o.index->seekPointFor(target);
    if (o.session->seekTo(sp) != DecodeStatus::Ok)
        return QStringLiteral("seek failed");
    int32_t last = -1;
    int frames = 0;
    for (;;) {
        FramePtr f;
        const DecodeStatus st = o.session->next(f, hints);
        if (st != DecodeStatus::Ok)
            return QStringLiteral("decode ended (%1) before frame %2").arg(int(st)).arg(target);
        ++frames;
        if (f->pres <= last)
            return QStringLiteral("frame %1 after %2").arg(f->pres).arg(last);
        last = f->pres;
        if (f->pres >= int32_t(o.ref.size()) || test::frameMd5(f->av) != o.ref[size_t(f->pres)])
            return QStringLiteral("wrong pixels for frame %1 (target %2)").arg(f->pres).arg(target);
        if (f->pres >= target) {
            if (framesOut)
                *framesOut = frames;
            return f->pres == target ? QString() : QStringLiteral("skipped past %1 to %2").arg(target).arg(f->pres);
        }
    }
}

bool extended() { return qEnvironmentVariableIntValue("SCVP_TEST_EXTENDED") != 0; }

// Random targets plus the frames around (a sample of) the keyframes, where GOP-boundary bugs live.
// SCVP_TEST_EXTENDED=1 uses every keyframe and three times the random targets.
std::vector<int32_t> targetsFor(const FrameIndex& idx, int randomCount)
{
    std::set<int32_t> t;
    const int32_t n = idx.frameCount();
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int32_t> dist(0, n - 1);
    if (extended())
        randomCount *= 3;
    for (int i = 0; i < randomCount; ++i)
        t.insert(dist(rng));
    const auto& keys = idx.keyFrames();
    const size_t stride = extended() ? 1 : std::max<size_t>(1, keys.size() / 24);
    for (size_t i = 0; i < keys.size(); i += stride)
        for (int32_t d = -1; d <= 1; ++d)
            if (keys[i] + d >= 0 && keys[i] + d < n)
                t.insert(keys[i] + d);
    t.insert(0);
    t.insert(n - 1);
    return {t.begin(), t.end()};
}

} // namespace

class TstDecodeSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void sequential_data() { addFixtureRows(); }
    void sequential()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        Opened o = openFixture(name);
        QVERIFY(o.session && o.index);
        QCOMPARE(o.index->frameCount(), int32_t(o.ref.size()));
        int32_t expected = 0;
        for (;;) {
            FramePtr f;
            const DecodeStatus st = o.session->next(f, DecodeHints{});
            if (st == DecodeStatus::Eof)
                break;
            QCOMPARE(int(st), int(DecodeStatus::Ok));
            QCOMPARE(f->pres, expected);
            QVERIFY(!f->provisional);
            QCOMPARE(test::frameMd5(f->av), o.ref[size_t(expected)]);
            ++expected;
        }
        QCOMPARE(expected, int32_t(o.ref.size()));
        QVERIFY(o.session->position().eof);
        QCOMPARE(o.session->indexMismatches(), 0);
    }

    void randomAccess_data() { addFixtureRows(); }
    void randomAccess()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        Opened o = openFixture(name);
        QVERIFY(o.session && o.index);
        for (int32_t t : targetsFor(*o.index, 100)) {
            const QString err = decodeTo(o, t, DecodeHints{});
            QVERIFY2(err.isEmpty(), qPrintable(err));
        }
        QCOMPARE(o.session->indexMismatches(), 0);
    }

    void randomAccessSkippingNonRef_data() { addFixtureRows(); }
    void randomAccessSkippingNonRef()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        Opened o = openFixture(name);
        QVERIFY(o.session && o.index);
        for (int32_t t : targetsFor(*o.index, 40)) {
            DecodeHints hints;
            hints.skipNonRefBelow = t;
            const QString err = decodeTo(o, t, hints);
            QVERIFY2(err.isEmpty(), qPrintable(err));
        }
    }

    void skippingNonRefDecodesFewerFrames()
    {
        SCVP_REQUIRE_FIXTURES();
        Opened o = openFixture(QStringLiteral("h264_b"));
        QVERIFY(o.session);
        const int32_t target = 46;  // late in the first 48-frame GOP
        int full = 0;
        int skipped = 0;
        QVERIFY(decodeTo(o, target, DecodeHints{}, &full).isEmpty());
        DecodeHints hints;
        hints.skipNonRefBelow = target;
        QVERIFY(decodeTo(o, target, hints, &skipped).isEmpty());
        QVERIFY2(skipped < full, qPrintable(QStringLiteral("%1 vs %2").arg(skipped).arg(full)));
    }

    void backwardWalk_data() { addFixtureRows(); }
    void backwardWalk()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        Opened o = openFixture(name);
        QVERIFY(o.session && o.index);
        for (int32_t t = o.index->frameCount() - 1; t >= 0; t -= extended() ? 3 : 11) {
            const QString err = decodeTo(o, t, DecodeHints{});
            QVERIFY2(err.isEmpty(), qPrintable(err));
        }
    }

    void cancellation()
    {
        SCVP_REQUIRE_FIXTURES();
        Opened o = openFixture(QStringLiteral("h264_b"));
        QVERIFY(o.session);
        std::atomic<uint64_t> gen{1};
        DecodeHints hints;
        hints.cancel = &gen;
        hints.generation = 1;
        FramePtr f;
        QCOMPARE(int(o.session->next(f, hints)), int(DecodeStatus::Ok));
        gen = 2;
        QCOMPARE(int(o.session->next(f, hints)), int(DecodeStatus::Cancelled));
    }
};

QTEST_GUILESS_MAIN(TstDecodeSession)
#include "tst_decodesession.moc"
