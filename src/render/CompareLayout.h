#pragma once

#include <QRect>
#include <QSize>
#include <QSizeF>

#include <vector>

namespace scvp::render {

enum class CompareMode {
    Split,       // side by side or top/bottom, same zoom and pan in both panes
    Wipe,        // A left of the divider, B right of it
    Flip,        // one video at a time, toggled
    Difference,  // |A - B| x gain
};

enum class SplitOrientation { Auto, SideBySide, TopBottom };

struct CompareSettings {
    CompareMode mode = CompareMode::Split;
    SplitOrientation orientation = SplitOrientation::Auto;
    double wipe = 0.5;     // divider position, fraction of the view width
    bool flipShowsB = false;
    double diffGain = 1.0;
};

// One viewport of the view and which slots it draws.
struct Pane {
    QRect rect;    // device px, top-left origin
    int slot = 0;  // the video drawn (Split/Flip) or the first operand (Wipe/Difference)
};

// Divides the framebuffer for the comparison mode. Split panes have equal sizes so one view
// transform maps both pixel-identically; the divider between them is kDivider px wide on screen
// (wider when the view is drawn larger than the screen, as for a capture).
struct CompareLayout {
    static constexpr int kDivider = 2;

    std::vector<Pane> panes;
    SplitOrientation orientation = SplitOrientation::SideBySide;  // resolved (Split only)

    static CompareLayout compute(const CompareSettings& s, QSize framebuffer, QSizeF image, bool hasA, bool hasB,
                                 int divider = kDivider);
    // The pane containing a device pixel, or the nearest one.
    const Pane* paneAt(QPointF device) const;
    QSize paneSize() const { return panes.empty() ? QSize(1, 1) : panes.front().rect.size(); }
};

} // namespace scvp::render
