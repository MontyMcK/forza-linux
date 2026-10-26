#include <algorithm>
#include "hle/d3d11x/trace.h"
#include "hle/d3d11x/gpumem.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
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
thread_local void* g_match_pending = nullptr;  // FL_TRACE_RT_MATCH draw awaiting after_draw
bool g_dump = getenv("FL_TRACE_DUMP") != nullptr;
bool g_last_compute = false;
void* g_last_uav[8] = {};
std::set<void*> g_dumped;  // textures already saved this frame
uint64_t g_frame = 0;
uint32_t g_draws = 0;

constexpr int kTraceSrvs = 128;  // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT

// D3D11 view slots (desktop): 7 GetResource, 8 GetDesc.
constexpr int kViewGetResource = 7;
constexpr int kViewGetDesc = 8;

// FL_TRACE_MIPS=1: the state of each mip of a placement texture's game
// memory: U uncommitted, Z all zero, D data (once per resource per frame).
std::map<const Resource*, std::string> g_mip_states;
std::mutex g_mip_mutex;  // bundles are recorded on several threads
std::string mip_state(const Resource* r) {
    static const bool want = getenv("FL_TRACE_MIPS") != nullptr;
    if (!want) return "?";
    std::lock_guard lock(g_mip_mutex);
    auto it = g_mip_states.find(r);
    if (it != g_mip_states.end()) return it->second;
    std::string out;
    for (const gcn::MipLayout& m : r->tex_layout.mips) {
        const uint8_t* p = r->place + m.offset;
        if (!gpumem::readable(p, m.size)) {
            out += 'U';
            continue;
        }
        bool zero = true;
        for (size_t i = 0; i < m.size && zero; i += 8) zero = *reinterpret_cast<const uint64_t*>(p + i) == 0;
        out += zero ? 'Z' : 'D';
    }
    g_mip_states[r] = out;
    return out;
}

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
        n += snprintf(buf + n, sizeof buf - n, " esram 0x%x+0x%x", d[words], d[words + 1]);
    static const bool mips_always = getenv("FL_TRACE_MIPS") != nullptr;
    if (r->place && r->dim > 1 && !r->gpu_owned && n > 0 && n < (int)sizeof buf && (g_active || mips_always))
        n += snprintf(buf + n, sizeof buf - n, " mips %s", mip_state(r).c_str());
    if (std::string name = r->priv.name(); !name.empty() && n > 0 && n < (int)sizeof buf)
        snprintf(buf + n, sizeof buf - n, " '%s'", name.c_str());
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
void dump_texture_ss(void* ctx, void* tex, const std::string& path);

// Save a 2D texture as PNG; MSAA colour targets are resolved first.
void dump_texture(void* ctx, void* tex, const std::string& path) {
    namespace dd = desktop::dev;
    uint32_t desc[11];
    dx<void>(tex, 10 /* ID3D11Texture2D::GetDesc */, desc);
    if (desc[5] <= 1) return dump_texture_ss(ctx, tex, path);
    uint32_t fmt = desc[4];
    if (fmt == 19 || fmt == 20 || fmt == 39 || fmt == 40 || fmt == 44 || fmt == 45 || fmt == 53 || fmt == 55) return;
    uint32_t typed = fmt == 23 ? 24 : fmt == 27 ? 28 : fmt == 9 ? 10 : fmt;  // resolve needs a typed format
    uint32_t rd[11];
    std::memcpy(rd, desc, sizeof rd);
    rd[2] = rd[3] = 1, rd[4] = typed, rd[5] = 1, rd[6] = 0, rd[7] = 0 /* DEFAULT */, rd[8] = 0x8 /* SRV */, rd[9] = 0,
    rd[10] = 0;
    void* dev = nullptr;
    dx<void>(ctx, 3 /* GetDevice */, &dev);
    void* resolved = nullptr;
    HRESULT hr = dx<HRESULT>(dev, dd::CreateTexture2D, rd, nullptr, &resolved);
    dx_release(dev);
    if (hr < 0) return;
    dx<void>(ctx, dc::ResolveSubresource, resolved, 0u, tex, 0u, typed);
    dump_texture_ss(ctx, resolved, path);
    dx_release(resolved);
}

uint32_t g_dump_mip = 0, g_dump_slice = 0;  // dump_texture_ss: which subresource (reset after each dump)

void dump_texture_ss(void* ctx, void* tex, const std::string& path) {
    namespace dd = desktop::dev;
    uint32_t desc[11];
    dx<void>(tex, 10 /* ID3D11Texture2D::GetDesc */, desc);
    uint32_t mips = desc[2] ? desc[2] : 1;
    uint32_t mip = std::min(g_dump_mip, mips - 1), slice = std::min(g_dump_slice, desc[3] ? desc[3] - 1 : 0);
    g_dump_mip = g_dump_slice = 0;
    uint32_t sub = mip + slice * mips;
    uint32_t w = std::max(1u, desc[0] >> mip), h = std::max(1u, desc[1] >> mip), fmt = desc[4];
    if (desc[5] > 1) return;  // MSAA: skip
    desc[0] = w, desc[1] = h;
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
    dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, tex, sub, nullptr);
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
                    case 1: case 2: {  // R32G32B32A32_FLOAT (range of the R channel logged)
                        float f[4]; memcpy(f, row + x * 16, 16);
                        o[0] = u8(f[0]), o[1] = u8(f[1]), o[2] = u8(f[2]), o[3] = 255;
                        fmin_ = std::fmin(fmin_, f[0]), fmax_ = std::fmax(fmax_, f[0]);
                        break;
                    }
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

// FL_TRACE_DUMP=pass: dump each render target once, when the draws move on
// to a different one (named after the last draw that wrote it).
void* g_pass_rt = nullptr;
uint32_t g_pass_last = 0;
void flush_pass(void* ctx) {
    if (!g_pass_rt) return;
    char name[64];
    snprintf(name, sizeof name, "d%03u_pass_rt0.png", g_pass_last);
    dump_texture(ctx, g_pass_rt, frame_prefix() + name);
    dx_release(g_pass_rt);
    g_pass_rt = nullptr;
}

// FL_TRACE_PIXEL=x,y: after every draw/dispatch/command list of a traced
// frame, read RT0's pixel (x, y) back (MSAA targets resolved first) and log
// it whenever it changed: which draws touch a broken part of the screen.
void probe_pixel(void* ctx) {
    static int px = -1, py = -1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("FL_TRACE_PIXEL")) sscanf(e, "%d,%d", &px, &py);
    }
    if (px < 0) return;
    namespace dd = desktop::dev;
    void* rtv = nullptr;
    void* dsv = nullptr;
    dx<void>(ctx, dc::OMGetRenderTargets, 1u, &rtv, &dsv);
    if (dsv) dx_release(dsv);
    if (!rtv) return;
    void* tex = nullptr;
    dx<void>(rtv, kViewGetResource, &tex);
    dx_release(rtv);
    if (!tex) return;
    uint32_t desc[11];
    dx<void>(tex, 10 /* ID3D11Texture2D::GetDesc */, desc);
    static void* last_tex = nullptr;
    static uint32_t last_val = 0;
    void* dev = nullptr;
    dx<void>(ctx, 3 /* GetDevice */, &dev);
    void* src = tex;
    void* resolved = nullptr;
    uint32_t fmt = desc[4], typed = fmt == 23 ? 24 : fmt == 27 ? 28 : fmt == 9 ? 10 : fmt;
    if (desc[5] > 1 && desc[0] > uint32_t(px) && desc[1] > uint32_t(py)) {
        uint32_t rd[11];
        std::memcpy(rd, desc, sizeof rd);
        rd[2] = rd[3] = 1, rd[4] = typed, rd[5] = 1, rd[6] = 0, rd[7] = 0, rd[8] = 0x8, rd[9] = 0, rd[10] = 0;
        if (dx<HRESULT>(dev, dd::CreateTexture2D, rd, nullptr, &resolved) >= 0) {
            dx<void>(ctx, dc::ResolveSubresource, resolved, 0u, tex, 0u, typed);
            src = resolved;
        }
    }
    uint32_t val = 0;
    bool ok = false;
    if (desc[0] > uint32_t(px) && desc[1] > uint32_t(py) && (desc[5] <= 1 || resolved)) {
        uint32_t sd[11] = {1, 1, 1, 1, typed, 1, 0, 3 /* STAGING */, 0, 0x20000 /* READ */, 0};
        void* staging = nullptr;
        if (dx<HRESULT>(dev, dd::CreateTexture2D, sd, nullptr, &staging) >= 0) {
            uint32_t box[6] = {uint32_t(px), uint32_t(py), 0, uint32_t(px) + 1, uint32_t(py) + 1, 1};
            dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, src, 0u, box);
            struct {
                uint8_t* data;
                UINT row, depth;
            } m{};
            if (dx<HRESULT>(ctx, dc::Map, staging, 0u, 1u, 0u, &m) >= 0) {
                std::memcpy(&val, m.data, 4);
                dx<void>(ctx, dc::Unmap, staging, 0u);
                ok = true;
            }
            dx_release(staging);
        }
    }
    if (resolved) dx_release(resolved);
    dx_release(dev);
    if (ok && (tex != last_tex || val != last_val)) {
        if (fmt == 26)
            log_write("trace", "    pixel %d,%d of %p = (%g %g %g)", px, py, tex, f11(val & 0x7ff, 6),
                      f11((val >> 11) & 0x7ff, 6), f11(val >> 22, 5));
        else
            log_write("trace", "    pixel %d,%d of %p (fmt %u) = 0x%08x", px, py, tex, fmt, val);
    }
    if (ok) last_tex = tex, last_val = val;
    dx_release(tex);
}

// FL_TRACE_DUMP_ECLS=a-b[/step]: dump RT0 after those ExecuteCommandLists of
// a traced frame (counted from 1 per frame), named e<k>_after_d<draw>.
uint32_t g_ecls = 0;
void after_ecl(void* ctx) {
    if (!g_active) return;
    g_ecls++;
    static uint32_t first = 1, last = 0, step = 1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("FL_TRACE_DUMP_ECLS")) sscanf(e, "%u-%u/%u", &first, &last, &step);
        if (!step) step = 1;
    }
    if (g_ecls < first || g_ecls > last || (g_ecls - first) % step) return;
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
    snprintf(name, sizeof name, "e%03u_after_d%03u_rt0.png", g_ecls, g_draws);
    dump_texture(ctx, res, frame_prefix() + name);
    dx_release(res);
}

void after_draw(void* ctx) {
    if (g_match_pending == ctx) {  // FL_TRACE_RT_MATCH: what the matched draw wrote
        g_match_pending = nullptr;
        static std::atomic<int> n{0};
        void* rtv = nullptr;
        void* dsv = nullptr;
        dx<void>(ctx, dc::OMGetRenderTargets, 1u, &rtv, &dsv);
        if (dsv) dx_release(dsv);
        if (rtv) {
            struct { uint32_t format, dim, u[4]; } vd{};
            dx<void>(rtv, kViewGetDesc, &vd);
            void* res = nullptr;
            dx<void>(rtv, kViewGetResource, &res);
            if (res) {
                char name[96];
                int k = n++;
                snprintf(name, sizeof name, "screenshots/trace/match_rt_%03d_%p_mip%u_slice%u.png", k, (void*)wrapper_of(res),
                         vd.u[0], vd.u[1]);
                log_write("trace", "    -> RT mip %u first slice %u count %u", vd.u[0], vd.u[1], vd.u[2]);
                if (k < 400 && vd.u[0] == 0 && vd.u[1] == 0) dump_texture(ctx, res, name);  // slice 0 mip 0 is what we dump
                dx_release(res);
            }
            dx_release(rtv);
        }
    }
    if (g_active) probe_pixel(ctx);
    if (!g_active || !g_dump) return;
    static const bool pass_mode = getenv("FL_TRACE_DUMP") && !strcmp(getenv("FL_TRACE_DUMP"), "pass");
    if (pass_mode) {
        if (g_last_compute) {
            g_last_compute = false;
            for (auto& u : g_last_uav)
                if (u) dx_release(u), u = nullptr;
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
        if (res != g_pass_rt) {
            flush_pass(ctx);
            g_pass_rt = res;  // keeps the reference
        } else {
            dx_release(res);
        }
        g_pass_last = g_draws;
        return;
    }
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
    // FL_TRACE_DUMP_DRAWS=a-b[/step]: only dump RT0 after those draws (and
    // the ExecuteCommandLists that follow them: suffix _e1, _e2, ...).
    static uint32_t first = 0, last = ~0u, step = 1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("FL_TRACE_DUMP_DRAWS"))
            sscanf(e, "%u-%u/%u", &first, &last, &step);
        if (!step) step = 1;
    }
    if (g_draws < first || g_draws > last || (g_draws - first) % step) return;
    static uint32_t sub_draw = ~0u, sub = 0;
    sub = sub_draw == g_draws ? sub + 1 : 0;
    sub_draw = g_draws;
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
    if (sub) snprintf(name, sizeof name, "d%03u_e%u_rt0.png", g_draws, sub);
    else snprintf(name, sizeof name, "d%03u_rt0.png", g_draws);
    dump_texture(ctx, res, frame_prefix() + name);
    dx_release(res);
}

void on_present(uint64_t frame) {
    static std::set<uint64_t> want = frames();
    if (g_active) log_write("trace", "==== end of frame %llu: %u draws/dispatches", (unsigned long long)g_frame, g_draws);
    g_frame = frame;
    g_active = want.count(frame) != 0;
    g_draws = 0;
    g_ecls = 0;
    g_dumped.clear();
    {
        std::lock_guard lock(g_mip_mutex);
        g_mip_states.clear();
    }
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

// IA, rasterizer, targets, blend and depth/stencil state of `ctx` (one line each).
void log_fixed_state(void* ctx) {
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

// Every binding and state object of `ctx` that a draw bundle could inherit
// (FL_TRACE_ECL_STATE=1 logs it at each traced ExecuteCommandList).
void log_state(void* ctx) {
    log_fixed_state(ctx);
    void* rs = nullptr;
    dx<void>(ctx, dc::RSGetState, &rs);
    if (rs) {
        uint32_t rd[10] = {};
        dx<void>(rs, 7 /* GetDesc */, rd);
        float clamp, slope;
        memcpy(&clamp, &rd[4], 4);
        memcpy(&slope, &rd[5], 4);
        log_write("trace", "    RS bias %d clamp %g slope %g scissor %u ms %u aa %u", int32_t(rd[3]), clamp, slope, rd[7],
                  rd[8], rd[9]);
        dx_release(rs);
    }
    void* dss = nullptr;
    UINT sref = 0;
    dx<void>(ctx, dc::OMGetDepthStencilState, &dss, &sref);
    if (dss) {
        uint32_t d[13] = {};
        dx<void>(dss, 7 /* GetDesc */, d);
        log_write("trace", "    DSS %p: %u %u %u %u %x %x | %u %u %u %u | %u %u %u %u ref %u", (void*)wrapper_of(dss), d[0], d[1],
                  d[2], d[3], d[4] & 0xff, d[5] & 0xff, d[6], d[7], d[8], d[9], d[10], d[11], d[12], 0u, sref);
        dx_release(dss);
    } else {
        log_write("trace", "    DSS default ref %u", sref);
    }
    struct Stage {
        const char* name;
        int shader, cbs, srvs, samplers;
    } stages[] = {
        {"VS", dc::VSGetShader, dc::VSGetConstantBuffers1, dc::VSGetShaderResources, dc::VSGetSamplers},
        {"PS", dc::PSGetShader, dc::PSGetConstantBuffers1, dc::PSGetShaderResources, dc::PSGetSamplers},
        {"GS", dc::GSGetShader, dc::GSGetConstantBuffers1, dc::GSGetShaderResources, dc::GSGetSamplers},
        {"HS", dc::HSGetShader, dc::HSGetConstantBuffers1, dc::HSGetShaderResources, dc::HSGetSamplers},
        {"DS", dc::DSGetShader, dc::DSGetConstantBuffers1, dc::DSGetShaderResources, dc::DSGetSamplers},
        {"CS", dc::CSGetShader, dc::CSGetConstantBuffers1, dc::CSGetShaderResources, dc::CSGetSamplers},
    };
    for (const Stage& st : stages) {
        void* sh = nullptr;
        dx<void>(ctx, st.shader, &sh, nullptr, nullptr);
        log_write("trace", "    %s shader %s", st.name, shader_str(sh).c_str());
        if (sh) dx_release(sh);
        void* cbs[14] = {};
        UINT first[14] = {}, num[14] = {};
        dx<void>(ctx, st.cbs, 0u, 14u, cbs, first, num);
        for (int i = 0; i < 14; i++)
            if (cbs[i]) {
                log_write("trace", "    %s cb%d %s +%u n%u", st.name, i, resource_str(cbs[i]).c_str(), first[i] * 16, num[i]);
                dx_release(cbs[i]);
            }
        void* srvs[64] = {};
        dx<void>(ctx, st.srvs, 0u, 64u, srvs);
        for (int i = 0; i < 64; i++)
            if (srvs[i]) {
                log_write("trace", "    %s t%d %s", st.name, i, view_str(srvs[i]).c_str());
                dx_release(srvs[i]);
            }
        void* smp[16] = {};
        dx<void>(ctx, st.samplers, 0u, 16u, smp);
        for (int i = 0; i < 16; i++)
            if (smp[i]) {
                log_write("trace", "    %s s%d %p", st.name, i, (void*)wrapper_of(smp[i]));
                dx_release(smp[i]);
            }
    }
    void* ib = nullptr;
    UINT ibfmt = 0, iboff = 0;
    dx<void>(ctx, dc::IAGetIndexBuffer, &ib, &ibfmt, &iboff);
    if (ib) {
        log_write("trace", "    IB %s fmt %u off %u", resource_str(ib).c_str(), ibfmt, iboff);
        dx_release(ib);
    }
    void* vb[8] = {};
    UINT stride[8] = {}, off[8] = {};
    dx<void>(ctx, dc::IAGetVertexBuffers, 0u, 8u, vb, stride, off);
    for (int i = 0; i < 8; i++)
        if (vb[i]) {
            log_write("trace", "    vb%d %s stride %u off %u", i, resource_str(vb[i]).c_str(), stride[i], off[i]);
            dx_release(vb[i]);
        }
}

void note_write(const char* how, void* dst_dxvk, void* src_dxvk) {
    if (!dst_dxvk || !log_enabled("firstwrite")) return;
    static std::mutex m;
    static std::set<void*> seen;
    {
        std::lock_guard lock(m);
        if (!seen.insert(dst_dxvk).second) return;
    }
    std::string dst = resource_str(dst_dxvk);
    log_write("firstwrite", "%s dst [%s]%s%s%s", how, dst.c_str(), src_dxvk ? " <- [" : "",
              src_dxvk ? resource_str(src_dxvk).c_str() : "", src_dxvk ? "]" : "");
}

void note_view_write(const char* how, void* view_dxvk) {
    if (!view_dxvk || !log_enabled("firstwrite")) return;
    void* res = nullptr;
    dx<void>(view_dxvk, kViewGetResource, &res);
    if (!res) return;
    note_write(how, res, nullptr);
    dx_release(res);
}

void log_dispatch(void* ctx, uint32_t x, uint32_t y, uint32_t z) {
    if (!log_enabled("dispatch")) return;
    void* cs = nullptr;
    dx<void>(ctx, dc::CSGetShader, &cs, nullptr, nullptr);
    void* uavs[2] = {};
    dx<void>(ctx, dc::CSGetUnorderedAccessViews, 0u, 2u, uavs);
    void* srvs[4] = {};
    dx<void>(ctx, dc::CSGetShaderResources, 0u, 4u, srvs);
    std::string line = "Dispatch(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ") CS " +
                       shader_str(cs);
    for (int i = 0; i < 2; i++)
        if (uavs[i]) line += " | u" + std::to_string(i) + " " + view_str(uavs[i]), dx_release(uavs[i]);
    for (int i = 0; i < 4; i++)
        if (srvs[i]) line += " | t" + std::to_string(i) + " " + view_str(srvs[i]), dx_release(srvs[i]);
    if (cs) dx_release(cs);
    log_write("dispatch", "%s", line.c_str());
}

void draw(void* ctx, const char* kind, uint32_t a, uint32_t b, uint32_t c) {
    // FL_TRACE_BUNDLES=1: also log every draw recorded on a deferred context,
    // whatever the frame (bundles are recorded once, at load, and replayed).
    static const bool bundles = getenv("FL_TRACE_BUNDLES") != nullptr;
    bool active = g_active;
    // FL_TRACE_RT_MATCH=<text>: log any draw (any frame) whose RT0 description
    // contains <text>, with its full state, e.g. "32x32 fmt 26 mips 6 arr 6".
    static const char* rt_match = getenv("FL_TRACE_RT_MATCH");
    if (!active && rt_match && strncmp(kind, "Dispatch", 8)) {
        void* rtv = nullptr;
        void* dsv0 = nullptr;
        dx<void>(ctx, dc::OMGetRenderTargets, 1u, &rtv, &dsv0);
        if (dsv0) dx_release(dsv0);
        std::string rts = rtv ? view_str(rtv) : "";
        if (rtv) dx_release(rtv);
        static std::atomic<int> logged{0};
        if (!rts.empty() && rts.find(rt_match) != std::string::npos && logged++ < 400) {
            g_match_pending = ctx;
            log_write("trace", "#match %s(%u, %u, %u) [ctx %p]", kind, a, b, c, ctx);
            void *vs = nullptr, *ps = nullptr;
            dx<void>(ctx, dc::VSGetShader, &vs, nullptr, nullptr);
            dx<void>(ctx, dc::PSGetShader, &ps, nullptr, nullptr);
            log_write("trace", "    VS %s PS %s", shader_str(vs).c_str(), shader_str(ps).c_str());
            if (vs) dx_release(vs);
            if (ps) dx_release(ps);
            log_fixed_state(ctx);
            log_constant_buffers(ctx, false);
            void* srvs[kTraceSrvs] = {};
            dx<void>(ctx, dc::PSGetShaderResources, 0u, UINT(kTraceSrvs), srvs);
            for (int i = 0; i < kTraceSrvs; i++)
                if (srvs[i]) {
                    log_write("trace", "    t%d %s", i, view_str(srvs[i]).c_str());
                    void* res = nullptr;
                    dx<void>(srvs[i], kViewGetResource, &res);
                    struct { uint32_t format, dim, u[4]; } vd{};
                    dx<void>(srvs[i], kViewGetDesc, &vd);
                    if (res && (vd.dim == 4 || vd.dim == 5 || vd.dim == 9)) {
                        static std::atomic<int> nd{0};
                        char name[96];
                        snprintf(name, sizeof name, "screenshots/trace/match_%03d_t%d_%p_mip%u.png", nd.load(), i, res,
                                 vd.u[0]);
                        if (nd++ < 60) {
                            g_dump_mip = vd.u[0];  // the view's most detailed mip
                            dump_texture(ctx, res, name);
                        }
                    }
                    if (res) dx_release(res);
                    dx_release(srvs[i]);
                }
        }
    }
    if (!active && bundles && dx<UINT>(ctx, dc::GetType) == 1 /* DEFERRED */) {
        log_write("trace", "#bundle %s(%u, %u, %u) [ctx %p]", kind, a, b, c, ctx);
        void* srvs[kTraceSrvs] = {};
        dx<void>(ctx, dc::PSGetShaderResources, 0u, UINT(kTraceSrvs), srvs);
        void *vs = nullptr, *ps = nullptr;
        dx<void>(ctx, dc::VSGetShader, &vs, nullptr, nullptr);
        dx<void>(ctx, dc::PSGetShader, &ps, nullptr, nullptr);
        log_write("trace", "    VS %s PS %s", shader_str(vs).c_str(), shader_str(ps).c_str());
        if (vs) dx_release(vs);
        if (ps) dx_release(ps);
        static const bool full = !strcmp(getenv("FL_TRACE_BUNDLES"), "full");
        if (full) log_fixed_state(ctx);
        else log_targets(ctx, "    ");
        for (int i = 0; i < kTraceSrvs; i++)
            if (srvs[i]) {
                log_write("trace", "    t%d %s", i, view_str(srvs[i]).c_str());
                dx_release(srvs[i]);
            }
        return;
    }
    if (!active) return;
    g_draws++;
    bool compute = !strncmp(kind, "Dispatch", 8);
    log_write("trace", "#%u %s(%u, %u, %u) [ctx %p]", g_draws, kind, a, b, c, ctx);
    void* srvs[kTraceSrvs] = {};
    if (compute) {
        void* cs = nullptr;
        dx<void>(ctx, dc::CSGetShader, &cs, nullptr, nullptr);
        log_write("trace", "    CS %s", shader_str(cs).c_str());
        if (cs) dx_release(cs);
        dx<void>(ctx, dc::CSGetShaderResources, 0u, UINT(kTraceSrvs), srvs);
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
        dx<void>(ctx, dc::PSGetShaderResources, 0u, UINT(kTraceSrvs), srvs);
        log_constant_buffers(ctx, false);
        log_constant_buffers(ctx, false, dc::VSGetConstantBuffers1, "vs-");
        log_fixed_state(ctx);
    }
    for (int i = 0; i < kTraceSrvs; i++)
        if (srvs[i]) {
            log_write("trace", "    t%d %s", i, view_str(srvs[i]).c_str());
            if (g_dump) {
                void* res = nullptr;
                dx<void>(srvs[i], kViewGetResource, &res);
                struct { uint32_t format, dim, u[4]; } vd{};
                dx<void>(srvs[i], kViewGetDesc, &vd);
                if (res && vd.dim == 8 /* TEXTURE3D */ && g_dumped.insert(res).second) stat_texture3d(ctx, res, "srv");
                if (res && (vd.dim == 1 || vd.dim == 11) /* BUFFER(EX) */ && g_dumped.insert(res).second) stat_buffer(ctx, res);
                // FL_TRACE_DUMP_SLOTS=a-b: only dump textures bound in those slots.
                static uint32_t slot_lo = 0, slot_hi = ~0u;
                static bool slots_parsed = false;
                if (!slots_parsed) {
                    slots_parsed = true;
                    if (const char* e = getenv("FL_TRACE_DUMP_SLOTS")) sscanf(e, "%u-%u", &slot_lo, &slot_hi);
                }
                bool slot_ok = uint32_t(i) >= slot_lo && uint32_t(i) <= slot_hi;
                // TEXTURE2D, and slice/face 0 of TEXTURE2DARRAY / TEXTURECUBE
                if (res && slot_ok && (vd.dim == 4 || vd.dim == 5 || vd.dim == 9) && g_dumped.insert(res).second) {
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
