// D3D11XCreateDeviceX, the device's own methods, and the DXGI wrappers.
// Resource/view creation is in resources.cpp, shaders/states in pipeline.cpp.
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/log.h"
#include "common/wstr.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/d3d11x/gpumem.h"
#include "hle/registry.h"
#include "loader/imports.h"

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Object;
using winrt::Vtable;
namespace dd = desktop::dev;

// D3D11XCreateDeviceX's parameter struct, as the game fills it (rva 0x10d87d6).
// Field meanings are guesses until something reads them.
struct CreateDeviceParams {
    UINT version;       // +0x00 0x1000B
    UINT flags;         // +0x04 0x04000000
    uint8_t zero08[16]; // +0x08
    uint64_t mask18;    // +0x18 0x3F (core mask?)
    uint64_t zero20;    // +0x20
    UINT zero28;        // +0x28
};

DeviceX* dev_of(Iface* self) { return self->as<DeviceX>(); }

// IDXGIDevice (Xbox, 18 slots). Private data lives on the device.
WINAPI HRESULT dxgidev_SetPrivateData(Iface* self, const GUID& g, UINT size, const void* data) {
    return dev_of(self)->priv.set(g, size, data);
}
WINAPI HRESULT dxgidev_SetPrivateDataInterface(Iface* self, const GUID& g, void* unknown) {
    return dev_of(self)->priv.set_interface(g, unknown);
}
WINAPI HRESULT dxgidev_GetPrivateData(Iface* self, const GUID& g, UINT* size, void* data) {
    return dev_of(self)->priv.get(g, size, data);
}

// Xbox slot 8 (desktop 7). Game call at rva 0x10d8980.
WINAPI HRESULT dxgi_GetAdapter(Iface* self, void** out) {
    auto* d = dev_of(self);
    void* dxgi = nullptr;
    void* adapter = nullptr;
    HRESULT hr = dx_query(d->dxvk, IID_IDXGIDevice, &dxgi);
    if (hr >= 0) {
        hr = dx<HRESULT>(dxgi, desktop::dxgidev::GetAdapter, &adapter);
        dx_release(dxgi);
    }
    FL_LOG(d3d, "IDXGIDevice::GetAdapter -> 0x%08x", static_cast<uint32_t>(hr));
    if (hr < 0) {
        *out = nullptr;
        return hr;
    }
    *out = make_adapter(adapter);
    return S_OK;
}

Vtable& dxgi_device_vt() {
    static Vtable vt = Vtable("IDXGIDeviceX", kDxgiDeviceSlots, false)
                           .set(0, &d3d_QueryInterface)
                           .set(3, &dxgidev_SetPrivateData)
                           .set(4, &dxgidev_SetPrivateDataInterface)
                           .set(6, &dxgidev_GetPrivateData)
                           .set(dxgi_slot(desktop::dxgidev::GetAdapter), &dxgi_GetAdapter);
    return vt;
}

// ---- ID3D11DeviceX: the device's own methods ----

WINAPI HRESULT dev_CheckFormatSupport(Iface* self, UINT format, UINT* support) {
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CheckFormatSupport, format, support);
    if (hr < 0) log_once("CheckFormatSupport failed for format", format);
    return hr;
}
WINAPI HRESULT dev_CheckMultisampleQualityLevels(Iface* self, UINT format, UINT count, UINT* levels) {
    return dx<HRESULT>(dev_of(self)->dxvk, dd::CheckMultisampleQualityLevels, format, count, levels);
}
WINAPI HRESULT dev_CheckFeatureSupport(Iface* self, UINT feature, void* data, UINT size) {
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CheckFeatureSupport, feature, data, size);
    if (hr < 0) log_once("CheckFeatureSupport failed for feature", feature);
    return hr;
}
WINAPI HRESULT dev_GetPrivateData(Iface* self, const GUID& g, UINT* size, void* data) {
    return dev_of(self)->priv.get(g, size, data);
}
WINAPI HRESULT dev_SetPrivateData(Iface* self, const GUID& g, UINT size, const void* data) {
    return dev_of(self)->priv.set(g, size, data);
}
WINAPI HRESULT dev_SetPrivateDataInterface(Iface* self, const GUID& g, void* unknown) {
    return dev_of(self)->priv.set_interface(g, unknown);
}
WINAPI int dev_GetFeatureLevel(Iface* self) { return kFeatureLevel11_1; }  // umd returns 0xb100

// STUB: umd returns an internal field; we return the params flags (unverified).
WINAPI UINT dev_GetCreationFlags(Iface* self) {
    FL_STUB_ONCE("ID3D11DeviceX::GetCreationFlags returns the D3D11XCreateDeviceX flags");
    return dev_of(self)->creation_flags;
}
WINAPI HRESULT dev_GetDeviceRemovedReason(Iface* self) { return S_OK; }  // umd returns E_NOTIMPL; S_OK is "not removed"

WINAPI void dev_GetImmediateContext(Iface* self, void** out) {
    auto* d = dev_of(self);
    d->add_ref();  // the context shares the device's refcount (as in umd)
    *out = d->immediate;
}
// Xbox flags seen: 0x60000, 0xa0000. Bit 17 (0x20000) makes a draw bundle
// (Azimuth rva 0x128ff0 accepts only 0, 0x60000, 0xa0000): its command lists
// run on the executing context's current state. Our DXVK patch
// (tools/dxvk-patches/0001-xbox-draw-bundles.patch) implements that for
// deferred contexts created with the same bit. Bits 18/19: meaning unknown.
constexpr UINT kDeferredBundle = 0x20000;
WINAPI HRESULT dev_CreateDeferredContext(Iface* self, UINT flags, void** out) {
    if (flags & ~(kDeferredBundle | 0xc0000)) log_once("CreateDeferredContext: unknown Xbox flags", flags);
    auto* d = dev_of(self);
    void* ctx = nullptr;
    HRESULT hr = dx<HRESULT>(d->dxvk, dd::CreateDeferredContext, flags & kDeferredBundle, out ? &ctx : nullptr);
    if (hr < 0 || !out) {
        FL_ERROR("d3d: CreateDeferredContext failed: 0x%08x", static_cast<uint32_t>(hr));
        if (out) *out = nullptr;
        return hr;
    }
    ContextX* c = make_context(d, ctx, true);
    c->bundle = (flags & kDeferredBundle) != 0;
    *out = c;
    return S_OK;
}

// Xbox slot 69 (umd 0x2b560 -> 0x8d360): walks the device's list of objects
// waiting on GPU fences and frees the ones whose fences completed (deferred
// destruction). A game worker thread calls it in a loop with 0 (rva 0xfafa9f).
// Nothing of ours is deferred, so there is nothing to do.
WINAPI void dev_ProcessDeferredFrees(Iface* self, UINT flags) {}

// Xbox slots 60/61 (names from Azimuth's table): IsFencePending(u64 fence) and
// IsResourcePending(resource) -> BOOL. The game spins on them (rva 0x10d7dda
// waits on a DMA fence after LZ decompression; rva 0x114fee9 on a resource).
// Our fences (context slot 154, DMA slot 16) stand for work that was already
// done on the CPU or recorded in DXVK, which orders its own GPU work, so
// nothing is ever pending. (Reading back memory the GPU writes is a separate
// gap: PLAN Phase 5.)
WINAPI BOOL dev_IsFencePending(Iface*, uint64_t) { return 0; }
WINAPI BOOL dev_IsResourcePending(Iface*, void*) { return 0; }

WINAPI HRESULT dev_SetExceptionMode(Iface* self, UINT flags) { return E_NOTIMPL; }  // umd: E_NOTIMPL
WINAPI UINT dev_GetExceptionMode(Iface* self) { return 0; }                       // umd: 0

Vtable& device_vt() {
    static Vtable vt = [] {
        Vtable v("ID3D11DeviceX", kDeviceSlots, false);
        v.set(0, &d3d_QueryInterface)
            .set(dev_slot(dd::CheckFormatSupport), &dev_CheckFormatSupport)
            .set(dev_slot(dd::CheckMultisampleQualityLevels), &dev_CheckMultisampleQualityLevels)
            .set(dev_slot(dd::CheckFeatureSupport), &dev_CheckFeatureSupport)
            .set(dev_slot(dd::GetPrivateData), &dev_GetPrivateData)
            .set(dev_slot(dd::SetPrivateData), &dev_SetPrivateData)
            .set(dev_slot(dd::SetPrivateDataInterface), &dev_SetPrivateDataInterface)
            .set(dev_slot(dd::GetFeatureLevel), &dev_GetFeatureLevel)
            .set(dev_slot(dd::GetCreationFlags), &dev_GetCreationFlags)
            .set(dev_slot(dd::GetDeviceRemovedReason), &dev_GetDeviceRemovedReason)
            .set(dev_slot(dd::GetImmediateContext), &dev_GetImmediateContext)
            .set(dev_slot(dd::SetExceptionMode), &dev_SetExceptionMode)
            .set(dev_slot(dd::GetExceptionMode), &dev_GetExceptionMode)
            .set(dev_slot(dd::CreateDeferredContext), &dev_CreateDeferredContext)
            .set(dev_slot(dd::CreateDeferredContext1), &dev_CreateDeferredContext)
            .set(dev_slot(dd::CreateDeferredContext2), &dev_CreateDeferredContext)
            .set(dev_slot(dd::GetImmediateContext1), &dev_GetImmediateContext)
            .set(dev_slot(dd::GetImmediateContext2), &dev_GetImmediateContext)
            // Slot 55: the first Xbox-only method. umd uses the same function as
            // GetImmediateContext, so: GetImmediateContextX.
            .set(55, &dev_GetImmediateContext)
            .set(60, &dev_IsFencePending)
            .set(61, &dev_IsResourcePending)
            .set(69, &dev_ProcessDeferredFrees);
        add_resource_methods(v);
        add_pipeline_methods(v);
        add_dma_methods(v);
        add_placement_methods(v);
        return v;
    }();
    return vt;
}

// ---- d3d11_x.dll exports ----

WINAPI HRESULT D3D11XCreateDeviceX_(const CreateDeviceParams* p, void** out_device, void** out_context) {
    FL_LOG(d3d, "D3D11XCreateDeviceX(version=0x%x flags=0x%x +0x18=0x%llx +0x20=0x%llx +0x28=0x%x)", p->version,
           p->flags, static_cast<unsigned long long>(p->mask18), static_cast<unsigned long long>(p->zero20),
           p->zero28);
    const int levels[] = {kFeatureLevel11_1, kFeatureLevel11_0};
    void* dev = nullptr;
    void* ctx = nullptr;
    int level = 0;
    HRESULT hr = D3D11CreateDevice(nullptr, 1 /* D3D_DRIVER_TYPE_HARDWARE */, nullptr, 0, levels, 2,
                                   7 /* D3D11_SDK_VERSION */, &dev, &level, &ctx);
    if (hr < 0) {
        FL_ERROR("d3d: dxvk D3D11CreateDevice failed: 0x%08x", static_cast<uint32_t>(hr));
        return hr;
    }
    FL_LOG(d3d, "dxvk device created, feature level 0x%x", level);
    gpumem::init();

    // The game uses the immediate context from more than one thread: the render
    // thread draws while the main thread clears and copies textures it is
    // loading (FL_LOG=ctxthreads shows it). A desktop D3D11 immediate context
    // is single-threaded, and unsynchronized use corrupts DXVK's command stream
    // (RADV then crashes copying into a stale image), so turn on DXVK's
    // per-call lock (ID3D10Multithread slot 5 = SetMultithreadProtected).
    {
        constexpr GUID IID_ID3D10Multithread = {0x9B7E4E00, 0x342C, 0x4106, {0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0}};
        void* mt = nullptr;
        if (dx_query(ctx, IID_ID3D10Multithread, &mt) >= 0 && mt) {
            dx<int>(mt, 5, 1);
            FL_LOG(d3d, "immediate context multithread protection: %d", dx<int>(mt, 6));
            dx_release(mt);
        } else {
            FL_ERROR("d3d: dxvk immediate context has no ID3D10Multithread; multi-threaded use is unprotected");
        }
    }

    auto* d = new DeviceX();
    d->dxvk = dev;
    d->creation_flags = p->flags;
    d->immediate = make_context(d, ctx, false);
    FL_LOG(d3d, "immediate context: dxvk %p, game block %p", ctx, static_cast<void*>(d->immediate));
    if (out_context) {
        d->add_ref();
        *out_context = d->immediate;
    }
    if (out_device) {
        *out_device = &d->device_iface;
    } else {
        d->release();
    }
    return S_OK;
}

}  // namespace

DeviceX::DeviceX() : Object(u"D3D11DeviceX"), device_iface(device_vt(), this), dxgi_iface(dxgi_device_vt(), this) {
    expose(IID_ID3D11Device, &device_iface);
    expose(IID_ID3D11Device1, &device_iface);
    expose(IID_ID3D11Device2, &device_iface);
    expose(IID_ID3D11DeviceX, &device_iface);
    expose(IID_IDXGIObject, &dxgi_iface);
    expose(IID_IDXGIDevice, &dxgi_iface);
    expose(IID_IDXGIDevice1, &dxgi_iface);
    expose(IID_IDXGIDevice2, &dxgi_iface);
}

DeviceX::~DeviceX() {
    FL_LOG(d3d, "D3D11DeviceX destroyed");
    if (immediate) {
        dx_release(immediate->dxvk);
        delete immediate;
    }
    dx_release(dxvk);
}

HLE_EXPORT(d3d11_x, D3D11XCreateDeviceX, D3D11XCreateDeviceX_);

}  // namespace fl::d3d
