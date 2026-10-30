#include "platform/os/thread.h"

#include <map>
#include <mutex>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <unistd.h>

#include "platform/os/ps5/kernel.h"

namespace fl::os {
namespace {
constexpr int kAmd64SetGsBase = 131;

// A title can't signal a thread by id (no thr_kill), only by pthread_t: every
// thread that asks for its id is remembered here.
std::mutex g_mu;
std::map<uint64_t, pthread_t> g_threads;
}  // namespace

bool set_gs_base(void* base) {
    uint64_t b = reinterpret_cast<uint64_t>(base);
    return sysarch(kAmd64SetGsBase, &b) == 0;
}

void prepare_game_thread() {
    // Titles' threads can start with flush-to-zero on (MXCSR 0x9fc0 in a
    // payload); the game expects Windows' 0x1f80.
    const unsigned mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    thread_id();
}

uint64_t thread_id() {
    static thread_local uint64_t id = 0;
    if (!id) {
        id = static_cast<uint64_t>(pthread_getthreadid_np());
        std::lock_guard lock(g_mu);
        g_threads[id] = pthread_self();
    }
    return id;
}

void current_stack(void** lo, size_t* size) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_get_np(pthread_self(), &attr);  // FreeBSD's pthread_getattr_np
    pthread_attr_getstack(&attr, lo, size);
    pthread_attr_destroy(&attr);
}

uint64_t process_id() { return static_cast<uint64_t>(getpid()); }

bool signal_thread(uint64_t tid, int sig) {
    pthread_t t;
    {
        std::lock_guard lock(g_mu);
        auto it = g_threads.find(tid);
        if (it == g_threads.end()) return false;
        t = it->second;
    }
    return pthread_kill(t, sig) == 0;
}

// For debug.cpp: the threads we know of.
std::map<uint64_t, pthread_t> known_threads() {
    std::lock_guard lock(g_mu);
    return g_threads;
}

}  // namespace fl::os
