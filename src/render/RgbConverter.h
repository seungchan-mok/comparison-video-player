#pragma once

#include "render/ColorParams.h"
#include "render/GlCommon.h"
#include "render/ShaderLibrary.h"
#include "render/TextureSource.h"

#include <vector>

namespace scvp::render {

// Converts the current frame of a TextureSource to RGB at source resolution, once per frame
// change. Output: RGBA8 for 8-bit sources, RGBA16 for deeper ones (values pre-quantised to the
// source bit depth, so digits read back exactly), with a full mip chain for minified display;
// optionally a second RGBA16UI attachment with the raw code values.
class RgbConverter {
public:
    bool convert(GL* gl, ShaderLibrary& shaders, const TextureSource& src, const ColorParams& color, bool emitCodes);
    void ensureMipmaps(GL* gl);
    void release(GL* gl);

    GLuint rgbTexture() const { return m_rgb; }
    GLuint codesTexture() const { return m_hasCodes ? m_codes : 0; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    bool deep() const { return m_deep; }
    int bitDepth() const { return m_bitDepth; }
    QString error() const { return m_error; }

    // Read-back helpers (tests, inspector fallback). Values are in [0, 2^bitDepth - 1].
    std::vector<uint16_t> readRgb(GL* gl) const;   // w*h*3, row 0 = top
    std::vector<uint16_t> readCodes(GL* gl) const; // w*h*3, empty without codes

private:
    void allocate(GL* gl, int w, int h, bool deep, bool codes);

    GLuint m_fbo = 0;
    GLuint m_rgb = 0;
    GLuint m_codes = 0;
    GLuint m_vao = 0;
    int m_w = 0;
    int m_h = 0;
    int m_levels = 1;
    int m_bitDepth = 8;
    bool m_deep = false;
    bool m_hasCodes = false;
    bool m_mipsDirty = true;
    QString m_error;
};

} // namespace scvp::render
