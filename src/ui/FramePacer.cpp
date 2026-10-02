#include "ui/FramePacer.h"

#include <QGuiApplication>
#include <QOpenGLWidget>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace scvp::ui {

FramePacer::FramePacer(QOpenGLWidget* view, QObject* parent)
    : QObject(parent)
    , m_view(view)
{
    m_wall.start();
    if (QScreen* s = QGuiApplication::primaryScreen())
        if (s->refreshRate() > 1)
            m_period = 1.0 / s->refreshRate();
    connect(view, &QOpenGLWidget::frameSwapped, this, &FramePacer::onSwapped);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] { advance(m_wall.nsecsElapsed() / 1e9); });
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(250);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_active)
            advance(m_wall.nsecsElapsed() / 1e9);
    });
}

double FramePacer::now() const { return m_media; }

void FramePacer::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    m_lastSwap = -1.0;
    if (!active) {
        m_timer.stop();
        m_watchdog.stop();
        return;
    }
    if (m_useTimer)
        m_timer.start(int(std::max(1.0, m_period * 1000.0)));
    m_view->update();
}

void FramePacer::onSwapped()
{
    if (!m_active || m_useTimer)
        return;
    const double wall = m_wall.nsecsElapsed() / 1e9;
    if (m_lastSwap >= 0) {
        const double dt = wall - m_lastSwap;
        // Swaps much faster than the refresh period mean they do not wait for vsync.
        if (dt < m_period * 0.5) {
            if (++m_fastSwaps > 10) {
                m_useTimer = true;
                m_timer.start(int(std::max(1.0, m_period * 1000.0)));
                return;
            }
        } else {
            m_fastSwaps = 0;
        }
    }
    advance(wall);
}

void FramePacer::advance(double wall)
{
    if (m_lastSwap < 0) {
        m_lastSwap = wall;
        m_media += m_period;
    } else {
        const double dt = wall - m_lastSwap;
        const int n = std::max(1, int(std::lround(dt / m_period)));
        if (n == 1 && dt > m_period * 0.5 && dt < m_period * 1.5)
            m_period = m_period * 0.95 + dt * 0.05;  // follow the real refresh rate
        m_media += n * m_period;
        m_lastSwap = wall;
    }
    m_watchdog.start();
    emit tick(m_media);
    m_view->update();  // keep the swap loop running
}

} // namespace scvp::ui
