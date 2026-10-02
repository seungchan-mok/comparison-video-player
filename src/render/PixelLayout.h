#pragma once

#include "engine/FfmpegCompat.h"

#include <array>
#include <cstdint>
#include <optional>

namespace scvp::render {

// Shader families; each has its own conversion shader variant.
enum class Layout : uint8_t {
    PlanarYuv,  // Y, U, V planes (any subsampling)
    SemiYuv,    // Y plane + interleaved UV plane (NV12/NV21/P010 ...)
    Packed422,  // YUYV / UYVY: two pixels per RGBA texel
    PlanarRgb,  // G, B, R planes (FFmpeg GBRP order)
    PackedRgb,  // RGB24 / RGBA / BGRA / RGB48 ...
    Gray,
};

// Integer texture formats: shaders read exact code values with texelFetch on usampler2D.
enum class TexFormat : uint8_t { R8UI, R16UI, RG8UI, RG16UI, RGB8UI, RGBA8UI, RGB16UI, RGBA16UI };

int bytesPerTexel(TexFormat f);
int componentCount(TexFormat f);

struct PlaneSpec {
    TexFormat format = TexFormat::R8UI;
    int log2w = 0;       // texture width = ceil(width / 2^log2w)
    int log2h = 0;
    bool halfWidth = false;  // packed 4:2:2: one texel per two pixels
};

// How the planes of an AVPixelFormat map to textures and how codes are read from them.
struct PixelLayout {
    AVPixelFormat format = AV_PIX_FMT_NONE;
    Layout layout = Layout::PlanarYuv;
    int planes = 0;
    std::array<PlaneSpec, 4> plane{};
    int bitDepth = 8;        // significant bits N per component
    int shift = 0;           // right shift that yields the code (MSB-aligned P010: 6)
    uint32_t mask = 0xff;    // (1 << N) - 1
    int chromaShiftX = 0;    // log2 chroma subsampling
    int chromaShiftY = 0;
    // Component selection inside a texel: SemiYuv (U, V) in [0], [1]; Packed422
    // (Y0, Y1, U, V); PackedRgb (R, G, B).
    std::array<int, 4> swizzle{0, 1, 2, 3};
    bool fullRangeDefault = false;  // yuvj*, RGB and gray formats

    bool isYuv() const { return layout == Layout::PlanarYuv || layout == Layout::SemiYuv || layout == Layout::Packed422; }
    bool isRgb() const { return layout == Layout::PlanarRgb || layout == Layout::PackedRgb; }
    bool isGray() const { return layout == Layout::Gray; }
    int planeWidth(int i, int width) const;
    int planeHeight(int i, int height) const;

    // Supported formats map directly; everything else must be converted first (nullopt).
    static std::optional<PixelLayout> of(AVPixelFormat format);
};

} // namespace scvp::render
