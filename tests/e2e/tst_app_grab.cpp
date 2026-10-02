#include "engine/FFmpegSource.h"
#include "render/CodeReader.h"
#include "render/CompareLayout.h"
#include "render/ColorParams.h"
#include "render/ViewTransform.h"
#include "support/TestSupport.h"

#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp;

namespace {

// Runs scvp on the virtual display and returns the grabbed view.
QImage grab(const QStringList& files, const QStringList& extra, QString* log)
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
    args += extra;
    p.start(QStringLiteral(SCVP_XVFB_RUN), args);
    p.waitForFinished(60000);
    *log = QString::fromLocal8Bit(p.readAll());
    return QImage(png).convertToFormat(QImage::Format_RGB888);
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
        const QImage img = grab({fx->path}, {QStringLiteral("--debug-seek"), QStringLiteral("100"), QStringLiteral("--debug-after"),
                                           QStringLiteral("1500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QLatin1String("(frame 100)")), qPrintable(log));

        // Same fit the view computes for its size; every source pixel is an s x s block.
        render::ViewTransform vt;
        vt.setViewportSize(QSizeF(img.size()));
        vt.setImageSize(QSizeF(320, 240));
        const int s = int(vt.scale());
        QVERIFY2(s >= 1 && double(s) == vt.scale(), qPrintable(QString::number(vt.scale())));
        const QPoint t = vt.renderTranslation().toPoint();
        FramePtr f = decodeFrame(fx->path, 100);
        QVERIFY(f);
        const auto layout = *render::PixelLayout::of(f->format());
        const auto color = render::ColorParams::resolve(layout, f->av->colorspace, f->av->color_range, f->height());
        int off = 0;
        int worst = 0;
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x) {
                const auto k = render::readCodes(f->av, layout, x, y);
                const auto ref = color.toRgb(k[0], k[1], k[2]);
                for (int dy = 0; dy < s; ++dy)
                    for (int dx = 0; dx < s; ++dx) {
                        const QColor c = img.pixelColor(t.x() + s * x + dx, t.y() + s * y + dy);
                        const int d = std::max({std::abs(c.red() - ref[0]), std::abs(c.green() - ref[1]), std::abs(c.blue() - ref[2])});
                        worst = std::max(worst, d);
                        off += d ? 1 : 0;
                    }
            }
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
        QVERIFY(off <= 320 * 240 * s * s / 1000);
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
};

QTEST_GUILESS_MAIN(TstAppGrab)
#include "tst_app_grab.moc"
