#include "engine/CudaApi.h"

#include "engine/Log.h"

#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace scvp::cuda {
namespace {

void* openDriver()
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA("nvcuda.dll"));
#elif defined(__APPLE__)
    return nullptr;  // no CUDA on macOS
#else
    return dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* lib, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

Api g_api;
const Api* g_loaded = nullptr;
std::once_flag g_once;

template <typename F>
bool load(void* lib, F& fn, const char* name, const char* fallback = nullptr)
{
    void* p = symbol(lib, name);
    if (!p && fallback)
        p = symbol(lib, fallback);
    fn = reinterpret_cast<F>(p);
    if (!p)
        SCVP_INFO("CUDA: %s missing in the driver", name);
    return p != nullptr;
}

} // namespace

const Api* api()
{
    std::call_once(g_once, [] {
        void* lib = openDriver();
        if (!lib)
            return;
        bool ok = true;
        ok &= load(lib, g_api.cuInit, "cuInit");
        ok &= load(lib, g_api.cuCtxPushCurrent, "cuCtxPushCurrent_v2");
        ok &= load(lib, g_api.cuCtxPopCurrent, "cuCtxPopCurrent_v2");
        ok &= load(lib, g_api.cuCtxGetDevice, "cuCtxGetDevice");
        ok &= load(lib, g_api.cuMemAllocPitch, "cuMemAllocPitch_v2");
        ok &= load(lib, g_api.cuMemFree, "cuMemFree_v2");
        ok &= load(lib, g_api.cuMemGetInfo, "cuMemGetInfo_v2");
        ok &= load(lib, g_api.cuMemcpy2D, "cuMemcpy2D_v2");
        ok &= load(lib, g_api.cuMemcpy2DAsync, "cuMemcpy2DAsync_v2");
        ok &= load(lib, g_api.cuStreamSynchronize, "cuStreamSynchronize");
        ok &= load(lib, g_api.cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage");
        ok &= load(lib, g_api.cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource");
        ok &= load(lib, g_api.cuGraphicsMapResources, "cuGraphicsMapResources");
        ok &= load(lib, g_api.cuGraphicsUnmapResources, "cuGraphicsUnmapResources");
        ok &= load(lib, g_api.cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray");
        ok &= load(lib, g_api.cuGLGetDevices, "cuGLGetDevices_v2", "cuGLGetDevices");
        ok &= load(lib, g_api.cuGetErrorName, "cuGetErrorName");
        if (ok && g_api.cuInit(0) == 0)
            g_loaded = &g_api;
    });
    return g_loaded;
}

const char* errorName(CUresult r)
{
    const char* n = nullptr;
    if (g_loaded && g_loaded->cuGetErrorName(r, &n) == 0 && n)
        return n;
    return "CUDA error";
}

ScopedContext::ScopedContext(CUcontext ctx)
{
    const Api* a = api();
    m_ok = a && ctx && a->cuCtxPushCurrent(ctx) == 0;
}

ScopedContext::~ScopedContext()
{
    if (m_ok) {
        CUcontext dummy = nullptr;
        api()->cuCtxPopCurrent(&dummy);
    }
}

} // namespace scvp::cuda
