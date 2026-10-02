#include "ui/TimelineWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace scvp::ui {
namespace {

constexpr double kPad = 8.0;   // horizontal padding (logical px)
constexpr double kHandle = 4.0;

const QColor kTrack(48, 52, 58);
const QColor kCachedA(56, 150, 168);
const QColor kCachedB(214, 150, 54);
const QColor kLoop(255, 255, 255, 38);
const QColor kLoopEdge(240, 240, 240);
const QColor kPlayhead(239, 72, 102);
const QColor kUnindexed(255, 255, 255, 18);

} // namespace

TimelineWidget::TimelineWidget(TimelineModel* model, QWidget* parent)
    : QWidget(parent)
    , m_model(model)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_poll.setInterval(100);  // cache bands at 10 Hz, only repainted when they changed
    connect(&m_poll, &QTimer::timeout, this, &TimelineWidget::pollCache);
    m_poll.start();
}

QSize TimelineWidget::sizeHint() const { return QSize(600, 30); }
QSize TimelineWidget::minimumSizeHint() const { return QSize(120, 24); }

QRectF TimelineWidget::track() const { return QRectF(kPad, 6, std::max(1.0, width() - 2 * kPad), height() - 12.0); }

int64_t TimelineWidget::frameAt(double x) const
{
    const int64_t n = m_model->length();
    if (n <= 0)
        return 0;
    const QRectF t = track();
    const double f = (x - t.left()) / t.width() * double(n);
    return std::clamp<int64_t>(int64_t(std::floor(f)), 0, n - 1);
}

double TimelineWidget::xAt(int64_t frame) const
{
    const int64_t n = std::max<int64_t>(1, m_model->length());
    const QRectF t = track();
    return t.left() + (double(frame) + 0.5) / double(n) * t.width();
}

QString TimelineWidget::timecode(int64_t frame) const
{
    const double fps = m_model->frameRate() > 0 ? m_model->frameRate() : 25.0;
    const double s = double(frame) / fps;
    const int h = int(s / 3600);
    const int m = int(s / 60) % 60;
    const double sec = std::fmod(s, 60.0);
    return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec, 6, 'f', 3, QLatin1Char('0'));
}

void TimelineWidget::pollCache()
{
    bool changed = false;
    for (int s = 0; s < 2; ++s) {
        RangeSetPtr r = m_model->slotOpen(s) ? m_model->cachedRanges(s) : nullptr;
        if (r != m_ranges[size_t(s)]) {
            m_ranges[size_t(s)] = r;
            changed = true;
        }
    }
    if (changed)
        update();
}

void TimelineWidget::positionChanged()
{
    const int64_t p = m_model->position();
    if (p == m_lastPainted)
        return;
    // Repaint only the strips of the old and new playhead.
    const double oldX = xAt(m_lastPainted);
    const double newX = xAt(p);
    update(QRect(int(oldX) - 3, 0, 7, height()));
    update(QRect(int(newX) - 3, 0, 7, height()));
    m_lastPainted = p;
}

void TimelineWidget::modelChanged() { update(); }

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QRectF t = track();
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(t, 3, 3);
    const int64_t n = m_model->length();
    if (n <= 0)
        return;

    // Not indexed yet (positions there are estimates).
    const int64_t indexed = std::clamp<int64_t>(m_model->indexedFrames(), 0, n);
    if (indexed < n) {
        const double x0 = t.left() + double(indexed) / double(n) * t.width();
        p.setBrush(kUnindexed);
        p.drawRect(QRectF(x0, t.top(), t.right() - x0, t.height()));
    }

    // Cached frames: A in the upper half, B in the lower half (or full height alone).
    const bool both = m_model->slotOpen(0) && m_model->slotOpen(1);
    for (int s = 0; s < 2; ++s) {
        const RangeSetPtr& r = m_ranges[size_t(s)];
        if (!r || !m_model->slotOpen(s))
            continue;
        const double y = both ? (s == 0 ? t.top() : t.center().y()) : t.top();
        const double h = both ? t.height() / 2.0 : t.height();
        p.setBrush(s == 0 ? kCachedA : kCachedB);
        const int64_t off = m_model->slotOffset(s);
        for (const FrameRange& fr : r->ranges) {
            const double x0 = t.left() + double(fr.first - off) / double(n) * t.width();
            const double x1 = t.left() + double(fr.last + 1 - off) / double(n) * t.width();
            if (x1 < t.left() || x0 > t.right())
                continue;
            p.drawRect(QRectF(std::max(x0, t.left()), y + 1, std::max(1.0, std::min(x1, t.right()) - std::max(x0, t.left())), h - 2));
        }
    }

    // Loop region with draggable edges.
    if (m_model->hasLoop()) {
        const double x0 = t.left() + double(m_model->loopFirst()) / double(n) * t.width();
        const double x1 = t.left() + double(m_model->loopLast() + 1) / double(n) * t.width();
        p.setBrush(m_model->loopActive() ? kLoop : QColor(255, 255, 255, 14));
        p.drawRect(QRectF(x0, t.top() - 4, x1 - x0, t.height() + 8));
        p.setPen(QPen(m_model->loopActive() ? kLoopEdge : QColor(150, 150, 150), 2));
        p.drawLine(QPointF(x0, t.top() - 5), QPointF(x0, t.bottom() + 5));
        p.drawLine(QPointF(x1, t.top() - 5), QPointF(x1, t.bottom() + 5));
        p.setPen(Qt::NoPen);
    }

    // Playhead.
    const double px = xAt(m_model->position());
    p.setBrush(kPlayhead);
    p.drawRect(QRectF(px - 1.0, 1, 2.0, height() - 2.0));
    m_lastPainted = m_model->position();

    // Hover readout.
    if (m_hoverX >= 0 && m_drag == Drag::None) {
        const int64_t f = frameAt(m_hoverX);
        p.setPen(QColor(255, 255, 255, 120));
        p.drawLine(QPointF(m_hoverX, t.top()), QPointF(m_hoverX, t.bottom()));
        const QString label = QStringLiteral("%1  %2").arg(f).arg(timecode(f));
        QFont font = p.font();
        font.setPointSizeF(font.pointSizeF() * 0.85);
        p.setFont(font);
        const QFontMetricsF fm(font);
        const double w = fm.horizontalAdvance(label) + 10;
        const double lx = std::clamp(m_hoverX - w / 2, 0.0, std::max(0.0, width() - w));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 190));
        p.drawRoundedRect(QRectF(lx, 0, w, fm.height() + 2), 3, 3);
        p.setPen(QColor(240, 240, 240));
        p.drawText(QRectF(lx, 0, w, fm.height() + 2), Qt::AlignCenter, label);
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || m_model->length() <= 0)
        return;
    const double x = e->position().x();
    if (m_model->hasLoop()) {
        const QRectF t = track();
        const int64_t n = m_model->length();
        const double x0 = t.left() + double(m_model->loopFirst()) / double(n) * t.width();
        const double x1 = t.left() + double(m_model->loopLast() + 1) / double(n) * t.width();
        if (std::abs(x - x0) <= kHandle) {
            m_drag = Drag::LoopFirst;
            return;
        }
        if (std::abs(x - x1) <= kHandle) {
            m_drag = Drag::LoopLast;
            return;
        }
    }
    m_drag = Drag::Seek;
    m_dragStartX = x;
    m_dragStartFrame = frameAt(x);
    m_model->scrubBegin();
    m_model->scrubTo(m_dragStartFrame);
    update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e)
{
    const double x = e->position().x();
    m_hoverX = x;
    switch (m_drag) {
    case Drag::Seek: {
        int64_t f = frameAt(x);
        if (e->modifiers() & Qt::ShiftModifier)  // fine scrubbing: one frame per pixel
            f = std::clamp<int64_t>(m_dragStartFrame + int64_t(std::lround(x - m_dragStartX)), 0, m_model->length() - 1);
        m_model->scrubTo(f);
        break;
    }
    case Drag::LoopFirst:
        m_model->setLoop(std::min(frameAt(x), m_model->loopLast()), m_model->loopLast());
        break;
    case Drag::LoopLast:
        m_model->setLoop(m_model->loopFirst(), std::max(frameAt(x), m_model->loopFirst()));
        break;
    case Drag::None:
        break;
    }
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_drag == Drag::Seek)
        m_model->scrubEnd(m_model->position());
    m_drag = Drag::None;
    QWidget::mouseReleaseEvent(e);
    update();
}

void TimelineWidget::leaveEvent(QEvent*)
{
    m_hoverX = -1;
    update();
}

} // namespace scvp::ui
