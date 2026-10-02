#include "engine/FrameAllocator.h"

#include "engine/Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace scvp {
namespace {

constexpr size_t kPage = 4096;
constexpr size_t kHeader = FramePool::kAlign;  // keeps data 64-byte aligned
constexpr uint32_t kMagic = 0x53435650;        // "SCVP"

struct BlockHeader {
    uint32_t magic;
    uint32_t reserved;
    size_t mapSize;  // bytes obtained from the OS, header included
};
static_assert(sizeof(BlockHeader) <= kHeader, "header must fit in the alignment slot");

size_t alignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

void* osAlloc(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
#if defined(MADV_HUGEPAGE)
    if (size >= (size_t(2) << 20))
        madvise(p, size, MADV_HUGEPAGE);
#endif
    return p;
#endif
}

void osFree(void* p, size_t size)
{
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

struct PlaneLayout {
    int linesize[4] = {};
    size_t offset[4] = {};
    size_t dataBytes = 0;  // bytes the planes span, padding included
};

// Linesizes aligned to max(kAlign, lineAlign[i]); planes start on kAlign boundaries; a trailing
// pad covers SIMD over-reads past the last row.
bool computeLayout(AVPixelFormat fmt, int width, int height, const int* lineAlign, PlaneLayout& out)
{
    int ls[4] = {};
    if (width <= 0 || height <= 0 || av_image_fill_linesizes(ls, fmt, width) < 0)
        return false;
    ptrdiff_t lsd[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (!ls[i])
            continue;
        const int a = std::max<int>(int(FramePool::kAlign), lineAlign ? lineAlign[i] : 0);
        ls[i] = (ls[i] + a - 1) / a * a;
        lsd[i] = ls[i];
    }
    size_t sizes[4] = {};
    if (av_image_fill_plane_sizes(sizes, fmt, height, lsd) < 0)
        return false;
    size_t off = 0;
    for (int i = 0; i < 4; ++i) {
        out.linesize[i] = ls[i];
        out.offset[i] = off;
        if (sizes[i])
            off += alignUp(sizes[i], FramePool::kAlign);
    }
    out.dataBytes = off + FramePool::kAlign + AV_INPUT_BUFFER_PADDING_SIZE;
    return true;
}

} // namespace

struct FramePool::Arena {
    std::string name;
    std::mutex mutex;
    std::unordered_map<size_t, std::vector<void*>> freeLists;  // mapSize -> blocks
    size_t cached = 0;
    size_t cacheLimit = size_t(256) << 20;
    std::atomic<size_t> live{0};
    std::atomic<size_t> peak{0};
    std::atomic<size_t> allocations{0};
    std::atomic<int> refs{1};  // owner + one per outstanding block

    // Returns a block whose usable area (after the header) is at least `bytes`.
    uint8_t* acquire(size_t bytes)
    {
        const size_t mapSize = alignUp(bytes + kHeader, kPage);
        void* base = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = freeLists.find(mapSize);
            if (it != freeLists.end() && !it->second.empty()) {
                base = it->second.back();
                it->second.pop_back();
                cached -= mapSize;
            }
        }
        if (!base) {
            base = osAlloc(mapSize);
            if (!base)
                return nullptr;
            allocations.fetch_add(1, std::memory_order_relaxed);
            auto* h = static_cast<BlockHeader*>(base);
            h->magic = kMagic;
            h->mapSize = mapSize;
        }
        const size_t now = live.fetch_add(mapSize, std::memory_order_relaxed) + mapSize;
        size_t prev = peak.load(std::memory_order_relaxed);
        while (now > prev && !peak.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
        }
        refs.fetch_add(1, std::memory_order_relaxed);
        return static_cast<uint8_t*>(base) + kHeader;
    }

    void release(uint8_t* data)
    {
        void* base = data - kHeader;
        auto* h = static_cast<BlockHeader*>(base);
        if (h->magic != kMagic) {
            SCVP_ERROR("FramePool(%s): foreign buffer released", name.c_str());
            return;
        }
        const size_t mapSize = h->mapSize;
        live.fetch_sub(mapSize, std::memory_order_relaxed);
        bool keep = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cached + mapSize <= cacheLimit) {
                freeLists[mapSize].push_back(base);
                cached += mapSize;
                keep = true;
            }
        }
        if (!keep)
            osFree(base, mapSize);
        unref();
    }

    void trim()
    {
        std::unordered_map<size_t, std::vector<void*>> drop;
        {
            std::lock_guard<std::mutex> lock(mutex);
            drop.swap(freeLists);
            cached = 0;
        }
        for (auto& [size, blocks] : drop)
            for (void* b : blocks)
                osFree(b, size);
    }

    void unref()
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            trim();
            delete this;
        }
    }

    static void freeCallback(void* opaque, uint8_t* data) { static_cast<Arena*>(opaque)->release(data); }
};

FramePool::FramePool(std::string name)
    : m_arena(new Arena)
{
    m_arena->name = std::move(name);
}

FramePool::~FramePool()
{
    m_arena->trim();
    m_arena->unref();
}

void FramePool::attachTo(AVCodecContext* ctx)
{
    ctx->opaque = this;
    ctx->get_buffer2 = &FramePool::getBuffer2Thunk;
}

int FramePool::getBuffer2Thunk(AVCodecContext* ctx, AVFrame* frame, int flags)
{
    return static_cast<FramePool*>(ctx->opaque)->getBuffer2(ctx, frame, flags);
}

int FramePool::getBuffer2(AVCodecContext* ctx, AVFrame* frame, int flags)
{
    const auto fmt = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    const bool dr1 = ctx->codec && (ctx->codec->capabilities & AV_CODEC_CAP_DR1);
    if (!desc || !dr1 || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL)) ||
        ctx->codec_type != AVMEDIA_TYPE_VIDEO)
        return avcodec_default_get_buffer2(ctx, frame, flags);

    int w = frame->width;
    int h = frame->height;
    int lineAlign[AV_NUM_DATA_POINTERS] = {};
    avcodec_align_dimensions2(ctx, &w, &h, lineAlign);

    PlaneLayout layout;
    if (!computeLayout(fmt, w, h, lineAlign, layout))
        return avcodec_default_get_buffer2(ctx, frame, flags);

    uint8_t* data = m_arena->acquire(layout.dataBytes);
    if (!data)
        return AVERROR(ENOMEM);
    frame->buf[0] = av_buffer_create(data, int(layout.dataBytes), &Arena::freeCallback, m_arena, 0);
    if (!frame->buf[0]) {
        m_arena->release(data);
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < 4; ++i) {
        frame->linesize[i] = layout.linesize[i];
        frame->data[i] = layout.linesize[i] ? data + layout.offset[i] : nullptr;
    }
    frame->extended_data = frame->data;
    return 0;
}

AVFrame* FramePool::allocFrame(AVPixelFormat format, int width, int height)
{
    PlaneLayout layout;
    if (!computeLayout(format, width, height, nullptr, layout))
        return nullptr;
    AVFrame* f = av_frame_alloc();
    if (!f)
        return nullptr;
    uint8_t* data = m_arena->acquire(layout.dataBytes);
    if (!data) {
        av_frame_free(&f);
        return nullptr;
    }
    f->buf[0] = av_buffer_create(data, int(layout.dataBytes), &Arena::freeCallback, m_arena, 0);
    if (!f->buf[0]) {
        m_arena->release(data);
        av_frame_free(&f);
        return nullptr;
    }
    f->format = format;
    f->width = width;
    f->height = height;
    for (int i = 0; i < 4; ++i) {
        f->linesize[i] = layout.linesize[i];
        f->data[i] = layout.linesize[i] ? data + layout.offset[i] : nullptr;
    }
    f->extended_data = f->data;
    return f;
}

size_t FramePool::liveBytes() const { return m_arena->live.load(std::memory_order_relaxed); }
size_t FramePool::peakLiveBytes() const { return m_arena->peak.load(std::memory_order_relaxed); }
size_t FramePool::allocationCount() const { return m_arena->allocations.load(std::memory_order_relaxed); }

size_t FramePool::cachedBytes() const
{
    std::lock_guard<std::mutex> lock(m_arena->mutex);
    return m_arena->cached;
}

void FramePool::setCacheLimit(size_t bytes)
{
    {
        std::lock_guard<std::mutex> lock(m_arena->mutex);
        m_arena->cacheLimit = bytes;
    }
    if (cachedBytes() > bytes)
        m_arena->trim();
}

void FramePool::trim() { m_arena->trim(); }

const std::string& FramePool::name() const { return m_arena->name; }

} // namespace scvp
