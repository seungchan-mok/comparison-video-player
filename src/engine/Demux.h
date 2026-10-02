#pragma once

#include "engine/FfmpegCompat.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scvp {

// How to open a media file with libavformat.
// Geometry of headerless raw video (.yuv): every frame is width x height in `format`, packed
// back to back (align 1) after `headerBytes`.
struct RawVideoParams {
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    double frameRate = 0;    // 0: 30
    int64_t headerBytes = 0;
    bool valid() const { return width > 0 && height > 0 && format != AV_PIX_FMT_NONE; }
};

struct InputOptions {
    std::string formatName;                                     // force a demuxer, e.g. "rawvideo"
    std::vector<std::pair<std::string, std::string>> options;  // demuxer options
    double frameRate = 0;  // > 0: overrides the stream's rate (raw streams often have none)
    std::optional<RawVideoParams> raw;  // open as headerless raw video
};

struct Demuxer {
    ff::FormatContextPtr fmt;
    int stream = -1;  // selected video stream

    AVStream* videoStream() const { return fmt ? fmt->streams[stream] : nullptr; }
};

// Opens `path` (UTF-8), selects the best video stream (cover art excluded) and discards every
// other stream so the demuxer does not read them.
std::optional<Demuxer> openDemuxer(const std::string& path, const InputOptions& opts, std::string* error);

// Decoder used for a codec. AV1 prefers libdav1d in software (the native decoder needs hwaccel).
const AVCodec* chooseDecoder(AVCodecID id, bool hardware);

// True when byte-offset seeking is the reliable way to reach a packet (no usable index).
bool prefersByteSeek(const AVFormatContext* fmt);

} // namespace scvp
