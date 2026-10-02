#pragma once

#include "ui/TimelineModel.h"

#include <QTimer>
#include <QWidget>

#include <array>

namespace scvp::ui {

// Position bar: cached frames per video (A above, B below), the A-B loop, how far indexing has
// got, the playhead and a hover readout. Click/drag scrubs (pausing, play resumes on release),
// Shift+drag moves one frame per pixel, the loop edges can be dragged.
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(TimelineModel* model, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    int64_t frameAt(double x) const;  // logical x -> frame
    double xAt(int64_t frame) const;  // frame -> logical x (centre of its slot)

public slots:
    void positionChanged();
    void modelChanged();  // loop, length, media: repaint everything

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    enum class Drag { None, Seek, LoopFirst, LoopLast };
    QRectF track() const;
    void pollCache();
    QString timecode(int64_t frame) const;

    TimelineModel* m_model;
    QTimer m_poll;
    std::array<RangeSetPtr, 2> m_ranges;
    Drag m_drag = Drag::None;
    double m_dragStartX = 0;
    int64_t m_dragStartFrame = 0;
    double m_hoverX = -1;
    int64_t m_lastPainted = -1;
};

} // namespace scvp::ui
