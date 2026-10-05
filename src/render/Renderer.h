#pragma once

#include "engine/Frame.h"
#include "render/CompareLayout.h"
#include "render/ColorParams.h"
#include "render/CudaInteropSource.h"
#include "render/DigitAtlas.h"
#include "render/PixelOverlay.h"
#include "render/PixelReadback.h"
#include "render/RgbConverter.h"
#include "render/ShaderLibrary.h"
#include "render/TextureSource.h"
#include "render/ViewTransform.h"

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>

#include <array>
#include <memory>

namespace scvp::render {

struct RenderState {
    std::array<FramePtr, 2> frames;  // slot A, slot B (null: nothing to show)
    ViewTransform view;
    QSize framebufferSize;           // device pixels
    QColor background{30, 30, 32};
    std::array<ColorOverrides, 2> color;
    CompareSettings compare;
    PixelOverlaySettings overlay;
    double devicePixelRatio = 1.0;   // for size thresholds given in logical px
    // Bilinear filtering at fractional magnifications (fit to window) below the pixel-grid size;
    // integer zooms and large magnifications always show exact pixels.
    bool smoothScaling = true;
    // Pixel under the cursor (view image coordinates): outlined in every pane and read back.
    bool probe = false;
    QPointF probePos;
    bool sampleProbe = true;         // read the probed pixel back (a capture only outlines it)
    QColor divider{90, 90, 96};      // between split panes, and the wipe line
    // Lines given in screen pixels (the wipe line, the divider between split panes) are this many
    // pixels wide per screen pixel: > 1 when the view is drawn larger than the screen.
    double lineScale = 1.0;
};

// How the frame on screen reached its textures.
enum class UploadPath { None, Upload, ZeroCopy, Download };

struct RenderTimings {
    double uploadMs = 0;
    double convertMs = 0;
    size_t uploadBytes = 0;
    bool zeroCopy = false;   // a GPU-resident frame went to the textures without the CPU
    bool converted = false;  // a new frame was uploaded and converted this time
};

// Owns all GL resources for drawing frames: per-slot upload + conversion, then the display
// pass. Independent of QOpenGLWidget: render() draws into whatever framebuffer is bound by id.
// Uploading and converting happen only when a slot's frame (or its colour settings) changed.
class Renderer {
public:
    bool initialize(GL* gl);
    void release();
    bool initialized() const { return m_gl != nullptr; }

    void render(const RenderState& state, GLuint targetFbo);

    // Draws the view of `screen` (a state as rendered for the screen) once more, offscreen and
    // `factor` times larger: the same picture -- zoom, pan, comparison, pixel values, cursor
    // outline -- with each screen pixel becoming factor x factor pixels, so a video the screen
    // shows minified can be captured at its full detail. Leaves the screen's results (layout,
    // timings, read-back) as they were. The context must be current; null when the GPU cannot
    // allocate the image.
    QImage capture(const RenderState& screen, double factor);
    // How much larger than the screen a capture of `state` should be drawn: enough that no shown
    // video is minified (each of its pixels gets at least one image pixel), never less than 1,
    // and within maxSide x maxSide and maxPixels.
    static double captureFactor(const RenderState& state, int maxSide, qint64 maxPixels);
    // The same, within this GPU's texture and viewport limits.
    double captureFactor(const RenderState& state, qint64 maxPixels = kMaxCapturePixels) const;
    // Captures stop here (an 8K frame at 1:1 fits): beyond, memory and encoding time grow
    // without a screen to show the difference.
    static constexpr qint64 kMaxCapturePixels = 40'000'000;
    static constexpr int kMaxCaptureSide = 16384;

    const RenderTimings& timings() const { return m_timings; }
    QString error() const { return m_error; }
    // Panes of the last render (the view transform's viewport must have the pane size; see
    // CompareLayout::compute).
    const CompareLayout& layout() const { return m_layout; }
    // Latest finished read-back of the probed pixel; pending while one is in flight (render
    // again to collect it). finishSample() waits for it (tests).
    const PixelSample& sample() const { return m_readback.result(); }
    bool samplePending() const { return m_readback.pending(); }
    void finishSample() { m_readback.poll(m_gl, true); }
    const RgbConverter& converter(int slot) const { return m_slots[size_t(slot)].conv; }
    ColorParams colorParams(int slot) const { return m_slots[size_t(slot)].color; }
    UploadPath uploadPath(int slot) const { return m_slots[size_t(slot)].path; }

private:
    struct Slot {
        CpuUploadSource upload;
        CudaInteropSource interop;
        RgbConverter conv;
        UploadPath path = UploadPath::None;
        const Frame* shown = nullptr;   // identity of the converted frame
        FramePtr hold;                  // keeps it alive while its pixels are on the GPU
        ColorParams color;
        ColorOverrides overrides;
        bool codes = false;             // the converter also wrote the source codes
        SwsContext* sws = nullptr;      // formats the shaders do not read: converted first
        bool gray = false;
        Layout layoutKind = Layout::PlanarYuv;  // of the converted frame
        bool valid = false;
    };
    bool prepare(Slot& s, const FramePtr& frame, const ColorOverrides& overrides, bool codes);
    bool uploadConverted(Slot& s, const Frame& frame);
    // mode: 0 = slot `a` only, 1 = wipe, 2 = difference (as in display.frag).
    void drawPane(int mode, const Slot& a, const Slot& b, const RenderState& state, QRect pane);

    GL* m_gl = nullptr;
    ShaderLibrary m_shaders;
    std::array<Slot, 2> m_slots;
    GLuint m_vao = 0;
    DigitAtlas m_digits;
    PixelReadback m_readback;
    RenderTimings m_timings;
    CompareLayout m_layout;
    QString m_error;
};

} // namespace scvp::render
