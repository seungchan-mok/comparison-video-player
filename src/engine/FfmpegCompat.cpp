#include "engine/FfmpegCompat.h"

#include "engine/Log.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace scvp::ff {

std::string errorString(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

double streamRotation(const AVStream* st)
{
    if (!st || !st->codecpar)
        return 0.0;
    const AVPacketSideData* sd = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * sizeof(int32_t))
        return 0.0;
    const double r = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
    return std::isnan(r) ? 0.0 : r;
}

size_t frameBufferBytes(const AVFrame* f)
{
    size_t total = 0;
    if (!f)
        return 0;
    for (const AVBufferRef* b : f->buf)
        if (b)
            total += b->size;
    return total;
}

namespace {

int g_ffLogLevel = AV_LOG_ERROR;

void logCallback(void* avcl, int level, const char* fmt, va_list vl)
{
    if (level > g_ffLogLevel)
        return;
    char line[1024];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    const log::Level ours = level <= AV_LOG_ERROR ? log::Level::Error
                            : level <= AV_LOG_WARNING ? log::Level::Warn
                            : level <= AV_LOG_INFO    ? log::Level::Info
                                                      : log::Level::Debug;
    // av_log_format_line2 keeps the trailing newline; the logger adds its own.
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n)
        log::write(ours, "[ffmpeg] %s", line);
}

} // namespace

void installLogCallback(int level)
{
    g_ffLogLevel = level;
    av_log_set_level(level);
    av_log_set_callback(&logCallback);
}

} // namespace scvp::ff
