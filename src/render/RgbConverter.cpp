#include "render/RgbConverter.h"

#include <QOpenGLContext>

#include <algorithm>
#include <cmath>

namespace scvp::render {
namespace {

const char* layoutDefine(Layout l)
{
    switch (l) {
    case Layout::PlanarYuv: return "PLANAR_YUV";
    case Layout::SemiYuv: return "SEMI_YUV";
    case Layout::Packed422: return "PACKED_422";
    case Layout::PlanarRgb: return "PLANAR_RGB";
    case Layout::PackedRgb: return "PACKED_RGB";
    case Layout::Gray: return "GRAY";
    }
    return "PLANAR_YUV";
}

} // namespace

void RgbConverter::allocate(GL* gl, int w, int h, bool deep, bool codes)
{
    const bool sizeChanged = w != m_w || h != m_h || deep != m_deep;
    if (!m_vao)
        gl->glGenVertexArrays(1, &m_vao);
    if (!m_fbo)
        gl->glGenFramebuffers(1, &m_fbo);
    if (!m_rgb || sizeChanged) {
        if (!m_rgb)
            gl->glGenTextures(1, &m_rgb);
        gl->glBindTexture(GL_TEXTURE_2D, m_rgb);
        // Allocate the whole mip chain up front: a texture with missing levels is incomplete.
        m_levels = 1 + int(std::floor(std::log2(double(std::max(w, h)))));
        const GLint internal = deep ? GL_RGBA16 : GL_RGBA8;
        const GLenum type = deep ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        for (int lv = 0, lw = w, lh = h; lv < m_levels; ++lv, lw = std::max(1, lw / 2), lh = std::max(1, lh / 2))
            gl->glTexImage2D(GL_TEXTURE_2D, lv, internal, lw, lh, 0, GL_RGBA, type, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, m_levels - 1);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (codes && (!m_codes || sizeChanged || !m_hasCodes)) {
        if (!m_codes)
            gl->glGenTextures(1, &m_codes);
        gl->glBindTexture(GL_TEXTURE_2D, m_codes);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rgb, 0);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, codes ? m_codes : 0, 0);
    m_w = w;
    m_h = h;
    m_deep = deep;
    m_hasCodes = codes;
}

bool RgbConverter::convert(GL* gl, ShaderLibrary& shaders, const TextureSource& src, const ColorParams& color, bool emitCodes)
{
    const PixelLayout& l = src.layout();
    const bool deep = l.bitDepth > 8;
    allocate(gl, src.width(), src.height(), deep, emitCodes);
    m_bitDepth = l.bitDepth;
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        m_error = QStringLiteral("conversion framebuffer incomplete");
        return false;
    }
    QByteArray defines = layoutDefine(l.layout);
    if (emitCodes)
        defines += " EMIT_CODES";
    const bool linear = color.chromaBilinear && (l.layout == Layout::PlanarYuv || l.layout == Layout::SemiYuv) &&
                        (l.chromaShiftX || l.chromaShiftY);
    if (linear)
        defines += " CHROMA_LINEAR";
    QOpenGLShaderProgram* prog = shaders.program(QStringLiteral("fullscreen.vert"), QStringLiteral("convert.frag"), defines);
    if (!prog) {
        m_error = shaders.lastError();
        return false;
    }
    const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    gl->glDrawBuffers(emitCodes ? 2 : 1, buffers);
    gl->glViewport(0, 0, m_w, m_h);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_SCISSOR_TEST);

    prog->bind();
    const GLuint progId = prog->programId();
    auto loc = [&](const char* n) { return gl->glGetUniformLocation(progId, n); };
    for (int i = 0; i < 3; ++i) {
        gl->glActiveTexture(GLenum(GL_TEXTURE0 + i));
        gl->glBindTexture(GL_TEXTURE_2D, i < l.planes ? src.texture(i) : 0);
    }
    gl->glUniform1i(loc("t0"), 0);
    gl->glUniform1i(loc("t1"), 1);
    gl->glUniform1i(loc("t2"), 2);
    gl->glUniform2i(loc("chromaShift"), l.chromaShiftX, l.chromaShiftY);
    gl->glUniform1ui(loc("shift"), GLuint(l.shift));
    gl->glUniform1ui(loc("mask"), GLuint(l.mask));
    gl->glUniform4i(loc("swz"), l.swizzle[0], l.swizzle[1], l.swizzle[2], l.swizzle[3]);
    gl->glUniform3f(loc("off"), color.offset[0], color.offset[1], color.offset[2]);
    gl->glUniform3f(loc("scl"), color.scale[0], color.scale[1], color.scale[2]);
    gl->glUniformMatrix3fv(loc("M"), 1, GL_TRUE, color.matrix.data());  // row-major -> transpose
    gl->glUniform1f(loc("outMax"), color.outMax);
    gl->glUniform1i(loc("flipY"), src.flipY() ? 1 : 0);
    gl->glUniform1i(loc("height"), m_h);
    if (linear) {
        gl->glUniform2f(loc("siting"), color.siting.x, color.siting.y);
        gl->glUniform2i(loc("chromaSize"), AV_CEIL_RSHIFT(m_w, l.chromaShiftX), AV_CEIL_RSHIFT(m_h, l.chromaShiftY));
    }
    gl->glBindVertexArray(m_vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    prog->release();
    gl->glActiveTexture(GL_TEXTURE0);
    m_mipsDirty = true;
    return true;
}

void RgbConverter::ensureMipmaps(GL* gl)
{
    if (!m_mipsDirty || !m_rgb)
        return;
    gl->glBindTexture(GL_TEXTURE_2D, m_rgb);
    gl->glGenerateMipmap(GL_TEXTURE_2D);
    m_mipsDirty = false;
}

std::vector<uint16_t> RgbConverter::readRgb(GL* gl) const
{
    std::vector<uint16_t> out(size_t(m_w) * size_t(m_h) * 3);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const double maxCode = double((1u << m_bitDepth) - 1u);
    if (m_deep) {
        std::vector<uint16_t> raw(size_t(m_w) * size_t(m_h) * 4);
        gl->glReadPixels(0, 0, m_w, m_h, GL_RGBA, GL_UNSIGNED_SHORT, raw.data());
        for (size_t i = 0, n = size_t(m_w) * size_t(m_h); i < n; ++i)
            for (int c = 0; c < 3; ++c)
                out[i * 3 + size_t(c)] = uint16_t(std::lround(double(raw[i * 4 + size_t(c)]) * maxCode / 65535.0));
    } else {
        std::vector<uint8_t> raw(size_t(m_w) * size_t(m_h) * 4);
        gl->glReadPixels(0, 0, m_w, m_h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        for (size_t i = 0, n = size_t(m_w) * size_t(m_h); i < n; ++i)
            for (int c = 0; c < 3; ++c)
                out[i * 3 + size_t(c)] = raw[i * 4 + size_t(c)];
    }
    return out;
}

std::vector<uint16_t> RgbConverter::readCodes(GL* gl) const
{
    if (!m_hasCodes)
        return {};
    std::vector<uint16_t> raw(size_t(m_w) * size_t(m_h) * 4);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    gl->glReadBuffer(GL_COLOR_ATTACHMENT1);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels(0, 0, m_w, m_h, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, raw.data());
    std::vector<uint16_t> out(size_t(m_w) * size_t(m_h) * 3);
    for (size_t i = 0, n = size_t(m_w) * size_t(m_h); i < n; ++i)
        for (int c = 0; c < 3; ++c)
            out[i * 3 + size_t(c)] = raw[i * 4 + size_t(c)];
    return out;
}

void RgbConverter::release(GL* gl)
{
    if (m_fbo)
        gl->glDeleteFramebuffers(1, &m_fbo);
    if (m_rgb)
        gl->glDeleteTextures(1, &m_rgb);
    if (m_codes)
        gl->glDeleteTextures(1, &m_codes);
    if (m_vao)
        gl->glDeleteVertexArrays(1, &m_vao);
    m_fbo = m_rgb = m_codes = m_vao = 0;
    m_w = m_h = 0;
}

} // namespace scvp::render
