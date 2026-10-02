// scvp-bench: latency and throughput of the decode engine on a real file.
//
//   scvp-bench FILE [--scenario all|open|seq|realtime|random|backstep|cached]
//              [--budget-mb N] [--jumps N] [--steps N] [--think-ms N] [--seconds N] [--json]
//
// open      time to the first frame and to the complete index
// seq       decode throughput when the playhead follows the decoder as fast as possible
// realtime  playback paced at the stream frame rate: stalls and their total length
// random    jumps to frames outside the cache: latency until the target is shown
// backstep  stepping backwards with a think time between key presses (key repeat ~33 ms)
// cached    lookup cost of frames already in the cache
// lockstep  with --with B: both videos through the PlaybackController (pairs only when both
//           frames are decoded): pair throughput, then real-time playback stalls

#include "engine/Engine.h"
#include "engine/PlaybackController.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace scvp;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Waiter {
    Engine& engine;
    std::mutex m;
    std::condition_variable cv;

    explicit Waiter(Engine& e)
        : engine(e)
    {
        engine.setFramesCallback([this](int) {
            std::lock_guard<std::mutex> lock(m);
            cv.notify_all();
        });
    }
    ~Waiter() { engine.setFramesCallback({}); }

    FramePtr wait(int slot, int32_t n, double timeoutMs = 20000)
    {
        const auto deadline = Clock::now() + std::chrono::microseconds(int64_t(timeoutMs * 1000));
        std::unique_lock<std::mutex> lock(m);
        for (;;) {
            if (FramePtr f = engine.frame(slot, n))
                return f;
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
                return engine.frame(slot, n);
        }
    }
};

struct Stats {
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
    size_t n = 0;
};

Stats summarize(std::vector<double> v)
{
    Stats s;
    s.n = v.size();
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]; };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = v.back();
    double sum = 0;
    for (double x : v)
        sum += x;
    s.mean = sum / double(v.size());
    return s;
}

PlaybackIntent paused(int32_t p, int dir = 1)
{
    PlaybackIntent in;
    in.playhead = p;
    in.direction = dir;
    return in;
}

struct Options {
    std::string file;
    std::string with;  // second video (lockstep)
    std::string scenario = "all";
    size_t budgetMb = 0;
    int jumps = 30;
    int steps = 300;
    double thinkMs = 33;
    double seconds = 10;
    int requestEvery = 1;
    std::string hw = "gpu";
    long long vramMb = -1;  // -1 = automatic
    bool json = false;
};

bool want(const Options& o, const char* s) { return o.scenario == "all" || o.scenario == s; }

std::vector<std::string> g_json;
void report(const Options& o, const std::string& key, const std::string& human, const std::string& json)
{
    if (o.json)
        g_json.push_back("\"" + key + "\": " + json);
    else
        std::printf("%-10s %s\n", key.c_str(), human.c_str());
}

std::string statsJson(const Stats& s)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "{\"n\": %zu, \"p50_ms\": %.3f, \"p95_ms\": %.3f, \"p99_ms\": %.3f, \"max_ms\": %.3f, \"mean_ms\": %.3f}",
                  s.n, s.p50, s.p95, s.p99, s.max, s.mean);
    return buf;
}

std::string statsHuman(const Stats& s)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "n=%zu p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms", s.n, s.p50, s.p95, s.p99, s.max);
    return buf;
}

int usage()
{
    std::fprintf(stderr,
                 "usage: scvp-bench FILE [--scenario all|open|seq|realtime|random|backstep|cached]\n"
                 "                  [--budget-mb N] [--jumps N] [--steps N] [--think-ms N] [--seconds N]\n"
                 "                  [--request-every N] [--hw gpu|cpu|cuda|vaapi|...] [--vram-mb N] [--with B] [--json]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--scenario") {
            if (const char* v = next())
                o.scenario = v;
        } else if (a == "--budget-mb") {
            if (const char* v = next())
                o.budgetMb = size_t(std::atoll(v));
        } else if (a == "--jumps") {
            if (const char* v = next())
                o.jumps = std::atoi(v);
        } else if (a == "--steps") {
            if (const char* v = next())
                o.steps = std::atoi(v);
        } else if (a == "--think-ms") {
            if (const char* v = next())
                o.thinkMs = std::atof(v);
        } else if (a == "--seconds") {
            if (const char* v = next())
                o.seconds = std::atof(v);
        } else if (a == "--request-every") {
            if (const char* v = next())
                o.requestEvery = std::max(1, std::atoi(v));
        } else if (a == "--vram-mb") {
            if (const char* v = next())
                o.vramMb = std::atoll(v);
        } else if (a == "--hw") {
            if (const char* v = next())
                o.hw = v;
        } else if (a == "--with") {
            if (const char* v = next())
                o.with = v;
        } else if (a == "--json") {
            o.json = true;
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else {
            o.file = a;
        }
    }
    if (o.file.empty())
        return usage();
    ff::installLogCallback(AV_LOG_ERROR);

    Engine engine;
    HwConfig hw;
    if (o.hw == "cpu" || o.hw == "off") {
        hw.policy = HwPolicy::Off;
    } else if (o.hw != "gpu") {
        hw.policy = HwPolicy::Device;
        hw.device = av_hwdevice_find_type_by_name(o.hw.c_str());
    }
    engine.setHardware(hw);
    if (o.vramMb >= 0)
        engine.setGpuMemoryBudget(size_t(o.vramMb) << 20);
    if (o.budgetMb)
        engine.setMemoryBudget(o.budgetMb << 20);
    Waiter waiter(engine);

    // --- open ------------------------------------------------------------------------------
    const auto t0 = Clock::now();
    std::string err;
    if (!engine.open(0, o.file, InputOptions{}, &err)) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    engine.request(0, paused(0));
    FramePtr first = waiter.wait(0, 0);
    const double firstFrameMs = msSince(t0);
    while (!engine.stats(0).indexComplete && msSince(t0) < 120000)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const double indexMs = msSince(t0);
    FrameIndexPtr idx = engine.index(0);
    const int32_t n = idx ? idx->frameCount() : 0;
    const double fps = idx ? idx->frameRate() : 0;
    if (!first || n <= 0) {
        std::fprintf(stderr, "no frames decoded\n");
        return 1;
    }
    const VideoInfo info = engine.info(0);
    if (!o.json)
        std::printf("%s: %dx%d %s %s, %d frames @ %.3f fps, frame %zu bytes, cache capacity %d frames (%.0f MB budget), decoder %s\n",
                    o.file.c_str(), info.width, info.height, info.codecName.c_str(),
                    av_get_pix_fmt_name(first->format()), n, fps, first->bytes, engine.capacityFrames(0),
                    double(engine.memoryBudget()) / (1 << 20), first->hardware ? "hardware" : "software");
    if (want(o, "open")) {
        char h[128], j[128];
        std::snprintf(h, sizeof(h), "first frame %.1f ms, complete index %.1f ms", firstFrameMs, indexMs);
        std::snprintf(j, sizeof(j), "{\"first_frame_ms\": %.3f, \"index_ms\": %.3f}", firstFrameMs, indexMs);
        report(o, "open", h, j);
    }

    std::mt19937 rng(12345);

    // --- seq: throughput -----------------------------------------------------------------
    if (want(o, "seq")) {
        const int32_t count = std::min<int32_t>(n, 1200);
        const SlotStats before = engine.stats(0);
        PlaybackIntent in;
        in.playing = true;
        const auto ts = Clock::now();
        int32_t got = 0;
        for (int32_t p = 0; p < count; ++p) {
            if (p % o.requestEvery == 0) {
                in.playhead = p;
                engine.request(0, in);
            }
            if (!waiter.wait(0, p))
                break;
            ++got;
        }
        const double ms = msSince(ts);
        const SlotStats after = engine.stats(0);
        char h[160], j[160];
        std::snprintf(h, sizeof(h), "%d frames in %.1f ms = %.1f fps (%llu seeks)", got, ms, got * 1000.0 / ms,
                      (unsigned long long)(after.seeks - before.seeks));
        std::snprintf(j, sizeof(j), "{\"frames\": %d, \"ms\": %.3f, \"fps\": %.3f}", got, ms, got * 1000.0 / ms);
        report(o, "seq", h, j);
    }

    // --- lockstep: two videos through the playback controller -------------------------------
    if (!o.with.empty() && (o.scenario == "all" || o.scenario == "lockstep")) {
        if (!engine.open(1, o.with, InputOptions{}, &err)) {
            std::fprintf(stderr, "open B failed: %s\n", err.c_str());
            return 1;
        }
        while (!engine.stats(1).indexComplete)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EngineBackend backend(engine);
        PlaybackController c(backend);
        c.mediaChanged();
        const int64_t count = std::min<int64_t>(c.length(), 1200);
        // Pair throughput: the clock jumps ahead, so only decoding limits the rate.
        c.seek(0);
        c.play();
        double now = 0;
        const auto ts = Clock::now();
        c.tick(now);
        while (c.position() < count - 1 && msSince(ts) < 120000) {
            const int64_t next = c.position() + 1;
            waiter.wait(0, int32_t(next), 2000);
            waiter.wait(1, int32_t(next), 2000);
            c.tick(now += 1.0);
        }
        const double ms = msSince(ts);
        char h[200], j[200];
        std::snprintf(h, sizeof(h), "%lld pairs in %.1f ms = %.1f pairs/s", (long long)c.position(), ms, double(c.position()) * 1000.0 / ms);
        std::snprintf(j, sizeof(j), "{\"pairs\": %lld, \"ms\": %.3f, \"pairs_per_s\": %.3f}", (long long)c.position(), ms,
                      double(c.position()) * 1000.0 / ms);
        report(o, "lockstep", h, j);
        if (!o.json)
            for (int sl = 0; sl < 2; ++sl) {
                const SlotStats st = engine.stats(sl);
                std::printf("           %c: %llu decoded, %llu seeks, %llu cancelled, %zu cached, capacity %d\n", sl ? 'B' : 'A',
                            (unsigned long long)st.framesDecoded, (unsigned long long)st.seeks, (unsigned long long)st.cancellations,
                            st.cachedFrames, st.capacityFrames);
            }

        // Real time from a cold cache: reopen both, then tick every millisecond on the wall clock.
        c.pause();
        engine.close(0);
        engine.close(1);
        if (!engine.open(0, o.file, InputOptions{}, &err) || !engine.open(1, o.with, InputOptions{}, &err)) {
            std::fprintf(stderr, "reopen failed: %s\n", err.c_str());
            return 1;
        }
        while (!engine.stats(0).indexComplete || !engine.stats(1).indexComplete)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        c.mediaChanged();
        c.seek(0);
        waiter.wait(0, 0);
        waiter.wait(1, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        c.play();
        const auto t1 = Clock::now();
        int stalls = 0;
        double stalledMs = 0;
        bool wasStalled = false;
        const int64_t frames = std::min<int64_t>(c.length(), int64_t(o.seconds * c.frameRate()));
        while (c.playing() && c.position() < frames - 1) {
            const TickResult r = c.tick(msSince(t1) / 1000.0);
            if (r.stalled && !wasStalled)
                ++stalls;
            if (r.stalled)
                stalledMs += 1.0;
            wasStalled = r.stalled;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::snprintf(h, sizeof(h), "%lld pairs at %.2f fps in %.1f s: %d stalls, ~%.0f ms stalled", (long long)frames, c.frameRate(),
                      msSince(t1) / 1000.0, stalls, stalledMs);
        std::snprintf(j, sizeof(j), "{\"pairs\": %lld, \"stalls\": %d, \"stall_ms\": %.1f}", (long long)frames, stalls, stalledMs);
        report(o, "lockstep-rt", h, j);
        c.pause();
        engine.close(1);
    }

    // --- realtime: paced playback --------------------------------------------------------
    if (want(o, "realtime")) {
        engine.request(0, paused(0));
        waiter.wait(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the window fill a little
        const int32_t count = std::min<int32_t>(n, int32_t(o.seconds * fps));
        PlaybackIntent in;
        in.playing = true;
        auto anchor = Clock::now();
        int32_t anchorFrame = 0;
        int stalls = 0;
        double stallMs = 0;
        for (int32_t p = 0; p < count; ++p) {
            in.playhead = p;
            engine.request(0, in);
            const auto due = anchor + std::chrono::microseconds(int64_t((p - anchorFrame) * 1e6 / fps));
            std::this_thread::sleep_until(due);
            if (!engine.frame(0, p)) {
                const auto ts = Clock::now();
                waiter.wait(0, p);
                ++stalls;
                stallMs += msSince(ts);
                anchor = Clock::now();  // re-anchor: no catch-up burst
                anchorFrame = p;
            }
        }
        char h[160], j[160];
        std::snprintf(h, sizeof(h), "%d frames at %.2f fps: %d stalls, %.1f ms stalled", count, fps, stalls, stallMs);
        std::snprintf(j, sizeof(j), "{\"frames\": %d, \"stalls\": %d, \"stall_ms\": %.3f}", count, stalls, stallMs);
        report(o, "realtime", h, j);
    }

    // --- random jumps outside the cache ---------------------------------------------------
    if (want(o, "random")) {
        std::vector<double> lat;
        const int32_t cap = engine.capacityFrames(0);
        int32_t pos = n / 2;
        for (int i = 0; i < o.jumps; ++i) {
            int32_t t = pos;
            for (int tries = 0; tries < 50; ++tries) {
                t = int32_t(rng() % uint32_t(n));
                if (std::abs(t - pos) > cap && !engine.frame(0, t))
                    break;
            }
            const auto ts = Clock::now();
            engine.request(0, paused(t));
            if (!waiter.wait(0, t))
                continue;
            lat.push_back(msSince(ts));
            pos = t;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let prefetch run like a user would
        }
        const Stats s = summarize(lat);
        report(o, "random", statsHuman(s), statsJson(s));
    }

    // --- backstep with think time -----------------------------------------------------------
    if (want(o, "backstep")) {
        const int32_t start = n - 1;
        engine.request(0, paused(start, -1));
        waiter.wait(0, start);
        std::this_thread::sleep_for(std::chrono::milliseconds(int(o.thinkMs)));
        const SlotStats before = engine.stats(0);
        std::vector<double> lat;
        int misses = 0;
        const int32_t steps = std::min<int32_t>(o.steps, start);
        for (int32_t k = 1; k <= steps; ++k) {
            const int32_t p = start - k;
            const bool cached = engine.frame(0, p) != nullptr;
            const auto ts = Clock::now();
            engine.request(0, paused(p, -1));
            waiter.wait(0, p);
            lat.push_back(cached ? 0.0 : msSince(ts));
            misses += cached ? 0 : 1;
            std::this_thread::sleep_for(std::chrono::microseconds(int64_t(o.thinkMs * 1000)));
        }
        const SlotStats after = engine.stats(0);
        const Stats s = summarize(lat);
        char h[256], j[320];
        std::snprintf(h, sizeof(h), "%s, %d/%d steps not cached, %.2f decoded frames/step (think %.0f ms)",
                      statsHuman(s).c_str(), misses, steps, double(after.framesDecoded - before.framesDecoded) / steps, o.thinkMs);
        std::snprintf(j, sizeof(j), "{\"latency\": %s, \"misses\": %d, \"steps\": %d, \"decoded_per_step\": %.3f}",
                      statsJson(s).c_str(), misses, steps, double(after.framesDecoded - before.framesDecoded) / steps);
        report(o, "backstep", h, j);
    }

    // --- cached lookups ---------------------------------------------------------------------
    if (want(o, "cached")) {
        const int32_t p = n / 3;
        engine.request(0, paused(p));
        waiter.wait(0, p);
        std::vector<double> lat;
        for (int i = 0; i < 2000; ++i) {
            const auto ts = Clock::now();
            FramePtr f = engine.frame(0, p);
            lat.push_back(msSince(ts));
            if (!f)
                break;
        }
        const Stats s = summarize(lat);
        report(o, "cached", statsHuman(s), statsJson(s));
    }

    if (o.json) {
        std::printf("{\n");
        for (size_t i = 0; i < g_json.size(); ++i)
            std::printf("  %s%s\n", g_json[i].c_str(), i + 1 < g_json.size() ? "," : "");
        std::printf("}\n");
    }
    return 0;
}
