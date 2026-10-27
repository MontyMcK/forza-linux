#include "hle/d3d11x/gpu_profile.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/log.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/d3d11x/trace.h"

namespace fl::d3d::gpuprof {
namespace {

namespace dc = desktop::ctx;
namespace dd = desktop::dev;

struct Mark {
    void* query;
    const char* kind;
    void* rt;  // resources (referenced) of RT0 and the depth target, or null
    void* ds;
    bool compute;
};

std::set<uint64_t> g_frames;
std::vector<double> g_times;  // FL_GPU_PROFILE_AT_TIME: run times (s); the next frame after each is profiled
size_t g_next_time = 0;
bool g_active = false;
void* g_disjoint = nullptr;
std::vector<Mark> g_marks;
uint64_t g_frame = 0;

void* resource_of(void* view) {
    if (!view) return nullptr;
    void* res = nullptr;
    dx<void>(view, 7 /* ID3D11View::GetResource */, &res);
    dx_release(view);
    return res;
}

void* new_query(void* dxctx, uint32_t type) {
    void* dev = nullptr;
    dx<void>(dxctx, 3 /* GetDevice */, &dev);
    uint32_t desc[2] = {type, 0};
    void* q = nullptr;
    dx<HRESULT>(dev, dd::CreateQuery, desc, &q);
    dx_release(dev);
    return q;
}

// Reads the results back (waits for the GPU) and logs per-pass totals.
void report(void* dxctx) {
    struct { uint64_t freq; int32_t disjoint; } dj{};
    while (dx<HRESULT>(dxctx, dc::GetData, g_disjoint, &dj, UINT(sizeof dj), 0u) == 1) {
    }
    std::vector<uint64_t> t(g_marks.size());
    for (size_t i = 0; i < g_marks.size(); i++)
        while (dx<HRESULT>(dxctx, dc::GetData, g_marks[i].query, &t[i], UINT(sizeof t[i]), 0u) == 1) {
        }
    double to_ms = dj.freq ? 1000.0 / double(dj.freq) : 0;
    struct Sum { double ms = 0; int ops = 0; std::map<std::string, int> kinds; };
    std::map<std::string, Sum> sums;
    double total = 0;
    for (size_t i = 0; i + 1 < g_marks.size(); i++) {
        double ms = double(t[i + 1] - t[i]) * to_ms;
        const Mark& m = g_marks[i];
        std::string key = m.compute ? "compute" : "";
        if (!m.compute) {
            key += m.rt ? trace::describe_dxvk(m.rt) : "no RT";
            key += " | ";
            key += m.ds ? trace::describe_dxvk(m.ds) : "no DS";
        }
        Sum& s = sums[key];
        s.ms += ms, s.ops++, s.kinds[m.kind]++;
        total += ms;
    }
    std::vector<std::pair<std::string, Sum>> v(sums.begin(), sums.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ms > b.second.ms; });
    log_write("gpuprof", "frame %llu: %.2f ms GPU between first and last operation (%zu operations)%s",
              (unsigned long long)g_frame, total, g_marks.size(), dj.disjoint ? " DISJOINT (unreliable)" : "");
    for (auto& [key, s] : v) {
        if (s.ms < 0.02) continue;
        std::string kinds;
        for (auto& [k, n] : s.kinds) kinds += " " + std::string(k) + "x" + std::to_string(n);
        log_write("gpuprof", "  %6.2f ms %5.1f%%  %4d ops [%s]  %s", s.ms, total > 0 ? 100 * s.ms / total : 0, s.ops,
                  kinds.c_str() + (kinds.empty() ? 0 : 1), key.c_str());
    }
    // The longest single operations, in frame order.
    std::vector<size_t> idx;
    for (size_t i = 0; i + 1 < g_marks.size(); i++) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return t[a + 1] - t[a] > t[b + 1] - t[b]; });
    for (size_t k = 0; k < idx.size() && k < 15; k++) {
        size_t i = idx[k];
        const Mark& m = g_marks[i];
        log_write("gpuprof", "  op %4zu %-14s %6.3f ms  %s", i, m.kind, double(t[i + 1] - t[i]) * to_ms,
                  m.compute ? "compute" : m.rt ? trace::describe_dxvk(m.rt) : m.ds ? trace::describe_dxvk(m.ds) : "-");
    }
    for (Mark& m : g_marks) {
        dx_release(m.query);
        if (m.rt) dx_release(m.rt);
        if (m.ds) dx_release(m.ds);
    }
    g_marks.clear();
    dx_release(g_disjoint);
    g_disjoint = nullptr;
}

}  // namespace

bool active() { return g_active; }

void mark(void* dxctx, const char* kind) {
    if (!g_active) return;
    void* q = new_query(dxctx, 2 /* D3D11_QUERY_TIMESTAMP */);
    if (!q) return;
    Mark m{q, kind, nullptr, nullptr, kind[0] == 'D' && kind[1] == 'i'};  // "Dispatch..."
    if (!m.compute) {
        void* rtv = nullptr;
        void* dsv = nullptr;
        dx<void>(dxctx, dc::OMGetRenderTargets, 1u, &rtv, &dsv);
        m.rt = resource_of(rtv);
        m.ds = resource_of(dsv);
    }
    dx<void>(dxctx, dc::End, q);
    g_marks.push_back(m);
}

void on_present(void* dxctx, uint64_t frame) {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("FL_GPU_PROFILE"))
            for (const char* p = e; *p;) {
                g_frames.insert(strtoull(p, const_cast<char**>(&p), 10));
                while (*p == ',' || *p == ' ') p++;
            }
        if (const char* e = getenv("FL_GPU_PROFILE_AT_TIME"))
            for (const char* p = e; *p;) {
                g_times.push_back(strtod(p, const_cast<char**>(&p)));
                while (*p == ',' || *p == ' ') p++;
            }
        std::sort(g_times.begin(), g_times.end());
    }
    if (g_active) {
        mark(dxctx, "Present");
        dx<void>(dxctx, dc::End, g_disjoint);
        g_active = false;
        report(dxctx);
    }
    bool by_time = g_next_time < g_times.size() && run_time() >= g_times[g_next_time];
    while (g_next_time < g_times.size() && run_time() >= g_times[g_next_time]) g_next_time++;
    if (g_frames.count(frame) || by_time) {
        g_frame = frame;
        g_disjoint = new_query(dxctx, 3 /* D3D11_QUERY_TIMESTAMP_DISJOINT */);
        if (g_disjoint) {
            dx<void>(dxctx, dc::Begin, g_disjoint);
            g_active = true;
        }
    }
}

}  // namespace fl::d3d::gpuprof
