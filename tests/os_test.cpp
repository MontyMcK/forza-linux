// Tests for src/platform/os/ (the host OS layer). Each backend must pass them.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <cstdint>
#include <cstring>

#include "platform/os/files.h"
#include "platform/os/futex.h"
#include "platform/os/memory.h"

using namespace fl;
using clk = std::chrono::steady_clock;

static int failures;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
static double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

int main() {
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

    auto* code = static_cast<uint8_t*>(os::map_anon(nullptr, page, os::kRead | os::kWrite));
    const uint8_t ret42[] = {0xB8, 42, 0, 0, 0, 0xC3};  // mov eax, 42; ret
    std::memcpy(code, ret42, sizeof ret42);
    ok = os::protect(code, page, os::kRead | os::kExec);
    check(ok && reinterpret_cast<int (*)()>(code)() == 42, "protect adds execute to written code");
    os::release(code, page);
    check(os::resident_bytes() > 0, "resident_bytes");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
