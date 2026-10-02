#include "engine/FFmpegSource.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <random>

using namespace scvp;

namespace {

bool haveHardware()
{
    for (AVHWDeviceType t : HwDevices::platformOrder())
        if (HwDevices::instance().available(t))
            return true;
    return false;
}

struct Opened {
    std::unique_ptr<IVideoSource> source;
    std::shared_ptr<FramePool> pool;
    std::unique_ptr<IDecodeSession> session;
    FrameIndexPtr index;
    std::vector<std::string> ref;
};

Opened open(const QString& name, HwConfig hw)
{
    Opened o;
    const auto fx = test::fixture(name);
    std::string err;
    o.source = openVideoSource(fx->path.toStdString(), InputOptions{}, &err);
    o.source->waitForIndex(30000);
    o.pool = std::make_shared<FramePool>();
    o.session = o.source->openSession(SessionConfig{o.pool, 0, hw}, &err);
    o.index = o.source->index();
    o.ref = test::referenceHashes(*fx);
    return o;
}

void addRows()
{
    QTest::addColumn<QString>("name");
    for (const auto& fx : test::fixtures({QStringLiteral("container"), QStringLiteral("rawes")}))
        if (fx.name != QLatin1String("h264_b_avi"))
            QTest::newRow(qPrintable(fx.name)) << fx.name;
}

} // namespace

class TstHwDecode : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        ff::installLogCallback(AV_LOG_QUIET);
        if (!haveHardware())
            QSKIP("no hardware decoder on this machine");
    }

    void sequential_data() { addRows(); }
    void sequential()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        Opened o = open(name, HwConfig{});
        QVERIFY(o.session);
        int32_t expected = 0;
        int hwFrames = 0;
        for (;;) {
            FramePtr f;
            const DecodeStatus st = o.session->next(f, DecodeHints{});
            if (st == DecodeStatus::Eof)
                break;
            QCOMPARE(int(st), int(DecodeStatus::Ok));
            QCOMPARE(f->pres, expected);
            QVERIFY2(test::frameMd5AsSoftware(f->av) == o.ref[size_t(expected)],
                     qPrintable(QStringLiteral("frame %1 (%2, %3)").arg(expected).arg(av_get_pix_fmt_name(f->format()),
                                                                                       f->hardware ? "hw" : "sw")));
            hwFrames += f->hardware ? 1 : 0;
            ++expected;
        }
        QCOMPARE(expected, int32_t(o.ref.size()));
        qInfo("%s: %d/%d frames decoded by %s", qPrintable(name), hwFrames, expected,
              hwFrames ? o.session->hardwareDevice().c_str() : "software");
    }

    void randomAccess_data() { addRows(); }
    void randomAccess()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        Opened o = open(name, HwConfig{});
        std::mt19937 rng(77);
        for (int i = 0; i < 30; ++i) {
            const int32_t t = int32_t(rng() % uint32_t(o.index->frameCount()));
            QCOMPARE(int(o.session->seekTo(o.index->seekPointFor(t))), int(DecodeStatus::Ok));
            for (;;) {
                FramePtr f;
                QCOMPARE(int(o.session->next(f, DecodeHints{})), int(DecodeStatus::Ok));
                QCOMPARE(test::frameMd5AsSoftware(f->av), o.ref[size_t(f->pres)]);
                if (f->pres >= t) {
                    QCOMPARE(f->pres, t);
                    break;
                }
            }
        }
    }

    void fallbackToSoftware_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* n : {"h264_b", "hevc10", "vp9", "av1_svt", "mpeg2_ts"})
            QTest::newRow(n) << QString::fromLatin1(n);
    }

    // Hardware initialisation "fails": decoding must continue in software with identical output.
    void fallbackToSoftware()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        HwConfig hw;
        hw.simulateFailure = true;
        Opened o = open(name, hw);
        QVERIFY(o.session);
        int32_t expected = 0;
        for (;;) {
            FramePtr f;
            const DecodeStatus st = o.session->next(f, DecodeHints{});
            if (st == DecodeStatus::Eof)
                break;
            QCOMPARE(int(st), int(DecodeStatus::Ok));
            QCOMPARE(f->pres, expected);
            QVERIFY(!f->hardware);
            QCOMPARE(test::frameMd5(f->av), o.ref[size_t(expected)]);
            ++expected;
        }
        QCOMPARE(expected, int32_t(o.ref.size()));
        QCOMPARE(o.session->hardwareDevice(), std::string());
    }
};

QTEST_GUILESS_MAIN(TstHwDecode)
#include "tst_hwdecode.moc"
