#pragma once

#include "ui/ExportDialog.h"
#include "ui/ExportJob.h"
#include "ui/KeyMap.h"
#include "ui/Playlist.h"
#include "ui/Settings.h"

#include "render/ColorParams.h"
#include "render/CompareLayout.h"

#include <QMainWindow>

#include <array>
#include <memory>
#include <optional>

class QActionGroup;
class QDockWidget;
class QListWidget;

namespace scvp::ui {

class ClipboardImage;
class ControlBar;
class ExportProgress;
class HudOverlay;
class KeyRouter;
class Stage;
class Player;
class VideoView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Raw video files without their geometry in the name ask for it (askRaw forces the dialog);
    // `raw` opens the file as that raw video (command line --raw). A folder holding videos becomes
    // the slot's playlist folder (paired with the other slot's by file name) and opens its first
    // item; a file replaces the slot's playlist folder.
    bool openFile(int slot, const QString& path, bool askRaw = false, const std::optional<RawVideoParams>& raw = std::nullopt);
    // Opens item i of the playlist in the slots that have a folder, from its first frame.
    bool playItem(int i);
    const Playlist& playlist() const { return m_playlist; }
    Player* player() const { return m_player; }
    VideoView* view() const { return m_view; }
    HudOverlay* hud() const { return m_hud; }

    void handleAction(Action a, bool autoRepeat);
    void toggleFullscreen();
    void openDialog(int slot, bool raw = false);
    void showSettings();
    void applySettings(const AppSettings& s);
    void overrideCacheBudget(size_t bytes);  // command line: this run only
    void overrideHardware(const HwConfig& hw);
    void overrideGpuBudget(size_t bytes);
    void reopenAll();  // after a decoding change: same files, same frame
    void setCompare(const render::CompareSettings& c);
    // Colour interpretation of one video; the Color menu follows.
    void setColorOverrides(int slot, const render::ColorOverrides& o);
    QString inspectorText() const;
    // The legend shows `text` for this video instead of its file name ("" = the file name).
    // Naming a video shows the legend for this run (the saved setting stays).
    void setLegendText(int slot, const QString& text);
    // The legend for this run only; the L key and the legend dialog also save it.
    void showLegend(bool on);
    // The view as it looks, as an image on the clipboard (Ctrl+C).
    void copyView();
    void offerDefaultPlayer();
    // File > Export Comparison Video: asks what to export and starts it in the background (or
    // shows the export that is running).
    void exportVideo();
    // Starts an export with these options (also --debug-export); false with the reason when it
    // cannot start (no video, still indexing, one already running).
    bool startExport(const ExportOptions& o, QString* error = nullptr);
    bool exporting() const { return m_export != nullptr; }
    // The export these options describe for the videos and the view as they are now.
    std::optional<ExportSpec> exportSpec(const ExportOptions& o, QString* error);
    const ExportStats& lastExportStats() const { return m_lastExport; }

signals:
    // The copy is on the clipboard; factor: its size over the screen's device pixels.
    void viewCopied(QSize size, double factor);
    void exportFinished(bool ok, const QString& message);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    bool openSlot(int slot, const QString& path, bool askRaw = false, const std::optional<RawVideoParams>& raw = std::nullopt);
    bool openFolder(int slot, const QString& dir);
    void folderDialog(int slot);
    void closeSlot(int slot);
    void syncPlaylist();  // the panel's rows, current row and visibility
    void updateTitle();
    void createMenus();
    QAction* playerAction(QMenu* menu, Action a);
    void createColorMenu();
    void syncViewActions();
    void syncColorActions();
    void setLegendVisible(bool on);  // and save it
    void editLegendText();
    void exportDone(bool ok, const QString& message);

    VideoView* m_view;
    Player* m_player;
    ControlBar* m_controls;
    Stage* m_stage;
    HudOverlay* m_hud;
    KeyRouter* m_keys;
    AppSettings m_settings;
    QList<QPair<QAction*, Action>> m_menuActions;
    QAction* m_smoothAction = nullptr;
    QAction* m_legendAction = nullptr;
    // Color menu per video; each action's data is the AVColorSpace / AVColorRange, -1 = auto.
    std::array<QActionGroup*, 2> m_matrixGroup{};
    std::array<QActionGroup*, 2> m_rangeGroup{};
    ClipboardImage* m_clipboard;
    double m_copyFactor = 1.0;
    std::unique_ptr<ExportJob> m_export;  // running
    ExportProgress* m_exportProgress;
    QAction* m_exportAction = nullptr;
    ExportStats m_lastExport;
    Playlist m_playlist;
    QDockWidget* m_playlistDock;
    QListWidget* m_playlistList;
    bool m_playlistShownWindowed = false;  // the panel hides in fullscreen and comes back after
};

} // namespace scvp::ui
