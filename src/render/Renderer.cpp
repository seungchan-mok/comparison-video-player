#include "render/Renderer.h"

#include "engine/FrameAllocator.h"
#include "engine/GpuFrame.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

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
    const bool sameColor = s.overrides == overrides;
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
    } else if (!PixelLayout::of(frame->format())) {
        if (uploadConverted(s, *frame)) {
            source = &s.upload;
            s.path = UploadPath::Upload;
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
    s.color.chromaBilinear = overrides.chromaBilinear;
    s.color.siting = ChromaSiting::of(frame->av->chroma_location);
    s.gray = source->layout().isGray();
    s.layoutKind = source->layout().layout;
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

// Pixel formats without a shader path (palettes, big-endian, packed 10-bit, float, ...) go
// through swscale into the closest supported format: same family and depth, point sampling, so
// values are kept wherever the target can hold them.
bool Renderer::uploadConverted(Slot& s, const Frame& frame)
{
    const AVPixelFormat target = PixelLayout::fallbackFor(frame.format());
    if (target == AV_PIX_FMT_NONE)
        return false;
    s.sws = sws_getCachedContext(s.sws, frame.width(), frame.height(), frame.format(), frame.width(), frame.height(), target,
                                 SWS_POINT | SWS_BITEXACT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP, nullptr,
                                 nullptr, nullptr);
    if (!s.sws)
        return false;
    Frame tmp;
    tmp.av = av_frame_alloc();
    tmp.av->format = target;
    tmp.av->width = frame.width();
    tmp.av->height = frame.height();
    if (av_frame_get_buffer(tmp.av, 64) < 0 || sws_scale_frame(s.sws, tmp.av, frame.av) < 0)
        return false;
    av_frame_copy_props(tmp.av, frame.av);
    return s.upload.update(m_gl, tmp);
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
    const bool fractional = std::abs(scale - std::round(scale)) > 1e-9;
    const bool smooth = state.smoothScaling && magnified && fractional && scale < kGridMinLogicalPx * dpr;
    gl->glUniform1i(loc("smoothMag"), smooth ? 1 : 0);
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
    const float wipe = float(std::floor(state.compare.wipe * pane.width()));
    gl->glUniform1f(loc("wipeX"), wipe + 0.5f);
    // The line covers lineWidth whole pixels around the divider pixel (one on screen).
    const int lineWidth = std::max(1, int(std::lround(state.lineScale)));
    const float lineLeft = wipe - float((lineWidth - 1) / 2);
    gl->glUniform2f(loc("wipeLine"), lineLeft, lineLeft + float(lineWidth));
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
    const int divider = std::max(1, int(std::lround(CompareLayout::kDivider * state.lineScale)));
    m_layout = CompareLayout::compute(state.compare, fb, state.view.imageSize(), a.valid, b.valid, divider);
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
    if (state.probe && state.sampleProbe) {
        const QSizeF view = state.view.imageSize();
        std::array<const RgbConverter*, 2> conv{};
        std::array<QPoint, 2> texel{};
        std::array<CodeKind, 2> kinds{};
        for (int i = 0; i < 2; ++i) {
            const Slot& sl = m_slots[size_t(i)];
            if (!sl.valid)
                continue;
            conv[size_t(i)] = &sl.conv;
            // Same mapping as the display shader: floor(x * k).
            texel[size_t(i)] = QPoint(int(std::floor(state.probePos.x() * sl.conv.width() / view.width())),
                                      int(std::floor(state.probePos.y() * sl.conv.height() / view.height())));
            const Layout l = sl.layoutKind;
            kinds[size_t(i)] = l == Layout::Gray    ? CodeKind::Gray
                               : l == Layout::Bayer ? CodeKind::Bayer
                               : l == Layout::PlanarRgb || l == Layout::PackedRgb ? CodeKind::Rgb
                                                                                    : CodeKind::Yuv;
        }
        if (state.probePos.x() >= 0 && state.probePos.y() >= 0 && state.probePos.x() < view.width() && state.probePos.y() < view.height())
            m_readback.request(gl, conv, texel, kinds, state.probePos);
        else
            m_readback.clear();
        gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    }
}

double Renderer::captureFactor(const RenderState& state, int maxSide, qint64 maxPixels)
{
    const QSize fb = state.framebufferSize;
    const QSizeF image = state.view.imageSize();
    if (fb.isEmpty() || image.isEmpty() || state.view.scale() <= 0)
        return 1.0;
    // Source pixels per view image pixel of each video on screen (B may be larger than A's grid).
    const bool both = state.frames[0] && state.frames[1];
    double texels = 0;
    for (int i = 0; i < 2; ++i) {
        const FramePtr& f = state.frames[size_t(i)];
        const bool hidden = both && state.compare.mode == CompareMode::Flip && state.compare.flipShowsB != (i == 1);
        if (f && !hidden)
            texels = std::max({texels, f->width() / image.width(), f->height() / image.height()});
    }
    double factor = texels / state.view.scale();
    factor = std::min({factor, double(maxSide) / fb.width(), double(maxSide) / fb.height(),
                       std::sqrt(double(maxPixels) / (double(fb.width()) * fb.height()))});
    return std::max(1.0, factor);
}

double Renderer::captureFactor(const RenderState& state, qint64 maxPixels) const
{
    int maxSide = kMaxCaptureSide;
    if (m_gl) {
        GLint tex = 0;
        GLint viewport[2] = {0, 0};
        m_gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex);
        m_gl->glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport);
        maxSide = std::min({maxSide, int(tex), int(viewport[0]), int(viewport[1])});
    }
    return captureFactor(state, maxSide, maxPixels);
}

QImage Renderer::capture(const RenderState& screen, double factor)
{
    if (!m_gl || factor <= 0)
        return {};
    RenderState s = screen;
    const QSize size(std::max(1, int(std::lround(screen.framebufferSize.width() * factor))),
                     std::max(1, int(std::lround(screen.framebufferSize.height() * factor))));
    s.framebufferSize = size;
    s.view = screen.view.scaled(factor);
    // Size thresholds in logical px (digits, grid, outline, smoothing) decide as on screen.
    s.devicePixelRatio = screen.devicePixelRatio * factor;
    s.lineScale = screen.lineScale * factor;
    s.sampleProbe = false;

    GL* gl = m_gl;
    while (gl->glGetError() != GL_NO_ERROR) {
    }
    GLuint tex = 0, fbo = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    QImage img;
    if (gl->glGetError() == GL_NO_ERROR && gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        const CompareLayout layout = m_layout;
        const RenderTimings timings = m_timings;
        render(s, fbo);
        m_layout = layout;
        m_timings = timings;
        img = QImage(size, QImage::Format_RGB32);  // null when memory runs out
        if (!img.isNull()) {
            // 0xffRRGGBB words, as QImage stores RGB32; rows come bottom-up.
            gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            gl->glReadPixels(0, 0, size.width(), size.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, img.bits());
            if (gl->glGetError() == GL_NO_ERROR) {
                std::vector<uchar> row(size_t(img.bytesPerLine()));
                for (int y = 0, z = size.height() - 1; y < z; ++y, --z) {
                    std::memcpy(row.data(), img.scanLine(y), row.size());
                    std::memcpy(img.scanLine(y), img.scanLine(z), row.size());
                    std::memcpy(img.scanLine(z), row.data(), row.size());
                }
            } else {
                img = QImage();
            }
        }
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &fbo);
    gl->glDeleteTextures(1, &tex);
    return img;
}

} // namespace scvp::render
