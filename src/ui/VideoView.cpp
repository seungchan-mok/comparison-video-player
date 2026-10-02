#include "ui/VideoView.h"

#include <QElapsedTimer>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QWheelEvent>

#include <cmath>

namespace scvp::ui {

VideoView::VideoView(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Plain key codes even while a Hangul/Japanese IME is active.
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setMinimumSize(160, 90);
}

VideoView::~VideoView()
{
    makeCurrent();
    releaseGl();
    doneCurrent();
}

void VideoView::releaseGl()
{
    if (m_renderer.initialized())
        m_renderer.release();
}

void VideoView::initializeGL()
{
    auto* gl = QOpenGLVersionFunctionsFactory::get<render::GL>(context());
    if (!gl) {
        qWarning("OpenGL 3.3 core functions unavailable");
        return;
    }
    gl->initializeOpenGLFunctions();
    m_renderer.initialize(gl);
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent();
        releaseGl();
        doneCurrent();
    }, Qt::DirectConnection);
}

double VideoView::devicePixelRatioForView() const { return devicePixelRatioF(); }

QPointF VideoView::toDevice(QPointF logical) const { return logical * devicePixelRatioF(); }

void VideoView::updateGeometry()
{
    const double dpr = devicePixelRatioF();
    const QSize fb(int(std::lround(width() * dpr)), int(std::lround(height() * dpr)));
    m_state.framebufferSize = fb;
    m_state.view.setViewportSize(QSizeF(fb));
}

void VideoView::resizeGL(int, int) { updateGeometry(); }

void VideoView::setFrames(const std::array<FramePtr, 2>& frames)
{
    m_state.frames = frames;
    const FramePtr& f = frames[0] ? frames[0] : frames[1];
    if (f)
        m_state.view.setImageSize(QSizeF(f->width(), f->height()));
    update();
}

void VideoView::paintGL()
{
    if (!m_renderer.initialized())
        return;
    QElapsedTimer t;
    t.start();
    updateGeometry();
    m_renderer.render(m_state, defaultFramebufferObject());
    m_lastPaintMs = t.nsecsElapsed() / 1e6;
}

void VideoView::fitToWindow()
{
    m_state.view.fit();
    emit viewChanged();
    update();
}

void VideoView::oneToOne()
{
    const QPointF c = rect().contains(mapFromGlobal(QCursor::pos())) ? toDevice(mapFromGlobal(QCursor::pos()))
                                                                    : QPointF(m_state.framebufferSize.width() / 2.0,
                                                                              m_state.framebufferSize.height() / 2.0);
    m_state.view.oneToOne(c);
    emit viewChanged();
    update();
}

void VideoView::zoomSteps(int steps)
{
    const QPoint local = mapFromGlobal(QCursor::pos());
    const QPointF anchor = rect().contains(local) ? toDevice(local)
                                                  : QPointF(m_state.framebufferSize.width() / 2.0, m_state.framebufferSize.height() / 2.0);
    m_state.view.zoomSteps(steps, anchor);
    emit viewChanged();
    update();
}

void VideoView::wheelEvent(QWheelEvent* e)
{
    const bool touchpadScroll = !e->pixelDelta().isNull() && !(e->modifiers() & Qt::ControlModifier);
    if (touchpadScroll) {
        // Two-finger scrolling pans; Ctrl/Cmd + scroll and the mouse wheel zoom.
        m_state.view.panBy(toDevice(QPointF(e->pixelDelta())));
    } else {
        m_wheelAccum += e->angleDelta().y();
        const int steps = m_wheelAccum / 120;
        m_wheelAccum -= steps * 120;
        if (steps)
            m_state.view.zoomSteps(steps, toDevice(e->position()));
    }
    emit viewChanged();
    update();
    e->accept();
}

bool VideoView::event(QEvent* e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            const double s = m_state.view.scale() * (1.0 + g->value());
            m_state.view.zoomTo(s, toDevice(g->position()));
            emit viewChanged();
            update();
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}

void VideoView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        m_dragging = true;
        m_lastDrag = e->position();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(e);
}

void VideoView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging) {
        m_state.view.panBy(toDevice(e->position() - m_lastDrag));
        m_lastDrag = e->position();
        emit viewChanged();
        update();
    }
    QOpenGLWidget::mouseMoveEvent(e);
}

void VideoView::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_dragging) {
        m_dragging = false;
        unsetCursor();
    }
    QOpenGLWidget::mouseReleaseEvent(e);
}

void VideoView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        emit doubleClicked();
}

} // namespace scvp::ui
