#include "hle/kernelx/kobject.h"

#include <algorithm>
#include <chrono>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

#include "loader/thread_env.h"
#include "platform/os/thread.h"

namespace fl::kx {
namespace {

std::mutex g_table_mutex;
std::unordered_map<uint64_t, std::shared_ptr<KObject>> g_table;
uint64_t g_next = 0x1000;

}  // namespace

std::mutex& wait_lock() {
    static std::mutex m;
    return m;
}

HANDLE handle_add(std::shared_ptr<KObject> obj) {
    std::lock_guard lock(g_table_mutex);
    uint64_t h = g_next;
    g_next += 4;
    g_table[h] = std::move(obj);
    return reinterpret_cast<HANDLE>(h);
}

std::shared_ptr<KObject> handle_get(HANDLE h) {
    std::lock_guard lock(g_table_mutex);
    auto it = g_table.find(reinterpret_cast<uint64_t>(h));
    return it == g_table.end() ? nullptr : it->second;
}

std::string handle_summary() {
    static const char* const kNames[] = {"event", "mutex", "semaphore", "thread", "file", "find", "iocp", "module", "other"};
    size_t counts[9] = {}, total;
    {
        std::lock_guard lock(g_table_mutex);
        total = g_table.size();
        for (auto& [h, o] : g_table) counts[std::min<size_t>(8, static_cast<size_t>(o->type))]++;
    }
    std::string out = "handles " + std::to_string(total) + ":";
    for (int i = 0; i < 9; i++)
        if (counts[i]) out += std::string(" ") + kNames[i] + " " + std::to_string(counts[i]);
    return out;
}

bool handle_close(HANDLE h) {
    std::shared_ptr<KObject> victim;  // destroyed outside the lock
    std::lock_guard lock(g_table_mutex);
    auto it = g_table.find(reinterpret_cast<uint64_t>(h));
    if (it == g_table.end()) return false;
    victim = std::move(it->second);
    g_table.erase(it);
    return true;
}

DWORD current_tid() {
    static thread_local DWORD tid = static_cast<DWORD>(os::thread_id());
    return tid;
}

void set_last_error(DWORD e) {
    if (auto* teb = loader::current_teb()) teb->field<DWORD>(loader::teb_off::LastErrorValue) = e;
}

DWORD wait_for(std::span<Waitable* const> objs, bool wait_all, DWORD timeout_ms) {
    using clock = std::chrono::steady_clock;
    const DWORD tid = current_tid();
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(wait_lock());
    Waiter self;
    bool registered = false;

    auto unregister = [&] {
        if (!registered) return;
        for (Waitable* o : objs) std::erase(o->waiters, &self);
        registered = false;
    };

    // Returns the WAIT_* result if the wait is satisfied now, else -1.
    auto try_acquire = [&]() -> int64_t {
        if (wait_all) {
            for (Waitable* o : objs)
                if (!o->signaled(tid)) return -1;
            for (Waitable* o : objs) o->consume(tid);
            return WAIT_OBJECT_0;
        }
        for (size_t i = 0; i < objs.size(); i++) {
            if (objs[i]->signaled(tid)) {
                objs[i]->consume(tid);
                return WAIT_OBJECT_0 + static_cast<DWORD>(i);
            }
        }
        return -1;
    };

    for (;;) {
        if (int64_t r = try_acquire(); r >= 0) {
            unregister();
            return static_cast<DWORD>(r);
        }
        if (timeout_ms == 0) return WAIT_TIMEOUT;
        if (!registered) {
            for (Waitable* o : objs) o->waiters.push_back(&self);
            registered = true;
        }
        if (timeout_ms == INFINITE) {
            self.cv.wait(lock);
        } else if (self.cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            int64_t r = try_acquire();
            unregister();
            return r >= 0 ? static_cast<DWORD>(r) : WAIT_TIMEOUT;
        }
    }
}

}  // namespace fl::kx
