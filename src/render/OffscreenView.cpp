#include "render/OffscreenView.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scvp::render {

bool OffscreenView::create(GL* gl, QSize size, QString* error)
{
    release();
    m_gl = gl;
    m_size = size;
    auto fail = [&](const QString& what) {
        if (error)
            *error = what;
        release();
        return false;
    };
    if (size.isEmpty() || size.width() > maxTextureSize(gl) || size.height() > maxTextureSize(gl))
        return fail(QStringLiteral("%1x%2 is larger than this GPU can draw (%3 px a side)")
                        .arg(size.width()).arg(size.height()).arg(maxTextureSize(gl)));
    while (gl->glGetError() != GL_NO_ERROR) {
    }
    gl->glGenTextures(1, &m_tex);
    gl->glBindTexture(GL_TEXTURE_2D, m_tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glGenFramebuffers(1, &m_fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
    const bool complete = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const GLsizeiptr bytes = GLsizeiptr(size.width()) * size.height() * 4;
    for (Readback& r : m_reads) {
        gl->glGenBuffers(1, &r.pbo);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, r.pbo);
        gl->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl->glGenVertexArrays(1, &m_vao);
    if (!complete || gl->glGetError() != GL_NO_ERROR)
        return fail(QStringLiteral("the GPU cannot hold a %1x%2 picture").arg(size.width()).arg(size.height()));
    return true;
}

void OffscreenView::release()
{
    if (!m_gl)
        return;
    GL* gl = m_gl;
    for (Readback& r : m_reads) {
        if (r.fence)
            gl->glDeleteSync(r.fence);
        if (r.pbo)
            gl->glDeleteBuffers(1, &r.pbo);
        r = {};
    }
    for (Layer& l : m_layers)
        if (l.tex)
            gl->glDeleteTextures(1, &l.tex);
    m_layers.clear();
    if (m_fbo)
        gl->glDeleteFramebuffers(1, &m_fbo);
    if (m_tex)
        gl->glDeleteTextures(1, &m_tex);
    if (m_vao)
        gl->glDeleteVertexArrays(1, &m_vao);
    m_fbo = m_tex = m_vao = 0;
    m_shaders.clear();
    m_next = m_pending = 0;
    m_gl = nullptr;
}

void OffscreenView::setLayer(int index, const QImage& image, QPoint origin)
{
    if (index < 0)
        return;
    if (size_t(index) >= m_layers.size())
        m_layers.resize(size_t(index) + 1);
    Layer& l = m_layers[size_t(index)];
    l.origin = origin;
    if (image.isNull()) {
        l.image = QImage();
        l.key = 0;
        return;
    }
    if (l.tex && image.cacheKey() == l.key)
        return;
    l.image = image.format() == QImage::Format_ARGB32_Premultiplied ? image
                                                                      : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    l.key = image.cacheKey();
    GL* gl = m_gl;
    if (!gl)
        return;
    if (!l.tex)
        gl->glGenTextures(1, &l.tex);
    gl->glBindTexture(GL_TEXTURE_2D, l.tex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, int(l.image.bytesPerLine() / 4));
    // 0xAARRGGBB words: B, G, R, A bytes in memory on little-endian, as GL_BGRA + 8_8_8_8_REV.
    if (l.texSize != l.image.size()) {
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, l.image.width(), l.image.height(), 0, GL_BGRA,
                         GL_UNSIGNED_INT_8_8_8_8_REV, l.image.constBits());
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        l.texSize = l.image.size();
    } else {
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, l.image.width(), l.image.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                            l.image.constBits());
    }
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    l.image = QImage();  // on the GPU now
}

bool OffscreenView::render(Renderer& renderer, const RenderState& state)
{
    if (!m_gl || m_pending >= kInFlight || state.framebufferSize != m_size)
        return false;
    GL* gl = m_gl;
    renderer.render(state, m_fbo);

    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    QOpenGLShaderProgram* prog = nullptr;
    for (const Layer& l : m_layers) {
        if (!l.key || !l.tex)
            continue;
        if (!prog) {
            prog = m_shaders.program(QStringLiteral("fullscreen.vert"), QStringLiteral("overlay.frag"), QByteArray());
            if (!prog)
                return false;
            prog->bind();
            gl->glUniform1i(gl->glGetUniformLocation(prog->programId(), "layer"), 0);
            gl->glEnable(GL_BLEND);
            gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied "source over"
            gl->glBindVertexArray(m_vao);
            gl->glActiveTexture(GL_TEXTURE0);
        }
        // GL viewport origin is bottom-left; the part outside the picture is clipped.
        const QRect vp(l.origin.x(), m_size.height() - l.origin.y() - l.texSize.height(), l.texSize.width(),
                       l.texSize.height());
        gl->glViewport(vp.x(), vp.y(), vp.width(), vp.height());
        gl->glUniform4f(gl->glGetUniformLocation(prog->programId(), "rect"), float(vp.x()), float(vp.y()),
                        float(vp.width()), float(vp.height()));
        gl->glBindTexture(GL_TEXTURE_2D, l.tex);
        gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (prog) {
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glBindVertexArray(0);
        gl->glDisable(GL_BLEND);
        prog->release();
        gl->glViewport(0, 0, m_size.width(), m_size.height());
    }

    Readback& r = m_reads[size_t(m_next)];
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, r.pbo);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    // 0xffRRGGBB words: the BGRX bytes the encoder takes. Rows come bottom-up.
    gl->glReadPixels(0, 0, m_size.width(), m_size.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    r.fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glFlush();  // start the GPU now: take() may poll without flushing
    if (!r.fence)
        return false;
    m_next = (m_next + 1) % kInFlight;
    ++m_pending;
    return true;
}

bool OffscreenView::take(std::vector<uint8_t>& out, bool wait)
{
    if (!m_gl || !m_pending)
        return false;
    GL* gl = m_gl;
    Readback& r = m_reads[size_t((m_next - m_pending + kInFlight) % kInFlight)];
    for (;;) {
        const GLenum st = gl->glClientWaitSync(r.fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? GLuint64(100'000'000) : 0);
        if (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED)
            break;
        if (st == GL_WAIT_FAILED || !wait)
            return false;
    }
    gl->glDeleteSync(r.fence);
    r.fence = nullptr;
    --m_pending;
    const size_t bytes = size_t(m_size.width()) * size_t(m_size.height()) * 4;
    out.resize(bytes);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, r.pbo);
    const void* p = gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(bytes), GL_MAP_READ_BIT);
    const bool ok = p != nullptr;
    if (ok) {
        std::memcpy(out.data(), p, bytes);
        gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return ok;
}

RenderState OffscreenView::fullFrameState(const RenderState& like, QSizeF image, bool hasA, bool hasB, double scale)
{
    RenderState s = like;
    s.probe = false;
    s.sampleProbe = false;
    s.devicePixelRatio = 1.0;
    s.lineScale = 1.0;
    if (s.compare.orientation == SplitOrientation::Auto)
        s.compare.orientation = SplitOrientation::SideBySide;
    const QSize pane(std::max(1, int(std::lround(image.width() * scale))), std::max(1, int(std::lround(image.height() * scale))));
    const bool split = hasA && hasB && s.compare.mode == CompareMode::Split;
    const int divider = CompareLayout::kDivider;
    if (!split)
        s.framebufferSize = pane;
    else if (s.compare.orientation == SplitOrientation::SideBySide)
        s.framebufferSize = QSize(2 * pane.width() + divider, pane.height());
    else
        s.framebufferSize = QSize(pane.width(), 2 * pane.height() + divider);
    s.view = ViewTransform();
    s.view.setImageSize(image);
    s.view.setViewportSize(QSizeF(pane));
    s.view.fit();
    return s;
}

} // namespace scvp::render
