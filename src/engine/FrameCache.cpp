#include "engine/FrameCache.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace scvp {

double CachePolicy::cost(int32_t f) const
{
    const int32_t p = playhead;
    if (loopWraps && loop.size() > 0) {
        if (!loop.contains(f))
            return 1e12 + std::abs(double(f) - double(p));  // not needed while looping
        if (loop.contains(p)) {
            if (f >= p)
                return double(f - p);
            return double(loop.last - p + 1) + double(f - loop.first);  // reached after wrapping
        }
    }
    const double ahead = direction >= 0 ? double(f) - double(p) : double(p) - double(f);
    return ahead >= 0 ? ahead : -ahead * behindWeight;
}

bool CachePolicy::isProtected(int32_t f) const
{
    for (const FrameRange& r : protect)
        if (r.contains(f))
            return true;
    return false;
}

FrameCache::FrameCache(size_t budgetBytes)
{
    m_budget[Ram] = budgetBytes;
}

void FrameCache::setBudget(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_budget[Ram] = bytes;
    evictToBudgetLocked(Ram, -1, nullptr);
}

size_t FrameCache::budget() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_budget[Ram];
}

size_t FrameCache::bytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes[Ram];
}

size_t FrameCache::frames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.size();
}

void FrameCache::setGpuBudget(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_budget[Vram] = bytes;
    // Frames that no longer fit are simply dropped here (no one to copy them down).
    evictToBudgetLocked(Vram, -1, nullptr);
}

size_t FrameCache::gpuBudget() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_budget[Vram];
}

size_t FrameCache::gpuBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes[Vram];
}

size_t FrameCache::gpuFrames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_keys[Vram].size();
}

void FrameCache::setPolicy(CachePolicy policy)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_policy = std::move(policy);
}

CachePolicy FrameCache::policy() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_policy;
}

void FrameCache::insertLocked(FramePtr f)
{
    const Tier t = tierOf(*f);
    m_bytes[t] += f->bytes;
    m_keys[t].insert(f->pres);
    m_frames.emplace(f->pres, std::move(f));
    bumpLocked();
}

void FrameCache::eraseLocked(Map::iterator it)
{
    const Tier t = tierOf(*it->second);
    m_bytes[t] -= it->second->bytes;
    m_keys[t].erase(it->first);
    m_frames.erase(it);
    bumpLocked();
}

// The cost function only peaks at a few places: the two ends of a tier's frames (V shape around
// the playhead, frames outside the loop), the frame just before the playhead (wrapped loop) and
// the last frame inside the loop. Protected frames are skipped by walking inwards.
int32_t FrameCache::worstLocked(Tier t) const
{
    const Keys& keys = m_keys[t];
    if (keys.empty())
        return -1;
    std::vector<Keys::const_iterator> candidates;
    auto fromFront = [&](Keys::const_iterator it) {
        while (it != keys.end() && m_policy.isProtected(*it))
            ++it;
        return it;
    };
    auto fromBack = [&](Keys::const_iterator it) {
        for (;;) {
            if (!m_policy.isProtected(*it))
                return it;
            if (it == keys.begin())
                return keys.end();
            --it;
        }
    };
    candidates.push_back(fromFront(keys.begin()));
    candidates.push_back(fromBack(std::prev(keys.end())));
    if (m_policy.loopWraps && m_policy.loop.size() > 0) {
        auto below = keys.lower_bound(m_policy.playhead);
        if (below != keys.begin())
            candidates.push_back(fromBack(std::prev(below)));
        auto loopEnd = keys.upper_bound(m_policy.loop.last);
        if (loopEnd != keys.begin())
            candidates.push_back(fromBack(std::prev(loopEnd)));
    }
    int32_t worst = -1;
    double worstCost = -std::numeric_limits<double>::infinity();
    for (auto it : candidates) {
        if (it == keys.end())
            continue;
        const double c = m_policy.cost(*it);
        if (c > worstCost) {
            worstCost = c;
            worst = *it;
        }
    }
    if (worst >= 0)
        return worst;
    for (int32_t k : keys) {  // everything at the candidate positions is protected
        if (m_policy.isProtected(k))
            continue;
        const double c = m_policy.cost(k);
        if (c > worstCost) {
            worstCost = c;
            worst = k;
        }
    }
    return worst;
}

void FrameCache::evictToBudgetLocked(Tier t, int32_t keep, std::vector<FramePtr>* spilled)
{
    while (m_bytes[t] > m_budget[t] && !m_keys[t].empty()) {
        const int32_t worst = worstLocked(t);
        if (worst < 0 || worst == keep)
            break;
        auto it = m_frames.find(worst);
        if (spilled && t == Vram)
            spilled->push_back(it->second);
        eraseLocked(it);
    }
}

bool FrameCache::wantsGpu(int32_t frame, size_t bytes) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_budget[Vram] == 0 || bytes > m_budget[Vram])
        return false;
    if (m_policy.isProtected(frame) || m_bytes[Vram] + bytes <= m_budget[Vram])
        return true;
    const int32_t worst = worstLocked(Vram);
    return worst >= 0 && m_policy.cost(frame) < m_policy.cost(worst);
}

bool FrameCache::offer(FramePtr frame, std::vector<FramePtr>* spilled)
{
    if (!frame || frame->pres < 0)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const int32_t n = frame->pres;
    const Tier t = tierOf(*frame);
    auto existing = m_frames.find(n);
    if (existing != m_frames.end()) {
        const Frame& old = *existing->second;
        // Replace a provisional frame with a definitive one, and a spilled VRAM frame with its
        // system-memory copy; otherwise keep what is there.
        const bool upgrade = old.provisional && !frame->provisional;
        const bool tierMove = tierOf(old) != t;
        if (!upgrade && !tierMove)
            return true;
        eraseLocked(existing);
    }
    const bool prot = m_policy.isProtected(n);
    if (!prot) {
        if (frame->bytes > m_budget[t])
            return false;
        if (m_bytes[t] + frame->bytes > m_budget[t]) {
            // Tier full: accept only frames needed sooner than its least needed frame.
            const int32_t worst = worstLocked(t);
            if (worst < 0 || m_policy.cost(n) >= m_policy.cost(worst))
                return false;
        }
    }
    insertLocked(frame);
    evictToBudgetLocked(t, n, spilled);
    if (!prot && m_bytes[t] > m_budget[t]) {
        eraseLocked(m_frames.find(n));
        return false;
    }
    return true;
}

FramePtr FrameCache::get(int32_t frame) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.find(frame);
    return it == m_frames.end() ? nullptr : it->second;
}

bool FrameCache::contains(int32_t frame) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.count(frame) != 0;
}

FramePtr FrameCache::nearest(int32_t frame, int32_t maxDistance) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_frames.empty())
        return nullptr;
    auto hi = m_frames.lower_bound(frame);
    FramePtr best;
    int64_t bestDist = int64_t(maxDistance) + 1;
    if (hi != m_frames.begin()) {
        auto lo = std::prev(hi);
        const int64_t d = int64_t(frame) - lo->first;
        if (d < bestDist) {
            bestDist = d;
            best = lo->second;
        }
    }
    if (hi != m_frames.end()) {
        const int64_t d = int64_t(hi->first) - frame;
        if (d < bestDist)
            best = hi->second;
    }
    return best;
}

int32_t FrameCache::firstMissing(int32_t first, int32_t last) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int32_t expected = first;
    for (auto it = m_frames.lower_bound(first); it != m_frames.end() && it->first <= last; ++it) {
        if (it->first > expected)
            return expected;
        expected = it->first + 1;
    }
    return expected <= last ? expected : -1;
}

int32_t FrameCache::lastMissing(int32_t first, int32_t last) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int32_t expected = last;
    auto it = m_frames.upper_bound(last);
    while (it != m_frames.begin()) {
        --it;
        if (it->first < first)
            break;
        if (it->first < expected)
            return expected;
        expected = it->first - 1;
    }
    return expected >= first ? expected : -1;
}

int32_t FrameCache::missingRunEnd(int32_t from, int32_t limit) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.lower_bound(from);
    if (it != m_frames.end() && it->first == from)
        return from - 1;  // `from` itself is cached: empty run
    if (it == m_frames.end() || it->first > limit)
        return limit;
    return it->first - 1;
}

int32_t FrameCache::countCached(int32_t first, int32_t last) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int32_t n = 0;
    for (auto it = m_frames.lower_bound(first); it != m_frames.end() && it->first <= last; ++it)
        ++n;
    return n;
}

int32_t FrameCache::lastCached(int32_t first, int32_t last) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.upper_bound(last);
    if (it == m_frames.begin())
        return -1;
    --it;
    return it->first >= first ? it->first : -1;
}

double FrameCache::worstCost() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int32_t w = worstLocked(Ram);
    return w < 0 ? -1.0 : m_policy.cost(w);
}

void FrameCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.clear();
    for (int t = 0; t < 2; ++t) {
        m_keys[t].clear();
        m_bytes[t] = 0;
    }
    bumpLocked();
}

void FrameCache::dropProvisional()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_frames.begin(); it != m_frames.end();) {
        auto next = std::next(it);
        if (it->second->provisional)
            eraseLocked(it);
        it = next;
    }
    bumpLocked();
}

void FrameCache::erase(int32_t frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.find(frame);
    if (it != m_frames.end())
        eraseLocked(it);
}

RangeSetPtr FrameCache::ranges() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rangesVersion == m_version && m_rangesSnapshot)
        return m_rangesSnapshot;
    auto rs = std::make_shared<RangeSet>();
    rs->frames = m_frames.size();
    rs->bytes = m_bytes[Ram] + m_bytes[Vram];
    rs->gpuFrames = m_keys[Vram].size();
    rs->gpuBytes = m_bytes[Vram];
    for (const auto& [n, f] : m_frames) {
        const bool gpu = f->location == FrameLocation::Cuda;
        if (!rs->ranges.empty() && rs->ranges.back().last + 1 == n && rs->ranges.back().gpu == gpu)
            rs->ranges.back().last = n;
        else
            rs->ranges.push_back({n, n, gpu});
    }
    m_rangesSnapshot = rs;
    m_rangesVersion = m_version;
    return rs;
}

uint64_t FrameCache::version() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_version;
}

} // namespace scvp
