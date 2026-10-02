#include "engine/FfmpegCompat.h"
#include "ui/KeyMap.h"
#include "ui/MainWindow.h"

#include "ui/Player.h"
#include "ui/VideoView.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdio>

namespace {

QString ffmpegVersionString()
{
    auto v = [](unsigned int x) { return QStringLiteral("%1.%2.%3").arg(x >> 16).arg((x >> 8) & 0xff).arg(x & 0xff); };
    return QStringLiteral("FFmpeg %1 (avcodec %2, avformat %3, avutil %4, swscale %5)")
        .arg(QString::fromUtf8(av_version_info()), v(avcodec_version()), v(avformat_version()), v(avutil_version()),
             v(swscale_version()));
}

void setDefaultSurfaceFormat()
{
    // Must happen before the QApplication exists (mandatory on macOS for a core profile).
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setProfile(QSurfaceFormat::CoreProfile);
#if defined(Q_OS_MACOS)
    f.setVersion(4, 1);
#else
    f.setVersion(3, 3);
#endif
    f.setDepthBufferSize(0);  // a fullscreen QOpenGLWidget FBO without depth/stencil saves ~85 MB at 6K
    f.setStencilBufferSize(0);
    f.setSamples(0);
    f.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(f);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}

} // namespace

int main(int argc, char* argv[])
{
    setDefaultSurfaceFormat();
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("scvp"));
    QApplication::setOrganizationName(QStringLiteral("scvp"));
    QApplication::setApplicationVersion(QStringLiteral(SCVP_VERSION));
    scvp::ff::installLogCallback(AV_LOG_ERROR);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Simple comparison video player"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("A"), QStringLiteral("Video to open (slot A)"), QStringLiteral("[A]"));
    parser.addPositionalArgument(QStringLiteral("B"), QStringLiteral("Video to compare with A (slot B)"), QStringLiteral("[B]"));
    QCommandLineOption offset(QStringLiteral("offset"), QStringLiteral("B shows frame A + N."), QStringLiteral("N"));
    parser.addOption(offset);
    QCommandLineOption sync(QStringLiteral("sync"), QStringLiteral("Pair frames by number (frame, default) or by time (time)."), QStringLiteral("mode"));
    parser.addOption(sync);
    QCommandLineOption view(QStringLiteral("view"), QStringLiteral("Comparison: split, sbs, tb, wipe, flip, diff."), QStringLiteral("mode"));
    parser.addOption(view);
    QCommandLineOption cacheMb(QStringLiteral("cache-mb"), QStringLiteral("Decoded frame cache budget for this run (MB)."), QStringLiteral("MB"));
    parser.addOption(cacheMb);
    QCommandLineOption vramMb(QStringLiteral("vram-mb"), QStringLiteral("GPU frame cache for this run (MB, 0 disables)."), QStringLiteral("MB"));
    parser.addOption(vramMb);
    QCommandLineOption hwdec(QStringLiteral("hwdec"), QStringLiteral("Decoding for this run: gpu (default), cpu, or a device (cuda, vaapi, d3d11va, videotoolbox)."), QStringLiteral("mode"));
    parser.addOption(hwdec);
    // Deterministic end-to-end checks on a virtual display (scripts/xvfb-run.sh).
    QCommandLineOption debugGrab(QStringLiteral("debug-grab"), QStringLiteral("Save the video view to PNG, then quit."), QStringLiteral("png"));
    QCommandLineOption debugAfter(QStringLiteral("debug-after"), QStringLiteral("Milliseconds before --debug-grab (default 1000)."), QStringLiteral("ms"), QStringLiteral("1000"));
    QCommandLineOption debugSeek(QStringLiteral("debug-seek"), QStringLiteral("Seek to this frame after opening."), QStringLiteral("frame"));
    QCommandLineOption debugPlay(QStringLiteral("debug-play"), QStringLiteral("Start playback after opening."));
    QCommandLineOption debugSize(QStringLiteral("debug-size"), QStringLiteral("Window size WxH."), QStringLiteral("WxH"));
    QCommandLineOption debugGrabWindow(QStringLiteral("debug-grab-window"), QStringLiteral("Grab the whole window, not just the video."));
    QCommandLineOption debugActions(QStringLiteral("debug-actions"), QStringLiteral("Comma separated actions to trigger after opening (e.g. info_overlay,fullscreen)."), QStringLiteral("ids"));
    for (const auto& o : {debugGrab, debugAfter, debugSeek, debugPlay, debugSize, debugGrabWindow, debugActions}) {
        QCommandLineOption hidden = o;
        hidden.setFlags(QCommandLineOption::HiddenFromHelp);
        parser.addOption(hidden);
    }
    parser.process(app);

    std::fprintf(stderr, "scvp %s, Qt %s, %s\n", SCVP_VERSION, qVersion(), qPrintable(ffmpegVersionString()));

    scvp::ui::MainWindow window;
    if (parser.isSet(cacheMb))
        window.overrideCacheBudget(size_t(parser.value(cacheMb).toULongLong()) << 20);
    if (parser.isSet(vramMb))
        window.overrideGpuBudget(size_t(parser.value(vramMb).toULongLong()) << 20);
    if (parser.isSet(hwdec)) {
        bool ok = false;
        const scvp::HwConfig hw = scvp::ui::AppSettings::parseHwdec(parser.value(hwdec), &ok);
        if (!ok)
            std::fprintf(stderr, "unknown --hwdec value '%s', using gpu\n", qPrintable(parser.value(hwdec)));
        window.overrideHardware(hw);
    }
    if (parser.isSet(debugSize)) {
        const QStringList wh = parser.value(debugSize).split(QLatin1Char('x'));
        if (wh.size() == 2)
            window.resize(wh[0].toInt(), wh[1].toInt());
    }
    window.show();
    const QStringList files = parser.positionalArguments();
    if (files.size() >= 2)
        window.openFile(1, files[1]);
    if (parser.isSet(sync))
        window.player()->setSyncMode(parser.value(sync) == QLatin1String("time") ? scvp::SyncMode::Timestamp : scvp::SyncMode::Frame);
    if (parser.isSet(offset))
        window.player()->setOffset(parser.value(offset).toLongLong());
    if (parser.isSet(view)) {
        scvp::render::CompareSettings c;
        const QString v = parser.value(view);
        using scvp::render::CompareMode;
        using scvp::render::SplitOrientation;
        if (v == QLatin1String("sbs"))
            c.orientation = SplitOrientation::SideBySide;
        else if (v == QLatin1String("tb"))
            c.orientation = SplitOrientation::TopBottom;
        else if (v == QLatin1String("wipe"))
            c.mode = CompareMode::Wipe;
        else if (v == QLatin1String("flip"))
            c.mode = CompareMode::Flip;
        else if (v == QLatin1String("diff"))
            c.mode = CompareMode::Difference;
        window.setCompare(c);
    }
    if (!files.isEmpty() && window.openFile(0, files[0])) {
        if (parser.isSet(debugSeek))
            window.player()->seek(parser.value(debugSeek).toLongLong());
        if (parser.isSet(debugPlay))
            window.player()->togglePlay();
    }
    if (parser.isSet(debugActions)) {
        const QStringList ids = parser.value(debugActions).split(QLatin1Char(','), Qt::SkipEmptyParts);
        QTimer::singleShot(200, &window, [&window, ids] {
            for (const QString& id : ids)
                for (int a = 0; a < int(scvp::ui::Action::Count); ++a)
                    if (id == QLatin1String(scvp::ui::KeyMap::info(scvp::ui::Action(a)).id))
                        window.handleAction(scvp::ui::Action(a), false);
        });
    }
    if (parser.isSet(debugGrab)) {
        const QString out = parser.value(debugGrab);
        const bool wholeWindow = parser.isSet(debugGrabWindow);
        QTimer::singleShot(parser.value(debugAfter).toInt(), &window, [&window, out, wholeWindow] {
            const QImage img = wholeWindow ? window.grab().toImage() : window.view()->grabFramebuffer();
            const bool ok = img.save(out);
            std::fprintf(stderr, "debug-grab: %s %dx%d -> %s (frame %lld)\n", ok ? "saved" : "FAILED", img.width(), img.height(),
                         qPrintable(out), (long long)window.player()->controller().position());
            QApplication::exit(ok ? 0 : 1);
        });
    }
    return QApplication::exec();
}
