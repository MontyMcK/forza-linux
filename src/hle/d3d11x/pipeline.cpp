// Shaders, input layouts, state objects and queries.
// Their descriptors use the desktop layouts (umd's Create* functions read the
// same offsets), so they go to DXVK unchanged.
#include <cstring>

#include "common/log.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Vtable;
namespace dd = desktop::dev;

DeviceX* dev_of(Iface* self) { return self->as<DeviceX>(); }

HRESULT wrap(DeviceX* d, HRESULT hr, void* obj, void** out, Vtable& vt, const char16_t* name, const GUID& iid) {
    if (hr < 0 || !out) {
        if (out) *out = nullptr;
        return hr;
    }
    auto* c = new DeviceChild(name, d, vt, obj);
    c->expose(iid, &c->iface);
    *out = &c->iface;
    return hr;
}

// ---- Shaders, input layouts, class linkage: no methods beyond DeviceChild ----

Vtable make_plain_vt(const char* name) {
    Vtable v(name, 16, false);  // 8 known + spare traps
    child_base(v);
    return v;
}
Vtable& vs_vt() { static Vtable v = make_plain_vt("ID3D11VertexShaderX"); return v; }
Vtable& hs_vt() { static Vtable v = make_plain_vt("ID3D11HullShaderX"); return v; }
Vtable& ds_vt() { static Vtable v = make_plain_vt("ID3D11DomainShaderX"); return v; }
Vtable& gs_vt() { static Vtable v = make_plain_vt("ID3D11GeometryShaderX"); return v; }
Vtable& ps_vt() { static Vtable v = make_plain_vt("ID3D11PixelShaderX"); return v; }
Vtable& cs_vt() { static Vtable v = make_plain_vt("ID3D11ComputeShaderX"); return v; }
Vtable& layout_vt() { static Vtable v = make_plain_vt("ID3D11InputLayoutX"); return v; }
Vtable& linkage_vt() { static Vtable v = make_plain_vt("ID3D11ClassLinkageX"); return v; }

void shader_failed(const char* what, const void* code, SIZE_T len, HRESULT hr) {
    const char* c = static_cast<const char*>(code);
    FL_ERROR("d3d: %s(%zu bytes, magic '%.4s') failed: 0x%08x", what, static_cast<size_t>(len), c ? c : "",
             static_cast<uint32_t>(hr));
}

#define FL_SHADER_CREATE(Stage, desktop_slot, vtfn, iid)                                                        \
    WINAPI HRESULT dev_Create##Stage(Iface* self, const void* code, SIZE_T len, const void* linkage, void** out) { \
        void* obj = nullptr;                                                                                    \
        HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, desktop_slot, code, len, unwrap(linkage),                  \
                                 out ? &obj : nullptr);                                                         \
        if (hr < 0) shader_failed("Create" #Stage, code, len, hr);                                              \
        return wrap(dev_of(self), hr, obj, out, vtfn(), u"D3D11" #Stage, iid);                                  \
    }
FL_SHADER_CREATE(VertexShader, dd::CreateVertexShader, vs_vt, IID_ID3D11VertexShader)
FL_SHADER_CREATE(GeometryShader, dd::CreateGeometryShader, gs_vt, IID_ID3D11GeometryShader)
FL_SHADER_CREATE(PixelShader, dd::CreatePixelShader, ps_vt, IID_ID3D11PixelShader)
FL_SHADER_CREATE(HullShader, dd::CreateHullShader, hs_vt, IID_ID3D11HullShader)
FL_SHADER_CREATE(DomainShader, dd::CreateDomainShader, ds_vt, IID_ID3D11DomainShader)
FL_SHADER_CREATE(ComputeShader, dd::CreateComputeShader, cs_vt, IID_ID3D11ComputeShader)
#undef FL_SHADER_CREATE

WINAPI HRESULT dev_CreateGeometryShaderWithStreamOutput(Iface* self, const void* code, SIZE_T len, const void* decl,
                                                        UINT num_entries, const UINT* strides, UINT num_strides,
                                                        UINT rasterized_stream, const void* linkage, void** out) {
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CreateGeometryShaderWithStreamOutput, code, len, decl,
                             num_entries, strides, num_strides, rasterized_stream, unwrap(linkage),
                             out ? &obj : nullptr);
    if (hr < 0) shader_failed("CreateGeometryShaderWithStreamOutput", code, len, hr);
    return wrap(dev_of(self), hr, obj, out, gs_vt(), u"D3D11GeometryShader", IID_ID3D11GeometryShader);
}

WINAPI HRESULT dev_CreateInputLayout(Iface* self, const void* elems, UINT num, const void* code, SIZE_T len,
                                     void** out) {
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CreateInputLayout, elems, num, code, len, out ? &obj : nullptr);
    if (hr < 0) FL_ERROR("d3d: CreateInputLayout(%u elements) failed: 0x%08x", num, static_cast<uint32_t>(hr));
    return wrap(dev_of(self), hr, obj, out, layout_vt(), u"D3D11InputLayout", IID_ID3D11InputLayout);
}

WINAPI HRESULT dev_CreateClassLinkage(Iface* self, void** out) {
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CreateClassLinkage, out ? &obj : nullptr);
    return wrap(dev_of(self), hr, obj, out, linkage_vt(), u"D3D11ClassLinkage", IID_ID3D11ClassLinkage);
}

// ---- State objects: 8 GetDesc (9 GetDesc1 for the "1" variants) ----

WINAPI void state_GetDesc(Iface* self, void* desc) {
    dx<void>(self->as<DeviceChild>()->dxvk, 7, desc);  // desktop: GetDesc right after DeviceChild
}
WINAPI void state_GetDesc1(Iface* self, void* desc) { dx<void>(self->as<DeviceChild>()->dxvk, 8, desc); }

Vtable make_state_vt(const char* name) {
    Vtable v(name, 16, false);
    child_base(v).set(8, &state_GetDesc).set(9, &state_GetDesc1);
    return v;
}
Vtable& blend_vt() { static Vtable v = make_state_vt("ID3D11BlendStateX"); return v; }
Vtable& depth_vt() { static Vtable v = make_state_vt("ID3D11DepthStencilStateX"); return v; }
Vtable& raster_vt() { static Vtable v = make_state_vt("ID3D11RasterizerStateX"); return v; }
Vtable& sampler_vt() { static Vtable v = make_state_vt("ID3D11SamplerStateX"); return v; }

// GetDesc1 only exists on the "1" variants; DXVK objects implement both anyway.
#define FL_STATE_CREATE(Name, desktop_slot, vtfn, iid)                                                \
    WINAPI HRESULT dev_Create##Name(Iface* self, const void* desc, void** out) {                      \
        void* obj = nullptr;                                                                          \
        HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, desktop_slot, desc, out ? &obj : nullptr);       \
        if (hr < 0) FL_ERROR("d3d: Create" #Name " failed: 0x%08x", static_cast<uint32_t>(hr));       \
        return wrap(dev_of(self), hr, obj, out, vtfn(), u"D3D11" #Name, iid);                         \
    }
FL_STATE_CREATE(BlendState, dd::CreateBlendState, blend_vt, IID_ID3D11BlendState)
FL_STATE_CREATE(DepthStencilState, dd::CreateDepthStencilState, depth_vt, IID_ID3D11DepthStencilState)
FL_STATE_CREATE(RasterizerState, dd::CreateRasterizerState, raster_vt, IID_ID3D11RasterizerState)
FL_STATE_CREATE(SamplerState, dd::CreateSamplerState, sampler_vt, IID_ID3D11SamplerState)
FL_STATE_CREATE(BlendState1, dd::CreateBlendState1, blend_vt, IID_ID3D11BlendState1)
FL_STATE_CREATE(RasterizerState1, dd::CreateRasterizerState1, raster_vt, IID_ID3D11RasterizerState1)
#undef FL_STATE_CREATE

// ---- Queries / predicates / counters: 8 GetDataSize, 9 GetDesc ----

WINAPI UINT async_GetDataSize(Iface* self) { return dx<UINT>(self->as<DeviceChild>()->dxvk, 7); }
WINAPI void async_GetDesc(Iface* self, void* desc) { dx<void>(self->as<DeviceChild>()->dxvk, 8, desc); }

Vtable& query_vt() {
    static Vtable v = [] {
        Vtable t("ID3D11QueryX", 16, false);
        child_base(t).set(8, &async_GetDataSize).set(9, &async_GetDesc);
        return t;
    }();
    return v;
}

WINAPI HRESULT dev_CreateQuery(Iface* self, const UINT* desc, void** out) {
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CreateQuery, desc, out ? &obj : nullptr);
    if (hr < 0) FL_ERROR("d3d: CreateQuery(type=%u misc=0x%x) failed: 0x%08x", desc[0], desc[1], static_cast<uint32_t>(hr));
    return wrap(dev_of(self), hr, obj, out, query_vt(), u"D3D11Query", IID_ID3D11Query);
}
WINAPI HRESULT dev_CreatePredicate(Iface* self, const UINT* desc, void** out) {
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CreatePredicate, desc, out ? &obj : nullptr);
    if (hr < 0) FL_ERROR("d3d: CreatePredicate(type=%u) failed: 0x%08x", desc[0], static_cast<uint32_t>(hr));
    HRESULT r = wrap(dev_of(self), hr, obj, out, query_vt(), u"D3D11Predicate", IID_ID3D11Predicate);
    if (r >= 0 && out) child_of(*out)->expose(IID_ID3D11Query, &child_of(*out)->iface);
    return r;
}

}  // namespace

void add_pipeline_methods(Vtable& v) {
    v.set(dev_slot(dd::CreateInputLayout), &dev_CreateInputLayout)
        .set(dev_slot(dd::CreateVertexShader), &dev_CreateVertexShader)
        .set(dev_slot(dd::CreateGeometryShader), &dev_CreateGeometryShader)
        .set(dev_slot(dd::CreateGeometryShaderWithStreamOutput), &dev_CreateGeometryShaderWithStreamOutput)
        .set(dev_slot(dd::CreatePixelShader), &dev_CreatePixelShader)
        .set(dev_slot(dd::CreateHullShader), &dev_CreateHullShader)
        .set(dev_slot(dd::CreateDomainShader), &dev_CreateDomainShader)
        .set(dev_slot(dd::CreateComputeShader), &dev_CreateComputeShader)
        .set(dev_slot(dd::CreateClassLinkage), &dev_CreateClassLinkage)
        .set(dev_slot(dd::CreateBlendState), &dev_CreateBlendState)
        .set(dev_slot(dd::CreateDepthStencilState), &dev_CreateDepthStencilState)
        .set(dev_slot(dd::CreateRasterizerState), &dev_CreateRasterizerState)
        .set(dev_slot(dd::CreateSamplerState), &dev_CreateSamplerState)
        .set(dev_slot(dd::CreateQuery), &dev_CreateQuery)
        .set(dev_slot(dd::CreatePredicate), &dev_CreatePredicate)
        .set(dev_slot(dd::CreateBlendState1), &dev_CreateBlendState1)
        .set(dev_slot(dd::CreateRasterizerState1), &dev_CreateRasterizerState1);
}

}  // namespace fl::d3d
