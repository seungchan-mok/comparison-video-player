#pragma once

#include "engine/CudaApi.h"
#include "render/TextureSource.h"

namespace scvp::render {

// Textures for frames that live in CUDA device memory (FrameLocation::Cuda): the integer plane
// textures are registered with CUDA once and each new frame is copied into them device to
// device (map -> cuMemcpy2D to the array -> unmap), so no pixel crosses the CPU or PCIe.
// Requires the CUDA device to be the one driving the GL context; update() returns false
// otherwise (the renderer then downloads the frame instead).
class CudaInteropSource final : public TextureSource {
public:
    ~CudaInteropSource() override;

    bool update(GL* gl, const Frame& frame) override;
    GLuint texture(int plane) const override { return m_tex[size_t(plane)]; }
    const PixelLayout& layout() const override { return m_layout; }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    bool flipY() const override { return false; }
    void release(GL* gl) override;

    bool usable() const { return m_usable != 0; }
    double lastCopyMs() const { return m_lastMs; }

private:
    bool ensureTextures(GL* gl, const PixelLayout& l, int w, int h, CUcontext ctx);
    void unregister();

    GLuint m_tex[4] = {};
    CUgraphicsResource m_res[4] = {};
    int m_texW[4] = {};
    int m_texH[4] = {};
    TexFormat m_texFmt[4] = {};
    PixelLayout m_layout;
    int m_width = 0;
    int m_height = 0;
    CUcontext m_ctx = nullptr;
    int m_usable = -1;  // -1 unknown, 0 no (different GPU / no interop), 1 yes
    double m_lastMs = 0;
};

} // namespace scvp::render
