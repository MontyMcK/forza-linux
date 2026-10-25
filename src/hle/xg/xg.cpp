// xg_x.dll: texture layout helpers (FINDINGS §3.2). Layouts come from
// gcn.cpp (addrlib with the console's tables); tile mode choices reimplement
// the console's XG heuristics. Struct layouts were recovered from how the game
// and xg_x.dll's own methods read them.
#include <cstring>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"
#include "hle/xg/formats.h"
#include "hle/xg/gcn.h"
#include "hle/xg/xg.h"
#include "loader/imports.h"

namespace fl::xg {
namespace {

// ---- XG_RESOURCE_LAYOUT (0xd40 bytes) ----
struct MipLevelLayout {  // 0x48
    uint64_t size_bytes;          // +0x00
    uint64_t offset_bytes;        // +0x08 (xg slot 3 returns it)
    uint64_t slice2d_size_bytes;  // +0x10 (the game steps slices by it)
    uint32_t pitch_pixels;        // +0x18
    uint32_t pitch_bytes;         // +0x1c (the game steps rows by it)
    uint32_t alignment_bytes;     // +0x20
    uint32_t padded_width;        // +0x24 elements
    uint32_t padded_height;       // +0x28
    uint32_t padded_depth;        // +0x2c
    uint32_t width;               // +0x30 elements (xg slots 5/6 loop over these three)
    uint32_t height;              // +0x34
    uint32_t depth_or_array;      // +0x38
    uint32_t sample_count;        // +0x3c
    uint32_t tile_mode;           // +0x40
    uint32_t reserved;            // +0x44
};
static_assert(sizeof(MipLevelLayout) == 0x48);

struct PlaneLayout {  // 0x460
    uint32_t usage;              // +0x00 (xg writes 1 for colour; 2/3/4/7/8 for other planes)
    uint32_t pad0;
    uint64_t size_bytes;         // +0x08
    uint64_t base_offset_bytes;  // +0x10
    uint64_t base_alignment;     // +0x18
    uint32_t bytes_per_element;  // +0x20 (the game reads it; xg slots read it)
    uint32_t pad1;
    MipLevelLayout mips[15];     // +0x28
};
static_assert(sizeof(PlaneLayout) == 0x460);

struct ResourceLayout {  // 0xd40
    uint64_t size_bytes;
    uint64_t base_alignment;
    uint32_t mip_levels;
    uint32_t planes;
    PlaneLayout plane[3];
    uint8_t pad[8];
};
static_assert(sizeof(ResourceLayout) == 0xd40);
static_assert(offsetof(ResourceLayout, plane[0].bytes_per_element) == 0x38);
static_assert(offsetof(ResourceLayout, plane[0].mips[0]) == 0x40);

constexpr uint32_t kTileLinearGeneral = 0x1f;

// Plane of a computer: its gcn surface and layout.
struct Plane {
    gcn::SurfaceDesc desc;
    gcn::Layout layout;
};

// The XGTextureAddressComputer object. NOT COM: slot 0 is AddRef, 1 Release,
// 2 GetResourceLayout (xg_x vtable at rva 0x14670).
struct Computer {
    void** vtbl;
    int refs = 1;
    ResourceLayout layout{};
    Plane planes[3];             // by layout plane index
    bool has_surface[3] = {};    // false for metadata planes (HTILE): no texels
    int num_planes = 0;
    int main_plane = 0;          // the colour or depth surface
};

// XG_PLANE_USAGE values, as xg_x writes them (2D builder rva 0x1770, see FINDINGS §3.2).
enum : uint32_t {
    kUsageDefault = 1,
    kUsageCmask = 2,
    kUsageFmask = 3,
    kUsageHtile = 4,
    kUsageLuma = 5,     // two-plane formats not bound as depth (unverified names)
    kUsageChroma = 6,
    kUsageDepth = 7,
    kUsageStencil = 8,
};
constexpr uint32_t kBindRenderTarget = 0x20, kBindDepthStencil = 0x40;
constexpr uint32_t kMiscNoDepthCompression = 1u << 20, kMiscNoStencilCompression = 1u << 21;  // names ours

// HTILE bytes for a depth surface, as xg_x's DB setup computes it (rva 0x59c0):
// 4 bytes per 8x8 tile of the padded surface, which is first aligned to 128x64
// (single slice of at most 2M pixels) or 512x512, rounded up to 16 KiB.
uint64_t htile_bytes(uint32_t pitch, uint32_t height, uint32_t slices) {
    bool small = static_cast<uint64_t>(pitch) * height <= 0x200000 && slices == 1;
    uint32_t aw = small ? 128 : 512, ah = small ? 64 : 512;
    uint64_t w = (pitch + aw - 1) / aw * aw, h = (height + ah - 1) / ah * ah;
    return ((w / 8) * (h / 8) * slices * 4 + 0x3fff) & ~uint64_t(0x3fff);
}

uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = std::max(w, std::max(h, d)), n = 1;
    while (m > 1) m >>= 1, n++;
    return n;
}

void fill_plane(PlaneLayout* p, const Plane& pl, uint32_t usage, uint64_t base_offset, uint32_t tile_mode) {
    p->usage = usage;
    p->size_bytes = pl.layout.size;
    p->base_offset_bytes = base_offset;
    p->base_alignment = pl.layout.align;
    p->bytes_per_element = pl.desc.bytes_per_element;
    for (size_t i = 0; i < pl.layout.mips.size() && i < 15; i++) {
        const gcn::MipLayout& m = pl.layout.mips[i];
        MipLevelLayout& o = p->mips[i];
        o.size_bytes = m.size;
        o.offset_bytes = base_offset + m.offset;
        o.slice2d_size_bytes = m.slice_size;
        o.pitch_pixels = m.pitch * pl.desc.block;
        o.pitch_bytes = m.pitch * pl.desc.bytes_per_element;
        o.alignment_bytes = m.base_align;
        o.padded_width = m.pitch;
        o.padded_height = m.padded_height;
        o.padded_depth = m.padded_depth;
        o.width = m.width;
        o.height = m.height;
        o.depth_or_array = m.depth;
        o.sample_count = pl.desc.samples;
        o.tile_mode = m.tile_index >= 0 ? static_cast<uint32_t>(m.tile_index) : tile_mode;
    }
}

HRESULT build(Computer* c, const TexDesc& t) {
    const FormatInfo& fi = format_info(t.format);
    if (t.pitch) fatal("xg: explicit Pitch %u not supported yet", t.pitch);
    if (t.tile_mode == kTileLinearGeneral) fatal("xg: linear-general tile mode not supported yet");
    gcn::SurfaceDesc d;
    d.width = t.width;
    d.height = t.height;
    d.depth = t.depth;
    d.array_size = t.array_size;
    d.mips = t.mips ? t.mips : full_chain(t.width, t.height, t.depth);
    d.samples = t.samples ? t.samples : 1;
    d.bytes_per_element = fi.bytes;
    d.block = fi.block;
    d.tile_index = t.tile_mode;
    d.is_3d = t.dim == 4;
    d.is_depth = fi.depth;
    // GCN keeps depth (32 bit for D24S8/D32S8) and stencil (8 bit) apart.
    gcn::SurfaceDesc stencil = d;
    stencil.bytes_per_element = 1;
    stencil.is_depth = false;
    if (fi.depth && fi.stencil) d.bytes_per_element = 4;

    // The planes, as xg_x's 2D builder (rva 0x1770) picks them: a single-mip
    // depth-stencil target gets [HTILE] DEPTH [STENCIL] (HTILE unless both
    // compression-off misc bits are set); other two-plane formats get DEPTH
    // STENCIL when bound as depth, else 5/6; everything else one plane.
    struct Spec { uint32_t usage; bool surface; gcn::SurfaceDesc desc; };
    Spec spec[3];
    int n = 0;
    bool single_mip = d.mips == 1;
    bool depth_target = t.dim == 3 && single_mip && (t.bind & kBindDepthStencil) && !(t.bind & kBindRenderTarget);
    if (depth_target) {
        if (!(t.misc & kMiscNoDepthCompression) || !(t.misc & kMiscNoStencilCompression))
            spec[n++] = {kUsageHtile, false, {}};
        c->main_plane = n;
        spec[n++] = {kUsageDepth, true, d};
        if (fi.stencil) spec[n++] = {kUsageStencil, true, stencil};
    } else if (fi.depth && fi.stencil) {
        bool ds = (t.bind & kBindDepthStencil) != 0;
        spec[n++] = {ds ? kUsageDepth : kUsageLuma, true, d};
        spec[n++] = {ds ? kUsageStencil : kUsageChroma, true, stencil};
    } else {
        // STUB: xg_x also gives single-mip colour targets a CMASK plane (and
        // FMASK with MSAA) after the colour plane; not modelled yet.
        if (t.dim == 3 && single_mip && (t.bind & kBindRenderTarget))
            FL_STUB_ONCE("xg: colour render target laid out without its CMASK/FMASK planes");
        spec[n++] = {kUsageDefault, true, d};
    }
    c->num_planes = n;
    for (int i = 0; i < n; i++) {
        if (!spec[i].surface) continue;
        c->planes[i].desc = spec[i].desc;
        c->has_surface[i] = true;
        if (!gcn::compute_layout(c->planes[i].desc, &c->planes[i].layout)) return E_INVALIDARG;
    }

    // Offsets as xg_x assigns them (rva 0x2600): a plane's base offset is the
    // previous plane's end, and the running end is aligned to the plane's
    // alignment before adding its size.
    ResourceLayout& L = c->layout;
    L.mip_levels = d.mips;
    L.planes = n;
    uint64_t end = 0;
    for (int i = 0; i < n; i++) {
        PlaneLayout& p = L.plane[i];
        if (spec[i].surface) {
            fill_plane(&p, c->planes[i], spec[i].usage, end, t.tile_mode);
        } else {  // HTILE, sized from the depth surface
            const gcn::Layout& dl = c->planes[c->main_plane].layout;
            p.usage = spec[i].usage;
            p.size_bytes = htile_bytes(dl.mips[0].pitch, dl.mips[0].padded_height, d.array_size);
            p.base_offset_bytes = end;
            p.base_alignment = 0x800;
        }
        end = (end + p.base_alignment - 1) / p.base_alignment * p.base_alignment + p.size_bytes;
        if (p.base_alignment > L.base_alignment) L.base_alignment = p.base_alignment;
    }
    L.size_bytes = end;
    FL_LOG(xg, "layout dim %u %ux%ux%u[%u] mips %u fmt %u tile %u bind 0x%x misc 0x%x -> %d planes, size 0x%llx align 0x%llx",
           t.dim, t.width, t.height, t.depth, t.array_size, d.mips, t.format, t.tile_mode, t.bind, t.misc, n,
           (unsigned long long)L.size_bytes, (unsigned long long)L.base_alignment);
    return S_OK;
}

// ---- computer methods (ms_abi) ----

WINAPI ULONG comp_AddRef(Computer* c) { return ++c->refs; }
WINAPI ULONG comp_Release(Computer* c) {
    int r = --c->refs;
    if (r == 0) delete c;
    return r;
}
WINAPI HRESULT comp_GetResourceLayout(Computer* c, ResourceLayout* out) {
    if (!out) return E_INVALIDARG;
    std::memcpy(out, &c->layout, sizeof(*out));
    return S_OK;
}
// slot 3 (xg 0x2a30): (plane, mip) -> offset, or ~0 for a bad plane.
WINAPI uint64_t comp_GetMipLevelOffsetBytes(Computer* c, UINT plane, UINT mip) {
    if (plane >= static_cast<UINT>(c->num_planes) || mip >= c->layout.mip_levels) return ~0ull;
    return c->layout.plane[plane].mips[mip].offset_bytes;
}
// slot 4 (xg 0x2a70): (plane, mip, x, y, z or slice, sample) -> offset. Game call at rva 0x10c390b.
WINAPI uint64_t comp_GetTexelElementOffsetBytes(Computer* c, UINT plane, UINT mip, uint64_t x, UINT y, UINT z,
                                                UINT sample) {
    if (plane >= static_cast<UINT>(c->num_planes) || mip >= c->layout.mip_levels) return ~0ull;
    if (!c->has_surface[plane]) {
        FL_ERROR("XGTextureAddressComputer: texel offset asked for metadata plane %u (HTILE)", plane);
        return ~0ull;
    }
    const Plane& p = c->planes[plane];
    return c->layout.plane[plane].base_offset_bytes +
           gcn::element_offset(p.desc, p.layout, mip, static_cast<uint32_t>(x), y, z, sample);
}

void** computer_vtable() {
    static void** t = [] {
        auto** v = new void*[16];
        for (int i = 0; i < 16; i++) v[i] = loader::make_named_trap("XGTextureAddressComputer", "slot " + std::to_string(i));
        v[0] = reinterpret_cast<void*>(&comp_AddRef);
        v[1] = reinterpret_cast<void*>(&comp_Release);
        v[2] = reinterpret_cast<void*>(&comp_GetResourceLayout);
        v[3] = reinterpret_cast<void*>(&comp_GetMipLevelOffsetBytes);
        v[4] = reinterpret_cast<void*>(&comp_GetTexelElementOffsetBytes);
        return v;
    }();
    return t;
}

HRESULT create_computer(const TexDesc& t, Computer** out) {
    auto* c = new Computer();
    c->vtbl = computer_vtable();
    HRESULT hr = build(c, t);
    if (hr < 0) {
        delete c;
        *out = nullptr;
        return hr;
    }
    *out = c;
    return S_OK;
}

// ---- descriptor parsing ----

// XG_TEXTURE1D_DESC: W Mips Array Format Usage Bind CPU Misc ESRAM0 ESRAM1 TileMode Pitch
TexDesc from_1d(const uint32_t* x) { return TexDesc{2, x[0], 1, 1, x[2], x[1], x[3], 1, x[10], x[11], x[5], x[7]}; }
// XG_TEXTURE2D_DESC: W H Mips Array Format Samples Quality Usage Bind CPU Misc ESRAM0 ESRAM1 TileMode Pitch
TexDesc from_2d(const uint32_t* x) { return TexDesc{3, x[0], x[1], 1, x[3], x[2], x[4], x[5], x[13], x[14], x[8], x[10]}; }
// XG_TEXTURE3D_DESC: W H D Mips Format Usage Bind CPU Misc ESRAM0 ESRAM1 TileMode Pitch
TexDesc from_3d(const uint32_t* x) { return TexDesc{4, x[0], x[1], x[2], 1, x[3], x[4], 1, x[11], x[12], x[6], x[8]}; }

// ---- exports ----

// Reimplements xg rva 0x12f0 + 0x4140 (FINDINGS §3.2).
WINAPI UINT XGComputeOptimalTileMode_(UINT dim, UINT format, UINT width, UINT height, UINT array_size, UINT samples,
                                      UINT bind, UINT misc) {
    UINT mode = optimal_tile_mode(dim, format, width, height, array_size, samples, bind, misc);
    FL_LOG(xg, "XGComputeOptimalTileMode(dim %u fmt %u %ux%u samples %u bind 0x%x misc 0x%x) -> %d", dim, format,
           width, height, samples, bind, misc, static_cast<int>(mode));
    return mode;
}

// xg rva 0x1350: only the sample count matters.
WINAPI void XGComputeOptimalDepthStencilTileModes_(UINT format, UINT width, UINT height, UINT array_size,
                                                    UINT samples, BOOL, UINT* depth, UINT* stencil) {
    UINT m = samples == 2 || samples == 4 ? 1 : samples == 8 ? 2 : 0;
    if (depth) *depth = m;
    if (stencil) *stencil = m;
}

WINAPI HRESULT XGCreateTexture1DComputer_(const uint32_t* desc, Computer** out) { return create_computer(from_1d(desc), out); }
WINAPI HRESULT XGCreateTexture2DComputer_(const uint32_t* desc, Computer** out) { return create_computer(from_2d(desc), out); }
WINAPI HRESULT XGCreateTexture3DComputer_(const uint32_t* desc, Computer** out) { return create_computer(from_3d(desc), out); }

HRESULT layout_of(const TexDesc& t, ResourceLayout* out) {
    if (!out) return E_INVALIDARG;
    Computer* c = nullptr;
    HRESULT hr = create_computer(t, &c);
    if (hr < 0) return hr;
    std::memcpy(out, &c->layout, sizeof(*out));
    comp_Release(c);
    return S_OK;
}
WINAPI HRESULT XGComputeTexture2DLayout_(const uint32_t* desc, ResourceLayout* out) { return layout_of(from_2d(desc), out); }
WINAPI HRESULT XGComputeTexture3DLayout_(const uint32_t* desc, ResourceLayout* out) { return layout_of(from_3d(desc), out); }

}  // namespace

UINT optimal_tile_mode(UINT dim, UINT format, UINT width, UINT height, UINT array_size, UINT samples, UINT bind,
                       UINT misc) {
    if (dim == 1) return kTileLinearGeneral;   // buffers
    if (bind & 0x40) return ~0u;               // depth: use the DepthStencil variant
    if (dim == 2) return 8;                    // 1D textures: linear aligned
    if (dim == 4) return 13;                   // 3D: 1D tiled thin
    if (dim != 3) return kTileLinearGeneral;
    bool render_target = (bind & 0x20) != 0;
    if (misc & (1u << 27)) return render_target ? 17 : 16;  // tiled-resource (PRT) modes
    uint32_t micro_tile = format_info(format).bytes * 64u;  // bytes of one 8x8 micro tile
    uint32_t v = std::min(std::min(std::max(2 * micro_tile, 256u), 2048u), micro_tile * samples);
    uint32_t log2v = 31 - __builtin_clz(v);
    // Minimum (width, height) for 2D tiling, indexed by log2(v) - 6 (xg rva 0x14b58).
    static const uint32_t kMin[7][2] = {{128, 64}, {64, 64}, {64, 32}, {64, 32}, {64, 32}, {64, 32}, {32, 32}};
    uint32_t i = std::min(log2v - 6, 6u);
    return width >= kMin[i][0] && height >= kMin[i][1] ? 14 : 13;
}

bool layout_for(const TexDesc& t, gcn::SurfaceDesc* d, gcn::Layout* l) {
    Computer* c = nullptr;
    if (create_computer(t, &c) < 0) return false;
    *d = c->planes[c->main_plane].desc;
    *l = c->planes[c->main_plane].layout;
    comp_Release(c);
    return true;
}

HLE_EXPORT(xg_x, XGComputeOptimalTileMode, XGComputeOptimalTileMode_);
HLE_EXPORT(xg_x, XGComputeOptimalDepthStencilTileModes, XGComputeOptimalDepthStencilTileModes_);
HLE_EXPORT(xg_x, XGCreateTexture1DComputer, XGCreateTexture1DComputer_);
HLE_EXPORT(xg_x, XGCreateTexture2DComputer, XGCreateTexture2DComputer_);
HLE_EXPORT(xg_x, XGCreateTexture3DComputer, XGCreateTexture3DComputer_);
HLE_EXPORT(xg_x, XGComputeTexture2DLayout, XGComputeTexture2DLayout_);
HLE_EXPORT(xg_x, XGComputeTexture3DLayout, XGComputeTexture3DLayout_);

}  // namespace fl::xg
