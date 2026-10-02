#include "render/PixelOverlay.h"

#include "render/DigitAtlas.h"

#include <algorithm>

namespace scvp::render {

int ValueGeometry::digitsOf(int v)
{
    int d = 1;
    for (v = std::max(v, 0); v >= 10; v /= 10)
        ++d;
    return d;
}

ValueGeometry ValueGeometry::compute(int maxValue, bool gray, bool signedValues, double scale, double dpr)
{
    ValueGeometry g;
    g.rows = gray ? 1 : 3;
    g.field = digitsOf(maxValue) + (signedValues ? 1 : 0);
    g.glyphH = g.rowH * 0.8;
    g.glyphW = g.glyphH * DigitAtlas::aspect();
    const double maxW = 0.92;
    if (g.field * g.glyphW > maxW) {
        const double f = maxW / (g.field * g.glyphW);
        g.glyphW *= f;
        g.glyphH *= f;
    }
    g.visible = scale * g.glyphH >= kDigitMinLogicalPx * dpr;
    return g;
}

} // namespace scvp::render
