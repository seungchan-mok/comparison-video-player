#include "render/CodeReader.h"

#include <cstring>

namespace scvp::render {
namespace {

uint32_t sample(const AVFrame* f, int plane, int x, int y, int bytesPerComponent, int componentsPerTexel, int component)
{
    const uint8_t* row = f->data[plane] + ptrdiff_t(y) * f->linesize[plane];
    const uint8_t* px = row + ptrdiff_t(x) * componentsPerTexel * bytesPerComponent + component * bytesPerComponent;
    if (bytesPerComponent == 1)
        return *px;
    uint16_t v;
    std::memcpy(&v, px, 2);  // little-endian formats only (PixelLayout rejects big-endian)
    return v;
}

} // namespace

std::array<uint32_t, 3> readCodes(const AVFrame* f, const PixelLayout& l, int x, int y)
{
    const int bpc = l.bitDepth > 8 ? 2 : 1;
    auto C = [&](uint32_t v) { return (v >> l.shift) & l.mask; };
    const int cx = x >> l.chromaShiftX;
    const int cy = y >> l.chromaShiftY;
    switch (l.layout) {
    case Layout::PlanarYuv:
        return {C(sample(f, 0, x, y, bpc, 1, 0)), C(sample(f, 1, cx, cy, bpc, 1, 0)), C(sample(f, 2, cx, cy, bpc, 1, 0))};
    case Layout::SemiYuv:
        return {C(sample(f, 0, x, y, bpc, 1, 0)), C(sample(f, 1, cx, cy, bpc, 2, l.swizzle[0])),
                C(sample(f, 1, cx, cy, bpc, 2, l.swizzle[1]))};
    case Layout::Packed422: {
        const int t = x >> 1;
        const int yc = (x & 1) == 0 ? l.swizzle[0] : l.swizzle[1];
        return {sample(f, 0, t, y, 1, 4, yc), sample(f, 0, t, y, 1, 4, l.swizzle[2]), sample(f, 0, t, y, 1, 4, l.swizzle[3])};
    }
    case Layout::PlanarRgb:
        return {C(sample(f, 2, x, y, bpc, 1, 0)), C(sample(f, 0, x, y, bpc, 1, 0)), C(sample(f, 1, x, y, bpc, 1, 0))};
    case Layout::PackedRgb: {
        const int comps = componentCount(l.plane[0].format);
        return {C(sample(f, 0, x, y, bpc, comps, l.swizzle[0])), C(sample(f, 0, x, y, bpc, comps, l.swizzle[1])),
                C(sample(f, 0, x, y, bpc, comps, l.swizzle[2]))};
    }
    case Layout::Gray:
    case Layout::Bayer: {
        const uint32_t v = C(sample(f, 0, x, y, bpc, 1, 0));
        return {v, v, v};
    }
    }
    return {0, 0, 0};
}

} // namespace scvp::render
