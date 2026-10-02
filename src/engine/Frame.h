#pragma once

#include "engine/FfmpegCompat.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace scvp {

// Where a frame's pixels live.
enum class FrameLocation : uint8_t {
    Cpu,   // system memory: av->data[] / av->linesize[]
    Cuda,  // NVIDIA device memory: `gpu`; av carries the metadata only (no data pointers)
};

struct GpuBlock;

// A decoded picture, immutable once it is published to the cache. Owns its AVFrame
// reference; the pixel buffers usually come from a FramePool.
struct Frame {
    AVFrame* av = nullptr;          // CPU planes, or for GPU frames format/size/colour metadata
    FrameLocation location = FrameLocation::Cpu;
    std::shared_ptr<const GpuBlock> gpu;  // pixels of a FrameLocation::Cuda frame
    int32_t pres = -1;              // presentation index: the frame number shown to the user
    int32_t dec = -1;               // decode-order index of the packet that produced the frame
    int64_t pts = AV_NOPTS_VALUE;   // stream time base
    size_t bytes = 0;               // buffer bytes held (all planes)
    char pictType = '?';            // 'I', 'P', 'B', ... from the decoder
    bool key = false;
    bool provisional = false;       // decoded before the index was final; number is an estimate
    bool hardware = false;          // decoded by a hardware decoder (then downloaded)

    Frame() = default;
    ~Frame() { av_frame_free(&av); }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    int width() const { return av ? av->width : 0; }
    int height() const { return av ? av->height : 0; }
    AVPixelFormat format() const { return av ? static_cast<AVPixelFormat>(av->format) : AV_PIX_FMT_NONE; }
};

using FramePtr = std::shared_ptr<const Frame>;

} // namespace scvp
