// Kernel objects behind Windows HANDLEs, and the shared wait machinery.
//
// All waitable state changes and waits happen under one global lock
// (wait_lock()). That is simple and correct; optimize only if profiling says so.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <string>
#include <mutex>
#include <span>
#include <vector>

#include "common/winabi.h"

namespace fl::kx {

enum class ObjType { Event, Mutex, Semaphore, Thread, File, FindFile, Iocp, Module, Other };

struct KObject {
    explicit KObject(ObjType t) : type(t) {}
    virtual ~KObject() = default;
    const ObjType type;
};

struct Waiter {
    std::condition_variable cv;
};

// Waitable objects. All virtuals are called with wait_lock() held.
struct Waitable : KObject {
    using KObject::KObject;
    std::vector<Waiter*> waiters;
    virtual bool signaled(DWORD tid) const = 0;
    virtual void consume(DWORD tid) {}  // auto-reset, mutex ownership, semaphore count
    void wake_all() {
        for (Waiter* w : waiters) w->cv.notify_all();
    }
};

std::mutex& wait_lock();

// Handle table. Handles are small multiples of 4, never 0 or -1.
HANDLE handle_add(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> handle_get(HANDLE h);
bool handle_close(HANDLE h);
// Open handles by type, e.g. "handles 812: event 300, thread 60, file 420" (FL_LOG=fps leak watch).
std::string handle_summary();

template <typename T>
std::shared_ptr<T> handle_get_as(HANDLE h, ObjType type) {
    auto o = handle_get(h);
    if (!o || o->type != type) return nullptr;
    return std::static_pointer_cast<T>(o);
}

// Wait for one or all of `objs`. timeout_ms may be INFINITE.
// Returns WAIT_OBJECT_0 + index, WAIT_TIMEOUT, or WAIT_FAILED.
DWORD wait_for(std::span<Waitable* const> objs, bool wait_all, DWORD timeout_ms);

// Current thread id (Linux tid), cached.
DWORD current_tid();

// Pseudo handles.
inline HANDLE const kCurrentProcess = reinterpret_cast<HANDLE>(-1);
inline HANDLE const kCurrentThread = reinterpret_cast<HANDLE>(-2);

// Start a host function on a new thread that has a TEB (so it may call game
// code, e.g. worker pools running game callbacks).
HANDLE spawn_game_thread(void (*fn)(void*), void* arg);

// Set the Windows last-error value of the calling game thread.
void set_last_error(DWORD e);
// The affinity mask GetProcessAffinityMask reports (the title's cores).
uint64_t process_affinity_mask();

}  // namespace fl::kx
