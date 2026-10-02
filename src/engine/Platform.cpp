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
