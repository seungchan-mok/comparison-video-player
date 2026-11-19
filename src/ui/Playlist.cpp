#include "ui/Playlist.h"

#include "engine/ImageSequenceSource.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>

#include <algorithm>

namespace scvp::ui {
namespace {

bool naturalLess(const QString& a, const QString& b) { return scvp::naturalLess(a.toStdString(), b.toStdString()); }

QFileInfoList videosIn(const QString& dir)
{
    // Hidden files (macOS "._clip.mp4" and the like) are left out by QDir::Files.
    return QDir(dir).entryInfoList(videoNameFilters(), QDir::Files);
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

bool folderHasVideos(const QString& dir) { return QFileInfo(dir).isDir() && !videosIn(dir).isEmpty(); }

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
        QStringList files;
        for (const QFileInfo& f : videosIn(m_folders[size_t(slot)]))
            files << f.absoluteFilePath();
        std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
            return naturalLess(QFileInfo(a).fileName(), QFileInfo(b).fileName());
        });
        for (const QString& f : files)
            byName[QFileInfo(f).completeBaseName()][size_t(slot)] << f;
    }
    QStringList names = byName.keys();
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return naturalLess(a, b); });
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
