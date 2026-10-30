// The immediate context: ID3D11DeviceContextX with the Xbox block layout.
//
// Slot map (FINDINGS §3.1): Xbox = desktop + 1 for desktop 7..23, desktop 24
// (IASetPrimitiveTopology) has no slot, Xbox = desktop for 25..143, and
// 144..260 are Xbox-only. ctx_slot() converts. Signatures are desktop unless a
// comment says otherwise.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "hle/d3d11x/gpu_profile.h"
#include "hle/d3d11x/trace.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "loader/imports.h"
#include <sys/syscall.h>
#include <dlfcn.h>
#include <mutex>
#include <map>
#include <atomic>
#include <unistd.h>
#include "loader/pe_image.h"
#include "platform/os/thread.h"

namespace fl::d3d {
namespace {

namespace dc = desktop::ctx;
using C = ContextX;

// FL_LOG=ctxthreads: log when a context is used by a different thread than
// its previous call (D3D11 contexts are single-threaded). Diagnostic only.
__attribute__((noinline)) void note_context_user(ContextX* c) {
    static std::mutex m;
    static std::map<ContextX*, pid_t> last;
    static thread_local pid_t tid = static_cast<pid_t>(os::thread_id());
    void* ra = __builtin_return_address(0);
    std::lock_guard lock(m);
    pid_t& prev = last[c];
    if (prev != tid) {
        if (prev) FL_LOG(ctxthreads, "%s context %p: thread %d after thread %d (in %p)", c->deferred ? "deferred" : "immediate", c, tid, prev, ra);
        prev = tid;
    }
}

// The context whose method runs on this thread (bind_unwrap records
// placement binds of deferred contexts through it).
thread_local C* t_current = nullptr;

__attribute__((always_inline)) inline void* ctxdx(void* self) {
    C* c = C::from(self);
    t_current = c;
    static const bool check = log_enabled("ctxthreads");
    if (check) note_context_user(c);
    return c->dxvk;
}

ULONG add_ref(C* c) { return c->deferred ? ++c->refs : c->device->add_ref(); }

// FL_LOG=fps counters (context_stats).
std::atomic<uint64_t> g_ecls{0}, g_ecl_syncs{0}, g_bind_syncs{0};

// Game object -> DXVK object for a bind: also brings placement memory up to date.
void* bind_unwrap(void* ctx, const void* obj) {
    DeviceChild* c = child_of(obj);
    if (!c) return nullptr;
    if (C* cur = t_current; cur && cur->deferred && cur->dxvk == ctx) {
        // Recording: an upload here would be baked into the command list and
        // replayed with stale data on every execution. Note the resource; the
        // executing context syncs it right before the list runs.
        if (Resource* r = c->backing(); r && r->place && !r->gpu_owned) {
            auto& v = cur->recorded_placements;
            if (std::find(v.begin(), v.end(), r) == v.end()) {
                r->add_ref();
                v.push_back(r);
            }
        }
        return c->dxvk;
    }
    if (Resource* r = c->backing(); r && r->place && !r->gpu_owned) g_bind_syncs.fetch_add(1, std::memory_order_relaxed);
    sync_for_gpu(ctx, c);
    return c->dxvk;
}

// Game interface pointers -> DXVK pointers, for array arguments of binds.
struct Unwrapped {
    void* p[128];
    Unwrapped(void* ctx, void* const* src, UINT n) {
        if (n > 128) fatal("d3d: %u objects in one call", n);
        for (UINT i = 0; i < n; i++) p[i] = src ? bind_unwrap(ctx, src[i]) : nullptr;
    }
};

// DXVK object (AddRef'd by a DXVK getter) -> our wrapper, AddRef'd.
void* rewrap(void* obj) {
    if (!obj) return nullptr;
    DeviceChild* w = acquire_wrapper(obj);
    if (!w) {
        // A state object the game released while it was still bound (DXVK
        // keeps it alive, our wrapper is gone): give it a new wrapper, as the
        // bound reference would have kept the D3D11 object alive.
        w = wrap_orphan_state(obj);
        if (!w) fatal("d3d: DXVK returned object %p that has no wrapper", obj);
        log_once("d3d: a released state object was still bound: new wrapper", 0);
        return &w->iface;  // takes over the getter's reference
    }
    dx_release(obj);
    return &w->iface;
}
void rewrap_array(void** arr, UINT n) {
    if (!arr) return;
    for (UINT i = 0; i < n; i++) arr[i] = rewrap(arr[i]);
}

// ---- Inline state the game writes into the block ----

// The game's inlined IASetPrimitiveTopology stores table[topology] at ctx+0x170,
// where `table` is a 65-entry u64 array in the game image (game rva 0x1df87e0).
// We read that same table at run time to map the value back to a topology.
constexpr uint32_t kTopologyTableRva = 0x1df87e0;
constexpr int kTopologyCount = 65;

int topology_from_bits(uint64_t bits) {
    static std::unordered_map<uint64_t, int> map = [] {
        std::unordered_map<uint64_t, int> m;
        const auto* t = loader::main_image().at<uint64_t>(kTopologyTableRva);
        // Several unused topologies share one value; keep the first (lowest) index.
        for (int i = kTopologyCount - 1; i >= 0; i--) m[t[i]] = i;
        return m;
    }();
    auto it = map.find(bits);
    if (it == map.end()) fatal("d3d: unknown inline topology value 0x%llx at ctx+0x170", (unsigned long long)bits);
    return it->second;
}

void apply_inline_state(C* c) {
    if (c->deferred) c->recorded_draws++;
    uint64_t bits = c->u64(C::kTopology);
    if (bits != c->applied_topology_bits && bits) {
        int topo = topology_from_bits(bits);
        if (topo >= 14 && topo <= 32) log_once("Xbox-only primitive topology (not supported by DXVK)", topo);
        dx<void>(c->dxvk, dc::IASetPrimitiveTopology, topo);
        c->applied_topology_bits = bits;
    }
}

// FL_GPU_PROFILE: a GPU timestamp before each operation on the immediate context.
inline void prof(void* self, const char* kind) {
    if (gpuprof::active() && !C::from(self)->deferred) gpuprof::mark(ctxdx(self), kind);
}

// ---- IUnknown / DeviceChild ----

WINAPI HRESULT ctx_QueryInterface(void* self, const GUID& iid, void** out) {
    auto* c = C::from(self);
    if (iid == winrt::IID_IUnknown || iid == IID_ID3D11DeviceChild || iid == IID_ID3D11DeviceContext ||
        iid == IID_ID3D11DeviceContext1 || iid == IID_ID3D11DeviceContext2 || iid == IID_ID3D11DeviceContextX ||
        iid == IID_XboxContextBase48800095 || iid == IID_XboxContextB2DAAD8B) {
        add_ref(c);
        *out = self;
        return S_OK;
    }
    FL_ERROR("d3d: ID3D11DeviceContextX::QueryInterface(%s) -> E_NOINTERFACE", winrt::guid_str(iid).c_str());
    *out = nullptr;
    return E_NOINTERFACE;
}
WINAPI ULONG ctx_AddRef(void* self) { return add_ref(C::from(self)); }
WINAPI ULONG ctx_Release(void* self) {
    auto* c = C::from(self);
    if (!c->deferred) return c->device->release();
    ULONG r = --c->refs;
    if (r == 0) {
        DeviceX* d = c->device;
        dx_release(c->dxvk);
        delete c;
        d->release();
    }
    return r;
}
WINAPI void ctx_GetDevice(void* self, void** out) {
    auto* d = C::from(self)->device;
    d->add_ref();
    *out = &d->device_iface;
}
WINAPI HRESULT ctx_GetPrivateData(void* self, const GUID& g, UINT* size, void* data) {
    return C::from(self)->device->priv.get(g, size, data);
}
WINAPI HRESULT ctx_SetPrivateData(void* self, const GUID& g, UINT size, const void* data) {
    return C::from(self)->device->priv.set(g, size, data);
}
WINAPI HRESULT ctx_SetPrivateDataInterface(void* self, const GUID& g, void* unknown) {
    return C::from(self)->device->priv.set_interface(g, unknown);
}

// ---- Per-stage setters (VS, PS, GS, HS, DS, CS) ----

#define FL_STAGE_SETTERS(S)                                                                                  \
    WINAPI void ctx_##S##SetConstantBuffers(void* self, UINT start, UINT n, void* const* bufs) {             \
        Unwrapped u(ctxdx(self), bufs, n);                                                                                \
        dx<void>(ctxdx(self), dc::S##SetConstantBuffers, start, n, u.p);                                     \
    }                                                                                                        \
    WINAPI void ctx_##S##SetShaderResources(void* self, UINT start, UINT n, void* const* views) {            \
        Unwrapped u(ctxdx(self), views, n);                                                                               \
        if (trace::active() && #S[0] == 'C')                                                                \
            trace::event("CSSetShaderResources(%u, %u) [ctx %p] from game rva 0x%llx", start, n, ctxdx(self),  \
                         (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -         \
                                              reinterpret_cast<uintptr_t>(loader::main_image().base)));         \
        dx<void>(ctxdx(self), dc::S##SetShaderResources, start, n, u.p);                                     \
    }                                                                                                        \
    WINAPI void ctx_##S##SetSamplers(void* self, UINT start, UINT n, void* const* samplers) {                \
        Unwrapped u(ctxdx(self), samplers, n);                                                                            \
        dx<void>(ctxdx(self), dc::S##SetSamplers, start, n, u.p);                                            \
    }                                                                                                        \
    /* umd's xSSetShader (e.g. VS umd 0x9320, PS 0x91f0) only stores the shader: the class-instance     \
       arguments are ignored on the console (the game leaves garbage in them). */                            \
    WINAPI void ctx_##S##SetShader(void* self, void* shader, void* const*, UINT) {                          \
        dx<void>(ctxdx(self), dc::S##SetShader, unwrap(shader), nullptr, 0u);                                \
    }                                                                                                        \
    WINAPI void ctx_##S##GetConstantBuffers(void* self, UINT start, UINT n, void** bufs) {                   \
        dx<void>(ctxdx(self), dc::S##GetConstantBuffers, start, n, bufs);                                    \
        rewrap_array(bufs, n);                                                                               \
    }                                                                                                        \
    WINAPI void ctx_##S##GetShaderResources(void* self, UINT start, UINT n, void** views) {                  \
        dx<void>(ctxdx(self), dc::S##GetShaderResources, start, n, views);                                   \
        rewrap_array(views, n);                                                                              \
    }                                                                                                        \
    WINAPI void ctx_##S##GetSamplers(void* self, UINT start, UINT n, void** samplers) {                      \
        dx<void>(ctxdx(self), dc::S##GetSamplers, start, n, samplers);                                       \
        rewrap_array(samplers, n);                                                                           \
    }                                                                                                        \
    WINAPI void ctx_##S##GetShader(void* self, void** shader, void** inst, UINT* n) {                        \
        UINT zero = 0;                                                                                       \
        dx<void>(ctxdx(self), dc::S##GetShader, shader, nullptr, &zero);                                     \
        if (shader) *shader = rewrap(*shader);                                                               \
        if (n) *n = 0;                                                                                       \
    }                                                                                                        \
    WINAPI void ctx_##S##SetConstantBuffers1(void* self, UINT start, UINT n, void* const* bufs,              \
                                             const UINT* first, const UINT* num) {                           \
        Unwrapped u(ctxdx(self), bufs, n);                                                                                \
        dx<void>(ctxdx(self), dc::S##SetConstantBuffers1, start, n, u.p, first, num);                        \
    }
FL_STAGE_SETTERS(VS)
FL_STAGE_SETTERS(PS)
FL_STAGE_SETTERS(GS)
FL_STAGE_SETTERS(HS)
FL_STAGE_SETTERS(DS)
FL_STAGE_SETTERS(CS)
#undef FL_STAGE_SETTERS

// ---- Input assembler ----

WINAPI void ctx_IASetInputLayout(void* self, void* layout) {
    dx<void>(ctxdx(self), dc::IASetInputLayout, unwrap(layout));
}
WINAPI void ctx_IASetVertexBuffers(void* self, UINT start, UINT n, void* const* bufs, const UINT* strides,
                                   const UINT* offsets) {
    Unwrapped u(ctxdx(self), bufs, n);
    dx<void>(ctxdx(self), dc::IASetVertexBuffers, start, n, u.p, strides, offsets);
}
// Xbox slot 20: NOT the desktop signature. (this, hardware index type, buffer, offset):
// umd emits PM4 INDEX_TYPE from edx; the game passes 0 for 16-bit indices (rva 0xc5b735).
WINAPI void ctx_IASetIndexBufferX(void* self, UINT hw_type, void* buffer, UINT offset) {
    UINT format;
    switch (hw_type) {
        case 0: format = 57; break;  // DXGI_FORMAT_R16_UINT
        case 1: format = 42; break;  // DXGI_FORMAT_R32_UINT
        default: fatal("d3d: IASetIndexBuffer: unknown hardware index type %u", hw_type);
    }
    dx<void>(ctxdx(self), dc::IASetIndexBuffer, bind_unwrap(ctxdx(self), buffer), format, offset);
}
WINAPI void ctx_IAGetInputLayout(void* self, void** layout) {
    dx<void>(ctxdx(self), dc::IAGetInputLayout, layout);
    *layout = rewrap(*layout);
}
WINAPI void ctx_IAGetVertexBuffers(void* self, UINT start, UINT n, void** bufs, UINT* strides, UINT* offsets) {
    dx<void>(ctxdx(self), dc::IAGetVertexBuffers, start, n, bufs, strides, offsets);
    rewrap_array(bufs, n);
}
WINAPI void ctx_IAGetIndexBuffer(void* self, void** buf, UINT* format, UINT* offset) {
    dx<void>(ctxdx(self), dc::IAGetIndexBuffer, buf, format, offset);
    if (buf) *buf = rewrap(*buf);
}
WINAPI void ctx_IAGetPrimitiveTopology(void* self, int* topo) {
    apply_inline_state(C::from(self));
    dx<void>(ctxdx(self), dc::IAGetPrimitiveTopology, topo);
}

// ---- Draws and dispatches ----
// The Xbox draw methods pack pairs of 32-bit arguments into one 64-bit
// register (umd slots 13/21/22 split them with shr 32 and put the halves in
// the DRAW_INDEX_OFFSET_2 / NUM_INSTANCES packets, FINDINGS §3.1c):
//   13 DrawIndexed(u64 start index | index count << 32, INT base vertex)
//   14 Draw(UINT vertex count, UINT start vertex)                (desktop)
//   21 DrawIndexedInstanced(u64 start index | count << 32, u64 base vertex | start instance << 32, UINT instances)
//   22 DrawInstanced(UINT vertex count, u64 start vertex | start instance << 32, UINT instances)

WINAPI void ctx_DrawIndexed(void* self, uint64_t start_count, INT base) {
    UINT start = static_cast<UINT>(start_count), count = static_cast<UINT>(start_count >> 32);
    apply_inline_state(C::from(self));
    prof(self, "DrawIndexed");
    trace::draw(ctxdx(self), "DrawIndexed", count, start, base);
    dx<void>(ctxdx(self), dc::DrawIndexed, count, start, base);
    trace::after_draw(ctxdx(self));
}
// FL_DEBUG_NULL_SRV=<ps fnv hex>:<slot>[,<slot>...] (bisect aid): unbind those
// pixel-shader resources right before every Draw with that pixel shader
// (hash from FL_LOG=shaders).
void debug_null_srvs(void* dxctx) {
    static const char* env = getenv("FL_DEBUG_NULL_SRV");
    if (!env) return;
    static uint64_t want = strtoull(env, nullptr, 16);
    static std::vector<UINT> slots = [] {
        std::vector<UINT> v;
        const char* p = strchr(env, ':');
        while (p && *++p) {
            v.push_back(static_cast<UINT>(strtoul(p, const_cast<char**>(&p), 10)));
            if (*p != ',') break;
        }
        return v;
    }();
    void* ps = nullptr;
    dx<void>(dxctx, dc::PSGetShader, &ps, nullptr, nullptr);
    if (!ps) return;
    bool match = shader_hash(ps) == want;
    dx_release(ps);
    if (!match) return;
    void* none = nullptr;
    for (UINT slot : slots) dx<void>(dxctx, dc::PSSetShaderResources, slot, 1u, &none);
    log_once("FL_DEBUG_NULL_SRV: unbound pixel-shader resources", want);
}

WINAPI void ctx_Draw(void* self, UINT count, UINT start) {
    apply_inline_state(C::from(self));
    debug_null_srvs(ctxdx(self));
    prof(self, "Draw");
    trace::draw(ctxdx(self), "Draw", count, start);
    dx<void>(ctxdx(self), dc::Draw, count, start);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_DrawIndexedInstanced(void* self, uint64_t start_count, uint64_t base_inst, UINT instances) {
    UINT start = static_cast<UINT>(start_count), count = static_cast<UINT>(start_count >> 32);
    INT base = static_cast<INT>(static_cast<uint32_t>(base_inst));
    UINT start_inst = static_cast<UINT>(base_inst >> 32);
    apply_inline_state(C::from(self));
    prof(self, "DrawIndexedInstanced");
    trace::draw(ctxdx(self), "DrawIndexedInstanced", count, instances, start);
    dx<void>(ctxdx(self), dc::DrawIndexedInstanced, count, instances, start, base, start_inst);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_DrawInstanced(void* self, UINT count, uint64_t start_inst64, UINT instances) {
    UINT start = static_cast<UINT>(start_inst64), start_inst = static_cast<UINT>(start_inst64 >> 32);
    apply_inline_state(C::from(self));
    prof(self, "DrawInstanced");
    trace::draw(ctxdx(self), "DrawInstanced", count, instances, start);
    dx<void>(ctxdx(self), dc::DrawInstanced, count, instances, start, start_inst);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_DrawAuto(void* self) {
    apply_inline_state(C::from(self));
    prof(self, "DrawAuto");
    trace::draw(ctxdx(self), "DrawAuto", 0);
    dx<void>(ctxdx(self), dc::DrawAuto);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_DrawIndexedInstancedIndirect(void* self, void* buf, UINT offset) {
    apply_inline_state(C::from(self));
    void* b = bind_unwrap(ctxdx(self), buf);
    prof(self, "DrawIndexedInstancedIndirect");
    trace::draw(ctxdx(self), "DrawIndexedInstancedIndirect", offset);
    trace::event("    args [%s] +%u", trace::describe_dxvk(b), offset);
    dx<void>(ctxdx(self), dc::DrawIndexedInstancedIndirect, b, offset);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_DrawInstancedIndirect(void* self, void* buf, UINT offset) {
    apply_inline_state(C::from(self));
    void* b = bind_unwrap(ctxdx(self), buf);
    prof(self, "DrawInstancedIndirect");
    trace::draw(ctxdx(self), "DrawInstancedIndirect", offset);
    trace::event("    args [%s] +%u", trace::describe_dxvk(b), offset);
    dx<void>(ctxdx(self), dc::DrawInstancedIndirect, b, offset);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_Dispatch(void* self, UINT x, UINT y, UINT z) {
    prof(self, "Dispatch");
    trace::draw(ctxdx(self), "Dispatch", x, y, z);
    trace::log_dispatch(ctxdx(self), x, y, z);
    dx<void>(ctxdx(self), dc::Dispatch, x, y, z);
    trace::after_draw(ctxdx(self));
}
WINAPI void ctx_DispatchIndirect(void* self, void* buf, UINT offset) {
    void* b = bind_unwrap(ctxdx(self), buf);
    prof(self, "DispatchIndirect");
    trace::draw(ctxdx(self), "DispatchIndirect", offset);
    trace::event("    args [%s] +%u", trace::describe_dxvk(b), offset);
    dx<void>(ctxdx(self), dc::DispatchIndirect, b, offset);
    trace::after_draw(ctxdx(self));
}

// ---- Map / copies / clears ----

struct MappedSubresource {
    void* data;
    UINT row_pitch;
    UINT depth_pitch;
};

// FL_LOG=cbwrite: what the game writes into 16-byte constant buffers.
std::mutex g_cbw_mutex;
std::unordered_map<DeviceChild*, void*> g_cbw_mapped;
bool small_cb(DeviceChild* c) {
    Resource* r = c ? c->backing() : nullptr;
    return r && r->dim == 1 && r->desc32()[0] <= 64 && (r->desc32()[2] & 0x4);
}
void note_small_cb_map(DeviceChild* c, UINT type, void* data) {
    if (!small_cb(c)) return;
    std::lock_guard lock(g_cbw_mutex);
    g_cbw_mapped[c] = data;
    FL_LOG(cbwrite, "Map %p type %u", (void*)c, type);
}
void note_small_cb_unmap(DeviceChild* c) {
    if (!small_cb(c)) return;
    std::lock_guard lock(g_cbw_mutex);
    auto it = g_cbw_mapped.find(c);
    if (it == g_cbw_mapped.end()) return;
    const float* f = static_cast<const float*>(it->second);
    FL_LOG(cbwrite, "Unmap %p: %g %g %g %g", (void*)c, f[0], f[1], f[2], f[3]);
    g_cbw_mapped.erase(it);
}

WINAPI HRESULT ctx_Map(void* self, void* res, UINT sub, UINT type, UINT flags, void* mapped) {
    if (type < 1 || type > 5) log_once("Map: Xbox-only map type", type);
    if (Resource* r = child_of(res)->backing(); r && r->place) {
        // Placement memory is the game's own: hand it back (as on Xbox). Its
        // writes are caught by the page tracking.
        if (r->dim != 1 || sub) fatal("d3d: Map of a placement texture (sub %u) not implemented", sub);
        auto* m = static_cast<MappedSubresource*>(mapped);
        m->data = r->place;
        m->row_pitch = m->depth_pitch = static_cast<UINT>(r->place_size);
        return S_OK;
    }
    if (flags & ~0x100000u) log_once("Map: Xbox-only map flags", flags);
    if (trace::active()) trace::event("Map [%s] sub %u type %u", trace::describe_dxvk(unwrap(res)), sub, type);
    HRESULT hr = dx<HRESULT>(ctxdx(self), dc::Map, unwrap(res), sub, type, flags, mapped);
    if (log_enabled("cbwrite") && hr >= 0) note_small_cb_map(child_of(res), type, static_cast<MappedSubresource*>(mapped)->data);
    // DXGI_ERROR_WAS_STILL_DRAWING for a D3D11_MAP_FLAG_DO_NOT_WAIT map is the
    // normal answer: the game polls its read-back buffers that way.
    if (hr == static_cast<HRESULT>(0x887A000A) && (flags & 0x100000u)) return hr;
    if (hr < 0) {
        auto* r = static_cast<Resource*>(child_of(res));
        const uint32_t* d = reinterpret_cast<const uint32_t*>(r->xdesc);
        FL_ERROR("d3d: Map(dim=%d sub=%u type=%u flags=0x%x; desc %u %u %u %u %u %u %u %u %u %u %u) failed: 0x%08x",
                 r->dim, sub, type, flags, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
                 static_cast<uint32_t>(hr));
    }
    return hr;
}
WINAPI void ctx_Unmap(void* self, void* res, UINT sub) {
    if (Resource* r = child_of(res)->backing(); r && r->place) return;
    if (log_enabled("cbwrite")) note_small_cb_unmap(child_of(res));
    dx<void>(ctxdx(self), dc::Unmap, unwrap(res), sub);
}

// Copy sources go through bind_unwrap: a placement source the CPU just wrote
// must be uploaded before the GPU reads it, exactly as for a bind.
WINAPI void ctx_CopySubresourceRegion(void* self, void* dst, UINT dst_sub, UINT x, UINT y, UINT z, void* src,
                                      UINT src_sub, const void* box) {
    prof(self, "CopySubresource");
    void* s = bind_unwrap(ctxdx(self), src);
    if (trace::active())
        trace::event("CopySubresourceRegion dst[%s] sub %u at %u,%u <- src sub %u", trace::describe_dxvk(unwrap(dst)),
                     dst_sub, x, y, src_sub);
    trace::note_write("CopySubresourceRegion", unwrap(dst), s);
    dx<void>(ctxdx(self), dc::CopySubresourceRegion, unwrap(dst), dst_sub, x, y, z, s, src_sub, box);
}
WINAPI void ctx_CopySubresourceRegion1(void* self, void* dst, UINT dst_sub, UINT x, UINT y, UINT z, void* src,
                                       UINT src_sub, const void* box, UINT flags) {
    prof(self, "CopySubresource");
    void* s = bind_unwrap(ctxdx(self), src);
    trace::note_write("CopySubresourceRegion1", unwrap(dst), s);
    dx<void>(ctxdx(self), dc::CopySubresourceRegion1, unwrap(dst), dst_sub, x, y, z, s, src_sub, box, flags);
}
WINAPI void ctx_CopyResource(void* self, void* dst, void* src) {
    prof(self, "CopyResource");
    void* s = bind_unwrap(ctxdx(self), src);
    if (trace::active())
        trace::event("CopyResource dst[%s] <- src[%s]", std::string(trace::describe_dxvk(unwrap(dst))).c_str(),
                     std::string(trace::describe_dxvk(s)).c_str());
    trace::note_write("CopyResource", unwrap(dst), s);
    dx<void>(ctxdx(self), dc::CopyResource, unwrap(dst), s);
}
// UpdateSubresource on a CPU-written placement buffer: on the console the
// update lands in the buffer's memory, which is the game's memory. Write it
// there too, or the next upload from game memory (after a CPU write to the
// buffer or to a neighbour on the same page) would put the old contents back.
void write_through_placement(void* dst, const void* box, const void* data) {
    DeviceChild* c = child_of(dst);
    Resource* r = c ? c->backing() : nullptr;
    if (!r || !r->place || r->gpu_owned || !data) return;
    if (r->dim != 1) {
        log_once("UNIMPLEMENTED UpdateSubresource of a placement texture: game memory not updated", r->dim);
        return;
    }
    const uint32_t* b = static_cast<const uint32_t*>(box);  // D3D11_BOX: left, top, front, right, bottom, back
    size_t lo = b ? b[0] : 0, hi = b ? b[3] : r->place_size;
    if (hi > r->place_size || lo >= hi) return;
    std::memcpy(r->place + lo, data, hi - lo);
}

WINAPI void ctx_UpdateSubresource(void* self, void* dst, UINT sub, const void* box, const void* data, UINT row,
                                  UINT depth) {
    if (trace::active()) trace::event("UpdateSubresource [%s] sub %u row %u", trace::describe_dxvk(unwrap(dst)), sub, row);
    FL_LOG(upload, "UpdateSubresource [%s] sub %u box %p row %u depth %u", trace::describe_dxvk(unwrap(dst)), sub, box, row,
           depth);
    write_through_placement(dst, box, data);
    trace::note_write("UpdateSubresource", unwrap(dst), nullptr);
    if (log_enabled("cbwrite") && small_cb(child_of(dst)) && data) {
        const float* f = static_cast<const float*>(data);
        FL_LOG(cbwrite, "UpdateSubresource %p: %g %g %g %g", (void*)child_of(dst), f[0], f[1], f[2], f[3]);
    }
    dx<void>(ctxdx(self), dc::UpdateSubresource, unwrap(dst), sub, box, data, row, depth);
}
WINAPI void ctx_UpdateSubresource1(void* self, void* dst, UINT sub, const void* box, const void* data, UINT row,
                                   UINT depth, UINT flags) {
    write_through_placement(dst, box, data);
    dx<void>(ctxdx(self), dc::UpdateSubresource1, unwrap(dst), sub, box, data, row, depth, flags);
}
WINAPI void ctx_CopyStructureCount(void* self, void* dst, UINT offset, void* uav) {
    dx<void>(ctxdx(self), dc::CopyStructureCount, unwrap(dst), offset, unwrap(uav));
}
WINAPI void ctx_ClearRenderTargetView(void* self, void* rtv, const float* color) {
    prof(self, "ClearRTV");
    if (trace::active())
        trace::event("ClearRenderTargetView %p (%.2f %.2f %.2f %.2f)", rtv, color[0], color[1], color[2], color[3]);
    dx<void>(ctxdx(self), dc::ClearRenderTargetView, unwrap(rtv), color);
}
WINAPI void ctx_ClearUnorderedAccessViewUint(void* self, void* uav, const UINT* values) {
    prof(self, "ClearUAV");
    dx<void>(ctxdx(self), dc::ClearUnorderedAccessViewUint, unwrap(uav), values);
}
WINAPI void ctx_ClearUnorderedAccessViewFloat(void* self, void* uav, const float* values) {
    prof(self, "ClearUAV");
    dx<void>(ctxdx(self), dc::ClearUnorderedAccessViewFloat, unwrap(uav), values);
}
WINAPI void ctx_ClearDepthStencilView(void* self, void* dsv, UINT flags, float depth, uint8_t stencil) {
    prof(self, "ClearDSV");
    if (trace::active()) {
        void* res = nullptr;
        if (dsv) dx<void>(unwrap(dsv), 7 /* ID3D11View::GetResource */, &res);
        trace::event("ClearDepthStencilView [%s] flags %u depth %g stencil %u",
                     std::string(trace::describe_dxvk(res)).c_str(), flags, depth, stencil);
        if (res) dx_release(res);
    }
    dx<void>(ctxdx(self), dc::ClearDepthStencilView, unwrap(dsv), flags, depth, stencil);
}
WINAPI void ctx_GenerateMips(void* self, void* srv) {
    if (log_enabled("genmips")) {
        void* res = nullptr;
        if (srv) dx<void>(unwrap(srv), 7 /* ID3D11View::GetResource */, &res);
        FL_LOG(genmips, "GenerateMips [%s]", trace::describe_dxvk(res));
        if (res) dx_release(res);
    }
    dx<void>(ctxdx(self), dc::GenerateMips, unwrap(srv));
}
WINAPI void ctx_SetResourceMinLOD(void* self, void* res, float lod) {
    // STUB: DXVK ignores SetResourceMinLOD (the game clamps per view, slot 223).
    log_once("STUB: SetResourceMinLOD is ignored by DXVK", 0);
    dx<void>(ctxdx(self), dc::SetResourceMinLOD, unwrap(res), lod);
}
WINAPI float ctx_GetResourceMinLOD(void* self, void* res) {
    return dx<float>(ctxdx(self), dc::GetResourceMinLOD, unwrap(res));
}
WINAPI void ctx_ResolveSubresource(void* self, void* dst, UINT dst_sub, void* src, UINT src_sub, UINT format) {
    dx<void>(ctxdx(self), dc::ResolveSubresource, unwrap(dst), dst_sub, unwrap(src), src_sub, format);
}
WINAPI void ctx_DiscardResource(void* self, void* res) { dx<void>(ctxdx(self), dc::DiscardResource, unwrap(res)); }
WINAPI void ctx_DiscardView(void* self, void* view) { dx<void>(ctxdx(self), dc::DiscardView, unwrap(view)); }
WINAPI void ctx_ClearView(void* self, void* view, const float* color, const void* rects, UINT n) {
    prof(self, "ClearView");
    dx<void>(ctxdx(self), dc::ClearView, unwrap(view), color, rects, n);
}

// ---- Queries ----

WINAPI void ctx_Begin(void* self, void* async) { dx<void>(ctxdx(self), dc::Begin, unwrap(async)); }
WINAPI void ctx_End(void* self, void* async) { dx<void>(ctxdx(self), dc::End, unwrap(async)); }
WINAPI HRESULT ctx_GetData(void* self, void* async, void* data, UINT size, UINT flags) {
    HRESULT hr = dx<HRESULT>(ctxdx(self), dc::GetData, unwrap(async), data, size, flags);
    if (log_enabled("query")) {
        uint64_t v = 0;
        if (hr == S_OK && data) std::memcpy(&v, data, std::min<UINT>(size, 8));
        FL_LOG(query, "GetData(dxvk %p, %u B, flags 0x%x) -> 0x%x value 0x%llx", unwrap(async), size, flags,
               static_cast<uint32_t>(hr), (unsigned long long)v);
    }
    return hr;
}
WINAPI void ctx_SetPredication(void* self, void* pred, BOOL value) {
    FL_LOG(query, "SetPredication(dxvk %p, %d)", unwrap(pred), value);
    dx<void>(ctxdx(self), dc::SetPredication, unwrap(pred), value);
}
WINAPI void ctx_GetPredication(void* self, void** pred, BOOL* value) {
    dx<void>(ctxdx(self), dc::GetPredication, pred, value);
    if (pred) *pred = rewrap(*pred);
}

// ---- Output merger / rasterizer / stream out ----

// A bundle's render target / depth binds: shadow state only (see ContextX::bundle).
void set_bundle_targets(C* c, UINT n, void* const* rtvs, void* dsv) {
    for (UINT i = 0; i < 8; i++) {
        DeviceChild* v = rtvs && i < n ? child_of(rtvs[i]) : nullptr;
        if (v) v->add_ref();
        if (c->shadow_rtv[i]) c->shadow_rtv[i]->release();
        c->shadow_rtv[i] = v;
    }
    DeviceChild* d = child_of(dsv);
    if (d) d->add_ref();
    if (c->shadow_dsv) c->shadow_dsv->release();
    c->shadow_dsv = d;
}
void get_bundle_targets(C* c, UINT n, void** rtvs, void** dsv) {
    for (UINT i = 0; rtvs && i < n; i++) {
        DeviceChild* v = i < 8 ? c->shadow_rtv[i] : nullptr;
        if (v) v->add_ref();
        rtvs[i] = v ? &v->iface : nullptr;
    }
    if (dsv) {
        if (c->shadow_dsv) c->shadow_dsv->add_ref();
        *dsv = c->shadow_dsv ? &c->shadow_dsv->iface : nullptr;
    }
}

WINAPI void ctx_OMSetRenderTargets(void* self, UINT n, void* const* rtvs, void* dsv) {
    if (C* c = C::from(self); c->bundle) {
        set_bundle_targets(c, n, rtvs, dsv);
        return;
    }
    Unwrapped u(ctxdx(self), rtvs, n);
    for (UINT i = 0; i < n; i++) trace::note_view_write("render target", u.p[i]);
    dx<void>(ctxdx(self), dc::OMSetRenderTargets, n, u.p, unwrap(dsv));
}
WINAPI void ctx_OMSetRenderTargetsAndUnorderedAccessViews(void* self, UINT n_rtv, void* const* rtvs, void* dsv,
                                                          UINT uav_start, UINT n_uav, void* const* uavs,
                                                          const UINT* counts) {
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL / D3D11_KEEP_UNORDERED_ACCESS_VIEWS are 0xffffffff.
    if (C* c = C::from(self); c->bundle && n_rtv != 0xffffffffu) {
        set_bundle_targets(c, n_rtv, rtvs, dsv);  // the UAVs are recorded (Azimuth packet kind 0xd)
        n_rtv = 0xffffffffu;
        if (n_uav == 0xffffffffu) return;
    }
    Unwrapped ur(ctxdx(self), n_rtv == 0xffffffffu ? nullptr : rtvs, n_rtv == 0xffffffffu ? 0 : n_rtv);
    Unwrapped uu(ctxdx(self), n_uav == 0xffffffffu ? nullptr : uavs, n_uav == 0xffffffffu ? 0 : n_uav);
    dx<void>(ctxdx(self), dc::OMSetRenderTargetsAndUnorderedAccessViews, n_rtv,
             n_rtv == 0xffffffffu ? nullptr : ur.p, n_rtv == 0xffffffffu ? nullptr : unwrap(dsv), uav_start, n_uav,
             n_uav == 0xffffffffu ? nullptr : uu.p, counts);
}
WINAPI void ctx_OMSetBlendState(void* self, void* state, const float* factor, UINT mask) {
    dx<void>(ctxdx(self), dc::OMSetBlendState, unwrap(state), factor, mask);
}
WINAPI void ctx_OMSetDepthStencilState(void* self, void* state, UINT ref) {
    dx<void>(ctxdx(self), dc::OMSetDepthStencilState, unwrap(state), ref);
}
WINAPI void ctx_OMGetRenderTargets(void* self, UINT n, void** rtvs, void** dsv) {
    if (C* c = C::from(self); c->bundle) return get_bundle_targets(c, n, rtvs, dsv);
    dx<void>(ctxdx(self), dc::OMGetRenderTargets, n, rtvs, dsv);
    rewrap_array(rtvs, n);
    if (dsv) *dsv = rewrap(*dsv);
}
WINAPI void ctx_OMGetRenderTargetsAndUnorderedAccessViews(void* self, UINT n_rtv, void** rtvs, void** dsv,
                                                          UINT uav_start, UINT n_uav, void** uavs) {
    if (C* c = C::from(self); c->bundle) {
        get_bundle_targets(c, n_rtv, rtvs, dsv);
        dx<void>(ctxdx(self), dc::OMGetRenderTargetsAndUnorderedAccessViews, 0u, nullptr, nullptr, uav_start, n_uav, uavs);
        rewrap_array(uavs, n_uav);
        return;
    }
    dx<void>(ctxdx(self), dc::OMGetRenderTargetsAndUnorderedAccessViews, n_rtv, rtvs, dsv, uav_start, n_uav, uavs);
    rewrap_array(rtvs, n_rtv);
    if (dsv) *dsv = rewrap(*dsv);
    rewrap_array(uavs, n_uav);
}
WINAPI void ctx_OMGetBlendState(void* self, void** state, float* factor, UINT* mask) {
    dx<void>(ctxdx(self), dc::OMGetBlendState, state, factor, mask);
    if (state) *state = rewrap(*state);
}
WINAPI void ctx_OMGetDepthStencilState(void* self, void** state, UINT* ref) {
    dx<void>(ctxdx(self), dc::OMGetDepthStencilState, state, ref);
    if (state) *state = rewrap(*state);
}
WINAPI void ctx_SOSetTargets(void* self, UINT n, void* const* bufs, const UINT* offsets) {
    Unwrapped u(ctxdx(self), bufs, n);
    dx<void>(ctxdx(self), dc::SOSetTargets, n, u.p, offsets);
}
WINAPI void ctx_SOGetTargets(void* self, UINT n, void** bufs) {
    dx<void>(ctxdx(self), dc::SOGetTargets, n, bufs);
    rewrap_array(bufs, n);
}
WINAPI void ctx_RSSetState(void* self, void* state) { dx<void>(ctxdx(self), dc::RSSetState, unwrap(state)); }
WINAPI void ctx_RSSetViewports(void* self, UINT n, const void* vps) {
    dx<void>(ctxdx(self), dc::RSSetViewports, n, vps);
}
WINAPI void ctx_RSSetScissorRects(void* self, UINT n, const void* rects) {
    dx<void>(ctxdx(self), dc::RSSetScissorRects, n, rects);
}
WINAPI void ctx_RSGetState(void* self, void** state) {
    dx<void>(ctxdx(self), dc::RSGetState, state);
    *state = rewrap(*state);
}
WINAPI void ctx_RSGetViewports(void* self, UINT* n, void* vps) { dx<void>(ctxdx(self), dc::RSGetViewports, n, vps); }
WINAPI void ctx_RSGetScissorRects(void* self, UINT* n, void* rects) {
    dx<void>(ctxdx(self), dc::RSGetScissorRects, n, rects);
}
WINAPI void ctx_CSSetUnorderedAccessViews(void* self, UINT start, UINT n, void* const* uavs, const UINT* counts) {
    Unwrapped u(ctxdx(self), uavs, n);
    dx<void>(ctxdx(self), dc::CSSetUnorderedAccessViews, start, n, u.p, counts);
}
WINAPI void ctx_CSGetUnorderedAccessViews(void* self, UINT start, UINT n, void** uavs) {
    dx<void>(ctxdx(self), dc::CSGetUnorderedAccessViews, start, n, uavs);
    rewrap_array(uavs, n);
}

// ---- Misc ----

WINAPI void ctx_ClearState(void* self) {
    auto* c = C::from(self);
    trace::event("ClearState [ctx %p]", c->dxvk);
    dx<void>(c->dxvk, dc::ClearState);
    c->applied_topology_bits = 0;  // DXVK forgot the topology; re-apply on the next draw
    if (c->bundle) set_bundle_targets(c, 0, nullptr, nullptr);
}
WINAPI void ctx_Flush(void* self) { dx<void>(ctxdx(self), dc::Flush); }
// D3D11_DEVICE_CONTEXT_IMMEDIATE / _DEFERRED (umd's immediate context returns 0).
WINAPI UINT ctx_GetType(void* self) { return C::from(self)->deferred ? 1 : 0; }
WINAPI UINT ctx_GetContextFlags(void* self) { return 0; }  // umd: 0

// Command lists: DeviceChild + 8 GetContextFlags (desktop 7).
WINAPI UINT cmdlist_GetContextFlags(winrt::Iface* self) { return 0; }
winrt::Vtable& cmdlist_vt() {
    static winrt::Vtable v = [] {
        winrt::Vtable t("ID3D11CommandListX", 16, false);
        child_base(t).set(8, &cmdlist_GetContextFlags);
        return t;
    }();
    return v;
}

struct CommandList : DeviceChild {
    CommandList(DeviceX* dev, void* dxvk_list) : DeviceChild(u"D3D11CommandList", dev, cmdlist_vt(), dxvk_list) {
        expose(IID_ID3D11CommandList, &iface);
    }
    ~CommandList() override {
        for (Resource* r : placements) r->release();
    }
    std::vector<Resource*> placements;  // re-synced at every ExecuteCommandList
};

WINAPI HRESULT ctx_FinishCommandList(void* self, BOOL restore, void** out) {
    auto* c = C::from(self);
    if (!c->deferred) {
        if (out) *out = nullptr;
        return E_NOTIMPL;  // umd: E_NOTIMPL on the immediate context
    }
    void* list = nullptr;
    HRESULT hr = dx<HRESULT>(c->dxvk, dc::FinishCommandList, restore, out ? &list : nullptr);
    static const bool trace_bundles = getenv("FL_TRACE_BUNDLES") != nullptr;
    if (trace_bundles && !trace::active())
        log_write("trace", "#finish deferred %p [ctx %p] -> list %p", self, c->dxvk, list);
    trace::event("FinishCommandList on deferred %p [ctx %p] -> list %p (hr 0x%x)", self, c->dxvk, list, static_cast<uint32_t>(hr));
    // A bundle's first draw always records its topology: the bundle runs on the
    // executing context's state, whatever this context had before.
    if (!restore || c->bundle) c->applied_topology_bits = 0;
    if (!restore && c->bundle) set_bundle_targets(c, 0, nullptr, nullptr);  // the context state resets
    c->recorded_draws = 0;
    if (hr < 0 || !out) {
        if (hr < 0) FL_ERROR("d3d: FinishCommandList failed: 0x%08x", static_cast<uint32_t>(hr));
        if (out) *out = nullptr;
        return hr;
    }
    auto* w = new CommandList(c->device, list);
    w->placements = std::move(c->recorded_placements);
    c->recorded_placements.clear();
    *out = &w->iface;
    return hr;
}

WINAPI void ctx_ExecuteCommandList(void* self, void* list, BOOL restore) {
    auto* c = C::from(self);
    prof(self, "ExecuteCommandList");
    g_ecls.fetch_add(1, std::memory_order_relaxed);
    FL_LOG(fence, "%s ctx %p ExecuteCommandList %p", c->deferred ? "deferred" : "immediate", self, list);
    if (auto* cl = static_cast<CommandList*>(child_of(list))) {
        g_ecl_syncs.fetch_add(cl->placements.size(), std::memory_order_relaxed);
        for (Resource* r : cl->placements) {
            sync_for_gpu(c->dxvk, r);
            if (c->deferred && std::find(c->recorded_placements.begin(), c->recorded_placements.end(), r) ==
                                   c->recorded_placements.end()) {
                r->add_ref();
                c->recorded_placements.push_back(r);
            }
        }
    }
    if (trace::active()) {
        void* rtv = nullptr;
        void* dsv = nullptr;
        dx<void>(c->dxvk, dc::OMGetRenderTargets, 1u, &rtv, &dsv);
        struct { float x, y, w, h, mn, mx; } vp{};
        UINT nvp = 1;
        dx<void>(c->dxvk, dc::RSGetViewports, &nvp, &vp);
        UINT flags = dx<UINT>(unwrap(list), 7 /* ID3D11CommandList::GetContextFlags */);
        trace::event("ExecuteCommandList %p (dxvk %p, flags 0x%x, restore %d) on %s %p: rt0 %p ds %p viewport %.0fx%.0f",
                     list, unwrap(list), flags, restore, c->deferred ? "deferred" : "immediate", self, rtv, dsv, vp.w,
                     vp.h);
        if (rtv) dx_release(rtv);
        if (dsv) dx_release(dsv);
        static const bool full = getenv("FL_TRACE_ECL_STATE") != nullptr;
        if (full) trace::log_state(c->dxvk);
        else trace::log_targets(c->dxvk, "      ");
    }
    // FL_TRACE_ECL_STATS=1 (traced frames, immediate context): pipeline
    // statistics of each command list, to see where a bundle's geometry is lost.
    static const bool ecl_stats = getenv("FL_TRACE_ECL_STATS") != nullptr;
    void* q = nullptr;
    if (ecl_stats && trace::active() && !c->deferred) {
        UINT qd[2] = {4 /* D3D11_QUERY_PIPELINE_STATISTICS */, 0};
        if (dx<HRESULT>(c->device->dxvk, desktop::dev::CreateQuery, qd, &q) >= 0) dx<void>(c->dxvk, dc::Begin, q);
        else q = nullptr;
    }
    dx<void>(c->dxvk, dc::ExecuteCommandList, unwrap(list), restore);
    if (q) {
        dx<void>(c->dxvk, dc::End, q);
        uint64_t st[11] = {};
        while (dx<HRESULT>(c->dxvk, dc::GetData, q, st, UINT(sizeof st), 0u) == 1 /* S_FALSE */) {
        }
        trace::event("    stats: IA verts %llu prims %llu | VS %llu | clip in %llu out %llu | PS %llu",
                     (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2],
                     (unsigned long long)st[5], (unsigned long long)st[6], (unsigned long long)st[7]);
        dx_release(q);
    }
    trace::after_draw(c->dxvk);
    if (!c->deferred) trace::after_ecl(c->dxvk);
    if (!restore) c->applied_topology_bits = 0;  // DXVK cleared the state
}
WINAPI BOOL ctx_IsAnnotationEnabled(void* self) { return FALSE_; }
WINAPI void ctx_SetMarkerInt(void* self, const WCHAR*, INT) {}
WINAPI void ctx_BeginEventInt(void* self, const WCHAR*, INT) {}
WINAPI void ctx_EndEvent(void* self) {}

// ---- Xbox-only methods ----

// Slot 196 (BOOL): umd sets/clears bit 4 of an internal flags byte (+0xc11) and
// nothing else. What that bit changes inside umd is unknown; we keep the value.
WINAPI void ctx_Slot196(void* self, BOOL on) {
    log_once("ctx slot 196 (internal mode flag) set to", on);
    C::from(self)->slot196_flag = on != 0;
}

// ---- Xbox GPU synchronisation (FINDINGS §3.1b) ----
// umd turns these into PM4 cache-flush / event / wait packets. DXVK tracks
// hazards itself and inserts the barriers these exist for, so on our side they
// are complete no-ops, not stubs.
WINAPI void ctx_FlushGpuCaches(void*) {}                                   // 151: ACQUIRE_MEM, all caches
WINAPI void ctx_FlushGpuCacheRange(void*, UINT flags, void*, uint64_t) {}  // 152: flags (0 = 0xc00000), base, size
WINAPI void ctx_InsertWaitUntilIdle(void*, UINT) {}                        // 153: fence on own engine + wait
// 154: InsertFence(flags) -> u64 fence value the game tracks (rva 0x1131b65
// passes it to its fence list at rva 0x10cedb0). DXVK orders the work itself,
// so a monotonically increasing value is enough; waits on it (155) are no-ops.
WINAPI uint64_t ctx_InsertFence(void* self, UINT flags) {
    static std::atomic<uint64_t> fence{0};
    uint64_t v = ++fence;
    FL_LOG(fence, "%s ctx %p InsertFence(0x%x) -> %llu", C::from(self)->deferred ? "deferred" : "immediate", self,
           flags, static_cast<unsigned long long>(v));
    return v;
}
WINAPI void ctx_InsertWaitOnFence(void* self, UINT flags, uint64_t fence) {  // 155: wait on a fence value
    FL_LOG(fence, "%s ctx %p InsertWaitOnFence(0x%x, 0x%llx)", C::from(self)->deferred ? "deferred" : "immediate",
           self, flags, static_cast<unsigned long long>(fence));
}
// 156-159 Remap{ConstantBuffer,ShaderResource,Sampler,VertexBuffer}Inheritance
// (dst stage, dst slot, src stage, src slot), stages as in compact commands
// (0 VS, 1 HS, 2 DS, 3 GS, 4 PS, 5 CS). Inside a draw bundle: from here on,
// slot (stage, slot) uses the executing context's binding of (src stage, src
// slot) as it was when the bundle started (Azimuth records it as packet kind
// 0xc, rva 0x12c0c0/0x12c230, and applies it at replay, rva 0x1c98f0). The
// game passes per-object textures and constants this way: the executing
// context binds them to CS slots, bundles remap them into PS t13-t23 and the
// VS/PS constant slots 5/6/9/12, between draws. The reference umd's slots are
// `ret 0` on the immediate context; there they do nothing.
extern "C" void dxvk_xbox_remap_inheritance(void* ctx, UINT kind, UINT dst_stage, UINT dst_slot, UINT src_stage,
                                            UINT src_slot);
void dx_remap(C* c, UINT kind, UINT dst_stage, UINT dst_slot, UINT src_stage, UINT src_slot) {
    dxvk_xbox_remap_inheritance(c->dxvk, kind, dst_stage, dst_slot, src_stage, src_slot);
}
template <int Kind>
WINAPI void ctx_RemapInheritance(void* self, UINT stage, UINT slot, UINT src_stage, UINT src_slot) {
    static const char* kKind[4] = {"CB", "SRV", "sampler", "VB"};
    C* c = C::from(self);
    FL_LOG(remap, "%s %p remap %s stage %u slot %u <- stage %u slot %u (after %u draws)",
           c->bundle ? "bundle" : c->deferred ? "deferred" : "immediate", self, kKind[Kind], stage, slot, src_stage,
           src_slot, c->recorded_draws);
    if (!c->bundle) {
        log_once("Remap*Inheritance outside a draw bundle (ignored), kind", Kind);
        return;
    }
    static const UINT kDxvkStage[6] = {1 /* VS */, 3 /* HS */, 4 /* DS */, 2 /* GS */, 0 /* PS */, 5 /* CS */};
    if (stage > 5 || src_stage > 5) {
        log_once("Remap*Inheritance with a bad stage", stage << 8 | src_stage);
        return;
    }
    dx_remap(c, Kind, kDxvkStage[stage], slot, kDxvkStage[src_stage], src_slot);
}
// 197: EVENT_WRITE of event `ev` (0x407 = CS_PARTIAL_FLUSH after dispatches).
// 0x80000001 takes another umd path (0xa89b0) that we have not decoded.
WINAPI void ctx_InsertGpuEvent(void*, int ev) {
    if (ev < 0) log_once("ctx slot 197 special event", static_cast<uint32_t>(ev));
}

// 214: EVENT_WRITE_EOP writing a 32-bit value to a CPU-visible address once the
// preceding GPU work is done; the game polls it as a frame fence (game rva
// 0xaa679e). DXVK took its own copies of the game's memory when the work was
// recorded (placement resources are snapshotted at bind/draw), so the CPU may
// reuse that memory right away: write the value now.
WINAPI void ctx_WriteValueBottomOfPipe(void*, uint32_t* addr, uint32_t value) {
    __atomic_store_n(addr, value, __ATOMIC_RELEASE);
}

// 222 IASetPrimitiveResetIndex(u32): umd stores it at ctx+0xa024 for the
// VGT reset index. D3D11/DXVK always cut strips at 0xFFFF / 0xFFFFFFFF, so
// those values need nothing; any other value can't be expressed.
WINAPI void ctx_IASetPrimitiveResetIndex(void*, uint32_t index) {
    if (index != 0xFFFF && index != 0xFFFFFFFF) log_once("IASetPrimitiveResetIndex: unsupported reset index", index);
}

// 223 SetShaderResourceViewMinLOD(view, float): a per-view LOD clamp (umd
// 0x17e60 writes it into the view's T#); see srv_set_min_lod.
WINAPI void ctx_SetShaderResourceViewMinLOD(void*, void* view, float min_lod) {
    if (view) srv_set_min_lod(view, min_lod);
}

// 240 SetResourceCompression(resource, flags): umd 0x17ec0 rewrites the
// DCC/HTILE enable bits in the resource's descriptors (and in bound targets'
// registers); the game passes flags 0 at rva 0x11420a3 while loading. Surface
// compression is the driver's business under Vulkan, so nothing to do.
WINAPI void ctx_SetResourceCompression(void*, void*, UINT) {}

// ---- Xbox fast binds (slots 160-177) ----
// One resource at a time, written straight into the constant-engine RAM:
// slots go stage by stage [PS, VS, GS, CS, HS, DS] x [constant buffer, SRV,
// sampler], args (slot index, object). Stage/kind verified by matching the CE
// RAM offsets against umd's desktop XSSetConstantBuffers/XSSetSamplers.
void bind_one(void* ctx, int stage, int kind, UINT slot, void* obj);
template <int Stage, int Kind>
WINAPI void ctx_FastBind(void* self, UINT slot, void* obj) {
    bind_one(ctxdx(self), Stage, Kind, slot, obj);
}

// ---- Xbox slot 209: compact binding commands ----
// With bit 0 of ctx+0x390 set, the game's inlined raw command-buffer writers
// send these instead (FINDINGS §3.1). A command is a u32 header followed by
// `count` 16-byte entries {u64 object (our wrapper), u64 extra}:
//   bit 31 = bind (always set), bits 28-30 = kind (0 SRV, 1 CB, 2 sampler),
//   bits 16-19 = stage (0 VS, 1 HS, 2 DS, 3 GS, 4 PS, 5 CS), bits 8-15 = first
//   slot, bits 0-7 = count (1..32). Header & 0x0fff0000 == 0x01060000 with kind
//   1 binds VERTEX BUFFERS instead: extra = stride << 48 | offset.
// Layout as Azimuth's decoder (its rva 0x1cbc80) checks it; other kinds have
// extra 0.
void bind_one(void* ctx, int stage, int kind, UINT slot, void* obj) {
    static const int cb[6] = {dc::VSSetConstantBuffers, dc::HSSetConstantBuffers, dc::DSSetConstantBuffers,
                              dc::GSSetConstantBuffers, dc::PSSetConstantBuffers, dc::CSSetConstantBuffers};
    static const int srv[6] = {dc::VSSetShaderResources, dc::HSSetShaderResources, dc::DSSetShaderResources,
                               dc::GSSetShaderResources, dc::PSSetShaderResources, dc::CSSetShaderResources};
    static const int smp[6] = {dc::VSSetSamplers, dc::HSSetSamplers, dc::DSSetSamplers,
                               dc::GSSetSamplers, dc::PSSetSamplers, dc::CSSetSamplers};
    void* p = bind_unwrap(ctx, obj);
    if (stage == 5 && kind == 0 && trace::active())
        trace::event("bind CS t%u <- %s [ctx %p]", slot, p ? trace::describe_dxvk(p) : "null", ctx);
    const int* table = kind == 0 ? srv : kind == 1 ? cb : smp;
    dx<void>(ctx, table[stage], slot, 1u, &p);
}

WINAPI void ctx_ExecuteCompactCommands(void* self, const uint8_t* begin, const uint8_t* end) {
    void* ctx = ctxdx(self);
    for (const uint8_t* p = begin; p + 4 <= end;) {
        uint32_t op;
        std::memcpy(&op, p, 4);
        uint32_t count = op & 0xff, slot = (op >> 8) & 0xff, stage = (op >> 16) & 0xf, kind = (op >> 28) & 7;
        bool vertex_buffers = (op & 0x0fff0000) == 0x01060000 && kind == 1;
        if (!(op >> 31) || count == 0 || count > 32 || kind > 2 || p + 4 + count * 16 > end ||
            (!vertex_buffers && ((op & 0x0ff00000) || stage > 5)))
            fatal("d3d: unknown compact command header 0x%08x (ctx slot 209)", op);
        const uint8_t* e = p + 4;
        if (vertex_buffers) {
            void* bufs[32];
            UINT strides[32], offsets[32];
            for (uint32_t i = 0; i < count; i++) {
                uint64_t obj, extra;
                std::memcpy(&obj, e + i * 16, 8);
                std::memcpy(&extra, e + i * 16 + 8, 8);
                bufs[i] = bind_unwrap(ctx, reinterpret_cast<void*>(obj));
                strides[i] = static_cast<UINT>(extra >> 48);
                offsets[i] = static_cast<UINT>(extra & 0xffffffffffffull);
            }
            dx<void>(ctx, dc::IASetVertexBuffers, slot, count, bufs, strides, offsets);
        } else {
            for (uint32_t i = 0; i < count; i++) {
                uint64_t obj, extra;
                std::memcpy(&obj, e + i * 16, 8);
                std::memcpy(&extra, e + i * 16 + 8, 8);
                if (extra) fatal("d3d: compact command 0x%08x entry %u has extra 0x%llx", op, i, (unsigned long long)extra);
                bind_one(ctx, stage, kind, slot + i, reinterpret_cast<void*>(obj));
            }
        }
        p = e + count * 16;
    }
}

// Built once; every context copies it into its block.
void** context_table() {
    static void** table = [] {
        auto** t = new void*[C::kSlots];
        for (int i = 0; i < C::kSlots; i++) {
            char name[64];
            std::snprintf(name, sizeof(name), "slot %d (ctx+0x%zx)", i, C::kTable + 8 * i);
            t[i] = loader::make_named_trap("ID3D11DeviceContextX", name);
        }
        add_context_methods(t);
        return t;
    }();
    return table;
}

}  // namespace

std::string context_stats() {
    char buf[160];
    snprintf(buf, sizeof buf, "%llu command lists executed (%llu placement syncs), %llu placement binds",
             (unsigned long long)g_ecls.exchange(0), (unsigned long long)g_ecl_syncs.exchange(0),
             (unsigned long long)g_bind_syncs.exchange(0));
    return buf;
}

void add_context_methods(void** t) {
    auto set = [&](int slot, auto fn) { t[slot] = reinterpret_cast<void*>(fn); };
    auto setd = [&](int desktop_slot, auto fn) { t[ctx_slot(desktop_slot)] = reinterpret_cast<void*>(fn); };
    set(0, &ctx_QueryInterface);
    set(1, &ctx_AddRef);
    set(2, &ctx_Release);
    set(3, &ctx_GetDevice);
    set(4, &ctx_GetPrivateData);
    set(5, &ctx_SetPrivateData);
    set(6, &ctx_SetPrivateDataInterface);

#define FL_STAGE_SLOTS(S)                                         \
    setd(dc::S##SetConstantBuffers, &ctx_##S##SetConstantBuffers); \
    setd(dc::S##SetShaderResources, &ctx_##S##SetShaderResources); \
    setd(dc::S##SetSamplers, &ctx_##S##SetSamplers);               \
    setd(dc::S##SetShader, &ctx_##S##SetShader);                   \
    setd(dc::S##GetConstantBuffers, &ctx_##S##GetConstantBuffers); \
    setd(dc::S##GetShaderResources, &ctx_##S##GetShaderResources); \
    setd(dc::S##GetSamplers, &ctx_##S##GetSamplers);               \
    setd(dc::S##GetShader, &ctx_##S##GetShader);                   \
    setd(dc::S##SetConstantBuffers1, &ctx_##S##SetConstantBuffers1);
    FL_STAGE_SLOTS(VS)
    FL_STAGE_SLOTS(PS)
    FL_STAGE_SLOTS(GS)
    FL_STAGE_SLOTS(HS)
    FL_STAGE_SLOTS(DS)
    FL_STAGE_SLOTS(CS)
#undef FL_STAGE_SLOTS

    setd(dc::IASetInputLayout, &ctx_IASetInputLayout);
    setd(dc::IASetVertexBuffers, &ctx_IASetVertexBuffers);
    set(20, &ctx_IASetIndexBufferX);  // Xbox signature, see above
    setd(dc::IAGetInputLayout, &ctx_IAGetInputLayout);
    setd(dc::IAGetVertexBuffers, &ctx_IAGetVertexBuffers);
    setd(dc::IAGetIndexBuffer, &ctx_IAGetIndexBuffer);
    setd(dc::IAGetPrimitiveTopology, &ctx_IAGetPrimitiveTopology);

    setd(dc::DrawIndexed, &ctx_DrawIndexed);
    setd(dc::Draw, &ctx_Draw);
    setd(dc::DrawIndexedInstanced, &ctx_DrawIndexedInstanced);
    setd(dc::DrawInstanced, &ctx_DrawInstanced);
    setd(dc::DrawAuto, &ctx_DrawAuto);
    setd(dc::DrawIndexedInstancedIndirect, &ctx_DrawIndexedInstancedIndirect);
    setd(dc::DrawInstancedIndirect, &ctx_DrawInstancedIndirect);
    setd(dc::Dispatch, &ctx_Dispatch);
    setd(dc::DispatchIndirect, &ctx_DispatchIndirect);

    setd(dc::Map, &ctx_Map);
    setd(dc::Unmap, &ctx_Unmap);
    setd(dc::CopySubresourceRegion, &ctx_CopySubresourceRegion);
    setd(dc::CopySubresourceRegion1, &ctx_CopySubresourceRegion1);
    setd(dc::CopyResource, &ctx_CopyResource);
    setd(dc::UpdateSubresource, &ctx_UpdateSubresource);
    setd(dc::UpdateSubresource1, &ctx_UpdateSubresource1);
    setd(dc::CopyStructureCount, &ctx_CopyStructureCount);
    setd(dc::ClearRenderTargetView, &ctx_ClearRenderTargetView);
    setd(dc::ClearUnorderedAccessViewUint, &ctx_ClearUnorderedAccessViewUint);
    setd(dc::ClearUnorderedAccessViewFloat, &ctx_ClearUnorderedAccessViewFloat);
    setd(dc::ClearDepthStencilView, &ctx_ClearDepthStencilView);
    setd(dc::GenerateMips, &ctx_GenerateMips);
    setd(dc::SetResourceMinLOD, &ctx_SetResourceMinLOD);
    setd(dc::GetResourceMinLOD, &ctx_GetResourceMinLOD);
    setd(dc::ResolveSubresource, &ctx_ResolveSubresource);
    setd(dc::DiscardResource, &ctx_DiscardResource);
    setd(dc::DiscardView, &ctx_DiscardView);
    setd(dc::ClearView, &ctx_ClearView);

    setd(dc::Begin, &ctx_Begin);
    setd(dc::End, &ctx_End);
    setd(dc::GetData, &ctx_GetData);
    setd(dc::SetPredication, &ctx_SetPredication);
    setd(dc::GetPredication, &ctx_GetPredication);

    setd(dc::OMSetRenderTargets, &ctx_OMSetRenderTargets);
    setd(dc::OMSetRenderTargetsAndUnorderedAccessViews, &ctx_OMSetRenderTargetsAndUnorderedAccessViews);
    setd(dc::OMSetBlendState, &ctx_OMSetBlendState);
    setd(dc::OMSetDepthStencilState, &ctx_OMSetDepthStencilState);
    setd(dc::OMGetRenderTargets, &ctx_OMGetRenderTargets);
    setd(dc::OMGetRenderTargetsAndUnorderedAccessViews, &ctx_OMGetRenderTargetsAndUnorderedAccessViews);
    setd(dc::OMGetBlendState, &ctx_OMGetBlendState);
    setd(dc::OMGetDepthStencilState, &ctx_OMGetDepthStencilState);
    setd(dc::SOSetTargets, &ctx_SOSetTargets);
    setd(dc::SOGetTargets, &ctx_SOGetTargets);
    setd(dc::RSSetState, &ctx_RSSetState);
    setd(dc::RSSetViewports, &ctx_RSSetViewports);
    setd(dc::RSSetScissorRects, &ctx_RSSetScissorRects);
    setd(dc::RSGetState, &ctx_RSGetState);
    setd(dc::RSGetViewports, &ctx_RSGetViewports);
    setd(dc::RSGetScissorRects, &ctx_RSGetScissorRects);
    setd(dc::CSSetUnorderedAccessViews, &ctx_CSSetUnorderedAccessViews);
    setd(dc::CSGetUnorderedAccessViews, &ctx_CSGetUnorderedAccessViews);

    setd(dc::ClearState, &ctx_ClearState);
    setd(dc::Flush, &ctx_Flush);
    setd(dc::GetType, &ctx_GetType);
    setd(dc::GetContextFlags, &ctx_GetContextFlags);
    setd(dc::FinishCommandList, &ctx_FinishCommandList);
    setd(dc::ExecuteCommandList, &ctx_ExecuteCommandList);
    setd(dc::IsAnnotationEnabled, &ctx_IsAnnotationEnabled);
    setd(dc::SetMarkerInt, &ctx_SetMarkerInt);
    setd(dc::BeginEventInt, &ctx_BeginEventInt);
    setd(dc::EndEvent, &ctx_EndEvent);

    set(151, &ctx_FlushGpuCaches);
    set(152, &ctx_FlushGpuCacheRange);
    set(153, &ctx_InsertWaitUntilIdle);
    set(154, &ctx_InsertFence);
    set(155, &ctx_InsertWaitOnFence);
    set(156, &ctx_RemapInheritance<0>);
    set(157, &ctx_RemapInheritance<1>);
    set(158, &ctx_RemapInheritance<2>);
    set(159, &ctx_RemapInheritance<3>);
    set(196, &ctx_Slot196);
    set(197, &ctx_InsertGpuEvent);
    set(209, &ctx_ExecuteCompactCommands);
    set(214, &ctx_WriteValueBottomOfPipe);
    set(222, &ctx_IASetPrimitiveResetIndex);
    set(223, &ctx_SetShaderResourceViewMinLOD);
    set(240, &ctx_SetResourceCompression);

    // bind_one stage codes: 0 VS, 1 HS, 2 DS, 3 GS, 4 PS, 5 CS; kinds: 0 SRV, 1 CB, 2 sampler.
    constexpr int kFastStages[6] = {4, 0, 3, 5, 1, 2};  // PS, VS, GS, CS, HS, DS
#define FL_FAST(i)                                                     \
    set(160 + 3 * i + 0, &ctx_FastBind<kFastStages[i], 1>);            \
    set(160 + 3 * i + 1, &ctx_FastBind<kFastStages[i], 0>);            \
    set(160 + 3 * i + 2, &ctx_FastBind<kFastStages[i], 2>);
    FL_FAST(0) FL_FAST(1) FL_FAST(2) FL_FAST(3) FL_FAST(4) FL_FAST(5)
#undef FL_FAST
}

ContextX* make_context(DeviceX* d, void* dxvk_ctx, bool deferred) {
    auto* c = new ContextX();
    std::memset(c->block, 0, sizeof(c->block));
    c->device = d;
    c->dxvk = dxvk_ctx;
    c->deferred = deferred;
    if (deferred) d->add_ref();
    std::memcpy(c->table(), context_table(), C::kSlots * sizeof(void*));
    *reinterpret_cast<void***>(c->block) = c->table();
    *reinterpret_cast<uint32_t*>(c->block + 8) = 1;  // umd's constructor stores 1 here
    *reinterpret_cast<void**>(c->block + C::kDevicePtr) = &d->device_iface;
    // Route the game's inlined context code through our table (bit 1) and turn
    // its raw command-buffer writes into slot 209 commands (bit 0).
    c->block[C::kModeFlags] |= 3;
    return c;
}

}  // namespace fl::d3d
