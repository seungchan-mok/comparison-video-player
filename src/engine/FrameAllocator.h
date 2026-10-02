#pragma once

#include "engine/FfmpegCompat.h"

#include <cstddef>
#include <string>

namespace scvp {

// Allocator for every frame buffer the engine creates: decoder output (as get_buffer2),
// hardware downloads and pixel-format conversions.
//
// Each frame gets one contiguous block with 64-byte aligned planes. Released blocks go to an
// exact-size free list (bounded by cacheLimit) so steady-state decoding causes no page faults,
// and trim() returns memory to the OS. Because decoders allocate their reference frames and
// frame-thread buffers through it as well, liveBytes() is the true pixel-memory footprint,
// unlike FFmpeg's internal AVBufferPool which never shrinks.
//
// Thread-safe. Outstanding buffers keep the pool's internals alive after the FramePool object
// is destroyed, so frames may outlive the decoder that produced them.
class FramePool {
public:
    static constexpr size_t kAlign = 64;

    explicit FramePool(std::string name = {});
    ~FramePool();
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    // get_buffer2 implementation. Formats it cannot serve (hardware surfaces, palettes,
    // decoders without AV_CODEC_CAP_DR1) are forwarded to avcodec_default_get_buffer2.
    int getBuffer2(AVCodecContext* ctx, AVFrame* frame, int flags);

    // Installs this pool as ctx->get_buffer2 (stores `this` in ctx->opaque). Call before
    // avcodec_open2; the pool must outlive the context.
    void attachTo(AVCodecContext* ctx);
    static int getBuffer2Thunk(AVCodecContext* ctx, AVFrame* frame, int flags);

    // A new frame with uninitialized planes; nullptr on failure.
    AVFrame* allocFrame(AVPixelFormat format, int width, int height);

    size_t liveBytes() const;       // blocks currently referenced by frames
    size_t cachedBytes() const;     // blocks parked in the free list
    size_t peakLiveBytes() const;
    size_t allocationCount() const; // blocks obtained from the OS so far

    void setCacheLimit(size_t bytes);
    void trim();

    const std::string& name() const;

private:
    struct Arena;
    Arena* m_arena;
};

} // namespace scvp
