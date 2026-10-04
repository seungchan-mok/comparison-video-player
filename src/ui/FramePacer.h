#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <cstdint>

class QOpenGLWidget;

namespace scvp::ui {

// Media clock on the vsync grid: wall time since the clock started, rounded to whole refresh
// periods. With swaps that block for vsync it advances by exactly one period per swap (jitter
// below half a period is absorbed), so 30 fps on 60 Hz shows every frame exactly twice; with
// swaps that do not block it simply follows the wall clock and never runs fast.
class VsyncClock {
public:
    explicit VsyncClock(double period = 1.0 / 60.0)
        : m_period(period)
    {
    }
    void setPeriod(double p) { m_period = p > 0 ? p : m_period; }
    double period() const { return m_period; }
    void restart() { m_anchorWall = -1; }       // next update starts a new run
    double update(double wall);                 // returns the media time
    double media() const { return m_media; }

private:
    double m_period;
    double m_anchorWall = -1;
    double m_anchorMedia = 0;
    int64_t m_ticks = 0;
    double m_media = 0;
};

// Paces playback by the display: on every swap of the video widget it advances the VsyncClock
// and emits tick(). If swaps do not block (vsync off, occluded window), a precise timer drives
// the clock instead; a watchdog keeps it going when no swaps happen at all.
class FramePacer : public QObject {
    Q_OBJECT
public:
    explicit FramePacer(QOpenGLWidget* view, QObject* parent = nullptr);

    void setActive(bool active);
    bool active() const { return m_active; }
    double now() const;  // media clock, seconds
    double refreshPeriod() const { return m_period; }

signals:
    void tick(double now);

private:
    void onSwapped();
    void advance(double wall);

    QOpenGLWidget* m_view;
    QElapsedTimer m_wall;
    QTimer m_timer;     // fallback pacing when swaps do not block
    QTimer m_watchdog;  // no swaps at all (minimised, unexposed)
    double m_period = 1.0 / 60.0;
    VsyncClock m_clock;
    double m_lastSwap = -1.0;
    int m_fastSwaps = 0;
    bool m_useTimer = false;
    bool m_active = false;
};

} // namespace scvp::ui
