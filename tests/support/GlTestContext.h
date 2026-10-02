#pragma once

#include "render/GlCommon.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>

#include <memory>

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

} // namespace scvp::test
