#pragma once
// Minimal CUDA driver API, loaded at run time from the NVIDIA driver (libcuda.so.1 / nvcuda.dll),
// so builds need no CUDA toolkit and machines without NVIDIA hardware simply report it missing.
// Only what zero-copy decoding needs: contexts, pitched allocations, 2D copies and OpenGL interop.

#include <cstddef>
#include <cstdint>

// Types as defined by cuda.h (driver API, CUDA >= 3.2 ABI).
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
using CUcontext = struct CUctx_st*;
using CUstream = struct CUstream_st*;
using CUarray = struct CUarray_st*;
using CUgraphicsResource = struct CUgraphicsResource_st*;

#ifndef CUDA_VERSION
#define CUDA_VERSION 11000  // lets libavutil/hwcontext_cuda.h use these typedefs instead of cuda.h
#endif

namespace scvp::cuda {

enum MemoryType : unsigned { MemHost = 1, MemDevice = 2, MemArray = 3 };
constexpr unsigned kRegisterWriteDiscard = 0x02;  // CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD
constexpr unsigned kGlDeviceListAll = 1;          // CU_GL_DEVICE_LIST_ALL

struct Memcpy2D {  // CUDA_MEMCPY2D
    size_t srcXInBytes, srcY;
    unsigned srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    unsigned dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
};

struct Api {
    CUresult (*cuInit)(unsigned);
    CUresult (*cuCtxPushCurrent)(CUcontext);
    CUresult (*cuCtxPopCurrent)(CUcontext*);
    CUresult (*cuCtxGetDevice)(CUdevice*);
    CUresult (*cuMemAllocPitch)(CUdeviceptr*, size_t*, size_t, size_t, unsigned);
    CUresult (*cuMemFree)(CUdeviceptr);
    CUresult (*cuMemGetInfo)(size_t*, size_t*);
    CUresult (*cuMemcpy2D)(const Memcpy2D*);
    CUresult (*cuMemcpy2DAsync)(const Memcpy2D*, CUstream);
    CUresult (*cuStreamSynchronize)(CUstream);
    CUresult (*cuGraphicsGLRegisterImage)(CUgraphicsResource*, unsigned, unsigned, unsigned);
    CUresult (*cuGraphicsUnregisterResource)(CUgraphicsResource);
    CUresult (*cuGraphicsMapResources)(unsigned, CUgraphicsResource*, CUstream);
    CUresult (*cuGraphicsUnmapResources)(unsigned, CUgraphicsResource*, CUstream);
    CUresult (*cuGraphicsSubResourceGetMappedArray)(CUarray*, CUgraphicsResource, unsigned, unsigned);
    CUresult (*cuGLGetDevices)(unsigned*, CUdevice*, unsigned, unsigned);
    CUresult (*cuGetErrorName)(CUresult, const char**);
};

// The loaded API, or nullptr when no NVIDIA driver is present (loaded once, thread-safe).
const Api* api();
const char* errorName(CUresult r);

// Makes a context current for the scope of the object.
class ScopedContext {
public:
    explicit ScopedContext(CUcontext ctx);
    ~ScopedContext();
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_ok = false;
};

} // namespace scvp::cuda
