// Checks our GCN layouts (addrlib + the console's tables) against texture
// sizes produced by the console's own XG for real assets. No game files needed.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

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
    // The fast untiler must give exactly what per-element addrlib gives, for
    // every mip of these layouts and several tile modes.
    struct UCase {
        const char* what;
        unsigned w, h, mips, bpe, block, tile, array;
    };
    const UCase kUntile[] = {
        {"2048x2048 BC1 (8 B blocks) tile 14", 2048, 2048, 12, 8, 4, 14, 1},
        {"1024x512 BC3 (16 B blocks) tile 14", 1024, 512, 11, 16, 4, 14, 1},
        {"512x512 RGBA8 tile 14", 512, 512, 10, 4, 1, 14, 1},
        {"256x256 RGBA8 tile 13, 6 slices", 256, 256, 9, 4, 1, 13, 6},
        {"300x200 R8 tile 14", 300, 200, 9, 1, 1, 14, 1},
        {"640x360 RGBA16F tile 10", 640, 360, 1, 8, 1, 10, 1},
        {"128x128 RGBA8 tile 8 (linear)", 128, 128, 8, 4, 1, 8, 1},
        {"1024x1024 BC1 tile 2", 1024, 1024, 11, 8, 4, 2, 1},
    };
    for (const UCase& c : kUntile) {
        fl::gcn::SurfaceDesc d;
        d.width = c.w, d.height = c.h, d.mips = c.mips, d.bytes_per_element = c.bpe, d.block = c.block;
        d.tile_index = c.tile, d.array_size = c.array;
        fl::gcn::Layout l;
        if (!fl::gcn::compute_layout(d, &l)) {
            std::printf("SKIP  untile %-40s (no layout)\n", c.what);
            continue;
        }
        std::vector<uint8_t> tiled(l.size);
        uint64_t s = 12345;
        for (auto& b : tiled) b = uint8_t((s = s * 6364136223846793005ull + 1) >> 56);
        bool pass = true;
        double fast_ms = 0, ref_ms = 0;
        for (uint32_t mip = 0; mip < l.mips.size() && pass; mip++) {
            const auto& m = l.mips[mip];
            size_t pitch = size_t(m.width) * c.bpe;
            std::vector<uint8_t> a(pitch * m.height), b(pitch * m.height);
            for (uint32_t slice = 0; slice < c.array && pass; slice++) {
                fl::gcn::untile(d, l, mip, slice, tiled.data() + slice * m.slice_size, a.data(), pitch);  // first use
                auto t0 = std::chrono::steady_clock::now();
                fl::gcn::untile(d, l, mip, slice, tiled.data() + slice * m.slice_size, a.data(), pitch);  // cached
                auto t1 = std::chrono::steady_clock::now();
                fl::gcn::untile_reference(d, l, mip, slice, tiled.data() + slice * m.slice_size, b.data(), pitch);
                auto t2 = std::chrono::steady_clock::now();
                fast_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                ref_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
                if (a != b) {
                    pass = false;
                    std::printf("      mismatch at mip %u slice %u\n", mip, slice);
                }
            }
        }
        failed += !pass;
        std::printf("%s  untile %-40s fast (cached) %7.2f ms, addrlib %8.2f ms\n", pass ? "PASS" : "FAIL", c.what, fast_ms, ref_ms);
    }
    return failed ? 1 : 0;
}
