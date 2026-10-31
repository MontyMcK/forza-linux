// Tests for src/platform/os/ (the host OS layer). Each backend must pass them.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>

#include "platform/os/debug.h"
#include "platform/os/files.h"
#include "platform/os/futex.h"
#include "platform/os/memory.h"
#include "platform/os/signal_context.h"
#include "platform/os/thread.h"
#include "platform/os/write_watch.h"

#include <csignal>
#include <ctime>
#include <shared_mutex>

using namespace fl;
using clk = std::chrono::steady_clock;

static int failures;
static int out_fd = -1;  // the PS5's network log (a title's printf goes nowhere)
static void say(const char* line) {
    std::printf("%s\n", line);
    if (out_fd >= 0) {
        std::string l = std::string(line) + "\n";
        send(out_fd, l.data(), l.size(), 0);
    }
}
static void check(bool ok, const char* what) {
    say((std::string(ok ? "PASS  " : "FAIL  ") + what).c_str());
    if (!ok) failures++;
}
static thread_local int t_counter;  // for the thread_local cost below

// Costs that decide host-specific policy (FINDINGS 7.13), printed, not checked.
static void report_costs(size_t page) {
    char line[160];
    auto per = [](clk::time_point t0, int n) { return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / n; };
    timespec ts;
    auto t0 = clk::now();
    for (int i = 0; i < 100000; i++) clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(line, sizeof line, "COST  clock_gettime(CLOCK_MONOTONIC): %.0f ns", per(t0, 100000));
    say(line);
    auto* p = static_cast<uint8_t*>(os::map_anon(nullptr, 16 * page, os::kRead | os::kWrite));
    t0 = clk::now();
    for (int i = 0; i < 10000; i++) os::protect(p, page, (i & 1) ? os::kRead | os::kWrite : os::kRead);
    snprintf(line, sizeof line, "COST  protect, 1 page: %.0f ns", per(t0, 10000));
    say(line);
    t0 = clk::now();
    for (int i = 0; i < 10000; i++) os::protect(p, 16 * page, (i & 1) ? os::kRead | os::kWrite : os::kRead);
    snprintf(line, sizeof line, "COST  protect, 16 pages: %.0f ns", per(t0, 10000));
    say(line);
    os::protect(p, 16 * page, os::kRead | os::kWrite);
    os::release(p, 16 * page);
    std::shared_mutex m;
    t0 = clk::now();
    for (int i = 0; i < 1000000; i++) {
        m.lock_shared();
        m.unlock_shared();
    }
    snprintf(line, sizeof line, "COST  shared_mutex lock_shared + unlock_shared: %.0f ns", per(t0, 1000000));
    say(line);
    t0 = clk::now();
    for (int i = 0; i < 1000000; i++) {
        t_counter++;
        asm volatile("" ::: "memory");
    }
    snprintf(line, sizeof line, "COST  thread_local increment: %.1f ns", per(t0, 1000000));
    say(line);
}

static double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

#ifdef OH2_LOG_HOST
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
// On the PS5 (a title has no visible stdout): send the output to
// tools/ps5/netlog.py on the PC.
static void stdout_to_netlog() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(OH2_LOG_PORT);
    a.sin_addr.s_addr = inet_addr(OH2_LOG_HOST);
    if (s >= 0 && connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) out_fd = s;
}
#endif

int main() {
#ifdef OH2_LOG_HOST
    stdout_to_netlog();
    say("os_test on the PS5 backend");
#endif
    std::atomic<int32_t> word{0};

    // A waiter sleeps until another thread changes the word and wakes it.
    std::atomic<double> woke_after{-1};
    std::thread t([&] {
        auto t0 = clk::now();
        while (word.load() == 0) os::futex_wait(&word, 0, 2000000000);
        woke_after = ms_since(t0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    word = 1;
    os::futex_wake(&word, 1);
    t.join();
    check(woke_after > 30 && woke_after < 1500, "futex wait/wake between two threads");

    // A word that already differs returns at once.
    auto t0 = clk::now();
    bool r = os::futex_wait(&word, 0, 1000000000);
    check(r && ms_since(t0) < 100, "futex wait on a changed value returns at once");

    // Timeout.
    t0 = clk::now();
    r = os::futex_wait(&word, 1, 30000000);
    double dt = ms_since(t0);
    check(!r && dt > 20 && dt < 500, "futex wait times out after 30 ms");

    check(os::path_exists("/") && !os::path_exists("/no/such/path/for/os_test"), "path_exists");

    // Memory.
    const size_t page = os::page_size();
    check(page >= 4096 && (page & (page - 1)) == 0, "page size is a power of two >= 4 KiB");

    auto* a = static_cast<uint8_t*>(os::map_anon(nullptr, 4 * page, os::kRead | os::kWrite, 0x10000));
    check(a && reinterpret_cast<uintptr_t>(a) % 0x10000 == 0 && a[0] == 0 && a[4 * page - 1] == 0,
          "map_anon: aligned, zero-filled");
    a[0] = 0x5a;
    check(os::map_anon(a, page, os::kRead | os::kWrite) == nullptr && a[0] == 0x5a,
          "map_anon at a taken address fails and keeps the old mapping");
    os::release(a, 4 * page);
    auto* b = static_cast<uint8_t*>(os::map_anon(a, page, os::kRead | os::kWrite));
    check(b == a, "map_anon exactly at a free address");
    os::release(b, page);

    auto* res = static_cast<uint8_t*>(os::map_anon(nullptr, 2 * page, os::kNone));
    bool ok = res && os::commit(res, page, os::kRead | os::kWrite);
    if (ok) res[10] = 7;
    ok = ok && os::commit(res, 2 * page, os::kRead | os::kWrite) && res[10] == 7 && res[page] == 0;
    check(ok, "commit keeps committed pages, new pages read zero");
    ok = os::decommit(res, page) && os::commit(res, page, os::kRead | os::kWrite) && res[10] == 0;
    check(ok, "decommit drops contents, keeps the reservation");
    os::release(res, 2 * page);

    // Decommitting whole 64 KiB units gives the memory back (the PS5 backend
    // once kept it until release), and the range can be committed again.
    auto* big = static_cast<uint8_t*>(os::map_anon(nullptr, 32 * page, os::kRead | os::kWrite, 0x10000));
    std::memset(big, 0x33, 32 * page);
    uint64_t rss_before = os::resident_bytes();
    ok = big && os::decommit(big + 8 * page, 16 * page);
    uint64_t rss_after = os::resident_bytes();
    ok = ok && rss_after + 8 * page <= rss_before && os::commit(big + 8 * page, 16 * page, os::kRead | os::kWrite) &&
         big[8 * page] == 0 && big[24 * page - 1] == 0 && big[8 * page - 1] == 0x33 && big[24 * page] == 0x33;
    if (ok) big[10 * page] = 1;
    check(ok, "decommit of whole units returns memory; committing again reads zero");
    // One unit decommitted and committed again (the PS5 reuses parked units).
    const size_t unit = std::max<size_t>(page, 0x10000);
    std::memset(big, 0x44, unit);
    ok = os::decommit(big, unit) && os::commit(big, unit, os::kRead | os::kWrite) && big[0] == 0 && big[unit - 1] == 0;
    check(ok, "a single unit decommitted and committed again reads zero");
    os::release(big, 32 * page);

    auto* code = static_cast<uint8_t*>(os::map_anon(nullptr, page, os::kRead | os::kWrite));
    const uint8_t ret42[] = {0xB8, 42, 0, 0, 0, 0xC3};  // mov eax, 42; ret
    std::memcpy(code, ret42, sizeof ret42);
    ok = os::protect(code, page, os::kRead | os::kExec);
    check(ok && reinterpret_cast<int (*)()>(code)() == 42, "protect adds execute to written code");
    os::release(code, page);
    check(os::resident_bytes() > 0, "resident_bytes");

    // Write watching: registering a range that starts inside a watched one
    // watches the union (the PS5 backend once erased one map entry twice here).
    auto* ww = static_cast<uint8_t*>(os::map_anon(nullptr, 6 * page, os::kRead | os::kWrite));
    uint64_t w0 = reinterpret_cast<uint64_t>(ww);
    size_t runs = 0;
    auto count = [](uint64_t, uint64_t, void* n) { ++*static_cast<size_t*>(n); };
    ok = ww && os::write_watch_init() && os::write_watch_register(w0, w0 + 4 * page) &&
         os::write_watch_register(w0 + page, w0 + 6 * page) && os::write_watch_register(w0 + 2 * page, w0 + 3 * page);
    ok = ok && os::write_watch_reset(w0, w0 + 6 * page, true, count, &runs) && runs >= 1 &&
         !os::write_watch_written(w0, w0 + 6 * page);
    check(ok, "write watch: overlapping registrations watch their union");

    // Checked copies stop at memory they can't access.
    auto* two = static_cast<uint8_t*>(os::map_anon(nullptr, 2 * page, os::kRead | os::kWrite));
    std::memset(two, 0x11, page);
    os::protect(two + page, page, os::kNone);
    static uint8_t buf[1 << 16];
    check(os::read_checked(buf, two + page - 64, 128) == 64 && buf[0] == 0x11, "read_checked stops at a no-access page");
    check(os::read_checked(buf, two + page, 16) == 0, "read_checked of a no-access page copies nothing");
    os::protect(two, page, os::kRead);
    check(os::write_checked(two, buf, 16) == 0 && two[0] == 0x11, "write_checked into a read-only page writes nothing");
    os::release(two, 2 * page);

    // Thread environment.
    static uint64_t teb_block[4] = {0x7eb};
    uint64_t gs0 = 0;
    bool gs_ok = os::set_gs_base(teb_block);
    if (gs_ok) __asm__ volatile("movq %%gs:0, %0" : "=r"(gs0));
    os::set_gs_base(nullptr);
    check(gs_ok && gs0 == 0x7eb, "set_gs_base: gs:0 reads the block");
    os::prepare_game_thread();
    unsigned mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    check((mxcsr & 0xffc0) == 0x1f80, "prepare_game_thread leaves the Windows MXCSR (0x1f80)");

    static std::atomic<int> got_signal{0};
    struct sigaction sa = {};
    sa.sa_handler = [](int) { got_signal = 1; };
    sigaction(SIGUSR2, &sa, nullptr);
    std::atomic<uint64_t> other_tid{0};
    std::atomic<bool> stop{false};
    std::thread t2([&] {
        other_tid = os::thread_id();
        while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!other_tid) std::this_thread::yield();
    bool sent = os::signal_thread(other_tid, SIGUSR2);
    for (int i = 0; i < 1000 && !got_signal; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop = true;
    t2.join();
    check(other_tid != os::thread_id() && os::process_id() != 0, "thread ids differ between threads");
    check(sent && got_signal, "signal_thread delivers to that thread");

    // Diagnostics.
    bool self_listed = false;
    for (const os::ThreadInfo& ti : os::list_threads()) self_listed |= ti.tid == os::thread_id() && !ti.name.empty();
    check(self_listed, "list_threads includes this thread with a name");
    int on_stack = 0;
    check(!os::mapping_of(reinterpret_cast<uint64_t>(&on_stack)).empty() && os::mapping_of(8).empty(),
          "mapping_of: the stack is mapped, address 8 is not");

    // Signal context: read rax and xmm0 of the interrupted code, change rax.
    static uint64_t seen_rax, seen_xmm0;
    struct sigaction tr = {};
    tr.sa_flags = SA_SIGINFO;
    tr.sa_sigaction = [](int, siginfo_t*, void* uc) {
        os::Regs g(uc);
        seen_rax = g[os::kRax];
        std::memcpy(&seen_xmm0, os::fxsave_area(uc) + 160, 8);
        g[os::kRax] = 0x55;
    };
    sigaction(SIGTRAP, &tr, nullptr);
    uint64_t after;
    __asm__ volatile("movabs $0x1234567890abcdef, %%rax\n\tmovq %%rax, %%xmm0\n\tint3\n\tmovq %%rax, %0"
                     : "=r"(after) : : "rax", "xmm0");
    check(seen_rax == 0x1234567890abcdefull && seen_xmm0 == 0x1234567890abcdefull && after == 0x55,
          "signal context: read rax and xmm0, write rax");

    report_costs(page);
    say(failures ? "FAILED" : "all passed");
#ifdef OH2_LOG_HOST
    // On the PS5 this is a title, which must not return from main (the
    // console treats that as a failure): stay open until closed.
    if (out_fd >= 0) close(out_fd);
    for (;;) sleep(3600);
#endif
    return failures ? 1 : 0;
}
