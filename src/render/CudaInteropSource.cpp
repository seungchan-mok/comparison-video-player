#include "render/CudaInteropSource.h"

#include "engine/GpuFrame.h"
#include "engine/Log.h"

#include <chrono>

namespace scvp::render {

CudaInteropSource::~CudaInteropSource() { unregister(); }

void CudaInteropSource::unregister()
{
    if (!m_ctx || !cuda::api())
        return;
    cuda::ScopedContext ctx(m_ctx);
    for (CUgraphicsResource& r : m_res)
        if (r) {
            if (ctx.ok())
                cuda::api()->cuGraphicsUnregisterResource(r);
            r = nullptr;
        }
}

void CudaInteropSource::release(GL* gl)
{
    unregister();
    for (GLuint& t : m_tex)
        if (t) {
            gl->glDeleteTextures(1, &t);
            t = 0;
        }
}

bool CudaInteropSource::ensureTextures(GL* gl, const PixelLayout& l, int w, int h, CUcontext ctx)
{
    const cuda::Api* api = cuda::api();
    if (m_usable < 0 || ctx != m_ctx) {
        // Interop needs the CUDA device to be (one of) the device(s) behind the current GL context.
        cuda::ScopedContext scoped(ctx);
        CUdevice frameDev = -1;
        unsigned count = 0;
        CUdevice glDevs[8] = {};
        m_usable = 0;
        if (scoped.ok() && api->cuCtxGetDevice(&frameDev) == 0 &&
            api->cuGLGetDevices(&count, glDevs, 8, cuda::kGlDeviceListAll) == 0)
            for (unsigned i = 0; i < count && i < 8; ++i)
                if (glDevs[i] == frameDev)
                    m_usable = 1;
        if (!m_usable)
            SCVP_INFO("CUDA-GL interop unavailable (decoding GPU does not drive the display); downloading frames");
        unregister();
        m_ctx = ctx;
    }
    if (!m_usable)
        return false;
    cuda::ScopedContext scoped(ctx);
    if (!scoped.ok())
        return false;
    for (int i = 0; i < l.planes; ++i) {
        const TexFormat fmt = l.plane[size_t(i)].format;
        const int tw = l.planeWidth(i, w);
        const int th = l.planeHeight(i, h);
        if (m_tex[i] && m_texW[i] == tw && m_texH[i] == th && m_texFmt[i] == fmt && m_res[i])
            continue;
        if (m_res[i]) {
            api->cuGraphicsUnregisterResource(m_res[i]);
            m_res[i] = nullptr;
        }
        if (!m_tex[i])
            gl->glGenTextures(1, &m_tex[i]);
        gl->glBindTexture(GL_TEXTURE_2D, m_tex[i]);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        const GlTexFormat f = glFormat(fmt);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, f.internal, tw, th, 0, f.format, f.type, nullptr);
        const CUresult r = api->cuGraphicsGLRegisterImage(&m_res[i], m_tex[i], GL_TEXTURE_2D, cuda::kRegisterWriteDiscard);
        if (r != 0) {
            SCVP_WARN("cuGraphicsGLRegisterImage: %s; downloading frames instead", cuda::errorName(r));
            m_res[i] = nullptr;
            m_usable = 0;
            return false;
        }
        m_texW[i] = tw;
        m_texH[i] = th;
        m_texFmt[i] = fmt;
    }
    return true;
}

bool CudaInteropSource::update(GL* gl, const Frame& frame)
{
    if (frame.location != FrameLocation::Cuda || !frame.gpu || !cuda::api())
        return false;
    const GpuBlock& b = *frame.gpu;
    const auto layout = PixelLayout::of(b.format);
    if (!layout || layout->planes != b.planes)
        return false;
    const auto t0 = std::chrono::steady_clock::now();
    CUcontext ctx = b.pool ? b.pool->context() : nullptr;
    if (!ensureTextures(gl, *layout, b.width, b.height, ctx))
        return false;
    m_layout = *layout;
    m_width = b.width;
    m_height = b.height;

    const cuda::Api* api = cuda::api();
    cuda::ScopedContext scoped(ctx);
    CUstream stream = b.pool->stream();
    if (api->cuGraphicsMapResources(unsigned(b.planes), m_res, stream) != 0)
        return false;
    bool ok = true;
    for (int i = 0; i < b.planes && ok; ++i) {
        CUarray arr = nullptr;
        ok = api->cuGraphicsSubResourceGetMappedArray(&arr, m_res[i], 0, 0) == 0;
        if (!ok)
            break;
        cuda::Memcpy2D c{};
        c.srcMemoryType = cuda::MemDevice;
        c.srcDevice = b.plane[i];
        c.srcPitch = b.pitch;
        c.dstMemoryType = cuda::MemArray;
        c.dstArray = arr;
        c.WidthInBytes = size_t(layout->planeWidth(i, b.width)) * size_t(bytesPerTexel(layout->plane[size_t(i)].format));
        c.Height = size_t(layout->planeHeight(i, b.height));
        ok = api->cuMemcpy2DAsync(&c, stream) == 0;
    }
    // Unmapping orders the copies before any later GL use of the textures.
    api->cuGraphicsUnmapResources(unsigned(b.planes), m_res, stream);
    m_lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

} // namespace scvp::render
