// kernelx: events, mutexes, semaphores, waits, critical sections, SRW locks.

#include <ctime>
#include <sched.h>
#include <thread>
#include <vector>

#include "common/log.h"
#include "hle/kernelx/futex_lock.h"
#include "hle/kernelx/kobject.h"
#include "hle/kernelx/sync_objects.h"
#include "hle/registry.h"

namespace fl::kx {
namespace {

// ---------------------------------------------------------------- objects

HANDLE new_event(bool manual, bool initial) {
    auto e = std::make_shared<Event>();
    e->manual = manual;
    e->state = initial;
    return handle_add(e);
}

WINAPI HANDLE kx_CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR name) {
    if (name) FL_LOG(kernelx, "CreateEventW: named event ignored");
    return new_event(manual, initial);
}

WINAPI HANDLE kx_CreateEventA(void* sa, BOOL manual, BOOL initial, LPCSTR name) {
    if (name) FL_LOG(kernelx, "CreateEventA: named event '%s' ignored", name);
    return new_event(manual, initial);
}

WINAPI HANDLE kx_CreateEventExW(void* sa, LPCWSTR name, DWORD flags, DWORD access) {
    return new_event(flags & 1 /* CREATE_EVENT_MANUAL_RESET */, flags & 2 /* CREATE_EVENT_INITIAL_SET */);
}

WINAPI BOOL kx_SetEvent(HANDLE h) {
    auto e = handle_get_as<Event>(h, ObjType::Event);
    if (!e) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    std::lock_guard lock(wait_lock());
    e->state = true;
    e->wake_all();
    return TRUE_;
}

WINAPI BOOL kx_ResetEvent(HANDLE h) {
    auto e = handle_get_as<Event>(h, ObjType::Event);
    if (!e) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    std::lock_guard lock(wait_lock());
    e->state = false;
    return TRUE_;
}

HANDLE new_mutex(bool initial_owner) {
    auto m = std::make_shared<Mutex>();
    if (initial_owner) {
        m->owner = current_tid();
        m->count = 1;
    }
    return handle_add(m);
}

WINAPI HANDLE kx_CreateMutexW(void* sa, BOOL owner, LPCWSTR name) { return new_mutex(owner); }
WINAPI HANDLE kx_CreateMutexA(void* sa, BOOL owner, LPCSTR name) { return new_mutex(owner); }

WINAPI BOOL kx_ReleaseMutex(HANDLE h) {
    auto m = handle_get_as<Mutex>(h, ObjType::Mutex);
    if (!m) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    std::lock_guard lock(wait_lock());
    if (m->owner != current_tid() || m->count == 0) return set_last_error(288 /* ERROR_NOT_OWNER */), FALSE_;
    if (--m->count == 0) {
        m->owner = 0;
        m->wake_all();
    }
    return TRUE_;
}

WINAPI HANDLE kx_CreateSemaphoreExW(void* sa, LONG initial, LONG maximum, LPCWSTR name, DWORD flags, DWORD access) {
    auto s = std::make_shared<Semaphore>();
    s->count = initial;
    s->max = maximum;
    return handle_add(s);
}

WINAPI BOOL kx_ReleaseSemaphore(HANDLE h, LONG release, LONG* previous) {
    auto s = handle_get_as<Semaphore>(h, ObjType::Semaphore);
    if (!s) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    std::lock_guard lock(wait_lock());
    if (previous) *previous = s->count;
    if (release <= 0 || s->count + release > s->max) return set_last_error(298 /* ERROR_TOO_MANY_POSTS */), FALSE_;
    s->count += release;
    s->wake_all();
    return TRUE_;
}

// ---------------------------------------------------------------- waits

DWORD wait_handles(DWORD n, const HANDLE* handles, bool all, DWORD ms) {
    std::vector<std::shared_ptr<KObject>> keep;  // hold refs during the wait
    std::vector<Waitable*> objs;
    for (DWORD i = 0; i < n; i++) {
        auto o = handle_get(handles[i]);
        if (!o || o->type == ObjType::File || o->type == ObjType::Iocp || o->type == ObjType::FindFile ||
            o->type == ObjType::Module || o->type == ObjType::Other) {
            FL_ERROR("wait on invalid/non-waitable handle %p", handles[i]);
            set_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        objs.push_back(static_cast<Waitable*>(o.get()));
        keep.push_back(std::move(o));
    }
    return wait_for(objs, all, ms);
}

WINAPI DWORD kx_WaitForSingleObject(HANDLE h, DWORD ms) { return wait_handles(1, &h, false, ms); }
WINAPI DWORD kx_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) { return wait_handles(1, &h, false, ms); }
WINAPI DWORD kx_WaitForMultipleObjects(DWORD n, const HANDLE* h, BOOL all, DWORD ms) {
    return wait_handles(n, h, all, ms);
}
WINAPI DWORD kx_WaitForMultipleObjectsEx(DWORD n, const HANDLE* h, BOOL all, DWORD ms, BOOL alertable) {
    return wait_handles(n, h, all, ms);
}

// A yield (SwitchToThread, Sleep(0)). The game's lightweight semaphores spin
// on SwitchToThread while they wait (rva 0x1088930: take a token with lock
// xadd, put it back, check the timeout against the clock, yield, retry). On the
// console such a thread owns its core; here sched_yield returns at once on an
// idle machine, and one waiter burned a whole core at 30 fps (99% of a
// thread). After 200 back-to-back yields a thread sleeps ~0.1 ms per yield
// instead: it notices a release at most ~0.1 ms later and uses ~2% of a core.
void yield_thread() {
    thread_local uint32_t spins = 0;
    thread_local int64_t last = 0;
    auto now_ns = [] {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
    };
    int64_t now = now_ns();
    spins = now - last < 200'000 ? spins + 1 : 0;  // the previous yield was < 0.2 ms ago: still spinning
    if (spins > 200) {
        timespec d{0, 100'000};
        nanosleep(&d, nullptr);
    } else {
        sched_yield();
    }
    last = now_ns();
}

WINAPI void kx_Sleep(DWORD ms) {
    if (ms == 0)
        yield_thread();
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

WINAPI DWORD kx_SleepEx(DWORD ms, BOOL alertable) {
    kx_Sleep(ms);
    return 0;
}

WINAPI BOOL kx_SwitchToThread() {
    yield_thread();
    return TRUE_;
}

// ---------------------------------------------------------------- critical sections
// Layout (40 bytes): DebugInfo*, LONG LockCount, LONG RecursionCount,
// HANDLE OwningThread, HANDLE LockSemaphore, ULONG_PTR SpinCount.
// We use LockCount as our futex word (0 = free), which differs from real
// Windows (-1 = free). The game only touches it through these functions.

struct CriticalSection {
    void* DebugInfo;
    std::atomic<int32_t> LockCount;
    int32_t RecursionCount;
    uint64_t OwningThread;
    uint64_t LockSemaphore;
    uint64_t SpinCount;
};
static_assert(sizeof(CriticalSection) == 40);

WINAPI void kx_InitializeCriticalSection(CriticalSection* cs) {
    // Windows sets DebugInfo to a pointer, or to -1 without debug info. The game
    // treats DebugInfo != 0 as "initialized" (spin loop at rva 0x13d0822).
    cs->DebugInfo = reinterpret_cast<void*>(-1);
    cs->LockCount.store(0);
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->LockSemaphore = 0;
    cs->SpinCount = 0;
}

WINAPI BOOL kx_InitializeCriticalSectionAndSpinCount(CriticalSection* cs, DWORD spin) {
    kx_InitializeCriticalSection(cs);
    cs->SpinCount = spin;
    return TRUE_;
}

WINAPI BOOL kx_InitializeCriticalSectionEx(CriticalSection* cs, DWORD spin, DWORD flags) {
    return kx_InitializeCriticalSectionAndSpinCount(cs, spin);
}

WINAPI void kx_DeleteCriticalSection(CriticalSection* cs) {}

WINAPI void kx_EnterCriticalSection(CriticalSection* cs) {
    DWORD tid = current_tid();
    if (cs->OwningThread == tid) {
        cs->RecursionCount++;
        return;
    }
    futex_lock(&cs->LockCount, static_cast<uint32_t>(std::min<uint64_t>(cs->SpinCount, 4000)));
    cs->OwningThread = tid;
    cs->RecursionCount = 1;
}

WINAPI BOOL kx_TryEnterCriticalSection(CriticalSection* cs) {
    DWORD tid = current_tid();
    if (cs->OwningThread == tid) {
        cs->RecursionCount++;
        return TRUE_;
    }
    if (!futex_try_lock(&cs->LockCount)) return FALSE_;
    cs->OwningThread = tid;
    cs->RecursionCount = 1;
    return TRUE_;
}

WINAPI void kx_LeaveCriticalSection(CriticalSection* cs) {
    if (--cs->RecursionCount > 0) return;
    cs->OwningThread = 0;
    futex_unlock(&cs->LockCount);
}

// ---------------------------------------------------------------- SRW locks
// SRWLOCK is one pointer; zero = SRWLOCK_INIT = unlocked. We use the low 32
// bits as a reader/writer word: 0 free, -1 writer, n > 0 readers. Waiters
// sleep on the word and are all woken on release (SRW locks are not fair).

std::atomic<int32_t>* srw(void* lock) { return static_cast<std::atomic<int32_t>*>(lock); }

WINAPI void kx_InitializeSRWLock(void* lock) { *static_cast<uint64_t*>(lock) = 0; }

WINAPI void kx_AcquireSRWLockExclusive(void* lock) {
    auto* w = srw(lock);
    for (int spin = 0;; spin++) {
        int32_t c = 0;
        if (w->compare_exchange_weak(c, -1, std::memory_order_acquire)) return;
        if (spin < 100) {
            __builtin_ia32_pause();
            continue;
        }
        futex_wait(w, c);
    }
}
WINAPI void kx_ReleaseSRWLockExclusive(void* lock) {
    srw(lock)->store(0, std::memory_order_release);
    futex_wake(srw(lock), INT32_MAX);
}
WINAPI void kx_AcquireSRWLockShared(void* lock) {
    auto* w = srw(lock);
    for (int spin = 0;; spin++) {
        int32_t c = w->load(std::memory_order_relaxed);
        if (c >= 0 && w->compare_exchange_weak(c, c + 1, std::memory_order_acquire)) return;
        if (spin < 100) {
            __builtin_ia32_pause();
            continue;
        }
        if (c < 0) futex_wait(w, c);
    }
}
WINAPI void kx_ReleaseSRWLockShared(void* lock) {
    if (srw(lock)->fetch_sub(1, std::memory_order_release) == 1) futex_wake(srw(lock), INT32_MAX);
}

}  // namespace

bool signal_event(HANDLE h) { return kx_SetEvent(h) != 0; }
bool reset_event(HANDLE h) { return kx_ResetEvent(h) != 0; }
HANDLE create_event(bool manual, bool initial) { return new_event(manual, initial); }
DWORD wait_multiple(DWORD n, const HANDLE* handles, bool all, DWORD ms) { return wait_handles(n, handles, all, ms); }

HLE_EXPORT(kernelx, CreateEventW, kx_CreateEventW);
HLE_EXPORT(kernelx, CreateEventA, kx_CreateEventA);
HLE_EXPORT(kernelx, CreateEventExW, kx_CreateEventExW);
HLE_EXPORT(kernelx, SetEvent, kx_SetEvent);
HLE_EXPORT(kernelx, ResetEvent, kx_ResetEvent);
HLE_EXPORT(kernelx, CreateMutexW, kx_CreateMutexW);
HLE_EXPORT(kernelx, CreateMutexA, kx_CreateMutexA);
HLE_EXPORT(kernelx, ReleaseMutex, kx_ReleaseMutex);
HLE_EXPORT(kernelx, CreateSemaphoreExW, kx_CreateSemaphoreExW);
HLE_EXPORT(kernelx, ReleaseSemaphore, kx_ReleaseSemaphore);
HLE_EXPORT(kernelx, WaitForSingleObject, kx_WaitForSingleObject);
HLE_EXPORT(kernelx, WaitForSingleObjectEx, kx_WaitForSingleObjectEx);
HLE_EXPORT(kernelx, WaitForMultipleObjects, kx_WaitForMultipleObjects);
HLE_EXPORT(kernelx, WaitForMultipleObjectsEx, kx_WaitForMultipleObjectsEx);
HLE_EXPORT(kernelx, Sleep, kx_Sleep);
HLE_EXPORT(kernelx, SleepEx, kx_SleepEx);
HLE_EXPORT(kernelx, SwitchToThread, kx_SwitchToThread);
HLE_EXPORT(kernelx, InitializeCriticalSection, kx_InitializeCriticalSection);
HLE_EXPORT(kernelx, InitializeCriticalSectionAndSpinCount, kx_InitializeCriticalSectionAndSpinCount);
HLE_EXPORT(kernelx, InitializeCriticalSectionEx, kx_InitializeCriticalSectionEx);
HLE_EXPORT(kernelx, DeleteCriticalSection, kx_DeleteCriticalSection);
HLE_EXPORT(kernelx, EnterCriticalSection, kx_EnterCriticalSection);
HLE_EXPORT(kernelx, TryEnterCriticalSection, kx_TryEnterCriticalSection);
HLE_EXPORT(kernelx, LeaveCriticalSection, kx_LeaveCriticalSection);
HLE_EXPORT(kernelx, InitializeSRWLock, kx_InitializeSRWLock);
HLE_EXPORT(kernelx, AcquireSRWLockShared, kx_AcquireSRWLockShared);
HLE_EXPORT(kernelx, ReleaseSRWLockShared, kx_ReleaseSRWLockShared);
HLE_EXPORT(kernelx, AcquireSRWLockExclusive, kx_AcquireSRWLockExclusive);
HLE_EXPORT(kernelx, ReleaseSRWLockExclusive, kx_ReleaseSRWLockExclusive);

}  // namespace fl::kx
