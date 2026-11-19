#pragma once

#include "engine/Demux.h"
#include "engine/HwDevice.h"
#include "engine/PlaybackController.h"
#include "render/ColorParams.h"
#include "render/CompareLayout.h"
#include "render/PixelOverlay.h"

#include <QPointF>
#include <QString>
#include <QStringList>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

class QCommandLineParser;

namespace scvp::ui {

class KeyMap;
class MainWindow;

// What the command line asks for. It all applies to this run only: the saved settings never
// change. Unset fields leave the saved settings and the defaults.
struct LaunchOptions {
    QStringList files;  // A, then B
    std::array<std::optional<RawVideoParams>, 2> raw;

    std::optional<int64_t> frame;
    bool play = false;
    std::optional<double> speed;
    std::optional<std::pair<int64_t, int64_t>> loop;  // first, last (inclusive)

    std::optional<int64_t> offset;
    std::optional<SyncMode> sync;

    std::optional<render::CompareSettings> compare;
    std::optional<double> zoom;     // device px per image px; 0 = fit to the window
    std::optional<QPointF> center;  // image pixel moved to the middle of the view
    std::optional<render::ValueMode> pixelMode;
    bool legend = false;
    std::array<std::optional<QString>, 2> legendText;
    bool info = false;
    bool fullscreen = false;
    std::array<render::ColorOverrides, 2> color;  // matrix and range; nullopt = the metadata

    std::optional<size_t> cacheMb;
    std::optional<size_t> vramMb;
    std::optional<HwConfig> hwdec;
};

// Registers every option and the A/B arguments, -h/--help and -v/--version included (the help
// text is launchHelp(), not Qt's).
void addLaunchOptions(QCommandLineParser& parser);
// The long name of every option launchHelp() describes.
QStringList launchOptionNames();
// Reads a parsed command line. False on a bad value, with a message naming the option.
bool readLaunchOptions(const QCommandLineParser& parser, LaunchOptions* out, QString* error);
// WxH:pix_fmt[:fps[:header bytes]], e.g. 1920x1080:nv12:30.
std::optional<RawVideoParams> parseRawSpec(const QString& spec);
// The --help text: the options by section, notes, examples, the keys as `keys` binds them, the mouse.
QString launchHelp(const KeyMap& keys);
// The versions of SCVP (QCoreApplication::applicationVersion), Qt and FFmpeg, one per line: --version
// prints it, Help > About SCVP shows it.
QString versionText();

// Shows the window and sets it up as `o` asks: decoding, the videos, position, comparison and
// view. The zoom waits until the opened videos show a frame (only then are the image and the
// pane sizes known); `viewReady` runs right after it.
void applyLaunchOptions(MainWindow& window, const LaunchOptions& o, std::function<void()> viewReady = {});

} // namespace scvp::ui
