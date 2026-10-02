#include "render/TextureSource.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace scvp::render {

void CpuUploadSource::ensureTexture(GL* gl, Set& s, int plane, TexFormat fmt, int w, int h)
{
    GLuint& tex = s.tex[size_t(plane)];
    if (tex && s.w[plane] == w && s.h[plane] == h && s.fmt[plane] == fmt)
        return;
    if (!tex)
        gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    // Integer textures must use NEAREST, otherwise they are incomplete and read as zero.
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    const GlTexFormat f = glFormat(fmt);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, f.internal, w, h, 0, f.format, f.type, nullptr);
    s.w[plane] = w;
    s.h[plane] = h;
    s.fmt[plane] = fmt;
}

bool CpuUploadSource::update(GL* gl, const Frame& frame)
{
    const AVFrame* av = frame.av;
    if (!av)
        return false;
    const auto layout = PixelLayout::of(frame.format());
    if (!layout || av->width <= 0 || av->height <= 0)
        return false;
    const int maxSize = maxTextureSize(gl);
    if (av->width > maxSize || av->height > maxSize)
        return false;

    const auto t0 = std::chrono::steady_clock::now();
    m_layout = *layout;
    m_width = av->width;
    m_height = av->height;
    m_flipY = av->linesize[0] < 0;
    m_current ^= 1;
    Set& s = m_sets[m_current];
    size_t bytes = 0;

    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < m_layout.planes; ++i) {
        const PlaneSpec& ps = m_layout.plane[size_t(i)];
        const int w = m_layout.planeWidth(i, m_width);
        const int h = m_layout.planeHeight(i, m_height);
        ensureTexture(gl, s, i, ps.format, w, h);
        const GlTexFormat f = glFormat(ps.format);
        const int bpt = bytesPerTexel(ps.format);
        const int ls = av->linesize[i];
        const int stride = std::abs(ls);
        // With a negative linesize data[i] is the top row and rows go up in memory: upload from
        // the lowest address (bottom row) and let the shader flip.
        const uint8_t* base = ls >= 0 ? av->data[i] : av->data[i] + ptrdiff_t(h - 1) * ls;
        gl->glBindTexture(GL_TEXTURE_2D, s.tex[i]);
        if (stride % bpt == 0) {
            gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / bpt);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, f.format, f.type, base);
        } else {
            // Rows not a whole number of texels apart (RGB24 with odd widths): repack tightly.
            const size_t rowBytes = size_t(w) * size_t(bpt);
            m_repack.resize(rowBytes * size_t(h));
            for (int y = 0; y < h; ++y)
                std::memcpy(m_repack.data() + size_t(y) * rowBytes, base + ptrdiff_t(y) * stride, rowBytes);
            gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, f.format, f.type, m_repack.data());
        }
        bytes += size_t(w) * size_t(h) * size_t(bpt);
    }
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    m_lastUploadBytes = bytes;
    m_lastUploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

void CpuUploadSource::release(GL* gl)
{
    for (Set& s : m_sets)
        for (GLuint& t : s.tex)
            if (t) {
                gl->glDeleteTextures(1, &t);
                t = 0;
            }
}

} // namespace scvp::render
