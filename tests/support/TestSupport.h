#pragma once

#include "engine/FfmpegCompat.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace scvp::test {

struct Fixture {
    QString name;
    QString kind;      // container | rawes | rawvideo | imageseq
    QString path;      // absolute
    QString openArgs;  // rawvideo: pix_fmt:WxH:fps, imageseq: fps, else "-"
    int frames = 0;
};

QString fixtureDir();
// All fixtures from manifest.tsv; empty when the fixtures were not generated.
std::vector<Fixture> fixtures();
std::vector<Fixture> fixtures(const QStringList& kinds);
std::optional<Fixture> fixture(const QString& name);

// framemd5 hashes of a fixture, in presentation order.
std::vector<std::string> referenceHashes(const Fixture& f);

// md5 of a frame's pixels packed with alignment 1 (what ffmpeg's framemd5 hashes).
std::string frameMd5(const AVFrame* frame);

} // namespace scvp::test

// Skips the current test when the fixtures have not been generated.
#define SCVP_REQUIRE_FIXTURES()                                                                  \
    do {                                                                                         \
        if (::scvp::test::fixtures().empty())                                                    \
            QSKIP("fixtures missing: run 'cmake --build <dir> --target fixtures'");              \
    } while (0)
