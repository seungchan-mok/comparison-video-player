#pragma once

#include "engine/Frame.h"
#include "render/GlCommon.h"
#include "render/PixelLayout.h"

namespace scvp::render {

// Gives the conversion pass the planes of a frame as integer textures. Implementations differ
// in where the pixels come from: system memory uploads, or GPU memory mapped through interop.
class TextureSource {
public:
    virtual ~TextureSource() = default;
    // Makes `frame` current; false when its format cannot be shown without conversion.
    virtual bool update(GL* gl, const Frame& frame) = 0;
    virtual GLuint texture(int plane) const = 0;
    virtual const PixelLayout& layout() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool flipY() const = 0;  // rows were uploaded bottom-up (negative linesize)
    virtual void release(GL* gl) = 0;
};

// Uploads frames from system memory with glTexSubImage2D into two alternating texture sets
// (the previous frame's textures may still be in flight on the GPU).
class CpuUploadSource final : public TextureSource {
public:
    bool update(GL* gl, const Frame& frame) override;
    GLuint texture(int plane) const override { return m_sets[m_current].tex[size_t(plane)]; }
    const PixelLayout& layout() const override { return m_layout; }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    bool flipY() const override { return m_flipY; }
    void release(GL* gl) override;

    double lastUploadMs() const { return m_lastUploadMs; }
    size_t lastUploadBytes() const { return m_lastUploadBytes; }

private:
    struct Set {
        GLuint tex[4] = {};
        int w[4] = {};
        int h[4] = {};
        TexFormat fmt[4] = {};
    };
    void ensureTexture(GL* gl, Set& s, int plane, TexFormat fmt, int w, int h);

    Set m_sets[2];
    int m_current = 0;
    PixelLayout m_layout;
    int m_width = 0;
    int m_height = 0;
    bool m_flipY = false;
    std::vector<uint8_t> m_repack;
    double m_lastUploadMs = 0;
    size_t m_lastUploadBytes = 0;
};

} // namespace scvp::render
