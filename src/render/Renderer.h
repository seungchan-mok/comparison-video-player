#pragma once

#include "engine/Frame.h"
#include "render/ColorParams.h"
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
};

struct RenderTimings {
    double uploadMs = 0;
    double convertMs = 0;
    size_t uploadBytes = 0;
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
    const RgbConverter& converter(int slot) const { return m_slots[size_t(slot)].conv; }
    ColorParams colorParams(int slot) const { return m_slots[size_t(slot)].color; }

private:
    struct Slot {
        CpuUploadSource upload;
        RgbConverter conv;
        const Frame* shown = nullptr;   // identity of the converted frame
        FramePtr hold;                  // keeps it alive while its pixels are on the GPU
        ColorParams color;
        ColorOverrides overrides;
        bool valid = false;
    };
    bool prepare(Slot& s, const FramePtr& frame, const ColorOverrides& overrides);
    void drawView(const Slot& s, const ViewTransform& view, QRect viewport, const QColor& bg);

    GL* m_gl = nullptr;
    ShaderLibrary m_shaders;
    std::array<Slot, 2> m_slots;
    GLuint m_vao = 0;
    RenderTimings m_timings;
    QString m_error;
};

} // namespace scvp::render
