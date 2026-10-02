#pragma once

#include "engine/PlaybackController.h"

#include <QTimer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QToolButton;

namespace scvp::ui {

class Player;
class TimelineWidget;

// Transport controls under the video (or overlaid on it in fullscreen): play/pause, timeline,
// position, jump intervals for the arrow keys, speed, loop and cache usage.
class ControlBar : public QWidget {
    Q_OBJECT
public:
    explicit ControlBar(Player* player, QWidget* parent = nullptr);

    TimelineWidget* timeline() const { return m_timeline; }
    void setJumpSettings(const JumpSettings& j);  // updates the fields without re-applying
    void setOverlay(bool overlay);  // translucent dark background over the video
    bool hasActivePopup() const;    // a combo box list is open (keep an overlay visible)

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    void syncTransport();
    void syncPosition();
    void syncStats();
    void applyJumps();

    Player* m_player;
    TimelineWidget* m_timeline;
    QToolButton* m_play;
    QLabel* m_time;
    QDoubleSpinBox* m_jumpSmall;
    QDoubleSpinBox* m_jumpLarge;
    QComboBox* m_jumpUnit;
    QComboBox* m_speed;
    QToolButton* m_loop;
    QLabel* m_loopLabel;
    QLabel* m_cache;
    QTimer m_stats;
    bool m_overlay = false;
};

} // namespace scvp::ui
