#pragma once

#include "render/GlCommon.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace scvp::test {

// A current OpenGL 3.3 core context on an offscreen surface (no window). Works with
// QT_QPA_PLATFORM=offscreen as long as an X display is reachable (GLX); tests QSKIP otherwise.
class GlTestContext {
public:
    bool create();
    render::GL* gl() const { return m_gl; }
    QOpenGLContext* context() const { return m_ctx.get(); }
    QString renderer() const;

private:
    std::unique_ptr<QOpenGLContext> m_ctx;
    std::unique_ptr<QOffscreenSurface> m_surface;
    render::GL* m_gl = nullptr;
};

// RGBA8 texture + FBO to render into and read back.
struct RenderTarget {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0;
    int h = 0;

    void create(render::GL* gl, int width, int height)
    {
        destroy(gl);
        w = width;
        h = height;
        gl->glGenTextures(1, &tex);
        gl->glBindTexture(GL_TEXTURE_2D, tex);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    void destroy(render::GL* gl)
    {
        if (fbo)
            gl->glDeleteFramebuffers(1, &fbo);
        if (tex)
            gl->glDeleteTextures(1, &tex);
        fbo = tex = 0;
    }
    // Pixel at device (x, y), top-left origin.
    std::array<int, 3> at(const std::vector<uint8_t>& px, int x, int y) const
    {
        const size_t i = (size_t(h - 1 - y) * size_t(w) + size_t(x)) * 4;
        return {px[i], px[i + 1], px[i + 2]};
    }
    std::vector<uint8_t> read(render::GL* gl) const
    {
        std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        return px;
    }
};

} // namespace scvp::test
