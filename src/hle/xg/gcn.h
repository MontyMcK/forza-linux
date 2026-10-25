// Xbox One GPU (GCN 1.1, "CI") surface layouts, computed with AMD's addrlib
// (third_party/addrlib, MIT) configured like the console: 4 pipes (P4_16x16),
// 256-byte pipe interleave, 2 KB DRAM rows, and the console's tile mode and
// macro tile mode tables (FINDINGS §3.2). Used by our xg_x implementation and by
// the placement-texture untiler, so both always agree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fl::gcn {

struct SurfaceDesc {
    uint32_t width = 1, height = 1;  // pixels (mip 0)
    uint32_t depth = 1;              // 3D depth, or 1
    uint32_t array_size = 1;
    uint32_t mips = 1;
    uint32_t samples = 1;
    uint32_t bytes_per_element = 4;  // per pixel, or per block for block-compressed formats
    uint32_t block = 1;              // 1, or 4 for BCn
    uint32_t tile_index = 8;         // GB_TILE_MODE index (XG_TILE_MODE)
    bool is_3d = false;
    bool is_depth = false;
};

struct MipLayout {
    uint64_t offset = 0;      // from the start of the surface
    uint64_t size = 0;        // all slices
    uint64_t slice_size = 0;  // one slice (2D array) / one depth slice (3D)
    uint32_t pitch = 0;       // padded width, in elements
    uint32_t padded_height = 0;
    uint32_t padded_depth = 0;
    uint32_t width = 0, height = 0, depth = 0;  // real size of the level, in elements
    uint32_t base_align = 0;
    int tile_index = 0;       // may be degraded for small mips (2D -> 1D)
    int macro_index = -1;
};

struct Layout {
    uint64_t size = 0;
    uint64_t align = 0;
    std::vector<MipLayout> mips;
};

bool compute_layout(const SurfaceDesc& d, Layout* out);

// Byte offset of element (x, y) in slice `slice` of mip `mip`, relative to the
// start of the surface. Coordinates are in elements (blocks for BCn).
uint64_t element_offset(const SurfaceDesc& d, const Layout& l, uint32_t mip, uint32_t x, uint32_t y, uint32_t slice,
                        uint32_t sample = 0);

// Tiled (game memory) -> linear rows for one mip/slice.
void untile(const SurfaceDesc& d, const Layout& l, uint32_t mip, uint32_t slice, const uint8_t* tiled, uint8_t* linear,
            size_t linear_pitch);

// The console's GB_TILE_MODEn value for a tile index (for array mode etc.).
uint32_t tile_mode_reg(uint32_t index);

}  // namespace fl::gcn
