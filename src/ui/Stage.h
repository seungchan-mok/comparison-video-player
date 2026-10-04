#pragma once

#include <QTimer>
#include <QWidget>

namespace scvp::ui {

class ControlBar;

// Holds the video and the control bar. Windowed: controls below the video. Fullscreen: the
// video fills everything and the control bar floats over its bottom edge as a translucent
// child, shown on mouse movement and hidden after two idle seconds (kept while hovered, while a
// popup is open, while a spin box is being edited or while dragging); the cursor hides with it.
class Stage : public QWidget {
    Q_OBJECT
public:
    Stage(QWidget* view, ControlBar* controls, QWidget* parent = nullptr);

    void setFullscreenMode(bool on);
    bool fullscreenMode() const { return m_fullscreen; }
    void setIdleInterval(int ms) { m_idle.setInterval(ms); }

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void layoutChildren();
    void showControls();
    void hideControlsIfIdle();

    QWidget* m_view;
    ControlBar* m_controls;
    QTimer m_idle;
    bool m_fullscreen = false;
};

} // namespace scvp::ui
