#pragma once
// Minimal thread-safe logger. Level comes from SCVP_LOG (debug|info|warn|error|off), default warn.

namespace scvp::log {

enum class Level { Debug = 0, Info, Warn, Error, Off };

void setLevel(Level level);
Level level();
bool enabled(Level level);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void write(Level level, const char* fmt, ...);

} // namespace scvp::log

#define SCVP_LOG(lvl, ...)                                                       \
    do {                                                                         \
        if (::scvp::log::enabled(::scvp::log::Level::lvl))                       \
            ::scvp::log::write(::scvp::log::Level::lvl, __VA_ARGS__);            \
    } while (0)
#define SCVP_DEBUG(...) SCVP_LOG(Debug, __VA_ARGS__)
#define SCVP_INFO(...) SCVP_LOG(Info, __VA_ARGS__)
#define SCVP_WARN(...) SCVP_LOG(Warn, __VA_ARGS__)
#define SCVP_ERROR(...) SCVP_LOG(Error, __VA_ARGS__)
