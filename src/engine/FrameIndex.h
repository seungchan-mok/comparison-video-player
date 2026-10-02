#pragma once

#include "engine/FfmpegCompat.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace scvp {

// One demuxed packet of the video stream (decode order).
struct IndexPacket {
    enum Flag : uint16_t {
        Key = 1 << 0,      // random access point (AV_PKT_FLAG_KEY)
        Discard = 1 << 1,  // decoded only as a reference (AV_PKT_FLAG_DISCARD, edit-list pre-roll)
        Hidden = 1 << 2,   // never output: before the first decodable frame of the stream
        Corrupt = 1 << 3,  // AV_PKT_FLAG_CORRUPT
    };
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t pos = -1;
    int32_t size = 0;
    int32_t pres = -1;     // presentation index; -1 when never shown or not yet final
    uint16_t flags = 0;
    int16_t segment = 0;   // timestamp-continuous run (MPEG-TS discontinuities start a new one)

    bool isKey() const { return flags & Key; }
    bool shown() const { return !(flags & (Discard | Hidden)); }
};

// A key packet decoding can start from to reach some frame.
struct SeekPoint {
    int32_t dec = -1;   // decode index of the key packet
    int32_t pres = -1;  // its presentation index (-1 for discarded/hidden keys)
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t pos = -1;
    int32_t size = 0;

    bool valid() const { return dec >= 0; }
};

// Immutable mapping between decode order (packets) and presentation order (frame numbers) of
// one video stream, with random-access points and timing. While a file is still being scanned,
// newer snapshots replace older ones; only the "stable" prefix of a partial index has final
// frame numbers.
class FrameIndex {
public:
    enum class Ordering { Pts, Decode, Poc, Arithmetic };

    int32_t frameCount() const { return int32_t(m_presToDec.size()); }
    int32_t packetCount() const { return int32_t(m_packets.size()); }
    bool complete() const { return m_complete; }
    Ordering ordering() const { return m_ordering; }
    AVRational timeBase() const { return m_timeBase; }
    double frameRate() const { return m_frameRate; }
    int64_t firstPts() const { return m_firstPts; }

    const IndexPacket& packet(int32_t dec) const { return m_packets[size_t(dec)]; }
    int32_t presFromDecode(int32_t dec) const;
    int32_t decodeFromPres(int32_t pres) const;
    int64_t ptsOf(int32_t pres) const;
    double secondsOf(int32_t pres) const;          // relative to frame 0
    int32_t presAtSeconds(double seconds) const;   // last frame shown at or before `seconds`
    int32_t estimatePres(int64_t pts) const;       // provisional number for an unindexed pts

    SeekPoint seekPointFor(int32_t pres) const;
    SeekPoint seekPointBefore(const SeekPoint& sp) const;  // previous usable key (retries)
    int32_t decodeIndexForPos(int64_t pos) const;           // -1 when unknown

    const std::vector<int32_t>& keyFrames() const { return m_keyPres; }  // shown keys, ascending
    int32_t gopLengthAround(int32_t pres) const;
    double meanGopLength() const;
    // Max number of packets the decoder must receive beyond a frame's own packet before that
    // frame can be output (B-frame reordering depth).
    int32_t reorderDepth() const { return m_reorderDepth; }

private:
    friend class FrameIndexBuilder;
    bool usableKey(int32_t dec) const;
    SeekPoint pointAt(int32_t dec) const;

    std::vector<IndexPacket> m_packets;
    std::vector<int32_t> m_presToDec;
    std::vector<int64_t> m_presPts;
    std::vector<int32_t> m_keyDecs;
    std::vector<int32_t> m_keyPres;
    std::vector<std::pair<int64_t, int32_t>> m_posToDec;  // sorted by pos
    AVRational m_timeBase{1, 1};
    double m_frameRate = 0;
    int64_t m_firstPts = AV_NOPTS_VALUE;
    int32_t m_reorderDepth = 0;
    Ordering m_ordering = Ordering::Pts;
    bool m_complete = false;
};

using FrameIndexPtr = std::shared_ptr<const FrameIndex>;

// Accumulates packets (decode order) and produces FrameIndex snapshots.
class FrameIndexBuilder {
public:
    FrameIndexBuilder(AVRational timeBase, double nominalFrameRate);

    void add(const AVPacket& pkt);
    void add(const IndexPacket& p);
    // Presentation time of the first frame the decoder outputs when starting at the beginning;
    // frames before it (leading pictures of an open first GOP) are hidden.
    void setFirstOutputPts(int64_t pts) { m_firstOutputPts = pts; }
    void setOrdering(FrameIndex::Ordering o) { m_ordering = o; }

    size_t size() const { return m_packets.size(); }
    bool missingPts() const { return m_missingPts > 0; }

    std::shared_ptr<FrameIndex> build(bool complete) const;

private:
    std::vector<IndexPacket> m_packets;
    AVRational m_timeBase;
    double m_nominalFrameRate;
    int64_t m_firstOutputPts = AV_NOPTS_VALUE;
    int64_t m_maxDts = AV_NOPTS_VALUE;
    int16_t m_segment = 0;
    size_t m_missingPts = 0;
    FrameIndex::Ordering m_ordering = FrameIndex::Ordering::Pts;
};

} // namespace scvp
