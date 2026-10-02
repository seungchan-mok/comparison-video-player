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
    : m_budget(budgetBytes)
{
}

void FrameCache::setBudget(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_budget = bytes;
    evictToBudgetLocked(-1);
}

size_t FrameCache::budget() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_budget;
}

size_t FrameCache::bytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes;
}

size_t FrameCache::frames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.size();
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

// The cost function only has maxima at a few places: the two ends of the map (V shape around
// the playhead, or frames outside the loop), the frame just before the playhead (wrapped loop)
// and the last frame inside the loop. Protected frames are skipped by walking inwards.
FrameCache::Map::iterator FrameCache::worstLocked()
{
    if (m_frames.empty())
        return m_frames.end();
    std::vector<Map::iterator> candidates;
    auto unprotectedFromFront = [&](Map::iterator it) {
        while (it != m_frames.end() && m_policy.isProtected(it->first))
            ++it;
        return it;
    };
    auto unprotectedFromBack = [&](Map::iterator it) {
        for (;;) {
            if (!m_policy.isProtected(it->first))
                return it;
            if (it == m_frames.begin())
                return m_frames.end();
            --it;
        }
    };
    candidates.push_back(unprotectedFromFront(m_frames.begin()));
    candidates.push_back(unprotectedFromBack(std::prev(m_frames.end())));
    if (m_policy.loopWraps && m_policy.loop.size() > 0) {
        auto below = m_frames.lower_bound(m_policy.playhead);
        if (below != m_frames.begin())
            candidates.push_back(unprotectedFromBack(std::prev(below)));
        auto loopEnd = m_frames.upper_bound(m_policy.loop.last);
        if (loopEnd != m_frames.begin())
            candidates.push_back(unprotectedFromBack(std::prev(loopEnd)));
    }
    Map::iterator worst = m_frames.end();
    double worstCost = -std::numeric_limits<double>::infinity();
    for (auto it : candidates) {
        if (it == m_frames.end())
            continue;
        const double c = m_policy.cost(it->first);
        if (c > worstCost) {
            worstCost = c;
            worst = it;
        }
    }
    if (worst != m_frames.end())
        return worst;
    // Everything at the candidate positions is protected: fall back to a full scan.
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (m_policy.isProtected(it->first))
            continue;
        const double c = m_policy.cost(it->first);
        if (c > worstCost) {
            worstCost = c;
            worst = it;
        }
    }
    return worst;
}

void FrameCache::evictToBudgetLocked(int32_t keep)
{
    while (m_bytes > m_budget && !m_frames.empty()) {
        auto worst = worstLocked();
        if (worst == m_frames.end() || worst->first == keep)
            break;
        m_bytes -= worst->second->bytes;
        m_frames.erase(worst);
        bumpLocked();
    }
}

bool FrameCache::offer(FramePtr frame)
{
    if (!frame || frame->pres < 0)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const int32_t n = frame->pres;
    auto existing = m_frames.find(n);
    if (existing != m_frames.end()) {
        // A definitive frame replaces a provisional one; otherwise keep what is there.
        if (!existing->second->provisional || frame->provisional)
            return true;
        m_bytes -= existing->second->bytes;
        m_frames.erase(existing);
        bumpLocked();
    }
    // Protected frames (the frame on screen, a pinned loop) are kept even beyond the budget.
    const bool prot = m_policy.isProtected(n);
    if (!prot) {
        if (frame->bytes > m_budget)
            return false;
        if (m_bytes + frame->bytes > m_budget) {
            // Full: accept only frames needed sooner than the least needed cached one.
            auto worst = worstLocked();
            if (worst == m_frames.end() || m_policy.cost(n) >= m_policy.cost(worst->first))
                return false;
        }
    }
    m_bytes += frame->bytes;
    m_frames.emplace(n, std::move(frame));
    bumpLocked();
    evictToBudgetLocked(n);
    if (!prot && m_bytes > m_budget) {
        auto it = m_frames.find(n);
        m_bytes -= it->second->bytes;
        m_frames.erase(it);
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

double FrameCache::worstCost() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* self = const_cast<FrameCache*>(this);
    auto it = self->worstLocked();
    return it == m_frames.end() ? -1.0 : m_policy.cost(it->first);
}

void FrameCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.clear();
    m_bytes = 0;
    bumpLocked();
}

void FrameCache::dropProvisional()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_frames.begin(); it != m_frames.end();) {
        if (it->second->provisional) {
            m_bytes -= it->second->bytes;
            it = m_frames.erase(it);
        } else {
            ++it;
        }
    }
    bumpLocked();
}

void FrameCache::erase(int32_t frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.find(frame);
    if (it == m_frames.end())
        return;
    m_bytes -= it->second->bytes;
    m_frames.erase(it);
    bumpLocked();
}

RangeSetPtr FrameCache::ranges() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rangesVersion == m_version && m_rangesSnapshot)
        return m_rangesSnapshot;
    auto rs = std::make_shared<RangeSet>();
    rs->frames = m_frames.size();
    rs->bytes = m_bytes;
    for (const auto& [n, f] : m_frames) {
        if (!rs->ranges.empty() && rs->ranges.back().last + 1 == n)
            rs->ranges.back().last = n;
        else
            rs->ranges.push_back({n, n});
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
