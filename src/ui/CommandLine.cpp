#include "ui/CommandLine.h"

#include "engine/FfmpegCompat.h"
#include "render/ViewTransform.h"

#include "ui/HudOverlay.h"
#include "ui/KeyMap.h"
#include "ui/MainWindow.h"
#include "ui/Player.h"
#include "ui/Settings.h"
#include "ui/VideoView.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QTimer>

#include <cmath>
#include <vector>

namespace scvp::ui {
namespace {

enum class Section { Files, Playback, TwoVideos, Comparison, View, Color, Decoding, Other };

struct SectionTitle {
    Section id;
    const char* title;
};

const SectionTitle kSections[] = {
    {Section::Files, "Files"},
    {Section::Playback, "Position and playback"},
    {Section::TwoVideos, "Two videos"},
    {Section::Comparison, "Comparison view (two videos)"},
    {Section::View, "View"},
    {Section::Color, "Color (how YUV is turned into RGB)"},
    {Section::Decoding, "Decoding"},
    {Section::Other, "Other"},
};

// One table for both the parser and the help, so the two cannot drift apart.
struct OptionDef {
    Section section;
    const char* name;       // long name
    const char* shortName;  // or nullptr
    const char* value;      // value name; "" for a switch
    const char* help;
};

const std::vector<OptionDef>& optionDefs()
{
    static const std::vector<OptionDef> defs = {
        {Section::Files, "raw", nullptr, "SPEC",
         "Open A and B as headerless raw video. SPEC is WxH:pix_fmt[:fps[:header bytes]] with an FFmpeg pixel format "
         "name, e.g. 1920x1080:yuv420p, 1920x1080:nv12:60, 3840x2160:bayer_rggb16le (fps defaults to 30). Without it, "
         "raw files (.yuv, .rgb, .raw, ...) take the geometry from their name (clip_1920x1080_nv12.yuv) or ask for it."},
        {Section::Files, "raw-a", nullptr, "SPEC", "Like --raw, for A only."},
        {Section::Files, "raw-b", nullptr, "SPEC", "Like --raw, for B only."},

        {Section::Playback, "frame", nullptr, "N",
         "Start at frame N of the timeline: 0 is the first, frames count in display order (the numbers the player "
         "shows)."},
        {Section::Playback, "play", nullptr, "", "Start playing."},
        {Section::Playback, "speed", nullptr, "X", "Playback speed, 1/16 to 16 (default 1)."},
        {Section::Playback, "loop", nullptr, "FIRST:LAST", "Loop over frames FIRST to LAST (inclusive)."},

        {Section::TwoVideos, "offset", nullptr, "N",
         "B shows its frame (A's frame + N). If B lacks A's first 3 frames, --offset -3 lines them up. Default 0."},
        {Section::TwoVideos, "sync", nullptr, "frame|time",
         "Pair the frames by number (frame, default) or by timestamp (time, for videos with different frame rates)."},

        {Section::Comparison, "view", nullptr, "MODE",
         "split: side by side or top/bottom, whichever fits the window (default); sbs: side by side; tb: top/bottom; "
         "wipe: A left of a divider, B right of it; flip: one video at a time, A first (Tab flips); diff: |A - B| "
         "times the gain."},
        {Section::Comparison, "wipe", nullptr, "POS",
         "Wipe divider position, 0 (left edge) to 1 (right edge), default 0.5. Selects the wipe view unless --view "
         "is given."},
        {Section::Comparison, "diff-gain", nullptr, "G",
         "Difference gain, 1 to 256 (default 1). Selects the diff view unless --view is given."},

        {Section::View, "zoom", nullptr, "fit|1:1|N",
         "fit: the whole image in the window (default). N (or 1:1 for 1): N screen pixels per video pixel, 1/32 to "
         "256. The values of the pixels appear in them once they are large enough for the digits: from about 27 on "
         "a screen at 100% scaling (try 32)."},
        {Section::View, "center", nullptr, "X,Y",
         "Put video pixel X,Y (0,0 = top-left) in the middle of the view. Zooms to 1:1 unless --zoom gives a scale."},
        {Section::View, "pixel-mode", nullptr, "rgb|codes",
         "Pixel values show the displayed RGB (rgb, default) or the source's own code values such as Y U V (codes)."},
        {Section::View, "legend", nullptr, "", "Show the A/B legend: which video is which, with its file name."},
        {Section::View, "legend-a", nullptr, "TEXT", "Name A in the legend instead of its file name (shows the legend)."},
        {Section::View, "legend-b", nullptr, "TEXT", "Name B in the legend instead of its file name (shows the legend)."},
        {Section::View, "info", nullptr, "",
         "Show the info overlay: file paths, codec, size, pixel format, frame rate, frame type, decoding and cache "
         "state."},
        {Section::View, "fullscreen", nullptr, "", "Start in fullscreen (Esc leaves it)."},

        {Section::Color, "matrix", nullptr, "M",
         "YUV matrix of both videos: auto (the file's metadata, default), 601, 709 or 2020."},
        {Section::Color, "matrix-a", nullptr, "M", "Like --matrix, for A only (overrides --matrix)."},
        {Section::Color, "matrix-b", nullptr, "M", "Like --matrix, for B only (overrides --matrix)."},
        {Section::Color, "range", nullptr, "R",
         "YUV range of both videos: auto (the file's metadata, default), limited (16-235) or full (0-255)."},
        {Section::Color, "range-a", nullptr, "R", "Like --range, for A only (overrides --range)."},
        {Section::Color, "range-b", nullptr, "R", "Like --range, for B only (overrides --range)."},

        {Section::Decoding, "hwdec", nullptr, "MODE",
         "gpu: hardware decoding for the codecs it decodes bit-exactly, checked against software once (default); "
         "cpu: software only; or a device: cuda, vaapi, d3d11va, videotoolbox."},
        {Section::Decoding, "cache-mb", nullptr, "MB",
         "Memory for decoded frames (default: File > Settings, automatically 25% of RAM)."},
        {Section::Decoding, "vram-mb", nullptr, "MB", "GPU memory for decoded frames (NVIDIA; 0 disables)."},

        {Section::Other, "help", "h", "", "Show this help and exit (needs no display)."},
        {Section::Other, "version", "v", "", "Show the versions of SCVP, Qt and FFmpeg and exit."},
    };
    return defs;
}

// Words of `text` in lines of at most `width` characters, each but the first indented by `indent`.
QString wrap(const QString& text, int indent, int width)
{
    QString out;
    int column = indent;
    for (const QString& word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (column > indent && column + 1 + word.size() > width) {
            out += QLatin1Char('\n') + QString(indent, QLatin1Char(' '));
            column = indent;
        } else if (column > indent) {
            out += QLatin1Char(' ');
            ++column;
        }
        out += word;
        column += int(word.size());
    }
    return out;
}

// "  --name VALUE" padded to the description column (or on a line of its own when longer).
QString entry(const QString& left, const QString& help, int column = 26)
{
    constexpr int kWidth = 80;
    QString line = QStringLiteral("  ") + left;
    if (line.size() + 2 > column)
        line += QLatin1Char('\n') + QString(column, QLatin1Char(' '));
    else
        line = line.leftJustified(column);
    return line + wrap(help, column, kWidth) + QLatin1Char('\n');
}

QString optionLeft(const OptionDef& d)
{
    QString s = d.shortName ? QStringLiteral("-%1, --%2").arg(QLatin1String(d.shortName), QLatin1String(d.name))
                            : QStringLiteral("--%1").arg(QLatin1String(d.name));
    if (*d.value)
        s += QLatin1Char(' ') + QLatin1String(d.value);
    return s;
}

std::optional<QString> valueOf(const QCommandLineParser& p, const char* name)
{
    const QString n = QLatin1String(name);
    return p.isSet(n) ? std::optional<QString>(p.value(n)) : std::nullopt;
}

std::optional<double> toNumber(const QString& s)
{
    bool ok = false;
    const double v = s.trimmed().toDouble(&ok);
    return ok && std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

std::optional<int64_t> toInteger(const QString& s)
{
    bool ok = false;
    const qlonglong v = s.trimmed().toLongLong(&ok);
    return ok ? std::optional<int64_t>(v) : std::nullopt;
}

template <typename T>
std::optional<T> pick(const QString& value, std::initializer_list<std::pair<const char*, T>> choices)
{
    const QString v = value.trimmed().toLower();
    for (const auto& [name, result] : choices)
        if (v == QLatin1String(name))
            return result;
    return std::nullopt;
}

// Matrix and range: an outer nullopt is a bad value, an inner one "auto" (the metadata).
std::optional<std::optional<AVColorSpace>> toMatrix(const QString& v)
{
    using M = std::optional<AVColorSpace>;
    return pick<M>(v, {{"auto", std::nullopt},
                       {"601", AVCOL_SPC_BT470BG},
                       {"bt601", AVCOL_SPC_BT470BG},
                       {"709", AVCOL_SPC_BT709},
                       {"bt709", AVCOL_SPC_BT709},
                       {"2020", AVCOL_SPC_BT2020_NCL},
                       {"bt2020", AVCOL_SPC_BT2020_NCL}});
}

std::optional<std::optional<AVColorRange>> toRange(const QString& v)
{
    using R = std::optional<AVColorRange>;
    return pick<R>(v, {{"auto", std::nullopt},
                       {"limited", AVCOL_RANGE_MPEG},
                       {"tv", AVCOL_RANGE_MPEG},
                       {"full", AVCOL_RANGE_JPEG},
                       {"pc", AVCOL_RANGE_JPEG}});
}

// Runs `fn` once every open video shows a frame, or after `limitMs` at the latest.
void whenFramesShown(MainWindow& w, int limitMs, std::function<void()> fn)
{
    auto* wait = new QTimer(&w);
    wait->setInterval(20);
    QElapsedTimer since;
    since.start();
    QObject::connect(wait, &QTimer::timeout, &w, [&w, wait, since, limitMs, fn = std::move(fn)] {
        Player* p = w.player();
        const bool shown = (!p->slotOpen(0) || p->shown(0).frame) && (!p->slotOpen(1) || p->shown(1).frame);
        if (!shown && since.elapsed() < limitMs)
            return;
        wait->stop();
        wait->deleteLater();
        fn();
    });
    wait->start();
}

} // namespace

void addLaunchOptions(QCommandLineParser& parser)
{
    for (const OptionDef& d : optionDefs()) {
        QStringList names;
        if (d.shortName)
            names << QLatin1String(d.shortName);
        names << QLatin1String(d.name);
        parser.addOption(QCommandLineOption(names, QLatin1String(d.help), QLatin1String(d.value)));
    }
    parser.addPositionalArgument(QStringLiteral("A"), QStringLiteral("Video to open (slot A)"), QStringLiteral("[A]"));
    parser.addPositionalArgument(QStringLiteral("B"), QStringLiteral("Video to compare with A (slot B)"), QStringLiteral("[B]"));
}

QStringList launchOptionNames()
{
    QStringList names;
    for (const OptionDef& d : optionDefs())
        names << QLatin1String(d.name);
    return names;
}

std::optional<RawVideoParams> parseRawSpec(const QString& spec)
{
    const QStringList parts = spec.trimmed().split(QLatin1Char(':'));
    if (parts.size() < 2 || parts.size() > 4)
        return std::nullopt;
    const QStringList wh = parts[0].toLower().split(QLatin1Char('x'));
    if (wh.size() != 2)
        return std::nullopt;
    RawVideoParams p;
    bool okW = false, okH = false;
    p.width = wh[0].toInt(&okW);
    p.height = wh[1].toInt(&okH);
    p.format = av_get_pix_fmt(parts[1].trimmed().toLatin1().constData());
    if (parts.size() > 2) {
        const std::optional<double> fps = toNumber(parts[2]);
        if (!fps || *fps <= 0)
            return std::nullopt;
        p.frameRate = *fps;
    }
    if (parts.size() > 3) {
        const std::optional<int64_t> header = toInteger(parts[3]);
        if (!header || *header < 0)
            return std::nullopt;
        p.headerBytes = *header;
    }
    if (!okW || !okH || !p.valid())
        return std::nullopt;
    return p;
}

bool readLaunchOptions(const QCommandLineParser& p, LaunchOptions* out, QString* error)
{
    LaunchOptions o;
    auto bad = [error](const char* option, const QString& value, const char* expected) {
        *error = QStringLiteral("--%1 expects %2, not '%3'").arg(QLatin1String(option), QLatin1String(expected), value);
        return false;
    };

    o.files = p.positionalArguments();
    if (o.files.size() > 2) {
        *error = QStringLiteral("at most two videos (A and B), not %1: %2").arg(o.files.size()).arg(o.files.join(QStringLiteral(", ")));
        return false;
    }

    // Raw: --raw for both, --raw-a / --raw-b for one.
    const char* rawNames[] = {"raw", "raw-a", "raw-b"};
    for (int i = 0; i < 3; ++i) {
        if (const auto v = valueOf(p, rawNames[i])) {
            const std::optional<RawVideoParams> r = parseRawSpec(*v);
            if (!r)
                return bad(rawNames[i], *v, "WxH:pix_fmt[:fps[:header bytes]] such as 1920x1080:nv12:30");
            for (int slot = 0; slot < 2; ++slot)
                if (i == 0 || i == slot + 1)
                    o.raw[size_t(slot)] = r;
        }
    }

    if (const auto v = valueOf(p, "frame")) {
        o.frame = toInteger(*v);
        if (!o.frame || *o.frame < 0)
            return bad("frame", *v, "a frame number from 0");
    }
    o.play = p.isSet(QStringLiteral("play"));
    if (const auto v = valueOf(p, "speed")) {
        QString s = v->trimmed();
        if (s.endsWith(QLatin1Char('x'), Qt::CaseInsensitive))
            s.chop(1);  // "2x", as the control bar writes it
        o.speed = toNumber(s);
        if (!o.speed || *o.speed < 1.0 / 16.0 || *o.speed > 16.0)
            return bad("speed", *v, "a speed from 0.0625 (1/16) to 16");
    }
    if (const auto v = valueOf(p, "loop")) {
        const QStringList ends = v->split(QLatin1Char(':'));
        const std::optional<int64_t> first = toInteger(ends.value(0));
        const std::optional<int64_t> last = toInteger(ends.value(1));
        if (ends.size() != 2 || !first || !last || *first < 0 || *last < *first)
            return bad("loop", *v, "FIRST:LAST frame numbers with FIRST <= LAST, such as 100:200");
        o.loop = std::make_pair(*first, *last);
    }

    if (const auto v = valueOf(p, "offset")) {
        o.offset = toInteger(*v);
        if (!o.offset)
            return bad("offset", *v, "a whole number of frames, such as -3");
    }
    if (const auto v = valueOf(p, "sync")) {
        o.sync = pick<SyncMode>(*v, {{"frame", SyncMode::Frame}, {"time", SyncMode::Timestamp}});
        if (!o.sync)
            return bad("sync", *v, "frame or time");
    }

    // Comparison. --wipe and --diff-gain also choose their view, unless --view does.
    render::CompareSettings c;
    std::optional<render::CompareMode> implied;
    const std::optional<QString> view = valueOf(p, "view");
    if (view) {
        using render::CompareMode;
        using render::SplitOrientation;
        using Choice = std::pair<CompareMode, SplitOrientation>;
        const auto m = pick<Choice>(*view, {{"split", {CompareMode::Split, SplitOrientation::Auto}},
                                            {"sbs", {CompareMode::Split, SplitOrientation::SideBySide}},
                                            {"tb", {CompareMode::Split, SplitOrientation::TopBottom}},
                                            {"wipe", {CompareMode::Wipe, SplitOrientation::Auto}},
                                            {"flip", {CompareMode::Flip, SplitOrientation::Auto}},
                                            {"diff", {CompareMode::Difference, SplitOrientation::Auto}}});
        if (!m)
            return bad("view", *view, "split, sbs, tb, wipe, flip or diff");
        c.mode = m->first;
        c.orientation = m->second;
    }
    const std::optional<QString> wipe = valueOf(p, "wipe");
    if (wipe) {
        const std::optional<double> w = toNumber(*wipe);
        if (!w || *w < 0 || *w > 1)
            return bad("wipe", *wipe, "a position from 0 (left) to 1 (right)");
        c.wipe = *w;
        implied = render::CompareMode::Wipe;
    }
    const std::optional<QString> gain = valueOf(p, "diff-gain");
    if (gain) {
        const std::optional<double> g = toNumber(*gain);
        if (!g || *g < 1 || *g > 256)
            return bad("diff-gain", *gain, "a gain from 1 to 256");
        c.diffGain = *g;
        if (implied && !view) {
            *error = QStringLiteral("--wipe and --diff-gain select different views: choose one with --view");
            return false;
        }
        implied = render::CompareMode::Difference;
    }
    if (!view && implied)
        c.mode = *implied;
    if (view || implied)
        o.compare = c;

    if (const auto v = valueOf(p, "zoom")) {
        const QString z = v->trimmed().toLower();
        if (z == QLatin1String("fit"))
            o.zoom = 0.0;
        else if (z == QLatin1String("1:1"))
            o.zoom = 1.0;
        else
            o.zoom = toNumber(z);
        using render::ViewTransform;
        if (!o.zoom || (*o.zoom != 0 && (*o.zoom < ViewTransform::kMinScale || *o.zoom > ViewTransform::kMaxScale)))
            return bad("zoom", *v, "fit, 1:1 or a scale from 0.03125 (1/32) to 256");
    }
    if (const auto v = valueOf(p, "center")) {
        const QStringList xy = v->split(QLatin1Char(','));
        const std::optional<double> x = toNumber(xy.value(0));
        const std::optional<double> y = toNumber(xy.value(1));
        if (xy.size() != 2 || !x || !y || *x < 0 || *y < 0)
            return bad("center", *v, "X,Y video pixel coordinates, such as 960,540");
        if (o.zoom && *o.zoom == 0) {
            *error = QStringLiteral("--center needs a zoom scale, not --zoom fit");
            return false;
        }
        o.center = QPointF(*x, *y);
        if (!o.zoom)
            o.zoom = 1.0;
    }
    if (const auto v = valueOf(p, "pixel-mode")) {
        o.pixelMode = pick<render::ValueMode>(*v, {{"rgb", render::ValueMode::Rgb}, {"codes", render::ValueMode::Codes}});
        if (!o.pixelMode)
            return bad("pixel-mode", *v, "rgb or codes");
    }
    o.legend = p.isSet(QStringLiteral("legend"));
    if (const auto v = valueOf(p, "legend-a"))
        o.legendText[0] = v->trimmed();
    if (const auto v = valueOf(p, "legend-b"))
        o.legendText[1] = v->trimmed();
    o.info = p.isSet(QStringLiteral("info"));
    o.fullscreen = p.isSet(QStringLiteral("fullscreen"));

    // Color: the shared option first, so -a / -b win.
    const char* matrixNames[] = {"matrix", "matrix-a", "matrix-b"};
    const char* rangeNames[] = {"range", "range-a", "range-b"};
    for (int i = 0; i < 3; ++i) {
        if (const auto v = valueOf(p, matrixNames[i])) {
            const auto m = toMatrix(*v);
            if (!m)
                return bad(matrixNames[i], *v, "auto, 601, 709 or 2020");
            for (int slot = 0; slot < 2; ++slot)
                if (i == 0 || i == slot + 1)
                    o.color[size_t(slot)].matrix = *m;
        }
        if (const auto v = valueOf(p, rangeNames[i])) {
            const auto r = toRange(*v);
            if (!r)
                return bad(rangeNames[i], *v, "auto, limited or full");
            for (int slot = 0; slot < 2; ++slot)
                if (i == 0 || i == slot + 1)
                    o.color[size_t(slot)].range = *r;
        }
    }

    if (const auto v = valueOf(p, "hwdec")) {
        bool ok = false;
        o.hwdec = AppSettings::parseHwdec(*v, &ok);
        if (!ok)
            return bad("hwdec", *v, "gpu, cpu or a device (cuda, vaapi, d3d11va, videotoolbox)");
    }
    if (const auto v = valueOf(p, "cache-mb")) {
        const std::optional<int64_t> mb = toInteger(*v);
        if (!mb || *mb <= 0)
            return bad("cache-mb", *v, "a size in MB above 0");
        o.cacheMb = size_t(*mb);
    }
    if (const auto v = valueOf(p, "vram-mb")) {
        const std::optional<int64_t> mb = toInteger(*v);
        if (!mb || *mb < 0)
            return bad("vram-mb", *v, "a size in MB (0 disables)");
        o.vramMb = size_t(*mb);
    }

    *out = o;
    return true;
}

QString launchHelp(const KeyMap& keys)
{
    QString h;
    h += QStringLiteral("Usage: scvp [options] [A] [B]\n\n");
    h += wrap(QStringLiteral(
                  "SCVP (Simple Comparison Video Player) plays a video, or two videos locked frame by frame, and shows "
                  "the values of the pixels when zoomed in. The options set up the window it opens; they apply to this "
                  "run only and never change the saved settings. The command returns when the window is closed."),
              0, 80);
    h += QStringLiteral("\n");
    for (const SectionTitle& s : kSections) {
        h += QStringLiteral("\n%1:\n").arg(QLatin1String(s.title));
        if (s.id == Section::Files) {
            h += entry(QStringLiteral("A"),
                       QStringLiteral("Video to open: any file FFmpeg reads (mp4, mkv, mov, avi, webm, ts, ...), raw "
                                      "H.264/HEVC/MPEG-2, raw YUV/RGB/Bayer, or an image sequence (a folder of images, or "
                                      "one numbered file such as shot_0001.png). A folder of videos opens as a playlist "
                                      "(Page Down: next item; the next one also starts when one has played to its end)."));
            h += entry(QStringLiteral("B"), QStringLiteral("Video to compare with A. With folders as A and B, the playlist pairs "
                                                           "their videos by file name without the extension (clip.mp4 with "
                                                           "clip.mkv); a video only one folder has plays alone."));
        }
        for (const OptionDef& d : optionDefs())
            if (d.section == s.id)
                h += entry(optionLeft(d), QLatin1String(d.help));
    }

    h += QStringLiteral("\nNotes:\n");
    const char* notes[] = {
        "With two videos the timeline is A's; B shows frame (A + offset), and a video outside the timeline holds "
        "its first or last frame. Both frames must be decoded before a position is shown.",
        "--zoom and --center apply once both videos show their first frame. N in --zoom counts physical screen "
        "pixels (as the 1 key does), so pixels stay square on HiDPI screens.",
        "Errors go to stderr as \"scvp: ...\". Exit status: 0 when the window is closed, 2 for a bad argument (no "
        "window opens). A file that cannot be opened is reported on stderr and in a dialog.",
    };
    for (const char* n : notes)
        h += QStringLiteral("  ") + wrap(QLatin1String(n), 2, 80) + QLatin1Char('\n');

    h += QStringLiteral("\nExamples:\n");
    const std::pair<const char*, const char*> examples[] = {
        {"scvp a.mp4 b.mp4", "A and B side by side"},
        {"scvp ref.mp4 enc.mp4 --view diff --diff-gain 8 --frame 120", "where they differ, amplified"},
        {"scvp a.mp4 a_cut.mp4 --offset -3", "B lacks A's first 3 frames"},
        {"scvp src.yuv enc.mp4 --raw-a 1920x1080:yuv420p:30 --wipe 0.3", "raw source against its encode"},
        {"scvp ref_dir/ enc_dir/ --view diff --play", "every video of two folders, paired by name, one after another"},
        {"scvp a.mp4 b.mp4 --view sbs --zoom 32 --center 960,540 --pixel-mode codes --frame 50",
         "the Y U V codes around one pixel"},
        {"scvp a.mkv b.mkv --legend-a \"x264 crf 20\" --legend-b \"x265 crf 22\" --play --loop 0:99", "named, looping"},
    };
    for (const auto& [cmd, what] : examples)
        h += QStringLiteral("  %1\n      %2\n").arg(QLatin1String(cmd), QLatin1String(what));

    h += QStringLiteral("\nKeys (as bound now; File > Settings changes them):\n");
    constexpr int kKeyColumn = 30;
    auto native = [](QKeyCombination k) { return QKeySequence(k).toString(QKeySequence::NativeText); };
    for (int a = 0; a < int(Action::Count); ++a) {
        // Not KeyMap::text(): its ", " separator reads badly next to the , key.
        QStringList k;
        for (const QKeyCombination& c : keys.keys(Action(a)))
            k << native(c);
        h += entry(QLatin1String(KeyMap::info(Action(a)).label), k.isEmpty() ? QStringLiteral("(none)") : k.join(QStringLiteral(" or ")),
                   kKeyColumn);
    }
    h += entry(QStringLiteral("Open A / B"),
               native(Qt::ControlModifier | Qt::Key_O) + QStringLiteral(" / ") + native(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_O),
               kKeyColumn);
    h += entry(QStringLiteral("Copy pixel values"), native(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_C), kKeyColumn);

    h += QStringLiteral("\nMouse:\n");
    h += QStringLiteral("  ") +
         wrap(QStringLiteral("Wheel or pinch zooms at the cursor, dragging pans, dragging the wipe divider moves it, "
                             "double-click toggles fullscreen, the back/forward buttons step one frame. A file or "
                             "folder dropped on the left (top) half opens as A, on the other half as B. In the "
                             "playlist panel, double-click opens an item."),
              2, 80) +
         QLatin1Char('\n');
    return h;
}

QString versionText()
{
    auto v = [](unsigned int x) { return QStringLiteral("%1.%2.%3").arg(x >> 16).arg((x >> 8) & 0xff).arg(x & 0xff); };
    return QStringLiteral("scvp %1\nQt %2\nFFmpeg %3 (avcodec %4, avformat %5, avutil %6, swscale %7)\n")
        .arg(QCoreApplication::applicationVersion(), QLatin1String(qVersion()), QString::fromUtf8(av_version_info()), v(avcodec_version()),
             v(avformat_version()), v(avutil_version()), v(swscale_version()));
}

void applyLaunchOptions(MainWindow& w, const LaunchOptions& o, std::function<void()> viewReady)
{
    if (o.cacheMb)
        w.overrideCacheBudget(*o.cacheMb << 20);
    if (o.vramMb)
        w.overrideGpuBudget(*o.vramMb << 20);
    if (o.hwdec)
        w.overrideHardware(*o.hwdec);
    w.show();

    Player* p = w.player();
    if (o.files.size() >= 2)
        w.openFile(1, o.files[1], false, o.raw[1]);
    if (o.sync)
        p->setSyncMode(*o.sync);
    if (o.offset)
        p->setOffset(*o.offset);
    if (o.compare)
        w.setCompare(*o.compare);
    if (!o.files.isEmpty())
        w.openFile(0, o.files[0], false, o.raw[0]);
    const bool open = p->slotOpen(0) || p->slotOpen(1);
    if (open) {
        if (o.loop)
            p->setLoop(o.loop->first, o.loop->second);
        if (o.frame)
            p->seek(*o.frame);
    }
    if (o.speed)
        p->setSpeed(*o.speed);
    if (open && o.play)
        p->togglePlay();

    // After opening: opening a file clears the name its slot had.
    for (int slot = 0; slot < 2; ++slot)
        if (o.legendText[size_t(slot)])
            w.setLegendText(slot, *o.legendText[size_t(slot)]);
    if (o.legend)
        w.showLegend(true);
    if (o.info)
        w.hud()->setInfoVisible(true);
    if (o.pixelMode) {
        render::PixelOverlaySettings overlay = w.view()->overlay();
        overlay.mode = *o.pixelMode;
        w.view()->setOverlay(overlay);
    }
    for (int slot = 0; slot < 2; ++slot)
        if (o.color[size_t(slot)] != render::ColorOverrides{})
            w.setColorOverrides(slot, o.color[size_t(slot)]);
    if (o.fullscreen && !w.isFullScreen())
        w.toggleFullscreen();

    if (!o.zoom && !viewReady)
        return;
    // Decoding a first 8K frame can take a while; past 30 s the view is set up regardless.
    whenFramesShown(w, 30000, [&w, zoom = o.zoom, center = o.center, viewReady = std::move(viewReady)] {
        if (zoom && (w.player()->slotOpen(0) || w.player()->slotOpen(1))) {
            if (*zoom == 0)
                w.view()->fitToWindow();
            else
                w.view()->zoomTo(*zoom, center);
        }
        if (viewReady)
            viewReady();
    });
}

} // namespace scvp::ui
