#pragma once

#include "engine/FfmpegCompat.h"

#include <mutex>
#include <string>
#include <vector>

namespace scvp {

enum class HwPolicy {
    GpuFirst,  // hardware whenever the device supports the stream and decoding is bit-exact (default)
    Off,       // software decoding only
    Device,    // the given device type, for any codec it supports
};

struct HwConfig {
    HwPolicy policy = HwPolicy::GpuFirst;
    AVHWDeviceType device = AV_HWDEVICE_TYPE_NONE;  // for HwPolicy::Device
    bool simulateFailure = false;                    // tests: hardware init "fails", software must take over
};

// Hardware device contexts shared by every decoder in the process: creating one is slow (CUDA
// ~160 ms), so each type is created once, optionally ahead of time on a background thread.
class HwDevices {
public:
    static HwDevices& instance();

    // A new reference to the device of `type`, created on first use; nullptr if unavailable.
    AVBufferRef* get(AVHWDeviceType type);
    bool available(AVHWDeviceType type);
    void warmUp();  // creates the platform's preferred devices in the background

    static std::vector<AVHWDeviceType> platformOrder();
    static const char* name(AVHWDeviceType type);
    // Codecs whose decoding is bit-exact by specification (integer transforms), so hardware and
    // software decoders produce identical pixels. GPU-first only uses hardware for these;
    // MPEG-2/4 Part 2, MJPEG and other IDCT-based codecs may differ slightly between decoders.
    static bool bitExactCodec(AVCodecID id);

private:
    struct Entry {
        AVHWDeviceType type;
        AVBufferRef* ref = nullptr;
        bool tried = false;
    };
    std::mutex m_mutex;
    std::vector<Entry> m_entries;
};

} // namespace scvp
