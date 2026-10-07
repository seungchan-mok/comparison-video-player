#include "engine/VideoEncoder.h"

#include "engine/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace scvp {

namespace {

struct Candidate {
    const char* name;
    const char* label;
};

// Preferred first. Hardware encoders lead (they leave the CPU to decoding and are several times
// faster at 4K); the software ones after them are the reference for quality.
const std::vector<Candidate>& candidates(OutputFormat f)
{
    static const std::vector<Candidate> h264{
        {"h264_nvenc", "NVIDIA NVENC"}, {"h264_videotoolbox", "VideoToolbox"}, {"h264_qsv", "Intel Quick Sync"},
        {"h264_amf", "AMD AMF"},        {"h264_mf", "Media Foundation"},     {"libx264", "x264"},
        {"libopenh264", "OpenH264"},
    };
    static const std::vector<Candidate> hevc{
        {"hevc_nvenc", "NVIDIA NVENC"}, {"hevc_videotoolbox", "VideoToolbox"}, {"hevc_qsv", "Intel Quick Sync"},
        {"hevc_amf", "AMD AMF"},        {"hevc_mf", "Media Foundation"},     {"libx265", "x265"},
    };
    static const std::vector<Candidate> ffv1{{"ffv1", "FFV1"}};
    static const std::vector<Candidate> png{{"png", "PNG"}};
    switch (f) {
    case OutputFormat::H264: return h264;
    case OutputFormat::Hevc: return hevc;
    case OutputFormat::Ffv1: return ffv1;
    case OutputFormat::PngSequence: return png;
    }
    return png;
}

bool isHardware(const AVCodec* c) { return (c->capabilities & AV_CODEC_CAP_HARDWARE) != 0; }

bool endsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool isYuv420(AVPixelFormat f) { return f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_NV12; }

AVPixelFormat choosePixelFormat(const AVCodec* c, OutputFormat f)
{
    std::vector<AVPixelFormat> preferred;
    switch (f) {
    case OutputFormat::H264:
    case OutputFormat::Hevc:
        // Hardware encoders take NV12 natively; software ones planar 4:2:0.
        if (isHardware(c))
            preferred = {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
        else
            preferred = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12};
        break;
    case OutputFormat::Ffv1: preferred = {AV_PIX_FMT_BGR0, AV_PIX_FMT_GBRP}; break;
    case OutputFormat::PngSequence: preferred = {AV_PIX_FMT_RGB24}; break;
    }
    const std::vector<AVPixelFormat> supported = ff::supportedPixelFormats(c);
    if (supported.empty())
        return preferred.front();
    for (AVPixelFormat p : preferred)
        if (std::find(supported.begin(), supported.end(), p) != supported.end())
            return p;
    return AV_PIX_FMT_NONE;
}

// Everything about the encoder context except what the muxer decides (global headers). Shared
// by probing and open() so a probe opens the encoder exactly as an export would.
bool configure(AVCodecContext* ctx, const AVCodec* codec, const EncoderSettings& s, AVDictionary** opts)
{
    const AVPixelFormat pix = choosePixelFormat(codec, s.format);
    if (pix == AV_PIX_FMT_NONE)
        return false;
    const bool yuv = isYuv420(pix);
    ctx->width = yuv ? (s.width + 1) & ~1 : s.width;
    ctx->height = yuv ? (s.height + 1) & ~1 : s.height;
    ctx->pix_fmt = pix;
    ctx->time_base = av_inv_q(s.frameRate);
    ctx->framerate = s.frameRate;
    ctx->sample_aspect_ratio = AVRational{1, 1};
    const double fps = av_q2d(s.frameRate);
    // About a second between keyframes: players seek and step a comparison video a lot.
    ctx->gop_size = std::max(1, int(std::lround(fps)));
    ctx->thread_count = 0;
    if (yuv) {
        ctx->color_range = AVCOL_RANGE_MPEG;
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->chroma_sample_location = AVCHROMA_LOC_CENTER;  // 2x2 box average (convertYuv)
    } else {
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->colorspace = AVCOL_SPC_RGB;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_IEC61966_2_1;
    }

    const int q = int(s.quality);  // High, Medium, Small
    static constexpr int kX264Crf[] = {16, 20, 26};
    static constexpr int kX265Crf[] = {18, 22, 28};
    static constexpr int kNvencH264Cq[] = {18, 23, 28};
    static constexpr int kNvencHevcCq[] = {20, 25, 30};
    static constexpr double kBitsPerPixel[] = {0.20, 0.10, 0.05};
    const char* name = codec->name;
    auto set = [&](const char* k, const char* v) { av_dict_set(opts, k, v, 0); };
    auto setInt = [&](const char* k, int v) { av_dict_set_int(opts, k, v, 0); };
    const bool hevc = s.format == OutputFormat::Hevc;
    if (std::strcmp(name, "libx264") == 0) {
        set("preset", "fast");
        setInt("crf", kX264Crf[q]);
    } else if (std::strcmp(name, "libx265") == 0) {
        set("preset", "fast");
        setInt("crf", kX265Crf[q]);
        set("x265-params", "log-level=error");
    } else if (endsWith(name, "_nvenc")) {
        set("preset", "p5");
        set("tune", "hq");
        set("rc", "vbr");
        setInt("cq", hevc ? kNvencHevcCq[q] : kNvencH264Cq[q]);
        ctx->bit_rate = 0;
    } else if (std::strcmp(name, "ffv1") == 0) {
        ctx->level = 3;  // version 3: slices coded independently (threads) with CRCs
        set("slicecrc", "1");
        // FFV1 picks 4 slices by itself; more of them let large pictures use more cores.
        if (int64_t(s.width) * s.height >= 1'000'000)
            ctx->slices = 16;
    } else if (std::strcmp(name, "png") == 0) {
        // Frame threads; nothing to tune.
    } else {
        // Encoders without a common constant-quality control: bits per pixel.
        ctx->bit_rate = int64_t(kBitsPerPixel[q] * ctx->width * ctx->height * std::max(1.0, fps));
    }
    return true;
}

// ---- picture conversion -------------------------------------------------------------------

// Runs fn(y0, y1) over [0, rows) in bands of a multiple of `align` rows, on up to `maxThreads`
// threads (the calling one included). Small pictures stay on the calling thread.
template <class Fn>
void parallelRows(int rows, int align, int64_t pixels, Fn&& fn)
{
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int threads = int(std::clamp<int64_t>(pixels / 300'000, 1, std::min<int64_t>(hw, 16)));
    if (threads <= 1) {
        fn(0, rows);
        return;
    }
    int band = (rows + threads - 1) / threads;
    band = (band + align - 1) / align * align;
    std::vector<std::thread> pool;
    for (int y = band; y < rows; y += band)
        pool.emplace_back([&fn, y, rows, band] { fn(y, std::min(rows, y + band)); });
    fn(0, std::min(rows, band));
    for (std::thread& t : pool)
        t.join();
}

// BT.709 limited range from full range R'G'B' in 16.16 fixed point (rounded to nearest).
constexpr double kKr = 0.2126, kKb = 0.0722, kKg = 1.0 - kKr - kKb;
constexpr int fix(double v) { return int(v * 65536.0 + (v < 0 ? -0.5 : 0.5)); }
constexpr int kYr = fix(219.0 / 255.0 * kKr), kYg = fix(219.0 / 255.0 * kKg), kYb = fix(219.0 / 255.0 * kKb);
constexpr double kCb = 224.0 / 255.0 / (2.0 * (1.0 - kKb));
constexpr double kCr = 224.0 / 255.0 / (2.0 * (1.0 - kKr));
constexpr int kUr = fix(-kCb * kKr), kUg = fix(-kCb * kKg), kUb = fix(kCb * (1.0 - kKb));
constexpr int kVr = fix(kCr * (1.0 - kKr)), kVg = fix(-kCr * kKg), kVb = fix(-kCr * kKb);

// BGRX (w x h) -> 4:2:0 (W x H, both even, >= w/h): luma per pixel, chroma from the average of
// each 2x2 block; the column/row beyond an odd picture repeats its last one.
void convertYuv(const uint8_t* rows, ptrdiff_t stride, int w, int h, AVFrame* dst)
{
    const bool nv12 = dst->format == AV_PIX_FMT_NV12;
    const int W = dst->width, H = dst->height;
    parallelRows(H / 2, 1, int64_t(W) * H, [&](int c0, int c1) {
        for (int cy = c0; cy < c1; ++cy) {
            const int y0 = 2 * cy;
            const uint8_t* s0 = rows + ptrdiff_t(std::min(y0, h - 1)) * stride;
            const uint8_t* s1 = rows + ptrdiff_t(std::min(y0 + 1, h - 1)) * stride;
            uint8_t* l0 = dst->data[0] + ptrdiff_t(y0) * dst->linesize[0];
            uint8_t* l1 = l0 + dst->linesize[0];
            uint8_t* u = dst->data[1] + ptrdiff_t(cy) * dst->linesize[1];
            uint8_t* v = nv12 ? u + 1 : dst->data[2] + ptrdiff_t(cy) * dst->linesize[2];
            const int step = nv12 ? 2 : 1;
            for (int cx = 0; cx < W / 2; ++cx) {
                const int x0 = std::min(2 * cx, w - 1) * 4, x1 = std::min(2 * cx + 1, w - 1) * 4;
                int sb = 0, sg = 0, sr = 0;
                auto luma = [&](const uint8_t* p, uint8_t* out) {
                    const int b = p[0], g = p[1], r = p[2];
                    sb += b;
                    sg += g;
                    sr += r;
                    *out = uint8_t((kYr * r + kYg * g + kYb * b + (16 << 16) + (1 << 15)) >> 16);
                };
                luma(s0 + x0, l0 + 2 * cx);
                luma(s0 + x1, l0 + 2 * cx + 1);
                luma(s1 + x0, l1 + 2 * cx);
                luma(s1 + x1, l1 + 2 * cx + 1);
                // Sums of four: two more fraction bits. Never negative with the +128 offset.
                u[cx * step] = uint8_t((kUr * sr + kUg * sg + kUb * sb + (128 << 18) + (1 << 17)) >> 18);
                v[cx * step] = uint8_t((kVr * sr + kVg * sg + kVb * sb + (128 << 18) + (1 << 17)) >> 18);
            }
        }
    });
}

// Lossless repacks of BGRX.
void convertRgb(const uint8_t* rows, ptrdiff_t stride, int w, int h, AVFrame* dst)
{
    const AVPixelFormat f = AVPixelFormat(dst->format);
    parallelRows(h, 1, int64_t(w) * h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = rows + ptrdiff_t(y) * stride;
            if (f == AV_PIX_FMT_BGR0) {
                std::memcpy(dst->data[0] + ptrdiff_t(y) * dst->linesize[0], s, size_t(w) * 4);
            } else if (f == AV_PIX_FMT_RGB24) {
                uint8_t* d = dst->data[0] + ptrdiff_t(y) * dst->linesize[0];
                for (int x = 0; x < w; ++x, s += 4, d += 3) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                }
            } else {  // GBRP: planes G, B, R
                uint8_t* g = dst->data[0] + ptrdiff_t(y) * dst->linesize[0];
                uint8_t* b = dst->data[1] + ptrdiff_t(y) * dst->linesize[1];
                uint8_t* r = dst->data[2] + ptrdiff_t(y) * dst->linesize[2];
                for (int x = 0; x < w; ++x, s += 4) {
                    b[x] = s[0];
                    g[x] = s[1];
                    r[x] = s[2];
                }
            }
        }
    });
}

std::filesystem::path fsPath(const std::string& utf8)
{
#if defined(_WIN32)
    return std::filesystem::u8path(utf8);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string partPath(const std::string& path) { return path + ".part"; }

} // namespace

// ---- VideoEncoder -------------------------------------------------------------------------

VideoEncoder::~VideoEncoder()
{
    if (!m_finished)
        abort();
}

const char* VideoEncoder::extension(OutputFormat format)
{
    switch (format) {
    case OutputFormat::H264:
    case OutputFormat::Hevc: return "mp4";
    case OutputFormat::Ffv1: return "mkv";
    case OutputFormat::PngSequence: return "png";
    }
    return "mp4";
}

bool VideoEncoder::lossless(OutputFormat format)
{
    return format == OutputFormat::Ffv1 || format == OutputFormat::PngSequence;
}

std::string VideoEncoder::sequenceFile(const std::string& path, int number)
{
    std::string base = path;
    const size_t dot = base.find_last_of('.');
    const size_t sep = base.find_last_of("/\\");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
        std::string ext = base.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".png")
            base.resize(dot);
    }
    char num[32];
    std::snprintf(num, sizeof(num), "_%06d.png", number);
    return base + num;
}

std::vector<EncoderChoice> VideoEncoder::available(OutputFormat format)
{
    static std::mutex mutex;
    static std::map<OutputFormat, std::vector<EncoderChoice>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(format);
    if (it != cache.end())
        return it->second;
    std::vector<EncoderChoice> out;
    for (const Candidate& c : candidates(format)) {
        const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
        if (!codec)
            continue;
        // Open it once at a small size: a hardware encoder without its device or driver fails here.
        ff::LogSilencer quiet;
        ff::CodecContextPtr ctx(avcodec_alloc_context3(codec));
        AVDictionary* opts = nullptr;
        EncoderSettings s;
        s.format = format;
        s.width = 256;
        s.height = 256;
        bool ok = ctx && configure(ctx.get(), codec, s, &opts) && avcodec_open2(ctx.get(), codec, &opts) >= 0;
        av_dict_free(&opts);
        if (!ok) {
            SCVP_INFO("encoder %s is not usable here", c.name);
            continue;
        }
        const bool hw = isHardware(codec);
        out.push_back({c.name, std::string(c.label) + (hw ? " (GPU)" : ""), hw});
    }
    cache[format] = out;
    return out;
}

bool VideoEncoder::fail(std::string* error, const std::string& what, int err)
{
    if (error)
        *error = err ? what + ": " + ff::errorString(err) : what;
    return false;
}

int VideoEncoder::encodedWidth() const { return m_codec ? m_codec->width : 0; }
int VideoEncoder::encodedHeight() const { return m_codec ? m_codec->height : 0; }

bool VideoEncoder::open(const std::string& path, const EncoderSettings& settings, std::string* error)
{
    close();
    m_settings = settings;
    m_path = path;
    m_frames = m_packets = 0;
    m_finished = false;
    if (settings.width <= 0 || settings.height <= 0 || settings.frameRate.num <= 0 || settings.frameRate.den <= 0)
        return fail(error, "invalid picture size or frame rate");

    // "Automatic": the first usable encoder that also takes this picture (NVENC's H.264 stops
    // at 4096 px a side, two 4K videos side by side are 7682 px wide; x264 then takes over).
    std::vector<std::string> names;
    if (settings.encoder.empty()) {
        for (const EncoderChoice& c : available(settings.format))
            names.push_back(c.name);
        if (names.empty())
            return fail(error, "no encoder for this format in this FFmpeg build");
    } else {
        names.push_back(settings.encoder);
    }

    const bool sequence = settings.format == OutputFormat::PngSequence;
    const char* muxer = sequence ? "image2" : settings.format == OutputFormat::Ffv1 ? "matroska" : "mp4";
    // A video goes to "<path>.part" first: an export that is cancelled or fails neither leaves
    // half a file nor destroys the one it would have replaced.
    std::string url;
    if (sequence) {
        for (char ch : sequenceFile(path, 0)) {
            url += ch;
            if (ch == '%')
                url += '%';
        }
        url.replace(url.size() - 10, 6, "%06d");  // "_000000.png" -> "_%06d.png"
    } else {
        url = partPath(path);
    }
    int err = avformat_alloc_output_context2(&m_format, nullptr, muxer, url.c_str());
    if (err < 0 || !m_format)
        return fail(error, std::string("cannot create the ") + muxer + " output", err);

    std::string why;
    for (const std::string& name : names) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) {
            why = "encoder " + name + " is not in this FFmpeg build";
            continue;
        }
        m_codec.reset(avcodec_alloc_context3(codec));
        AVDictionary* opts = nullptr;
        if (!m_codec || !configure(m_codec.get(), codec, settings, &opts)) {
            av_dict_free(&opts);
            why = "encoder " + name + " takes none of the picture formats scvp produces";
            continue;
        }
        if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
            m_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ff::LogSilencer quiet(names.size() > 1);  // trying the next one is not an error
        err = avcodec_open2(m_codec.get(), codec, &opts);
        av_dict_free(&opts);
        if (err >= 0) {
            m_encoderName = name;
            break;
        }
        why = "cannot open encoder " + name + " for " + std::to_string(settings.width) + "x" + std::to_string(settings.height) +
              ": " + ff::errorString(err);
        m_codec.reset();
    }
    if (!m_codec) {
        close();
        return fail(error, why);
    }

    m_stream = avformat_new_stream(m_format, nullptr);
    if (!m_stream) {
        close();
        return fail(error, "cannot add the video stream");
    }
    avcodec_parameters_from_context(m_stream->codecpar, m_codec.get());
    m_stream->time_base = m_codec->time_base;
    m_stream->avg_frame_rate = settings.frameRate;
    m_stream->r_frame_rate = settings.frameRate;
    if (settings.format == OutputFormat::Hevc)
        m_stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');  // what Apple players accept

    if (!(m_format->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&m_format->pb, url.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            close();
            return fail(error, "cannot write " + url, err);
        }
    }
    m_created = true;
    AVDictionary* muxOpts = nullptr;
    if (settings.format == OutputFormat::H264 || settings.format == OutputFormat::Hevc)
        av_dict_set(&muxOpts, "movflags", "+faststart", 0);  // index first: plays while it downloads
    if (sequence)
        av_dict_set_int(&muxOpts, "start_number", settings.startNumber, 0);
    err = avformat_write_header(m_format, &muxOpts);
    av_dict_free(&muxOpts);
    if (err < 0) {
        abort();
        return fail(error, "cannot write the file header", err);
    }
    m_headerWritten = true;

    m_frame = ff::makeFrame();
    m_packet = ff::makePacket();
    m_frame->format = m_codec->pix_fmt;
    m_frame->width = m_codec->width;
    m_frame->height = m_codec->height;
    err = av_frame_get_buffer(m_frame.get(), 0);
    if (err < 0) {
        abort();
        return fail(error, "out of memory", err);
    }
    SCVP_INFO("export: %s %dx%d %s via %s", url.c_str(), m_codec->width, m_codec->height,
              av_get_pix_fmt_name(m_codec->pix_fmt), m_encoderName.c_str());
    return true;
}

bool VideoEncoder::write(const uint8_t* rows, int stride, std::string* error)
{
    if (!m_codec || !m_headerWritten || m_finished)
        return fail(error, "encoder is not open");
    // The encoder may still hold the previous picture (lookahead); then use a new buffer.
    if (!av_frame_is_writable(m_frame.get())) {
        ff::FramePtr f = ff::makeFrame();
        f->format = m_frame->format;
        f->width = m_frame->width;
        f->height = m_frame->height;
        const int err = av_frame_get_buffer(f.get(), 0);
        if (err < 0)
            return fail(error, "out of memory", err);
        m_frame = std::move(f);
    }
    AVFrame* f = m_frame.get();
    if (isYuv420(AVPixelFormat(f->format)))
        convertYuv(rows, stride, m_settings.width, m_settings.height, f);
    else
        convertRgb(rows, stride, m_settings.width, m_settings.height, f);
    f->pts = m_frames;
    f->color_range = m_codec->color_range;
    f->colorspace = m_codec->colorspace;
    f->color_primaries = m_codec->color_primaries;
    f->color_trc = m_codec->color_trc;
    f->chroma_location = m_codec->chroma_sample_location;
    const int err = avcodec_send_frame(m_codec.get(), f);
    if (err < 0)
        return fail(error, "encoding failed", err);
    ++m_frames;
    return drain(false, error);
}

bool VideoEncoder::drain(bool flush, std::string* error)
{
    for (;;) {
        int err = avcodec_receive_packet(m_codec.get(), m_packet.get());
        if (err == AVERROR(EAGAIN) && !flush)
            return true;
        if (err == AVERROR_EOF)
            return true;
        if (err < 0)
            return fail(error, "encoding failed", err);
        av_packet_rescale_ts(m_packet.get(), m_codec->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        err = av_interleaved_write_frame(m_format, m_packet.get());
        if (err < 0)
            return fail(error, "cannot write to the file (disk full?)", err);
        ++m_packets;
    }
}

bool VideoEncoder::finish(std::string* error)
{
    if (!m_codec || !m_headerWritten || m_finished)
        return fail(error, "encoder is not open");
    int err = avcodec_send_frame(m_codec.get(), nullptr);
    if (err < 0 && err != AVERROR_EOF) {
        abort();
        return fail(error, "encoding failed", err);
    }
    if (!drain(true, error)) {
        abort();
        return false;
    }
    err = av_write_trailer(m_format);
    if (err < 0) {
        abort();
        return fail(error, "cannot complete the file", err);
    }
    close();
    if (m_settings.format != OutputFormat::PngSequence) {
        std::error_code ec;
        std::filesystem::rename(fsPath(partPath(m_path)), fsPath(m_path), ec);
        if (ec) {
            removeOutput();
            return fail(error, "cannot rename the finished file to " + m_path + ": " + ec.message());
        }
    }
    m_finished = true;
    return true;
}

void VideoEncoder::abort()
{
    const bool created = m_created;
    close();
    if (created)
        removeOutput();
    m_finished = true;  // nothing left to clean up
}

void VideoEncoder::removeOutput()
{
    std::error_code ec;
    if (m_settings.format == OutputFormat::PngSequence) {
        // Files are written as packets come out; one more may be half written.
        for (int64_t i = 0; i <= m_packets; ++i)
            std::filesystem::remove(fsPath(sequenceFile(m_path, m_settings.startNumber + int(i))), ec);
    } else {
        std::filesystem::remove(fsPath(partPath(m_path)), ec);
    }
}

void VideoEncoder::close()
{
    m_codec.reset();
    if (m_format) {
        if (m_format->pb && !(m_format->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m_format->pb);
        avformat_free_context(m_format);
        m_format = nullptr;
    }
    m_stream = nullptr;
    m_headerWritten = false;
    m_created = false;
    m_frame.reset();
    m_packet.reset();
}

} // namespace scvp
