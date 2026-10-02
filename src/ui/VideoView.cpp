#include "ui/VideoView.h"

#include <QElapsedTimer>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
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

QPointF VideoView::toPane(QPointF logical) const
{
    const QPointF d = toDevice(logical);
    const render::CompareLayout l = layout();
    const render::Pane* p = l.paneAt(d);
    return p ? d - QPointF(p->rect.topLeft()) : d;
}

render::CompareLayout VideoView::layout() const
{
    return render::CompareLayout::compute(m_state.compare, m_state.framebufferSize, m_state.view.imageSize(),
                                          m_state.frames[0] != nullptr, m_state.frames[1] != nullptr);
}

// All panes have the same size, so one view transform serves every pane.
void VideoView::updateGeometry()
{
    const double dpr = devicePixelRatioF();
    const QSize fb(int(std::lround(width() * dpr)), int(std::lround(height() * dpr)));
    m_state.framebufferSize = fb;
    m_state.devicePixelRatio = dpr;
    m_state.view.setViewportSize(QSizeF(layout().paneSize()));
}

void VideoView::setColorOverrides(int slot, const render::ColorOverrides& o)
{
    m_state.color[size_t(slot)] = o;
    update();
}

void VideoView::setOverlay(const render::PixelOverlaySettings& o)
{
    m_state.overlay = o;
    update();
}

void VideoView::setCompare(const render::CompareSettings& c)
{
    m_state.compare = c;
    updateGeometry();
    emit viewChanged();
    update();
}

void VideoView::resizeGL(int, int) { updateGeometry(); }

void VideoView::setFrames(const std::array<FramePtr, 2>& frames)
{
    m_state.frames = frames;
    const FramePtr& f = frames[0] ? frames[0] : frames[1];
    if (f)
        m_state.view.setImageSize(QSizeF(f->width(), f->height()));
    updateGeometry();  // the layout depends on which videos have frames
    update();
}

void VideoView::paintGL()
{
    if (!m_renderer.initialized())
        return;
    QElapsedTimer t;
    t.start();
    updateGeometry();
    // The pixel under the cursor, recomputed every paint (zoom, pan and new frames move it).
    m_state.probe = m_hovering && (m_state.frames[0] || m_state.frames[1]);
    if (m_state.probe) {
        const QPointF d = toPane(m_hoverPos);
        m_state.probePos = m_state.view.deviceToImage(QPointF(std::floor(d.x()), std::floor(d.y())));
    }
    m_renderer.render(m_state, defaultFramebufferObject());
    m_lastPaintMs = t.nsecsElapsed() / 1e6;
    const render::PixelSample& s = m_renderer.sample();
    auto same = [](const render::PixelSample& a, const render::PixelSample& b) {
        if (a.valid != b.valid || a.viewPos != b.viewPos)
            return false;
        for (int i = 0; i < 2; ++i) {
            const auto &x = a.videos[size_t(i)], &y = b.videos[size_t(i)];
            if (x.valid != y.valid || x.texel != y.texel || x.rgb != y.rgb || x.codes != y.codes || x.hasCodes != y.hasCodes)
                return false;
        }
        return true;
    };
    const render::PixelSample shown = m_state.probe ? s : render::PixelSample{};
    if (!same(shown, m_lastSample)) {
        m_lastSample = shown;
        emit sampleChanged(shown);
    }
    if (m_renderer.samplePending())
        QTimer::singleShot(0, this, qOverload<>(&QWidget::update));  // collect it next frame
}

void VideoView::leaveEvent(QEvent* e)
{
    m_hovering = false;
    update();
    QOpenGLWidget::leaveEvent(e);
}

void VideoView::fitToWindow()
{
    m_state.view.fit();
    emit viewChanged();
    update();
}

void VideoView::oneToOne()
{
    const QSizeF pane = m_state.view.viewportSize();
    const QPointF c = rect().contains(mapFromGlobal(QCursor::pos())) ? toPane(mapFromGlobal(QCursor::pos()))
                                                                    : QPointF(pane.width() / 2.0, pane.height() / 2.0);
    m_state.view.oneToOne(c);
    emit viewChanged();
    update();
}

void VideoView::zoomSteps(int steps)
{
    const QPoint local = mapFromGlobal(QCursor::pos());
    const QSizeF pane = m_state.view.viewportSize();
    const QPointF anchor = rect().contains(local) ? toPane(local) : QPointF(pane.width() / 2.0, pane.height() / 2.0);
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
            m_state.view.zoomSteps(steps, toPane(e->position()));
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
            m_state.view.zoomTo(s, toPane(g->position()));
            emit viewChanged();
            update();
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}

bool VideoView::nearWipeDivider(QPointF logical) const
{
    if (m_state.compare.mode != render::CompareMode::Wipe || !m_state.frames[0] || !m_state.frames[1])
        return false;
    const double x = m_state.compare.wipe * width();
    return std::abs(logical.x() - x) <= 6.0;
}

void VideoView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && nearWipeDivider(e->position())) {
        m_draggingWipe = true;
        e->accept();
        return;
    }
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
    m_hovering = true;
    m_hoverPos = e->position();
    update();
    if (m_draggingWipe) {
        m_state.compare.wipe = std::clamp(e->position().x() / std::max(1, width()), 0.0, 1.0);
        emit compareChanged();
        update();
    } else if (!m_dragging) {
        if (nearWipeDivider(e->position()))
            setCursor(Qt::SplitHCursor);
        else
            unsetCursor();
    }
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
    m_draggingWipe = false;
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
