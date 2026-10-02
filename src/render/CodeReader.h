#pragma once

#include "render/PixelLayout.h"

#include <array>
#include <cstdint>

namespace scvp::render {

// Raw code values of one pixel, read on the CPU exactly as the conversion shader reads them:
// (Y, Cb, Cr) for YUV with nearest chroma, (R, G, B) for RGB, (Y, Y, Y) for gray.
std::array<uint32_t, 3> readCodes(const AVFrame* frame, const PixelLayout& layout, int x, int y);

} // namespace scvp::render
