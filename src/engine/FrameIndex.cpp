#include "engine/FrameIndex.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scvp {

// ---------------------------------------------------------------------------------------------
// FrameIndex

int32_t FrameIndex::presFromDecode(int32_t dec) const
{
    if (dec < 0 || dec >= packetCount())
        return -1;
    return m_packets[size_t(dec)].pres;
}

int32_t FrameIndex::decodeFromPres(int32_t pres) const
{
    if (pres < 0 || pres >= frameCount())
        return -1;
    return m_presToDec[size_t(pres)];
}

int64_t FrameIndex::ptsOf(int32_t pres) const
{
    if (pres < 0 || pres >= frameCount())
        return AV_NOPTS_VALUE;
    return m_presPts[size_t(pres)];
}

double FrameIndex::secondsOf(int32_t pres) const
{
    const int64_t pts = ptsOf(pres);
    if (pts == AV_NOPTS_VALUE || m_firstPts == AV_NOPTS_VALUE) {
        return m_frameRate > 0 ? pres / m_frameRate : 0.0;
    }
    return double(pts - m_firstPts) * av_q2d(m_timeBase);
}

int32_t FrameIndex::presAtSeconds(double seconds) const
{
    if (frameCount() == 0)
        return -1;
    if (m_ordering != Ordering::Pts || m_firstPts == AV_NOPTS_VALUE) {
        const double fps = m_frameRate > 0 ? m_frameRate : 25.0;
        return std::clamp(int32_t(std::floor(seconds * fps + 1e-6)), 0, frameCount() - 1);
    }
    const auto target = m_firstPts + int64_t(std::floor(seconds / av_q2d(m_timeBase) + 1e-6));
    auto it = std::upper_bound(m_presPts.begin(), m_presPts.end(), target);
    if (it == m_presPts.begin())
        return 0;
    return int32_t(it - m_presPts.begin()) - 1;
}

int32_t FrameIndex::estimatePres(int64_t pts) const
{
    if (pts == AV_NOPTS_VALUE || m_firstPts == AV_NOPTS_VALUE || m_frameRate <= 0)
        return -1;
    return int32_t(std::llround(double(pts - m_firstPts) * av_q2d(m_timeBase) * m_frameRate));
}

bool FrameIndex::usableKey(int32_t dec) const
{
    const IndexPacket& p = m_packets[size_t(dec)];
    // Discarded keys (edit-list pre-roll) are valid decode starts; hidden ones are not.
    return p.isKey() && !((p.flags & IndexPacket::Hidden) && !(p.flags & IndexPacket::Discard));
}

SeekPoint FrameIndex::pointAt(int32_t dec) const
{
    const IndexPacket& p = m_packets[size_t(dec)];
    SeekPoint sp;
    sp.dec = dec;
    sp.pres = p.pres;
    sp.pts = p.pts;
    sp.dts = p.dts;
    sp.pos = p.pos;
    sp.size = p.size;
    return sp;
}

SeekPoint FrameIndex::seekPointFor(int32_t pres) const
{
    const int32_t target = decodeFromPres(pres);
    if (target < 0 || m_keyDecs.empty())
        return m_packets.empty() ? SeekPoint{} : pointAt(0);
    const IndexPacket& t = m_packets[size_t(target)];
    // The last usable key at or before the target in decode order that is not displayed after
    // the target. A key displayed after the target means the target is one of its leading
    // pictures (open GOP), which are undecodable when decoding starts at that key.
    auto it = std::upper_bound(m_keyDecs.begin(), m_keyDecs.end(), target);
    while (it != m_keyDecs.begin()) {
        --it;
        const int32_t k = *it;
        if (!usableKey(k))
            continue;
        const IndexPacket& kp = m_packets[size_t(k)];
        const bool earlierSegment = kp.segment < t.segment;
        if (earlierSegment || kp.pts == AV_NOPTS_VALUE || t.pts == AV_NOPTS_VALUE || kp.pts <= t.pts)
            return pointAt(k);
    }
    return pointAt(m_keyDecs.front());
}

SeekPoint FrameIndex::seekPointBefore(const SeekPoint& sp) const
{
    auto it = std::lower_bound(m_keyDecs.begin(), m_keyDecs.end(), sp.dec);
    while (it != m_keyDecs.begin()) {
        --it;
        if (usableKey(*it))
            return pointAt(*it);
    }
    return SeekPoint{};
}

int32_t FrameIndex::decodeIndexForPos(int64_t pos) const
{
    if (pos < 0)
        return -1;
    auto it = std::lower_bound(m_posToDec.begin(), m_posToDec.end(), std::make_pair(pos, int32_t(-1)));
    if (it == m_posToDec.end() || it->first != pos)
        return -1;
    return it->second;
}

int32_t FrameIndex::gopLengthAround(int32_t pres) const
{
    if (m_keyPres.empty())
        return std::max(1, frameCount());
    auto next = std::upper_bound(m_keyPres.begin(), m_keyPres.end(), pres);
    const int32_t end = next == m_keyPres.end() ? frameCount() : *next;
    const int32_t begin = next == m_keyPres.begin() ? 0 : *(next - 1);
    return std::max(1, end - begin);
}

double FrameIndex::meanGopLength() const
{
    return m_keyPres.empty() ? double(std::max(1, frameCount())) : double(frameCount()) / double(m_keyPres.size());
}

// ---------------------------------------------------------------------------------------------
// FrameIndexBuilder

FrameIndexBuilder::FrameIndexBuilder(AVRational timeBase, double nominalFrameRate)
    : m_timeBase(timeBase.num > 0 && timeBase.den > 0 ? timeBase : AVRational{1, 90000})
    , m_nominalFrameRate(nominalFrameRate)
{
}

void FrameIndexBuilder::add(const AVPacket& pkt)
{
    IndexPacket p;
    p.pts = pkt.pts;
    p.dts = pkt.dts;
    p.pos = pkt.pos;
    p.size = pkt.size;
    if (pkt.flags & AV_PKT_FLAG_KEY)
        p.flags |= IndexPacket::Key;
    if (pkt.flags & AV_PKT_FLAG_DISCARD)
        p.flags |= IndexPacket::Discard;
    if (pkt.flags & AV_PKT_FLAG_CORRUPT)
        p.flags |= IndexPacket::Corrupt;
    add(p);
}

void FrameIndexBuilder::add(const IndexPacket& in)
{
    IndexPacket p = in;
    if (p.pts == AV_NOPTS_VALUE)
        ++m_missingPts;
    // A backwards dts jump of more than a second starts a new timestamp segment.
    if (p.dts != AV_NOPTS_VALUE) {
        const int64_t second = av_rescale_q(1, AVRational{1, 1}, m_timeBase);
        if (m_maxDts != AV_NOPTS_VALUE && p.dts < m_maxDts - second) {
            ++m_segment;
            m_maxDts = p.dts;
        } else if (m_maxDts == AV_NOPTS_VALUE || p.dts > m_maxDts) {
            m_maxDts = p.dts;
        }
    }
    p.segment = m_segment;
    p.pres = -1;
    m_packets.push_back(p);
}

std::shared_ptr<FrameIndex> FrameIndexBuilder::build(bool complete) const
{
    auto idx = std::make_shared<FrameIndex>();
    idx->m_packets = m_packets;
    idx->m_timeBase = m_timeBase;
    idx->m_complete = complete;
    idx->m_ordering = m_ordering;
    auto& pk = idx->m_packets;
    const int32_t n = int32_t(pk.size());

    // Frames the decoder never outputs at the start of the stream: everything before the first
    // key, and (open GOP) everything displayed before the first frame a probe decode produced.
    int32_t firstKey = -1;
    for (int32_t i = 0; i < n; ++i)
        if (pk[size_t(i)].isKey()) {
            firstKey = i;
            break;
        }
    for (int32_t i = 0; i < n; ++i) {
        IndexPacket& p = pk[size_t(i)];
        if (firstKey >= 0 && i < firstKey)
            p.flags |= IndexPacket::Hidden;
        if (m_firstOutputPts != AV_NOPTS_VALUE && p.segment == 0 && p.pts != AV_NOPTS_VALUE &&
            p.pts < m_firstOutputPts && !(p.flags & IndexPacket::Discard))
            p.flags |= IndexPacket::Hidden;
    }

    std::vector<int32_t> shown;
    shown.reserve(size_t(n));
    for (int32_t i = 0; i < n; ++i)
        if (pk[size_t(i)].shown())
            shown.push_back(i);

    size_t stable = shown.size();
    if (m_ordering == FrameIndex::Ordering::Pts) {
        std::stable_sort(shown.begin(), shown.end(), [&](int32_t a, int32_t b) {
            const IndexPacket& pa = pk[size_t(a)];
            const IndexPacket& pb = pk[size_t(b)];
            if (pa.segment != pb.segment)
                return pa.segment < pb.segment;
            return pa.pts < pb.pts;
        });
        if (!complete && !shown.empty()) {
            // A frame's position is final once no later packet can be displayed before it:
            // later packets have dts >= the largest dts seen and pts >= dts.
            const int16_t lastSeg = m_segment;
            stable = 0;
            for (int32_t d : shown) {
                const IndexPacket& p = pk[size_t(d)];
                if (p.segment < lastSeg || (m_maxDts != AV_NOPTS_VALUE && p.pts <= m_maxDts))
                    ++stable;
                else
                    break;
            }
        }
    }
    // Decode / Arithmetic orderings: presentation order is decode order.

    idx->m_presToDec.assign(shown.begin(), shown.begin() + long(stable));
    idx->m_presPts.resize(stable);
    for (size_t r = 0; r < stable; ++r) {
        IndexPacket& p = pk[size_t(shown[r])];
        p.pres = int32_t(r);
        idx->m_presPts[r] = p.pts;
    }
    if (stable)
        idx->m_firstPts = pk[size_t(shown[0])].pts;

    for (int32_t i = 0; i < n; ++i) {
        const IndexPacket& p = pk[size_t(i)];
        if (p.isKey()) {
            idx->m_keyDecs.push_back(i);
            if (p.pres >= 0)
                idx->m_keyPres.push_back(p.pres);
        }
        if (p.pos >= 0)
            idx->m_posToDec.emplace_back(p.pos, i);
    }
    std::sort(idx->m_keyPres.begin(), idx->m_keyPres.end());
    std::sort(idx->m_posToDec.begin(), idx->m_posToDec.end());

    // Reordering depth: how far beyond a frame's own packet decoding must have progressed
    // before frames up to it can all be output.
    int32_t runMax = -1;
    for (size_t r = 0; r < stable; ++r) {
        runMax = std::max(runMax, idx->m_presToDec[r]);
        idx->m_reorderDepth = std::max(idx->m_reorderDepth, runMax - idx->m_presToDec[r]);
    }

    // Frame rate from the median spacing of consecutive displayed frames.
    double fps = m_nominalFrameRate;
    if (m_ordering == FrameIndex::Ordering::Pts && stable >= 2) {
        std::vector<int64_t> deltas;
        deltas.reserve(stable - 1);
        for (size_t r = 1; r < stable; ++r) {
            const int64_t d = idx->m_presPts[r] - idx->m_presPts[r - 1];
            if (d > 0)
                deltas.push_back(d);
        }
        if (!deltas.empty()) {
            std::nth_element(deltas.begin(), deltas.begin() + long(deltas.size() / 2), deltas.end());
            const double median = double(deltas[deltas.size() / 2]) * av_q2d(m_timeBase);
            if (median > 0) {
                const double measured = 1.0 / median;
                // Millisecond time bases (Matroska) round 33.33 ms to 33/34 ms; snap to the
                // nominal rate when it agrees within 2 %.
                fps = (m_nominalFrameRate > 0 && std::abs(measured - m_nominalFrameRate) / m_nominalFrameRate < 0.02)
                          ? m_nominalFrameRate
                          : measured;
            }
        }
    }
    idx->m_frameRate = fps > 0 ? fps : 25.0;
    return idx;
}

} // namespace scvp
