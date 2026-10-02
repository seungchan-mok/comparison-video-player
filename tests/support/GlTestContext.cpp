#include "support/GlTestContext.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>

namespace scvp::test {

bool GlTestContext::create()
{
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setProfile(QSurfaceFormat::CoreProfile);
#if defined(Q_OS_MACOS)
    f.setVersion(4, 1);
#else
    f.setVersion(3, 3);
#endif
    m_ctx = std::make_unique<QOpenGLContext>();
    m_ctx->setFormat(f);
    if (!m_ctx->create())
        return false;
    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setFormat(m_ctx->format());
    m_surface->create();
    if (!m_ctx->makeCurrent(m_surface.get()))
        return false;
    m_gl = QOpenGLVersionFunctionsFactory::get<render::GL>(m_ctx.get());
    if (!m_gl)
        return false;
    m_gl->initializeOpenGLFunctions();
    return true;
}

QString GlTestContext::renderer() const
{
    if (!m_gl)
        return {};
    return QString::fromLatin1(reinterpret_cast<const char*>(m_gl->glGetString(GL_RENDERER)));
}

} // namespace scvp::test
