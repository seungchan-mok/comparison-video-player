#include "engine/Engine.h"
#include "engine/RawVideoSource.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <chrono>
#include <random>
#include <thread>

using namespace scvp;

namespace {

// "pix_fmt:WxH:fps" from the fixture manifest.
RawVideoParams paramsOf(const test::Fixture& fx)
{
    const QStringList parts = fx.openArgs.split(QLatin1Char(':'));
    RawVideoParams p;
    p.format = av_get_pix_fmt(parts.value(0).toLatin1().constData());
    const QStringList wh = parts.value(1).split(QLatin1Char('x'));
    p.width = wh.value(0).toInt();
    p.height = wh.value(1).toInt();
    p.frameRate = parts.value(2).toDouble();
    return p;
}

void addRows()
{
    QTest::addColumn<QString>("name");
    for (const auto& fx : test::fixtures({QStringLiteral("rawvideo")}))
        QTest::newRow(qPrintable(fx.name)) << fx.name;
}

} // namespace

class TstRawVideo : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void parsesFileNames_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<int>("fmt");
        QTest::addColumn<double>("fps");
        QTest::newRow("underscores") << "/data/clip_1920x1080_nv12.yuv" << 1920 << 1080 << int(AV_PIX_FMT_NV12) << 0.0;
        QTest::newRow("dots + fps") << "a.1280x720.p010le.29.97fps.raw" << 1280 << 720 << int(AV_PIX_FMT_P010LE) << 29.97;
        QTest::newRow("alias p010") << "cam_3840x2160_p010_60fps.yuv" << 3840 << 2160 << int(AV_PIX_FMT_P010LE) << 60.0;
        QTest::newRow("10-bit planar") << "x_640x480_yuv420p10le.yuv" << 640 << 480 << int(AV_PIX_FMT_YUV420P10LE) << 0.0;
        QTest::newRow("i420") << "foo-352x288-i420.yuv" << 352 << 288 << int(AV_PIX_FMT_YUV420P) << 0.0;
        QTest::newRow("extension is the format") << "frames_320x240.nv12" << 320 << 240 << int(AV_PIX_FMT_NV12) << 0.0;
        QTest::newRow("bayer") << "sensor_1920x1080_bayer_rggb16le.raw" << 1920 << 1080 << int(AV_PIX_FMT_BAYER_RGGB16LE) << 0.0;
        QTest::newRow("uppercase") << "C:\\\\v\\\\CLIP_720X576_UYVY422.YUV" << 720 << 576 << int(AV_PIX_FMT_UYVY422) << 0.0;
        QTest::newRow("no format") << "clip_320x240.yuv" << 320 << 240 << int(AV_PIX_FMT_NONE) << 0.0;
        QTest::newRow("nothing") << "video.yuv" << 0 << 0 << int(AV_PIX_FMT_NONE) << 0.0;
    }

    void parsesFileNames()
    {
        QFETCH(QString, file);
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(int, fmt);
        QFETCH(double, fps);
        const RawVideoParams p = parseRawVideoName(file.toStdString());
        QCOMPARE(p.width, w);
        QCOMPARE(p.height, h);
        QCOMPARE(int(p.format), fmt);
        QCOMPARE(p.frameRate, fps);
        QVERIFY(isRawVideoExtension(file.toStdString()));
        QVERIFY(!isRawVideoExtension("movie.mp4"));
    }

    void sequentialFrames_data() { addRows(); }
    void sequentialFrames()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        const auto fx = test::fixture(name);
        std::string err;
        auto src = RawVideoSource::open(fx->path.toStdString(), paramsOf(*fx), &err);
        QVERIFY2(src, err.c_str());
        const auto ref = test::referenceHashes(*fx);
        QCOMPARE(src->index()->frameCount(), int32_t(ref.size()));
        QCOMPARE(src->index()->frameRate(), 30.0);
        QCOMPARE(src->index()->secondsOf(15), 0.5);
        auto pool = std::make_shared<FramePool>();
        auto s = src->openSession(SessionConfig{pool, 0, HwConfig{HwPolicy::Off}}, &err);
        QVERIFY(s);
        QCOMPARE(s->seekTo(src->index()->seekPointFor(0)), DecodeStatus::Ok);
        for (size_t i = 0; i < ref.size(); ++i) {
            FramePtr f;
            QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Ok);
            QCOMPARE(f->pres, int32_t(i));
            QVERIFY(f->key);
            QCOMPARE(test::frameMd5(f->av), ref[i]);
        }
        FramePtr f;
        QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Eof);
        QVERIFY(s->position().eof);
    }

    void randomFrames_data() { addRows(); }
    void randomFrames()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        const auto fx = test::fixture(name);
        std::string err;
        // Opened by name only: the fixtures are called clip_320x240_<pix_fmt>.yuv.
        auto src = openVideoSource(fx->path.toStdString(), InputOptions{}, &err);
        QVERIFY2(src, err.c_str());
        QVERIFY(src->caps().randomAccess);
        const auto ref = test::referenceHashes(*fx);
        auto pool = std::make_shared<FramePool>();
        auto s = src->openSession(SessionConfig{pool, 0, HwConfig{HwPolicy::Off}}, &err);
        std::mt19937 rng(3);
        for (int i = 0; i < 60; ++i) {
            const int32_t n = int32_t(rng() % ref.size());
            const SeekPoint sp = src->index()->seekPointFor(n);
            QCOMPARE(sp.pres, n);  // every frame is its own key
            QCOMPARE(s->seekTo(sp), DecodeStatus::Ok);
            FramePtr f;
            QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Ok);
            QCOMPARE(f->pres, n);
            QCOMPARE(test::frameMd5(f->av), ref[size_t(n)]);
        }
    }

    void headerAndTrailingBytes()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("raw_yuv420p"));
        QFile in(fx->path);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray body = in.readAll();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("hdr.yuv"));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QByteArray(100, 'h'));
        out.write(body);
        out.write(QByteArray(7, 't'));  // not a whole frame: ignored
        out.close();
        RawVideoParams p = paramsOf(*fx);
        p.headerBytes = 100;
        std::string err;
        auto src = RawVideoSource::open(path.toStdString(), p, &err);
        QVERIFY2(src, err.c_str());
        QCOMPARE(src->index()->frameCount(), fx->frames);
        auto pool = std::make_shared<FramePool>();
        auto s = src->openSession(SessionConfig{pool, 0, HwConfig{HwPolicy::Off}}, &err);
        QCOMPARE(s->seekTo(src->index()->seekPointFor(fx->frames - 1)), DecodeStatus::Ok);
        FramePtr f;
        QCOMPARE(s->next(f, DecodeHints{}), DecodeStatus::Ok);
        QCOMPARE(test::frameMd5(f->av), test::referenceHashes(*fx).back());
    }

    void unknownGeometryIsAnError()
    {
        std::string err;
        QVERIFY(!openVideoSource("/nonexistent/video.yuv", InputOptions{}, &err));
        QVERIFY(err.find("frame size") != std::string::npos);
        RawVideoParams tooBig{4096, 4096, AV_PIX_FMT_YUV420P, 30, 0};
        SCVP_REQUIRE_FIXTURES();
        QVERIFY(!RawVideoSource::open(test::fixture(QStringLiteral("raw_nv12"))->path.toStdString(), tooBig, &err));
    }

    void playsThroughTheEngine()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("raw_yuv420p10le"));
        Engine e;
        std::string err;
        QVERIFY2(e.open(0, fx->path.toStdString(), InputOptions{}, &err), err.c_str());
        PlaybackIntent in;
        in.playhead = 20;
        e.request(0, in);
        const auto ref = test::referenceHashes(*fx);
        for (int32_t n : {20, 21, 19, 29, 0}) {
            FramePtr f;
            for (int i = 0; i < 5000 && !(f = e.frame(0, n)); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            QVERIFY2(f, qPrintable(QString::number(n)));
            QCOMPARE(test::frameMd5(f->av), ref[size_t(n)]);
        }
    }
};

QTEST_APPLESS_MAIN(TstRawVideo)
#include "tst_rawvideo.moc"
