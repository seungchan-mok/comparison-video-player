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

// What HwDevices recorded for the device `session` uses (or used) on this source's stream.
HwVerdict verdictFor(const IVideoSource& source, const std::string& device)
{
    const AVCodecDescriptor* d = avcodec_descriptor_get_by_name(source.info().codecName.c_str());
    return HwDevices::instance().verdict(av_hwdevice_find_type_by_name(device.c_str()), d ? d->id : AV_CODEC_ID_NONE,
                                         source.info().pixelFormat);
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

    void init() { HwDevices::instance().forgetVerdicts(); }

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
        // Hardware frames are only shown once the device has been checked against software.
        if (hwFrames)
            QCOMPARE(int(verdictFor(*o.source, o.session->hardwareDevice())), int(HwVerdict::Exact));
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

    // A device whose pictures differ from software decoding (as VideoToolbox's 8-bit HEVC does in
    // macOS VMs) is caught before it shows a frame, and GPU-first does not use it again.
    void hardwareThatDiffersIsNotUsed()
    {
        SCVP_REQUIRE_FIXTURES();
        QString name;
        std::string device;
        for (const char* n : {"h264_b", "hevc10", "vp9", "av1_svt"}) {
            Opened probe = open(QString::fromLatin1(n), HwConfig{});
            FramePtr f;
            if (probe.session && probe.session->next(f, DecodeHints{}) == DecodeStatus::Ok && f->hardware) {
                name = QString::fromLatin1(n);
                device = probe.session->hardwareDevice();
                QCOMPARE(int(verdictFor(*probe.source, device)), int(HwVerdict::Exact));
                break;
            }
        }
        if (name.isEmpty())
            QSKIP("no fixture decodes in hardware here");
        HwDevices::instance().forgetVerdicts();

        HwConfig differs;
        differs.simulateMismatch = true;
        Opened o = open(name, differs);
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
        QCOMPARE(int(verdictFor(*o.source, device)), int(HwVerdict::Differs));

        // Later GPU-first sessions pass that device over (for software, or another device that
        // gets checked in turn), after seeks too.
        Opened again = open(name, HwConfig{});
        QVERIFY(again.session->hardwareDevice() != device);
        const int32_t t = again.index->frameCount() / 2;
        QCOMPARE(int(again.session->seekTo(again.index->seekPointFor(t))), int(DecodeStatus::Ok));
        FramePtr f;
        QCOMPARE(int(again.session->next(f, DecodeHints{})), int(DecodeStatus::Ok));
        QVERIFY(again.session->hardwareDevice() != device);
        QCOMPARE(test::frameMd5AsSoftware(f->av), again.ref[size_t(f->pres)]);

        // Choosing the device explicitly still uses it: that policy accepts its output as is.
        HwConfig explicitDevice;
        explicitDevice.policy = HwPolicy::Device;
        explicitDevice.device = av_hwdevice_find_type_by_name(device.c_str());
        Opened chosen = open(name, explicitDevice);
        QCOMPARE(int(chosen.session->next(f, DecodeHints{})), int(DecodeStatus::Ok));
        QVERIFY(f->hardware);
    }
};

QTEST_GUILESS_MAIN(TstHwDecode)
#include "tst_hwdecode.moc"
