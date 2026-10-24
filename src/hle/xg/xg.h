// Our xg_x.dll (texture layouts), also used by the placement-texture upload so
// both sides agree on where texels are.
#pragma once

#include <cstdint>

#include "common/winabi.h"
#include "hle/xg/gcn.h"

namespace fl::xg {

struct TexDesc {
    uint32_t dim;  // XG/D3D11 resource dimension: 2 tex1d, 3 tex2d, 4 tex3d
    uint32_t width, height, depth, array_size, mips, format, samples, tile_mode, pitch;
};

UINT optimal_tile_mode(UINT dim, UINT format, UINT width, UINT height, UINT array_size, UINT samples, UINT bind,
                       UINT misc);

// Layout of plane 0 (colour, or depth). False if the descriptor is invalid.
bool layout_for(const TexDesc& t, gcn::SurfaceDesc* d, gcn::Layout* l);

}  // namespace fl::xg
