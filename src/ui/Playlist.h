#pragma once

#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace scvp::ui {

// "*.mp4", "*.mkv", ...: the videos a folder playlist picks up (case-insensitive as QDir name
// filters); the open dialog's filter lists the same.
const QStringList& videoNameFilters();
// A video is in the folder or a folder below it: it opens as a playlist, not as an image
// sequence.
bool folderHasVideos(const QString& dir);

struct PlaylistItem {
    QString name;                  // path below the folder without the extension ("day1/clip"), shared by A and B
    std::array<QString, 2> paths;  // per slot; "" when that folder has no such video
};

// The videos of a folder per slot, subfolders included, paired by their path below the folder
// without the extension (day1/clip.mp4 in A with day1/clip.mkv in B), in natural order folder by
// folder. A video only one folder has is an item of its own with the other path empty; several
// videos of one name in a folder pair up in order, so no file is left out.
class Playlist {
public:
    // "" drops the slot's folder. The items are paired again; the current item stays when its
    // name is still there, else it is the first.
    void setFolder(int slot, const QString& dir);
    void swapSlots();
    const QString& folder(int slot) const { return m_folders[size_t(slot)]; }
    const std::vector<PlaylistItem>& items() const { return m_items; }
    bool active() const { return !m_items.empty(); }
    int current() const { return m_current; }  // -1 without items
    void setCurrent(int i) { m_current = i; }

private:
    void rebuild();

    std::array<QString, 2> m_folders;
    std::vector<PlaylistItem> m_items;
    int m_current = -1;
};

} // namespace scvp::ui
