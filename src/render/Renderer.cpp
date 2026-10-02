#include "render/Renderer.h"

#include "engine/FrameAllocator.h"
#include "engine/GpuFrame.h"

#include <chrono>
#include <cmath>

namespace scvp::render {

bool Renderer::initialize(GL* gl)
{
    m_gl = gl;
    gl->glGenVertexArrays(1, &m_vao);
    return true;
}

void Renderer::release()
{
    if (!m_gl)
        return;
    for (Slot& s : m_slots) {
        s.upload.release(m_gl);
        s.interop.release(m_gl);
        s.conv.release(m_gl);
        s.hold.reset();
        s.shown = nullptr;
        s.valid = false;
    }
    if (m_vao)
        m_gl->glDeleteVertexArrays(1, &m_vao);
    m_vao = 0;
    m_shaders.clear();
    m_gl = nullptr;
}

bool Renderer::prepare(Slot& s, const FramePtr& frame, const ColorOverrides& overrides)
{
    if (!frame) {
        s.valid = false;
        s.hold.reset();
        s.shown = nullptr;
        return false;
    }
    const bool sameColor = s.overrides.matrix == overrides.matrix && s.overrides.range == overrides.range;
    if (s.valid && s.shown == frame.get() && sameColor)
        return true;
    const auto t0 = std::chrono::steady_clock::now();
    const TextureSource* source = nullptr;
    if (frame->location == FrameLocation::Cuda) {
        if (s.interop.update(m_gl, *frame)) {
            source = &s.interop;
            m_timings.zeroCopy = true;
            s.path = UploadPath::ZeroCopy;
        } else if (frame->gpu && frame->gpu->pool) {
            // No interop with this GPU: bring the pixels to system memory once.
            FramePool tmp("interop-fallback");
            Frame cpu;
            cpu.av = frame->gpu->pool->download(*frame->gpu, tmp);
            if (cpu.av) {
                av_frame_copy_props(cpu.av, frame->av);
                if (s.upload.update(m_gl, cpu)) {
                    source = &s.upload;
                    s.path = UploadPath::Download;
                }
            }
        }
    } else if (s.upload.update(m_gl, *frame)) {
        source = &s.upload;
        s.path = UploadPath::Upload;
    }
    if (!source) {
        m_error = QStringLiteral("cannot display pixel format %1").arg(QString::fromLatin1(av_get_pix_fmt_name(frame->format())));
        s.valid = false;
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    s.overrides = overrides;
    s.color = ColorParams::resolve(source->layout(), frame->av->colorspace, frame->av->color_range, frame->height(), overrides);
    if (!s.conv.convert(m_gl, m_shaders, *source, s.color, false)) {
        m_error = s.conv.error();
        s.valid = false;
        return false;
    }
    const auto t2 = std::chrono::steady_clock::now();
    s.shown = frame.get();
    s.hold = frame;
    s.valid = true;
    m_timings.uploadMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    m_timings.convertMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
    if (source == &s.upload)
        m_timings.uploadBytes += s.upload.lastUploadBytes();
    m_timings.converted = true;
    return true;
}

void Renderer::drawView(const Slot& s, const ViewTransform& view, QRect viewport, const QColor& bg)
{
    QOpenGLShaderProgram* prog = m_shaders.program(QStringLiteral("fullscreen.vert"), QStringLiteral("display.frag"), QByteArray());
    if (!prog) {
        m_error = m_shaders.lastError();
        return;
    }
    GL* gl = m_gl;
    const RgbConverter& conv = s.conv;
    const double scale = view.scale();
    const float lod = scale < 1.0 ? float(std::log2(1.0 / scale)) : 0.0f;
    if (lod > 0.0f)
        const_cast<RgbConverter&>(conv).ensureMipmaps(gl);
    gl->glViewport(viewport.x(), viewport.y(), viewport.width(), viewport.height());
    prog->bind();
    const GLuint id = prog->programId();
    auto loc = [&](const char* n) { return gl->glGetUniformLocation(id, n); };
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, conv.rgbTexture());
    gl->glUniform1i(loc("img"), 0);
    gl->glUniform2f(loc("imgSize"), float(conv.width()), float(conv.height()));
    gl->glUniform4f(loc("vp"), float(viewport.x()), float(viewport.y()), float(viewport.width()), float(viewport.height()));
    const QPointF t = view.renderTranslation();
    gl->glUniform2f(loc("T"), float(t.x()), float(t.y()));
    gl->glUniform1f(loc("S"), float(scale));
    gl->glUniform1f(loc("lod"), lod);
    gl->glUniform3f(loc("bg"), float(bg.redF()), float(bg.greenF()), float(bg.blueF()));
    gl->glBindVertexArray(m_vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    prog->release();
}

void Renderer::render(const RenderState& state, GLuint targetFbo)
{
    if (!m_gl)
        return;
    m_timings = {};
    for (int i = 0; i < 2; ++i)
        prepare(m_slots[size_t(i)], state.frames[size_t(i)], state.color[size_t(i)]);

    GL* gl = m_gl;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    const GLenum back = targetFbo ? GL_COLOR_ATTACHMENT0 : GL_BACK;
    gl->glDrawBuffers(1, &back);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_SCISSOR_TEST);
    const QSize fb = state.framebufferSize;
    gl->glViewport(0, 0, fb.width(), fb.height());
    gl->glClearColor(float(state.background.redF()), float(state.background.greenF()), float(state.background.blueF()), 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    const Slot& a = m_slots[0];
    if (a.valid)
        drawView(a, state.view, QRect(0, 0, fb.width(), fb.height()), state.background);
}

} // namespace scvp::render
