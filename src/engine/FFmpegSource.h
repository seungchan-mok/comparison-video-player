#pragma once

#include "engine/IndexScanner.h"
#include "engine/VideoSource.h"

#include <condition_variable>
#include <mutex>

namespace scvp {

// Containers and elementary streams read through libavformat/libavcodec. The index is built
// in the background by an IndexScanner; sessions open their own demuxer and decoder.
class FFmpegSource final : public IVideoSource {
public:
    static std::unique_ptr<FFmpegSource> open(const std::string& path, const InputOptions& opts, std::string* error);
    ~FFmpegSource() override;

    const VideoInfo& info() const override { return m_info; }
    FrameIndexPtr index() const override;
    bool indexComplete() const override;
    double indexProgress() const override;
    SourceCaps caps() const override { return {}; }
    std::unique_ptr<IDecodeSession> openSession(const SessionConfig& cfg, std::string* error) override;
    bool waitForIndex(int timeoutMs) override;

private:
    FFmpegSource() = default;
    void onSnapshot(FrameIndexPtr idx);

    std::string m_path;
    InputOptions m_opts;
    VideoInfo m_info;
    std::unique_ptr<IndexScanner> m_scanner;
    mutable std::mutex m_mutex;
    std::condition_variable m_indexChanged;
    FrameIndexPtr m_index;
};

} // namespace scvp
