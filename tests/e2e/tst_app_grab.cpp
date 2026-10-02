#include "engine/FFmpegSource.h"
#include "render/CodeReader.h"
#include "render/ColorParams.h"
#include "support/TestSupport.h"

#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp;

namespace {

// Runs scvp on the virtual display and returns the grabbed view.
QImage grab(const QString& file, const QStringList& extra, QString* log)
{
    QTemporaryDir dir;
    const QString png = dir.filePath(QStringLiteral("grab.png"));
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList args{QStringLiteral(SCVP_APP_PATH), file, QStringLiteral("--debug-size"), QStringLiteral("640x480"),
                     QStringLiteral("--debug-grab"), png};
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
    auto s = src->openSession(SessionConfig{pool, 0}, &err);
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
        const QImage img = grab(fx->path, {QStringLiteral("--debug-seek"), QStringLiteral("100"), QStringLiteral("--debug-after"),
                                           QStringLiteral("1500")},
                                &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        QVERIFY2(log.contains(QLatin1String("(frame 100)")), qPrintable(log));
        QCOMPARE(img.size(), QSize(640, 480));

        // The window fits 320x240 at exactly 2x: every source pixel is a 2x2 block.
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
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const QColor c = img.pixelColor(2 * x + dx, 2 * y + dy);
                        const int d = std::max({std::abs(c.red() - ref[0]), std::abs(c.green() - ref[1]), std::abs(c.blue() - ref[2])});
                        worst = std::max(worst, d);
                        off += d ? 1 : 0;
                    }
            }
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
        QVERIFY(off <= 640 * 480 / 1000);
    }

    void playbackAdvances()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("hevc_opengop"));
        QString log;
        const QImage img = grab(fx->path, {QStringLiteral("--debug-play"), QStringLiteral("--debug-after"), QStringLiteral("2000")}, &log);
        if (img.isNull() && log.contains(QLatin1String("Xvfb not found")))
            QSKIP("Xvfb not available");
        QVERIFY2(!img.isNull(), qPrintable(log));
        // ~2 s at 30 fps after start-up: well past the first second, before the 6 s end.
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("\\(frame (\\d+)\\)")).match(log);
        QVERIFY2(m.hasMatch(), qPrintable(log));
        const int frame = m.captured(1).toInt();
        QVERIFY2(frame > 20 && frame < 170, qPrintable(QString::number(frame)));
    }
};

QTEST_GUILESS_MAIN(TstAppGrab)
#include "tst_app_grab.moc"
