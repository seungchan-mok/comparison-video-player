#include "engine/Demux.h"

#include "engine/Log.h"

#include <cstring>

namespace scvp {

std::optional<Demuxer> openDemuxer(const std::string& path, const InputOptions& opts, std::string* error)
{
    const AVInputFormat* ifmt = nullptr;
    if (!opts.formatName.empty()) {
        ifmt = av_find_input_format(opts.formatName.c_str());
        if (!ifmt) {
            if (error)
                *error = "unknown input format '" + opts.formatName + "'";
            return std::nullopt;
        }
    }
    AVDictionary* dict = nullptr;
    for (const auto& [k, v] : opts.options)
        av_dict_set(&dict, k.c_str(), v.c_str(), 0);

    AVFormatContext* raw = nullptr;
    const int err = avformat_open_input(&raw, path.c_str(), ifmt, &dict);
    av_dict_free(&dict);
    if (err < 0) {
        if (error)
            *error = ff::errorString(err);
        return std::nullopt;
    }
    Demuxer d;
    d.fmt.reset(raw);

    const int infoErr = avformat_find_stream_info(raw, nullptr);
    if (infoErr < 0)
        SCVP_WARN("avformat_find_stream_info(%s): %s", path.c_str(), ff::errorString(infoErr).c_str());

    int best = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (best >= 0 && (raw->streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        best = -1;
        for (unsigned i = 0; i < raw->nb_streams; ++i) {
            const AVStream* st = raw->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                best = int(i);
                break;
            }
        }
    }
    if (best < 0) {
        if (error)
            *error = "no video stream";
        return std::nullopt;
    }
    d.stream = best;
    for (unsigned i = 0; i < raw->nb_streams; ++i)
        raw->streams[i]->discard = int(i) == best ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    return d;
}

const AVCodec* chooseDecoder(AVCodecID id, bool hardware)
{
    if (id == AV_CODEC_ID_AV1 && !hardware) {
        if (const AVCodec* dav1d = avcodec_find_decoder_by_name("libdav1d"))
            return dav1d;
    }
    if (id == AV_CODEC_ID_AV1 && hardware) {
        if (const AVCodec* native = avcodec_find_decoder_by_name("av1"))
            return native;
    }
    return avcodec_find_decoder(id);
}

bool prefersByteSeek(const AVFormatContext* fmt)
{
    if (!fmt || !fmt->iformat)
        return false;
    if (fmt->iformat->flags & AVFMT_NO_BYTE_SEEK)
        return false;
    static const char* const byteSeekFormats[] = {"mpegts", "mpeg", "h264", "hevc", "mpegvideo", "m4v", "vc1", "avs2", "cavsvideo"};
    for (const char* name : byteSeekFormats) {
        // iformat->name can be a comma-separated list ("mov,mp4,m4a,3gp,3g2,mj2").
        const char* n = fmt->iformat->name;
        const size_t len = std::strlen(name);
        for (const char* p = n; (p = std::strstr(p, name)) != nullptr; p += len) {
            const bool startOk = p == n || p[-1] == ',';
            const bool endOk = p[len] == '\0' || p[len] == ',';
            if (startOk && endOk)
                return true;
        }
    }
    return false;
}

} // namespace scvp
