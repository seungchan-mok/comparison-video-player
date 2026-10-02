#pragma once

namespace scvp::render {

enum class ValueMode { Rgb, Codes };  // displayed RGB, or the source's code values (YUV, ...)

struct PixelOverlaySettings {
    bool values = true;  // digits in each pixel once the cells are big enough
    ValueMode mode = ValueMode::Rgb;
    bool grid = true;    // pixel grid once cells are big enough
};

// Where the digits go inside a source pixel ("cell", unit square): up to three rows of `field`
// glyphs, centred. Shared by the display shader (as uniforms) and tests.
struct ValueGeometry {
    int rows = 3;
    int field = 3;        // glyphs per row (digits of the largest value, + sign)
    double glyphW = 0;    // cell units
    double glyphH = 0;
    double rowH = 1.0 / 3.0;
    bool visible = false;

    static int digitsOf(int maxValue);
    // scale: device px per cell; dpr: device px per logical px.
    static ValueGeometry compute(int maxValue, bool gray, bool signedValues, double scale, double dpr);
    double top() const { return 0.5 - rows * rowH / 2.0; }
    double left() const { return 0.5 - field * glyphW / 2.0; }
};

// Grid lines from this many logical px per source pixel.
constexpr double kGridMinLogicalPx = 6.0;
// Digits from this glyph height in logical px.
constexpr double kDigitMinLogicalPx = 7.0;

} // namespace scvp::render
