#pragma once

#include "engine/CudaApi.h"
#include "engine/FfmpegCompat.h"
#include "engine/Frame.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace scvp {

class FramePool;
class GpuFramePool;

// One frame's planes in device memory: a single pitched allocation with the planes stacked.
struct GpuBlock {
    std::shared_ptr<GpuFramePool> pool;  // returns the allocation on destruction
    CUdeviceptr base = 0;
    size_t pitch = 0;
    size_t rows = 0;
    size_t allocWidth = 0;                   // requested row bytes (recycling key)
    AVPixelFormat format = AV_PIX_FMT_NONE;  // layout of the planes (NV12, P010, YUV444P, ...)
    int width = 0;
    int height = 0;
    int planes = 0;
    CUdeviceptr plane[4] = {};
    size_t planeWidthBytes[4] = {};
    int planeRows[4] = {};

    size_t bytes() const { return pitch * rows; }
    ~GpuBlock();
};

// Device-memory frames for zero-copy decoding on the CUDA context of an FFmpeg device:
// exact-size recycling of pitched blocks (cuMemAlloc is slow), D2D copies out of decoder
// surfaces, downloads into system-memory pool frames and uploads (tests, fallbacks).
class GpuFramePool : public std::enable_shared_from_this<GpuFramePool> {
public:
    // nullptr when `cudaDevice` is not a CUDA device or the driver API is unavailable.
    static std::shared_ptr<GpuFramePool> create(AVBufferRef* cudaDevice);
    // The process-wide pool for a device context (one per CUDA context).
    static std::shared_ptr<GpuFramePool> forDevice(AVBufferRef* cudaDevice);
    ~GpuFramePool();

    CUcontext context() const { return m_ctx; }
    CUstream stream() const { return m_stream; }

    std::shared_ptr<const GpuBlock> allocate(AVPixelFormat format, int width, int height);
    // Copies an AV_PIX_FMT_CUDA frame (decoder surface) into a new block; the surface can be
    // released as soon as this returns.
    std::shared_ptr<const GpuBlock> copyFrom(const AVFrame* cudaFrame);
    std::shared_ptr<const GpuBlock> upload(const AVFrame* cpuFrame);
    // A system-memory frame (from `pool`) with the block's pixels; nullptr on failure.
    AVFrame* download(const GpuBlock& block, FramePool& pool);

    size_t liveBytes() const;
    size_t cachedBytes() const;
    uint64_t allocationFailures() const { return m_failures.load(); }
    void setCacheLimit(size_t bytes);
    void trim();
    // Free / total device memory of the context's device.
    bool memoryInfo(size_t* freeBytes, size_t* totalBytes);

private:
    friend struct GpuBlock;
    GpuFramePool() = default;
    void release(CUdeviceptr base, size_t pitch, size_t rows, size_t widthBytes);
    bool copyPlanes(const GpuBlock& b, const uint8_t* const* srcHost, CUdeviceptr const* srcDev, const int* srcPitch, bool toDevice,
                    uint8_t* const* dstHost, const int* dstPitch);

    AVBufferRef* m_device = nullptr;
    CUcontext m_ctx = nullptr;
    CUstream m_stream = nullptr;
    mutable std::mutex m_mutex;
    struct Free {
        CUdeviceptr base;
        size_t pitch;
    };
    std::unordered_map<uint64_t, std::vector<Free>> m_free;  // key: widthBytes x rows
    size_t m_live = 0;
    size_t m_cached = 0;
    size_t m_cacheLimit = size_t(256) << 20;
    std::atomic<uint64_t> m_failures{0};
};

// A Frame in device memory with `src`'s metadata (format/size/colour copied, no pixels).
std::shared_ptr<Frame> makeGpuFrame(std::shared_ptr<const GpuBlock> block, const AVFrame* metadataFrom);

} // namespace scvp
