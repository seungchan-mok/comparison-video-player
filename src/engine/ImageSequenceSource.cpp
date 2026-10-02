#include "engine/ImageSequenceSource.h"

#include "engine/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

namespace scvp {
namespace fs = std::filesystem;
namespace {

fs::path toPath(const std::string& utf8)
{
#if defined(_WIN32)
    return fs::u8path(utf8);
#else
    return fs::path(utf8);
#endif
}

std::string toUtf8(const fs::path& p)
{
#if defined(_WIN32)
    return p.u8string();
#else
    return p.string();
#endif
}

std::string lowerExt(const fs::path& p)
{
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e.empty() ? e : e.substr(1);
}

// Splits "shot_0012.v2.png" at its last digit run: ("shot_", "0012", ".v2.png").
bool splitNumber(const std::string& name, std::string* prefix, std::string* digits, std::string* suffix)
{
    const size_t end = name.find_last_of("0123456789");
    if (end == std::string::npos)
        return false;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(name[begin - 1])))
        --begin;
    *prefix = name.substr(0, begin);
    *digits = name.substr(begin, end - begin + 1);
    *suffix = name.substr(end + 1);
    return true;
}

// Decodes one image file per frame; decoders are kept for files of the same codec.
class ImageSequenceSession final : public IDecodeSession {
public:
    ImageSequenceSession(const ImageSequenceSource& src, SessionConfig cfg)
        : m_source(src)
        , m_cfg(std::move(cfg))
    {
        m_pkt = ff::makePacket();
        m_frame = ff::makeFrame();
    }

    DecodeStatus seekTo(const SeekPoint& sp) override
    {
        if (!sp.valid() || sp.pres < 0 || sp.pres >= int32_t(m_source.files().size()))
            return DecodeStatus::Error;
        m_next = sp.pres;
        m_lastPres = -1;
        m_eof = false;
        return DecodeStatus::Ok;
    }

    DecodeStatus next(FramePtr& out, const DecodeHints& hints) override
    {
        if (hints.cancelled())
            return DecodeStatus::Cancelled;
        if (m_next >= int32_t(m_source.files().size())) {
            m_eof = true;
            return DecodeStatus::Eof;
        }
        AVFrame* av = decode(m_source.files()[size_t(m_next)]);
        if (!av) {
            ++m_next;  // a broken file must not stall playback forever
            return DecodeStatus::Error;
        }
        av->pts = m_next;
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
    AVFrame* decode(const std::string& file)
    {
        std::string err;
        auto demux = openDemuxer(file, InputOptions{}, &err);
        if (!demux) {
            SCVP_WARN("%s: %s", file.c_str(), err.c_str());
            return nullptr;
        }
        const AVCodecParameters* par = demux->videoStream()->codecpar;
        if (!m_ctx || m_codec != par->codec_id) {
            const AVCodec* codec = chooseDecoder(par->codec_id, false);
            m_ctx.reset(codec ? avcodec_alloc_context3(codec) : nullptr);
            if (!m_ctx || avcodec_parameters_to_context(m_ctx.get(), par) < 0)
                return nullptr;
            m_ctx->thread_count = 1;  // parallelism comes from several sessions
            if (m_cfg.pool)
                m_cfg.pool->attachTo(m_ctx.get());
            if (avcodec_open2(m_ctx.get(), codec, nullptr) < 0) {
                m_ctx.reset();
                return nullptr;
            }
            m_codec = par->codec_id;
        } else {
            avcodec_parameters_to_context(m_ctx.get(), par);  // sizes may differ between files
        }
        while (av_read_frame(demux->fmt.get(), m_pkt.get()) >= 0) {
            if (m_pkt->stream_index != demux->stream) {
                av_packet_unref(m_pkt.get());
                continue;
            }
            const int ret = avcodec_send_packet(m_ctx.get(), m_pkt.get());
            av_packet_unref(m_pkt.get());
            if (ret < 0)
                break;
            if (avcodec_receive_frame(m_ctx.get(), m_frame.get()) == 0)
                return av_frame_clone_and_unref();
            break;
        }
        // Single-image codecs may hold the frame until flushed.
        avcodec_send_packet(m_ctx.get(), nullptr);
        AVFrame* out = avcodec_receive_frame(m_ctx.get(), m_frame.get()) == 0 ? av_frame_clone_and_unref() : nullptr;
        avcodec_flush_buffers(m_ctx.get());
        if (!out)
            m_ctx.reset();  // drained: reopen for the next file
        return out;
    }

    AVFrame* av_frame_clone_and_unref()
    {
        AVFrame* out = av_frame_alloc();
        av_frame_move_ref(out, m_frame.get());
        return out;
    }

    const ImageSequenceSource& m_source;
    SessionConfig m_cfg;
    ff::CodecContextPtr m_ctx;
    AVCodecID m_codec = AV_CODEC_ID_NONE;
    ff::PacketPtr m_pkt;
    ff::FramePtr m_frame;
    int32_t m_next = 0;
    int32_t m_lastPres = -1;
    bool m_eof = false;
};

} // namespace

bool naturalLess(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])), db = std::isdigit(static_cast<unsigned char>(b[j]));
        if (da && db) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])))
                ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])))
                ++ej;
            // Compare numerically without overflow: strip leading zeros, then length, then digits.
            size_t si = i, sj = j;
            while (si + 1 < ei && a[si] == '0')
                ++si;
            while (sj + 1 < ej && b[sj] == '0')
                ++sj;
            if (ei - si != ej - sj)
                return ei - si < ej - sj;
            const int c = a.compare(si, ei - si, b, sj, ej - sj);
            if (c)
                return c < 0;
            if (ei - i != ej - j)
                return ei - i < ej - j;  // "01" before "001"
            i = ei;
            j = ej;
            continue;
        }
        const char ca = char(std::tolower(static_cast<unsigned char>(a[i]))), cb = char(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb)
            return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

bool isImageExtension(const std::string& path)
{
    static const char* exts[] = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "dpx", "exr", "webp", "tga", "ppm", "pgm", "pnm", "pam", "j2k", "jp2", "jxl", "qoi", "sgi", "hdr"};
    const std::string e = lowerExt(toPath(path));
    for (const char* x : exts)
        if (e == x)
            return true;
    return false;
}

bool isDirectory(const std::string& path)
{
    std::error_code ec;
    return fs::is_directory(toPath(path), ec);
}

std::vector<std::string> findImageSequence(const std::string& path)
{
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path p = toPath(path);
    if (fs::is_directory(p, ec)) {
        // Every image in the folder, of its most common type.
        std::map<std::string, std::vector<fs::path>> byExt;
        for (const auto& e : fs::directory_iterator(p, ec))
            if (e.is_regular_file(ec) && isImageExtension(toUtf8(e.path())))
                byExt[lowerExt(e.path())].push_back(e.path());
        const std::vector<fs::path>* best = nullptr;
        for (const auto& [ext, files] : byExt)
            if (!best || files.size() > best->size())
                best = &files;
        if (best)
            for (const fs::path& f : *best)
                out.push_back(toUtf8(f));
    } else {
        const std::string name = toUtf8(p.filename());
        std::string prefix, digits, suffix;
        if (!splitNumber(name, &prefix, &digits, &suffix)) {
            out.push_back(path);
            return out;
        }
        const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec))
                continue;
            const std::string n = toUtf8(e.path().filename());
            std::string pr, dg, sf;
            if (splitNumber(n, &pr, &dg, &sf) && pr == prefix && sf == suffix)
                out.push_back(toUtf8(e.path()));
        }
        if (out.empty())
            out.push_back(path);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return naturalLess(toUtf8(toPath(a).filename()), toUtf8(toPath(b).filename()));
    });
    return out;
}

std::unique_ptr<ImageSequenceSource> ImageSequenceSource::open(const std::string& path, double frameRate, std::string* error)
{
    std::vector<std::string> files = findImageSequence(path);
    if (files.empty()) {
        if (error)
            *error = "no images found";
        return nullptr;
    }
    // Describe the sequence from its first image.
    auto demux = openDemuxer(files.front(), InputOptions{}, error);
    if (!demux)
        return nullptr;
    const AVCodecParameters* par = demux->videoStream()->codecpar;
    std::unique_ptr<ImageSequenceSource> src(new ImageSequenceSource);
    src->m_files = std::move(files);
    const double fps = frameRate > 0 ? frameRate : 30.0;
    const AVRational rate = av_d2q(fps, 1001000);
    VideoInfo& info = src->m_info;
    info.path = path;
    info.formatName = "image sequence";
    info.codecName = avcodec_get_name(par->codec_id);
    info.width = par->width;
    info.height = par->height;
    info.pixelFormat = AVPixelFormat(par->format);
    info.timeBase = av_inv_q(rate);
    info.nominalFrameRate = fps;
    info.durationSeconds = double(src->m_files.size()) / fps;
    info.colorRange = par->color_range;
    info.colorSpace = par->color_space;

    FrameIndexBuilder b(av_inv_q(rate), fps);
    b.setOrdering(FrameIndex::Ordering::Arithmetic);
    for (size_t i = 0; i < src->m_files.size(); ++i) {
        IndexPacket p;
        p.pts = p.dts = int64_t(i);
        p.size = 1;
        p.flags = IndexPacket::Key;
        b.add(p);
    }
    src->m_index = b.build(true);
    return src;
}

std::unique_ptr<IDecodeSession> ImageSequenceSource::openSession(const SessionConfig& cfg, std::string*)
{
    return std::make_unique<ImageSequenceSession>(*this, cfg);
}

} // namespace scvp
