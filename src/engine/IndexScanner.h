#pragma once

#include "engine/Demux.h"
#include "engine/FrameIndex.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace scvp {

// Builds a FrameIndex for a file by demuxing every video packet (no decoding, except the first
// frame of a short probe decode that tells where output starts). Runs on its own low-priority
// thread with its own demuxer and publishes snapshots as it goes: an early one after a few
// packets so playback can start immediately, then periodic ones, then the complete index.
class IndexScanner {
public:
    using SnapshotFn = std::function<void(FrameIndexPtr)>;

    IndexScanner(std::string path, InputOptions opts, SnapshotFn onSnapshot);
    ~IndexScanner();
    IndexScanner(const IndexScanner&) = delete;
    IndexScanner& operator=(const IndexScanner&) = delete;

    void start();
    void stop();  // cancels and joins

    bool finished() const { return m_finished.load(); }
    bool failed() const { return m_failed.load(); }
    double progress() const { return m_progress.load(); }  // 0..1 by bytes read
    std::string error() const;

    // Scans synchronously in the calling thread (tests, tools). `onSnapshot` may be empty.
    static FrameIndexPtr scan(const std::string& path, const InputOptions& opts, std::string* error,
                              const SnapshotFn& onSnapshot = {}, const std::atomic<bool>* cancel = nullptr,
                              std::atomic<double>* progress = nullptr);

private:
    void run();

    std::string m_path;
    InputOptions m_opts;
    SnapshotFn m_onSnapshot;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_failed{false};
    std::atomic<double> m_progress{0.0};
    mutable std::mutex m_errorMutex;
    std::string m_error;
};

} // namespace scvp
