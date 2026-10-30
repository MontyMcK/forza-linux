// Tests for src/platform/os/ (the host OS layer). Each backend must pass them.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "platform/os/files.h"
#include "platform/os/futex.h"

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

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
