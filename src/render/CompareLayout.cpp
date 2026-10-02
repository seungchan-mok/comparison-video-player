#include "render/CompareLayout.h"

#include "render/ViewTransform.h"

#include <algorithm>
#include <cmath>

namespace scvp::render {

CompareLayout CompareLayout::compute(const CompareSettings& s, QSize fb, QSizeF image, bool hasA, bool hasB)
{
    CompareLayout l;
    const QRect full(0, 0, std::max(1, fb.width()), std::max(1, fb.height()));
    if (!hasA || !hasB) {
        l.panes.push_back({full, hasA || !hasB ? 0 : 1});
        return l;
    }
    switch (s.mode) {
    case CompareMode::Flip:
        l.panes.push_back({full, s.flipShowsB ? 1 : 0});
        return l;
    case CompareMode::Wipe:
    case CompareMode::Difference:
        l.panes.push_back({full, 0});
        return l;
    case CompareMode::Split:
        break;
    }
    const int w = full.width(), h = full.height();
    const QSize sbs(std::max(1, (w - kDivider) / 2), h);
    const QSize tb(w, std::max(1, (h - kDivider) / 2));
    SplitOrientation o = s.orientation;
    if (o == SplitOrientation::Auto)
        o = ViewTransform::fitScale(image, QSizeF(tb)) > ViewTransform::fitScale(image, QSizeF(sbs)) ? SplitOrientation::TopBottom
                                                                                                    : SplitOrientation::SideBySide;
    l.orientation = o;
    if (o == SplitOrientation::SideBySide) {
        l.panes.push_back({QRect(QPoint(0, 0), sbs), 0});
        l.panes.push_back({QRect(QPoint(w - sbs.width(), 0), sbs), 1});
    } else {
        l.panes.push_back({QRect(QPoint(0, 0), tb), 0});
        l.panes.push_back({QRect(QPoint(0, h - tb.height()), tb), 1});
    }
    return l;
}

const Pane* CompareLayout::paneAt(QPointF d) const
{
    const Pane* best = nullptr;
    double bestDist = 0;
    for (const Pane& p : panes) {
        const QRectF r(p.rect);
        const double dx = std::max({r.left() - d.x(), 0.0, d.x() - r.right()});
        const double dy = std::max({r.top() - d.y(), 0.0, d.y() - r.bottom()});
        const double dist = dx + dy;
        if (!best || dist < bestDist) {
            best = &p;
            bestDist = dist;
        }
    }
    return best;
}

} // namespace scvp::render
