#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

#include <cstring>

namespace scvp::test {

QString fixtureDir()
{
    const QByteArray env = qgetenv("SCVP_FIXTURE_DIR");
    return env.isEmpty() ? QStringLiteral(SCVP_FIXTURE_DIR) : QString::fromLocal8Bit(env);
}

std::vector<Fixture> fixtures()
{
    static const std::vector<Fixture> all = [] {
        std::vector<Fixture> out;
        QFile f(QDir(fixtureDir()).filePath(QStringLiteral("manifest.tsv")));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return out;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QStringList c = in.readLine().split(QLatin1Char('\t'));
            if (c.size() < 5)
                continue;
            out.push_back({c[0], c[1], QDir(fixtureDir()).filePath(c[2]), c[3], c[4].toInt()});
        }
        return out;
    }();
    return all;
}

std::vector<Fixture> fixtures(const QStringList& kinds)
{
    std::vector<Fixture> out;
    for (const Fixture& f : fixtures())
        if (kinds.contains(f.kind))
            out.push_back(f);
    return out;
}

std::optional<Fixture> fixture(const QString& name)
{
    for (const Fixture& f : fixtures())
        if (f.name == name)
            return f;
    return std::nullopt;
}

std::vector<std::string> referenceHashes(const Fixture& fx)
{
    std::vector<std::string> out;
    QFile f(QDir(fixtureDir()).filePath(fx.name + QStringLiteral(".framemd5")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith(QLatin1Char('#')) || line.trimmed().isEmpty())
            continue;
        out.push_back(line.section(QLatin1Char(','), -1).trimmed().toStdString());
    }
    return out;
}

std::string frameMd5(const AVFrame* frame)
{
    const auto fmt = static_cast<AVPixelFormat>(frame->format);
    const int size = av_image_get_buffer_size(fmt, frame->width, frame->height, 1);
    if (size <= 0)
        return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (av_image_copy_to_buffer(buf.data(), size, frame->data, frame->linesize, fmt, frame->width,
                                frame->height, 1) < 0)
        return {};
    uint8_t digest[16];
    av_md5_sum(digest, buf.data(), size_t(size));
    static const char* hex = "0123456789abcdef";
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[2 * i] = hex[digest[i] >> 4];
        s[2 * i + 1] = hex[digest[i] & 15];
    }
    return s;
}

std::string frameMd5AsSoftware(const AVFrame* f)
{
    AVPixelFormat target;
    int shift = 0;
    switch (f->format) {
    case AV_PIX_FMT_NV12: target = AV_PIX_FMT_YUV420P; break;
    case AV_PIX_FMT_P010LE: target = AV_PIX_FMT_YUV420P10LE; shift = 6; break;
    case AV_PIX_FMT_P016LE: target = AV_PIX_FMT_YUV420P16LE; break;
    default: return frameMd5(f);
    }
    AVFrame* out = av_frame_alloc();
    out->format = target;
    out->width = f->width;
    out->height = f->height;
    av_frame_get_buffer(out, 0);
    const bool wide = target != AV_PIX_FMT_YUV420P;
    const int cw = (f->width + 1) / 2;
    const int ch = (f->height + 1) / 2;
    for (int y = 0; y < f->height; ++y) {
        if (!wide) {
            std::memcpy(out->data[0] + ptrdiff_t(y) * out->linesize[0], f->data[0] + ptrdiff_t(y) * f->linesize[0], size_t(f->width));
        } else {
            auto* src = reinterpret_cast<const uint16_t*>(f->data[0] + ptrdiff_t(y) * f->linesize[0]);
            auto* dst = reinterpret_cast<uint16_t*>(out->data[0] + ptrdiff_t(y) * out->linesize[0]);
            for (int x = 0; x < f->width; ++x)
                dst[x] = uint16_t(src[x] >> shift);
        }
    }
    for (int y = 0; y < ch; ++y) {
        if (!wide) {
            const uint8_t* uv = f->data[1] + ptrdiff_t(y) * f->linesize[1];
            uint8_t* u = out->data[1] + ptrdiff_t(y) * out->linesize[1];
            uint8_t* v = out->data[2] + ptrdiff_t(y) * out->linesize[2];
            for (int x = 0; x < cw; ++x) {
                u[x] = uv[2 * x];
                v[x] = uv[2 * x + 1];
            }
        } else {
            auto* uv = reinterpret_cast<const uint16_t*>(f->data[1] + ptrdiff_t(y) * f->linesize[1]);
            auto* u = reinterpret_cast<uint16_t*>(out->data[1] + ptrdiff_t(y) * out->linesize[1]);
            auto* v = reinterpret_cast<uint16_t*>(out->data[2] + ptrdiff_t(y) * out->linesize[2]);
            for (int x = 0; x < cw; ++x) {
                u[x] = uint16_t(uv[2 * x] >> shift);
                v[x] = uint16_t(uv[2 * x + 1] >> shift);
            }
        }
    }
    const std::string h = frameMd5(out);
    av_frame_free(&out);
    return h;
}

} // namespace scvp::test
