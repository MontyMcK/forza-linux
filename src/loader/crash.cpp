#include "loader/crash.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

#include "common/log.h"
#include "loader/modules.h"
#include "loader/pe_image.h"
#include "hle/seh/seh.h"
#include "loader/thread_env.h"

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
    } else
        snprintf(out, n, "0x%016llx", (unsigned long long)addr);
}

void handler(int sig, siginfo_t* si, void* uc_) {
    auto* uc = static_cast<ucontext_t*>(uc_);
    auto* g = uc->uc_mcontext.gregs;
    uint64_t fault = reinterpret_cast<uint64_t>(si->si_addr);

    // Write-tracked GPU memory: the hook unprotects the page and we retry.
    // REG_ERR bit 1 = the access was a write (x86 page-fault error code).
    if (sig == SIGSEGV && g_fault_hook && g_fault_hook(fault, (g[REG_ERR] & 2) != 0)) return;

    for (auto& t : g_data_traps) {
        if (fault >= reinterpret_cast<uint64_t>(t.page) && fault < reinterpret_cast<uint64_t>(t.page) + 4096) {
            char rip[96];
            describe(rip, sizeof rip, g[REG_RIP]);
            fl::log_write("FATAL", "UNIMPLEMENTED data import %s!%s (touched at rip %s)", t.dll.c_str(),
                          t.name.c_str(), rip);
            _exit(134);
        }
    }

    char rip[96], fa[96];
    describe(rip, sizeof rip, g[REG_RIP]);
    describe(fa, sizeof fa, fault);
    fl::log_write("FATAL", "signal %d (%s) at rip %s, fault address %s", sig, strsignal(sig), rip, fa);
    fl::log_write("FATAL", "rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", (unsigned long long)g[REG_RAX],
                  (unsigned long long)g[REG_RBX], (unsigned long long)g[REG_RCX], (unsigned long long)g[REG_RDX]);
    fl::log_write("FATAL", "rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", (unsigned long long)g[REG_RSI],
                  (unsigned long long)g[REG_RDI], (unsigned long long)g[REG_RBP], (unsigned long long)g[REG_RSP]);
    fl::log_write("FATAL", "r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", (unsigned long long)g[REG_R8],
                  (unsigned long long)g[REG_R9], (unsigned long long)g[REG_R10], (unsigned long long)g[REG_R11]);
    fl::log_write("FATAL", "r12=%016llx r13=%016llx r14=%016llx r15=%016llx", (unsigned long long)g[REG_R12],
                  (unsigned long long)g[REG_R13], (unsigned long long)g[REG_R14], (unsigned long long)g[REG_R15]);

    PeImage& img = main_image();
    if (img.base && img.contains(reinterpret_cast<void*>(g[REG_RIP]))) {
        // Real backtrace through the game's .pdata unwind info.
        static seh::Context c;  // big; we're on the alt stack
        memset(&c, 0, sizeof c);
        const int map[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                             REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
        for (int i = 0; i < 16; i++) c.Gpr[i] = g[map[i]];
        c.Rip = g[REG_RIP];
        uint64_t pcs[32];
        int n = seh::walk_stack(c, pcs, 32);
        for (int i = 0; i < n; i++)
            fl::log_write("FATAL", "  #%-2d game rva 0x%llx", i,
                          (unsigned long long)(pcs[i] - reinterpret_cast<uint64_t>(img.base)));
    } else {
        // Crash outside the game: scan the stack for return addresses into it.
        auto* sp = reinterpret_cast<uint64_t*>(g[REG_RSP]);
        int found = 0;
        for (int i = 0; i < 512 && found < 16; i++) {
            uint64_t v = sp[i];
            if (img.base && img.contains(reinterpret_cast<void*>(v))) {
                fl::log_write("FATAL", "  stack[%3d] game rva 0x%llx", i,
                              (unsigned long long)(v - reinterpret_cast<uint64_t>(img.base)));
                found++;
            }
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

void install_crash_handler() {
    // Alternate stack so stack overflows can still be reported.
    static std::vector<uint8_t> altstack(1 << 16);
    stack_t ss = {};
    ss.ss_sp = altstack.data();
    ss.ss_size = altstack.size();
    sigaltstack(&ss, nullptr);

    struct sigaction sa = {};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
}

void set_fault_hook(bool (*hook)(uint64_t addr, bool write)) { g_fault_hook = hook; }

void* make_data_trap(const std::string& dll, const std::string& name) {
    std::lock_guard lock(g_mutex);
    void* p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fatal("mmap data trap failed");
    g_data_traps.push_back({static_cast<uint8_t*>(p), dll, name});
    return p;
}

}  // namespace fl::loader
