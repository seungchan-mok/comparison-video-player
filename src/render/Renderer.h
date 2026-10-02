#pragma once

#include "engine/Frame.h"
#include "render/CompareLayout.h"
#include "render/ColorParams.h"
#include "render/CudaInteropSource.h"
#include "render/DigitAtlas.h"
#include "render/PixelOverlay.h"
#include "render/RgbConverter.h"
#include "render/ShaderLibrary.h"
#include "render/TextureSource.h"
#include "render/ViewTransform.h"

#include <QColor>
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
    QColor divider{90, 90, 96};      // between split panes, and the wipe line
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

    const RenderTimings& timings() const { return m_timings; }
    QString error() const { return m_error; }
    // Panes of the last render (the view transform's viewport must have the pane size; see
    // CompareLayout::compute).
    const CompareLayout& layout() const { return m_layout; }
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
        bool gray = false;
        bool valid = false;
    };
    bool prepare(Slot& s, const FramePtr& frame, const ColorOverrides& overrides, bool codes);
    // mode: 0 = slot `a` only, 1 = wipe, 2 = difference (as in display.frag).
    void drawPane(int mode, const Slot& a, const Slot& b, const RenderState& state, QRect pane);

    GL* m_gl = nullptr;
    ShaderLibrary m_shaders;
    std::array<Slot, 2> m_slots;
    GLuint m_vao = 0;
    DigitAtlas m_digits;
    RenderTimings m_timings;
    CompareLayout m_layout;
    QString m_error;
};

} // namespace scvp::render
