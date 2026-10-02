#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class QOpenGLWidget;

namespace scvp::ui {

// Paces playback by the display: on every swap of the video widget it advances a media clock by
// the number of vsync periods that passed (n = round(dt / period)) and emits tick(). Counting
// vsyncs instead of reading the wall clock keeps frame cadence regular (30 fps on 60 Hz shows
// every frame exactly twice). If swaps do not block (vsync off, occluded window), a precise
// timer takes over; a watchdog keeps the clock going when no swaps happen at all.
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
    double m_media = 0.0;
    double m_lastSwap = -1.0;
    int m_fastSwaps = 0;
    bool m_useTimer = false;
    bool m_active = false;
};

} // namespace scvp::ui
