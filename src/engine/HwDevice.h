#pragma once

#include "engine/FfmpegCompat.h"

#include <map>
#include <mutex>
#include <string>
#include <tuple>
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
    bool simulateMismatch = false;                   // tests: the hardware check finds different pixels
};

// Whether a device decodes one kind of stream exactly like software, as found by the first
// GPU-first session that checked it (see FFmpegDecodeSession::verifyHardware).
enum class HwVerdict { Unknown, Exact, Differs };

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
    // A device can still fail the specification, hence the verdicts below.
    static bool bitExactCodec(AVCodecID id);

    // Per (device, codec, stream pixel format), for the life of the process. GPU-first skips a
    // device whose verdict is Differs and checks one whose verdict is Unknown.
    HwVerdict verdict(AVHWDeviceType type, AVCodecID codec, int pixelFormat);
    void setVerdict(AVHWDeviceType type, AVCodecID codec, int pixelFormat, HwVerdict v);
    void forgetVerdicts();  // tests

private:
    struct Entry {
        AVHWDeviceType type;
        AVBufferRef* ref = nullptr;
        bool tried = false;
    };
    std::mutex m_mutex;
    std::vector<Entry> m_entries;
    std::map<std::tuple<int, int, int>, HwVerdict> m_verdicts;
};

} // namespace scvp
