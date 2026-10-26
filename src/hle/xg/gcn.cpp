#include "hle/xg/gcn.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <map>

#include "addrinterface.h"
#include "common/log.h"
#include "common/workers.h"

namespace fl::gcn {
namespace {

// The console's GB_TILE_MODE0..31 and GB_MACROTILE_MODE0..15 values
// (FINDINGS §3.2: read from xg_x.dll's tables; they are the standard GCN 1.1
// 4-pipe P4_16x16 configuration, the same form as amdgpu's gfx_v7 tables).
constexpr uint32_t kTileModes[32] = {
    0x00800150, 0x00800950, 0x00801150, 0x00801950, 0x00802950, 0x00800148, 0x00801158, 0x00802958,
    0x00000144, 0x00000148, 0x02000150, 0x06000154, 0x06000158, 0x00400148, 0x02400150, 0x02400170,
    0x06400154, 0x06400158, 0x0640016c, 0x0100014c, 0x0100015c, 0x01000174, 0x01000164, 0x01000168,
    0x0100017c, 0x01000160, 0x01000178, 0x00c00148, 0x02c00150, 0x06c00154, 0x06c00158, 0x00000000,
};
constexpr uint32_t kMacroModes[16] = {
    0xa8, 0x94, 0x90, 0x90, 0x90, 0x90, 0x40, 0x00, 0xed, 0xe9, 0xe8, 0xe4, 0xd0, 0x90, 0x40, 0x00,
};

// GB_ADDR_CONFIG: 4 pipes (log2 = 2), 256 B pipe interleave (0), 2 shader
// engines (1), 2 KB rows (1). xg_x.dll uses pipe interleave 0x100 and row 0x800.
constexpr uint32_t kGbAddrConfig = 2 | (1u << 12) | (1u << 28);

void* ADDR_API alloc_cb(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
ADDR_E_RETURNCODE ADDR_API free_cb(const ADDR_FREESYSMEM_INPUT* in) {
    std::free(in->pVirtAddr);
    return ADDR_OK;
}
ADDR_E_RETURNCODE ADDR_API print_cb(const ADDR_DEBUGPRINT_INPUT* in) { return ADDR_OK; }

ADDR_HANDLE handle() {
    static ADDR_HANDLE h = [] {
        ADDR_CREATE_INPUT in = {};
        ADDR_CREATE_OUTPUT out = {};
        in.size = sizeof(in);
        out.size = sizeof(out);
        in.chipEngine = 0x0000000A;  // CIASICIDGFXENGINE_SOUTHERNISLAND
        in.chipFamily = 0x78;        // FAMILY_CI
        in.chipRevision = 0x14;      // Bonaire range (GCN 1.1, like the console GPU)
        in.callbacks.allocSysMem = alloc_cb;
        in.callbacks.freeSysMem = free_cb;
        in.callbacks.debugPrint = print_cb;
        in.createFlags.useTileIndex = 1;
        in.regValue.gbAddrConfig = kGbAddrConfig;
        in.regValue.noOfBanks = 2;  // MC_ARB_RAMCFG.NOOFBANK (8 banks); CI takes banks from the macro table
        in.regValue.noOfRanks = 1;
        in.regValue.pTileConfig = kTileModes;
        in.regValue.noOfEntries = 32;
        in.regValue.pMacroTileConfig = kMacroModes;
        in.regValue.noOfMacroEntries = 16;
        if (AddrCreate(&in, &out) != ADDR_OK) fatal("gcn: AddrCreate failed");
        return out.hLib;
    }();
    return h;
}

AddrTileMode array_mode(uint32_t index) { return static_cast<AddrTileMode>(((kTileModes[index] >> 2) & 0xf) + 1); }

std::mutex g_mutex;  // addrlib calls are cheap; keep them serialized to be safe

void fill_surface_input(const SurfaceDesc& d, uint32_t mip, uint32_t base_pitch_px, ADDR_COMPUTE_SURFACE_INFO_INPUT* in) {
    in->size = sizeof(*in);
    in->tileIndex = static_cast<INT_32>(d.tile_index);
    in->tileMode = array_mode(d.tile_index);
    in->bpp = d.bytes_per_element * 8;
    if (d.block == 4) in->format = d.bytes_per_element == 8 ? ADDR_FMT_BC1 : ADDR_FMT_BC3;
    in->numSamples = d.samples;
    in->numFrags = d.samples;
    in->width = std::max(1u, d.width >> mip);
    in->height = std::max(1u, d.height >> mip);
    in->numSlices = d.is_3d ? std::max(1u, d.depth >> mip) : d.array_size;
    in->mipLevel = mip;
    in->numMipLevels = d.mips;
    in->flags.texture = 1;
    in->flags.color = !d.is_depth;
    in->flags.depth = d.is_depth;
    in->flags.volume = d.is_3d;
    in->flags.pow2Pad = d.mips > 1;
    if (mip > 0) in->basePitch = base_pitch_px;
}

}  // namespace

uint32_t tile_mode_reg(uint32_t index) { return index < 32 ? kTileModes[index] : 0; }

bool compute_layout(const SurfaceDesc& d, Layout* out) {
    std::lock_guard lock(g_mutex);
    out->mips.clear();
    out->size = 0;
    out->align = 0;
    uint32_t base_pitch_px = 0;
    for (uint32_t mip = 0; mip < d.mips; mip++) {
        ADDR_COMPUTE_SURFACE_INFO_INPUT in = {};
        ADDR_COMPUTE_SURFACE_INFO_OUTPUT o = {};
        ADDR_TILEINFO tile = {};
        o.size = sizeof(o);
        o.pTileInfo = &tile;
        fill_surface_input(d, mip, base_pitch_px, &in);
        if (AddrComputeSurfaceInfo(handle(), &in, &o) != ADDR_OK) return false;
        if (mip == 0) base_pitch_px = o.pitch * d.block;
        MipLayout m;
        uint64_t off = (out->size + o.baseAlign - 1) / o.baseAlign * o.baseAlign;
        m.offset = off;
        m.size = o.surfSize;
        // addrlib's sliceSize is the whole level for a volume; XG's
        // Slice2DSizeBytes (what the game steps z by, e.g. its linear 3D LUT
        // fill at rva 0x13b38c0) is one depth slice.
        m.slice_size = d.is_3d ? o.surfSize / std::max(1u, o.depth) : o.sliceSize;
        m.pitch = o.pitch;
        m.padded_height = o.height;
        m.padded_depth = o.depth;
        m.width = std::max(1u, ((d.width >> mip ? d.width >> mip : 1) + d.block - 1) / d.block);
        m.height = std::max(1u, ((d.height >> mip ? d.height >> mip : 1) + d.block - 1) / d.block);
        m.depth = in.numSlices;
        m.base_align = o.baseAlign;
        m.tile_index = o.tileIndex;
        m.macro_index = o.macroModeIndex;
        m.block_w = o.blockWidth;
        m.block_h = o.blockHeight;
        out->size = off + o.surfSize;
        if (o.baseAlign > out->align) out->align = o.baseAlign;
        out->mips.push_back(m);
    }
    return true;
}

namespace {

// Per-mip addressing parameters, resolved once.
struct MipAddr {
    ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in = {};
    ADDR_TILEINFO tile = {};
};

void prepare(const SurfaceDesc& d, const MipLayout& m, MipAddr* a) {
    ADDR_CONVERT_TILEINDEX_INPUT ci = {};
    ADDR_CONVERT_TILEINDEX_OUTPUT co = {};
    ci.size = sizeof(ci);
    co.size = sizeof(co);
    ci.tileIndex = m.tile_index;
    ci.macroModeIndex = m.macro_index;
    co.pTileInfo = &a->tile;
    {
        std::lock_guard lock(g_mutex);
        if (AddrConvertTileIndex(handle(), &ci, &co) != ADDR_OK) fatal("gcn: AddrConvertTileIndex(%d)", m.tile_index);
    }
    auto& in = a->in;
    in.size = sizeof(in);
    // Elements are addressed as "pixels" of bytes_per_element (blocks for BCn).
    in.bpp = d.bytes_per_element * 8;
    in.pitch = m.pitch;
    in.height = m.padded_height;
    in.numSlices = m.padded_depth;
    in.numSamples = d.samples;
    in.numFrags = d.samples;
    in.tileIndex = m.tile_index;
    in.tileMode = co.tileMode;
    in.tileType = co.tileType;
    in.isDepth = d.is_depth;
    in.pTileInfo = &a->tile;
}

uint64_t addr_of(MipAddr& a, uint32_t x, uint32_t y, uint32_t slice, uint32_t sample) {
    ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT o = {};
    o.size = sizeof(o);
    a.in.x = x;
    a.in.y = y;
    a.in.slice = slice;
    a.in.sample = sample;
    if (AddrComputeSurfaceAddrFromCoord(handle(), &a.in, &o) != ADDR_OK) fatal("gcn: AddrComputeSurfaceAddrFromCoord");
    return o.addr;
}

}  // namespace

uint64_t element_offset(const SurfaceDesc& d, const Layout& l, uint32_t mip, uint32_t x, uint32_t y, uint32_t slice,
                        uint32_t sample) {
    MipAddr a;
    prepare(d, l.mips[mip], &a);
    return l.mips[mip].offset + addr_of(a, x, y, slice, sample);
}

namespace {

// Fast untiling. addrlib costs ~50 ns per element, so a 2048x2048 BCn texture
// (all mips) took ~13 ms on the render thread, in the hitches when driving
// into new areas. Tiled addressing repeats: an element's address is its
// tile's base address combined with an offset pattern that is the same in
// every tile. On this layout family (P4_16x16) `base XOR pattern` holds with
// the 64x32 addrlib block, and `base + pattern` with 256x64; 1D tiling
// repeats every 8x8 micro tile (measured over whole levels). If no model
// holds for a layout, the per-element path is used.
struct Model {
    uint32_t w, h;
    bool xor_;
};

// A layout's addressing, resolved once: tile bases and the in-tile pattern.
// Addresses depend only on addrlib's inputs (MipAddr) and the slice, so a plan
// is shared by every texture with the same layout (most textures share a few
// sizes and formats). Built and checked against addrlib once per layout:
// every element for small levels, 1024 samples plus the corners otherwise.
struct Plan {
    Model md;
    uint32_t tiles_x = 0;
    std::vector<int64_t> pattern, tiles;
    int64_t at(uint32_t x, uint32_t y) const {
        int64_t t = tiles[size_t(y / md.h) * tiles_x + x / md.w], p = pattern[size_t(y % md.h) * md.w + x % md.w];
        return md.xor_ ? (t ^ p) : t + p;
    }
};

std::shared_ptr<const Plan> build_plan(MipAddr& a, const MipLayout& m, uint32_t slice, Model md) {
    const uint32_t bw = md.w, bh = md.h;
    if (!bw || !bh || uint64_t(bw) * bh > 65536 || m.pitch % bw || m.padded_height % bh) return nullptr;
    auto plan = std::make_shared<Plan>();
    plan->md = md;
    plan->tiles_x = (m.width + bw - 1) / bw;
    const uint32_t tiles_y = (m.height + bh - 1) / bh;
    const uint64_t origin = addr_of(a, 0, 0, slice, 0);
    plan->pattern.resize(size_t(bw) * bh);
    for (uint32_t j = 0; j < bh; j++)
        for (uint32_t i = 0; i < bw; i++) {
            uint64_t v = addr_of(a, i, j, slice, 0);
            plan->pattern[size_t(j) * bw + i] = md.xor_ ? int64_t(v ^ origin) : int64_t(v) - int64_t(origin);
        }
    plan->tiles.resize(size_t(plan->tiles_x) * tiles_y);
    for (uint32_t ty = 0; ty < tiles_y; ty++)
        for (uint32_t tx = 0; tx < plan->tiles_x; tx++)
            plan->tiles[size_t(ty) * plan->tiles_x + tx] = addr_of(a, tx * bw, ty * bh, slice, 0);
    auto ok = [&](uint32_t x, uint32_t y) {
        int64_t v = plan->at(x, y);
        return v >= 0 && uint64_t(v) < m.size && v == int64_t(addr_of(a, x, y, slice, 0));
    };
    if (uint64_t(m.width) * m.height <= 16384) {
        for (uint32_t y = 0; y < m.height; y++)
            for (uint32_t x = 0; x < m.width; x++)
                if (!ok(x, y)) return nullptr;
        return plan;
    }
    const uint32_t xs[] = {0, m.width - 1}, ys[] = {0, m.height - 1};
    for (uint32_t x : xs)
        for (uint32_t y : ys)
            if (!ok(x, y)) return nullptr;
    uint64_t seed = 0x9e3779b97f4a7c15ull ^ (uint64_t(m.pitch) << 20) ^ slice;
    for (int k = 0; k < 1024; k++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        if (!ok(uint32_t(seed >> 33) % m.width, uint32_t(seed >> 13) % m.height)) return nullptr;
    }
    return plan;
}

std::mutex g_plans_mutex;
std::map<std::vector<uint64_t>, std::shared_ptr<const Plan>> g_plans;  // nullptr: no model fits, use addrlib

std::shared_ptr<const Plan> plan_for(MipAddr& a, const MipLayout& m, uint32_t slice) {
    const auto& in = a.in;
    const auto& t = a.tile;
    using u = uint64_t;
    std::vector<uint64_t> key = {u(in.bpp), u(in.pitch), u(in.height), u(in.numSlices), u(in.numSamples),
                                 u(in.numFrags), u(uint32_t(in.tileIndex)), u(in.tileMode), u(in.tileType),
                                 u(in.isDepth), u(t.banks), u(t.bankWidth), u(t.bankHeight), u(t.macroAspectRatio),
                                 u(t.tileSplitBytes), u(t.pipeConfig), u(slice), u(m.width), u(m.height),
                                 u(m.block_w), u(m.block_h), u(m.size)};
    {
        std::lock_guard lock(g_plans_mutex);
        auto it = g_plans.find(key);
        if (it != g_plans.end()) return it->second;
    }
    std::shared_ptr<const Plan> plan;
    const Model models[] = {{m.block_w, m.block_h, true}, {m.block_w * 4, m.block_h * 2, false}, {8, 8, true},
                            {8, 8, false}};
    for (const Model& md : models)
        if ((plan = build_plan(a, m, slice, md))) break;
    std::lock_guard lock(g_plans_mutex);
    if (g_plans.size() > 1024) g_plans.clear();  // ~16 KB each at most
    g_plans.emplace(key, plan);
    return plan;
}

bool untile_rows(const Plan& p, const MipLayout& m, uint32_t bpe, const uint8_t* base, uint8_t* linear,
                 size_t linear_pitch, uint32_t y0, uint32_t y1) {
    const Plan* plan = &p;
    const uint32_t bw = plan->md.w, bh = plan->md.h;
    const bool x = plan->md.xor_;
    for (uint32_t y = y0; y < y1; y++) {
        const int64_t* row_tiles = &plan->tiles[size_t(y / bh) * plan->tiles_x];
        const int64_t* row_pattern = &plan->pattern[size_t(y % bh) * bw];
        uint8_t* out = linear + y * linear_pitch;
        for (uint32_t i = 0; i < m.width; i++) {
            int64_t off = x ? (row_tiles[i / bw] ^ row_pattern[i % bw]) : row_tiles[i / bw] + row_pattern[i % bw];
            if (off < 0 || uint64_t(off) + bpe > m.size) return false;  // never read outside the level
            const uint8_t* src = base + off;
            switch (bpe) {
                case 1: out[i] = *src; break;
                case 2: std::memcpy(out + i * 2, src, 2); break;
                case 4: std::memcpy(out + i * 4, src, 4); break;
                case 8: std::memcpy(out + i * 8, src, 8); break;
                case 16: std::memcpy(out + i * 16, src, 16); break;
                default: std::memcpy(out + size_t(i) * bpe, src, bpe);
            }
        }
    }
    return true;
}

bool untile_fast(MipAddr& a, const MipLayout& m, uint32_t slice, uint32_t bpe, const uint8_t* base, uint8_t* linear,
                 size_t linear_pitch) {
    std::shared_ptr<const Plan> plan = plan_for(a, m, slice);
    if (!plan) return false;
    // Big levels: row bands on the worker pool (~16k elements each).
    const uint64_t elements = uint64_t(m.width) * m.height;
    if (elements < (1u << 17)) return untile_rows(*plan, m, bpe, base, linear, linear_pitch, 0, m.height);
    const uint32_t band = std::max<uint32_t>(1, uint32_t((1u << 14) / m.width));
    const size_t bands = (m.height + band - 1) / band;
    std::atomic<bool> ok{true};
    parallel_for(bands, [&](size_t b) {
        uint32_t y0 = uint32_t(b * band), y1 = std::min(m.height, y0 + band);
        if (!untile_rows(*plan, m, bpe, base, linear, linear_pitch, y0, y1)) ok = false;
    });
    return ok;
}

}  // namespace

void untile(const SurfaceDesc& d, const Layout& l, uint32_t mip, uint32_t slice, const uint8_t* tiled, uint8_t* linear,
            size_t linear_pitch) {
    const MipLayout& m = l.mips[mip];
    MipAddr a;
    prepare(d, m, &a);
    const uint8_t* base = tiled + m.offset;
    uint32_t bpe = d.bytes_per_element;
    if (d.samples <= 1 && untile_fast(a, m, slice, bpe, base, linear, linear_pitch)) return;
    for (uint32_t y = 0; y < m.height; y++)
        for (uint32_t x = 0; x < m.width; x++)
            std::memcpy(linear + y * linear_pitch + size_t(x) * bpe, base + addr_of(a, x, y, slice, 0), bpe);
}

void untile_reference(const SurfaceDesc& d, const Layout& l, uint32_t mip, uint32_t slice, const uint8_t* tiled,
                      uint8_t* linear, size_t linear_pitch) {
    const MipLayout& m = l.mips[mip];
    MipAddr a;
    prepare(d, m, &a);
    const uint8_t* base = tiled + m.offset;
    uint32_t bpe = d.bytes_per_element;
    for (uint32_t y = 0; y < m.height; y++)
        for (uint32_t x = 0; x < m.width; x++)
            std::memcpy(linear + y * linear_pitch + size_t(x) * bpe, base + addr_of(a, x, y, slice, 0), bpe);
}

}  // namespace fl::gcn
