// Placement resources: buffers and textures the game creates over memory it
// manages itself (device slots 62-65, FINDINGS §3.1). umd copies the Xbox
// descriptor, ORs an internal flag into MiscFlags, creates an ordinary resource
// and binds it to the game's address. We create an ordinary DXVK resource and
// copy the game memory into it when it changed (gpumem.h).
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <ctime>
#include <string>

#include "common/log.h"
#include "hle/d3d11x/trace.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/d3d11x/gpumem.h"
#include "hle/registry.h"
#include "hle/xg/xg.h"
#include "platform/os/clock.h"
#include "platform/os/memory.h"
#include "platform/os/write_watch.h"

#include <map>
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
// GPU-owned placements (render and depth targets) by address, for
// placement_containing: the DMA engine's fills look them up by address, and a
// scan of every placement per fill took a fifth of the CPU time.
std::map<uintptr_t, Resource*> g_gpu_owned;
uint64_t g_gpu_owned_max = 0;  // largest place_total among them (bounds the search)

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

// Buffers smaller than this are compared with their last upload instead of
// watched for writes (os::write_watch_compare_below); FL_COMPARE_BELOW=<bytes>
// sets it, e.g. to test compare mode on Linux.
size_t compare_below() {
    static const size_t n = getenv("FL_COMPARE_BELOW") ? strtoull(getenv("FL_COMPARE_BELOW"), nullptr, 0)
                                                        : os::write_watch_compare_below();
    return n;
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
        r->compare = !r->gpu_owned && r->place_size < compare_below();
        if (!r->gpu_owned) {
            if (!r->compare) gpumem::track(r->place, r->place_size);
            page_index_add(r);  // compared ones too: the remap hook finds them through it
        }
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
        if (r->gpu_owned) {
            g_gpu_owned[reinterpret_cast<uintptr_t>(addr)] = r;
            g_gpu_owned_max = std::max(g_gpu_owned_max, std::max(r->place_total, uint64_t(r->place_size)));
        }
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
    if (!os::commit(addr, size_t(pages) * kEsramPage, os::kRead | os::kWrite)) {
        FL_ERROR("d3d: D3DMapEsramMemory: mprotect(%p, %u pages) failed", addr, pages);
        return E_INVALIDARG;
    }
    return S_OK;
}

}  // namespace

HLE_EXPORT(d3d11_x, D3DMapEsramMemory, D3DMapEsramMemory_);

// FL_LOG=fps: where placement syncing spends its time (see placement_stats).
namespace {
std::atomic<uint64_t> g_sync_calls{0}, g_scan_ns{0}, g_uploads{0}, g_upload_bytes{0}, g_upload_ns{0};
std::atomic<uint64_t> g_bytes_scanned{0}, g_cold_skips{0}, g_frame_scan_ns{0}, g_frame_spans{0}, g_frame_pages{0},
    g_frame_cb_ns{0}, g_rearm_ns{0}, g_readable_ns{0}, g_untile_ns{0}, g_update_ns{0};

// The frame scan. Per-bind page scans were ~15000 per frame (each resource
// ~10 times: every command list re-syncs its placements), ~5 ms of a 33 ms
// frame, and frames late by that much were the steady 39 ms stutter. Now each
// frame re-arms one of kScanParts slices of all tracked memory (walking all of
// it took ~3 ms) and marks every resource on a written page dirty and "hot".
// Hot resources (written in the last kColdFrames frames: constants, dynamic
// geometry) keep the exact per-bind scan; cold ones (static textures and
// meshes) trust the frame scan, so a write to them reaches the GPU at most
// kScanParts frames late (streamed mips, rare rewrites).
// FL_NO_FRAME_SCAN=1: per-bind scans for everything (the old behaviour).
// FL_SCAN_PARTS=n overrides the slice count (1: everything every frame).
const uint32_t kScanParts = [] {
    const char* e = getenv("FL_SCAN_PARTS");
    int n = e ? atoi(e) : 4;
    return static_cast<uint32_t>(n < 1 ? 1 : n);
}();
const uint32_t kColdFrames = 2 * kScanParts + 2;
std::atomic<uint32_t> g_epoch{1};

// FL_LOG=latewrite (diagnostic): copies of the small buffers uploaded this
// frame; at Present, any whose game memory changed since is reported (the
// console GPU would have read the newer bytes).
struct LateCheck {
    Resource* r;
    uint8_t* place;
    std::vector<uint8_t> bytes;
};
std::mutex g_late_mutex;
std::vector<LateCheck> g_late;

void late_remember(Resource* r) {
    if (r->dim != 1 || r->place_size > 65536 || !gpumem::readable(r->place, r->place_size)) return;
    r->add_ref();
    std::lock_guard lock(g_late_mutex);
    g_late.push_back({r, r->place, std::vector<uint8_t>(r->place, r->place + r->place_size)});
}

void late_check(uint32_t epoch) {
    std::vector<LateCheck> v;
    {
        std::lock_guard lock(g_late_mutex);
        v.swap(g_late);
    }
    static int logged = 0;
    size_t changed = 0;
    for (auto& c : v) {
        size_t n = c.bytes.size();
        if (c.r->place == c.place && gpumem::readable(c.place, n) && std::memcmp(c.place, c.bytes.data(), n) != 0) {
            changed++;
            size_t first = 0, last = n;
            while (first < n && c.place[first] == c.bytes[first]) first++;
            while (last > first && c.place[last - 1] == c.bytes[last - 1]) last--;
            static const uint32_t from = getenv("FL_LATE_FROM") ? atoi(getenv("FL_LATE_FROM")) : 0;
            if (epoch >= from && logged < 400) {
                logged++;
                FL_LOG(latewrite, "epoch %u late write: [%s] %zu B at %p, bytes [%zu, %zu) changed", epoch,
                       trace::describe_dxvk(c.r->dxvk), n, static_cast<void*>(c.place), first, last);
            }
        }
        c.r->release();
    }
    FL_LOG(latewrite, "epoch %u: %zu of %zu small buffers changed after upload", epoch, changed, v.size());
}
bool frame_scan_on() {
    static const bool on = getenv("FL_NO_FRAME_SCAN") == nullptr;
    return on;
}
int64_t now_ns() { return static_cast<int64_t>(os::monotonic_ns()); }
struct Span {  // adds the time until it goes out of scope to a counter (FL_LOG=fps only)
    std::atomic<uint64_t>* c;
    int64_t t0;
    Span(std::atomic<uint64_t>* c, bool on) : c(on ? c : nullptr), t0(on ? now_ns() : 0) {}
    void stop() {
        if (c) c->fetch_add(now_ns() - t0, std::memory_order_relaxed);
        c = nullptr;
    }
    ~Span() { stop(); }
};
}  // namespace

std::string placement_stats() {
    char buf[512];
    snprintf(buf, sizeof buf, "placement: %llu syncs (%llu cold, %.1f ms scanning %.0f MB), frame scans %.1f ms "
             "(%llu calls, %llu written pages, %.1f ms marking), %llu uploads (%.1f MB, %.1f ms)",
             (unsigned long long)g_sync_calls.exchange(0), (unsigned long long)g_cold_skips.exchange(0),
             g_scan_ns.exchange(0) / 1e6, g_bytes_scanned.exchange(0) / 1048576.0, g_frame_scan_ns.exchange(0) / 1e6,
             (unsigned long long)g_frame_spans.exchange(0), (unsigned long long)g_frame_pages.exchange(0),
             g_frame_cb_ns.exchange(0) / 1e6,
             (unsigned long long)g_uploads.exchange(0), g_upload_bytes.exchange(0) / 1048576.0,
             g_upload_ns.exchange(0) / 1e6);
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof buf - n, " = re-arm %.1f + readable %.1f + untile %.1f + UpdateSubresource %.1f ms",
             g_rearm_ns.exchange(0) / 1e6, g_readable_ns.exchange(0) / 1e6, g_untile_ns.exchange(0) / 1e6,
             g_update_ns.exchange(0) / 1e6);
    return buf;
}

// A compared buffer (Resource::compare): uploaded when its memory differs
// from the last upload, from the copy it was compared with.
void sync_compared(void* dxvk_ctx, Resource* r, bool stats) {
    int64_t t0 = stats ? now_ns() : 0;
    auto guard = gpumem::read_guard();
    if (r->remapped.exchange(false, std::memory_order_relaxed) && !gpumem::mapped(r->place, r->place_size)) {
        r->remapped.store(true, std::memory_order_relaxed);  // check again next time
        log_once("placement resource bound over released memory (not uploaded)", 0);
        return;
    }
    std::lock_guard lock(r->shadow_mutex);
    bool forced = r->force_dirty.exchange(false, std::memory_order_relaxed);
    bool same = !forced && r->shadow.size() == r->place_size && !memcmp(r->shadow.data(), r->place, r->place_size);
    if (stats) {
        g_bytes_scanned.fetch_add(r->place_size, std::memory_order_relaxed);
        g_scan_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    }
    if (same) return;
    r->shadow.assign(r->place, r->place + r->place_size);
    g_uploads.fetch_add(1, std::memory_order_relaxed);
    g_upload_bytes.fetch_add(r->place_size, std::memory_order_relaxed);
    Span t(&g_update_ns, stats);
    dx<void>(dxvk_ctx, dc::UpdateSubresource, r->dxvk, 0u, nullptr, static_cast<const void*>(r->shadow.data()), 0u, 0u);
}

void sync_placement(void* dxvk_ctx, Resource* r) {
    if (r->gpu_owned || !r->place_size) return;
    static const bool stats = log_enabled("fps");
    g_sync_calls.fetch_add(1, std::memory_order_relaxed);
    int64_t t0 = stats ? now_ns() : 0;
    struct UploadTimer {
        bool on;
        int64_t t0;
        bool uploading = false;
        ~UploadTimer() {
            if (on && uploading) g_upload_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
        }
    } timer{stats, 0};
    // FL_NO_PLACEMENT_UPLOAD=tex|all: bisect aid, leaves textures (or all
    // placement resources) un-uploaded.
    static const char* skip = getenv("FL_NO_PLACEMENT_UPLOAD");
    if (skip && (r->dim != 1 || skip[0] == 'a')) return;
    if (r->compare) return sync_compared(dxvk_ctx, r, stats);
    // FL_CHECK_TRACKING=1 (diagnostic): verify "clean" answers against a hash
    // of what we uploaded last time.
    static const bool check = getenv("FL_CHECK_TRACKING") != nullptr;
    auto hash = [](const uint8_t* p, size_t n) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
        return h;
    };
    bool forced = r->force_dirty.exchange(false, std::memory_order_relaxed);
    uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
    uint32_t written = r->written_epoch.load(std::memory_order_relaxed);
    bool cold = !forced && frame_scan_on() && written && epoch - written > kColdFrames &&
                !r->untracked.load(std::memory_order_relaxed);
    bool is_dirty = forced || (!cold && gpumem::dirty(r->place, r->place_size));
    if (cold) g_cold_skips.fetch_add(1, std::memory_order_relaxed);
    else if (stats) g_bytes_scanned.fetch_add(r->place_size, std::memory_order_relaxed);
    if (is_dirty) r->written_epoch.store(epoch, std::memory_order_relaxed);
    if (stats) {
        int64_t t1 = now_ns();
        g_scan_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
        timer.t0 = t1;
    }
    if (!is_dirty) {
        // FL_CHECK_TRACKING=buf: buffers up to 64 KiB only (fast enough to play);
        // FL_LATE_FROM=epoch: report from that frame on.
        static const bool buf_only = check && getenv("FL_CHECK_TRACKING")[0] == 'b';
        static const uint32_t from = getenv("FL_LATE_FROM") ? atoi(getenv("FL_LATE_FROM")) : 0;
        size_t limit = buf_only ? 65536 : 64u << 20;
        if (check && epoch >= from && r->upload_hash && (!buf_only || r->dim == 1) && r->place_size <= limit &&
            gpumem::readable(r->place, r->place_size) && hash(r->place, r->place_size) != r->upload_hash) {
            static std::atomic<int> reported{0};
            if (reported++ < 200)
                FL_ERROR("gpumem: MISSED write (epoch %u, %s, written %u): [%s] at %p size 0x%zx changed but reads clean",
                         epoch, cold ? "cold" : "scanned", written, trace::describe_dxvk(r->dxvk),
                         static_cast<void*>(r->place), r->place_size);
            r->upload_hash = hash(r->place, r->place_size);  // report each change once
        }
        return;
    }
    // From here on game memory is read: it must not be unmapped meanwhile.
    auto guard = gpumem::read_guard();
    if (!gpumem::mapped(r->place, r->place_size)) {
        // The game released this memory and still binds the resource (Azimuth
        // reports the same). Keep the last contents.
        log_once("placement resource bound over released memory (not uploaded)", 0);
        return;
    }
    timer.uploading = true;
    g_uploads.fetch_add(1, std::memory_order_relaxed);
    g_upload_bytes.fetch_add(r->place_size, std::memory_order_relaxed);
    {
        Span t(&g_rearm_ns, stats);
        r->untracked.store(!gpumem::begin_upload(r->place, r->place_size, &mark_page_neighbours, r),
                           std::memory_order_relaxed);
    }
    if (check && r->place_size <= (64u << 20) && gpumem::readable(r->place, r->place_size))
        r->upload_hash = hash(r->place, r->place_size);
    static const bool late = log_enabled("latewrite");
    if (late) late_remember(r);
    FL_LOG(upload, "placement upload [%s] dim %d at %p size 0x%zx tile %u", trace::describe_dxvk(r->dxvk), r->dim,
           static_cast<void*>(r->place), r->place_size, r->tile_mode);
    if (trace::active()) trace::event("upload placement [%s]", trace::describe_dxvk(r->dxvk));
    if (r->dim == 1) {
        Span t(&g_update_ns, stats);
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
        Span readable_time(&g_readable_ns, stats);
        bool ok = gpumem::readable(r->place + m.offset, m.size);
        readable_time.stop();
        if (!ok) {
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
            Span ut(&g_untile_ns, stats);
            for (uint32_t z = 0; z < m.depth; z++) gcn::untile(d, l, mip, z, r->place, linear.data() + z * slice, row);
            ut.stop();
            Span up(&g_update_ns, stats);
            dx<void>(dxvk_ctx, dc::UpdateSubresource, r->dxvk, mip, nullptr, static_cast<const void*>(linear.data()),
                     static_cast<UINT>(row), static_cast<UINT>(slice));
            continue;
        }
        linear.resize(slice);
        for (uint32_t a = 0; a < d.array_size; a++) {
            Span ut(&g_untile_ns, stats);
            gcn::untile(d, l, mip, a, r->place, linear.data(), row);
            ut.stop();
            Span up(&g_update_ns, stats);
            dx<void>(dxvk_ctx, dc::UpdateSubresource, r->dxvk, mip + a * mips, nullptr,
                     static_cast<const void*>(linear.data()), static_cast<UINT>(row), static_cast<UINT>(slice));
        }
    }
}

// gpumem remap hook: memory under these pages was decommitted/released and
// mapped afresh (untracked): every resource over it uploads on its next sync.
void on_memory_remapped(uint64_t start, uint64_t end) {
    uint32_t e = g_epoch.load(std::memory_order_relaxed);
    std::lock_guard lock(g_placed_mutex);
    if (g_page_users.empty()) return;
    for (uint64_t p = start >> 12; p < end >> 12; p++) {
        auto it = g_page_users.find(p);
        if (it == g_page_users.end()) continue;
        for (Resource* o : it->second) {
            o->force_dirty.store(true, std::memory_order_relaxed);
            o->remapped.store(true, std::memory_order_relaxed);
            o->written_epoch.store(e, std::memory_order_relaxed);
        }
    }
}

void placement_frame_scan() {
    static const bool hooked = (gpumem::set_remap_hook(&on_memory_remapped), true);
    (void)hooked;
    static const bool late = log_enabled("latewrite");
    if (late) late_check(g_epoch.load(std::memory_order_relaxed));
    if (!frame_scan_on()) return;
    static const bool stats = log_enabled("fps");
    int64_t t0 = stats ? now_ns() : 0;
    uint32_t epoch = g_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t spans = gpumem::rearm_all(
        [](uint64_t start, uint64_t end, void* user) {
            uint32_t e = *static_cast<uint32_t*>(user);
            int64_t c0 = now_ns();
            g_frame_pages.fetch_add((end - start) >> 12, std::memory_order_relaxed);
            struct Done {
                int64_t c0;
                ~Done() { g_frame_cb_ns.fetch_add(now_ns() - c0, std::memory_order_relaxed); }
            } done{c0};
            std::lock_guard lock(g_placed_mutex);
            for (uint64_t p = start >> 12; p < (end + 0xfff) >> 12; p++) {
                auto it = g_page_users.find(p);
                if (it == g_page_users.end()) continue;
                for (Resource* o : it->second) {
                    o->force_dirty.store(true, std::memory_order_relaxed);
                    o->written_epoch.store(e, std::memory_order_relaxed);
                }
            }
        },
        &epoch, epoch % kScanParts, kScanParts);
    if (stats) {
        g_frame_scan_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
        g_frame_spans.fetch_add(spans, std::memory_order_relaxed);
    }
}

Resource* placement_containing(uint64_t addr) {
    std::lock_guard lock(g_placed_mutex);
    // Walk down from the last target starting at or below addr, as far as the
    // largest target could reach.
    for (auto it = g_gpu_owned.upper_bound(addr); it != g_gpu_owned.begin();) {
        --it;
        if (addr - it->first >= g_gpu_owned_max) break;
        Resource* r = it->second;
        if (addr - it->first < std::max(r->place_total, uint64_t(r->place_size))) {
            r->add_ref();
            return r;
        }
    }
    return nullptr;
}

void placement_forget(Resource* r) {
    std::lock_guard lock(g_placed_mutex);
    if (!r->gpu_owned) page_index_remove(r);
    auto it = g_placed.find(reinterpret_cast<uintptr_t>(r->place));
    if (it != g_placed.end() && it->second == r) g_placed.erase(it);
    auto g = g_gpu_owned.find(reinterpret_cast<uintptr_t>(r->place));
    if (g != g_gpu_owned.end() && g->second == r) g_gpu_owned.erase(g);
}

void add_placement_methods(Vtable& v) {
    v.set(62, &dev_CreatePlacementBuffer)
        .set(63, &dev_CreatePlacementTexture1D)
        .set(64, &dev_CreatePlacementTexture2D)
        .set(65, &dev_CreatePlacementTexture3D);
}

}  // namespace fl::d3d
