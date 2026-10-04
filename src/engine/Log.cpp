#include "engine/Log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace scvp::log {
namespace {

Level levelFromEnv()
{
    const char* v = std::getenv("SCVP_LOG");
    if (!v)
        return Level::Warn;
    if (!std::strcmp(v, "debug"))
        return Level::Debug;
    if (!std::strcmp(v, "info"))
        return Level::Info;
    if (!std::strcmp(v, "error"))
        return Level::Error;
    if (!std::strcmp(v, "off"))
        return Level::Off;
    return Level::Warn;
}

std::atomic<int> g_level{static_cast<int>(levelFromEnv())};
std::mutex g_mutex;
const auto g_start = std::chrono::steady_clock::now();

} // namespace

void setLevel(Level level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }
Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }
bool enabled(Level l) { return static_cast<int>(l) >= g_level.load(std::memory_order_relaxed) && l != Level::Off; }

void write(Level l, const char* fmt, ...)
{
    if (!enabled(l))
        return;
    static const char* const names[] = {"D", "I", "W", "E", "-"};
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%9.3f %s %s\n", t, names[static_cast<int>(l)], msg);
}

} // namespace scvp::log
