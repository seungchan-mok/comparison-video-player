#pragma once

#include "engine/FfmpegCompat.h"

namespace scvp {

// True when two system-memory frames hold the same picture: same size and the same value for
// every component sample, whatever the memory layout. Hardware decoders output semi-planar
// NV12/P010/P016 where software decoders give planar yuv420p/yuv420p10le/yuv420p16le; those
// compare equal when the decoded values are. Formats with different subsampling, depth or
// colour model never do.
bool samePixelValues(const AVFrame* a, const AVFrame* b);

} // namespace scvp
