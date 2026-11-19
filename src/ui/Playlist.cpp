#include "ui/Playlist.h"

#include "engine/ImageSequenceSource.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>

#include <algorithm>

namespace scvp::ui {
namespace {

bool naturalLess(const QString& a, const QString& b) { return scvp::naturalLess(a.toStdString(), b.toStdString()); }

// Path by path, each compared naturally: "day1/z" before "day1 b/a", "c2/x" before "c10/x".
bool pathLess(const QString& a, const QString& b)
{
    const QStringList x = a.split(QLatin1Char('/')), y = b.split(QLatin1Char('/'));
    for (qsizetype i = 0; i < std::min(x.size(), y.size()); ++i)
        if (x[i] != y[i])
            return naturalLess(x[i], y[i]);
    return x.size() < y.size();
}

// Calls `found` with the path below `dir` of each video in it and in every folder below it,
// until it returns false. A name starting with a dot is hidden on every platform (macOS
// "._clip.mp4" copied to other disks, ".git"; Windows hides by attribute only). Symlinked folders
// are not followed, so a link loop cannot make it endless.
template <typename F>
void forEachVideo(const QString& dir, F found)
{
    const QDir root(dir);
    for (QDirIterator it(dir, videoNameFilters(), QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString rel = root.relativeFilePath(it.next());
        if (!rel.startsWith(QLatin1Char('.')) && !rel.contains(QLatin1String("/.")) && !found(rel))
            return;
    }
}

} // namespace

const QStringList& videoNameFilters()
{
    static const QStringList filters = [] {
        QStringList f;
        for (const char* e : {"mp4", "mkv", "mov", "avi", "webm", "ts", "m2ts", "mts", "mpg", "mpeg", "m4v", "flv", "wmv", "mxf",
                              "y4m", "h264", "264", "h265", "265", "hevc", "ivf", "obu", "yuv"})
            f << QStringLiteral("*.") + QLatin1String(e);
        return f;
    }();
    return filters;
}

bool folderHasVideos(const QString& dir)
{
    bool any = false;
    if (QFileInfo(dir).isDir())
        forEachVideo(dir, [&any](const QString&) {
            any = true;
            return false;  // one is enough
        });
    return any;
}

void Playlist::setFolder(int slot, const QString& dir)
{
    m_folders[size_t(slot)] = dir;
    rebuild();
}

void Playlist::swapSlots()
{
    std::swap(m_folders[0], m_folders[1]);
    for (PlaylistItem& item : m_items)
        std::swap(item.paths[0], item.paths[1]);
}

void Playlist::rebuild()
{
    const QString was = m_current >= 0 ? m_items[size_t(m_current)].name : QString();
    QMap<QString, std::array<QStringList, 2>> byName;
    for (int slot = 0; slot < 2; ++slot) {
        if (m_folders[size_t(slot)].isEmpty())
            continue;
        const QDir root(m_folders[size_t(slot)]);
        QStringList files;
        forEachVideo(root.path(), [&files](const QString& rel) {
            files << rel;
            return true;
        });
        std::sort(files.begin(), files.end(), naturalLess);  // x.mkv before x.mp4
        for (const QString& f : files) {
            // The path below the folder without the extension: day1/clip.mp4 -> "day1/clip".
            const QFileInfo rel(f);
            const QString name = rel.path() == QLatin1String(".") ? rel.completeBaseName() : rel.path() + QLatin1Char('/') + rel.completeBaseName();
            byName[name][size_t(slot)] << root.absoluteFilePath(f);
        }
    }
    QStringList names = byName.keys();
    std::sort(names.begin(), names.end(), pathLess);
    m_items.clear();
    m_current = -1;
    for (const QString& name : names) {
        const std::array<QStringList, 2>& v = byName[name];
        for (qsizetype i = 0; i < std::max(v[0].size(), v[1].size()); ++i) {
            if (name == was && m_current < 0)
                m_current = int(m_items.size());
            m_items.push_back({name, {v[0].value(i), v[1].value(i)}});
        }
    }
    if (m_current < 0 && !m_items.empty())
        m_current = 0;
}

} // namespace scvp::ui
