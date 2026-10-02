#pragma once

#include "render/Renderer.h"

#include <QOpenGLWidget>
#include <QPointF>

#include <array>

namespace scvp::ui {

// The single GL widget that presents the video(s). Owns the Renderer and the view transform;
// handles zoom (wheel at the cursor, touchpad pinch) and pan (drag, touchpad scroll).
class VideoView : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit VideoView(QWidget* parent = nullptr);
    ~VideoView() override;

    void setFrames(const std::array<FramePtr, 2>& frames);
    render::ViewTransform& transform() { return m_state.view; }

    void fitToWindow();
    void oneToOne();
    void zoomSteps(int steps);  // keyboard zoom: around the cursor if inside, else the centre
    double devicePixelRatioForView() const;

    const render::RenderTimings& lastTimings() const { return m_renderer.timings(); }
    render::UploadPath uploadPath(int slot) const { return m_renderer.uploadPath(slot); }
    double lastPaintMs() const { return m_lastPaintMs; }
    QString renderError() const { return m_renderer.error(); }

signals:
    void doubleClicked();
    void viewChanged();  // zoom/pan changed

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    bool event(QEvent* e) override;

private:
    QPointF toDevice(QPointF logical) const;
    void updateGeometry();
    void releaseGl();

    render::Renderer m_renderer;
    render::RenderState m_state;
    bool m_dragging = false;
    QPointF m_lastDrag;
    int m_wheelAccum = 0;
    double m_lastPaintMs = 0;
};

} // namespace scvp::ui
