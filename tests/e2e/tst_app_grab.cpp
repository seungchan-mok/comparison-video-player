#include "engine/FFmpegSource.h"
#include "render/CodeReader.h"
#include "render/CompareLayout.h"
#include "render/ColorParams.h"
#include "render/ViewTransform.h"
#include "support/TestSupport.h"

#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp;

namespace {

// Runs scvp on the virtual display and returns the grabbed view; with `copy`, also the image a
// copy_view action put on the clipboard (its PNG).
QImage grab(const QStringList& files, const QStringList& extra, QString* log, QImage* copy = nullptr)
{
    QTemporaryDir dir;
    const QString png = dir.filePath(QStringLiteral("grab.png"));
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // Never touch the user's real settings.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), dir.filePath(QStringLiteral("config")));
    p.setProcessEnvironment(env);
    QStringList args{QStringLiteral(SCVP_APP_PATH)};
    args += files;
    args += {QStringLiteral("--debug-size"), QStringLiteral("700x600"), QStringLiteral("--debug-grab"), png};
    const QString copied = dir.filePath(QStringLiteral("copy.png"));
    if (copy)
        args += {QStringLiteral("--debug-clipboard"), copied};
    args += extra;
    p.start(QStringLiteral(SCVP_XVFB_RUN), args);
    p.waitForFinished(60000);
    *log = QString::fromLocal8Bit(p.readAll());
    if (copy)
        *copy = QImage(copied).convertToFormat(QImage::Format_RGB888);
    return QImage(png).convertToFormat(QImage::Format_RGB888);
}

// The bounding box of everything that is not the view's background.
QRect contentRect(const QImage& img)
{
    QRect r;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y) != qRgb(30, 30, 32))
                r |= QRect(x, y, 1, 1);
    return r;
}

int countColor(const QImage& img, QRect r, QColor want)
{
    int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            n += std::abs(c.red() - want.red()) + std::abs(c.green() - want.green()) + std::abs(c.blue() - want.blue()) < 12 ? 1 : 0;
        }
    return n;
}

FramePtr decodeFrame(const QString& path, int32_t n)
{
    std::string err;
    auto src = openVideoSource(path.toStdString(), InputOptions{}, &err);
    if (!src || !src->waitForIndex(20000))
        return nullptr;
    auto pool = std::make_shared<FramePool>();
    auto s = src->openSession(SessionConfig{pool, 0, HwConfig{HwPolicy::Off}}, &err);
    FrameIndexPtr idx = src->index();
    s->seekTo(idx->seekPointFor(n));
    for (;;) {
        FramePtr f;
        if (s->next(f, DecodeHints{}) != DecodeStatus::Ok)
            return nullptr;
        if (f->pres == n)
            return f;
    }
}

} // namespace

class TstAppGrab : public QObject {
    Q_OBJECT

private slots:
    void seekShowsExactFrame()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("h264_b"));
        QString log;
        // Smooth scaling off (N): at the fractional fit zoom every screen pixel shows exactly one
        // source pixel.
        const QImage img = grab({fx->path}, {QStringLiteral("--debug-seek"), QStringLiteral("100"), QStringLiteral("--debug-actions"),
                                           QStringLiteral("smooth_scaling"), QStringLiteral("--debug-after"), QStringLiteral("1500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QLatin1String("(frame 100)")), qPrintable(log));

        // Same fit the view computes for its size: the image fills the view in one direction.
        render::ViewTransform vt;
        vt.setViewportSize(QSizeF(img.size()));
        vt.setImageSize(QSizeF(320, 240));
        QCOMPARE(vt.scale(), std::min(img.width() / 320.0, img.height() / 240.0));
        const QPointF t = vt.renderTranslation();
        QVERIFY(t.x() == 0 || t.y() == 0);
        FramePtr f = decodeFrame(fx->path, 100);
        QVERIFY(f);
        const auto layout = *render::PixelLayout::of(f->format());
        const auto color = render::ColorParams::resolve(layout, f->av->colorspace, f->av->color_range, f->height());
        const QPointF end = vt.imageToDevice(QPointF(320, 240));
        int off = 0, worst = 0, checked = 0;
        for (int y = int(t.y()); y < int(end.y()); ++y)
            for (int x = int(t.x()); x < int(end.x()); ++x) {
                const QPointF i = vt.deviceToImage(QPointF(x, y));
                // GPU and CPU float math may disagree right on a pixel boundary: skip those.
                const double fx_ = i.x() - std::floor(i.x()), fy_ = i.y() - std::floor(i.y());
                if (fx_ < 1e-3 || fx_ > 1 - 1e-3 || fy_ < 1e-3 || fy_ > 1 - 1e-3 || i.x() >= 320 || i.y() >= 240)
                    continue;
                const auto k = render::readCodes(f->av, layout, int(i.x()), int(i.y()));
                const auto ref = color.toRgb(k[0], k[1], k[2]);
                const QColor c = img.pixelColor(x, y);
                const int d = std::max({std::abs(c.red() - ref[0]), std::abs(c.green() - ref[1]), std::abs(c.blue() - ref[2])});
                worst = std::max(worst, d);
                off += d ? 1 : 0;
                ++checked;
            }
        QVERIFY(checked > 320 * 240);
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
        QVERIFY(off <= checked / 1000);
    }

    void playbackAdvances()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("hevc_opengop"));
        QString log;
        const QImage img = grab({fx->path}, {QStringLiteral("--debug-play"), QStringLiteral("--debug-after"), QStringLiteral("2000")}, &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        // ~2 s at 30 fps after start-up: well past the first second, before the 6 s end.
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("\\(frame (\\d+)\\)")).match(log);
        QVERIFY2(m.hasMatch(), qPrintable(log));
        const int frame = m.captured(1).toInt();
        QVERIFY2(frame > 20 && frame < 170, qPrintable(QString::number(frame)));
    }

    // A and a copy without its first 3 frames: at offset -3 every pair is the same picture, so
    // the difference view is black and the split panes are identical; any other offset is not.
    void syncedDifferenceIsZero_data()
    {
        QTest::addColumn<int>("offset");
        QTest::addColumn<int>("seek");
        QTest::addColumn<bool>("zero");
        QTest::newRow("offset -3, frame 3") << -3 << 3 << true;
        QTest::newRow("offset -3, frame 60") << -3 << 60 << true;
        QTest::newRow("offset -3, last frame") << -3 << 119 << true;
        QTest::newRow("offset -2") << -2 << 60 << false;
        QTest::newRow("offset -3, B held at frame 0") << -3 << 1 << false;
    }

    void syncedDifferenceIsZero()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(int, offset);
        QFETCH(int, seek);
        QFETCH(bool, zero);
        const auto a = test::fixture(QStringLiteral("ffv1_10bit"));
        const auto b = test::fixture(QStringLiteral("ffv1_10bit_trim3"));
        QVERIFY(a && b);
        QString log;
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--offset"), QString::number(offset), QStringLiteral("--view"), QStringLiteral("diff"),
                                 QStringLiteral("--debug-seek"), QString::number(seek), QStringLiteral("--debug-after"), QStringLiteral("2000")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("(frame %1)").arg(seek)), qPrintable(log));
        render::ViewTransform vt;
        vt.setViewportSize(QSizeF(img.size()));
        vt.setImageSize(QSizeF(320, 240));
        const QPoint t = vt.renderTranslation().toPoint();
        const int w = int(320 * vt.scale()), h = int(240 * vt.scale());
        int nonzero = 0;
        for (int y = t.y(); y < t.y() + h; ++y)
            for (int x = t.x(); x < t.x() + w; ++x) {
                const QColor c = img.pixelColor(x, y);
                nonzero += (c.red() | c.green() | c.blue()) ? 1 : 0;
            }
        if (zero)
            QCOMPARE(nonzero, 0);
        else
            QVERIFY2(nonzero > w * h / 4, qPrintable(QString::number(nonzero)));
    }

    void splitPanesMatchWhenSynced()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto a = test::fixture(QStringLiteral("ffv1_10bit"));
        const auto b = test::fixture(QStringLiteral("ffv1_10bit_trim3"));
        QString log;
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--offset"), QStringLiteral("-3"), QStringLiteral("--view"), QStringLiteral("sbs"),
                                 QStringLiteral("--debug-seek"), QStringLiteral("40"), QStringLiteral("--debug-after"), QStringLiteral("2000")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        const int paneW = (img.width() - render::CompareLayout::kDivider) / 2;
        const int bx = img.width() - paneW;
        int diff = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < paneW; ++x)
                diff += img.pixel(x, y) != img.pixel(bx + x, y) ? 1 : 0;
        QCOMPARE(diff, 0);
        // And the panes show the video, not just background.
        render::ViewTransform vt;
        vt.setViewportSize(QSizeF(paneW, img.height()));
        vt.setImageSize(QSizeF(320, 240));
        const QPointF c = vt.imageToDevice(QPointF(160, 120));
        QVERIFY(img.pixelColor(int(c.x()), int(c.y())) != QColor(30, 30, 32));
    }

    // A raw .yuv opens by its name (clip_320x240_nv12.yuv) and shows the exact frame.
    void rawVideoOpensByName()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("raw_nv12"));
        QString log;
        const QImage img = grab({fx->path},
                                {QStringLiteral("--debug-seek"), QStringLiteral("17"), QStringLiteral("--debug-hover"), QStringLiteral("100,100"),
                                 QStringLiteral("--debug-after"), QStringLiteral("1500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QLatin1String("(frame 17)")), qPrintable(log));
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("debug-inspector: A \\((\\d+),(\\d+)\\)  r \\d+ g \\d+ b \\d+  y (\\d+) u (\\d+) v (\\d+)")).match(log);
        QVERIFY2(m.hasMatch(), qPrintable(log));
        // Codes straight from the file: Y plane then interleaved UV.
        QFile f(fx->path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const int x = m.captured(1).toInt(), y = m.captured(2).toInt();
        const qint64 frame = 17LL * 320 * 240 * 3 / 2;
        f.seek(frame + y * 320 + x);
        const int Y = uchar(f.read(1)[0]);
        f.seek(frame + 320 * 240 + (y / 2) * 320 + (x / 2) * 2);
        const QByteArray uv = f.read(2);
        QCOMPARE(m.captured(3).toInt(), Y);
        QCOMPARE(m.captured(4).toInt(), int(uchar(uv[0])));
        QCOMPARE(m.captured(5).toInt(), int(uchar(uv[1])));
    }

    // L: each pane names its video with a coloured chip (A orange, B teal).
    void legendMarksBothVideos()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto a = test::fixture(QStringLiteral("h264_b"));
        const auto b = test::fixture(QStringLiteral("hevc_opengop"));
        QString log;
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--view"), QStringLiteral("sbs"), QStringLiteral("--debug-actions"), QStringLiteral("legend"),
                                 QStringLiteral("--debug-grab-window"), QStringLiteral("--debug-after"), QStringLiteral("1500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        auto count = [&](QRect r, QColor want) {
            int n = 0;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    n += std::abs(c.red() - want.red()) + std::abs(c.green() - want.green()) + std::abs(c.blue() - want.blue()) < 12 ? 1 : 0;
                }
            return n;
        };
        const QRect leftTop(0, 0, img.width() / 2, img.height() / 3), rightTop(img.width() / 2, 0, img.width() / 2, img.height() / 3);
        const QColor orange(255, 138, 61), teal(41, 182, 197);
        QVERIFY2(count(leftTop, orange) > 150, qPrintable(QString::number(count(leftTop, orange))));
        QVERIFY2(count(rightTop, teal) > 150, qPrintable(QString::number(count(rightTop, teal))));
        QVERIFY(count(leftTop, teal) < 20);
    }

    // --legend-a/--legend-b name the videos in the legend and turn it on; the names follow their
    // videos when A and B are swapped.
    void legendShowsUserText()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto a = test::fixture(QStringLiteral("h264_b"));
        const auto b = test::fixture(QStringLiteral("hevc_opengop"));
        QString log;
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--view"), QStringLiteral("sbs"), QStringLiteral("--legend-a"), QStringLiteral("기준 x264"),
                                 QStringLiteral("--legend-b"), QStringLiteral(" new (crf 20) "), QStringLiteral("--debug-actions"),
                                 QStringLiteral("swap_ab"), QStringLiteral("--debug-grab-window"), QStringLiteral("--debug-after"),
                                 QStringLiteral("1500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("debug-legend: on, A \"new (crf 20)\", B \"기준 x264\"")), qPrintable(log));
        int orange = 0;  // the legend is on screen without pressing L: A's chip, top left
        for (int y = 0; y < img.height() / 3; ++y)
            for (int x = 0; x < img.width() / 2; ++x) {
                const QColor c = img.pixelColor(x, y);
                orange += std::abs(c.red() - 255) + std::abs(c.green() - 138) + std::abs(c.blue() - 61) < 12 ? 1 : 0;
            }
        QVERIFY2(orange > 150, qPrintable(QString::number(orange)));
    }

    // Ctrl+C copies the view as it is on screen: here (magnified, one video, nothing over it) the
    // copy is the screen's framebuffer, pixel for pixel.
    void copyViewIsTheScreen()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("h264_b"));
        QString log;
        QImage copy;
        const QImage img = grab({fx->path}, {QStringLiteral("--debug-actions"), QStringLiteral("copy_view"), QStringLiteral("--debug-after"),
                                           QStringLiteral("2500")},
                                &log, &copy);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull() && !copy.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QLatin1String("factor 1.000000")), qPrintable(log));
        QCOMPARE(copy.size(), img.size());
        QVERIFY(copy == img);
    }

    // Zoomed out below 1:1, the copy is drawn larger so that the video is 1:1 in it: the same
    // pixels as a 1:1 view.
    void copyViewShowsMinifiedVideoAtFullDetail()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("h264_b"));
        QString log;
        QImage copy;
        const QImage small = grab({fx->path},
                                  {QStringLiteral("--debug-actions"), QStringLiteral("zoom_out,zoom_out,zoom_out,zoom_out,copy_view"),
                                   QStringLiteral("--debug-after"), QStringLiteral("2500")},
                                  &log, &copy);
        if (small.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!copy.isNull(), qPrintable(log));
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("debug-copy: saved (\\d+)x(\\d+) factor ([0-9.]+)")).match(log);
        QVERIFY2(m.hasMatch(), qPrintable(log));
        QVERIFY2(m.captured(3).toDouble() > 1.3, qPrintable(log));  // 1:1 over 0.71x on screen
        QCOMPARE(copy.size(), QSize(m.captured(1).toInt(), m.captured(2).toInt()));
        QVERIFY(copy.width() > small.width());
        QString log1;
        const QImage oneToOne = grab({fx->path}, {QStringLiteral("--debug-actions"), QStringLiteral("zoom_1to1")}, &log1);
        QVERIFY2(!oneToOne.isNull(), qPrintable(log1));
        const QRect a = contentRect(copy), b = contentRect(oneToOne);
        QCOMPARE(a.size(), QSize(320, 240));
        QCOMPARE(b.size(), QSize(320, 240));
        QVERIFY(copy.copy(a) == oneToOne.copy(b));
    }

    // With the mouse over the video, the copy shows what the screen shows: the cursor pixel's
    // outline and, from the overlay, the pixel values line.
    void copyViewIncludesTheCursorPixel()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("h264_b"));
        QString log;
        QImage copy;
        // A view fitting the 320x240 video at 4x: the outline shows from 4 device px per pixel.
        const QImage img = grab({fx->path},
                                {QStringLiteral("--debug-size"), QStringLiteral("1400x1060"), QStringLiteral("--debug-hover"), QStringLiteral("700,500"),
                                 QStringLiteral("--debug-actions"), QStringLiteral("copy_view"), QStringLiteral("--debug-after"), QStringLiteral("2500")},
                                &log, &copy);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!copy.isNull(), qPrintable(log));
        QCOMPARE(copy.size(), img.size());
        QVERIFY2(img.height() > 900, qPrintable(log));
        int outline = 0;  // black on a bright pixel, amber on a dark one (display.frag)
        for (int y = 495; y <= 505; ++y)
            for (int x = 695; x <= 705; ++x)
                outline += copy.pixel(x, y) == qRgb(0, 0, 0) || copy.pixel(x, y) == qRgb(255, 217, 51) ? 1 : 0;
        QVERIFY2(outline >= 8, qPrintable(QString::number(outline)));
        // The framebuffer (no overlay) equals the copy above the values line, and not on it.
        const int line = 40;
        QVERIFY(copy.copy(0, 0, copy.width(), copy.height() - line) == img.copy(0, 0, img.width(), img.height() - line));
        QVERIFY(copy.copy(0, copy.height() - 20, 200, 20) != img.copy(0, img.height() - 20, 200, 20));
    }

    // The overlay comes along, drawn at the copy's resolution: the legend chips of both videos,
    // larger by the copy's factor.
    void copyViewIncludesTheLegend()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto a = test::fixture(QStringLiteral("h264_b"));
        const auto b = test::fixture(QStringLiteral("hevc_opengop"));
        QString log;
        QImage copy;
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--view"), QStringLiteral("sbs"), QStringLiteral("--legend-a"), QStringLiteral("before"),
                                 QStringLiteral("--debug-actions"), QStringLiteral("zoom_out,zoom_out,copy_view"), QStringLiteral("--debug-grab-window"),
                                 QStringLiteral("--debug-after"), QStringLiteral("2500")},
                                &log, &copy);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!copy.isNull(), qPrintable(log));
        const QColor orange(255, 138, 61), teal(41, 182, 197);
        const QRect leftTop(0, 0, copy.width() / 2, copy.height() / 3), rightTop(copy.width() / 2, 0, copy.width() / 2, copy.height() / 3);
        const int chipA = countColor(copy, leftTop, orange);
        QVERIFY2(chipA > 150, qPrintable(QString::number(chipA)));
        QVERIFY(countColor(copy, rightTop, teal) > 150);
        QVERIFY(countColor(copy, leftTop, teal) < 20);
        // The window shows the chip at 1x; the copy at ~1.19x: ~1.41x the pixels.
        const int onScreen = countColor(img, img.rect(), orange);
        QVERIFY2(chipA > onScreen * 1.2 && chipA < onScreen * 1.7, qPrintable(QStringLiteral("%1 vs %2").arg(chipA).arg(onScreen)));
    }

    // Flipping shows a large chip of the video now on screen in the middle of the view.
    void flipShowsWhichVideo()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto a = test::fixture(QStringLiteral("h264_b"));
        const auto b = test::fixture(QStringLiteral("hevc_opengop"));
        QString log;
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--debug-actions"), QStringLiteral("view_flip"), QStringLiteral("--debug-grab-window"),
                                 QStringLiteral("--debug-after"), QStringLiteral("500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        int teal = 0;
        const QRect mid(img.width() / 3, img.height() / 4, img.width() / 3, img.height() / 2);
        for (int y = mid.top(); y <= mid.bottom(); ++y)
            for (int x = mid.left(); x <= mid.right(); ++x) {
                const QColor c = img.pixelColor(x, y);
                teal += std::abs(c.red() - 41) + std::abs(c.green() - 182) + std::abs(c.blue() - 197) < 12 ? 1 : 0;
            }
        QVERIFY2(teal > 2000, qPrintable(QString::number(teal)));  // entering flip shows B first
    }

    // Hovering reads the pixel back from the GPU: synced A and B agree and match the decoder.
    void inspectorReadsThePixelUnderTheCursor()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto a = test::fixture(QStringLiteral("ffv1_10bit"));
        const auto b = test::fixture(QStringLiteral("ffv1_10bit_trim3"));
        QString log;
        // 700x600 window: view fit is integral; hover the view centre.
        const QImage img = grab({a->path, b->path},
                                {QStringLiteral("--offset"), QStringLiteral("-3"), QStringLiteral("--view"), QStringLiteral("wipe"),
                                 QStringLiteral("--debug-seek"), QStringLiteral("40"), QStringLiteral("--debug-hover"), QStringLiteral("100,100"),
                                 QStringLiteral("--debug-after"), QStringLiteral("2000")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("debug-inspector: A \\((\\d+),(\\d+)\\)  r (\\d+) g (\\d+) b (\\d+)  y (\\d+) u (\\d+) v (\\d+)"
                                              "   \\|   B \\((\\d+),(\\d+)\\)  r (\\d+) g (\\d+) b (\\d+).*A-B  r 0 g 0 b 0"))
                .match(log);
        QVERIFY2(m.hasMatch(), qPrintable(log));
        const int x = m.captured(1).toInt(), y = m.captured(2).toInt();
        QCOMPARE(m.captured(9).toInt(), x);
        QCOMPARE(m.captured(10).toInt(), y);
        render::ViewTransform vt;
        vt.setViewportSize(QSizeF(img.size()));
        vt.setImageSize(QSizeF(320, 240));
        const QPointF expect = vt.deviceToImage(QPointF(100, 100));
        QCOMPARE(x, int(expect.x()));
        QCOMPARE(y, int(expect.y()));
        // Codes are the decoder's (frame 40 of A), RGB the CPU reference conversion.
        FramePtr f = decodeFrame(a->path, 40);
        QVERIFY(f);
        const auto layout = *render::PixelLayout::of(f->format());
        const auto codes = render::readCodes(f->av, layout, x, y);
        const auto color = render::ColorParams::resolve(layout, f->av->colorspace, f->av->color_range, f->height());
        const auto rgb = color.toRgb(codes[0], codes[1], codes[2]);
        for (int c = 0; c < 3; ++c) {
            QCOMPARE(m.captured(6 + c).toUInt(), codes[size_t(c)]);
            QVERIFY(std::abs(m.captured(3 + c).toInt() - rgb[size_t(c)]) <= 1);
        }
    }
};

QTEST_GUILESS_MAIN(TstAppGrab)
#include "tst_app_grab.moc"
