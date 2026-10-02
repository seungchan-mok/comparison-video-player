#pragma once

#include "engine/VideoSource.h"

#include <string>
#include <vector>

namespace scvp {

// A numbered series of still images (PNG, JPEG, TIFF, DPX, EXR, ...) played as video: a folder,
// or one file and its siblings that differ only in the number ("shot_0001.png"). Files are
// ordered naturally by name (gaps simply skip); each frame decodes on its own, so any frame is
// one file read away.
class ImageSequenceSource final : public IVideoSource {
public:
    static std::unique_ptr<ImageSequenceSource> open(const std::string& path, double frameRate, std::string* error);

    const VideoInfo& info() const override { return m_info; }
    FrameIndexPtr index() const override { return m_index; }
    bool indexComplete() const override { return true; }
    double indexProgress() const override { return 1.0; }
    SourceCaps caps() const override { return {true, 4}; }
    std::unique_ptr<IDecodeSession> openSession(const SessionConfig& cfg, std::string* error) override;
    bool waitForIndex(int) override { return true; }
    void setIndexListener(std::function<void()>) override {}

    const std::vector<std::string>& files() const { return m_files; }

private:
    std::vector<std::string> m_files;
    VideoInfo m_info;
    FrameIndexPtr m_index;
};

bool isImageExtension(const std::string& path);
bool isDirectory(const std::string& path);
// The files of the sequence `path` belongs to (or that the folder `path` holds), in order.
std::vector<std::string> findImageSequence(const std::string& path);
// "frame2" < "frame10".
bool naturalLess(const std::string& a, const std::string& b);

} // namespace scvp
