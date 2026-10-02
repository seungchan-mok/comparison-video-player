#include "engine/DecodeLane.h"

#include "engine/Log.h"
#include "engine/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace scvp {

DecodeLane::DecodeLane(IVideoSource& source, FrameCache& cache, WantMailbox& mailbox, SessionConfig cfg, FramesFn onFrames)
    : m_source(source)
    , m_cache(cache)
    , m_mailbox(mailbox)
    , m_cfg(std::move(cfg))
    , m_onFrames(std::move(onFrames))
{
}

DecodeLane::~DecodeLane() { stop(); }

bool DecodeLane::start(std::string* error)
{
    m_session = m_source.openSession(m_cfg, error);
    if (!m_session)
        return false;
    m_thread = std::thread([this] { run(); });
    return true;
}

void DecodeLane::stop()
{
    m_stop = true;
    m_mailbox.close();
    if (m_thread.joinable())
        m_thread.join();
}

int32_t DecodeLane::firstMissing(int32_t first, int32_t last) const
{
    while (first <= last) {
        const int32_t m = m_cache.firstMissing(first, last);
        if (m < 0 || !m_unreachable.count(m))
            return m;
        first = m + 1;
    }
    return -1;
}

int32_t DecodeLane::lastMissing(int32_t first, int32_t last) const
{
    while (first <= last) {
        const int32_t m = m_cache.lastMissing(first, last);
        if (m < 0 || !m_unreachable.count(m))
            return m;
        last = m - 1;
    }
    return -1;
}

// Continuing needs (dec(frame) - nextDec) packets; seeking needs (dec(frame) - key.dec) packets
// plus refilling the decoder pipeline (threads and reordering). Continue when the decoder is
// not further from the frame than the key is, give or take that refill.
bool DecodeLane::continueBeatsSeek(const SeekPoint& key, int32_t frame, const FrameIndex& idx) const
{
    if (m_forceSeek)
        return false;
    const IDecodeSession::Position pos = m_session->position();
    if (pos.eof || pos.nextDec < 0 || frame <= pos.lastPres)
        return false;
    const int32_t d = idx.decodeFromPres(frame);
    if (d < 0 || !key.valid())
        return true;
    // Already fed without decoding it (it was cached or unneeded then): only a seek brings it back.
    if (d < pos.nextDec && m_session->wasSkipped(d))
        return false;
    const int32_t refill = (m_cfg.threads > 0 ? m_cfg.threads : 4) + idx.reorderDepth() + 2;
    return key.dec - pos.nextDec <= refill;
}

std::optional<DecodeLane::Task> DecodeLane::pick(const WantList& w, const FrameIndex& idx)
{
    const int32_t n = idx.frameCount();
    // Lowest frame the plan wants near the playhead (islands excluded).
    int32_t planLow = w.target;
    for (const Want& want : w.wants)
        if (want.cls != WantClass::Island)
            planLow = std::min(planLow, want.range.first);
    planLow = std::max(planLow, 0);
    for (const Want& want : w.wants) {
        const int32_t lo = std::max(want.range.first, 0);
        const int32_t hi = std::min(want.range.last, n - 1);
        if (lo > hi)
            continue;
        Task t;
        t.cls = want.cls;
        if (want.fill >= 0) {
            t.first = firstMissing(lo, hi);
            if (t.first < 0)
                continue;
            t.last = std::min(m_cache.missingRunEnd(t.first, hi), hi);
        } else {
            // Backward: the missing frame nearest to the playhead and everything the plan wants
            // from its key packet up to it. Reaching it means decoding from the key anyway, so
            // one pass fills the whole span, even when earlier passes left it fragmented (a
            // target reached with NONREF skipping caches only the reference frames on the way).
            t.last = lastMissing(lo, hi);
            if (t.last < 0)
                continue;
            const SeekPoint k = idx.seekPointFor(t.last);
            const int32_t spanStart = std::clamp(std::max(k.pres, planLow), 0, t.last);
            // Resume where an interrupted pass over this span stopped instead of restarting at
            // the key (every key press cancels the in-flight pass).
            t.first = firstMissing(spanStart, t.last);
            if (t.first < 0)
                t.first = t.last;
        }
        // A target reached by a forward move or jump skips the frames below it (latency first;
        // a later backward pass fills that span). Everything else decodes what the plan wants.
        t.keepFrom = (want.cls == WantClass::Target && w.direction > 0) ? t.first : planLow;
        t.key = idx.seekPointFor(t.first);
        t.seek = !continueBeatsSeek(t.key, t.first, idx);
        return t;
    }
    return std::nullopt;
}

void DecodeLane::markUnreachable(int32_t first, int32_t last)
{
    for (int32_t i = first; i <= last; ++i)
        if (!m_cache.contains(i))
            m_unreachable.insert(i);
}

void DecodeLane::exec(const Task& t, const FrameIndex& idx, uint64_t generation)
{
    (void)idx;
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "task [%d,%d] class %d %s key dec %d pres %d; decoder at dec %d, last frame %d; %zu unreachable",
                      t.first, t.last, int(t.cls), t.seek ? "seek" : "continue", t.key.dec, t.key.pres,
                      m_session->position().nextDec, m_session->position().lastPres, m_unreachable.size());
        SCVP_DEBUG("lane %s", buf);
        std::lock_guard<std::mutex> lock(m_describeMutex);
        m_description = buf;
    }
    if (t.seek) {
        m_stats.seeks.fetch_add(1, std::memory_order_relaxed);
        if (m_session->seekTo(t.key) != DecodeStatus::Ok) {
            m_stats.errors.fetch_add(1, std::memory_order_relaxed);
            markUnreachable(t.first, t.last);
            return;
        }
        m_forceSeek = false;
    }
    // Non-reference frames are skipped unless they are wanted and not cached yet.
    const std::function<bool(int32_t)> needed = [this, &t](int32_t frame) {
        return frame >= t.keepFrom && !m_cache.contains(frame);
    };
    DecodeHints hints;
    hints.frameNeeded = &needed;
    hints.cancel = &m_mailbox.generationRef();
    hints.generation = generation;
    bool gotFirst = m_cache.contains(t.first);
    for (;;) {
        FramePtr f;
        const DecodeStatus st = m_session->next(f, hints);
        if (st == DecodeStatus::Cancelled) {
            m_stats.cancellations.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (st == DecodeStatus::Eof) {
            if (!gotFirst)
                markUnreachable(t.first, t.last);
            return;
        }
        if (st == DecodeStatus::Error) {
            m_stats.errors.fetch_add(1, std::memory_order_relaxed);
            m_forceSeek = true;
            if (!gotFirst)
                markUnreachable(t.first, t.first);
            return;
        }
        m_stats.framesDecoded.fetch_add(1, std::memory_order_relaxed);
        m_frameBytes.store(f->bytes, std::memory_order_relaxed);
        const int32_t pres = f->pres;
        if (pres > t.first && !gotFirst) {
            // The decoder went past the frame without producing it. After a fresh seek that
            // means it cannot be decoded; when continuing it may simply have been skipped
            // earlier, so seek next time instead.
            if (t.seek)
                markUnreachable(t.first, std::min(pres - 1, t.last));
            else
                m_forceSeek = true;
        }
        if (pres >= t.first)
            gotFirst = true;
        if (m_cache.offer(std::move(f))) {
            m_stats.framesCached.fetch_add(1, std::memory_order_relaxed);
            if (m_onFrames)
                m_onFrames();
        }
        if (pres >= t.last)
            return;
    }
}

std::string DecodeLane::describe() const
{
    std::lock_guard<std::mutex> lock(m_describeMutex);
    return m_description;
}

void DecodeLane::run()
{
    platform::setCurrentThreadName("scvp-decode");
    uint64_t seenNudges = m_mailbox.nudges();
    while (!m_stop.load()) {
        const WantList w = m_mailbox.snapshot();
        if (m_mailbox.closed())
            break;
        FrameIndexPtr idx = m_source.index();
        if (!idx || idx->frameCount() == 0 || w.wants.empty()) {
            m_mailbox.wait(w.generation, seenNudges, std::chrono::milliseconds(100));
            continue;
        }
        if (idx != m_unreachableFor) {
            m_unreachable.clear();
            m_unreachableFor = idx;
        }
        const std::optional<Task> task = pick(w, *idx);
        if (!task) {
            m_mailbox.wait(w.generation, seenNudges, std::chrono::milliseconds(250));
            continue;
        }
        exec(*task, *idx, w.generation);
    }
}

} // namespace scvp
