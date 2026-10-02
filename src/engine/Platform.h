#pragma once

namespace scvp::platform {

// Lowers the scheduling priority of the calling thread (background work such as index scans).
// Threads created by FFmpeg after this call inherit it on Linux.
void lowerCurrentThreadPriority(int niceDelta = 10);

// Names the calling thread for debuggers and profilers (truncated to 15 chars on Linux).
void setCurrentThreadName(const char* name);

} // namespace scvp::platform
