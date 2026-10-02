#include "render/PixelLayout.h"

namespace scvp::render {

int bytesPerTexel(TexFormat f)
{
    switch (f) {
    case TexFormat::R8UI: return 1;
    case TexFormat::R16UI: return 2;
    case TexFormat::RG8UI: return 2;
    case TexFormat::RG16UI: return 4;
    case TexFormat::RGB8UI: return 3;
    case TexFormat::RGBA8UI: return 4;
    case TexFormat::RGB16UI: return 6;
    case TexFormat::RGBA16UI: return 8;
    }
    return 1;
}

int componentCount(TexFormat f)
{
    switch (f) {
    case TexFormat::R8UI:
    case TexFormat::R16UI: return 1;
    case TexFormat::RG8UI:
    case TexFormat::RG16UI: return 2;
    case TexFormat::RGB8UI:
    case TexFormat::RGB16UI: return 3;
    case TexFormat::RGBA8UI:
    case TexFormat::RGBA16UI: return 4;
    }
    return 1;
}

int PixelLayout::planeWidth(int i, int width) const
{
    const PlaneSpec& p = plane[size_t(i)];
    if (p.halfWidth)
        return (width + 1) / 2;
    return -((-width) >> p.log2w);  // ceil(width / 2^log2w)
}

int PixelLayout::planeHeight(int i, int height) const
{
    return -((-height) >> plane[size_t(i)].log2h);
}

namespace {

bool isBigEndian(const AVPixFmtDescriptor* d) { return d->flags & AV_PIX_FMT_FLAG_BE; }

} // namespace

std::optional<PixelLayout> PixelLayout::of(AVPixelFormat format)
{
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(format);
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                           AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT)))
        return std::nullopt;
    const int depth = d->comp[0].depth;
    if (depth > 16 || (depth > 8 && isBigEndian(d)))
        return std::nullopt;
    for (int c = 1; c < d->nb_components; ++c)
        if (d->comp[c].depth != depth)
            return std::nullopt;  // mixed depths (rgb565, ...)

    PixelLayout l;
    l.format = format;
    l.bitDepth = depth;
    l.mask = (1u << depth) - 1u;
    l.chromaShiftX = d->log2_chroma_w;
    l.chromaShiftY = d->log2_chroma_h;
    const bool rgb = d->flags & AV_PIX_FMT_FLAG_RGB;
    const bool planar = d->flags & AV_PIX_FMT_FLAG_PLANAR;
    const bool wide = depth > 8;
    const TexFormat r = wide ? TexFormat::R16UI : TexFormat::R8UI;
    const TexFormat rg = wide ? TexFormat::RG16UI : TexFormat::RG8UI;
    const char* name = d->name;
    l.fullRangeDefault = rgb || d->nb_components <= 2 || (name[0] == 'y' && name[1] == 'u' && name[2] == 'v' && name[3] == 'j');

    // Gray (optionally with alpha that is ignored).
    if (d->nb_components <= 2 && !rgb) {
        if (d->nb_components == 2 && !planar)
            return std::nullopt;  // ya8/ya16: packed gray+alpha
        l.layout = Layout::Gray;
        l.planes = 1;
        l.plane[0] = {r, 0, 0, false};
        l.shift = d->comp[0].shift;
        return l;
    }

    if (rgb && planar) {
        // GBRP: plane 0 = G, 1 = B, 2 = R (alpha plane ignored).
        l.layout = Layout::PlanarRgb;
        l.planes = 3;
        for (int i = 0; i < 3; ++i)
            l.plane[size_t(i)] = {r, 0, 0, false};
        l.shift = d->comp[0].shift;
        return l;
    }

    if (rgb) {
        // Packed RGB: one texel per pixel with R, G, B at the offsets the descriptor gives.
        const int step = d->comp[0].step;
        if (d->comp[0].plane || d->comp[1].plane || d->comp[2].plane)
            return std::nullopt;
        TexFormat f;
        if (!wide && step == 3)
            f = TexFormat::RGB8UI;
        else if (!wide && step == 4)
            f = TexFormat::RGBA8UI;
        else if (wide && step == 6)
            f = TexFormat::RGB16UI;
        else if (wide && step == 8)
            f = TexFormat::RGBA16UI;
        else
            return std::nullopt;
        const int bpc = wide ? 2 : 1;
        for (int c = 0; c < 3; ++c) {
            if (d->comp[c].offset % bpc || d->comp[c].shift)
                return std::nullopt;
            l.swizzle[size_t(c)] = d->comp[c].offset / bpc;
        }
        l.layout = Layout::PackedRgb;
        l.planes = 1;
        l.plane[0] = {f, 0, 0, false};
        return l;
    }

    // YUV.
    if (planar) {
        if (d->comp[0].plane == 0 && d->comp[1].plane == 1 && d->comp[2].plane == 2) {
            l.layout = Layout::PlanarYuv;
            l.planes = 3;
            l.plane[0] = {r, 0, 0, false};
            l.plane[1] = {r, d->log2_chroma_w, d->log2_chroma_h, false};
            l.plane[2] = {r, d->log2_chroma_w, d->log2_chroma_h, false};
            l.shift = d->comp[0].shift;
            return l;
        }
        if (d->comp[0].plane == 0 && d->comp[1].plane == 1 && d->comp[2].plane == 1) {
            // Semi-planar: interleaved chroma (NV12: U first, NV21: V first).
            const int bpc = wide ? 2 : 1;
            l.layout = Layout::SemiYuv;
            l.planes = 2;
            l.plane[0] = {r, 0, 0, false};
            l.plane[1] = {rg, d->log2_chroma_w, d->log2_chroma_h, false};
            l.swizzle[0] = d->comp[1].offset / bpc;
            l.swizzle[1] = d->comp[2].offset / bpc;
            l.shift = d->comp[0].shift;  // P010/P016: MSB-aligned codes
            if (d->comp[1].shift != l.shift || d->comp[2].shift != l.shift)
                return std::nullopt;
            return l;
        }
        return std::nullopt;
    }

    // Packed 4:2:2 (YUYV, UYVY, YVYU): 4 bytes per 2 pixels.
    if (!wide && d->nb_components == 3 && d->log2_chroma_w == 1 && d->log2_chroma_h == 0 && d->comp[0].step == 2 &&
        d->comp[1].step == 4 && d->comp[2].step == 4) {
        l.layout = Layout::Packed422;
        l.planes = 1;
        l.plane[0] = {TexFormat::RGBA8UI, 0, 0, true};
        const int y0 = d->comp[0].offset;
        l.swizzle = {y0, y0 + 2, d->comp[1].offset, d->comp[2].offset};
        return l;
    }
    return std::nullopt;
}

} // namespace scvp::render
