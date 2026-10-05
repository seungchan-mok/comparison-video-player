#pragma once

#include "ui/KeyMap.h"
#include "ui/Settings.h"

#include "render/CompareLayout.h"

#include <QMainWindow>

#include <optional>

namespace scvp::ui {

class ControlBar;
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

    // Raw video files without their geometry in the name ask for it (askRaw forces the dialog).
    bool openFile(int slot, const QString& path, bool askRaw = false);
    void setRawOverride(const RawVideoParams& p) { m_rawOverride = p; }  // command line --raw
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
    QString inspectorText() const;
    // The legend shows `text` for this video instead of its file name ("" = the file name).
    void setLegendText(int slot, const QString& text);
    void offerDefaultPlayer();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void updateTitle();
    void createMenus();
    QAction* playerAction(QMenu* menu, Action a);
    void createColorMenu();
    void syncViewActions();
    void setLegendVisible(bool on);
    void editLegendText();

    VideoView* m_view;
    Player* m_player;
    ControlBar* m_controls;
    Stage* m_stage;
    HudOverlay* m_hud;
    KeyRouter* m_keys;
    AppSettings m_settings;
    std::optional<RawVideoParams> m_rawOverride;
    QList<QPair<QAction*, Action>> m_menuActions;
    QAction* m_smoothAction = nullptr;
    QAction* m_legendAction = nullptr;
};

} // namespace scvp::ui
