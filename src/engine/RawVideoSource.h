#pragma once

#include "engine/VideoSource.h"

#include <optional>
#include <string>

namespace scvp {

// Headerless raw video: frame n is read from offset header + n x frameSize, no decoding. Every
// frame is a key frame, so sessions jump anywhere at the cost of one read.
class RawVideoSource final : public IVideoSource {
public:
    static std::unique_ptr<RawVideoSource> open(const std::string& path, const RawVideoParams& params, std::string* error);

    const VideoInfo& info() const override { return m_info; }
    FrameIndexPtr index() const override { return m_index; }
    bool indexComplete() const override { return true; }
    double indexProgress() const override { return 1.0; }
    SourceCaps caps() const override { return {true, 4}; }
    std::unique_ptr<IDecodeSession> openSession(const SessionConfig& cfg, std::string* error) override;
    bool waitForIndex(int) override { return true; }
    void setIndexListener(std::function<void()>) override {}

    const RawVideoParams& params() const { return m_params; }
    size_t frameSize() const { return m_frameSize; }

private:
    std::string m_path;
    RawVideoParams m_params;
    size_t m_frameSize = 0;
    VideoInfo m_info;
    FrameIndexPtr m_index;
};

// Size, pixel format and rate from names like "clip_1920x1080_nv12.yuv", "a.1280x720.p010le.30fps.raw"
// or "cam_3840x2160_yuv420p10le_60fps.yuv". Missing parts stay unset (format NONE, rate 0).
RawVideoParams parseRawVideoName(const std::string& path);

// Extensions that hold headerless raw video.
bool isRawVideoExtension(const std::string& path);

} // namespace scvp
