#include "engine/RawVideoSource.h"

#include "engine/Log.h"
#include "engine/Platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

#if defined(_WIN32)
#define scvp_fseek _fseeki64
#define scvp_ftell _ftelli64
#else
#define scvp_fseek fseeko
#define scvp_ftell ftello
#endif

namespace scvp {
namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string baseName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

AVPixelFormat pixelFormatFromToken(const std::string& t)
{
    static const std::pair<const char*, AVPixelFormat> aliases[] = {
        {"i420", AV_PIX_FMT_YUV420P}, {"iyuv", AV_PIX_FMT_YUV420P}, {"yuv420", AV_PIX_FMT_YUV420P}, {"yuv422", AV_PIX_FMT_YUV422P},
        {"yuv444", AV_PIX_FMT_YUV444P}, {"yuy2", AV_PIX_FMT_YUYV422}, {"yuyv", AV_PIX_FMT_YUYV422}, {"uyvy", AV_PIX_FMT_UYVY422},
        {"p010", AV_PIX_FMT_P010LE}, {"p016", AV_PIX_FMT_P016LE}, {"yuv420p10", AV_PIX_FMT_YUV420P10LE},
        {"yuv422p10", AV_PIX_FMT_YUV422P10LE}, {"yuv444p10", AV_PIX_FMT_YUV444P10LE}, {"yuv420p12", AV_PIX_FMT_YUV420P12LE},
        {"yuv420p16", AV_PIX_FMT_YUV420P16LE}, {"rgb", AV_PIX_FMT_RGB24}, {"bgr", AV_PIX_FMT_BGR24}, {"y8", AV_PIX_FMT_GRAY8},
        {"gray", AV_PIX_FMT_GRAY8}, {"y16", AV_PIX_FMT_GRAY16LE}, {"gray16", AV_PIX_FMT_GRAY16LE}, {"gray10", AV_PIX_FMT_GRAY10LE},
        {"gray12", AV_PIX_FMT_GRAY12LE}, {"rgba", AV_PIX_FMT_RGBA}, {"bgra", AV_PIX_FMT_BGRA},
    };
    for (const auto& [name, fmt] : aliases)
        if (t == name)
            return fmt;
    // Any FFmpeg name (nv12, yuv420p10le, uyvy422, gbrp, bayer_rggb8, ...); hardware formats excluded.
    const AVPixelFormat f = av_get_pix_fmt(t.c_str());
    const AVPixFmtDescriptor* d = f != AV_PIX_FMT_NONE ? av_pix_fmt_desc_get(f) : nullptr;
    return d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL) && !(d->flags & AV_PIX_FMT_FLAG_BITSTREAM) ? f : AV_PIX_FMT_NONE;
}

// Reads frames by offset; sequential reads continue without seeking.
class RawVideoSession final : public IDecodeSession {
public:
    RawVideoSession(const RawVideoSource& src, std::string path, SessionConfig cfg)
        : m_source(src)
        , m_path(std::move(path))
        , m_cfg(std::move(cfg))
    {
    }
    ~RawVideoSession() override
    {
        if (m_file)
            std::fclose(m_file);
    }

    bool open(std::string* error)
    {
        m_file = platform::fopenUtf8(m_path, "rb");
        if (!m_file) {
            if (error)
                *error = "cannot open " + m_path;
            return false;
        }
        std::setvbuf(m_file, nullptr, _IONBF, 0);  // whole frames per read: no stdio copy
        return true;
    }

    DecodeStatus seekTo(const SeekPoint& sp) override
    {
        if (!sp.valid() || sp.pres < 0 || sp.pres >= m_source.index()->frameCount())
            return DecodeStatus::Error;
        m_next = sp.pres;
        m_lastPres = -1;
        m_eof = false;
        return DecodeStatus::Ok;
    }

    DecodeStatus next(FramePtr& out, const DecodeHints& hints) override
    {
        const int32_t n = m_source.index()->frameCount();
        if (hints.cancelled())
            return DecodeStatus::Cancelled;
        if (m_next >= n) {
            m_eof = true;
            return DecodeStatus::Eof;
        }
        const RawVideoParams& p = m_source.params();
        AVFrame* av = m_cfg.pool ? m_cfg.pool->allocFrame(p.format, p.width, p.height) : nullptr;
        if (!av)
            return DecodeStatus::Error;
        if (!read(av, m_next)) {
            av_frame_free(&av);
            return DecodeStatus::Error;
        }
        av->pts = m_next;
        av->color_range = AVCOL_RANGE_UNSPECIFIED;
        av->colorspace = AVCOL_SPC_UNSPECIFIED;
        av->flags |= AV_FRAME_FLAG_KEY;
        av->pict_type = AV_PICTURE_TYPE_I;
        auto f = std::make_shared<Frame>();
        f->av = av;
        f->pres = f->dec = m_next;
        f->pts = m_next;
        f->key = true;
        f->pictType = 'I';
        f->bytes = ff::frameBufferBytes(av);
        out = std::move(f);
        m_lastPres = m_next++;
        return DecodeStatus::Ok;
    }

    Position position() const override { return {m_next, m_lastPres, m_eof}; }
    bool wasSkipped(int32_t) const override { return false; }
    int indexMismatches() const override { return 0; }
    std::string hardwareDevice() const override { return {}; }

private:
    bool read(AVFrame* av, int32_t n)
    {
        const RawVideoParams& p = m_source.params();
        const int64_t offset = p.headerBytes + int64_t(n) * int64_t(m_source.frameSize());
        if (offset != m_filePos && scvp_fseek(m_file, offset, SEEK_SET) != 0)
            return false;
        m_filePos = offset;
        // Planes as stored (align 1); copied row by row into the pool's aligned planes.
        int lines[4] = {};
        av_image_fill_linesizes(lines, p.format, p.width);
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(p.format);
        size_t sizes[4] = {};
        ptrdiff_t ls[4] = {lines[0], lines[1], lines[2], lines[3]};
        av_image_fill_plane_sizes(sizes, p.format, p.height, ls);
        bool direct = true;
        for (int i = 0; i < 4; ++i)
            direct &= !sizes[i] || av->linesize[i] == lines[i];
        if (direct) {
            for (int i = 0; i < 4 && sizes[i]; ++i)
                if (std::fread(av->data[i], 1, sizes[i], m_file) != sizes[i])
                    return false;
        } else {
            m_buffer.resize(m_source.frameSize());
            if (std::fread(m_buffer.data(), 1, m_buffer.size(), m_file) != m_buffer.size())
                return false;
            const uint8_t* src = m_buffer.data();
            for (int i = 0; i < 4 && sizes[i]; ++i) {
                const int rows = int(sizes[i] / size_t(lines[i]));
                av_image_copy_plane(av->data[i], av->linesize[i], src, lines[i], lines[i], rows);
                src += sizes[i];
            }
        }
        (void)d;
        m_filePos += int64_t(m_source.frameSize());
        return true;
    }

    const RawVideoSource& m_source;
    std::string m_path;
    SessionConfig m_cfg;
    std::FILE* m_file = nullptr;
    int64_t m_filePos = -1;
    int32_t m_next = 0;
    int32_t m_lastPres = -1;
    bool m_eof = false;
    std::vector<uint8_t> m_buffer;
};

} // namespace

bool isRawVideoExtension(const std::string& path)
{
    const std::string name = lower(baseName(path));
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string ext = name.substr(dot + 1);
    static const char* exts[] = {"yuv", "raw", "rgb", "bgr", "nv12", "nv21", "p010", "y4m_raw", "gray", "uyvy", "yuyv", "bayer"};
    for (const char* e : exts)
        if (ext == e)
            return true;
    return false;
}

RawVideoParams parseRawVideoName(const std::string& path)
{
    RawVideoParams p;
    const std::string name = lower(baseName(path));
    std::smatch m;
    static const std::regex size(R"((^|[^0-9])([0-9]{2,5})x([0-9]{2,5})([^0-9]|$))");
    if (std::regex_search(name, m, size)) {
        p.width = std::stoi(m[2]);
        p.height = std::stoi(m[3]);
    }
    static const std::regex fps(R"((^|[^0-9])([0-9]+(\.[0-9]+)?)fps)");
    if (std::regex_search(name, m, fps))
        p.frameRate = std::stod(m[2]);
    // Bayer names contain a separator in FFmpeg's spelling (bayer_rggb16le).
    static const std::regex bayer(R"(bayer[_-]?(rggb|bggr|grbg|gbrg)(8|16le|16be|16)?)");
    if (std::regex_search(name, m, bayer)) {
        const std::string depth = m[2].matched ? (m[2] == "16" ? std::string("16le") : m[2].str()) : std::string("8");
        p.format = av_get_pix_fmt(("bayer_" + m[1].str() + depth).c_str());
        return p;
    }
    // Pixel format: a token between separators; the extension counts too (".nv12").
    static const std::regex token(R"([a-z][a-z0-9]*)");
    for (auto it = std::sregex_iterator(name.begin(), name.end(), token); it != std::sregex_iterator(); ++it) {
        const AVPixelFormat f = pixelFormatFromToken(it->str());
        if (f != AV_PIX_FMT_NONE)
            p.format = f;  // the last match wins ("yuv_..._nv12.yuv": nv12)
    }
    return p;
}

std::unique_ptr<RawVideoSource> RawVideoSource::open(const std::string& path, const RawVideoParams& params, std::string* error)
{
    if (!params.valid()) {
        if (error)
            *error = "raw video needs a frame size and a pixel format";
        return nullptr;
    }
    const int frameSize = av_image_get_buffer_size(params.format, params.width, params.height, 1);
    if (frameSize <= 0) {
        if (error)
            *error = "unsupported raw pixel format";
        return nullptr;
    }
    std::FILE* f = platform::fopenUtf8(path, "rb");
    if (!f) {
        if (error)
            *error = "cannot open " + path;
        return nullptr;
    }
    scvp_fseek(f, 0, SEEK_END);
    const int64_t fileSize = scvp_ftell(f);
    std::fclose(f);
    const int64_t payload = fileSize - params.headerBytes;
    const int64_t frames = payload > 0 ? payload / frameSize : 0;
    if (frames <= 0) {
        if (error)
            *error = "file is smaller than one frame of the given size and format";
        return nullptr;
    }
    if (payload % frameSize)
        SCVP_WARN("%s: %lld trailing bytes do not form a whole frame (size or format wrong?)", path.c_str(),
                  (long long)(payload % frameSize));

    std::unique_ptr<RawVideoSource> src(new RawVideoSource);
    src->m_path = path;
    src->m_params = params;
    if (src->m_params.frameRate <= 0)
        src->m_params.frameRate = 30.0;
    src->m_frameSize = size_t(frameSize);
    const AVRational rate = av_d2q(src->m_params.frameRate, 1001000);
    VideoInfo& info = src->m_info;
    info.path = path;
    info.formatName = "rawvideo";
    info.codecName = "rawvideo";
    info.width = params.width;
    info.height = params.height;
    info.pixelFormat = params.format;
    info.timeBase = av_inv_q(rate);
    info.nominalFrameRate = src->m_params.frameRate;
    info.durationSeconds = double(frames) / src->m_params.frameRate;
    info.bitRate = int64_t(double(frameSize) * 8 * src->m_params.frameRate);

    FrameIndexBuilder b(av_inv_q(rate), src->m_params.frameRate);
    b.setOrdering(FrameIndex::Ordering::Arithmetic);
    for (int64_t i = 0; i < frames; ++i) {
        IndexPacket p;
        p.pts = p.dts = i;
        p.pos = params.headerBytes + i * frameSize;
        p.size = frameSize;
        p.flags = IndexPacket::Key;
        b.add(p);
    }
    src->m_index = b.build(true);
    return src;
}

std::unique_ptr<IDecodeSession> RawVideoSource::openSession(const SessionConfig& cfg, std::string* error)
{
    auto s = std::make_unique<RawVideoSession>(*this, m_path, cfg);
    if (!s->open(error))
        return nullptr;
    return s;
}

} // namespace scvp
