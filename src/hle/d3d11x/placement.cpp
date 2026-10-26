// Placement resources: buffers and textures the game creates over memory it
// manages itself (device slots 62-65, FINDINGS §3.1). umd copies the Xbox
// descriptor, ORs an internal flag into MiscFlags, creates an ordinary resource
// and binds it to the game's address. We create an ordinary DXVK resource and
// copy the game memory into it when it changed (gpumem.h).
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <sys/mman.h>

#include "common/log.h"
#include "hle/d3d11x/trace.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/d3d11x/gpumem.h"
#include "hle/registry.h"
#include "hle/xg/xg.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Vtable;
namespace dc = desktop::ctx;

constexpr UINT kGpuWriteBinds = 0x20 | 0x40 | 0x80;  // RENDER_TARGET | DEPTH_STENCIL | UNORDERED_ACCESS

// Live placement resources by address. On the console two placement
// resources over the same memory ARE the same memory; the game creates the
// 4x MSAA depth target twice at one address (one object to render depth into,
// one to read it back in compute, game rva 0x10d8... at boot). Separate DXVK
// textures would never see each other's contents, so an identical
// descriptor at an address that already has a live resource gets that
// resource (another reference) instead of a new copy.
std::mutex g_placed_mutex;
std::unordered_map<uintptr_t, Resource*> g_placed;

// CPU-written placement resources by 4 KiB page: write tracking is per page,
// and small resources (constant buffers) share pages. Guarded by g_placed_mutex.
std::unordered_map<uint64_t, std::vector<Resource*>> g_page_users;

template <typename F>
void for_pages(const Resource* r, F f) {
    uint64_t a = reinterpret_cast<uint64_t>(r->place);
    for (uint64_t p = a >> 12, e = (a + (r->place_size ? r->place_size : 1) - 1) >> 12; p <= e; p++) f(p);
}

void page_index_add(Resource* r) {
    std::lock_guard lock(g_placed_mutex);
    for_pages(r, [&](uint64_t p) { g_page_users[p].push_back(r); });
}

void page_index_remove(Resource* r) {
    for_pages(r, [&](uint64_t p) {
        auto it = g_page_users.find(p);
        if (it == g_page_users.end()) return;
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), r), v.end());
        if (v.empty()) g_page_users.erase(it);
    });
}

// begin_upload callback: pages [start, end) were written and are re-armed now;
// every other resource on them still has to upload those writes.
void mark_page_neighbours(uint64_t start, uint64_t end, void* self) {
    std::lock_guard lock(g_placed_mutex);
    for (uint64_t p = start >> 12; p < (end + 0xfff) >> 12; p++) {
        auto it = g_page_users.find(p);
        if (it == g_page_users.end()) continue;
        for (Resource* o : it->second)
            if (o != self) o->force_dirty.store(true, std::memory_order_relaxed);
    }
}

Resource* find_identical(int dim, const uint32_t* xd, size_t xd_size, UINT tile_mode, UINT pitch, void* addr) {
    std::lock_guard lock(g_placed_mutex);
    auto it = g_placed.find(reinterpret_cast<uintptr_t>(addr));
    if (it == g_placed.end()) return nullptr;
    Resource* r = it->second;
    if (r->magic != DeviceChild::kMagic || r->dim != dim || r->tile_mode != tile_mode || r->pitch != pitch ||
        r->xdesc_size != xd_size || std::memcmp(r->xdesc, xd, xd_size) != 0)
        return nullptr;
    r->add_ref();
    return r;
}

HRESULT create_placement(Iface* self, int dim, const uint32_t* xd, UINT tile_mode, UINT pitch, void* addr,
                         void** out) {
    size_t xd_size = dim == 1 ? 0x20 : dim == 2 ? 0x28 : dim == 3 ? 0x34 : 0x2c;  // Xbox descriptor sizes (resources.cpp)
    if (Resource* same = out ? find_identical(dim, xd, xd_size, tile_mode, pitch, addr) : nullptr) {
        FL_LOG(d3d, "placement dim %d at %p: same descriptor as live %p, sharing it", dim, addr, (void*)same);
        *out = &same->iface;
        return S_OK;
    }
    Resource* r = nullptr;
    HRESULT hr = create_resource(self->as<DeviceX>(), dim, xd, nullptr, out, true, &r);
    if (hr < 0 || !r) return hr;
    r->place = static_cast<uint8_t*>(addr);
    r->tile_mode = tile_mode;
    r->pitch = pitch;
    UINT bind = xd[dim == 1 ? 2 : dim == 2 ? 5 : dim == 3 ? 8 : 6];
    r->gpu_owned = (bind & kGpuWriteBinds) != 0;
    if (dim == 1) {
        r->place_size = r->place_total = xd[0];  // ByteWidth
        if (!r->gpu_owned) gpumem::track(r->place, r->place_size), page_index_add(r);
    } else {
        // The same layout our xg_x hands the game, so texels are where it put them.
        xg::TexDesc t{};
        t.dim = dim, t.tile_mode = tile_mode, t.pitch = pitch, t.samples = 1, t.depth = 1, t.height = 1;
        t.bind = bind, t.misc = xd[dim == 2 ? 7 : dim == 3 ? 10 : 8];
        if (dim == 2) t.width = xd[0], t.mips = xd[1], t.array_size = xd[2], t.format = xd[3];
        if (dim == 3) t.width = xd[0], t.height = xd[1], t.mips = xd[2], t.array_size = xd[3], t.format = xd[4],
                      t.samples = xd[5];
        if (dim == 4) t.width = xd[0], t.height = xd[1], t.depth = xd[2], t.mips = xd[3], t.format = xd[4],
                      t.array_size = 1;
        if (!xg::layout_for(t, &r->tex_desc, &r->tex_layout))
            fatal("d3d: placement texture layout failed (dim %d fmt %u tile %u)", dim, t.format, tile_mode);
        r->place_size = r->tex_layout.size;
        if (!xg::plane_spans(t, r->planes, &r->num_planes, &r->place_total)) r->place_total = r->place_size;
        if (!r->gpu_owned) gpumem::track(r->place, r->place_size), page_index_add(r);
    }
    FL_LOG(d3d, "placement dim %d at %p size 0x%zx tile %u pitch %u bind 0x%x", dim, addr, r->place_size, tile_mode,
           pitch, bind);
    {
        std::lock_guard lock(g_placed_mutex);
        // FL_LOG=alias: placement resources over the same memory with different
        // descriptors are separate DXVK resources here, so GPU writes through
        // one never show in the other.
        if (log_enabled("alias")) {
            uintptr_t a = reinterpret_cast<uintptr_t>(addr), e = a + r->place_size;
            for (auto& [oa, o] : g_placed) {
                if (o == r || oa >= e || oa + o->place_size <= a) continue;
                if (!o->gpu_owned && !r->gpu_owned) continue;
                std::string mine = trace::describe_dxvk(r->dxvk);
                FL_LOG(alias, "[%s] at %p+0x%zx overlaps live [%s] at %p+0x%zx", mine.c_str(), addr, r->place_size,
                       trace::describe_dxvk(o->dxvk), (void*)oa, o->place_size);
            }
        }
        g_placed[reinterpret_cast<uintptr_t>(addr)] = r;  // the newest resource at an address owns it
    }
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
    // FL_NO_PLACEMENT_UPLOAD=tex|all: bisect aid, leaves textures (or all
    // placement resources) un-uploaded.
    static const char* skip = getenv("FL_NO_PLACEMENT_UPLOAD");
    if (skip && (r->dim != 1 || skip[0] == 'a')) return;
    // FL_CHECK_TRACKING=1 (diagnostic): verify "clean" answers against a hash
    // of what we uploaded last time.
    static const bool check = getenv("FL_CHECK_TRACKING") != nullptr;
    auto hash = [](const uint8_t* p, size_t n) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
        return h;
    };
    bool forced = r->force_dirty.exchange(false, std::memory_order_relaxed);
    if (!forced && !gpumem::dirty(r->place, r->place_size)) {
        if (check && r->upload_hash && r->place_size <= (1u << 20) && gpumem::readable(r->place, r->place_size) &&
            hash(r->place, r->place_size) != r->upload_hash) {
            static std::atomic<int> reported{0};
            if (reported++ < 20)
                FL_ERROR("gpumem: MISSED write: [%s] at %p size 0x%zx changed but reads clean", trace::describe_dxvk(r->dxvk),
                         static_cast<void*>(r->place), r->place_size);
        }
        return;
    }
    if (!gpumem::mapped(r->place, r->place_size)) {
        // The game released this memory and still binds the resource (Azimuth
        // reports the same). Keep the last contents.
        log_once("placement resource bound over released memory (not uploaded)", 0);
        return;
    }
    gpumem::begin_upload(r->place, r->place_size, &mark_page_neighbours, r);
    if (check && r->place_size <= (1u << 20) && gpumem::readable(r->place, r->place_size))
        r->upload_hash = hash(r->place, r->place_size);
    FL_LOG(upload, "placement upload [%s] dim %d at %p size 0x%zx tile %u", trace::describe_dxvk(r->dxvk), r->dim,
           static_cast<void*>(r->place), r->place_size, r->tile_mode);
    if (trace::active()) trace::event("upload placement [%s]", trace::describe_dxvk(r->dxvk));
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
        if (!gpumem::readable(r->place + m.offset, m.size)) {
            // Not committed yet: a streamed texture whose big mips have not
            // arrived (the game clamps its LOD meanwhile). Uploaded once the
            // game commits and writes them (the write marks them dirty).
            FL_LOG(upload, "  mip %u at %p+0x%llx not committed: skipped", mip, static_cast<void*>(r->place + m.offset),
                   (unsigned long long)m.size);
            continue;
        }
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

Resource* placement_containing(uint64_t addr) {
    std::lock_guard lock(g_placed_mutex);
    for (auto& [a, r] : g_placed)
        if (addr >= a && addr - a < std::max(r->place_total, uint64_t(r->place_size))) {
            r->add_ref();
            return r;
        }
    return nullptr;
}

void placement_forget(Resource* r) {
    std::lock_guard lock(g_placed_mutex);
    if (!r->gpu_owned) page_index_remove(r);
    auto it = g_placed.find(reinterpret_cast<uintptr_t>(r->place));
    if (it != g_placed.end() && it->second == r) g_placed.erase(it);
}

void add_placement_methods(Vtable& v) {
    v.set(62, &dev_CreatePlacementBuffer)
        .set(63, &dev_CreatePlacementTexture1D)
        .set(64, &dev_CreatePlacementTexture2D)
        .set(65, &dev_CreatePlacementTexture3D);
}

}  // namespace fl::d3d
