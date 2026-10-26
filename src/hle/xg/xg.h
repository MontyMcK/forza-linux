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
    uint32_t bind = 0, misc = 0;  // D3D11 bind / misc flags: they decide the metadata planes
};

UINT optimal_tile_mode(UINT dim, UINT format, UINT width, UINT height, UINT array_size, UINT samples, UINT bind,
                       UINT misc);

// Layout of the main surface plane (colour, or depth: not HTILE). Its offsets
// are relative to that plane. False if the descriptor is invalid.
bool layout_for(const TexDesc& t, gcn::SurfaceDesc* d, gcn::Layout* l);

// Every plane of the resource as xg_x reports it: XG_PLANE_USAGE (1 colour,
// 4 HTILE, 7 depth, 8 stencil, ...), byte offset from the resource start, and
// size. `total` is the whole resource. False if the descriptor is invalid.
struct PlaneSpan {
    uint32_t usage = 0;
    uint64_t offset = 0, size = 0;
};
bool plane_spans(const TexDesc& t, PlaneSpan out[3], int* count, uint64_t* total);

}  // namespace fl::xg
