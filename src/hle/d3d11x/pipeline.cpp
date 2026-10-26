// Shaders, input layouts, state objects and queries.
// Their descriptors use the desktop layouts (umd's Create* functions read the
// same offsets), so they go to DXVK unchanged.
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

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

// FL_LOG=shaders: which wrapper (the pointer the frame trace prints) has which
// bytecode (FNV-1a of the DXBC; match it against DXVK_SHADER_DUMP_PATH dumps).
void log_shader(const char* stage, void* dxvk_obj, const void* code, SIZE_T len) {
    static const bool want = log_enabled("shaders") || getenv("FL_DEBUG_NULL_SRV");
    if (!want) return;
    uint64_t h = 1469598103934665603ull;
    for (SIZE_T i = 0; i < len; i++) h = (h ^ static_cast<const uint8_t*>(code)[i]) * 1099511628211ull;
    note_shader_hash(dxvk_obj, h);
    FL_LOG(shaders, "%s %p: %zu bytes, fnv %016llx", stage, static_cast<void*>(wrapper_of(dxvk_obj)),
           static_cast<size_t>(len), static_cast<unsigned long long>(h));
}

#define FL_SHADER_CREATE(Stage, desktop_slot, vtfn, iid)                                                        \
    WINAPI HRESULT dev_Create##Stage(Iface* self, const void* code, SIZE_T len, const void* linkage, void** out) { \
        void* obj = nullptr;                                                                                    \
        HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, desktop_slot, code, len, unwrap(linkage),                  \
                                 out ? &obj : nullptr);                                                         \
        if (hr < 0) shader_failed("Create" #Stage, code, len, hr);                                              \
        HRESULT r = wrap(dev_of(self), hr, obj, out, vtfn(), u"D3D11" #Stage, iid);                             \
        if (hr >= 0) log_shader(#Stage, obj, code, len);                                                         \
        return r;                                                                                               \
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
    HRESULT r = wrap(dev_of(self), hr, obj, out, layout_vt(), u"D3D11InputLayout", IID_ID3D11InputLayout);
    if (hr >= 0 && log_enabled("shaders")) {
        struct Elem { const char* name; UINT index, format, slot, offset, cls, step; };
        const Elem* e = static_cast<const Elem*>(elems);
        for (UINT i = 0; i < num; i++)
            FL_LOG(shaders, "InputLayout %p [%u] %s%u fmt %u slot %u off %u class %u step %u",
                   static_cast<void*>(wrapper_of(obj)), i, e[i].name, e[i].index, e[i].format, e[i].slot, e[i].offset,
                   e[i].cls, e[i].step);
    }
    return r;
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

Vtable make_state_vt(const char* name, bool has_desc1) {
    Vtable v(name, 16, false);
    child_base(v).set(8, &state_GetDesc);
    if (has_desc1) v.set(9, &state_GetDesc1);  // only the "1" variants have it in DXVK
    return v;
}

// ---- Samplers: the Xbox exposes the GCN hardware sampler (S#, 4 dwords) ----
// umd keeps it at sampler+0x20 next to the D3D11 desc (+0x30): slot 9 copies
// it out, device slot 67 CreateSamplerStateX(const u32[4], out) builds a
// sampler from one. The game reads a sampler's S#, sets BORDER_COLOR_TYPE
// (word 3 bits 30-31) and makes a variant (game rva 0x136f995). We encode the
// D3D11 desc into the public GCN SQ_IMG_SAMP_WORD0..3 layout and back.
struct SamplerDesc {  // D3D11_SAMPLER_DESC
    uint32_t filter, addr[3];
    float mip_bias;
    uint32_t max_aniso, cmp;
    float border[4];
    float min_lod, max_lod;
};
static_assert(sizeof(SamplerDesc) == 0x34);

uint32_t fixed_u48(float v) { return uint32_t(std::clamp(v, 0.0f, 15.996f) * 256.0f) & 0xfff; }
uint32_t fixed_s58(float v) { return uint32_t(int32_t(std::clamp(v, -16.0f, 15.996f) * 256.0f)) & 0x3fff; }
float unfixed_u48(uint32_t v) { return float(v & 0xfff) / 256.0f; }
float unfixed_s58(uint32_t v) { return float(int32_t((v & 0x3fff) << 18) >> 18) / 256.0f; }

std::mutex g_border_m;
std::vector<std::array<float, 4>> g_border_colors;  // BORDER_COLOR_TYPE 3: index in BORDER_COLOR_PTR

void encode_sampler(const SamplerDesc& d, uint32_t w[4]) {
    static const uint8_t kClamp[6] = {0, 0, 1, 2, 6, 3};  // D3D11 WRAP/MIRROR/CLAMP/BORDER/MIRROR_ONCE -> SQ_TEX_*
    uint32_t c[3];
    for (int i = 0; i < 3; i++) c[i] = d.addr[i] <= 5 ? kClamp[d.addr[i]] : 0;
    bool aniso = (d.filter & 0x40) != 0;
    uint32_t reduction = (d.filter >> 7) & 3;  // 0 standard, 1 comparison, 2 min, 3 max
    uint32_t mag = aniso ? 3 : (d.filter >> 2) & 1, min = aniso ? 3 : (d.filter >> 4) & 1;
    uint32_t mip = aniso ? 2 : (d.filter & 1) + 1;
    uint32_t ratio = 0;
    while (ratio < 4 && (2u << ratio) <= d.max_aniso) ratio++;
    uint32_t cmp = reduction == 1 && d.cmp >= 1 && d.cmp <= 8 ? d.cmp - 1 : 0;
    w[0] = c[0] | c[1] << 3 | c[2] << 6 | (aniso ? ratio : 0) << 9 | cmp << 12 | (reduction >= 2 ? reduction - 1 : 0) << 29;
    w[1] = fixed_u48(d.min_lod) | fixed_u48(d.max_lod) << 12;
    w[2] = fixed_s58(d.mip_bias) | mag << 20 | min << 22 | (min & 1) << 24 | mip << 26;
    const float* b = d.border;
    uint32_t type = 3, ptr = 0;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0) type = b[3] == 0 ? 0 : b[3] == 1 ? 1 : 3;
    else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) type = 2;
    if (type == 3) {
        std::lock_guard lock(g_border_m);
        std::array<float, 4> col{b[0], b[1], b[2], b[3]};
        auto it = std::find(g_border_colors.begin(), g_border_colors.end(), col);
        ptr = uint32_t(it - g_border_colors.begin());
        if (it == g_border_colors.end()) g_border_colors.push_back(col);
    }
    w[3] = (ptr & 0xfff) | type << 30;
    // Comparison samplers keep their D3D11 meaning: we mark them in a word-3
    // bit the hardware leaves unused (bit 29) so decode can restore the filter.
    if (reduction == 1) w[3] |= 1u << 29;
}

SamplerDesc decode_sampler(const uint32_t w[4]) {
    static const uint8_t kAddr[8] = {1, 2, 3, 5, 4, 5, 4, 5};  // SQ_TEX_* -> D3D11
    SamplerDesc d{};
    for (int i = 0; i < 3; i++) d.addr[i] = kAddr[(w[0] >> (3 * i)) & 7];
    uint32_t mag = (w[2] >> 20) & 3, min = (w[2] >> 22) & 3, mip = (w[2] >> 26) & 3;
    uint32_t reduction = (w[3] >> 29) & 1 ? 1 : ((w[0] >> 29) & 3) ? ((w[0] >> 29) & 3) + 1 : 0;
    if (mag >= 2 || min >= 2) {
        d.filter = 0x55;
        d.max_aniso = 1u << ((w[0] >> 9) & 7);
    } else {
        d.filter = (mip >= 2 ? 1 : 0) | (mag & 1) << 2 | (min & 1) << 4;
        d.max_aniso = 1;
    }
    d.filter |= reduction << 7;
    d.cmp = reduction == 1 ? ((w[0] >> 12) & 7) + 1 : 1;
    d.min_lod = unfixed_u48(w[1]);
    d.max_lod = unfixed_u48(w[1] >> 12);
    if (d.max_lod >= 15.99f) d.max_lod = 3.402823466e+38f;  // FLT_MAX: "no clamp"
    d.mip_bias = unfixed_s58(w[2]);
    switch (w[3] >> 30) {
        case 0: break;
        case 1: d.border[3] = 1; break;
        case 2: d.border[0] = d.border[1] = d.border[2] = d.border[3] = 1; break;
        default: {
            std::lock_guard lock(g_border_m);
            uint32_t ptr = w[3] & 0xfff;
            if (ptr < g_border_colors.size()) std::copy(g_border_colors[ptr].begin(), g_border_colors[ptr].end(), d.border);
        }
    }
    return d;
}

WINAPI void sampler_GetHardwareDesc(Iface* self, uint32_t* out) {
    SamplerDesc d;
    dx<void>(self->as<DeviceChild>()->dxvk, 7, &d);
    encode_sampler(d, out);
}

Vtable& blend_vt() { static Vtable v = make_state_vt("ID3D11BlendStateX", true); return v; }
Vtable& depth_vt() { static Vtable v = make_state_vt("ID3D11DepthStencilStateX", false); return v; }
Vtable& raster_vt() { static Vtable v = make_state_vt("ID3D11RasterizerStateX", true); return v; }
Vtable& sampler_vt() {
    static Vtable v = [] {
        Vtable t = make_state_vt("ID3D11SamplerStateX", false);
        t.set(9, &sampler_GetHardwareDesc);
        return t;
    }();
    return v;
}

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

// Device slot 67: CreateSamplerStateX(const u32 hardware sampler[4], out).
WINAPI HRESULT dev_CreateSamplerStateX(Iface* self, const uint32_t* hw, void** out) {
    SamplerDesc d = decode_sampler(hw);
    return dev_CreateSamplerState(self, &d, out);
}

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
    FL_LOG(query, "CreateQuery(type %u misc 0x%x) -> dxvk %p", desc[0], desc[1], obj);
    return wrap(dev_of(self), hr, obj, out, query_vt(), u"D3D11Query", IID_ID3D11Query);
}
WINAPI HRESULT dev_CreatePredicate(Iface* self, const UINT* desc, void** out) {
    void* obj = nullptr;
    HRESULT hr = dx<HRESULT>(dev_of(self)->dxvk, dd::CreatePredicate, desc, out ? &obj : nullptr);
    if (hr < 0) FL_ERROR("d3d: CreatePredicate(type=%u) failed: 0x%08x", desc[0], static_cast<uint32_t>(hr));
    FL_LOG(query, "CreatePredicate(type %u misc 0x%x) -> dxvk %p", desc[0], desc[1], obj);
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
        .set(dev_slot(dd::CreateRasterizerState1), &dev_CreateRasterizerState1)
        .set(67, &dev_CreateSamplerStateX);
}

}  // namespace fl::d3d
