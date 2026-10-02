#include "engine/GpuFrame.h"

#include "engine/FrameAllocator.h"
#include "engine/Log.h"

extern "C" {
#include <libavutil/hwcontext_cuda.h>
}

#include <algorithm>
#include <cstring>

namespace scvp {
namespace {

uint64_t key(size_t widthBytes, size_t rows) { return (uint64_t(widthBytes) << 24) ^ uint64_t(rows); }

struct PlaneGeometry {
    int planes = 0;
    size_t widthBytes[4] = {};
    int rows[4] = {};
    size_t maxWidth = 0;
    size_t totalRows = 0;
};

bool geometry(AVPixelFormat fmt, int w, int h, PlaneGeometry& g)
{
    int ls[4] = {};
    if (av_image_fill_linesizes(ls, fmt, w) < 0)
        return false;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    g.planes = av_pix_fmt_count_planes(fmt);
    for (int i = 0; i < g.planes; ++i) {
        g.widthBytes[i] = size_t(ls[i]);
        g.rows[i] = (i == 1 || i == 2) ? -((-h) >> d->log2_chroma_h) : h;
        g.maxWidth = std::max(g.maxWidth, g.widthBytes[i]);
        g.totalRows += size_t(g.rows[i]);
    }
    return g.planes > 0;
}

} // namespace

GpuBlock::~GpuBlock()
{
    if (pool && base)
        pool->release(base, pitch, rows, allocWidth);
}

std::shared_ptr<GpuFramePool> GpuFramePool::create(AVBufferRef* cudaDevice)
{
    if (!cudaDevice || !cuda::api())
        return nullptr;
    auto* dev = reinterpret_cast<AVHWDeviceContext*>(cudaDevice->data);
    if (dev->type != AV_HWDEVICE_TYPE_CUDA)
        return nullptr;
    auto* cu = static_cast<AVCUDADeviceContext*>(dev->hwctx);
    std::shared_ptr<GpuFramePool> p(new GpuFramePool);
    p->m_device = av_buffer_ref(cudaDevice);
    p->m_ctx = cu->cuda_ctx;
    p->m_stream = cu->stream;
    return p;
}

GpuFramePool::~GpuFramePool()
{
    trim();
    av_buffer_unref(&m_device);
}

std::shared_ptr<const GpuBlock> GpuFramePool::allocate(AVPixelFormat format, int width, int height)
{
    PlaneGeometry g;
    if (!geometry(format, width, height, g))
        return nullptr;
    auto b = std::make_shared<GpuBlock>();
    b->format = format;
    b->width = width;
    b->height = height;
    b->planes = g.planes;
    b->rows = g.totalRows;
    b->allocWidth = g.maxWidth;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_free.find(key(g.maxWidth, g.totalRows));
        if (it != m_free.end() && !it->second.empty()) {
            b->base = it->second.back().base;
            b->pitch = it->second.back().pitch;
            it->second.pop_back();
            m_cached -= b->pitch * b->rows;
        }
    }
    if (!b->base) {
        cuda::ScopedContext ctx(m_ctx);
        if (!ctx.ok())
            return nullptr;
        const CUresult r = cuda::api()->cuMemAllocPitch(&b->base, &b->pitch, g.maxWidth, g.totalRows, 16);
        if (r != 0) {
            SCVP_WARN("cuMemAllocPitch(%zu x %zu): %s", g.maxWidth, g.totalRows, cuda::errorName(r));
            return nullptr;
        }
    }
    size_t row = 0;
    for (int i = 0; i < g.planes; ++i) {
        b->plane[i] = b->base + row * b->pitch;
        b->planeWidthBytes[i] = g.widthBytes[i];
        b->planeRows[i] = g.rows[i];
        row += size_t(g.rows[i]);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_live += b->pitch * b->rows;
    }
    b->pool = shared_from_this();
    return b;
}

void GpuFramePool::release(CUdeviceptr base, size_t pitch, size_t rows, size_t widthBytes)
{
    const size_t bytes = pitch * rows;
    bool keep = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_live -= bytes;
        if (m_cached + bytes <= m_cacheLimit) {
            m_free[key(widthBytes, rows)].push_back({base, pitch});
            m_cached += bytes;
            keep = true;
        }
    }
    if (!keep) {
        cuda::ScopedContext ctx(m_ctx);
        if (ctx.ok())
            cuda::api()->cuMemFree(base);
    }
}

bool GpuFramePool::copyPlanes(const GpuBlock& b, const uint8_t* const* srcHost, CUdeviceptr const* srcDev, const int* srcPitch,
                              bool toDevice, uint8_t* const* dstHost, const int* dstPitch)
{
    cuda::ScopedContext ctx(m_ctx);
    if (!ctx.ok())
        return false;
    for (int i = 0; i < b.planes; ++i) {
        cuda::Memcpy2D c{};
        c.WidthInBytes = b.planeWidthBytes[i];
        c.Height = size_t(b.planeRows[i]);
        if (toDevice) {
            if (srcDev) {
                c.srcMemoryType = cuda::MemDevice;
                c.srcDevice = srcDev[i];
            } else {
                c.srcMemoryType = cuda::MemHost;
                c.srcHost = srcHost[i];
            }
            c.srcPitch = size_t(srcPitch[i]);
            c.dstMemoryType = cuda::MemDevice;
            c.dstDevice = b.plane[i];
            c.dstPitch = b.pitch;
        } else {
            c.srcMemoryType = cuda::MemDevice;
            c.srcDevice = b.plane[i];
            c.srcPitch = b.pitch;
            c.dstMemoryType = cuda::MemHost;
            c.dstHost = dstHost[i];
            c.dstPitch = size_t(dstPitch[i]);
        }
        const CUresult r = cuda::api()->cuMemcpy2DAsync(&c, m_stream);
        if (r != 0) {
            SCVP_WARN("cuMemcpy2DAsync: %s", cuda::errorName(r));
            return false;
        }
    }
    return cuda::api()->cuStreamSynchronize(m_stream) == 0;
}

std::shared_ptr<const GpuBlock> GpuFramePool::copyFrom(const AVFrame* f)
{
    if (f->format != AV_PIX_FMT_CUDA || !f->hw_frames_ctx)
        return nullptr;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(f->hw_frames_ctx->data);
    auto b = allocate(frames->sw_format, f->width, f->height);
    if (!b)
        return nullptr;
    CUdeviceptr src[4] = {};
    int pitch[4] = {};
    for (int i = 0; i < b->planes; ++i) {
        src[i] = reinterpret_cast<CUdeviceptr>(f->data[i]);
        pitch[i] = f->linesize[i];
    }
    return copyPlanes(*b, nullptr, src, pitch, true, nullptr, nullptr) ? b : nullptr;
}

std::shared_ptr<const GpuBlock> GpuFramePool::upload(const AVFrame* f)
{
    auto b = allocate(AVPixelFormat(f->format), f->width, f->height);
    if (!b)
        return nullptr;
    return copyPlanes(*b, f->data, nullptr, f->linesize, true, nullptr, nullptr) ? b : nullptr;
}

AVFrame* GpuFramePool::download(const GpuBlock& b, FramePool& pool)
{
    AVFrame* f = pool.allocFrame(b.format, b.width, b.height);
    if (!f)
        return nullptr;
    if (!copyPlanes(b, nullptr, nullptr, nullptr, false, f->data, f->linesize)) {
        av_frame_free(&f);
        return nullptr;
    }
    return f;
}

size_t GpuFramePool::liveBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_live;
}

size_t GpuFramePool::cachedBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cached;
}

void GpuFramePool::setCacheLimit(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheLimit = bytes;
}

void GpuFramePool::trim()
{
    std::unordered_map<uint64_t, std::vector<Free>> drop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        drop.swap(m_free);
        m_cached = 0;
    }
    if (drop.empty())
        return;
    cuda::ScopedContext ctx(m_ctx);
    if (!ctx.ok())
        return;
    for (auto& [k, blocks] : drop)
        for (const Free& f : blocks)
            cuda::api()->cuMemFree(f.base);
}

bool GpuFramePool::memoryInfo(size_t* freeBytes, size_t* totalBytes)
{
    cuda::ScopedContext ctx(m_ctx);
    return ctx.ok() && cuda::api()->cuMemGetInfo(freeBytes, totalBytes) == 0;
}

std::shared_ptr<Frame> makeGpuFrame(std::shared_ptr<const GpuBlock> block, const AVFrame* src)
{
    auto f = std::make_shared<Frame>();
    f->av = av_frame_alloc();
    av_frame_copy_props(f->av, src);
    f->av->format = block->format;
    f->av->width = block->width;
    f->av->height = block->height;
    f->location = FrameLocation::Cuda;
    f->bytes = block->bytes();
    f->gpu = std::move(block);
    return f;
}

} // namespace scvp
