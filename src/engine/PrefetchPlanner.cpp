#include "engine/PrefetchPlanner.h"

#include <algorithm>
#include <cmath>

namespace scvp {
namespace {

FrameRange clip(FrameRange r, int32_t n)
{
    r.first = std::max(r.first, 0);
    r.last = std::min(r.last, n - 1);
    return r;
}

void push(WantList& w, FrameRange r, int32_t n, WantClass cls, int fill)
{
    r = clip(r, n);
    if (r.size() > 0)
        w.wants.push_back({r, cls, fill});
}

} // namespace

WantList planPrefetch(const PlannerInput& in)
{
    WantList w;
    const int32_t n = in.frameCount;
    if (n <= 0)
        return w;
    const int32_t p = std::clamp(in.playhead, 0, n - 1);
    const int dir = in.direction >= 0 ? 1 : -1;
    const int32_t cap = std::max(in.capacity, 1);
    w.target = p;
    w.direction = dir;
    w.playing = in.playing;
    w.wants.push_back({{p, p}, WantClass::Target, 1});

    // A loop only shapes the plan while the playhead is inside it.
    const bool loop = in.loopActive && in.loop.size() > 0 && clip(in.loop, n).contains(p);
    const FrameRange loopR = loop ? clip(in.loop, n) : FrameRange{0, -1};
    const bool loopPinned = loop && double(loopR.size()) <= 0.9 * cap;  // whole loop stays cached
    const bool loopWraps = loop && in.playing && dir > 0;

    // While paused, a few frames at each upcoming jump target are kept too (at most 5 % of the
    // capacity); the window shrinks by that much so cache and plan stay consistent.
    int32_t jumps = 0;
    for (int32_t j : in.jumpDistances)
        jumps += j > 0 ? 2 : 0;
    const int32_t islandReserve = in.playing ? 0 : std::min<int32_t>(cap / 20, 4 * jumps);
    const int32_t winCap = std::max(1, cap - islandReserve);

    // Share of the capacity behind the playhead (opposite to the move direction), in percent.
    const int32_t behindPercent = in.playing ? 20 : 40;
    int32_t behind = int32_t(int64_t(winCap) * behindPercent / 100);
    if (!in.playing && in.gop > 0 && winCap >= 2 * in.gop + 8)
        behind = std::max(behind, in.gop + 8);
    behind = std::min(behind, winCap - 1);
    int32_t ahead = winCap - 1 - behind;  // the target itself takes one slot
    // Capacity a file boundary leaves unused on one side goes to the other side.
    const int32_t roomAhead = dir > 0 ? n - 1 - p : p;
    const int32_t roomBehind = dir > 0 ? p : n - 1 - p;
    if (!loop) {
        if (behind > roomBehind) {
            ahead += behind - roomBehind;
            behind = roomBehind;
        }
        if (ahead > roomAhead) {
            behind = std::min(roomBehind, behind + (ahead - roomAhead));
            ahead = roomAhead;
        }
    }
    const double behindWeight = behind > 0 ? double(std::max(ahead, 1)) / double(behind) : 1e6;

    // Near frames: the next steps while paused, the next two seconds while playing.
    if (in.playing) {
        const int32_t nearCount = std::max<int32_t>(2, int32_t(std::ceil(2.0 * in.fps * std::max(in.speed, 0.1))));
        const int32_t nearEnd = std::min(ahead, nearCount);
        if (loopWraps) {
            const int32_t toEnd = std::min(nearEnd, loopR.last - p);
            push(w, {p + 1, p + toEnd}, n, WantClass::Near, 1);
            if (nearEnd > toEnd)
                push(w, {loopR.first, std::min(loopR.first + (nearEnd - toEnd) - 1, p - 1)}, n, WantClass::Near, 1);
        } else if (dir > 0) {
            push(w, {p + 1, p + nearEnd}, n, WantClass::Near, 1);
        } else {
            push(w, {p - nearEnd, p - 1}, n, WantClass::Near, -1);
        }
    } else {
        const int32_t k = std::min<int32_t>(8, std::max(1, winCap / 4));
        if (dir > 0) {
            push(w, {p + 1, p + k}, n, WantClass::Near, 1);
            push(w, {p - k, p - 1}, n, WantClass::Near, -1);
        } else {
            push(w, {p - k, p - 1}, n, WantClass::Near, -1);
            push(w, {p + 1, p + k}, n, WantClass::Near, 1);
        }
    }

    // The window proper.
    int32_t aheadLimit = in.aheadLimit >= 0 ? in.aheadLimit : (dir > 0 ? n - 1 : 0);
    if (loopPinned) {
        // The loop is the window: decode it all once, then playback loops without decoding.
        if (dir > 0) {
            push(w, {p + 1, loopR.last}, n, WantClass::Window, 1);
            push(w, {loopR.first, p - 1}, n, WantClass::Window, in.playing ? 1 : -1);
        } else {
            push(w, {loopR.first, p - 1}, n, WantClass::Window, -1);
            push(w, {p + 1, loopR.last}, n, WantClass::Window, 1);
        }
    } else if (loopWraps) {
        // Ahead runs to the loop end and continues at the loop start.
        const int32_t toEnd = std::min(ahead, loopR.last - p);
        push(w, {p + 1, std::min(p + toEnd, aheadLimit)}, n, WantClass::Window, 1);
        const int32_t rest = ahead - toEnd;
        if (rest > 0)
            push(w, {loopR.first, std::min(loopR.first + rest - 1, p - 1)}, n, WantClass::Window, 1);
        push(w, {std::max(loopR.first, p - behind), p - 1}, n, WantClass::Window, -1);
    } else if (dir > 0) {
        push(w, {p + 1, std::min(p + ahead, aheadLimit)}, n, WantClass::Window, 1);
        push(w, {p - behind, p - 1}, n, WantClass::Window, -1);
    } else {
        push(w, {std::max(p - ahead, aheadLimit), p - 1}, n, WantClass::Window, -1);
        push(w, {p + 1, p + behind}, n, WantClass::Window, 1);
    }

    // Islands at the next jump targets: four frames each, within the reserve.
    int32_t islandBudget = islandReserve;
    for (int32_t j : in.jumpDistances) {
        if (j <= 0)
            continue;
        for (int sign : {+1, -1}) {
            const int32_t t = p + sign * j;
            if (t < 0 || t >= n || islandBudget < 4)
                continue;
            const bool inWindow = (t > p && t <= p + ahead) || (t < p && t >= p - behind);
            if (inWindow)
                continue;
            push(w, {t, t + 3}, n, WantClass::Island, 1);
            islandBudget -= 4;
        }
    }

    CachePolicy& pol = w.policy;
    pol.playhead = p;
    pol.direction = dir;
    pol.behindWeight = behindWeight;
    pol.loop = loop ? loopR : FrameRange{0, -1};
    pol.loopWraps = loopWraps;
    pol.protect.push_back({p, p});
    if (loopPinned)
        pol.protect.push_back(loopR);
    for (const Want& want : w.wants)
        if (want.cls == WantClass::Island)
            pol.protect.push_back(want.range);
    return w;
}

} // namespace scvp
