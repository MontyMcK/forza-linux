// Placement resources: buffers and textures the game creates over memory it
// manages itself (device slots 62-65, FINDINGS §3.1). umd copies the Xbox
// descriptor, ORs an internal flag into MiscFlags, creates an ordinary resource
// and binds it to the game's address. We create an ordinary DXVK resource and
// copy the game memory into it when it changed (gpumem.h).
#include <cstring>
#include <sys/mman.h>

#include "common/log.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/d3d11x/gpumem.h"
#include "hle/registry.h"
#include "hle/xg/xg.h"

#include <vector>

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Vtable;
namespace dc = desktop::ctx;

constexpr UINT kGpuWriteBinds = 0x20 | 0x40 | 0x80;  // RENDER_TARGET | DEPTH_STENCIL | UNORDERED_ACCESS

HRESULT create_placement(Iface* self, int dim, const uint32_t* xd, UINT tile_mode, UINT pitch, void* addr,
                         void** out) {
    Resource* r = nullptr;
    HRESULT hr = create_resource(self->as<DeviceX>(), dim, xd, nullptr, out, true, &r);
    if (hr < 0 || !r) return hr;
    r->place = static_cast<uint8_t*>(addr);
    r->tile_mode = tile_mode;
    r->pitch = pitch;
    UINT bind = xd[dim == 1 ? 2 : dim == 2 ? 5 : dim == 3 ? 8 : 6];
    r->gpu_owned = (bind & kGpuWriteBinds) != 0;
    if (dim == 1) {
        r->place_size = xd[0];  // ByteWidth
        if (!r->gpu_owned) gpumem::track(r->place, r->place_size);
    } else {
        // The same layout our xg_x hands the game, so texels are where it put them.
        xg::TexDesc t{};
        t.dim = dim, t.tile_mode = tile_mode, t.pitch = pitch, t.samples = 1, t.depth = 1, t.height = 1;
        if (dim == 2) t.width = xd[0], t.mips = xd[1], t.array_size = xd[2], t.format = xd[3];
        if (dim == 3) t.width = xd[0], t.height = xd[1], t.mips = xd[2], t.array_size = xd[3], t.format = xd[4],
                      t.samples = xd[5];
        if (dim == 4) t.width = xd[0], t.height = xd[1], t.depth = xd[2], t.mips = xd[3], t.format = xd[4],
                      t.array_size = 1;
        if (!xg::layout_for(t, &r->tex_desc, &r->tex_layout))
            fatal("d3d: placement texture layout failed (dim %d fmt %u tile %u)", dim, t.format, tile_mode);
        r->place_size = r->tex_layout.size;
        if (!r->gpu_owned) gpumem::track(r->place, r->place_size);
    }
    FL_LOG(d3d, "placement dim %d at %p size 0x%zx tile %u pitch %u bind 0x%x", dim, addr, r->place_size, tile_mode,
           pitch, bind);
    return hr;
}

// Xbox device slot 62 (umd 0x2b340): (desc, address, out). Calls CreateBuffer internally.
WINAPI HRESULT dev_CreatePlacementBuffer(Iface* self, const uint32_t* xd, void* addr, void** out) {
    return create_placement(self, 1, xd, 0, 0, addr, out);
}
// Xbox device slots 63/64/65 (umd 0x2d650 / 0x2d720 / 0x2d880):
// (desc, tile mode index, pitch, address, out). umd packs (tile << 24 | pitch)
// into the descriptor's first Xbox field and calls CreateTexture1D/2D/3D.
WINAPI HRESULT dev_CreatePlacementTexture1D(Iface* self, const uint32_t* xd, UINT tile, UINT pitch, void* addr,
                                            void** out) {
    return create_placement(self, 2, xd, tile, pitch, addr, out);
}
WINAPI HRESULT dev_CreatePlacementTexture2D(Iface* self, const uint32_t* xd, UINT tile, UINT pitch, void* addr,
                                            void** out) {
    return create_placement(self, 3, xd, tile, pitch, addr, out);
}
WINAPI HRESULT dev_CreatePlacementTexture3D(Iface* self, const uint32_t* xd, UINT tile, UINT pitch, void* addr,
                                            void** out) {
    return create_placement(self, 4, xd, tile, pitch, addr, out);
}

// d3d11_x!D3DMapEsramMemory(flags, address, page count, page list): maps 64 KB
// ESRAM pages into a range the game reserved (game call at rva 0x10c5a9d:
// flags 1, consecutive page indices). ESRAM is ordinary memory for us, so the
// range just becomes readable and writable. Which pages (and aliasing between
// mappings of the same pages) is ignored: GPU resources live in DXVK anyway.
WINAPI HRESULT D3DMapEsramMemory_(UINT flags, void* addr, UINT pages, const UINT* page_list) {
    constexpr size_t kEsramPage = 0x10000;
    FL_LOG(d3d, "D3DMapEsramMemory(flags 0x%x, %p, %u pages, first %u)", flags, addr, pages, page_list ? page_list[0] : 0);
    if (mprotect(addr, size_t(pages) * kEsramPage, PROT_READ | PROT_WRITE) != 0) {
        FL_ERROR("d3d: D3DMapEsramMemory: mprotect(%p, %u pages) failed", addr, pages);
        return E_INVALIDARG;
    }
    return S_OK;
}

}  // namespace

HLE_EXPORT(d3d11_x, D3DMapEsramMemory, D3DMapEsramMemory_);

void sync_placement(void* dxvk_ctx, Resource* r) {
    if (r->gpu_owned || !r->place_size) return;
    if (!gpumem::dirty(r->place, r->place_size)) return;
    gpumem::begin_upload(r->place, r->place_size);
    if (r->dim == 1) {
        dx<void>(dxvk_ctx, dc::UpdateSubresource, r->dxvk, 0u, nullptr, static_cast<const void*>(r->place), 0u, 0u);
        return;
    }
    // Textures: untile every mip/slice from game memory into DXVK.
    const gcn::SurfaceDesc& d = r->tex_desc;
    const gcn::Layout& l = r->tex_layout;
    uint32_t mips = static_cast<uint32_t>(l.mips.size());
    std::vector<uint8_t> linear;
    for (uint32_t mip = 0; mip < mips; mip++) {
        const gcn::MipLayout& m = l.mips[mip];
        size_t row = size_t(m.width) * d.bytes_per_element;
        size_t slice = row * m.height;
        if (r->dim == 4) {
            linear.resize(slice * m.depth);
            for (uint32_t z = 0; z < m.depth; z++) gcn::untile(d, l, mip, z, r->place, linear.data() + z * slice, row);
            dx<void>(dxvk_ctx, dc::UpdateSubresource, r->dxvk, mip, nullptr, static_cast<const void*>(linear.data()),
                     static_cast<UINT>(row), static_cast<UINT>(slice));
            continue;
        }
        linear.resize(slice);
        for (uint32_t a = 0; a < d.array_size; a++) {
            gcn::untile(d, l, mip, a, r->place, linear.data(), row);
            dx<void>(dxvk_ctx, dc::UpdateSubresource, r->dxvk, mip + a * mips, nullptr,
                     static_cast<const void*>(linear.data()), static_cast<UINT>(row), static_cast<UINT>(slice));
        }
    }
}

void add_placement_methods(Vtable& v) {
    v.set(62, &dev_CreatePlacementBuffer)
        .set(63, &dev_CreatePlacementTexture1D)
        .set(64, &dev_CreatePlacementTexture2D)
        .set(65, &dev_CreatePlacementTexture3D);
}

}  // namespace fl::d3d
