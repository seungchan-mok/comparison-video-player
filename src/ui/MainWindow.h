#pragma once

#include "ui/KeyMap.h"
#include "ui/Settings.h"

#include "render/CompareLayout.h"

#include <QMainWindow>

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

    bool openFile(int slot, const QString& path);
    Player* player() const { return m_player; }
    VideoView* view() const { return m_view; }

    void handleAction(Action a, bool autoRepeat);
    void toggleFullscreen();
    void openDialog(int slot);
    void showSettings();
    void applySettings(const AppSettings& s);
    void overrideCacheBudget(size_t bytes);  // command line: this run only
    void overrideHardware(const HwConfig& hw);
    void overrideGpuBudget(size_t bytes);
    void reopenAll();  // after a decoding change: same files, same frame
    void setCompare(const render::CompareSettings& c);
    QString inspectorText() const;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void updateTitle();
    void createMenus();
    QAction* playerAction(QMenu* menu, Action a);
    void createColorMenu();

    VideoView* m_view;
    Player* m_player;
    ControlBar* m_controls;
    Stage* m_stage;
    HudOverlay* m_hud;
    KeyRouter* m_keys;
    AppSettings m_settings;
    QList<QPair<QAction*, Action>> m_menuActions;
};

} // namespace scvp::ui
