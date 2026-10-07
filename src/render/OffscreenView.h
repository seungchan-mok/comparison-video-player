#pragma once

#include "render/GlCommon.h"
#include "render/Renderer.h"
#include "render/ShaderLibrary.h"

#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace scvp::render {

// Pictures of the view for a video file: draws a RenderState into an offscreen framebuffer of
// a fixed size, blends overlay layers (legend, labels, frame numbers painted by the UI) over it
// and reads it back without stalling: glReadPixels into one of kInFlight pixel buffers plus a
// fence, collected in order by take() once the GPU got there. The framebuffer is allocated
// once, unlike Renderer::capture(), so a picture costs the drawing and the read-back only.
// Uses the context current at create(); one thread.
class OffscreenView {
public:
    static constexpr int kInFlight = 3;

    bool create(GL* gl, QSize size, QString* error);
    void release();
    QSize size() const { return m_size; }

    // Layer `index` (drawn in index order) becomes `image` (premultiplied ARGB, 1:1) with its
    // top-left at `origin` in picture px; a null image removes it. Uploaded only when the image
    // changed (QImage::cacheKey).
    void setLayer(int index, const QImage& image, QPoint origin);

    // Draws `state` (framebufferSize must be size()) and the layers, and starts reading the
    // picture back. False when kInFlight pictures wait to be taken, or on a GL error.
    bool render(Renderer& renderer, const RenderState& state);
    int pending() const { return m_pending; }
    // The oldest picture read back: BGRX, bottom row first (`out` is resized to w * h * 4). With
    // wait, blocks until the GPU finished it; else false while it is still running.
    bool take(std::vector<uint8_t>& out, bool wait);

    // The comparison of `like` (its compare, colour and smoothing settings) drawn with the videos
    // at `scale` x their size and nothing around them: split panes side by side (Auto) or top/
    // bottom with the divider between, one picture for the other modes. `image` is the view
    // image (A's size, B's when A is absent), as VideoView uses.
    static RenderState fullFrameState(const RenderState& like, QSizeF image, bool hasA, bool hasB, double scale = 1.0);

private:
    struct Layer {
        QImage image;
        QPoint origin;
        GLuint tex = 0;
        qint64 key = 0;
        QSize texSize;
    };
    struct Readback {
        GLuint pbo = 0;
        GLsync fence = nullptr;
    };

    GL* m_gl = nullptr;
    QSize m_size;
    GLuint m_fbo = 0;
    GLuint m_tex = 0;
    GLuint m_vao = 0;
    ShaderLibrary m_shaders;
    std::vector<Layer> m_layers;
    std::array<Readback, kInFlight> m_reads;
    int m_next = 0;     // ring index of the next render
    int m_pending = 0;  // renders not yet taken (the oldest is m_next - m_pending)
};

} // namespace scvp::render
