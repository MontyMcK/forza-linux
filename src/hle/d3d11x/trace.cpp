#include <algorithm>
#include "hle/d3d11x/trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include <sys/stat.h>

#include <cmath>
#include <vector>

#include "common/log.h"
#include "common/png.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"

namespace fl::d3d::trace {
namespace {

namespace dc = desktop::ctx;

std::set<uint64_t> frames() {
    std::set<uint64_t> out;
    if (const char* e = getenv("FL_TRACE_FRAME"))
        for (const char* p = e; *p;) {
            out.insert(strtoull(p, const_cast<char**>(&p), 10));
            while (*p == ',' || *p == ' ') p++;
        }
    return out;
}

bool g_active = false;
bool g_dump = getenv("FL_TRACE_DUMP") != nullptr;
bool g_last_compute = false;
void* g_last_uav[8] = {};
std::set<void*> g_dumped;  // textures already saved this frame
uint64_t g_frame = 0;
uint32_t g_draws = 0;

// D3D11 view slots (desktop): 7 GetResource, 8 GetDesc.
constexpr int kViewGetResource = 7;
constexpr int kViewGetDesc = 8;

std::string resource_str(void* dxvk_res) {
    if (!dxvk_res) return "null";
    char buf[256];
    DeviceChild* w = wrapper_of(dxvk_res);
    Resource* r = w ? w->backing() : nullptr;
    if (!r) {
        snprintf(buf, sizeof buf, "dxvk %p (no wrapper)", dxvk_res);
        return buf;
    }
    const uint32_t* d = r->desc32();
    int n = 0;
    switch (r->dim) {
        case 1: n = snprintf(buf, sizeof buf, "buf %p %u B bind 0x%x", (void*)r, d[0], d[2]); break;
        case 2: n = snprintf(buf, sizeof buf, "tex1d %p %u fmt %u mips %u", (void*)r, d[0], d[3], d[1]); break;
        case 3:
            n = snprintf(buf, sizeof buf, "tex2d %p %ux%u fmt %u mips %u arr %u bind 0x%x", (void*)r, d[0], d[1], d[4],
                         d[2], d[3], d[8]);
            break;
        case 4: n = snprintf(buf, sizeof buf, "tex3d %p %ux%ux%u fmt %u", (void*)r, d[0], d[1], d[2], d[4]); break;
        default: n = snprintf(buf, sizeof buf, "res %p dim %d", (void*)r, r->dim);
    }
    if (r->place && n > 0 && n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - n, " placed@%p tile %u%s", (void*)r->place, r->tile_mode,
                      r->gpu_owned ? " gpu-owned" : "");
    // The two Xbox fields after the desktop descriptor: ESRAM offset and bytes.
    int words = r->dim == 1 ? 6 : r->dim == 2 ? 9 : r->dim == 3 ? 11 : 9;
    if (!r->place && (d[words] || d[words + 1]) && n > 0 && n < (int)sizeof buf)
        snprintf(buf + n, sizeof buf - n, " esram 0x%x+0x%x", d[words], d[words + 1]);
    return buf;
}

std::string view_str(void* dxvk_view) {
    if (!dxvk_view) return "-";
    struct {
        uint32_t format, dim, u[4];
    } desc{};
    dx<void>(dxvk_view, kViewGetDesc, &desc);
    void* res = nullptr;
    dx<void>(dxvk_view, kViewGetResource, &res);
    std::string s = "view fmt " + std::to_string(desc.format) + " dim " + std::to_string(desc.dim) + " mip " +
                    std::to_string(desc.u[0]) + " of [" + resource_str(res) + "]";
    if (res) dx_release(res);
    return s;
}

std::string shader_str(void* dxvk_shader) {
    if (!dxvk_shader) return "none";
    char buf[48];
    snprintf(buf, sizeof buf, "%p", (void*)wrapper_of(dxvk_shader));
    return buf;
}

float half(uint16_t h) {
    int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.0f * std::ldexp(1.0f, -14) : e == 31 ? 65504.0f : (1 + m / 1024.0f) * std::ldexp(1.0f, e - 15);
    return (h & 0x8000) ? -v : v;
}
float f11(uint32_t v, int mbits) {  // unsigned small float (R11G11B10)
    int e = (v >> mbits) & 31, m = v & ((1 << mbits) - 1);
    if (e == 0) return m / float(1 << mbits) * std::ldexp(1.0f, -14);
    return (1 + m / float(1 << mbits)) * std::ldexp(1.0f, e - 15);
}
uint8_t u8(float f) { return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(f, 0.0f), 1.0f) * 255)); }

// Save a 2D texture (mip 0, slice 0) as PNG. Floats are clamped to 0..1.
void dump_texture(void* ctx, void* tex, const std::string& path) {
    namespace dd = desktop::dev;
    uint32_t desc[11];
    dx<void>(tex, 10 /* ID3D11Texture2D::GetDesc */, desc);
    uint32_t w = desc[0], h = desc[1], fmt = desc[4];
    if (desc[5] > 1) return;  // MSAA: skip
    desc[2] = desc[3] = 1, desc[5] = 1, desc[6] = 0, desc[7] = 3, desc[8] = 0, desc[9] = 0x20000, desc[10] = 0;
    void* dev = nullptr;
    dx<void>(ctx, 3 /* GetDevice */, &dev);
    void* staging = nullptr;
    HRESULT hr = dx<HRESULT>(dev, dd::CreateTexture2D, desc, nullptr, &staging);
    dx_release(dev);
    if (hr < 0) {
        log_write("trace", "    (dump: staging %ux%u fmt %u failed)", w, h, fmt);
        return;
    }
    dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, tex, 0u, nullptr);
    struct {
        uint8_t* data;
        UINT row, depth;
    } m{};
    if (dx<HRESULT>(ctx, dc::Map, staging, 0u, 1u, 0u, &m) >= 0) {
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        bool known = true;
        float fmin_ = 1e30f, fmax_ = -1e30f;
        for (uint32_t y = 0; y < h && known; y++) {
            const uint8_t* row = m.data + size_t(y) * m.row;
            for (uint32_t x = 0; x < w; x++) {
                uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                uint32_t px = 0;
                switch (fmt) {
                    case 61: case 62: case 63: o[0] = o[1] = o[2] = row[x]; o[3] = 255; break;  // R8
                    case 49: o[0] = row[x * 2]; o[1] = row[x * 2 + 1]; o[2] = 0; o[3] = 255; break;  // R8G8
                    case 27: case 28: case 29: memcpy(o, row + x * 4, 4); break;
                    case 87: case 88: case 90: case 91:
                        o[0] = row[x * 4 + 2], o[1] = row[x * 4 + 1], o[2] = row[x * 4], o[3] = row[x * 4 + 3];
                        break;
                    case 23: case 24: case 25:
                        memcpy(&px, row + x * 4, 4);
                        o[0] = (px & 0x3ff) >> 2, o[1] = ((px >> 10) & 0x3ff) >> 2, o[2] = ((px >> 20) & 0x3ff) >> 2;
                        o[3] = 255;
                        break;
                    case 26:
                        memcpy(&px, row + x * 4, 4);
                        o[0] = u8(f11(px & 0x7ff, 6)), o[1] = u8(f11((px >> 11) & 0x7ff, 6)), o[2] = u8(f11(px >> 22, 5));
                        o[3] = 255;
                        break;
                    case 10: {
                        const uint16_t* hp = reinterpret_cast<const uint16_t*>(row + x * 8);
                        o[0] = u8(half(hp[0])), o[1] = u8(half(hp[1])), o[2] = u8(half(hp[2])), o[3] = u8(half(hp[3]));
                        break;
                    }
                    case 39: case 40: case 41: {
                        float f; memcpy(&f, row + x * 4, 4); o[0] = o[1] = o[2] = u8(f); o[3] = 255;
                        fmin_ = std::fmin(fmin_, f), fmax_ = std::fmax(fmax_, f);
                        break;
                    }
                    case 53: case 55: case 56: {
                        uint16_t v; memcpy(&v, row + x * 2, 2); float f = v / 65535.0f; o[0] = o[1] = o[2] = u8(f); o[3] = 255;
                        fmin_ = std::fmin(fmin_, f), fmax_ = std::fmax(fmax_, f);
                        break;
                    }
                    case 54: { uint16_t v; memcpy(&v, row + x * 2, 2); o[0] = o[1] = o[2] = u8(half(v)); o[3] = 255; break; }
                    default: known = false;
                }
                if (!known) break;
            }
        }
        if (w * h <= 4 && (fmt == 41 || fmt == 26 || fmt == 10)) {  // tiny float textures: log the raw values
            const float* f = reinterpret_cast<const float*>(m.data);
            if (fmt == 41) log_write("trace", "    (1x1 R32_FLOAT value %g)", f[0]);
            else log_write("trace", "    (tiny fmt %u raw 0x%08x)", fmt, reinterpret_cast<const uint32_t*>(m.data)[0]);
        }
        dx<void>(ctx, dc::Unmap, staging, 0u);
        if (known) {
            mkdir("screenshots", 0755);
            mkdir("screenshots/trace", 0755);
            write_png(path, rgba.data(), int(w), int(h));
            if (fmax_ >= fmin_) log_write("trace", "    dumped %s (single-channel range %g..%g)", path.c_str(), fmin_, fmax_);
            else log_write("trace", "    dumped %s", path.c_str());
        } else {
            log_write("trace", "    (dump: format %u not decoded)", fmt);
        }
    }
    dx_release(staging);
}

// Log how many bytes of a 3D texture's mip 0 are non-zero, and its first texels.
void stat_texture3d(void* ctx, void* tex, const char* what) {
    namespace dd = desktop::dev;
    uint32_t desc[9];
    dx<void>(tex, 10 /* ID3D11Texture3D::GetDesc */, desc);
    uint32_t w = desc[0], h = desc[1], d = desc[2], fmt = desc[4];
    desc[3] = 1, desc[5] = 3 /* STAGING */, desc[6] = 0, desc[7] = 0x20000, desc[8] = 0;
    void* dev = nullptr;
    dx<void>(ctx, 3 /* GetDevice */, &dev);
    void* staging = nullptr;
    HRESULT hr = dx<HRESULT>(dev, dd::CreateTexture3D, desc, nullptr, &staging);
    dx_release(dev);
    if (hr < 0) return;
    dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, tex, 0u, nullptr);
    struct { uint8_t* data; UINT row, depth; } m{};
    if (dx<HRESULT>(ctx, dc::Map, staging, 0u, 1u, 0u, &m) >= 0) {
        size_t nonzero = 0, total = 0;
        for (uint32_t z = 0; z < d; z++)
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t* row = m.data + size_t(z) * m.depth + size_t(y) * m.row;
                for (uint32_t x = 0; x < w * 4; x++, total++) nonzero += row[x] != 0;
            }
        uint32_t t0, tlast;
        memcpy(&t0, m.data, 4);
        memcpy(&tlast, m.data + size_t(d - 1) * m.depth + size_t(h - 1) * m.row + (w - 1) * 4, 4);
        log_write("trace", "    (%s tex3d %ux%ux%u fmt %u: %zu/%zu bytes non-zero, first 0x%08x last 0x%08x)", what, w, h, d,
                  fmt, nonzero, total, t0, tlast);
        dx<void>(ctx, dc::Unmap, staging, 0u);
    }
    dx_release(staging);
}

// Log the first 8 floats of a small buffer bound as an SRV.
void stat_buffer(void* ctx, void* buf) {
    namespace dd = desktop::dev;
    uint32_t bd[6] = {};
    dx<void>(buf, 10 /* ID3D11Buffer::GetDesc */, bd);
    uint32_t n = std::min<uint32_t>(bd[0], 32);
    void* dev = nullptr;
    dx<void>(ctx, 3 /* GetDevice */, &dev);
    uint32_t sd[6] = {32, 3 /* STAGING */, 0, 0x20000, 0, 0};
    void* staging = nullptr;
    HRESULT hr = dx<HRESULT>(dev, dd::CreateBuffer, sd, nullptr, &staging);
    dx_release(dev);
    if (hr < 0) return;
    uint32_t box[6] = {0, 0, 0, n, 1, 1};
    dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, buf, 0u, box);
    struct { uint8_t* data; UINT row, depth; } m{};
    float v[8] = {};
    if (dx<HRESULT>(ctx, dc::Map, staging, 0u, 1u, 0u, &m) >= 0) {
        memcpy(v, m.data, n);
        dx<void>(ctx, dc::Unmap, staging, 0u);
    }
    dx_release(staging);
    log_write("trace", "    (srv buffer %u B: %g %g %g %g %g %g %g %g)", bd[0], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

std::string frame_prefix() { return "screenshots/trace/f" + std::to_string(g_frame) + "_"; }

}  // namespace

void after_draw(void* ctx) {
    if (!g_active || !g_dump) return;
    if (g_last_compute) {
        g_last_compute = false;
        for (int i = 0; i < 8; i++)
            if (g_last_uav[i]) {
                uint32_t dim = 0;
                dx<void>(g_last_uav[i], 7 /* ID3D11Resource::GetType */, &dim);
                if (dim == 3 /* TEXTURE2D */) {
                    char name[64];
                    snprintf(name, sizeof name, "d%03u_u%d.png", g_draws, i);
                    dump_texture(ctx, g_last_uav[i], frame_prefix() + name);
                }
                dx_release(g_last_uav[i]);
                g_last_uav[i] = nullptr;
            }
        return;
    }
    void* rtv = nullptr;
    void* dsv = nullptr;
    dx<void>(ctx, dc::OMGetRenderTargets, 1u, &rtv, &dsv);
    if (dsv) dx_release(dsv);
    if (!rtv) return;
    void* res = nullptr;
    dx<void>(rtv, kViewGetResource, &res);
    dx_release(rtv);
    if (!res) return;
    char name[64];
    snprintf(name, sizeof name, "d%03u_rt0.png", g_draws);
    dump_texture(ctx, res, frame_prefix() + name);
    dx_release(res);
}

void on_present(uint64_t frame) {
    static std::set<uint64_t> want = frames();
    if (g_active) log_write("trace", "==== end of frame %llu: %u draws/dispatches", (unsigned long long)g_frame, g_draws);
    g_frame = frame;
    g_active = want.count(frame) != 0;
    g_draws = 0;
    g_dumped.clear();
    if (g_active) log_write("trace", "==== frame %llu", (unsigned long long)frame);
}

bool active() { return g_active; }

void event(const char* fmt, ...) {
    if (!g_active) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_write("trace", "  %s", buf);
}

void log_targets(void* ctx, const char* indent) {
    void* rtvs[8] = {};
    void* dsv = nullptr;
    dx<void>(ctx, dc::OMGetRenderTargets, 8u, rtvs, &dsv);
    for (int i = 0; i < 8; i++)
        if (rtvs[i]) {
            log_write("trace", "%sRT%d %s", indent, i, view_str(rtvs[i]).c_str());
            dx_release(rtvs[i]);
        }
    if (dsv) {
        log_write("trace", "%sDS  %s", indent, view_str(dsv).c_str());
        dx_release(dsv);
    }
}

const char* describe_dxvk(void* obj) {
    static thread_local std::string s;
    s = resource_str(obj);
    return s.c_str();
}

// FL_TRACE_CB=1: also log each bound constant buffer of the PS (or CS) and its
// first 8 floats (read back through a small staging copy).
// FL_TRACE_CB_REGS=3,4,5,6: also print those vec4 registers of every buffer.
std::vector<uint32_t> cb_regs() {
    std::vector<uint32_t> v;
    if (const char* e = getenv("FL_TRACE_CB_REGS"))
        for (const char* p = e; *p;) {
            v.push_back(static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 10)));
            if (*p == ',') p++;
            else break;
        }
    return v;
}

void log_constant_buffers(void* ctx, bool compute, int stage_slot = -1, const char* stage_name = nullptr) {
    static const bool want = getenv("FL_TRACE_CB") != nullptr;
    if (!want) return;
    namespace dd = desktop::dev;
    void* cbs[14] = {};
    UINT first[14] = {}, num[14] = {};
    dx<void>(ctx, stage_slot >= 0 ? stage_slot : compute ? dc::CSGetConstantBuffers1 : dc::PSGetConstantBuffers1, 0u, 14u,
             cbs, first, num);
    static const std::vector<uint32_t> regs = cb_regs();
    for (int i = 0; i < 14; i++) {
        if (!cbs[i]) continue;
        float v[8] = {};
        void* dev = nullptr;
        dx<void>(ctx, 3 /* GetDevice */, &dev);
        uint32_t sd[6] = {64, 3 /* STAGING */, 0, 0x20000 /* CPU_ACCESS_READ */, 0, 0};
        void* staging = nullptr;
        if (dx<HRESULT>(dev, dd::CreateBuffer, sd, nullptr, &staging) >= 0) {
            uint32_t bd[6] = {};
            dx<void>(cbs[i], 10 /* ID3D11Buffer::GetDesc */, bd);
            uint32_t off = first[i] * 16;
            if (off + 32 <= bd[0]) {
                uint32_t box[6] = {off, 0, 0, off + 32, 1, 1};
                dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, cbs[i], 0u, box);
                struct { uint8_t* data; UINT row, depth; } m{};
                if (dx<HRESULT>(ctx, dc::Map, staging, 0u, 1u, 0u, &m) >= 0) {
                    memcpy(v, m.data, sizeof v);
                    dx<void>(ctx, dc::Unmap, staging, 0u);
                }
            }
            dx_release(staging);
        }
        dx_release(dev);
        log_write("trace", "    %scb%d %s +%u: %g %g %g %g | %g %g %g %g", stage_name ? stage_name : "", i,
                  resource_str(cbs[i]).c_str(), first[i] * 16, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        for (uint32_t reg : regs) {
            uint32_t bd[6] = {};
            dx<void>(cbs[i], 10 /* ID3D11Buffer::GetDesc */, bd);
            uint32_t off = first[i] * 16 + reg * 16;
            if (off + 16 > bd[0]) continue;
            void* dev2 = nullptr;
            dx<void>(ctx, 3 /* GetDevice */, &dev2);
            uint32_t sd2[6] = {16, 3, 0, 0x20000, 0, 0};
            void* st2 = nullptr;
            if (dx<HRESULT>(dev2, dd::CreateBuffer, sd2, nullptr, &st2) >= 0) {
                uint32_t box[6] = {off, 0, 0, off + 16, 1, 1};
                dx<void>(ctx, dc::CopySubresourceRegion, st2, 0u, 0u, 0u, 0u, cbs[i], 0u, box);
                struct { uint8_t* data; UINT row, depth; } m2{};
                float r4[4] = {};
                if (dx<HRESULT>(ctx, dc::Map, st2, 0u, 1u, 0u, &m2) >= 0) {
                    memcpy(r4, m2.data, 16);
                    dx<void>(ctx, dc::Unmap, st2, 0u);
                }
                dx_release(st2);
                log_write("trace", "      c%u: %g %g %g %g", reg, r4[0], r4[1], r4[2], r4[3]);
            }
            dx_release(dev2);
        }
        dx_release(cbs[i]);
    }
}

void draw(void* ctx, const char* kind, uint32_t a, uint32_t b, uint32_t c) {
    if (!g_active) return;
    g_draws++;
    bool compute = !strncmp(kind, "Dispatch", 8);
    log_write("trace", "#%u %s(%u, %u, %u) [ctx %p]", g_draws, kind, a, b, c, ctx);
    void* srvs[16] = {};
    if (compute) {
        void* cs = nullptr;
        dx<void>(ctx, dc::CSGetShader, &cs, nullptr, nullptr);
        log_write("trace", "    CS %s", shader_str(cs).c_str());
        if (cs) dx_release(cs);
        dx<void>(ctx, dc::CSGetShaderResources, 0u, 16u, srvs);
        log_constant_buffers(ctx, true);
        void* uavs[8] = {};
        dx<void>(ctx, dc::CSGetUnorderedAccessViews, 0u, 8u, uavs);
        g_last_compute = true;
        for (int i = 0; i < 8; i++)
            if (uavs[i]) {
                log_write("trace", "    u%d %s", i, view_str(uavs[i]).c_str());
                void* res = nullptr;
                dx<void>(uavs[i], kViewGetResource, &res);
                if (g_last_uav[i]) dx_release(g_last_uav[i]);
                g_last_uav[i] = res;  // keeps the reference until after_draw
                dx_release(uavs[i]);
            }
    } else {
        void *vs = nullptr, *ps = nullptr;
        dx<void>(ctx, dc::VSGetShader, &vs, nullptr, nullptr);
        dx<void>(ctx, dc::PSGetShader, &ps, nullptr, nullptr);
        log_write("trace", "    VS %s PS %s", shader_str(vs).c_str(), shader_str(ps).c_str());
        if (vs) dx_release(vs);
        if (ps) dx_release(ps);
        dx<void>(ctx, dc::PSGetShaderResources, 0u, 16u, srvs);
        log_constant_buffers(ctx, false);
        log_constant_buffers(ctx, false, dc::VSGetConstantBuffers1, "vs-");
        {
            void* il = nullptr;
            dx<void>(ctx, dc::IAGetInputLayout, &il);
            void* vb[2] = {};
            UINT stride[2] = {}, off[2] = {};
            dx<void>(ctx, dc::IAGetVertexBuffers, 0u, 2u, vb, stride, off);
            UINT topo = 0;
            dx<void>(ctx, dc::IAGetPrimitiveTopology, &topo);
            log_write("trace", "    IA topo %u layout %p vb0 [%s] stride %u off %u", topo, il ? (void*)wrapper_of(il) : nullptr,
                      vb[0] ? resource_str(vb[0]).c_str() : "none", stride[0], off[0]);
            void* rs = nullptr;
            dx<void>(ctx, dc::RSGetState, &rs);
            int32_t rd[10] = {};
            if (rs) dx<void>(rs, 7 /* ID3D11RasterizerState::GetDesc */, rd);
            int32_t sc[4] = {};
            UINT nsc = 1;
            dx<void>(ctx, dc::RSGetScissorRects, &nsc, sc);
            log_write("trace", "    RS %s fill %d cull %d frontCCW %d depthclip %d scissor %d rect %d,%d-%d,%d", rs ? "set" : "default",
                      rd[0], rd[1], rd[2], rd[6], rd[7], sc[0], sc[1], sc[2], sc[3]);
            if (rs) dx_release(rs);
            if (vb[0] && g_dump) stat_buffer(ctx, vb[0]);
            if (il) dx_release(il);
            for (void* b : vb)
                if (b) dx_release(b);
        }
        void* rtvs[8] = {};
        void* dsv = nullptr;
        dx<void>(ctx, dc::OMGetRenderTargets, 8u, rtvs, &dsv);
        for (int i = 0; i < 8; i++)
            if (rtvs[i]) {
                log_write("trace", "    RT%d %s", i, view_str(rtvs[i]).c_str());
                dx_release(rtvs[i]);
            }
        if (dsv) {
            log_write("trace", "    DS  %s", view_str(dsv).c_str());
            dx_release(dsv);
        }
        void* blend = nullptr;
        float factor[4];
        uint32_t mask = 0;
        dx<void>(ctx, dc::OMGetBlendState, &blend, factor, &mask);
        struct {
            float x, y, w, h, mn, mx;
        } vp{};
        uint32_t nvp = 1;
        dx<void>(ctx, dc::RSGetViewports, &nvp, &vp);
        log_write("trace", "    blend %p mask 0x%x viewport %.0f,%.0f %.0fx%.0f depth %.2f-%.2f",
                  blend ? (void*)wrapper_of(blend) : nullptr, mask, vp.x, vp.y, vp.w, vp.h, vp.mn, vp.mx);
        if (blend) {
            uint32_t bd[2 + 8 * 8] = {};
            dx<void>(blend, 7 /* ID3D11BlendState::GetDesc */, bd);
            log_write("trace", "    blend rt0: enable %u src %u dst %u op %u write 0x%x", bd[2], bd[3], bd[4], bd[5], bd[9]);
        }
        void* dss = nullptr;
        UINT sref = 0;
        dx<void>(ctx, dc::OMGetDepthStencilState, &dss, &sref);
        if (dss) {
            uint32_t dd2[13] = {};
            dx<void>(dss, 7 /* ID3D11DepthStencilState::GetDesc */, dd2);
            log_write("trace", "    depth enable %u write %u func %u stencil %u ref %u", dd2[0], dd2[1], dd2[2], dd2[3], sref);
            dx_release(dss);
        }
        if (blend) dx_release(blend);
    }
    for (int i = 0; i < 16; i++)
        if (srvs[i]) {
            log_write("trace", "    t%d %s", i, view_str(srvs[i]).c_str());
            if (g_dump) {
                void* res = nullptr;
                dx<void>(srvs[i], kViewGetResource, &res);
                struct { uint32_t format, dim, u[4]; } vd{};
                dx<void>(srvs[i], kViewGetDesc, &vd);
                if (res && vd.dim == 8 /* TEXTURE3D */ && g_dumped.insert(res).second) stat_texture3d(ctx, res, "srv");
                if (res && (vd.dim == 1 || vd.dim == 11) /* BUFFER(EX) */ && g_dumped.insert(res).second) stat_buffer(ctx, res);
                if (res && vd.dim == 4 /* TEXTURE2D */ && g_dumped.insert(res).second) {
                    char name[64];
                    snprintf(name, sizeof name, "d%03u_t%d_%p.png", g_draws, i, res);
                    dump_texture(ctx, res, frame_prefix() + name);
                }
                if (res) dx_release(res);
            }
            dx_release(srvs[i]);
        }
}

}  // namespace fl::d3d::trace
