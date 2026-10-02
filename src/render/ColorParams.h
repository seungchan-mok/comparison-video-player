#pragma once

#include "render/PixelLayout.h"

#include <array>
#include <optional>

namespace scvp::render {

// User choices that replace what the stream metadata says.
struct ColorOverrides {
    std::optional<AVColorSpace> matrix;  // AVCOL_SPC_BT709, AVCOL_SPC_BT470BG (601), AVCOL_SPC_BT2020_NCL, ...
    std::optional<AVColorRange> range;   // AVCOL_RANGE_MPEG (limited) or AVCOL_RANGE_JPEG (full)
};

// Code values -> RGB for one source:  rgb = clamp(M * ((code - offset) * scale)), then quantized
// to the source bit depth (outMax = 2^N - 1) so the numbers shown on screen are exact integers.
// The conversion shader receives exactly these numbers; toRgb() is the CPU reference used by
// tests and by anything that needs a value without reading the GPU back.
struct ColorParams {
    std::array<float, 9> matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major
    std::array<float, 3> offset{0, 0, 0};
    std::array<float, 3> scale{1, 1, 1};
    float outMax = 255.0f;
    int bitDepth = 8;
    AVColorSpace matrixUsed = AVCOL_SPC_RGB;
    AVColorRange rangeUsed = AVCOL_RANGE_JPEG;

    // `colorspace`/`range` are the frame's metadata (unspecified values pick defaults: BT.709
    // for height >= 720 else BT.601; limited range for YUV except yuvj*, full for RGB/gray).
    static ColorParams resolve(const PixelLayout& layout, AVColorSpace colorspace, AVColorRange range, int height,
                               const ColorOverrides& overrides = {});

    // Codes in the order the shader feeds them: (Y, Cb, Cr) for YUV, (R, G, B) for RGB,
    // (Y, Y, Y) for gray. Returns integers in [0, outMax].
    std::array<int, 3> toRgb(uint32_t c0, uint32_t c1, uint32_t c2) const;

    static const char* matrixName(AVColorSpace cs);
};

} // namespace scvp::render
