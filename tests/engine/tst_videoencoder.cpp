#include "engine/VideoEncoder.h"
#include "engine/VideoSource.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace scvp;

namespace {

// A BGRX picture with smooth gradients, a moving square and colour bars: something every
// encoder can compress well, with saturated colours that tell BT.709 from BT.601.
std::vector<uint8_t> picture(int w, int h, int n)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255 / std::max(1, w - 1));             // B
            p[1] = uint8_t(y * 255 / std::max(1, h - 1));             // G
            p[2] = uint8_t((x + y + n * 3) * 255 / std::max(1, w + h)); // R
            p[3] = 0xff;
            const int sq = (n * 2) % std::max(1, w - 16);
            if (x >= sq && x < sq + 16 && y >= 8 && y < 24)
                p[0] = p[1] = p[2] = 240;
        }
    }
    return px;
}

// Expected 4:2:0 BT.709 limited range values of a BGRX picture, the way the encoder pads it.
struct Yuv {
    int w = 0, h = 0;
    std::vector<double> y, u, v;
};
Yuv reference(const std::vector<uint8_t>& px, int w, int h)
{
    Yuv out;
    out.w = (w + 1) & ~1;
    out.h = (h + 1) & ~1;
    auto at = [&](int x, int y) { return &px[(size_t(std::min(y, h - 1)) * w + size_t(std::min(x, w - 1))) * 4]; };
    const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
    out.y.resize(size_t(out.w) * out.h);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const uint8_t* p = at(x, y);
            out.y[size_t(y) * out.w + x] = 16 + 219 * (kr * p[2] + kg * p[1] + kb * p[0]) / 255;
        }
    for (int y = 0; y < out.h / 2; ++y)
        for (int x = 0; x < out.w / 2; ++x) {
            double r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = at(2 * x + dx, 2 * y + dy);
                    b += p[0] / 4.0;
                    g += p[1] / 4.0;
                    r += p[2] / 4.0;
                }
            const double l = kr * r + kg * g + kb * b;
            out.u.push_back(128 + 224 * (b - l) / (2 * (1 - kb)) / 255);
            out.v.push_back(128 + 224 * (r - l) / (2 * (1 - kr)) / 255);
        }
    return out;
}

double psnr(const std::vector<double>& want, const AVFrame* f, int plane, int w, int h)
{
    double se = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double d = f->data[plane][size_t(y) * f->linesize[plane] + x] - want[size_t(y) * w + x];
            se += d * d;
        }
    const double mse = se / (double(w) * h);
    return mse <= 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

// B, G, R of a decoded RGB frame.
std::array<int, 3> bgrAt(const AVFrame* f, int x, int y)
{
    const auto fmt = AVPixelFormat(f->format);
    const uint8_t* row = f->data[0] + size_t(y) * f->linesize[0];
    if (fmt == AV_PIX_FMT_BGR0 || fmt == AV_PIX_FMT_BGRA)
        return {row[x * 4], row[x * 4 + 1], row[x * 4 + 2]};
    if (fmt == AV_PIX_FMT_RGB24)
        return {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
    if (fmt == AV_PIX_FMT_GBRP)  // planes G, B, R
        return {f->data[1][size_t(y) * f->linesize[1] + x], row[x], f->data[2][size_t(y) * f->linesize[2] + x]};
    return {-1, -1, -1};
}

struct Decoded {
    std::unique_ptr<IVideoSource> source;
    std::vector<FramePtr> frames;
    double frameRate = 0;
};

Decoded decodeAll(const QString& path)
{
    Decoded d;
    std::string err;
    d.source = openVideoSource(path.toStdString(), InputOptions{}, &err);
    if (!d.source || !d.source->waitForIndex(30000))
        return d;
    d.frameRate = d.source->index()->frameRate();
    auto session = d.source->openSession(SessionConfig{std::make_shared<FramePool>("check"), 0, HwConfig{HwPolicy::Off}}, &err);
    if (!session || session->seekTo(d.source->index()->seekPointFor(0)) != DecodeStatus::Ok)
        return d;
    for (;;) {
        FramePtr f;
        if (session->next(f, DecodeHints{}) != DecodeStatus::Ok)
            break;
        d.frames.push_back(f);
    }
    return d;
}

struct Written {
    QString path;   // what to open: the file, or the first PNG of the sequence
    QString error;
    int width = 0, height = 0;
};

Written encode(const QString& dir, const EncoderSettings& s, int frames, bool bottomUp = true)
{
    Written out;
    const QString path = dir + QStringLiteral("/out.") + QLatin1String(VideoEncoder::extension(s.format));
    VideoEncoder enc;
    std::string err;
    if (!enc.open(path.toStdString(), s, &err)) {
        out.error = QString::fromStdString(err);
        return out;
    }
    for (int n = 0; n < frames; ++n) {
        const std::vector<uint8_t> px = picture(s.width, s.height, n);
        const int stride = s.width * 4;
        // glReadPixels order: bottom row first in memory, so rows go up from the top one.
        std::vector<uint8_t> flipped(px.size());
        for (int y = 0; y < s.height; ++y)
            std::memcpy(&flipped[size_t(s.height - 1 - y) * stride], &px[size_t(y) * stride], size_t(stride));
        const bool ok = bottomUp ? enc.write(&flipped[size_t(s.height - 1) * stride], -stride, &err)
                                 : enc.write(px.data(), stride, &err);
        if (!ok) {
            out.error = QString::fromStdString(err);
            return out;
        }
    }
    out.width = enc.encodedWidth();
    out.height = enc.encodedHeight();
    if (!enc.finish(&err)) {
        out.error = QString::fromStdString(err);
        return out;
    }
    out.path = s.format == OutputFormat::PngSequence
                   ? QString::fromStdString(VideoEncoder::sequenceFile(path.toStdString(), s.startNumber))
                   : path;
    return out;
}

} // namespace

class TstVideoEncoder : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void namesAndBuiltInEncoders()
    {
        QCOMPARE(QString(VideoEncoder::extension(OutputFormat::H264)), QStringLiteral("mp4"));
        QCOMPARE(QString(VideoEncoder::extension(OutputFormat::Hevc)), QStringLiteral("mp4"));
        QCOMPARE(QString(VideoEncoder::extension(OutputFormat::Ffv1)), QStringLiteral("mkv"));
        QCOMPARE(QString(VideoEncoder::extension(OutputFormat::PngSequence)), QStringLiteral("png"));
        QCOMPARE(VideoEncoder::sequenceFile("/d/x.png", 12), std::string("/d/x_000012.png"));
        QCOMPARE(VideoEncoder::sequenceFile("/d/x.PNG", 3), std::string("/d/x_000003.png"));
        QCOMPARE(VideoEncoder::sequenceFile("/d.v/x", 0), std::string("/d.v/x_000000.png"));
        // Built into every FFmpeg: the lossless formats always work.
        QVERIFY(!VideoEncoder::available(OutputFormat::Ffv1).empty());
        QVERIFY(!VideoEncoder::available(OutputFormat::PngSequence).empty());
        // Hardware encoders lead the list when they work here.
        bool seenSoftware = false;
        for (const EncoderChoice& c : VideoEncoder::available(OutputFormat::H264)) {
            QVERIFY2(!(c.hardware && seenSoftware), c.name.c_str());
            seenSoftware = seenSoftware || !c.hardware;
        }
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("encoder");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        for (OutputFormat f : {OutputFormat::H264, OutputFormat::Hevc, OutputFormat::Ffv1, OutputFormat::PngSequence}) {
            for (const EncoderChoice& c : VideoEncoder::available(f)) {
                // Hardware encoders have minimum sizes well above tiny test pictures.
                for (const auto& [w, h] : {std::pair{192, 108}, std::pair{193, 109}}) {
                    const QString name = QStringLiteral("%1 %2x%3").arg(QString::fromStdString(c.name)).arg(w).arg(h);
                    QTest::newRow(qPrintable(name)) << int(f) << QString::fromStdString(c.name) << w << h;
                }
            }
        }
    }

    // Every usable encoder: the file decodes to as many pictures at the frame rate, lossless
    // formats bit-exact, lossy ones close to the BT.709 values of the pictures (padded to even).
    void roundTrip()
    {
        QFETCH(int, format);
        QFETCH(QString, encoder);
        QFETCH(int, width);
        QFETCH(int, height);
        QTemporaryDir dir;
        EncoderSettings s;
        s.format = OutputFormat(format);
        s.encoder = encoder.toStdString();
        s.width = width;
        s.height = height;
        s.frameRate = AVRational{30000, 1001};
        s.startNumber = 7;
        const int frames = 30;
        const Written w = encode(dir.path(), s, frames);
        QVERIFY2(w.error.isEmpty(), qPrintable(w.error));
        const bool lossless = VideoEncoder::lossless(s.format);
        QCOMPARE(w.width, lossless ? width : (width + 1) & ~1);
        QCOMPARE(w.height, lossless ? height : (height + 1) & ~1);

        const Decoded d = decodeAll(w.path);
        QCOMPARE(int(d.frames.size()), frames);
        if (s.format != OutputFormat::PngSequence)  // an image sequence has no timing of its own
            QVERIFY2(std::abs(d.frameRate - 30000.0 / 1001.0) < 0.01, qPrintable(QString::number(d.frameRate)));
        for (int n = 0; n < frames; ++n) {
            const AVFrame* f = d.frames[size_t(n)]->av;
            QCOMPARE(f->width, w.width);
            QCOMPARE(f->height, w.height);
            const std::vector<uint8_t> px = picture(width, height, n);
            if (lossless) {
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const uint8_t* p = &px[(size_t(y) * width + x) * 4];
                        const std::array<int, 3> want{p[0], p[1], p[2]};
                        if (bgrAt(f, x, y) != want)
                            QFAIL(qPrintable(QStringLiteral("frame %1 pixel %2,%3 differs").arg(n).arg(x).arg(y)));
                    }
                continue;
            }
            QCOMPARE(AVPixelFormat(f->format), AV_PIX_FMT_YUV420P);
            QCOMPARE(f->colorspace, AVCOL_SPC_BT709);
            QCOMPARE(f->color_range, AVCOL_RANGE_MPEG);
            const Yuv ref = reference(px, width, height);
            const double py = psnr(ref.y, f, 0, ref.w, ref.h);
            const double pu = psnr(ref.u, f, 1, ref.w / 2, ref.h / 2);
            const double pv = psnr(ref.v, f, 2, ref.w / 2, ref.h / 2);
            QVERIFY2(py > 38 && pu > 38 && pv > 38,
                     qPrintable(QStringLiteral("frame %1: PSNR Y %2 U %3 V %4").arg(n).arg(py).arg(pu).arg(pv)));
        }
    }

    // Solid colours land on their BT.709 limited range codes (BT.601 would give red Y = 81).
    void coloursAreBt709()
    {
        const auto list = VideoEncoder::available(OutputFormat::H264);
        if (list.empty())
            QSKIP("no H.264 encoder in this FFmpeg build");
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/c.mp4");
        EncoderSettings s;
        s.width = 192;
        s.height = 108;
        s.frameRate = AVRational{25, 1};
        VideoEncoder enc;
        std::string err;
        QVERIFY2(enc.open(path.toStdString(), s, &err), err.c_str());
        struct Colour {
            uint8_t b, g, r;
            int y, u, v;
        };
        const Colour colours[] = {{0, 0, 255, 63, 102, 240}, {0, 255, 0, 173, 42, 26}, {255, 0, 0, 32, 240, 118},
                                  {255, 255, 255, 235, 128, 128}, {0, 0, 0, 16, 128, 128}};
        for (const Colour& c : colours) {
            std::vector<uint8_t> px(size_t(s.width) * s.height * 4);
            for (size_t i = 0; i < px.size(); i += 4) {
                px[i] = c.b;
                px[i + 1] = c.g;
                px[i + 2] = c.r;
            }
            for (int k = 0; k < 5; ++k)
                QVERIFY2(enc.write(px.data(), s.width * 4, &err), err.c_str());
        }
        QVERIFY2(enc.finish(&err), err.c_str());
        const Decoded d = decodeAll(path);
        QCOMPARE(int(d.frames.size()), 25);
        for (int i = 0; i < 5; ++i) {
            const AVFrame* f = d.frames[size_t(i * 5 + 2)]->av;
            const int y = f->data[0][50 * f->linesize[0] + 90];
            const int u = f->data[1][25 * f->linesize[1] + 45];
            const int v = f->data[2][25 * f->linesize[2] + 45];
            const Colour& c = colours[i];
            QVERIFY2(std::abs(y - c.y) <= 2 && std::abs(u - c.u) <= 2 && std::abs(v - c.v) <= 2,
                     qPrintable(QStringLiteral("colour %1: %2 %3 %4, want %5 %6 %7").arg(i).arg(y).arg(u).arg(v)
                                    .arg(c.y).arg(c.u).arg(c.v)));
        }
    }

    // Top-down rows (positive stride) work as well as the read-back's bottom-up ones.
    void topDownRows()
    {
        QTemporaryDir dir;
        EncoderSettings s;
        s.format = OutputFormat::Ffv1;
        s.width = 64;
        s.height = 48;
        const Written w = encode(dir.path(), s, 3, false);
        QVERIFY2(w.error.isEmpty(), qPrintable(w.error));
        const Decoded d = decodeAll(w.path);
        QCOMPARE(int(d.frames.size()), 3);
        const std::vector<uint8_t> px = picture(64, 48, 2);
        const uint8_t* p = &px[(size_t(5) * 64 + 7) * 4];
        QCOMPARE(bgrAt(d.frames[2]->av, 7, 5), (std::array<int, 3>{p[0], p[1], p[2]}));
    }

    // Cancelling leaves nothing behind and keeps the file an export would have replaced.
    void abortKeepsTheOldFile_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("ffv1") << int(OutputFormat::Ffv1);
        QTest::newRow("png") << int(OutputFormat::PngSequence);
    }
    void abortKeepsTheOldFile()
    {
        QFETCH(int, format);
        QTemporaryDir dir;
        EncoderSettings s;
        s.format = OutputFormat(format);
        s.width = 64;
        s.height = 48;
        s.startNumber = 3;
        const QString path = dir.path() + QStringLiteral("/old.") + QLatin1String(VideoEncoder::extension(s.format));
        {
            QFile old(path);
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("previous export");
        }
        {
            VideoEncoder enc;
            std::string err;
            QVERIFY2(enc.open(path.toStdString(), s, &err), err.c_str());
            const std::vector<uint8_t> px = picture(64, 48, 0);
            for (int n = 0; n < 4; ++n)
                QVERIFY2(enc.write(px.data(), 64 * 4, &err), err.c_str());
            if (s.format != OutputFormat::PngSequence)  // PNGs appear as frame threads finish them
                QVERIFY(QDir(dir.path()).entryList(QDir::Files).size() > 1);  // the ".part" file
            enc.abort();
        }
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("old.") +
                                                                     QLatin1String(VideoEncoder::extension(s.format))});
        QFile old(path);
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(old.readAll(), QByteArray("previous export"));

        // Destroyed without finish(): the same.
        {
            VideoEncoder enc;
            std::string err;
            QVERIFY(enc.open(path.toStdString(), s, &err));
            const std::vector<uint8_t> px = picture(64, 48, 0);
            QVERIFY(enc.write(px.data(), 64 * 4, &err));
        }
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);
    }

    void finishReplacesTheOldFile()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/v.mkv");
        {
            QFile old(path);
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("previous export");
        }
        EncoderSettings s;
        s.format = OutputFormat::Ffv1;
        s.width = 64;
        s.height = 48;
        VideoEncoder enc;
        std::string err;
        QVERIFY(enc.open(path.toStdString(), s, &err));
        const std::vector<uint8_t> px = picture(64, 48, 0);
        QVERIFY(enc.write(px.data(), 64 * 4, &err));
        QVERIFY2(enc.finish(&err), err.c_str());
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("v.mkv")});
        QCOMPARE(int(decodeAll(path).frames.size()), 1);
    }

    void invalidSettingsFail()
    {
        QTemporaryDir dir;
        const std::string path = (dir.path() + QStringLiteral("/x.mkv")).toStdString();
        VideoEncoder enc;
        std::string err;
        EncoderSettings s;
        s.format = OutputFormat::Ffv1;
        QVERIFY(!enc.open(path, s, &err));  // no size
        QVERIFY(!err.empty());
        s.width = 64;
        s.height = 48;
        s.encoder = "no_such_encoder";
        err.clear();
        QVERIFY(!enc.open(path, s, &err));
        QVERIFY(QString::fromStdString(err).contains(QLatin1String("no_such_encoder")));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstVideoEncoder)
#include "tst_videoencoder.moc"
