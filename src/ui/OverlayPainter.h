#pragma once

#include "render/CompareLayout.h"

#include <QColor>
#include <QFont>
#include <QRect>
#include <QSize>
#include <QString>

#include <array>

class QPainter;
class QFontMetrics;

namespace scvp::ui {

// One picture of the view, as the legend and the pane labels describe it. Plain values, so the
// same painting serves the screen (HudOverlay), a still copy and an exported video, where the
// picture is drawn at another size on another thread.
struct OverlayScene {
    render::CompareLayout layout;    // panes in device px of the picture
    double dpr = 1.0;                // device px per painter (logical) px
    render::CompareSettings compare;
    std::array<bool, 2> open{};      // the slot has a video
    std::array<bool, 2> hold{};      // it is holding its first or last frame
    std::array<QString, 2> names;    // the user's text, else the file name
    QFont font;                      // base font of the legend
};

namespace overlay {

QColor slotColor(int slot);  // A orange, B teal
// Which video each part of the view shows ("A", "B HOLD", "|A-B| x2") at the top right of each
// pane, or at the bottom when the info box takes the top-left corner. Two videos only.
void paintPaneLabels(QPainter& p, const OverlayScene& s, const QFontMetrics& fm, bool infoVisible);
// A coloured A/B chip and the name in each part of the view (wipe: A at the left edge, B at the
// right; difference: both), moved below `avoid` (the info box) where they would overlap it.
void paintLegend(QPainter& p, const OverlayScene& s, const QRect& avoid);
// One line in a box at the bottom left of a `size` (logical px) picture: frame numbers and time
// in an exported video.
void paintFrameInfo(QPainter& p, QSize size, const QString& text, const QFont& mono);

} // namespace overlay
} // namespace scvp::ui
