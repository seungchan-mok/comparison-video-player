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
    m_digits.release(m_gl);
    m_readback.release(m_gl);
    if (m_vao)
        m_gl->glDeleteVertexArrays(1, &m_vao);
    m_vao = 0;
    m_shaders.clear();
    m_gl = nullptr;
}

bool Renderer::prepare(Slot& s, const FramePtr& frame, const ColorOverrides& overrides, bool codes)
{
    if (!frame) {
        s.valid = false;
        s.hold.reset();
        s.shown = nullptr;
        return false;
    }
    const bool sameColor = s.overrides.matrix == overrides.matrix && s.overrides.range == overrides.range;
    if (s.valid && s.shown == frame.get() && sameColor && (s.codes || !codes))
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
    s.gray = source->layout().isGray();
    s.codes = codes;
    if (!s.conv.convert(m_gl, m_shaders, *source, s.color, codes)) {
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

void Renderer::drawPane(int mode, const Slot& a, const Slot& b, const RenderState& state, QRect pane)
{
    QOpenGLShaderProgram* prog = m_shaders.program(QStringLiteral("fullscreen.vert"), QStringLiteral("display.frag"), QByteArray());
    if (!prog) {
        m_error = m_shaders.lastError();
        return;
    }
    GL* gl = m_gl;
    const ViewTransform& view = state.view;
    const double scale = view.scale();
    const QSizeF viewSize = view.imageSize();
    auto k = [&](const Slot& s) { return QSizeF(s.conv.width() / viewSize.width(), s.conv.height() / viewSize.height()); };
    const QSizeF k0 = k(a), k1 = mode ? k(b) : QSizeF(1, 1);
    const float lod = scale < 1.0 ? float(std::log2(1.0 / scale)) : 0.0f;
    if (lod > 0.0f) {
        const_cast<RgbConverter&>(a.conv).ensureMipmaps(gl);
        if (mode)
            const_cast<RgbConverter&>(b.conv).ensureMipmaps(gl);
    }
    // GL viewport origin is bottom-left.
    const QSize fb = state.framebufferSize;
    const QRect vp(pane.x(), fb.height() - pane.y() - pane.height(), pane.width(), pane.height());
    gl->glViewport(vp.x(), vp.y(), vp.width(), vp.height());
    prog->bind();
    const GLuint id = prog->programId();
    auto loc = [&](const char* n) { return gl->glGetUniformLocation(id, n); };
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, a.conv.rgbTexture());
    gl->glActiveTexture(GL_TEXTURE1);
    gl->glBindTexture(GL_TEXTURE_2D, mode ? b.conv.rgbTexture() : a.conv.rgbTexture());
    // Digits and grid, once the cells are big enough.
    const double dpr = state.devicePixelRatio > 0 ? state.devicePixelRatio : 1.0;
    const PixelOverlaySettings& ov = state.overlay;
    const bool magnified = lod <= 0.0f;
    const bool codes = ov.mode == ValueMode::Codes && a.conv.codesTexture() && (!mode || b.conv.codesTexture());
    const int maxCode = (1 << a.conv.bitDepth()) - 1;
    const ValueGeometry geom = ValueGeometry::compute(maxCode, a.gray && (!mode || b.gray), mode == 2, scale, dpr);
    const int values = ov.values && magnified && geom.visible ? (codes ? 2 : 1) : 0;
    if (values && !m_digits.texture())
        m_digits.create(gl);
    gl->glActiveTexture(GL_TEXTURE2);
    gl->glBindTexture(GL_TEXTURE_2D, values == 2 ? a.conv.codesTexture() : 0);
    gl->glActiveTexture(GL_TEXTURE3);
    gl->glBindTexture(GL_TEXTURE_2D, values == 2 && mode ? b.conv.codesTexture() : 0);
    gl->glActiveTexture(GL_TEXTURE4);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, values ? m_digits.texture() : 0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glUniform1i(loc("img0"), 0);
    gl->glUniform1i(loc("img1"), 1);
    gl->glUniform1i(loc("codes0"), 2);
    gl->glUniform1i(loc("codes1"), 3);
    gl->glUniform1i(loc("digits"), 4);
    gl->glUniform1i(loc("values"), values);
    gl->glUniform1f(loc("maxCode0"), float(maxCode));
    gl->glUniform1f(loc("maxCode1"), float((1 << (mode ? b.conv.bitDepth() : a.conv.bitDepth())) - 1));
    gl->glUniform1i(loc("rows"), geom.rows);
    gl->glUniform1i(loc("field"), geom.field);
    gl->glUniform2f(loc("glyph"), float(geom.glyphW), float(geom.glyphH));
    gl->glUniform1f(loc("rowH"), float(geom.rowH));
    const bool outline = state.probe && magnified && scale >= 4.0 * dpr;
    gl->glUniform2f(loc("cursor"), outline ? float(std::floor(state.probePos.x())) : -1.0f,
                    outline ? float(std::floor(state.probePos.y())) : -1.0f);
    gl->glUniform1f(loc("gridAlpha"), ov.grid && magnified && scale >= kGridMinLogicalPx * dpr ? 0.25f : 0.0f);
    gl->glUniform2f(loc("size0"), float(a.conv.width()), float(a.conv.height()));
    gl->glUniform2f(loc("size1"), float(mode ? b.conv.width() : a.conv.width()), float(mode ? b.conv.height() : a.conv.height()));
    gl->glUniform2f(loc("k0"), float(k0.width()), float(k0.height()));
    gl->glUniform2f(loc("k1"), float(k1.width()), float(k1.height()));
    gl->glUniform2f(loc("viewSize"), float(viewSize.width()), float(viewSize.height()));
    gl->glUniform4f(loc("vp"), float(vp.x()), float(vp.y()), float(vp.width()), float(vp.height()));
    const QPointF t = view.renderTranslation();
    gl->glUniform2f(loc("T"), float(t.x()), float(t.y()));
    gl->glUniform1f(loc("S"), float(scale));
    gl->glUniform1f(loc("lod"), lod);
    const QColor& bg = state.background;
    gl->glUniform3f(loc("bg"), float(bg.redF()), float(bg.greenF()), float(bg.blueF()));
    gl->glUniform1i(loc("mode"), mode);
    gl->glUniform1f(loc("wipeX"), float(std::floor(state.compare.wipe * pane.width())) + 0.5f);
    gl->glUniform1f(loc("gain"), float(state.compare.diffGain));
    const QColor& line = state.divider;
    gl->glUniform3f(loc("lineColor"), float(line.redF()), float(line.greenF()), float(line.blueF()));
    gl->glBindVertexArray(m_vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    prog->release();
    gl->glActiveTexture(GL_TEXTURE4);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    gl->glActiveTexture(GL_TEXTURE0);
}

void Renderer::render(const RenderState& state, GLuint targetFbo)
{
    if (!m_gl)
        return;
    m_timings = {};
    for (int i = 0; i < 2; ++i)
        prepare(m_slots[size_t(i)], state.frames[size_t(i)], state.color[size_t(i)],
                (state.overlay.values && state.overlay.mode == ValueMode::Codes) || state.probe);
    m_readback.poll(m_gl);

    GL* gl = m_gl;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    const GLenum back = targetFbo ? GL_COLOR_ATTACHMENT0 : GL_BACK;
    gl->glDrawBuffers(1, &back);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_SCISSOR_TEST);
    const QSize fb = state.framebufferSize;
    gl->glViewport(0, 0, fb.width(), fb.height());
    const Slot& a = m_slots[0];
    const Slot& b = m_slots[1];
    m_layout = CompareLayout::compute(state.compare, fb, state.view.imageSize(), a.valid, b.valid);
    // The divider colour fills the gap between split panes.
    const QColor& clear = m_layout.panes.size() > 1 ? state.divider : state.background;
    gl->glClearColor(float(clear.redF()), float(clear.greenF()), float(clear.blueF()), 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    if (!a.valid && !b.valid)
        return;
    const CompareMode mode = state.compare.mode;
    for (const Pane& pane : m_layout.panes) {
        const Slot& first = m_slots[size_t(pane.slot)];
        if (!first.valid)
            continue;
        int shaderMode = 0;
        if (a.valid && b.valid && m_layout.panes.size() == 1)
            shaderMode = mode == CompareMode::Wipe ? 1 : mode == CompareMode::Difference ? 2 : 0;
        drawPane(shaderMode, first, b, state, pane.rect);
    }
    if (state.probe) {
        const QSizeF view = state.view.imageSize();
        std::array<const RgbConverter*, 2> conv{};
        std::array<QPoint, 2> texel{};
        std::array<bool, 2> gray{};
        for (int i = 0; i < 2; ++i) {
            const Slot& sl = m_slots[size_t(i)];
            if (!sl.valid)
                continue;
            conv[size_t(i)] = &sl.conv;
            // Same mapping as the display shader: floor(x * k).
            texel[size_t(i)] = QPoint(int(std::floor(state.probePos.x() * sl.conv.width() / view.width())),
                                      int(std::floor(state.probePos.y() * sl.conv.height() / view.height())));
            gray[size_t(i)] = sl.gray;
        }
        if (state.probePos.x() >= 0 && state.probePos.y() >= 0 && state.probePos.x() < view.width() && state.probePos.y() < view.height())
            m_readback.request(gl, conv, texel, gray, state.probePos);
        else
            m_readback.clear();
        gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    }
}

} // namespace scvp::render
