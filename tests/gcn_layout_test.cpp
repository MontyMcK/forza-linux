// Checks our GCN layouts (addrlib + the console's tables) against texture
// sizes produced by the console's own XG for real assets. No game files needed.
#include <cstdio>

#include "hle/xg/gcn.h"

namespace {

struct Case {
    const char* what;
    unsigned w, h, mips, bpe, block, tile;
    unsigned long long expect;
};

// tile 14 = what XGComputeOptimalTileMode picks for these sizes (FINDINGS §3.2).
const Case kCases[] = {
    {"1024x1024 BC4/BC1-class (8 B blocks), 1 mip", 1024, 1024, 1, 8, 4, 14, 524288},
    {"4096x1536 16 B blocks, 13 mips", 4096, 1536, 13, 16, 4, 14, 11190272},
    {"2048x768 16 B blocks, 12 mips", 2048, 768, 12, 16, 4, 14, 2801664},
    {"512x512 1 B uncompressed (R8), 10 mips", 512, 512, 10, 1, 1, 14, 350464},
};

}  // namespace

int main() {
    int failed = 0;
    for (const Case& c : kCases) {
        fl::gcn::SurfaceDesc d;
        d.width = c.w, d.height = c.h, d.mips = c.mips, d.bytes_per_element = c.bpe, d.block = c.block;
        d.tile_index = c.tile;
        fl::gcn::Layout l;
        bool ok = fl::gcn::compute_layout(d, &l);
        bool pass = ok && l.size == c.expect;
        failed += !pass;
        std::printf("%s  %-45s size %llu (expect %llu)\n", pass ? "PASS" : "FAIL", c.what,
                    (unsigned long long)l.size, c.expect);
        if (!pass)
            for (size_t i = 0; i < l.mips.size(); i++)
                std::printf("      mip %2zu off %9llu size %8llu pitch %4u h %4u tile %2d align %u\n", i,
                            (unsigned long long)l.mips[i].offset, (unsigned long long)l.mips[i].size,
                            l.mips[i].pitch, l.mips[i].padded_height, l.mips[i].tile_index, l.mips[i].base_align);
    }
    return failed ? 1 : 0;
}
