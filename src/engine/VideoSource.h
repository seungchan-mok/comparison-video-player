#pragma once

#include "engine/Demux.h"
#include "engine/Frame.h"
#include "engine/FrameAllocator.h"
#include "engine/FrameIndex.h"
#include "engine/HwDevice.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace scvp {

// Static description of an opened video stream.
struct VideoInfo {
    std::string path;
    std::string formatName;   // demuxer
    std::string codecName;
    int width = 0;
    int height = 0;
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;  // as declared by the container; frames are authoritative
    AVRational timeBase{1, 1};
    AVRational sampleAspect{1, 1};
    double nominalFrameRate = 0;  // container/codec declaration; FrameIndex::frameRate() is measured
    double durationSeconds = 0;
    double rotation = 0;          // display-matrix degrees, counter-clockwise
    int64_t bitRate = 0;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
    AVChromaLocation chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
};

struct SourceCaps {
    bool randomAccess = false;  // every frame decodes independently (raw video, image sequences)
    int maxSessions = 1;        // sessions that can usefully decode in parallel
};

struct SessionConfig {
    std::shared_ptr<FramePool> pool;  // frame buffers; required
    int threads = 0;                  // decoder threads, 0 = automatic
    HwConfig hw;
};

struct DecodeHints {
    // Non-key packets whose frame number is below this are decoded with AVDISCARD_NONREF:
    // frames nobody needs are skipped, references are still decoded. -1 disables.
    int32_t skipNonRefBelow = -1;
    // Finer alternative (takes precedence): non-key packets whose frame is not needed are
    // decoded with AVDISCARD_NONREF.
    const std::function<bool(int32_t frame)>* frameNeeded = nullptr;
    // Frames decoded on a CUDA device stay in device memory when this returns true for them
    // (zero-copy display); otherwise they are downloaded to system memory.
    const std::function<bool(int32_t frame)>* keepOnGpu = nullptr;
    // Decoding stops with Cancelled as soon as *cancel != generation.
    const std::atomic<uint64_t>* cancel = nullptr;
    uint64_t generation = 0;

    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed) != generation; }
};

enum class DecodeStatus { Ok, Eof, Cancelled, Error };

// One decoder instance positioned somewhere in the stream. Not thread-safe; owned by a lane.
class IDecodeSession {
public:
    struct Position {
        int32_t nextDec = 0;   // decode index of the next packet to be fed (-1 unknown)
        int32_t lastPres = -1; // frame number of the last frame returned since the last seek
        bool eof = false;      // all frames up to the end of the stream were returned
    };

    virtual ~IDecodeSession() = default;
    // Positions the decoder so that the following next() calls return frames from `sp` on.
    virtual DecodeStatus seekTo(const SeekPoint& sp) = 0;
    // Next frame in presentation order (frames that are never shown are skipped).
    virtual DecodeStatus next(FramePtr& out, const DecodeHints& hints) = 0;
    virtual Position position() const = 0;
    // Whether the packet with this decode index was fed with AVDISCARD_NONREF since the last
    // seek (its frame will not come out by continuing).
    virtual bool wasSkipped(int32_t dec) const = 0;
    // Frames whose number contradicted the index since the session was opened.
    virtual int indexMismatches() const = 0;
    // The hardware device decoding right now ("cuda", ...), or "" for software.
    virtual std::string hardwareDevice() const = 0;
};

class IVideoSource {
public:
    virtual ~IVideoSource() = default;
    virtual const VideoInfo& info() const = 0;
    virtual FrameIndexPtr index() const = 0;  // latest snapshot; may be partial while scanning
    virtual bool indexComplete() const = 0;
    virtual double indexProgress() const = 0;
    virtual SourceCaps caps() const = 0;
    virtual std::unique_ptr<IDecodeSession> openSession(const SessionConfig& cfg, std::string* error) = 0;
    // Blocks until the index is complete (or timeout); for tests and tools.
    virtual bool waitForIndex(int timeoutMs) = 0;
    // Called (from the scanner thread) whenever a newer index snapshot is published.
    virtual void setIndexListener(std::function<void()> listener) = 0;
};

// Opens a media file, picking the source implementation from its type.
std::unique_ptr<IVideoSource> openVideoSource(const std::string& path, const InputOptions& opts, std::string* error);

} // namespace scvp
