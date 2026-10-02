#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

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

} // namespace scvp::test
