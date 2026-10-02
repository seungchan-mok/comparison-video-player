#pragma once

#include "ui/KeyMap.h"

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

private:
    void updateTitle();

    VideoView* m_view;
    Player* m_player;
    ControlBar* m_controls;
    Stage* m_stage;
    HudOverlay* m_hud;
    KeyRouter* m_keys;
};

} // namespace scvp::ui
