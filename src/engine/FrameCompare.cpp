#include "engine/FrameCompare.h"

#include <cstring>
#include <vector>

namespace scvp {
namespace {

constexpr uint64_t kUnsupported = AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM;

// Planes 1 and 2 are the subsampled ones (U/V, or the interleaved UV plane of NV12 in plane 1).
int planeHeight(const AVPixFmtDescriptor* d, int plane, int height)
{
    return plane == 1 || plane == 2 ? AV_CEIL_RSHIFT(height, d->log2_chroma_h) : height;
}

template <typename T>
const T* row(const AVFrame* f, int plane, int y)
{
    return reinterpret_cast<const T*>(f->data[plane] + ptrdiff_t(y) * f->linesize[plane]);
}

// 4:2:0 semi-planar (NV12, P010, P016: values in the top bits) against planar, the pair every
// hardware check compares. Native-endian reads; pixdesc's generic per-sample reader takes ~4x
// as long (8 ms for a 4K picture).
template <typename T>
bool sameSemiPlanar(const AVFrame* semi, const AVFrame* planar, int shift)
{
    for (int y = 0; y < semi->height; ++y) {
        const T* s = row<T>(semi, 0, y);
        const T* p = row<T>(planar, 0, y);
        for (int x = 0; x < semi->width; ++x)
            if (T(s[x] >> shift) != p[x])
                return false;
    }
    const int cw = AV_CEIL_RSHIFT(semi->width, 1);
    for (int y = 0; y < AV_CEIL_RSHIFT(semi->height, 1); ++y) {
        const T* uv = row<T>(semi, 1, y);
        const T* u = row<T>(planar, 1, y);
        const T* v = row<T>(planar, 2, y);
        for (int x = 0; x < cw; ++x)
            if (T(uv[2 * x] >> shift) != u[x] || T(uv[2 * x + 1] >> shift) != v[x])
                return false;
    }
    return true;
}

// -1 when (semi, planar) is not one of the pairs sameSemiPlanar handles, else the value shift.
int semiPlanarShift(AVPixelFormat semi, AVPixelFormat planar)
{
    if (AV_HAVE_BIGENDIAN)
        return -1;
    if (semi == AV_PIX_FMT_NV12 && planar == AV_PIX_FMT_YUV420P)
        return 0;
    if (semi == AV_PIX_FMT_P010LE && planar == AV_PIX_FMT_YUV420P10LE)
        return 6;
    if (semi == AV_PIX_FMT_P016LE && planar == AV_PIX_FMT_YUV420P16LE)
        return 0;
    return -1;
}

} // namespace

bool samePixelValues(const AVFrame* a, const AVFrame* b)
{
    if (a->width != b->width || a->height != b->height)
        return false;
    const auto fa = static_cast<AVPixelFormat>(a->format);
    const auto fb = static_cast<AVPixelFormat>(b->format);
    const AVPixFmtDescriptor* da = av_pix_fmt_desc_get(fa);
    const AVPixFmtDescriptor* db = av_pix_fmt_desc_get(fb);
    if (!da || !db || ((da->flags | db->flags) & kUnsupported))
        return false;

    if (fa == fb) {
        // Same layout: compare each row's used bytes (padding after them may differ).
        for (int p = 0; p < av_pix_fmt_count_planes(fa); ++p) {
            const int bytes = av_image_get_linesize(fa, a->width, p);
            if (bytes < 0)
                return false;
            for (int y = 0; y < planeHeight(da, p, a->height); ++y)
                if (std::memcmp(a->data[p] + ptrdiff_t(y) * a->linesize[p], b->data[p] + ptrdiff_t(y) * b->linesize[p], size_t(bytes)) != 0)
                    return false;
        }
        return true;
    }

    if (da->nb_components != db->nb_components || da->log2_chroma_w != db->log2_chroma_w ||
        da->log2_chroma_h != db->log2_chroma_h || (da->flags & AV_PIX_FMT_FLAG_RGB) != (db->flags & AV_PIX_FMT_FLAG_RGB))
        return false;
    for (int c = 0; c < da->nb_components; ++c)
        if (da->comp[c].depth != db->comp[c].depth)
            return false;

    for (int swap = 0; swap < 2; ++swap) {
        const AVFrame* semi = swap ? b : a;
        const AVFrame* planar = swap ? a : b;
        const int shift = semiPlanarShift(static_cast<AVPixelFormat>(semi->format), static_cast<AVPixelFormat>(planar->format));
        if (shift >= 0)
            return semi->format == AV_PIX_FMT_NV12 ? sameSemiPlanar<uint8_t>(semi, planar, shift)
                                                   : sameSemiPlanar<uint16_t>(semi, planar, shift);
    }

    // Other layouts: read each component's values line by line (pixdesc handles planes,
    // interleaving, bit shifts and endianness) and compare those.
    std::vector<uint16_t> la(size_t(a->width)), lb(size_t(a->width));
    const uint8_t** pa = const_cast<const uint8_t**>(a->data);
    const uint8_t** pb = const_cast<const uint8_t**>(b->data);
    for (int c = 0; c < da->nb_components; ++c) {
        const bool chroma = (c == 1 || c == 2) && !(da->flags & AV_PIX_FMT_FLAG_RGB);
        const int w = chroma ? AV_CEIL_RSHIFT(a->width, da->log2_chroma_w) : a->width;
        const int h = chroma ? AV_CEIL_RSHIFT(a->height, da->log2_chroma_h) : a->height;
        for (int y = 0; y < h; ++y) {
            av_read_image_line2(la.data(), pa, a->linesize, da, 0, y, c, w, 0, 2);
            av_read_image_line2(lb.data(), pb, b->linesize, db, 0, y, c, w, 0, 2);
            if (std::memcmp(la.data(), lb.data(), size_t(w) * sizeof(uint16_t)) != 0)
                return false;
        }
    }
    return true;
}

} // namespace scvp
