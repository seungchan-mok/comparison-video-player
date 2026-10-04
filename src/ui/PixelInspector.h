#pragma once

#include "render/PixelReadback.h"

#include <QString>

namespace scvp::ui {

// Text for the pixel under the cursor: position, A and B as displayed RGB and source codes,
// and A - B. Shown by the HUD at the bottom of the view; Ctrl+C copies it.
QString formatPixelSample(const render::PixelSample& s);

} // namespace scvp::ui
