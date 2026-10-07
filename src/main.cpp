#include "engine/FfmpegCompat.h"
#include "render/ViewTransform.h"
#include "ui/AppIcon.h"
#include "ui/CommandLine.h"
#include "ui/ExportDialog.h"
#include "ui/HudOverlay.h"
#include "ui/KeyMap.h"
#include "ui/MainWindow.h"
#include "ui/Settings.h"

#include "ui/Player.h"
#include "ui/VideoView.h"

#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

QString ffmpegVersionString()
{
    auto v = [](unsigned int x) { return QStringLiteral("%1.%2.%3").arg(x >> 16).arg((x >> 8) & 0xff).arg(x & 0xff); };
    return QStringLiteral("FFmpeg %1 (avcodec %2, avformat %3, avutil %4, swscale %5)")
        .arg(QString::fromUtf8(av_version_info()), v(avcodec_version()), v(avformat_version()), v(avutil_version()),
             v(swscale_version()));
}

void setApplicationNames()
{
    QCoreApplication::setApplicationName(QStringLiteral("scvp"));
    QCoreApplication::setOrganizationName(QStringLiteral("scvp"));  // the settings (and the keys in --help) live under it
    QCoreApplication::setApplicationVersion(QStringLiteral(SCVP_VERSION));
}

// Help, version and argument errors go to stdout / stderr, or to a message box where there is
// no console to print to (a GUI program on Windows), as QCommandLineParser does it.
void report(const QString& text, bool error)
{
#if defined(Q_OS_WIN)
    if (!GetConsoleWindow()) {
        QMessageBox box(error ? QMessageBox::Warning : QMessageBox::Information, QStringLiteral("SCVP"),
                        QStringLiteral("<pre>%1</pre>").arg(text.toHtmlEscaped()));
        box.exec();
        return;
    }
#endif
    std::FILE* out = error ? stderr : stdout;
    std::fputs(text.toUtf8().constData(), out);
    std::fflush(out);
}

// Deterministic end-to-end checks on a virtual display (scripts/xvfb-run.sh); not in --help.
void addDebugOptions(QCommandLineParser& parser)
{
    const QCommandLineOption options[] = {
        QCommandLineOption(QStringLiteral("debug-grab"), QStringLiteral("Save the video view to PNG, then quit."), QStringLiteral("png")),
        QCommandLineOption(QStringLiteral("debug-after"), QStringLiteral("Milliseconds before --debug-grab (default 1000)."),
                           QStringLiteral("ms"), QStringLiteral("1000")),
        QCommandLineOption(QStringLiteral("debug-size"), QStringLiteral("Window size WxH."), QStringLiteral("WxH")),
        QCommandLineOption(QStringLiteral("debug-grab-window"), QStringLiteral("Grab the whole window, not just the video.")),
        QCommandLineOption(QStringLiteral("debug-actions"),
                           QStringLiteral("Comma separated actions to trigger after opening (e.g. info_overlay,fullscreen)."),
                           QStringLiteral("ids")),
        QCommandLineOption(QStringLiteral("debug-hover"), QStringLiteral("Move the mouse over the view at X,Y (logical px)."),
                           QStringLiteral("x,y")),
        QCommandLineOption(QStringLiteral("debug-clipboard"), QStringLiteral("Save the PNG of each view copy (copy_view) here."),
                           QStringLiteral("png")),
        QCommandLineOption(QStringLiteral("debug-export"),
                           QStringLiteral("Export the comparison to this file (.mp4 H.264, .mkv FFV1, .png sequence) once the "
                                          "videos are shown and indexed, then quit."),
                           QStringLiteral("path")),
        QCommandLineOption(QStringLiteral("debug-export-opts"),
                           QStringLiteral("Comma separated: hevc, screen, size=PERCENT, range=FIRST-LAST, encoder=NAME, "
                                          "quality=high|medium|small, labels=0|1, info=0|1."),
                           QStringLiteral("list")),
    };
    for (QCommandLineOption o : options) {
        o.setFlags(QCommandLineOption::HiddenFromHelp);
        parser.addOption(o);
    }
}

enum class Info { None, Help, Version };

// --help or --version (help wins), told apart from option values as the real parse does. Runs
// before any QApplication exists: asking for them must not need a display.
Info requestedInfo(int argc, char* argv[])
{
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);
    QCommandLineParser parser;
    scvp::ui::addLaunchOptions(parser);
    addDebugOptions(parser);
    parser.parse(args);  // errors are reported by the real parse
    return parser.isSet(QStringLiteral("help")) ? Info::Help : parser.isSet(QStringLiteral("version")) ? Info::Version : Info::None;
}

int printInfo(int argc, char* argv[], Info info)
{
#if defined(Q_OS_WIN)
    QApplication app(argc, argv);  // the text goes to a message box
#else
    QCoreApplication app(argc, argv);  // no display: a headless shell or a script can ask
#endif
    setApplicationNames();
    if (info == Info::Help)
        report(scvp::ui::launchHelp(scvp::ui::AppSettings::load().keys), false);
    else
        report(QStringLiteral("scvp %1\nQt %2\n%3\n").arg(QStringLiteral(SCVP_VERSION), QLatin1String(qVersion()), ffmpegVersionString()),
               false);
    return 0;
}

// --debug-export: what to export, from the file's extension and the option list.
scvp::ui::ExportOptions debugExportOptions(const QString& path, const QString& list)
{
    using namespace scvp;
    ui::ExportOptions o;
    o.path = path;
    const QString ext = QFileInfo(path).suffix().toLower();
    o.format = ext == QLatin1String("mkv") ? OutputFormat::Ffv1 : ext == QLatin1String("png") ? OutputFormat::PngSequence : OutputFormat::H264;
    for (const QString& item : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString key = item.section(QLatin1Char('='), 0, 0).trimmed();
        const QString value = item.section(QLatin1Char('='), 1).trimmed();
        if (key == QLatin1String("hevc"))
            o.format = OutputFormat::Hevc;
        else if (key == QLatin1String("screen"))
            o.picture = ui::ExportPicture::Screen;
        else if (key == QLatin1String("size"))
            o.scale = value.toInt();
        else if (key == QLatin1String("range")) {
            o.first = value.section(QLatin1Char('-'), 0, 0).toLongLong();
            o.last = value.section(QLatin1Char('-'), 1, 1).toLongLong();
        } else if (key == QLatin1String("encoder"))
            o.encoder = value;
        else if (key == QLatin1String("quality"))
            o.quality = value == QLatin1String("small") ? OutputQuality::Small : value == QLatin1String("medium") ? OutputQuality::Medium : OutputQuality::High;
        else if (key == QLatin1String("labels"))
            o.labels = value != QLatin1String("0");
        else if (key == QLatin1String("info"))
            o.frameInfo = value != QLatin1String("0");
    }
    return o;
}

// The view state the command line can set, for the end-to-end checks.
QString debugState(scvp::ui::MainWindow& w)
{
    using namespace scvp;
    const render::CompareSettings& c = w.view()->compare();
    const char* modes[] = {"split", "wipe", "flip", "diff"};
    const render::ViewTransform& t = w.view()->transform();
    const QSizeF pane = t.viewportSize();
    const QPointF mid = t.deviceToImage(QPointF(std::floor(pane.width() / 2), std::floor(pane.height() / 2)));
    auto matrix = [](std::optional<AVColorSpace> m) {
        return !m ? "auto" : *m == AVCOL_SPC_BT470BG ? "601" : *m == AVCOL_SPC_BT709 ? "709" : *m == AVCOL_SPC_BT2020_NCL ? "2020" : "other";
    };
    auto range = [](std::optional<AVColorRange> r) { return !r ? "auto" : *r == AVCOL_RANGE_MPEG ? "limited" : "full"; };
    const render::ColorOverrides& a = w.view()->colorOverrides(0);
    const render::ColorOverrides& b = w.view()->colorOverrides(1);
    PlaybackController& pc = w.player()->controller();
    QStringList s;
    s << QStringLiteral("view=%1").arg(QLatin1String(modes[int(c.mode)])) << QStringLiteral("wipe=%1").arg(c.wipe)
      << QStringLiteral("gain=%1").arg(c.diffGain) << QStringLiteral("zoom=%1").arg(t.isFit() ? QStringLiteral("fit") : QString::number(t.scale()))
      << QStringLiteral("center=%1,%2").arg(int(std::floor(mid.x()))).arg(int(std::floor(mid.y())))
      << QStringLiteral("pixel=%1").arg(w.view()->overlay().mode == render::ValueMode::Codes ? QStringLiteral("codes") : QStringLiteral("rgb"))
      << QStringLiteral("legend=%1").arg(w.hud()->legendVisible() ? QStringLiteral("on") : QStringLiteral("off"))
      << QStringLiteral("info=%1").arg(w.hud()->infoVisible() ? QStringLiteral("on") : QStringLiteral("off"))
      << QStringLiteral("fullscreen=%1").arg(w.isFullScreen() ? QStringLiteral("on") : QStringLiteral("off"))
      << QStringLiteral("speed=%1").arg(pc.speed())
      << QStringLiteral("loop=%1").arg(pc.hasLoop() ? QStringLiteral("%1-%2%3").arg(pc.loopFirst()).arg(pc.loopLast()).arg(pc.loopActive() ? QString() : QStringLiteral("(off)"))
                                                    : QStringLiteral("none"))
      << QStringLiteral("offset=%1").arg(pc.offset())
      << QStringLiteral("matrix=%1,%2").arg(QLatin1String(matrix(a.matrix)), QLatin1String(matrix(b.matrix)))
      << QStringLiteral("range=%1,%2").arg(QLatin1String(range(a.range)), QLatin1String(range(b.range)));
    return s.join(QLatin1Char(' '));
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

// Files handed over by the desktop instead of the command line (macOS Finder, Dock). The first
// opens as A; one arriving right after it (several files opened together) becomes B.
class FileOpenFilter : public QObject {
public:
    explicit FileOpenFilter(scvp::ui::MainWindow& w)
        : m_window(w)
    {
        m_since.start();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::FileOpen)
            return QObject::eventFilter(obj, e);
        const QString file = static_cast<QFileOpenEvent*>(e)->file();
        if (file.isEmpty())
            return false;
        const bool together = m_count > 0 && m_since.elapsed() < 1500;
        m_window.openFile(together ? 1 : 0, file);
        m_count = together ? 0 : 1;
        m_since.restart();
        return true;
    }

private:
    scvp::ui::MainWindow& m_window;
    QElapsedTimer m_since;
    int m_count = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    // Packaging: write the launcher icons and exit (no display needed).
    if (argc == 3 && std::strcmp(argv[1], "--export-icons") == 0)
        return scvp::ui::exportAppIcons(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
    if (const Info info = requestedInfo(argc, argv); info != Info::None)
        return printInfo(argc, argv, info);

    QElapsedTimer started;
    started.start();
    setDefaultSurfaceFormat();
    QApplication app(argc, argv);
    setApplicationNames();
    QApplication::setApplicationDisplayName(QStringLiteral("SCVP"));
    // Matches the installed scvp.desktop: dock icon and window grouping, Wayland app id.
    QGuiApplication::setDesktopFileName(QStringLiteral("scvp"));
    QApplication::setWindowIcon(scvp::ui::appIcon());
    scvp::ff::installLogCallback(AV_LOG_ERROR);

    // Qt has taken its own options (-platform, ...) out of the arguments by now.
    QCommandLineParser parser;
    scvp::ui::addLaunchOptions(parser);
    addDebugOptions(parser);
    scvp::ui::LaunchOptions launch;
    QString error;
    if (!parser.parse(QCoreApplication::arguments()))
        error = parser.errorText();
    else
        scvp::ui::readLaunchOptions(parser, &launch, &error);
    if (!error.isEmpty()) {
        report(QStringLiteral("scvp: %1\nRun 'scvp --help' for the options.\n").arg(error), true);
        return 2;
    }

    std::fprintf(stderr, "scvp %s, Qt %s, %s\n", SCVP_VERSION, qVersion(), qPrintable(ffmpegVersionString()));

    scvp::ui::MainWindow window;
    FileOpenFilter fileOpen(window);
    app.installEventFilter(&fileOpen);
    if (parser.isSet(QStringLiteral("debug-size"))) {
        const QStringList wh = parser.value(QStringLiteral("debug-size")).split(QLatin1Char('x'));
        if (wh.size() == 2)
            window.resize(wh[0].toInt(), wh[1].toInt());
    }
    if (parser.isSet(QStringLiteral("debug-clipboard"))) {
        const QString out = parser.value(QStringLiteral("debug-clipboard"));
        QObject::connect(&window, &scvp::ui::MainWindow::viewCopied, &window, [out](QSize size, double factor) {
            const QMimeData* data = QGuiApplication::clipboard()->mimeData();
            const QByteArray png = data ? data->data(QStringLiteral("image/png")) : QByteArray();
            QFile f(out);
            const bool ok = !png.isEmpty() && f.open(QIODevice::WriteOnly) && f.write(png) == png.size();
            std::fprintf(stderr, "debug-copy: %s %dx%d factor %.6f -> %s\n", ok ? "saved" : "FAILED", size.width(), size.height(), factor,
                         qPrintable(out));
        });
    }
    std::function<void()> viewReady;
    if (parser.isSet(QStringLiteral("debug-actions")) || parser.isSet(QStringLiteral("debug-hover"))) {
        const QStringList ids = parser.value(QStringLiteral("debug-actions")).split(QLatin1Char(','), Qt::SkipEmptyParts);
        const bool hover = parser.isSet(QStringLiteral("debug-hover"));
        const QStringList xy = parser.value(QStringLiteral("debug-hover")).split(QLatin1Char(','));
        const QPointF at(xy.value(0).toDouble(), xy.value(1).toDouble());
        // Once the opened videos show a frame and the command line's view is set (zoom steps and
        // copies need both), at the earliest 200 ms after start. The hover comes first and the
        // actions a moment later, when the pixel under the cursor has been read back, as for a user.
        viewReady = [&window, ids, hover, at, started] {
            QTimer::singleShot(std::max<qint64>(0, 200 - started.elapsed()), &window, [&window, ids, hover, at] {
                if (hover) {
                    QWidget* v = window.view();
                    QMouseEvent move(QEvent::MouseMove, at, v->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(v, &move);
                }
                QTimer::singleShot(hover ? 300 : 0, &window, [&window, ids] {
                    for (const QString& id : ids)
                        for (int a = 0; a < int(scvp::ui::Action::Count); ++a)
                            if (id == QLatin1String(scvp::ui::KeyMap::info(scvp::ui::Action(a)).id))
                                window.handleAction(scvp::ui::Action(a), false);
                });
            });
        };
    }
    scvp::ui::applyLaunchOptions(window, launch, viewReady);

    if (parser.isSet(QStringLiteral("debug-export"))) {
        const scvp::ui::ExportOptions o =
            debugExportOptions(parser.value(QStringLiteral("debug-export")), parser.value(QStringLiteral("debug-export-opts")));
        // Once every open video shows a frame (the view has its geometry) and is indexed.
        auto* wait = new QTimer(&window);
        wait->setInterval(50);
        QObject::connect(wait, &QTimer::timeout, &window, [&window, wait, o] {
            scvp::ui::Player* p = window.player();
            bool ready = p->slotOpen(0) || p->slotOpen(1);
            for (int slot = 0; slot < 2; ++slot)
                if (p->slotOpen(slot) && (!p->shown(slot).frame || !p->engine().stats(slot).indexComplete))
                    ready = false;
            if (!ready)
                return;
            wait->stop();
            QString err;
            if (!window.startExport(o, &err)) {
                std::fprintf(stderr, "debug-export: FAILED to start: %s\n", qPrintable(err));
                QApplication::exit(1);
            }
        });
        QObject::connect(&window, &scvp::ui::MainWindow::exportFinished, &window, [&window, o](bool ok, const QString& message) {
            const scvp::ui::ExportStats& s = window.lastExportStats();
            auto pct = [&s](double busy) { return s.seconds > 0 ? 100.0 * busy / s.seconds : 0.0; };
            std::fprintf(stderr,
                         "debug-export: %s %lld frames %dx%d via %s in %.2f s (%.1f fps; busy: decode A %.0f%% B %.0f%%, render %.0f%%, "
                         "encode %.0f%%; %d missing) -> %s%s%s\n",
                         ok ? "done" : "FAILED", static_cast<long long>(s.frames), s.width, s.height, s.encoder.c_str(), s.seconds,
                         s.seconds > 0 ? s.frames / s.seconds : 0.0, pct(s.decode[0]), pct(s.decode[1]), pct(s.render), pct(s.encode),
                         s.missing, qPrintable(o.path), message.isEmpty() ? "" : ": ", qPrintable(message));
            QApplication::exit(ok ? 0 : 1);
        });
        wait->start();
    }

    if (parser.isSet(QStringLiteral("debug-grab"))) {
        const QString out = parser.value(QStringLiteral("debug-grab"));
        const bool wholeWindow = parser.isSet(QStringLiteral("debug-grab-window"));
        QTimer::singleShot(parser.value(QStringLiteral("debug-after")).toInt(), &window, [&window, out, wholeWindow] {
            const QImage img = wholeWindow ? window.grab().toImage() : window.view()->grabFramebuffer();
            const bool ok = img.save(out);
            std::fprintf(stderr, "debug-grab: %s %dx%d -> %s (frame %lld)\n", ok ? "saved" : "FAILED", img.width(), img.height(),
                         qPrintable(out), (long long)window.player()->controller().position());
            std::fprintf(stderr, "debug-inspector: %s\n", qPrintable(window.inspectorText()));
            const scvp::ui::HudOverlay* hud = window.hud();
            std::fprintf(stderr, "debug-legend: %s, A \"%s\", B \"%s\"\n", hud->legendVisible() ? "on" : "off",
                         qPrintable(hud->slotName(0)), qPrintable(hud->slotName(1)));
            std::fprintf(stderr, "debug-state: %s\n", qPrintable(debugState(window)));
            QApplication::exit(ok ? 0 : 1);
        });
    }
    return QApplication::exec();
}
