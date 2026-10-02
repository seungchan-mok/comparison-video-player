#include "render/ColorParams.h"

#include <algorithm>
#include <cmath>

namespace scvp::render {
namespace {

struct Coeffs {
    double kr, kb;
};

Coeffs coefficientsFor(AVColorSpace cs)
{
    switch (cs) {
    case AVCOL_SPC_BT709: return {0.2126, 0.0722};
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return {0.2627, 0.0593};  // constant luminance approximated by NCL
    case AVCOL_SPC_SMPTE240M: return {0.212, 0.087};
    case AVCOL_SPC_FCC: return {0.30, 0.11};
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    default: return {0.299, 0.114};
    }
}

bool isRgbMatrix(AVColorSpace cs) { return cs == AVCOL_SPC_RGB; }

} // namespace

const char* ColorParams::matrixName(AVColorSpace cs)
{
    switch (cs) {
    case AVCOL_SPC_BT709: return "BT.709";
    case AVCOL_SPC_BT2020_NCL: return "BT.2020";
    case AVCOL_SPC_BT2020_CL: return "BT.2020 CL";
    case AVCOL_SPC_SMPTE240M: return "SMPTE 240M";
    case AVCOL_SPC_FCC: return "FCC";
    case AVCOL_SPC_YCGCO: return "YCgCo";
    case AVCOL_SPC_RGB: return "RGB";
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return "BT.601";
    default: return "?";
    }
}

ChromaSiting ChromaSiting::of(AVChromaLocation loc)
{
    switch (loc) {
    case AVCHROMA_LOC_CENTER: return {0.5f, 0.5f};
    case AVCHROMA_LOC_TOPLEFT: return {0.0f, 0.0f};
    case AVCHROMA_LOC_TOP: return {0.5f, 0.0f};
    case AVCHROMA_LOC_BOTTOMLEFT: return {0.0f, 1.0f};
    case AVCHROMA_LOC_BOTTOM: return {0.5f, 1.0f};
    default: return {0.0f, 0.5f};  // left: MPEG-2/H.264/HEVC default
    }
}

ColorParams ColorParams::resolve(const PixelLayout& layout, AVColorSpace colorspace, AVColorRange range, int height,
                                 const ColorOverrides& overrides)
{
    ColorParams p;
    const int n = layout.bitDepth;
    p.bitDepth = n;
    p.outMax = float((1u << n) - 1u);
    const double maxCode = double((1u << n) - 1u);
    const double k = std::ldexp(1.0, n - 8);  // 2^(N-8): scales the 8-bit range constants

    AVColorRange rng = overrides.range ? *overrides.range : range;
    if (rng != AVCOL_RANGE_MPEG && rng != AVCOL_RANGE_JPEG)
        rng = layout.fullRangeDefault ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    p.rangeUsed = rng;
    const bool full = rng == AVCOL_RANGE_JPEG;

    if (layout.isRgb() || layout.isGray()) {
        p.matrixUsed = AVCOL_SPC_RGB;
        const double off = full ? 0.0 : 16.0 * k;
        const double scl = full ? 1.0 / maxCode : 1.0 / (219.0 * k);
        p.offset = {float(off), float(off), float(off)};
        p.scale = {float(scl), float(scl), float(scl)};
        if (layout.isGray())
            p.matrix = {1, 0, 0, 1, 0, 0, 1, 0, 0};  // R = G = B = Y
        return p;
    }

    AVColorSpace cs = overrides.matrix ? *overrides.matrix : colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RESERVED || cs == AVCOL_SPC_NB)
        cs = height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    p.matrixUsed = cs;

    const double yOff = full ? 0.0 : 16.0 * k;
    const double yScl = full ? 1.0 / maxCode : 1.0 / (219.0 * k);
    const double cOff = full ? double(1u << (n - 1)) : 128.0 * k;
    const double cScl = full ? 1.0 / maxCode : 1.0 / (224.0 * k);
    p.offset = {float(yOff), float(cOff), float(cOff)};
    p.scale = {float(yScl), float(cScl), float(cScl)};

    if (isRgbMatrix(cs)) {
        // "RGB" matrix on YUV-labelled planes (H.264 RGB): G = Y, B = Cb, R = Cr, no offsets.
        p.offset = {float(yOff), float(yOff), float(yOff)};
        p.scale = {float(yScl), float(yScl), float(yScl)};
        p.matrix = {0, 0, 1, 1, 0, 0, 0, 1, 0};
        return p;
    }
    if (cs == AVCOL_SPC_YCGCO) {
        // R = Y - Cg + Co, G = Y + Cg, B = Y - Cg - Co (components: Y, Cg, Co).
        p.matrix = {1, -1, 1, 1, 1, 0, 1, -1, -1};
        return p;
    }
    const Coeffs c = coefficientsFor(cs);
    const double kg = 1.0 - c.kr - c.kb;
    p.matrix = {1.0f, 0.0f, float(2.0 * (1.0 - c.kr)),
                1.0f, float(-2.0 * c.kb * (1.0 - c.kb) / kg), float(-2.0 * c.kr * (1.0 - c.kr) / kg),
                1.0f, float(2.0 * (1.0 - c.kb)), 0.0f};
    return p;
}

std::array<int, 3> ColorParams::toRgb(uint32_t c0, uint32_t c1, uint32_t c2) const
{
    // Same float32 arithmetic and order as the shader.
    const float v[3] = {(float(c0) - offset[0]) * scale[0], (float(c1) - offset[1]) * scale[1],
                        (float(c2) - offset[2]) * scale[2]};
    std::array<int, 3> out{};
    for (int r = 0; r < 3; ++r) {
        float x = matrix[size_t(r * 3)] * v[0] + matrix[size_t(r * 3 + 1)] * v[1] + matrix[size_t(r * 3 + 2)] * v[2];
        x = std::clamp(x, 0.0f, 1.0f);
        out[size_t(r)] = int(std::floor(x * outMax + 0.5f));
    }
    return out;
}

} // namespace scvp::render
