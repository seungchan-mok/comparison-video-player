#pragma once

#include <cstdio>
#include <string>

namespace scvp::platform {

// fopen with a UTF-8 path on every OS (Windows needs the wide-character API).
std::FILE* fopenUtf8(const std::string& path, const char* mode);

// Lowers the scheduling priority of the calling thread (background work such as index scans).
// Threads created by FFmpeg after this call inherit it on Linux.
void lowerCurrentThreadPriority(int niceDelta = 10);

// Names the calling thread for debuggers and profilers (truncated to 15 chars on Linux).
void setCurrentThreadName(const char* name);

} // namespace scvp::platform
