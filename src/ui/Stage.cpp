#include "ui/Stage.h"

#include "ui/ControlBar.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QMouseEvent>

namespace scvp::ui {

Stage::Stage(QWidget* view, ControlBar* controls, QWidget* parent)
    : QWidget(parent)
    , m_view(view)
    , m_controls(controls)
{
    m_view->setParent(this);
    m_controls->setParent(this);
    m_idle.setSingleShot(true);
    m_idle.setInterval(2000);
    connect(&m_idle, &QTimer::timeout, this, &Stage::hideControlsIfIdle);
    qApp->installEventFilter(this);  // mouse movement anywhere in the window
    layoutChildren();
}

void Stage::setFullscreenMode(bool on)
{
    m_fullscreen = on;
    m_controls->setOverlay(on);
    if (on) {
        showControls();
    } else {
        m_idle.stop();
        m_controls->show();
        m_view->unsetCursor();
    }
    layoutChildren();
}

void Stage::resizeEvent(QResizeEvent*) { layoutChildren(); }

void Stage::layoutChildren()
{
    const int barH = m_controls->sizeHint().height();
    if (m_fullscreen) {
        m_view->setGeometry(rect());
        m_controls->setGeometry(0, height() - barH, width(), barH);
        m_controls->raise();
    } else {
        m_view->setGeometry(0, 0, width(), std::max(0, height() - barH));
        m_controls->setGeometry(0, height() - barH, width(), barH);
    }
}

void Stage::showControls()
{
    if (!m_fullscreen)
        return;
    m_controls->show();
    m_controls->raise();
    m_view->unsetCursor();
    m_idle.start();
}

void Stage::hideControlsIfIdle()
{
    if (!m_fullscreen)
        return;
    const bool hovered = m_controls->underMouse();
    const bool editing = qobject_cast<QAbstractSpinBox*>(QApplication::focusWidget()) != nullptr;
    const bool dragging = QApplication::mouseButtons() != Qt::NoButton;
    if (hovered || editing || dragging || m_controls->hasActivePopup()) {
        m_idle.start();
        return;
    }
    m_controls->hide();
    m_view->setCursor(Qt::BlankCursor);
}

bool Stage::eventFilter(QObject* obj, QEvent* e)
{
    if (m_fullscreen && e->type() == QEvent::MouseMove) {
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w->window() == window())
            showControls();
    }
    return QWidget::eventFilter(obj, e);
}

} // namespace scvp::ui
