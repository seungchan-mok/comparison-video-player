#pragma once
// Single include point for FFmpeg. Everything that differs between FFmpeg 6.1 (the oldest
// supported release, Ubuntu 24.04) and newer releases is wrapped here; version checks use
// AV_VERSION_INT only.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>
#include <vector>

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 100) || \
    LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(60, 16, 100) || \
    LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58, 29, 100)
#error "scvp requires FFmpeg 6.1 or newer"
#endif

namespace scvp::ff {

std::string errorString(int errnum);

inline bool frameIsKey(const AVFrame* f) { return (f->flags & AV_FRAME_FLAG_KEY) != 0; }
inline bool frameIsInterlaced(const AVFrame* f) { return (f->flags & AV_FRAME_FLAG_INTERLACED) != 0; }
inline bool frameIsCorrupt(const AVFrame* f) { return (f->flags & AV_FRAME_FLAG_CORRUPT) != 0; }

// Display rotation of a stream in degrees (counter-clockwise, display-matrix convention),
// 0 when the stream carries no display matrix.
double streamRotation(const AVStream* st);

// Bytes referenced by a frame's buffers (all planes).
size_t frameBufferBytes(const AVFrame* f);

// Pixel formats an encoder accepts (empty: unknown, anything may work). FFmpeg 7.1 replaced
// AVCodec::pix_fmts with avcodec_get_supported_config().
std::vector<AVPixelFormat> supportedPixelFormats(const AVCodec* codec);

// Routes FFmpeg's log output through scvp's logger at or above `level` (AV_LOG_*).
void installLogCallback(int level);

// While one exists, FFmpeg messages logged by this thread are dropped (probing encoders that
// are expected to fail, e.g. NVENC without an NVIDIA driver). Other threads are unaffected.
class LogSilencer {
public:
    explicit LogSilencer(bool active = true);
    ~LogSilencer();
    LogSilencer(const LogSilencer&) = delete;
    LogSilencer& operator=(const LogSilencer&) = delete;

private:
    bool m_active;
};

struct AVFrameFree {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct AVPacketFree {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct AVCodecContextFree {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct AVFormatContextClose {
    void operator()(AVFormatContext* f) const noexcept { avformat_close_input(&f); }
};
struct AVBufferRefUnref {
    void operator()(AVBufferRef* b) const noexcept { av_buffer_unref(&b); }
};
struct AVCodecParserContextClose {
    void operator()(AVCodecParserContext* p) const noexcept { av_parser_close(p); }
};

using FramePtr = std::unique_ptr<AVFrame, AVFrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, AVPacketFree>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextFree>;
using FormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextClose>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, AVBufferRefUnref>;
using ParserPtr = std::unique_ptr<AVCodecParserContext, AVCodecParserContextClose>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

} // namespace scvp::ff
