// DXGI format properties needed for surface layouts (public DXGI formats).
// Xbox-only formats are added when the game uses one, with evidence.
#pragma once

#include <cstdint>

namespace fl::xg {

struct FormatInfo {
    uint8_t bytes = 0;  // per element: per pixel, or per 4x4 block when block == 4
    uint8_t block = 1;  // 1, or 4 for BCn
    bool depth = false;
    bool stencil = false;  // has a separate stencil plane
};

// Aborts on formats we don't know (so a wrong size can't go unnoticed).
const FormatInfo& format_info(uint32_t dxgi_format);

}  // namespace fl::xg
