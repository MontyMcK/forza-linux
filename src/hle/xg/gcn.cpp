#include "hle/xg/gcn.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "addrinterface.h"
#include "common/log.h"

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
        m.slice_size = o.sliceSize;
        m.pitch = o.pitch;
        m.padded_height = o.height;
        m.padded_depth = o.depth;
        m.width = std::max(1u, ((d.width >> mip ? d.width >> mip : 1) + d.block - 1) / d.block);
        m.height = std::max(1u, ((d.height >> mip ? d.height >> mip : 1) + d.block - 1) / d.block);
        m.depth = in.numSlices;
        m.base_align = o.baseAlign;
        m.tile_index = o.tileIndex;
        m.macro_index = o.macroModeIndex;
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

void untile(const SurfaceDesc& d, const Layout& l, uint32_t mip, uint32_t slice, const uint8_t* tiled, uint8_t* linear,
            size_t linear_pitch) {
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
