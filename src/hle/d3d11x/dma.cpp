// Device slot 59: the Xbox-only object the game creates twice right after the
// device (FINDINGS §3.1). Probably a DMA/copy-engine context: its methods take
// GPU virtual addresses, submit work and wait on fences. umd: 30-slot vtable at
// 0x16f580. Only slots verified at a game call site are filled.
#include <cstring>

#include "common/log.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Vtable;

struct DmaDesc {
    UINT flags;      // low 4 bits: engine (the game passes 1 and 2)
    UINT ring_size;  // 0 means 0x80000 in umd
    UINT unknown8;
};

struct DmaContext : DeviceChild {
    DmaContext(DeviceX* d, Vtable& vt, const DmaDesc& desc) : DeviceChild(u"D3D11DmaContextX", d, vt, nullptr), desc(desc) {}
    DmaDesc desc;
};

// umd returns the constant 2 here; meaning unknown (maybe a context type).
WINAPI UINT dma_Slot8(Iface* self) { return 2; }

Vtable& dma_vt() {
    static Vtable v = [] {
        Vtable t("ID3D11DmaEngineContextX", 30, false);
        child_base(t).set(8, &dma_Slot8);
        return t;
    }();
    return v;
}

// Xbox device slot 59: (const DmaDesc*, out**). Game calls at rva 0x10d8afb / 0x10d8b3d.
WINAPI HRESULT dev_CreateDmaContext(Iface* self, const DmaDesc* desc, void** out) {
    FL_LOG(d3d, "CreateDmaContext(flags=0x%x ring=0x%x +8=0x%x)", desc->flags, desc->ring_size, desc->unknown8);
    auto* c = new DmaContext(self->as<DeviceX>(), dma_vt(), *desc);
    *out = &c->iface;
    return S_OK;
}

}  // namespace

void add_dma_methods(Vtable& v) { v.set(59, &dev_CreateDmaContext); }

}  // namespace fl::d3d
