// DXGI wrappers: adapter, output, factory, swap chain (Xbox slot order:
// IDXGIObject has one extra method after SetPrivateDataInterface, so every
// desktop slot >= 5 is +1; FINDINGS §3.1).
//
// The swap chain lives on our SDL window (platform/window.h). Screenshots for
// automated checks: FL_SCREENSHOT_AT_FRAME=N[,M...] writes screenshots/frame_N.png,
// FL_EXIT_AFTER_FRAME=M exits after frame M.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "loader/crash.h"
#include "common/png.h"
#include "hle/d3d11x/gpu_profile.h"
#include "hle/d3d11x/trace.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/registry.h"
#include "platform/window.h"

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Object;
using winrt::Vtable;

// A DXGI object that forwards to a DXVK object.
struct DxgiObject : Object {
    DxgiObject(const char16_t* name, Vtable& vt, void* dxvk_obj) : Object(name), iface(vt, this), dxvk(dxvk_obj) {
        expose(IID_IDXGIObject, &iface);
    }
    ~DxgiObject() override { dx_release(dxvk); }
    Iface iface;
    void* dxvk;
    PrivateData priv;
};

WINAPI HRESULT dxgiobj_SetPrivateData(Iface* self, const GUID& g, UINT size, const void* data) {
    return self->as<DxgiObject>()->priv.set(g, size, data);
}
WINAPI HRESULT dxgiobj_SetPrivateDataInterface(Iface* self, const GUID& g, void* unknown) {
    return self->as<DxgiObject>()->priv.set_interface(g, unknown);
}
WINAPI HRESULT dxgiobj_GetPrivateData(Iface* self, const GUID& g, UINT* size, void* data) {
    return self->as<DxgiObject>()->priv.get(g, size, data);
}

// 3 SetPrivateData, 4 SetPrivateDataInterface, 5 extra, 6 GetPrivateData, 7 GetParent.
Vtable& dxgi_base(Vtable& vt) {
    return vt.set(0, &d3d_QueryInterface)
        .set(3, &dxgiobj_SetPrivateData)
        .set(4, &dxgiobj_SetPrivateDataInterface)
        .set(6, &dxgiobj_GetPrivateData);
}

void* make_factory(void* dxvk_factory);

// ---- IDXGIOutput (slot count unverified: 64 is generous) ----

WINAPI HRESULT output_GetDesc(Iface* self, void* desc) {
    return dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::output::GetDesc, desc);
}

// Xbox slot 15 (desktop 14). The game passes a DXGI_GAMMA_CONTROL with a 1025-entry ramp (rva 0x10ef55d).
WINAPI HRESULT output_SetGammaControl(Iface* self, const void* gamma) {
    HRESULT hr = dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::output::SetGammaControl, gamma);
    if (hr < 0) log_once("IDXGIOutput::SetGammaControl failed in DXVK (ignored)", static_cast<uint32_t>(hr));
    return S_OK;
}
WINAPI HRESULT output_GetGammaControl(Iface* self, void* gamma) {
    return dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::output::GetGammaControl, gamma);
}
WINAPI HRESULT output_GetGammaControlCapabilities(Iface* self, void* caps) {
    return dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::output::GetGammaControlCapabilities, caps);
}
WINAPI HRESULT output_GetDisplayModeList(Iface* self, UINT format, UINT flags, UINT* num, void* modes) {
    return dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::output::GetDisplayModeList, format, flags, num, modes);
}
WINAPI HRESULT output_WaitForVBlank(Iface* self) {
    if (getenv("FL_TRACE_TIME")) {  // rate and callers, every 10 s
        static std::atomic<uint64_t> calls{0};
        static std::atomic<int64_t> since{0};
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        int64_t start = since.load();
        if (!start) since.compare_exchange_strong(start, now);
        uint64_t n = ++calls;
        if (start && now - start >= 10000000000LL && since.compare_exchange_strong(start, now)) {
            calls = 0;
            log_write("time", "WaitForVBlank: %.1f/s (caller %p)", n / ((now - start) / 1e9),
                      __builtin_return_address(0));
        }
    }
    return dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::output::WaitForVBlank);
}

Vtable& output_vt() {
    static Vtable vt = [] {
        namespace dout = desktop::output;
        Vtable v("IDXGIOutputX", 64, false);
        dxgi_base(v)
            .set(dxgi_slot(dout::GetDesc), &output_GetDesc)
            .set(dxgi_slot(dout::GetDisplayModeList), &output_GetDisplayModeList)
            .set(dxgi_slot(dout::WaitForVBlank), &output_WaitForVBlank)
            .set(dxgi_slot(dout::GetGammaControlCapabilities), &output_GetGammaControlCapabilities)
            .set(dxgi_slot(dout::SetGammaControl), &output_SetGammaControl)
            .set(dxgi_slot(dout::GetGammaControl), &output_GetGammaControl);
        return v;
    }();
    return vt;
}

// ---- IDXGIAdapter ----

// Xbox slot 8 (desktop 7). Game call: (0, &output), kept at renderer+0x28 (rva 0x10d89a2).
WINAPI HRESULT adapter_EnumOutputs(Iface* self, UINT index, void** out) {
    auto* a = self->as<DxgiObject>();
    void* output = nullptr;
    HRESULT hr = dx<HRESULT>(a->dxvk, desktop::adapter::EnumOutputs, index, &output);
    FL_LOG(d3d, "IDXGIAdapter::EnumOutputs(%u) -> 0x%08x", index, static_cast<uint32_t>(hr));
    if (hr < 0) {
        *out = nullptr;
        return hr;
    }
    auto* o = new DxgiObject(u"DXGIOutput", output_vt(), output);
    o->expose(IID_IDXGIOutput, &o->iface);
    *out = &o->iface;
    return S_OK;
}

WINAPI HRESULT adapter_GetDesc(Iface* self, void* desc) {
    return dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::adapter::GetDesc, desc);
}

// Xbox slot 7 (desktop 6). Game call at rva 0x10ca9ae asks for IDXGIFactory2.
WINAPI HRESULT adapter_GetParent(Iface* self, const GUID& iid, void** out) {
    void* f = nullptr;
    HRESULT hr = dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::adapter::GetParent, &IID_IDXGIFactory2, &f);
    if (hr < 0) {
        FL_ERROR("d3d: IDXGIAdapter::GetParent(%s) failed: 0x%08x", winrt::guid_str(iid).c_str(),
                 static_cast<uint32_t>(hr));
        *out = nullptr;
        return hr;
    }
    auto* w = static_cast<Iface*>(make_factory(f));
    hr = w->owner->query(iid, out);
    w->owner->release();
    return hr;
}

Vtable& adapter_vt() {
    static Vtable vt = [] {
        Vtable v("IDXGIAdapterX", 64, false);  // slot count unverified
        dxgi_base(v)
            .set(7, &adapter_GetParent)
            .set(dxgi_slot(desktop::adapter::EnumOutputs), &adapter_EnumOutputs)
            .set(dxgi_slot(desktop::adapter::GetDesc), &adapter_GetDesc);
        return v;
    }();
    return vt;
}

// ---- IDXGISwapChain1 ----

struct SwapChain : DxgiObject {
    SwapChain(Vtable& vt, void* sc, DeviceX* dev) : DxgiObject(u"DXGISwapChain", vt, sc), device(dev) {
        device->add_ref();
    }
    ~SwapChain() override { device->release(); }
    DeviceX* device;
    uint64_t frame = 0;
};

std::set<uint64_t> screenshot_frames() {
    std::set<uint64_t> s;
    if (const char* e = getenv("FL_SCREENSHOT_AT_FRAME")) {
        for (const char* p = e; *p;) {
            s.insert(strtoull(p, const_cast<char**>(&p), 10));
            while (*p == ',' || *p == ' ') p++;
        }
    }
    return s;
}

// FL_SCREENSHOT_AT_TIME=t[,t...]: screenshots/time_<t>.png at the first Present
// after t seconds of run time (to compare game speed at different frame rates).
std::vector<double> screenshot_times() {
    std::vector<double> v;
    if (const char* e = getenv("FL_SCREENSHOT_AT_TIME"))
        for (const char* p = e; *p;) {
            v.push_back(strtod(p, const_cast<char**>(&p)));
            while (*p == ',' || *p == ' ') p++;
        }
    std::sort(v.begin(), v.end());
    return v;
}

// Copy back buffer 0 to a PNG (RGBA8 / BGRA8 / RGB10A2 back buffers).
void screenshot(SwapChain* sc, std::string name = "") {
    namespace dd = desktop::dev;
    namespace dc = desktop::ctx;
    void* tex = nullptr;
    if (HRESULT hr = dx<HRESULT>(sc->dxvk, desktop::swapchain::GetBuffer, 0u, &IID_ID3D11Texture2D, &tex); hr < 0) {
        FL_ERROR("d3d: screenshot: GetBuffer failed 0x%08x", static_cast<uint32_t>(hr));
        return;
    }
    uint32_t desc[11];
    dx<void>(tex, 10 /* ID3D11Texture2D::GetDesc */, desc);
    uint32_t w = desc[0], h = desc[1], fmt = desc[4];
    desc[2] = desc[3] = 1;        // mips, array
    desc[5] = 1, desc[6] = 0;     // samples
    desc[7] = 3;                  // STAGING
    desc[8] = 0;                  // bind
    desc[9] = 0x20000;            // CPU_ACCESS_READ
    desc[10] = 0;
    void* staging = nullptr;
    void* dev = sc->device->dxvk;
    void* ctx = sc->device->immediate->dxvk;
    if (HRESULT hr = dx<HRESULT>(dev, dd::CreateTexture2D, desc, nullptr, &staging); hr < 0) {
        FL_ERROR("d3d: screenshot: staging texture (%ux%u fmt %u) failed 0x%08x", w, h, fmt, static_cast<uint32_t>(hr));
    } else {
        dx<void>(ctx, dc::CopySubresourceRegion, staging, 0u, 0u, 0u, 0u, tex, 0u, nullptr);
        struct { uint8_t* data; UINT row, depth; } m{};
        if (HRESULT hr = dx<HRESULT>(ctx, dc::Map, staging, 0u, 1u /* READ */, 0u, &m); hr < 0) {
            FL_ERROR("d3d: screenshot: Map failed 0x%08x", static_cast<uint32_t>(hr));
        } else {
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t* src = m.data + size_t(y) * m.row;
                uint8_t* dst = &rgba[size_t(y) * w * 4];
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t px;
                    std::memcpy(&px, src + x * 4, 4);
                    uint8_t r, g, b;
                    if (fmt == 24 || fmt == 25) {  // R10G10B10A2
                        r = (px & 0x3ff) >> 2, g = ((px >> 10) & 0x3ff) >> 2, b = ((px >> 20) & 0x3ff) >> 2;
                    } else if (fmt >= 87 && fmt <= 93) {  // B8G8R8A8 / B8G8R8X8
                        b = px, g = px >> 8, r = px >> 16;
                    } else {
                        r = px, g = px >> 8, b = px >> 16;
                    }
                    dst[x * 4] = r, dst[x * 4 + 1] = g, dst[x * 4 + 2] = b, dst[x * 4 + 3] = 255;
                }
            }
            dx<void>(ctx, dc::Unmap, staging, 0u);
            mkdir("screenshots", 0755);
            std::string path = "screenshots/" + (name.empty() ? "frame_" + std::to_string(sc->frame) : name) + ".png";
            bool ok = write_png(path, rgba.data(), w, h);
            log_write("d3d", "screenshot %s (%ux%u fmt %u): %s", path.c_str(), w, h, fmt, ok ? "ok" : "FAILED");
        }
        dx_release(staging);
    }
    dx_release(tex);
}

// Frame pacing. The game was made for the console's 59.94 Hz display and presents
// with sync interval 2: one frame every 33.3 ms. Its simulation advances a fixed
// step per frame, so on a 240 Hz monitor (interval 2 = 120 fps) it ran 4x too
// fast. Present waits on a virtual 59.94 Hz vblank clock instead of the
// monitor's: `sync` vblanks per frame, whatever the display. DXVK then gets
// the frame at once (see sc_Present).
// FL_FRAME_PACING=dynamic: the game's update follows real time (Config), so
// frames go out as fast as the display takes them, or FL_FPS_CAP per second.
// FL_FRAME_PACING=off: the old behaviour (DXVK waits `sync` monitor vblanks).
void pace_present(UINT sync) {
    static int64_t next = 0;
    const Config& c = config();
    int64_t period;
    if (c.pacing == Config::Pacing::console) {
        if (sync == 0) return;
        period = sync * 16'683'333LL;  // a 59.94 Hz vblank (1001/60000 s): the game steps 1/29.97 s per frame
    } else if (c.pacing == Config::Pacing::dynamic) {
        // Without a cap, the display's refresh rate: unpaced, the loading
        // screen presented ~15000 frames a second and starved the loaders.
        static int64_t refresh_period = [] {
            float hz = platform::refresh_rate();
            return static_cast<int64_t>(1e9 / (hz > 20 ? hz : 60));
        }();
        period = c.fps_cap > 0 ? 1'000'000'000LL / c.fps_cap : refresh_period;
    } else {
        return;
    }
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
    // A frame more than 4 ms late restarts the cadence from now, as a late flip
    // does on the console. Keeping the old grid would hand the next frames over
    // back to back (a 60 ms frame, then 14 and 22 ms ones), which looks like
    // a second stutter.
    if (!next || now > next + 4'000'000LL) {
        next = now;
    } else if (now < next) {
        timespec until{static_cast<time_t>(next / 1'000'000'000LL), static_cast<long>(next % 1'000'000'000LL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {
        }
    }
    next += period;
}

// FL_LOG=fps: every 10 s, statistics of the intervals at which frames are
// handed to the display (after pacing): average, 1% low, worst frame, frames
// over 50 ms, percentiles and a histogram, to measure stutter.
// DXVK's backend counters (tools/dxvk-patches/0006), in DxvkStatCounter order.
extern "C" void dxvk_xbox_stat_counters(void* device, uint64_t* counters, uint32_t count);

std::string dxvk_stats(void* device, size_t frames) {
    enum { kDraws, kMerged, kDispatches, kPasses, kBarriers, kPipeG, kPipeL, kPipeC, kTasksDone, kTasksTotal,
           kSubmits, kPresents, kGpuSyncs, kGpuSyncTicks, kGpuIdle, kCsSyncs, kCsSyncTicks, kCsIdle, kCsChunks, kN };
    static uint64_t last[kN] = {};
    uint64_t now[kN] = {};
    dxvk_xbox_stat_counters(device, now, kN);
    auto d = [&](int i) { return double(now[i] - last[i]) / double(frames ? frames : 1); };
    char buf[400];
    snprintf(buf, sizeof buf,
             "dxvk per frame: %.0f draws, %.0f dispatches, %.0f render passes, %.0f barriers, %.1f submits, "
             "%.0f CS chunks, %.2f CS syncs (%.2f ms), %.2f GPU syncs (%.2f ms), GPU idle %.2f ms, pipelines +%llu",
             d(kDraws), d(kDispatches), d(kPasses), d(kBarriers), d(kSubmits), d(kCsChunks), d(kCsSyncs),
             d(kCsSyncTicks) / 1000.0, d(kGpuSyncs), d(kGpuSyncTicks) / 1000.0, d(kGpuIdle) / 1000.0,
             (unsigned long long)(now[kPipeG] - last[kPipeG] + now[kPipeC] - last[kPipeC]));
    memcpy(last, now, sizeof last);
    return buf;
}

void frame_stats(void* device) {
    if (!log_enabled("fps")) return;
    static std::vector<double> ms;
    static int64_t last = 0, window = 0;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
    if (last) {
        ms.push_back((now - last) / 1e6);
        if (ms.back() > 40) log_write("fps", "hitch: %.1f ms frame", ms.back());  // to line up with other events
    }
    last = now;
    if (!window) window = now;
    if (now - window < 10'000'000'000LL || ms.empty()) return;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double total = 0;
    int hitches = 0;
    for (double m : ms) {
        total += m;
        hitches += m > 50;
    }
    auto pct = [&](size_t p) { return sorted[std::min(sorted.size() - 1, sorted.size() * p / 100)]; };
    double p99 = pct(99);
    log_write("fps", "%zu frames: %.1f fps average, 1%% low %.1f fps (%.1f ms), worst %.1f ms, %d over 50 ms; "
              "frame time p10/p50/p90 %.1f/%.1f/%.1f ms",
              ms.size(), ms.size() * 1000.0 / total, 1000.0 / p99, p99, sorted.back(), hitches, pct(10), pct(50),
              pct(90));
    {
        // Frame-time histogram, 2 ms buckets.
        int buckets[32] = {};
        for (double m : ms) buckets[std::min(31, static_cast<int>(m / 2))]++;
        std::string h;
        for (int i = 0; i < 32; i++)
            if (buckets[i]) h += " " + std::to_string(i * 2) + (i == 31 ? "+" : "") + ":" + std::to_string(buckets[i]);
        log_write("fps", "  frame times (ms:count):%s", h.c_str());
    }
    log_write("fps", "  %s", placement_stats().c_str());
    log_write("fps", "  %s", context_stats().c_str());
    log_write("fps", "  %s", dxvk_stats(device, ms.size()).c_str());
    ms.clear();
    window = now;
}

WINAPI HRESULT sc_Present(Iface* self, UINT sync, UINT flags) {
    auto* sc = self->as<SwapChain>();
    static std::set<uint64_t> shots = screenshot_frames();
    static uint64_t exit_after = getenv("FL_EXIT_AFTER_FRAME") ? strtoull(getenv("FL_EXIT_AFTER_FRAME"), 0, 10) : 0;
    sc->frame++;
    FL_LOG(fence, "present -> frame %llu", static_cast<unsigned long long>(sc->frame));
    if (sc->frame == 1) log_write("d3d", "first Present (sync %u flags 0x%x)", sync, flags);
    if (shots.count(sc->frame)) screenshot(sc);
    static std::vector<double> times = screenshot_times();
    static size_t next_time = 0;
    if (next_time < times.size() && run_time() >= times[next_time]) {
        char name[32];
        snprintf(name, sizeof name, "time_%g", times[next_time]);
        screenshot(sc, name);
        while (next_time < times.size() && run_time() >= times[next_time]) next_time++;
    }
    gpuprof::on_present(sc->device->immediate->dxvk, sc->frame + 1);
    trace::on_present(sc->frame + 1);
    if (config().pacing != Config::Pacing::off) {
        pace_present(sync);
        // Our clock sets the cadence, so DXVK must not wait on the monitor too:
        // a FIFO present blocks up to a refresh plus the frame queue, and the
        // frames came out at 39/28 ms instead of 33. Interval 0 (mailbox in a
        // compositor: no tearing) hands the frame over at once.
        // FL_VSYNC=1: FIFO with interval 1 instead.
        static const bool vsync = getenv("FL_VSYNC") && strcmp(getenv("FL_VSYNC"), "0") != 0;
        sync = (sync && vsync) ? 1 : 0;
    }
    loader::note_present();
    frame_stats(sc->device->dxvk);  // after pacing: the cadence frames are handed to the display at
    HRESULT hr = dx<HRESULT>(sc->dxvk, desktop::swapchain::Present, sync, flags & 0xff);
    placement_frame_scan();
    if (exit_after && sc->frame >= exit_after) {
        log_write("d3d", "FL_EXIT_AFTER_FRAME=%llu reached", (unsigned long long)exit_after);
        _exit(0);
    }
    return hr;
}

// Back buffers are wrapped once (the registry maps DXVK texture -> wrapper).
WINAPI HRESULT sc_GetBuffer(Iface* self, UINT index, const GUID& iid, void** out) {
    auto* sc = self->as<SwapChain>();
    void* tex = nullptr;
    HRESULT hr = dx<HRESULT>(sc->dxvk, desktop::swapchain::GetBuffer, index, &IID_ID3D11Texture2D, &tex);
    if (hr < 0) {
        *out = nullptr;
        return hr;
    }
    if (DeviceChild* w = wrapper_of(tex)) {
        dx_release(tex);
        return w->query(iid, out);
    }
    uint32_t xd[13] = {};
    dx<void>(tex, 10 /* ID3D11Texture2D::GetDesc */, xd);
    auto* r = new Resource(u"D3D11Texture2D (back buffer)", sc->device, texture2d_vtable(), tex, 3, xd, 0x34);
    r->expose(IID_ID3D11Texture2D, &r->iface);
    r->gpu_owned = true;
    hr = r->query(iid, out);
    r->release();
    return hr;
}

WINAPI HRESULT sc_GetDesc1(Iface* self, void* desc) {
    return dx<HRESULT>(self->as<SwapChain>()->dxvk, desktop::swapchain::GetDesc1, desc);
}
WINAPI HRESULT sc_GetDesc(Iface* self, void* desc) {
    return dx<HRESULT>(self->as<SwapChain>()->dxvk, desktop::swapchain::GetDesc, desc);
}
WINAPI HRESULT sc_ResizeBuffers(Iface* self, UINT count, UINT w, UINT h, UINT fmt, UINT flags) {
    return dx<HRESULT>(self->as<SwapChain>()->dxvk, desktop::swapchain::ResizeBuffers, count, w, h, fmt, flags);
}
WINAPI HRESULT sc_GetDevice(Iface* self, const GUID& iid, void** out) {
    return self->as<SwapChain>()->device->query(iid, out);
}

Vtable& swapchain_vt() {
    static Vtable vt = [] {
        namespace ds = desktop::swapchain;
        Vtable v("IDXGISwapChainX", 48, false);
        dxgi_base(v)
            .set(dxgi_slot(ds::GetDevice), &sc_GetDevice)
            .set(dxgi_slot(ds::Present), &sc_Present)
            .set(dxgi_slot(ds::GetBuffer), &sc_GetBuffer)
            .set(dxgi_slot(ds::GetDesc), &sc_GetDesc)
            .set(dxgi_slot(ds::ResizeBuffers), &sc_ResizeBuffers)
            .set(dxgi_slot(ds::GetDesc1), &sc_GetDesc1);
        return v;
    }();
    return vt;
}

// ---- IDXGIFactory2 ----

// DXGI_SWAP_CHAIN_DESC1: Width Height Format Stereo SampleCount SampleQuality
// BufferUsage BufferCount Scaling SwapEffect AlphaMode Flags.
struct SwapChainDesc1 {
    UINT w, h, format, stereo, samples, quality, usage, count, scaling, effect, alpha, flags;
};

// Xbox slot 17 (desktop 16). Game call at rva 0x10ca9fa: (device, CoreWindow, desc, null, &out).
WINAPI HRESULT factory_CreateSwapChainForCoreWindow(Iface* self, void* device, void* window,
                                                    const SwapChainDesc1* desc, void* restrict_output, void** out) {
    auto* dev = static_cast<Iface*>(device)->as<DeviceX>();
    SwapChainDesc1 d = *desc;
    log_write("d3d", "CreateSwapChainForCoreWindow(%ux%u fmt %u samples %u usage 0x%x count %u scaling %u effect %u "
              "alpha %u flags 0x%x)", d.w, d.h, d.format, d.samples, d.usage, d.count, d.scaling, d.effect, d.alpha,
              d.flags);
    if (d.flags) log_once("swap chain: Xbox flags (ignored)", d.flags);
    d.flags = 0;
    d.scaling = 0;  // DXGI_SCALING_STRETCH: the window size is ours, not the game's
    SDL_Window* win = platform::window();
    void* sc = nullptr;
    HRESULT hr = dx<HRESULT>(self->as<DxgiObject>()->dxvk, desktop::factory::CreateSwapChainForHwnd, dev->dxvk,
                             static_cast<void*>(win), &d, nullptr, nullptr, &sc);
    if (hr < 0) {
        FL_ERROR("d3d: CreateSwapChainForHwnd failed: 0x%08x", static_cast<uint32_t>(hr));
        *out = nullptr;
        return hr;
    }
    auto* w = new SwapChain(swapchain_vt(), sc, dev);
    w->expose(IID_IDXGISwapChain, &w->iface);
    w->expose(IID_IDXGISwapChain1, &w->iface);
    *out = &w->iface;
    return S_OK;
}

Vtable& factory_vt() {
    static Vtable vt = [] {
        Vtable v("IDXGIFactoryX", 48, false);
        dxgi_base(v).set(dxgi_slot(desktop::factory::CreateSwapChainForCoreWindow),
                         &factory_CreateSwapChainForCoreWindow);
        return v;
    }();
    return vt;
}

void* make_factory(void* dxvk_factory) {
    auto* f = new DxgiObject(u"DXGIFactory", factory_vt(), dxvk_factory);
    f->expose(IID_IDXGIFactory, &f->iface);
    f->expose(IID_IDXGIFactory1, &f->iface);
    f->expose(IID_IDXGIFactory2, &f->iface);
    return &f->iface;
}

// d3d11_x!DXGIXPresentArray(sync interval, flags, ?, count, swap chains, plane params).
// Game call at rva 0x10f484d: (0/1/2, flags, 0, 1, &swapchain, &params). The
// params (source size, scale, frame counter) only matter for the Xbox
// compositor; we just present each swap chain.
WINAPI HRESULT DXGIXPresentArray_(UINT sync, UINT flags, UINT unknown, UINT count, void* const* swapchains,
                                  const void* params) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        log_write("d3d", "DXGIXPresentArray(sync %u flags 0x%x unknown %u count %u)", sync, flags, unknown, count);
    }
    HRESULT hr = S_OK;
    for (UINT i = 0; i < count; i++)
        if (swapchains[i]) hr = sc_Present(static_cast<Iface*>(swapchains[i]), sync, 0);
    return hr;
}

}  // namespace

HLE_EXPORT(d3d11_x, DXGIXPresentArray, DXGIXPresentArray_);

void* make_adapter(void* dxvk_adapter) {
    auto* a = new DxgiObject(u"DXGIAdapter", adapter_vt(), dxvk_adapter);
    a->expose(IID_IDXGIAdapter, &a->iface);
    a->expose(IID_IDXGIAdapter1, &a->iface);
    return &a->iface;
}

}  // namespace fl::d3d
