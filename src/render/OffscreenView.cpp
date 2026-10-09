#include "render/OffscreenView.h"

#include "engine/VideoEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scvp::render {

namespace {

GLuint planeTexture(GL* gl, GLint internal, GLenum format, QSize size)
{
    GLuint t = 0;
    gl->glGenTextures(1, &t);
    gl->glBindTexture(GL_TEXTURE_2D, t);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, internal, size.width(), size.height(), 0, format, GL_UNSIGNED_BYTE, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

} // namespace

size_t OffscreenView::pictureBytes() const
{
    if (m_layout == PlaneLayout::Bgrx)
        return size_t(m_size.width()) * size_t(m_size.height()) * 4;
    return size_t(m_planes.width()) * size_t(m_planes.height()) * 3 / 2;
}

bool OffscreenView::create(GL* gl, QSize size, QString* error, PlaneLayout layout)
{
    release();
    m_gl = gl;
    m_size = size;
    m_layout = layout;
    m_planes = layout == PlaneLayout::Bgrx ? size : QSize((size.width() + 1) & ~1, (size.height() + 1) & ~1);
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
    bool complete = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (layout != PlaneLayout::Bgrx) {
        const QSize half(m_planes.width() / 2, m_planes.height() / 2);
        m_lumaTex = planeTexture(gl, GL_R8, GL_RED, m_planes);
        gl->glGenFramebuffers(1, &m_lumaFbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, m_lumaFbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_lumaTex, 0);
        complete = complete && gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        gl->glGenFramebuffers(1, &m_chromaFbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, m_chromaFbo);
        if (layout == PlaneLayout::Nv12) {
            m_chromaTex[0] = planeTexture(gl, GL_RG8, GL_RG, half);
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_chromaTex[0], 0);
        } else {
            for (int i = 0; i < 2; ++i) {
                m_chromaTex[size_t(i)] = planeTexture(gl, GL_R8, GL_RED, half);
                gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GLenum(GL_COLOR_ATTACHMENT0 + i), GL_TEXTURE_2D, m_chromaTex[size_t(i)], 0);
            }
        }
        complete = complete && gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const GLsizeiptr bytes = GLsizeiptr(pictureBytes());
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
    for (GLuint* fbo : {&m_fbo, &m_lumaFbo, &m_chromaFbo})
        if (*fbo)
            gl->glDeleteFramebuffers(1, fbo);
    for (GLuint* tex : {&m_tex, &m_lumaTex, &m_chromaTex[0], &m_chromaTex[1]})
        if (*tex)
            gl->glDeleteTextures(1, tex);
    if (m_vao)
        gl->glDeleteVertexArrays(1, &m_vao);
    m_fbo = m_tex = m_vao = m_lumaTex = m_lumaFbo = m_chromaFbo = 0;
    m_chromaTex = {};
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

    if (m_layout != PlaneLayout::Bgrx && !convert())
        return false;

    Readback& r = m_reads[size_t(m_next)];
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, r.pbo);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (m_layout == PlaneLayout::Bgrx) {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
        gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        // 0xffRRGGBB words: the BGRX bytes the encoder takes. Rows come bottom-up.
        gl->glReadPixels(0, 0, m_size.width(), m_size.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    } else {
        // Packed planes, 1.5 bytes a pixel instead of 4: Y, then UV (NV12) or U and V (I420).
        const int W = m_planes.width(), H = m_planes.height();
        const auto at = [](size_t offset) { return reinterpret_cast<void*>(uintptr_t(offset)); };
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lumaFbo);
        gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
        gl->glReadPixels(0, 0, W, H, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_chromaFbo);
        if (m_layout == PlaneLayout::Nv12) {
            gl->glReadPixels(0, 0, W / 2, H / 2, GL_RG, GL_UNSIGNED_BYTE, at(size_t(W) * H));
        } else {
            gl->glReadPixels(0, 0, W / 2, H / 2, GL_RED, GL_UNSIGNED_BYTE, at(size_t(W) * H));
            gl->glReadBuffer(GL_COLOR_ATTACHMENT1);
            gl->glReadPixels(0, 0, W / 2, H / 2, GL_RED, GL_UNSIGNED_BYTE, at(size_t(W) * H + size_t(W / 2) * (H / 2)));
        }
        gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
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

bool OffscreenView::convert()
{
    GL* gl = m_gl;
    const QByteArray chroma = m_layout == PlaneLayout::I420 ? QByteArray("I420") : QByteArray();
    QOpenGLShaderProgram* luma = m_shaders.program(QStringLiteral("fullscreen.vert"), QStringLiteral("rgb2yuv.frag"), "LUMA");
    QOpenGLShaderProgram* uv = m_shaders.program(QStringLiteral("fullscreen.vert"), QStringLiteral("rgb2yuv.frag"), chroma);
    if (!luma || !uv)
        return false;
    gl->glDisable(GL_BLEND);
    gl->glBindVertexArray(m_vao);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, m_tex);
    auto pass = [&](QOpenGLShaderProgram* prog, GLuint fbo, QSize size, int outputs, const int* coef, const int* coefV) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        gl->glDrawBuffers(outputs, bufs);
        gl->glViewport(0, 0, size.width(), size.height());
        prog->bind();
        const GLuint id = prog->programId();
        gl->glUniform1i(gl->glGetUniformLocation(id, "picture"), 0);
        gl->glUniform2i(gl->glGetUniformLocation(id, "size"), m_size.width(), m_size.height());
        gl->glUniform3i(gl->glGetUniformLocation(id, "coef"), coef[0], coef[1], coef[2]);
        if (coefV)
            gl->glUniform3i(gl->glGetUniformLocation(id, "coefV"), coefV[0], coefV[1], coefV[2]);
        gl->glDrawArrays(GL_TRIANGLES, 0, 3);
        prog->release();
    };
    pass(luma, m_lumaFbo, m_planes, 1, bt709::kY, nullptr);
    pass(uv, m_chromaFbo, m_planes / 2, m_layout == PlaneLayout::I420 ? 2 : 1, bt709::kU, bt709::kV);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindVertexArray(0);
    return true;
}

template <class Fn>
bool OffscreenView::takeMapped(bool wait, Fn&& use)
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
    const size_t bytes = pictureBytes();
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, r.pbo);
    const void* p = gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(bytes), GL_MAP_READ_BIT);
    const bool ok = p != nullptr;
    if (ok) {
        use(static_cast<const uint8_t*>(p), bytes);
        gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return ok;
}

bool OffscreenView::take(std::vector<uint8_t>& out, bool wait)
{
    return takeMapped(wait, [&out](const uint8_t* p, size_t bytes) {
        out.resize(bytes);
        std::memcpy(out.data(), p, bytes);
    });
}

bool OffscreenView::take(AVFrame* f, bool wait)
{
    const AVPixelFormat want = m_layout == PlaneLayout::Nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    if (m_layout == PlaneLayout::Bgrx || f->format != want || f->width != m_planes.width() || f->height != m_planes.height())
        return false;
    return takeMapped(wait, [this, f](const uint8_t* p, size_t) {
        const int W = m_planes.width(), H = m_planes.height();
        auto copy = [](uint8_t* dst, int dstStride, const uint8_t* src, int rowBytes, int rows) {
            for (int y = 0; y < rows; ++y)
                std::memcpy(dst + ptrdiff_t(y) * dstStride, src + ptrdiff_t(y) * rowBytes, size_t(rowBytes));
        };
        copy(f->data[0], f->linesize[0], p, W, H);
        p += size_t(W) * H;
        if (m_layout == PlaneLayout::Nv12) {
            copy(f->data[1], f->linesize[1], p, W, H / 2);
        } else {
            copy(f->data[1], f->linesize[1], p, W / 2, H / 2);
            copy(f->data[2], f->linesize[2], p + size_t(W / 2) * (H / 2), W / 2, H / 2);
        }
    });
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
