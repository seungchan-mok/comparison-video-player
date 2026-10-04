#include "engine/HwDevice.h"

#include "engine/Log.h"
#include "engine/Platform.h"

#include <thread>

namespace scvp {

HwDevices& HwDevices::instance()
{
    static HwDevices d;
    return d;
}

std::vector<AVHWDeviceType> HwDevices::platformOrder()
{
#if defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_DXVA2};
#elif defined(__APPLE__)
    return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#else
    return {AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VAAPI};
#endif
}

bool HwDevices::bitExactCodec(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1:
        return true;
    default:
        return false;
    }
}

const char* HwDevices::name(AVHWDeviceType type)
{
    const char* n = av_hwdevice_get_type_name(type);
    return n ? n : "none";
}

AVBufferRef* HwDevices::get(AVHWDeviceType type)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries)
        if (e.type == type)
            return e.ref ? av_buffer_ref(e.ref) : nullptr;
    Entry e;
    e.type = type;
    e.tried = true;
    const int err = av_hwdevice_ctx_create(&e.ref, type, nullptr, nullptr, 0);
    if (err < 0) {
        SCVP_INFO("hardware device %s unavailable: %s", name(type), ff::errorString(err).c_str());
        e.ref = nullptr;
    } else {
        SCVP_INFO("hardware device %s ready", name(type));
    }
    m_entries.push_back(e);
    return e.ref ? av_buffer_ref(e.ref) : nullptr;
}

HwVerdict HwDevices::verdict(AVHWDeviceType type, AVCodecID codec, int pixelFormat)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_verdicts.find({int(type), int(codec), pixelFormat});
    return it != m_verdicts.end() ? it->second : HwVerdict::Unknown;
}

void HwDevices::setVerdict(AVHWDeviceType type, AVCodecID codec, int pixelFormat, HwVerdict v)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_verdicts[{int(type), int(codec), pixelFormat}] = v;
}

void HwDevices::forgetVerdicts()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_verdicts.clear();
}

bool HwDevices::available(AVHWDeviceType type)
{
    AVBufferRef* r = get(type);
    const bool ok = r != nullptr;
    av_buffer_unref(&r);
    return ok;
}

void HwDevices::warmUp()
{
    std::thread([] {
        platform::setCurrentThreadName("scvp-hwinit");
        for (AVHWDeviceType t : platformOrder())
            if (instance().available(t))
                break;  // the first one that works is what decoders will use
    }).detach();
}

} // namespace scvp
