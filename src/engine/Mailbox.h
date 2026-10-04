#pragma once

#include "engine/PrefetchPlanner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace scvp {

// Latest-wins hand-off of WantLists from the engine to a decode lane. Posting bumps the
// generation, which in-flight decoding polls through DecodeHints to stop early. nudge() wakes a
// waiting lane without invalidating its work (index grew, budget changed, ...).
class WantMailbox {
public:
    uint64_t post(WantList w)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        w.generation = m_generation.load(std::memory_order_relaxed) + 1;
        m_latest = std::move(w);
        m_generation.store(m_latest.generation, std::memory_order_release);
        m_cv.notify_all();
        return m_latest.generation;
    }

    WantList snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_latest;
    }

    uint64_t generation() const { return m_generation.load(std::memory_order_acquire); }
    const std::atomic<uint64_t>& generationRef() const { return m_generation; }

    void nudge()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_nudges;
        m_cv.notify_all();
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_generation.fetch_add(1, std::memory_order_release);  // cancels in-flight decoding
        m_cv.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    // Waits until a newer generation is posted, a nudge arrives, the mailbox closes or timeout.
    void wait(uint64_t seenGeneration, uint64_t& seenNudges, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, timeout, [&] {
            return m_closed || m_generation.load(std::memory_order_relaxed) != seenGeneration || m_nudges != seenNudges;
        });
        seenNudges = m_nudges;
    }

    uint64_t nudges() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_nudges;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    WantList m_latest;
    std::atomic<uint64_t> m_generation{0};
    uint64_t m_nudges = 0;
    bool m_closed = false;
};

} // namespace scvp
