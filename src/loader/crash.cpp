#include "loader/crash.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include <atomic>
#include <algorithm>
#include <thread>

#include <csignal>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

#include "common/log.h"
#include "loader/modules.h"
#include "loader/pe_image.h"
#include "hle/seh/seh.h"
#include "loader/thread_env.h"
#include "platform/os/memory.h"
#include "platform/os/thread.h"
#include "platform/os/signal_context.h"

namespace fl::loader {
namespace {

struct DataTrap {
    uint8_t* page;
    std::string dll, name;
};
std::vector<DataTrap> g_data_traps;
std::mutex g_mutex;
bool (*g_fault_hook)(uint64_t, bool) = nullptr;

void describe(char* out, size_t n, uint64_t addr) {
    PeImage* img = image_for(reinterpret_cast<void*>(addr));
    if (img == &main_image())
        snprintf(out, n, "0x%016llx (game rva 0x%llx)", (unsigned long long)addr,
                 (unsigned long long)(addr - reinterpret_cast<uint64_t>(img->base)));
    else if (img) {
        const char* name = strrchr(img->path.c_str(), '/');
        snprintf(out, n, "0x%016llx (%s rva 0x%llx)", (unsigned long long)addr, name ? name + 1 : img->path.c_str(),
                 (unsigned long long)(addr - reinterpret_cast<uint64_t>(img->base)));
    } else {
        // Native code: name the shared object (and symbol, when exported).
        Dl_info di{};
        if (dladdr(reinterpret_cast<void*>(addr), &di) && di.dli_fname) {
            const char* so = strrchr(di.dli_fname, '/');
            if (di.dli_sname)
                snprintf(out, n, "0x%016llx (%s: %s+0x%llx)", (unsigned long long)addr, so ? so + 1 : di.dli_fname,
                         di.dli_sname, (unsigned long long)(addr - reinterpret_cast<uint64_t>(di.dli_saddr)));
            else
                snprintf(out, n, "0x%016llx (%s+0x%llx)", (unsigned long long)addr, so ? so + 1 : di.dli_fname,
                         (unsigned long long)(addr - reinterpret_cast<uint64_t>(di.dli_fbase)));
        } else {
            snprintf(out, n, "0x%016llx", (unsigned long long)addr);
        }
    }
}

void dump_threads();

// FL_CRASH_PEEK=[@]<rva>:<off>[,...]: dump 32 bytes at *(image + rva) + off
// (a game global's object field; '@' reads image + rva itself) in crash
// reports and at FL_DUMP_THREADS_AT times. Diagnostic.
void peek_globals() {
    PeImage& img = main_image();
    const char* peek = getenv("FL_CRASH_PEEK");
    if (!peek || !img.base) return;
    for (const char* p = peek; *p;) {
        char* e;
        bool direct = *p == '@';  // @rva:off reads the image itself, no pointer
        if (direct) p++;
        uint64_t rva = strtoull(p, &e, 16);
        uint64_t off = *e == ':' ? strtoull(e + 1, &e, 16) : 0;
        auto* obj = direct ? img.base + rva : *reinterpret_cast<uint8_t**>(img.base + rva);
        if (obj) {
            const uint8_t* q = obj + off;
            fl::log_write("FATAL", "peek *(rva 0x%llx)=%p +0x%llx: %02x %02x %02x %02x %02x %02x %02x %02x | %016llx %016llx %016llx",
                          (unsigned long long)rva, obj, (unsigned long long)off, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
                          (unsigned long long)reinterpret_cast<const uint64_t*>(q)[1], (unsigned long long)reinterpret_cast<const uint64_t*>(q)[2],
                          (unsigned long long)reinterpret_cast<const uint64_t*>(q)[3]);
        }
        p = *e == ',' ? e + 1 : e;
        if (p == e && *e) break;
    }
}

// Present in the AddressSanitizer build (build-asan): says which allocation
// the fault address is in or next to, and who allocated it.
extern "C" void __asan_describe_address(void*) __attribute__((weak));

// FL_WATCH=<function rva>:<offset>: when that game function is entered, watch
// writes to (rcx + offset) (rcx = `this`): the page is made read-only, writes
// to the watched byte are logged with the writer (channel "watch"), every write
// to the page is single-stepped and the page re-protected. Diagnostic.
std::atomic<uint64_t> g_watch_addr{0};
uint64_t g_watch_off = 0, g_watch_fn = 0;
thread_local bool t_watch_step = false;

bool watch_fault(uint64_t fault, os::Regs g) {
    uint64_t w = g_watch_addr.load();
    const uint64_t pm = os::page_size() - 1;
    if (!w || (fault & ~pm) != (w & ~pm)) return false;
    if (fault <= w && w < fault + 16) {
        uint64_t base = reinterpret_cast<uint64_t>(main_image().base);
        uint64_t rip = g[os::kRip];
        bool game = main_image().contains(reinterpret_cast<void*>(rip));
        fl::log_write("watch", "write to watched %llx (fault %llx) at %s rva 0x%llx, [rsp] rva 0x%llx",
                      (unsigned long long)w, (unsigned long long)fault, game ? "game" : "native",
                      (unsigned long long)(rip - (game ? base : 0)),
                      (unsigned long long)(*reinterpret_cast<uint64_t*>(g[os::kRsp]) - base));
    }
    os::protect(reinterpret_cast<void*>(w & ~uint64_t(os::page_size() - 1)), os::page_size(), os::kRead | os::kWrite);
    g[os::kRflags] |= 0x100;  // single-step the write, then re-protect (trap handler)
    t_watch_step = true;
    return true;
}

void handler(int sig, siginfo_t* si, void* uc_) {
    auto* uc = static_cast<ucontext_t*>(uc_);
    os::Regs g(uc);
    uint64_t fault = reinterpret_cast<uint64_t>(si->si_addr);

    // Write-tracked GPU memory: the hook unprotects the page and we retry.
    // os::kErr bit 1 = the access was a write (x86 page-fault error code).
    if (sig == SIGSEGV && watch_fault(fault, g)) return;
    if (sig == SIGSEGV && g_fault_hook && g_fault_hook(fault, (g[os::kErr] & 2) != 0)) return;

    for (auto& t : g_data_traps) {
        if (fault >= reinterpret_cast<uint64_t>(t.page) && fault < reinterpret_cast<uint64_t>(t.page) + os::page_size()) {
            char rip[96];
            describe(rip, sizeof rip, g[os::kRip]);
            fl::log_write("FATAL", "UNIMPLEMENTED data import %s!%s (touched at rip %s)", t.dll.c_str(),
                          t.name.c_str(), rip);
            _exit(134);
        }
    }

    char rip[96], fa[96];
    describe(rip, sizeof rip, g[os::kRip]);
    describe(fa, sizeof fa, fault);
    if (sig == SIGSEGV) {
        // The mapping the fault address is in (not async-signal-safe; we are dying).
        if (FILE* f = fopen("/proc/self/maps", "r")) {
            char line[256];
            bool found = false;
            while (fgets(line, sizeof line, f)) {
                unsigned long long lo, hi;
                if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && fault >= lo && fault < hi) {
                    line[strcspn(line, "\n")] = 0;
                    fl::log_write("FATAL", "fault address mapping: %s", line);
                    found = true;
                    break;
                }
            }
            if (!found) fl::log_write("FATAL", "fault address is not mapped");
            fclose(f);
        }
    }
    fl::log_write("FATAL", "signal %d (%s) at rip %s, fault address %s", sig, strsignal(sig), rip, fa);
    fl::log_write("FATAL", "rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", (unsigned long long)g[os::kRax],
                  (unsigned long long)g[os::kRbx], (unsigned long long)g[os::kRcx], (unsigned long long)g[os::kRdx]);
    fl::log_write("FATAL", "rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", (unsigned long long)g[os::kRsi],
                  (unsigned long long)g[os::kRdi], (unsigned long long)g[os::kRbp], (unsigned long long)g[os::kRsp]);
    fl::log_write("FATAL", "r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", (unsigned long long)g[os::kR8],
                  (unsigned long long)g[os::kR9], (unsigned long long)g[os::kR10], (unsigned long long)g[os::kR11]);
    fl::log_write("FATAL", "r12=%016llx r13=%016llx r14=%016llx r15=%016llx", (unsigned long long)g[os::kR12],
                  (unsigned long long)g[os::kR13], (unsigned long long)g[os::kR14], (unsigned long long)g[os::kR15]);

    PeImage& img = main_image();
    if (img.base && img.contains(reinterpret_cast<void*>(g[os::kRip]))) {
        // Real backtrace through the game's .pdata unwind info.
        static seh::Context c;  // big; we're on the alt stack
        memset(&c, 0, sizeof c);
        const int map[16] = {os::kRax, os::kRcx, os::kRdx, os::kRbx, os::kRsp, os::kRbp, os::kRsi, os::kRdi,
                             os::kR8,  os::kR9,  os::kR10, os::kR11, os::kR12, os::kR13, os::kR14, os::kR15};
        for (int i = 0; i < 16; i++) c.Gpr[i] = g[map[i]];
        c.Rip = g[os::kRip];
        uint64_t pcs[32];
        int n = seh::walk_stack(c, pcs, 32);
        for (int i = 0; i < n; i++)
            fl::log_write("FATAL", "  #%-2d game rva 0x%llx", i,
                          (unsigned long long)(pcs[i] - reinterpret_cast<uint64_t>(img.base)));
    } else {
        // Crash outside the game: scan the stack for return addresses into
        // the game and into native code (a heuristic: not every hit is a frame).
        auto* sp = reinterpret_cast<uint64_t*>(g[os::kRsp]);
        int found = 0, native = 0;
        for (int i = 0; i < 1024 && found < 16; i++) {
            uint64_t v = sp[i];
            if (img.base && img.contains(reinterpret_cast<void*>(v))) {
                fl::log_write("FATAL", "  stack[%3d] game rva 0x%llx", i,
                              (unsigned long long)(v - reinterpret_cast<uint64_t>(img.base)));
                found++;
                continue;
            }
            Dl_info di{};
            if (native < 16 && v > 0x10000 && dladdr(reinterpret_cast<void*>(v), &di) && di.dli_fname &&
                v - reinterpret_cast<uint64_t>(di.dli_fbase) < (1ull << 28)) {
                char d[160];
                describe(d, sizeof d, v);
                fl::log_write("FATAL", "  stack[%3d] native %s", i, d);
                native++;
            }
        }
    }
    if (&__asan_describe_address && sig == SIGSEGV) {
        fl::log_write("FATAL", "AddressSanitizer on the fault address:");
        fflush(stderr);
        __asan_describe_address(reinterpret_cast<void*>(fault));
    }
    peek_globals();
    // FL_CRASH_DUMP_THREADS=1: what every other thread was doing.
    if (getenv("FL_CRASH_DUMP_THREADS")) dump_threads();
    signal(sig, SIG_DFL);
    raise(sig);
}

// ---------------------------------------------------------------- thread dump
// FL_DUMP_THREADS_AT=90[,150]: at those seconds, every thread logs where it
// is (channel "threads"): the native symbol it's in and the game frames
// (walked when it's in game code, else found by scanning its stack). For
// diagnosing hangs. The handler only fills a buffer; the watchdog prints it.

const int kDumpSignal = SIGRTMIN + 4;
std::atomic<int> g_dump_done{0};
char g_dump_buf[4096];

void dump_append(size_t& n, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void dump_append(size_t& n, const char* fmt, ...) {
    if (n >= sizeof g_dump_buf) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(g_dump_buf + n, sizeof g_dump_buf - n, fmt, ap);
    va_end(ap);
    if (w > 0) n += size_t(w);
}

void dump_handler(int, siginfo_t*, void* uc_) {
    os::Regs g(uc_);
    size_t n = 0;
    char d[160];
    describe(d, sizeof d, g[os::kRip]);
    dump_append(n, "at %s\n", d);
    PeImage& img = main_image();
    uint64_t base = reinterpret_cast<uint64_t>(img.base);
    if (img.base && img.contains(reinterpret_cast<void*>(g[os::kRip]))) {
        static seh::Context c;
        memset(&c, 0, sizeof c);
        const int map[16] = {os::kRax, os::kRcx, os::kRdx, os::kRbx, os::kRsp, os::kRbp, os::kRsi, os::kRdi,
                             os::kR8,  os::kR9,  os::kR10, os::kR11, os::kR12, os::kR13, os::kR14, os::kR15};
        for (int i = 0; i < 16; i++) c.Gpr[i] = g[map[i]];
        c.Rip = g[os::kRip];
        uint64_t pcs[16];
        int k = seh::walk_stack(c, pcs, 16);
        for (int i = 0; i < k; i++) dump_append(n, "    #%d game rva 0x%llx\n", i, (unsigned long long)(pcs[i] - base));
    } else {
        // Copy the stack with a checked read: it stops at the end of the
        // mapping instead of faulting (thread stacks can be small).
        static uint64_t sp[2048];
        size_t words = 0;
        for (size_t off = 0; off < sizeof sp; off += 4096) {
            size_t r = os::read_checked(reinterpret_cast<char*>(sp) + off, reinterpret_cast<void*>(g[os::kRsp] + off), 4096);
            words += r / 8;
            if (r != 4096) break;
        }
        int game = 0, native = 0;
        for (size_t i = 0; i < words && game < 10; i++) {
            uint64_t v = sp[i];
            if (img.base && img.contains(reinterpret_cast<void*>(v))) {
                dump_append(n, "    stack[%zu] game rva 0x%llx\n", i, (unsigned long long)(v - base));
                game++;
            } else if (native < 6 && game == 0 && v > 0x10000) {
                Dl_info di{};
                if (dladdr(reinterpret_cast<void*>(v), &di) && di.dli_fname && di.dli_sname &&
                    v - reinterpret_cast<uint64_t>(di.dli_fbase) < (1ull << 28)) {
                    describe(d, sizeof d, v);
                    dump_append(n, "    stack[%zu] native %s\n", i, d);
                    native++;
                }
            }
        }
    }
    g_dump_done.store(1, std::memory_order_release);
}

void dump_threads() {
    pid_t self = static_cast<pid_t>(os::thread_id());
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return;
    fl::log_write("threads", "==== thread dump");
    peek_globals();
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        pid_t tid = atoi(e->d_name);
        if (tid == self) continue;
        char comm[64] = "?";
        if (FILE* f = fopen(("/proc/self/task/" + std::string(e->d_name) + "/comm").c_str(), "r")) {
            if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
            fclose(f);
        }
        // CPU time so far (utime + stime, fields 14-15 of stat): two dumps a
        // few seconds apart show which threads burn CPU.
        double cpu = 0;
        if (FILE* f = fopen(("/proc/self/task/" + std::string(e->d_name) + "/stat").c_str(), "r")) {
            char line[1024];
            if (fgets(line, sizeof line, f))
                if (const char* p = strrchr(line, ')')) {
                    unsigned long long ut = 0, st = 0;
                    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
                        cpu = double(ut + st) / sysconf(_SC_CLK_TCK);
                }
            fclose(f);
        }
        g_dump_done = 0;
        g_dump_buf[0] = 0;
        os::signal_thread(tid, kDumpSignal);
        for (int i = 0; i < 200 && !g_dump_done.load(std::memory_order_acquire); i++) usleep(1000);
        fl::log_write("threads", "thread %d (%s) cpu %.2fs %s", tid, comm, cpu,
                      g_dump_done ? g_dump_buf : "did not answer\n");
    }
    closedir(dir);
}

std::atomic<int64_t> g_last_present_ns{0};

int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

void start_watchdog() {
    const char* e = getenv("FL_WATCHDOG");
    if (e && !strcmp(e, "0")) return;
    std::thread([] {
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int64_t last = g_last_present_ns.load(std::memory_order_relaxed);
            if (!last) continue;  // not presenting yet
            int64_t idle = mono_ns() - last;
            if (idle < 15'000'000'000LL) {
                reported = false;
                continue;
            }
            if (reported) continue;
            reported = true;
            FL_ERROR("watchdog: no Present for %lld s: the game looks frozen; thread dump follows",
                     static_cast<long long>(idle / 1'000'000'000LL));
            dump_threads();
        }
    }).detach();
}

void start_thread_dumps() {
    start_watchdog();
    struct sigaction sa = {};
    sa.sa_sigaction = dump_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(kDumpSignal, &sa, nullptr);
    const char* env = getenv("FL_DUMP_THREADS_AT");
    if (!env) return;
    std::vector<double> times;
    for (const char* p = env; *p;) {
        times.push_back(strtod(p, const_cast<char**>(&p)));
        while (*p == ',') p++;
    }
    std::thread([times] {
        auto start = std::chrono::steady_clock::now();
        for (double t : times) {
            std::this_thread::sleep_until(start + std::chrono::milliseconds(int64_t(t * 1000)));
            dump_threads();
        }
    }).detach();
}

}  // namespace

void note_present() { g_last_present_ns.store(mono_ns(), std::memory_order_relaxed); }

// FL_BREAK=<rva>[,<rva>...]: one-shot breakpoints in game code. Each logs its
// first hit (channel "break": thread and caller), then the original byte is
// put back and execution continues. Diagnostic: shows whether code runs.
// FL_BREAK_ALL=<seconds>: one on every function in .pdata; hits before that
// time are silent, later first hits are logged (differential coverage).
struct Breakpoint {
    uint64_t addr;
    uint8_t orig;
};
static std::vector<Breakpoint> g_breaks;  // sorted by addr, fixed before the game runs
static std::vector<std::atomic<bool>>* g_break_hit = nullptr;
static double g_break_after = 0;
static std::chrono::steady_clock::time_point g_break_t0;

static void trap_handler(int, siginfo_t* si, void* uc_) {
    os::Regs g(uc_);
    static thread_local uint64_t rearm_at = 0;
    if (rearm_at && si->si_code == TRAP_TRACE) {
        *reinterpret_cast<volatile uint8_t*>(rearm_at) = 0xCC;
        rearm_at = 0;
        g[os::kRflags] &= ~0x100;
        return;
    }
    if (t_watch_step && si->si_code == TRAP_TRACE) {
        t_watch_step = false;
        g[os::kRflags] &= ~0x100;
        if (uint64_t w = g_watch_addr.load())
            os::protect(reinterpret_cast<void*>(w & ~uint64_t(os::page_size() - 1)), os::page_size(), os::kRead);
        return;
    }
    uint64_t at = g[os::kRip] - 1;
    if (g_watch_fn && at == g_watch_fn && !g_watch_addr.load()) {
        uint64_t w = g[os::kRcx] + g_watch_off;
        g_watch_addr = w;
        fl::log_write("watch", "watching %llx (this %llx + 0x%llx)", (unsigned long long)w, (unsigned long long)g[os::kRcx],
                      (unsigned long long)g_watch_off);
        // Protect after the constructor has run: arm on the first write fault instead.
        os::protect(reinterpret_cast<void*>(w & ~uint64_t(os::page_size() - 1)), os::page_size(), os::kRead);
    }
    size_t lo = 0, hi = g_breaks.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_breaks[mid].addr < at) lo = mid + 1; else hi = mid;
    }
    static thread_local uint64_t last_hit = 0, last_rsp = 0, last_ret = 0;
    if (lo < g_breaks.size() && g_breaks[lo].addr == at) {
        Breakpoint& b = g_breaks[lo];
        // FL_BREAK_RETURN=<rva>=<value>[,...] (debug experiments only): that function
        // returns <value> at once instead of running (its rva must also be in FL_BREAK).
        static const std::vector<std::pair<uint64_t, uint64_t>> forced = [] {
            std::vector<std::pair<uint64_t, uint64_t>> v;
            if (const char* e = getenv("FL_BREAK_RETURN"))
                for (const char* p = e; *p;) {
                    uint64_t rva = strtoull(p, const_cast<char**>(&p), 16);
                    uint64_t val = *p == '=' ? strtoull(p + 1, const_cast<char**>(&p), 0) : 0;
                    v.push_back({rva, val});
                    if (*p == ',') p++;
                    else break;
                }
            return v;
        }();
        for (auto& [rva, val] : forced)
            if (at - reinterpret_cast<uint64_t>(main_image().base) == rva) {
                static std::atomic<int> logged{0};
                if (logged++ < 3) fl::log_write("break", "forced return 0x%llx from game rva 0x%llx", (unsigned long long)val,
                                                (unsigned long long)rva);
                g[os::kRax] = val;
                g[os::kRip] = *reinterpret_cast<uint64_t*>(g[os::kRsp]);
                g[os::kRsp] += 8;
                return;  // the int3 stays armed
            }
        // FL_BREAK_REPEAT=<n>: log up to n hits per breakpoint (re-armed after
        // single-stepping the original instruction).
        static const int repeat = getenv("FL_BREAK_REPEAT") ? atoi(getenv("FL_BREAK_REPEAT")) : 0;
        if (repeat) {
            static std::atomic<int> counts[512];
            // FL_BREAK_AFTER=<seconds>: repeat-mode hits before then are not counted or logged.
            static const double after = getenv("FL_BREAK_AFTER") ? atof(getenv("FL_BREAK_AFTER")) : 0;
            bool early = after > 0 &&
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - g_break_t0).count() < after;
            // FL_BREAK_RET=<rva>: only hits whose return address is that rva count.
            static const uint64_t want_ret = getenv("FL_BREAK_RET") ? strtoull(getenv("FL_BREAK_RET"), nullptr, 16) : 0;
            if (want_ret && *reinterpret_cast<uint64_t*>(g[os::kRsp]) - reinterpret_cast<uint64_t>(main_image().base) != want_ret)
                early = true;
            int c = early ? repeat : lo < 512 ? counts[lo]++ : repeat;
            if (early) {  // re-arm without logging
                *reinterpret_cast<volatile uint8_t*>(at) = b.orig;
                g[os::kRip] = at;
                rearm_at = at;
                g[os::kRflags] |= 0x100;
                return;
            }
            // FL_BREAK_RBXSTR=<off>: a C string at rbx + off.
            if (c < repeat && getenv("FL_BREAK_RBXSTR") && g[os::kRbx] > 0x10000)
                fl::log_write("break", "  rbx+%s cstr '%.200s'", getenv("FL_BREAK_RBXSTR"),
                              reinterpret_cast<const char*>(g[os::kRbx] + strtoull(getenv("FL_BREAK_RBXSTR"), nullptr, 16)));
            // FL_BREAK_RCXSTR=<off>: a C string at rcx + off.
            if (c < repeat && getenv("FL_BREAK_RCXSTR") && g[os::kRcx] > 0x10000)
                fl::log_write("break", "  rcx+%s cstr '%.200s'", getenv("FL_BREAK_RCXSTR"),
                              reinterpret_cast<const char*>(g[os::kRcx] + strtoull(getenv("FL_BREAK_RCXSTR"), nullptr, 16)));
            if (c < repeat && getenv("FL_BREAK_CSTR") && g[os::kRcx] > 0x10000)
                fl::log_write("break", "  rcx cstr '%.200s'", reinterpret_cast<const char*>(g[os::kRcx]));
            // FL_BREAK_XMM=1: the low floats of xmm0-xmm3 (float arguments).
            if (c < repeat && getenv("FL_BREAK_XMM")) {
                const uint8_t* fx = os::fxsave_area(uc_);
                float f[4];
                for (int i = 0; i < 4; i++) memcpy(&f[i], fx + 160 + 16 * i, 4);
                fl::log_write("break", "  xmm0-3 %g %g %g %g", f[0], f[1], f[2], f[3]);
            }
            // FL_BREAK_DEREF=<off> in repeat mode: the value at [rcx+off] (as a game rva if in the image).
            if (c < repeat && getenv("FL_BREAK_DEREF") && g[os::kRcx] > 0x10000) {
                uint64_t v = *reinterpret_cast<uint64_t*>(g[os::kRcx] + strtoull(getenv("FL_BREAK_DEREF"), nullptr, 16));
                uint64_t base = reinterpret_cast<uint64_t>(main_image().base);
                fl::log_write("break", "  [rcx+%s] = %llx (rva 0x%llx)", getenv("FL_BREAK_DEREF"), (unsigned long long)v,
                              (unsigned long long)(v - base));
            }
            // FL_BREAK_DUMP=<off>:<len>[,<off>:<len>...] (hex): bytes at rcx+off; an offset
            // written as *<off1>+<off2> follows the pointer at rcx+off1 first.
            if (c < repeat && getenv("FL_BREAK_DUMP")) {
                for (const char* p = getenv("FL_BREAK_DUMP"); *p;) {
                    uint64_t addr = g[os::kRcx];
                    // optional register prefix: rax@ rbx@ rdx@ rdi@ rsi@ r8@ (default rcx)
                    static const struct { const char* name; int reg; } kRegs[] = {
                        {"rax@", os::kRax}, {"rbx@", os::kRbx}, {"rdx@", os::kRdx}, {"rdi@", os::kRdi},
                        {"rsi@", os::kRsi}, {"r8@", os::kR8}, {"rcx@", os::kRcx}};
                    for (auto& r : kRegs)
                        if (!strncmp(p, r.name, strlen(r.name))) {
                            addr = g[r.reg];
                            p += strlen(r.name);
                            break;
                        }
                    bool indirect = *p == '*';
                    if (indirect) p++;
                    uint64_t off = strtoull(p, const_cast<char**>(&p), 16);
                    if (indirect) {
                        addr = *reinterpret_cast<uint64_t*>(addr + off);
                        off = *p == '+' ? strtoull(p + 1, const_cast<char**>(&p), 16) : 0;
                    }
                    uint64_t len = *p == ':' ? strtoull(p + 1, const_cast<char**>(&p), 16) : 8;
                    char hex[3 * 64 + 1] = "";
                    if (addr > 0x10000)
                        for (uint64_t i = 0; i < len && i < 64; i++)
                            snprintf(hex + 3 * i, 4, "%02x ", reinterpret_cast<const uint8_t*>(addr + off)[i]);
                    fl::log_write("break", "  dump %llx+%llx: %s", (unsigned long long)addr, (unsigned long long)off, hex);
                    if (*p == ',') p++;
                    else break;
                }
            }
            // FL_BREAK_PTRVEC=<off>:<stride> (hex): for the vector [rcx+off, rcx+off+8)
            // of objects (or shared_ptrs, stride 0x10), the vtable rva of each element.
            if (c < repeat && getenv("FL_BREAK_PTRVEC") && g[os::kRcx] > 0x10000) {
                const char* p = getenv("FL_BREAK_PTRVEC");
                uint64_t off = strtoull(p, const_cast<char**>(&p), 16);
                uint64_t stride = *p == ':' ? strtoull(p + 1, nullptr, 16) : 8;
                auto* v = reinterpret_cast<uint64_t*>(g[os::kRcx] + off);
                uint64_t base = reinterpret_cast<uint64_t>(main_image().base);
                for (uint64_t e = v[0], i = 0; e < v[1] && i < 32; e += stride, i++) {
                    uint64_t obj = *reinterpret_cast<uint64_t*>(e);
                    uint64_t vt = obj > 0x10000 ? *reinterpret_cast<uint64_t*>(obj) : 0;
                    fl::log_write("break", "  [%llu] %llx vtable rva 0x%llx", (unsigned long long)i, (unsigned long long)obj,
                                  (unsigned long long)(vt ? vt - base : 0));
                }
            }
            // FL_BREAK_RDXF=1: the 4 floats at rdx (a float4 passed by pointer).
            if (c < repeat && getenv("FL_BREAK_RDXF") && g[os::kRdx] > 0x10000) {
                auto* f = reinterpret_cast<const float*>(g[os::kRdx]);
                fl::log_write("break", "  [rdx] floats %g %g %g %g", f[0], f[1], f[2], f[3]);
            }
            if (c < repeat && getenv("FL_BREAK_STR") && g[os::kRdx] > 0x10000) {
                // rdx as an MSVC std::string: {buf[16] or ptr, size, capacity}
                auto* sp = reinterpret_cast<const uint64_t*>(g[os::kRdx]);
                const char* str = sp[3] > 15 ? reinterpret_cast<const char*>(sp[0]) : reinterpret_cast<const char*>(sp);
                if (sp[2] < 512) fl::log_write("break", "  rdx string '%.*s'", int(sp[2]), str);
            }
            if (c < repeat) {
                uint64_t base = reinterpret_cast<uint64_t>(main_image().base);
                fl::log_write("break", "hit game rva 0x%llx (return to rva 0x%llx) rcx %llx rdx %llx rax %llx", (unsigned long long)(at - base),
                              (unsigned long long)(*reinterpret_cast<uint64_t*>(g[os::kRsp]) - base),
                              (unsigned long long)g[os::kRcx], (unsigned long long)g[os::kRdx], (unsigned long long)g[os::kRax]);
            }
            *reinterpret_cast<volatile uint8_t*>(at) = b.orig;
            g[os::kRip] = at;
            if (c < repeat) {
                rearm_at = at;
                g[os::kRflags] |= 0x100;
            }
            return;
        }
        last_hit = at;
        last_rsp = g[os::kRsp];
        last_ret = *reinterpret_cast<uint64_t*>(g[os::kRsp]);
        *reinterpret_cast<volatile uint8_t*>(at) = b.orig;
        g[os::kRip] = at;
        if (!(*g_break_hit)[lo].exchange(true)) {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_break_t0).count();
            if (t >= g_break_after) {
                uint64_t base = reinterpret_cast<uint64_t>(main_image().base);
                uint64_t ret = *reinterpret_cast<uint64_t*>(g[os::kRsp]);
                fl::log_write("break", "hit game rva 0x%llx (return to rva 0x%llx) rcx %llx rdx %llx", (unsigned long long)(at - base),
                              (unsigned long long)(ret - base), (unsigned long long)g[os::kRcx], (unsigned long long)g[os::kRdx]);
                // FL_BREAK_DEREF=<off>: also the object at [rcx+off] and its vtable rva.
                static const char* deref = getenv("FL_BREAK_DEREF");
                if (deref && g[os::kRcx] > 0x10000) {
                    uint64_t obj = *reinterpret_cast<uint64_t*>(g[os::kRcx] + strtoull(deref, nullptr, 16));
                    uint64_t vt = obj > 0x10000 ? *reinterpret_cast<uint64_t*>(obj) : 0;
                    fl::log_write("break", "  [rcx+%s] = %llx vtable rva 0x%llx", deref, (unsigned long long)obj,
                                  (unsigned long long)(vt ? vt - base : 0));
                }
            }
        }
        return;
    }
    char d[160];
    describe(d, sizeof d, at);
    fl::log_write("FATAL", "SIGTRAP at %s that is not one of our breakpoints (si_code %d, byte 0x%02x, pid %d)", d,
                  si->si_code, *reinterpret_cast<uint8_t*>(at), si->si_pid);
    {
        uint64_t base = reinterpret_cast<uint64_t>(main_image().base);
        auto* sp = reinterpret_cast<uint64_t*>(g[os::kRsp]);
        fl::log_write("FATAL", "  [rsp-8] %llx [rsp] %llx [rsp+8] %llx; rax %llx rcx %llx", (unsigned long long)sp[-1],
                      (unsigned long long)sp[0], (unsigned long long)sp[1], (unsigned long long)g[os::kRax],
                      (unsigned long long)g[os::kRcx]);
        fl::log_write("FATAL", "  this thread's last breakpoint: rva 0x%llx (rsp %llx ret rva 0x%llx); now rsp %llx",
                      (unsigned long long)(last_hit - base), (unsigned long long)last_rsp,
                      (unsigned long long)(last_ret - base), (unsigned long long)g[os::kRsp]);
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}

void arm_breakpoints() {
    const char* env = getenv("FL_BREAK");
    const char* all = getenv("FL_BREAK_ALL");
    const char* watch = getenv("FL_WATCH");
    if (!env && !all && !watch) return;
    PeImage& img = main_image();
    uint64_t base = reinterpret_cast<uint64_t>(img.base);
    std::vector<uint64_t> rvas;
    if (env)
        for (const char* p = env; *p;) {
            char* e;
            uint64_t rva = strtoull(p, &e, 16);
            if (e == p) break;
            rvas.push_back(rva);
            p = *e == ',' ? e + 1 : e;
        }
    if (watch) {
        char* e;
        uint64_t fn = strtoull(watch, &e, 16);
        g_watch_off = *e == ':' ? strtoull(e + 1, nullptr, 16) : 0;
        g_watch_fn = reinterpret_cast<uint64_t>(main_image().base) + fn;
        rvas.push_back(fn);
    }
    if (all) {
        g_break_after = atof(all);
        // Primary entries only: a chained entry (UNW_FLAG_CHAININFO) continues
        // the previous function and can start on inline data such as a
        // switch's jump table.
        auto* rf = reinterpret_cast<const uint32_t*>(img.base + img.exception_dir_rva);
        for (uint32_t i = 0; i < img.exception_dir_size / 12; i++) {
            uint8_t flags = img.base[rf[i * 3 + 2]] >> 3;
            if (!(flags & 4)) rvas.push_back(rf[i * 3]);
        }
    }
    std::sort(rvas.begin(), rvas.end());
    rvas.erase(std::unique(rvas.begin(), rvas.end()), rvas.end());
    // Make the code writable once (the loader maps it RX).
    uint64_t text_lo = base + rvas.front(), text_hi = base + rvas.back() + 1;
    const uint64_t pm = os::page_size() - 1;
    os::protect(reinterpret_cast<void*>(text_lo & ~pm), ((text_hi + pm) & ~pm) - (text_lo & ~pm), os::kRead | os::kWrite | os::kExec);
    g_breaks.reserve(rvas.size());
    for (uint64_t rva : rvas) {
        uint64_t a = base + rva;
        g_breaks.push_back({a, *reinterpret_cast<uint8_t*>(a)});
        *reinterpret_cast<uint8_t*>(a) = 0xCC;
    }
    if (const char* chk = getenv("FL_BREAK_CHECK")) {
        uint64_t a = base + strtoull(chk, nullptr, 16);
        fl::log_write("break", "check %s: byte 0x%02x after arming", chk, *reinterpret_cast<uint8_t*>(a));
    }
    g_break_hit = new std::vector<std::atomic<bool>>(g_breaks.size());
    g_break_t0 = std::chrono::steady_clock::now();
    struct sigaction sa = {};
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, nullptr);
    fl::log_write("break", "armed %zu breakpoints", g_breaks.size());
}

void install_crash_handler() {
    start_thread_dumps();
    // Alternate stack so stack overflows can still be reported.
    static std::vector<uint8_t> altstack(1 << 16);
    stack_t ss = {};
    ss.ss_sp = altstack.data();
    ss.ss_size = altstack.size();
    sigaltstack(&ss, nullptr);

    struct sigaction sa = {};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

void set_fault_hook(bool (*hook)(uint64_t addr, bool write)) { g_fault_hook = hook; }

void* make_data_trap(const std::string& dll, const std::string& name) {
    std::lock_guard lock(g_mutex);
    void* p = os::map_anon(nullptr, os::page_size(), os::kNone);
    if (!p) fatal("mmap data trap failed");
    g_data_traps.push_back({static_cast<uint8_t*>(p), dll, name});
    return p;
}

}  // namespace fl::loader
