#include "engine/Platform.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <cstring>

namespace scvp::platform {

std::FILE* fopenUtf8(const std::string& path, const char* mode)
{
#if defined(_WIN32)
    auto widen = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void lowerCurrentThreadPriority(int niceDelta)
{
#if defined(_WIN32)
    (void)niceDelta;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    (void)niceDelta;
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    // On Linux the nice value is per thread (task).
    const pid_t tid = pid_t(syscall(SYS_gettid));
    const int current = getpriority(PRIO_PROCESS, id_t(tid));
    setpriority(PRIO_PROCESS, id_t(tid), current + niceDelta);
#endif
}

void setCurrentThreadName(const char* name)
{
#if defined(_WIN32)
    (void)name;
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    char buf[16];
    std::strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pthread_setname_np(pthread_self(), buf);
#endif
}

} // namespace scvp::platform
