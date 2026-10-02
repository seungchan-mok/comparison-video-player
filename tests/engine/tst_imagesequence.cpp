#include "engine/Engine.h"
#include "engine/ImageSequenceSource.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <chrono>
#include <random>
#include <thread>

using namespace scvp;

namespace {

void addRows()
{
    QTest::addColumn<QString>("name");
    for (const auto& fx : test::fixtures({QStringLiteral("imageseq")}))
        QTest::newRow(qPrintable(fx.name)) << fx.name;
}

} // namespace

class TstImageSequence : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void naturalOrder()
    {
        QVERIFY(naturalLess("frame2.png", "frame10.png"));
        QVERIFY(!naturalLess("frame10.png", "frame2.png"));
        QVERIFY(naturalLess("a_0009.png", "a_0010.png"));
        QVERIFY(naturalLess("Shot1.png", "shot2.png"));
        QVERIFY(naturalLess("x99999999999999999999.png", "x100000000000000000000.png"));  // no overflow
        QVERIFY(!naturalLess("same.png", "same.png"));
    }

    void findsTheSequence()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto start7 = test::fixture(QStringLiteral("png_start7"));
        // From any member file: its numbered siblings, from the folder: every image.
        const auto fromFile = findImageSequence((start7->path + QStringLiteral("/img012.png")).toStdString());
        QCOMPARE(int(fromFile.size()), 10);
        QVERIFY(QString::fromStdString(fromFile.front()).endsWith(QLatin1String("img007.png")));
        QVERIFY(QString::fromStdString(fromFile.back()).endsWith(QLatin1String("img016.png")));
        QCOMPARE(findImageSequence(start7->path.toStdString()), fromFile);
        const auto gap = test::fixture(QStringLiteral("png_gap"));
        QCOMPARE(int(findImageSequence(gap->path.toStdString()).size()), gap->frames);  // the hole just skips
    }

    void sequentialFrames_data() { addRows(); }
    void sequentialFrames()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        const auto fx = test::fixture(name);
        std::string err;
        InputOptions opts;
        opts.frameRate = fx->openArgs.toDouble();
        auto src = openVideoSource(fx->path.toStdString(), opts, &err);
        QVERIFY2(src, err.c_str());
        QVERIFY(src->caps().randomAccess);
        const auto ref = test::referenceHashes(*fx);
        QCOMPARE(src->index()->frameCount(), int32_t(ref.size()));
        QCOMPARE(src->index()->frameRate(), 30.0);
        auto pool = std::make_shared<FramePool>();
        auto s = src->openSession(SessionConfig{pool, 0, HwConfig{HwPolicy::Off}}, &err);
        QVERIFY(s);
        QCOMPARE(s->seekTo(src->index()->seekPointFor(0)), DecodeStatus::Ok);
        for (size_t i = 0; i < ref.size(); ++i) {
            FramePtr f;
            QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Ok);
            QCOMPARE(f->pres, int32_t(i));
            QCOMPARE(test::frameMd5(f->av), ref[i]);
        }
        FramePtr f;
        QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Eof);
    }

    void randomFrames_data() { addRows(); }
    void randomFrames()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        const auto fx = test::fixture(name);
        std::string err;
        auto src = openVideoSource(fx->path.toStdString(), InputOptions{}, &err);
        QVERIFY2(src, err.c_str());
        const auto ref = test::referenceHashes(*fx);
        auto pool = std::make_shared<FramePool>();
        auto s = src->openSession(SessionConfig{pool, 0, HwConfig{HwPolicy::Off}}, &err);
        std::mt19937 rng(11);
        for (int i = 0; i < 25; ++i) {
            const int32_t n = int32_t(rng() % ref.size());
            QCOMPARE(s->seekTo(src->index()->seekPointFor(n)), DecodeStatus::Ok);
            FramePtr f;
            QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Ok);
            QCOMPARE(f->pres, n);
            QCOMPARE(test::frameMd5(f->av), ref[size_t(n)]);
        }
    }

    void singleImage()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("png_seq"));
        QTemporaryDir dir;
        const QString one = dir.filePath(QStringLiteral("still.png"));
        QVERIFY(QFile::copy(fx->path + QStringLiteral("/frame_0003.png"), one));
        std::string err;
        auto src = openVideoSource(one.toStdString(), InputOptions{}, &err);
        QVERIFY2(src, err.c_str());
        QCOMPARE(src->index()->frameCount(), 1);
        QCOMPARE(src->info().width, 320);
    }

    void playsThroughTheEngine()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("jpg_seq"));
        Engine e;
        std::string err;
        QVERIFY2(e.open(0, fx->path.toStdString(), InputOptions{}, &err), err.c_str());
        PlaybackIntent in;
        in.playhead = 12;
        e.request(0, in);
        const auto ref = test::referenceHashes(*fx);
        for (int32_t n : {12, 13, 11, 19, 0}) {
            FramePtr f;
            for (int i = 0; i < 5000 && !(f = e.frame(0, n)); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            QVERIFY2(f, qPrintable(QString::number(n)));
            QCOMPARE(test::frameMd5(f->av), ref[size_t(n)]);
        }
    }
};

QTEST_APPLESS_MAIN(TstImageSequence)
#include "tst_imagesequence.moc"
