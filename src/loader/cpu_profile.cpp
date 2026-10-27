#include "loader/cpu_profile.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

#include "common/log.h"
#include "loader/pe_image.h"

namespace fl::loader {
namespace {

struct Sample {
    uint64_t rip;
    uint64_t ret;  // [rsp]: the caller of a leaf function such as a libc syscall wrapper
    uint32_t tid;
};
constexpr size_t kMax = 1 << 20;
Sample* g_samples = nullptr;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_on{false};

void on_prof(int, siginfo_t*, void* uc) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMax) return;
    int saved = errno;
    const greg_t* g = static_cast<ucontext_t*>(uc)->uc_mcontext.gregs;
    g_samples[i].rip = g[REG_RIP];
    g_samples[i].ret = *reinterpret_cast<const uint64_t*>(g[REG_RSP]);
    g_samples[i].tid = static_cast<uint32_t>(syscall(SYS_gettid));
    errno = saved;
}

std::string thread_name(uint32_t tid) {
    char path[64], name[64] = "?";
    snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
    if (FILE* f = fopen(path, "r")) {
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
    }
    return name;
}

// The game function containing `rva` (from .pdata), or 0.
uint32_t game_function(const PeImage& img, uint32_t rva) {
    struct Rf { uint32_t begin, end, unwind; };
    auto* rf = img.at<Rf>(img.exception_dir_rva);
    size_t n = img.exception_dir_size / sizeof(Rf);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rf[mid].end <= rva) lo = mid + 1; else hi = mid;
    }
    return lo < n && rf[lo].begin <= rva ? rf[lo].begin : 0;
}

void report(size_t count, double seconds) {
    const PeImage& img = main_image();
    uint64_t base = reinterpret_cast<uint64_t>(img.base);
    std::map<uint32_t, size_t> per_thread;
    std::map<std::string, size_t> per_place;   // "game rva 0x.." or "module+0xoffset"
    std::map<std::string, size_t> per_module;
    std::map<uint32_t, std::map<std::string, size_t>> per_thread_place, per_thread_module;
    for (size_t i = 0; i < count; i++) {
        const Sample& s = g_samples[i];
        per_thread[s.tid]++;
        auto describe = [&](uint64_t pc, std::string* module) {
            char key[160];
            if (pc >= base && pc < base + img.size) {
                uint32_t fn = game_function(img, static_cast<uint32_t>(pc - base));
                snprintf(key, sizeof key, "game fn 0x%x", fn ? fn : static_cast<uint32_t>(pc - base));
                *module = "game";
                return std::string(key);
            }
            Dl_info di{};
            if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_fname) {
                const char* m = strrchr(di.dli_fname, '/');
                m = m ? m + 1 : di.dli_fname;
                *module = m;
                if (di.dli_sname)
                    snprintf(key, sizeof key, "%s %s", m, di.dli_sname);
                else
                    snprintf(key, sizeof key, "%s+0x%llx", m, (unsigned long long)(pc - reinterpret_cast<uint64_t>(di.dli_fbase)));
            } else {
                snprintf(key, sizeof key, "? 0x%llx", (unsigned long long)pc);
                *module = "?";
            }
            return std::string(key);
        };
        std::string module, caller_module;
        std::string key = describe(s.rip, &module);
        // A sample in libc (syscall wrappers, locks): name who called it too.
        if (module == "libc.so.6") key += "  <- " + describe(s.ret, &caller_module);
        per_place[key]++;
        per_module[module]++;
        per_thread_place[s.tid][key]++;
        per_thread_module[s.tid][module]++;
    }
    log_write("cpuprof", "%zu samples in %.1f s (%.2f cores busy)", count, seconds, count / 1000.0 / seconds);
    std::vector<std::pair<size_t, uint32_t>> threads;
    for (auto& [t, n] : per_thread) threads.push_back({n, t});
    std::sort(threads.rbegin(), threads.rend());
    for (size_t i = 0; i < threads.size() && i < 16; i++)
        log_write("cpuprof", "  thread %u %-16s %5.1f%% (%.0f%% of a core)", threads[i].second,
                  thread_name(threads[i].second).c_str(), 100.0 * threads[i].first / count,
                  threads[i].first / 10.0 / seconds);
    // The hottest places of the three busiest threads.
    for (size_t t = 0; t < threads.size() && t < 3; t++) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto& [k, n] : per_thread_place[threads[t].second]) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        log_write("cpuprof", "  thread %u %s:", threads[t].second, thread_name(threads[t].second).c_str());
        std::string mods;
        for (auto& [m, n] : per_thread_module[threads[t].second])
            if (n * 100 >= threads[t].first) {
                char b[96];
                snprintf(b, sizeof b, " %s %.0f%%", m.c_str(), 100.0 * n / threads[t].first);
                mods += b;
            }
        log_write("cpuprof", "      modules:%s", mods.c_str());
        for (size_t i = 0; i < v.size() && i < 30; i++)
            log_write("cpuprof", "      %5.2f%% of it  %s", 100.0 * v[i].first / threads[t].first, v[i].second.c_str());
    }
    std::vector<std::pair<size_t, std::string>> mods;
    for (auto& [m, n] : per_module) mods.push_back({n, m});
    std::sort(mods.rbegin(), mods.rend());
    for (auto& [n, m] : mods)
        if (n * 200 >= count) log_write("cpuprof", "  module %-28s %5.1f%%", m.c_str(), 100.0 * n / count);
    std::vector<std::pair<size_t, std::string>> places;
    for (auto& [k, n] : per_place) places.push_back({n, k});
    std::sort(places.rbegin(), places.rend());
    for (size_t i = 0; i < places.size() && i < 60; i++)
        log_write("cpuprof", "  %5.2f%%  %s", 100.0 * places[i].first / count, places[i].second.c_str());
}

}  // namespace

void start_cpu_profiler() {
    const char* e = getenv("FL_CPU_PROFILE");
    if (!e) return;
    double start = 0, length = 10;
    sscanf(e, "%lf:%lf", &start, &length);
    g_samples = new Sample[kMax];
    struct sigaction sa{};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    std::thread([start, length] {
        while (run_time() < start) usleep(100000);
        itimerval it{{0, 1000}, {0, 1000}};
        g_on = true;
        setitimer(ITIMER_PROF, &it, nullptr);
        double t0 = run_time();
        while (run_time() < t0 + length) usleep(100000);
        itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        g_on = false;
        size_t n = std::min(g_count.load(), kMax);
        report(n, run_time() - t0);
    }).detach();
    log_write("cpuprof", "CPU profile from %.0f s for %.0f s", start, length);
}

}  // namespace fl::loader
