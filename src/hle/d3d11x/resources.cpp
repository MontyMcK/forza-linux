// Buffers, textures and views.
//
// Xbox resource descriptors are the desktop ones plus two trailing u32 fields
// (umd copies both into the same internal slots for every resource type;
// probably ESRAM offset/size, FINDINGS §3.1). Xbox-only flag bits are stripped
// before DXVK sees them, and logged once.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "loader/pe_image.h"
#include "hle/d3d11x/trace.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Vtable;
namespace dd = desktop::dev;

constexpr UINT kDesktopBindMask = 0x3ff;     // VERTEX_BUFFER .. VIDEO_ENCODER
constexpr UINT kDesktopCpuMask = 0x30000;    // CPU_ACCESS_WRITE | READ
constexpr UINT kDesktopMiscMask = 0x1 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80;  // GENERATE_MIPS, TEXTURECUBE,
                                                                           // DRAWINDIRECT, RAW, STRUCTURED, CLAMP

void sanitize(const char* what, UINT* bind, UINT* cpu, UINT* misc) {
    if (*bind & ~kDesktopBindMask) log_once((std::string(what) + ": Xbox-only BindFlags").c_str(), *bind);
    if (*cpu & ~kDesktopCpuMask) log_once((std::string(what) + ": Xbox-only CPUAccessFlags").c_str(), *cpu);
    if (*misc & ~kDesktopMiscMask) log_once((std::string(what) + ": Xbox-only MiscFlags").c_str(), *misc);
    *bind &= kDesktopBindMask;
    *cpu &= kDesktopCpuMask;
    *misc &= kDesktopMiscMask;
}

// ---- Resource vtables (Xbox, 13 slots: umd 0x16f750) ----
// 8 GetType, 9 SetEvictionPriority, 10 GetEvictionPriority, 11 Xbox extra, 12 GetDesc.

WINAPI void res_GetType(Iface* self, int* dim) { *dim = self->as<Resource>()->dim; }
WINAPI void res_SetEvictionPriority(Iface* self, UINT) {}
WINAPI UINT res_GetEvictionPriority(Iface* self) { return 0; }
// umd: `*out = this`. What it means is unknown; do the same.
WINAPI void res_Slot11(Iface* self, void** out) {
    FL_STUB_ONCE("ID3D11Resource (Xbox) slot 11 returns the object itself, like umd");
    *out = self;
}
WINAPI void res_GetDesc(Iface* self, void* desc) {
    auto* r = self->as<Resource>();
    std::memcpy(desc, r->xdesc, r->xdesc_size);
}

Vtable make_resource_vt(const char* name) {
    Vtable v(name, 24, false);  // 13 known + spare traps
    child_base(v)
        .set(8, &res_GetType)
        .set(9, &res_SetEvictionPriority)
        .set(10, &res_GetEvictionPriority)
        .set(11, &res_Slot11)
        .set(12, &res_GetDesc);
    return v;
}
Vtable& buffer_vt() { static Vtable v = make_resource_vt("ID3D11BufferX"); return v; }
Vtable& tex1d_vt() { static Vtable v = make_resource_vt("ID3D11Texture1DX"); return v; }
Vtable& tex2d_vt() { static Vtable v = make_resource_vt("ID3D11Texture2DX"); return v; }
Vtable& tex3d_vt() { static Vtable v = make_resource_vt("ID3D11Texture3DX"); return v; }

// ---- Device: resource creation ----

DeviceX* dev_of(Iface* self) { return self->as<DeviceX>(); }

// Xbox resource descriptor -> desktop (FINDINGS §3.1): desktop fields + 2 u32.
// dim: 1 buffer (0x18 -> 0x20 bytes), 2 tex1d (0x20 -> 0x28), 3 tex2d (0x2c -> 0x34), 4 tex3d (0x24 -> 0x2c).
struct DimInfo {
    int desktop_words, usage, bind, cpu, misc, create_slot;
    size_t xbox_size;
    Vtable& (*vt)();
    const char16_t* name;
    const GUID* iid;
    const char* what;
};
const DimInfo& dim_info(int dim) {
    static const DimInfo info[5] = {
        {},
        {6, 1, 2, 3, 4, dd::CreateBuffer, 0x20, buffer_vt, u"D3D11Buffer", &IID_ID3D11Buffer, "CreateBuffer"},
        {8, 4, 5, 6, 7, dd::CreateTexture1D, 0x28, tex1d_vt, u"D3D11Texture1D", &IID_ID3D11Texture1D, "CreateTexture1D"},
        {11, 7, 8, 9, 10, dd::CreateTexture2D, 0x34, tex2d_vt, u"D3D11Texture2D", &IID_ID3D11Texture2D, "CreateTexture2D"},
        {9, 5, 6, 7, 8, dd::CreateTexture3D, 0x2c, tex3d_vt, u"D3D11Texture3D", &IID_ID3D11Texture3D, "CreateTexture3D"},
    };
    return info[dim];
}

}  // namespace

winrt::Vtable& texture2d_vtable() { return tex2d_vt(); }

HRESULT create_resource(DeviceX* dev, int dim, const uint32_t* xd, const void* init, void** out, bool placement,
                        Resource** created) {
    const DimInfo& di = dim_info(dim);
    uint32_t d[16];
    std::memcpy(d, xd, di.desktop_words * 4);
    sanitize(di.what, &d[di.bind], &d[di.cpu], &d[di.misc]);
    uint32_t e0 = xd[di.desktop_words], e1 = xd[di.desktop_words + 1];
    if ((e0 || e1) && !placement)
        log_once((std::string(di.what) + ": Xbox extra fields set, ignored").c_str(), (uint64_t(e1) << 32) | e0);
    if (placement) {
        // Our copy of a placement resource is filled with UpdateSubresource, so it
        // must be DEFAULT usage without CPU access (game Maps return game memory).
        d[di.usage] = 0;
        d[di.cpu] = 0;
    }
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev->dxvk, di.create_slot, d, init, out ? &obj : nullptr);
    if (hr < 0) {
        char buf[256];
        int n = std::snprintf(buf, sizeof buf, "d3d: %s%s(", placement ? "placement " : "", di.what);
        for (int i = 0; i < di.desktop_words + 2 && n < (int)sizeof buf - 16; i++)
            n += std::snprintf(buf + n, sizeof buf - n, "%s%u", i ? " " : "", xd[i]);
        FL_ERROR("%s) failed: 0x%08x", buf, static_cast<uint32_t>(hr));
    }
    if (hr < 0 || !out) {
        if (out) *out = nullptr;
        return hr;
    }
    auto* r = new Resource(di.name, dev, di.vt(), obj, dim, xd, di.xbox_size);
    if (e0 || e1)
        FL_LOG(esram, "%s %ux%u fmt %u bind 0x%x: extra fields 0x%x 0x%x -> dxvk %p", di.what, xd[0], dim >= 3 ? xd[1] : 1,
               xd[dim == 3 ? 4 : dim == 4 ? 4 : 3], d[di.bind], e0, e1, obj);
    // MipLevels 0 asks for the full chain; GetDesc must then report the real
    // count (the game sizes its per-mip view arrays from it, e.g. the
    // reflection pyramid at game rva 0x10d8bb0). Desktop texture GetDesc = slot 10.
    if (int mip_index = dim == 2 ? 1 : dim == 3 ? 2 : dim == 4 ? 3 : -1;
        mip_index >= 0 && reinterpret_cast<uint32_t*>(r->xdesc)[mip_index] == 0) {
        uint32_t d[12] = {};
        dx<void>(obj, 10, d);
        reinterpret_cast<uint32_t*>(r->xdesc)[mip_index] = d[mip_index];
    }
    r->expose(*di.iid, &r->iface);
    *out = &r->iface;
    if (created) *created = r;
    return hr;
}

namespace {

WINAPI HRESULT dev_CreateBuffer(Iface* self, const uint32_t* xd, const void* init, void** out) {
    // FL_LOG=create: D3D11_BUFFER_DESC words (bytes, usage, bind, cpu, misc, stride) and the caller.
    FL_LOG(create, "CreateBuffer %u B usage %u bind 0x%x cpu 0x%x misc 0x%x stride %u init %p extra 0x%x 0x%x from game rva 0x%llx",
           xd[0], xd[1], xd[2], xd[3], xd[4], xd[5], init, xd[6], xd[7],
           (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                                reinterpret_cast<uintptr_t>(loader::main_image().base)));
    // MiscFlags 0x20000 (Xbox): the game's raw view of a depth target's HTILE
    // (game rva 0x10c9594: 128 KiB = 4 bytes per 8x8 tile of 1920x1080). Its
    // compute passes read it to skip work per tile: Z mask (bits 0-3) 0 = cleared
    // tile, 15 = read every pixel's depth; stencil state (bits 8-9) 0 = no stencil,
    // 2-3 = read every pixel's stencil. DXVK keeps no HTILE, so all zeros made every
    // tile "cleared, no stencil": the motion-blur mask lost the car (blurred) and
    // the depth pass wrote the clear depth everywhere. 0xffffffff is the expanded
    // state (what the game's own DMA clears write): every tile takes the exact
    // per-pixel path.
    if ((xd[4] & 0x20000) && !init) {
        FL_STUB_ONCE("d3d: HTILE buffer (MiscFlags 0x20000) filled with 0xffffffff (expanded tiles)");
        std::vector<uint32_t> expanded(xd[0] / 4, 0xffffffffu);
        struct { const void* mem; UINT pitch, slice; } data{expanded.data(), xd[0], xd[0]};
        return create_resource(dev_of(self), 1, xd, &data, out, false, nullptr);
    }
    return create_resource(dev_of(self), 1, xd, init, out, false, nullptr);
}
WINAPI HRESULT dev_CreateTexture1D(Iface* self, const uint32_t* xd, const void* init, void** out) {
    return create_resource(dev_of(self), 2, xd, init, out, false, nullptr);
}
WINAPI HRESULT dev_CreateTexture2D(Iface* self, const uint32_t* xd, const void* init, void** out) {
    // FL_LOG=create: who creates which texture (to find the code behind a render target).
    if (log_enabled("create") && init) {
        struct SubData { const uint8_t* mem; uint32_t pitch, slice; };
        auto* sd = static_cast<const SubData*>(init);
        uint32_t n = std::min<uint32_t>(xd[2] ? xd[2] : 1, 12);
        std::string subs;
        for (uint32_t i = 0; i < n; i++) {
            char b[64];
            uint32_t first = 0;
            if (sd[i].mem) std::memcpy(&first, sd[i].mem, 4);
            snprintf(b, sizeof b, " [%u: %p pitch %u first 0x%x]", i, (const void*)sd[i].mem, sd[i].pitch, first);
            subs += b;
        }
        FL_LOG(create, "  init (usage %u cpu 0x%x):%s", xd[7], xd[9], subs.c_str());
    }
    FL_LOG(create, "CreateTexture2D %ux%u mips %u arr %u fmt %u samples %u bind 0x%x misc 0x%x from game rva 0x%llx", xd[0],
           xd[1], xd[2], xd[3], xd[4], xd[5], xd[8], xd[10],
           (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                                reinterpret_cast<uintptr_t>(loader::main_image().base)));
    // FL_CREATE_STACK_FMT=<fmt>: also print the game return addresses on the
    // stack (a rough backtrace) when a texture of that format is created.
    static const int stack_fmt = getenv("FL_CREATE_STACK_FMT") ? atoi(getenv("FL_CREATE_STACK_FMT")) : -1;
    if (int(xd[4]) == stack_fmt) {
        const loader::PeImage& img = loader::main_image();
        auto* sp = static_cast<const uint64_t*>(__builtin_frame_address(0));
        std::string bt;
        for (int i = 0, found = 0; i < 1024 && found < 16; i++) {
            uint64_t v = sp[i];
            if (v > uint64_t(reinterpret_cast<uintptr_t>(img.base)) &&
                v < uint64_t(reinterpret_cast<uintptr_t>(img.base)) + img.size) {
                char b[24];
                snprintf(b, sizeof b, " 0x%llx", (unsigned long long)(v - reinterpret_cast<uintptr_t>(img.base)));
                bt += b, found++;
            }
        }
        FL_LOG(create, "  stack:%s", bt.c_str());
    }
    return create_resource(dev_of(self), 3, xd, init, out, false, nullptr);
}
WINAPI HRESULT dev_CreateTexture3D(Iface* self, const uint32_t* xd, const void* init, void** out) {
    return create_resource(dev_of(self), 4, xd, init, out, false, nullptr);
}

// ---- Views (Xbox: 8 GetResource, 9 GetDesc; umd 0x117970 etc.) ----

// All view types share the desktop GetDesc slot (7: after ID3D11View::GetResource).
constexpr int kDesktopViewGetDesc = 8;
constexpr int kViewGetDescSlot = kDesktopViewGetDesc;

struct View : DeviceChild {
    View(const char16_t* name, DeviceX* dev, Vtable& vt, void* obj, DeviceChild* resource, int desc_slot)
        : DeviceChild(name, dev, vt, obj), res(resource), desktop_getdesc(desc_slot), full(obj) {
        res->add_ref();
        expose(IID_ID3D11View, &iface);
    }
    ~View() override {
        dxvk = full;  // the base class releases it
        for (auto& [k, obj] : clamped) unregister_alias(obj, this), dx_release(obj);
        res->release();
    }
    Resource* backing() override { return res->backing(); }
    DeviceChild* res;
    int desktop_getdesc;
    // SetShaderResourceViewMinLOD: `dxvk` then points at a view of the same
    // resource that starts `min_mip` mips further down; `full` is the view
    // the game created. Clamped views are kept (by first mip) until the view
    // dies: command lists recorded with one still reference it.
    void* full;
    std::mutex clamp_mutex;
    std::unordered_map<uint32_t, void*> clamped;
};

WINAPI void view_GetResource(Iface* self, void** out) {
    auto* v = self->as<View>();
    v->res->add_ref();
    *out = &v->res->iface;
}
// View descriptors are the desktop layouts (unverified for Xbox-only formats); DXVK fills them.
WINAPI void view_GetDesc(Iface* self, void* desc) {
    auto* v = self->as<View>();
    dx<void>(v->dxvk, v->desktop_getdesc, desc);
}

Vtable make_view_vt(const char* name) {
    Vtable v(name, 16, false);  // 10-12 known + spare traps
    child_base(v).set(8, &view_GetResource).set(9, &view_GetDesc);
    return v;
}
Vtable& srv_vt() { static Vtable v = make_view_vt("ID3D11ShaderResourceViewX"); return v; }
Vtable& rtv_vt() { static Vtable v = make_view_vt("ID3D11RenderTargetViewX"); return v; }
Vtable& dsv_vt() { static Vtable v = make_view_vt("ID3D11DepthStencilViewX"); return v; }
Vtable& uav_vt() { static Vtable v = make_view_vt("ID3D11UnorderedAccessViewX"); return v; }



HRESULT create_view(Iface* self, int desktop_slot, const void* res, const void* desc, void** out, Vtable& vt,
                    const char16_t* name, const GUID& iid, const char* what) {
    DeviceChild* r = child_of(res);
    if (!r) return E_INVALIDARG;
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, desktop_slot, r->dxvk, desc, out ? &obj : nullptr);
    Resource* rr = r->backing();
    if (hr < 0 && desktop_slot == dd::CreateShaderResourceView && desc && rr && rr->dim == 3 && out) {
        // The console reinterprets a texture's memory in any view format (e.g.
        // R8_UINT over an R11G11B10_FLOAT target at startup); D3D11 forbids
        // views across format families. STUB: such a view reads a zeroed
        // stand-in texture of the view's format, so the game's setup that
        // creates it completes instead of half-failing.
        const uint32_t* vd = static_cast<const uint32_t*>(desc);
        uint32_t td[11];
        memcpy(td, rr->desc32(), sizeof td);
        td[2] = 1, td[3] = 1, td[4] = vd[0], td[5] = 1, td[6] = 0, td[7] = 0, td[8] = 0x8, td[9] = 0, td[10] = 0;
        void* stand_in = nullptr;
        if (dx<HRESULT>(dev_of(self)->dxvk, dd::CreateTexture2D, td, nullptr, &stand_in) >= 0) {
            uint32_t vd2[6] = {vd[0], vd[1], 0, 1, 0, 0};
            hr = dx<HRESULT>(dev_of(self)->dxvk, desktop_slot, stand_in, vd2, &obj);
            dx_release(stand_in);
            if (hr >= 0) {
                log_once("STUB: cross-format SRV backed by a zeroed stand-in texture, view format", vd[0]);
                auto* v = new View(name, dev_of(self), vt, obj, r, kDesktopViewGetDesc);
                v->expose(iid, &v->iface);
                *out = &v->iface;
                return hr;
            }
        }
    }
    if (hr < 0) {
        const uint32_t* dw = static_cast<const uint32_t*>(desc);
        if (dw)
            FL_ERROR("d3d: %s(fmt=%u dim=%u %u %u %u %u) of [%s] failed: 0x%08x", what, dw[0], dw[1], dw[2], dw[3],
                     dw[4], dw[5], trace::describe_dxvk(r->dxvk), static_cast<uint32_t>(hr));
        else
            FL_ERROR("d3d: %s(no desc) failed: 0x%08x", what, static_cast<uint32_t>(hr));
    }
    if (hr < 0 || !out) {
        if (out) *out = nullptr;
        return hr;
    }
    auto* v = new View(name, dev_of(self), vt, obj, r, kDesktopViewGetDesc);
    v->expose(iid, &v->iface);
    *out = &v->iface;
    if (log_enabled("view")) {
        const uint32_t* dw = static_cast<const uint32_t*>(desc);
        log_write("view", "%s(fmt %u dim %u %u %u) of [%s] -> %p", what, dw ? dw[0] : 0, dw ? dw[1] : 0, dw ? dw[2] : 0,
                  dw ? dw[3] : 0, trace::describe_dxvk(r->dxvk), *out);
    }
    return hr;
}

}  // namespace

// Xbox context slot 223 SetShaderResourceViewMinLOD(view, float): umd 0x17e60
// writes the clamp into the view's T# (MIN_LOD, word 1 bits 8-19, 4.8 fixed
// point, in resource mips, at least the view's first mip), so every later bind of the
// view samples no finer mip than that. The game's texture streamer uses it to
// keep the GPU off big mips that have not arrived (their memory holds zeros,
// which BC7 decodes to transparent black). D3D11 has no per-view clamp and
// DXVK ignores SetResourceMinLOD, so we swap in a view whose most detailed mip
// is ceil(min_lod): for sampling with derivatives that picks the same mip as
// the clamp. STUB: Load/GetDimensions see the clamped view's mip numbering.
void srv_set_min_lod(void* game_view, float min_lod) {
    auto* v = static_cast<View*>(child_of(game_view));
    struct { uint32_t format, dim, u[4]; } d{};
    dx<void>(v->full, kViewGetDescSlot, &d);
    bool has_mips = d.dim >= 2 && d.dim <= 10 && d.dim != 6 && d.dim != 7;  // not buffers or MSAA
    if (!has_mips) return;
    // The clamp counts resource mips, not view mips (umd raises it to the
    // view's first mip and stores it as 4.8 fixed point in T# word 1 bits 8-19).
    uint32_t base = d.u[0], levels = d.u[1];
    uint32_t first = min_lod > 0 ? static_cast<uint32_t>(std::ceil(std::min(min_lod, 15.996f))) : 0;
    uint32_t k = first > base ? first - base : 0;
    if (levels && k > levels - 1) k = levels - 1;
    FL_LOG(lod, "view %p [%s] min LOD %.2f -> first mip %u of %u", game_view, trace::describe_dxvk(v->res->dxvk),
           min_lod, base + k, base + levels);
    if (k == 0) {
        v->dxvk = v->full;
        return;
    }
    std::lock_guard lock(v->clamp_mutex);
    if (auto it = v->clamped.find(k); it != v->clamped.end()) {
        v->dxvk = it->second;
        return;
    }
    d.u[0] += k;
    d.u[1] = levels - k;
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(v->device->dxvk, dd::CreateShaderResourceView, v->res->dxvk, &d, &obj);
    if (hr < 0) {
        FL_ERROR("d3d: SetShaderResourceViewMinLOD: clamped view (first mip %u) failed: 0x%08x", d.u[0],
                 static_cast<uint32_t>(hr));
        return;
    }
    log_once("STUB: SetShaderResourceViewMinLOD emulated with a view starting at the clamped mip", 0);
    v->clamped[k] = obj;
    register_alias(obj, v);
    v->dxvk = obj;
}

namespace {

WINAPI HRESULT dev_CreateShaderResourceView(Iface* self, const void* res, const void* desc, void** out) {
    return create_view(self, dd::CreateShaderResourceView, res, desc, out, srv_vt(), u"D3D11ShaderResourceView",
                       IID_ID3D11ShaderResourceView, "CreateShaderResourceView");
}
WINAPI HRESULT dev_CreateUnorderedAccessView(Iface* self, const void* res, const void* desc, void** out) {
    return create_view(self, dd::CreateUnorderedAccessView, res, desc, out, uav_vt(), u"D3D11UnorderedAccessView",
                       IID_ID3D11UnorderedAccessView, "CreateUnorderedAccessView");
}
WINAPI HRESULT dev_CreateRenderTargetView(Iface* self, const void* res, const void* desc, void** out) {
    return create_view(self, dd::CreateRenderTargetView, res, desc, out, rtv_vt(), u"D3D11RenderTargetView",
                       IID_ID3D11RenderTargetView, "CreateRenderTargetView");
}
WINAPI HRESULT dev_CreateDepthStencilView(Iface* self, const void* res, const void* desc, void** out) {
    return create_view(self, dd::CreateDepthStencilView, res, desc, out, dsv_vt(), u"D3D11DepthStencilView",
                       IID_ID3D11DepthStencilView, "CreateDepthStencilView");
}

}  // namespace

void add_resource_methods(Vtable& v) {
    v.set(dev_slot(dd::CreateBuffer), &dev_CreateBuffer)
        .set(dev_slot(dd::CreateTexture1D), &dev_CreateTexture1D)
        .set(dev_slot(dd::CreateTexture2D), &dev_CreateTexture2D)
        .set(dev_slot(dd::CreateTexture3D), &dev_CreateTexture3D)
        .set(dev_slot(dd::CreateShaderResourceView), &dev_CreateShaderResourceView)
        .set(dev_slot(dd::CreateUnorderedAccessView), &dev_CreateUnorderedAccessView)
        .set(dev_slot(dd::CreateRenderTargetView), &dev_CreateRenderTargetView)
        .set(dev_slot(dd::CreateDepthStencilView), &dev_CreateDepthStencilView);
}

}  // namespace fl::d3d
