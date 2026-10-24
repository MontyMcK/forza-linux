// kernelx: threads, thread-local storage slots, fibers.

#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

#include "common/log.h"
#include "hle/kernelx/futex_lock.h"
#include "hle/kernelx/kobject.h"
#include "hle/registry.h"
#include "loader/thread_env.h"

extern "C" void fl_fiber_switch(uint64_t* save_rsp, uint64_t new_rsp);
extern "C" void fl_fiber_entry();

namespace fl::kx {
namespace {

using loader::teb_off::DeallocationStack;
using loader::teb_off::FiberData;
using loader::teb_off::StackBase;
using loader::teb_off::StackLimit;
using loader::teb_off::TlsSlots;

constexpr DWORD STILL_ACTIVE = 259;
constexpr DWORD CREATE_SUSPENDED = 0x4;
constexpr size_t kMinThreadStack = 2u << 20;
constexpr int kSuspendSignal = SIGUSR2;

using ThreadStart = DWORD(WINAPI*)(void* param);

struct Thread : Waitable {
    Thread() : Waitable(ObjType::Thread) {}
    std::atomic<DWORD> tid{0};
    bool exited = false;  // guarded by wait_lock()
    DWORD exit_code = STILL_ACTIVE;
    int priority = 0;
    ThreadStart start = nullptr;
    void* param = nullptr;
    std::atomic<int32_t> suspend_count{0};
    std::atomic<int32_t> parked{0};
    std::atomic<int32_t> started{0};
    timespec created{};

    bool signaled(DWORD) const override { return exited; }
};

std::mutex g_threads_mutex;
std::unordered_map<DWORD, std::weak_ptr<Thread>> g_threads;
thread_local std::shared_ptr<Thread> t_self;
thread_local Thread* t_self_raw = nullptr;  // for the signal handler

void register_thread(const std::shared_ptr<Thread>& t) {
    std::lock_guard lock(g_threads_mutex);
    g_threads[t->tid] = t;
}

std::shared_ptr<Thread> current_thread() {
    if (!t_self) {
        // A thread we didn't create through CreateThread (the game's main thread).
        t_self = std::make_shared<Thread>();
        t_self->tid = current_tid();
        clock_gettime(CLOCK_REALTIME, &t_self->created);
        t_self_raw = t_self.get();
        register_thread(t_self);
    }
    return t_self;
}

std::shared_ptr<Thread> thread_from_handle(HANDLE h) {
    if (h == kCurrentThread) return current_thread();
    return handle_get_as<Thread>(h, ObjType::Thread);
}

// Suspension of a running thread: we signal it and it parks in the handler.
void suspend_handler(int, siginfo_t*, void*) {
    Thread* t = t_self_raw;
    if (!t) return;
    t->parked.store(1);
    futex_wake(&t->parked, 1);
    while (int32_t c = t->suspend_count.load()) futex_wait(&t->suspend_count, c);
    t->parked.store(0);
}

void install_suspend_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa = {};
        sa.sa_sigaction = suspend_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(kSuspendSignal, &sa, nullptr);
    });
}

void* thread_trampoline(void* arg) {
    auto* holder = static_cast<std::shared_ptr<Thread>*>(arg);
    t_self = std::move(*holder);
    delete holder;
    Thread* t = t_self.get();
    t_self_raw = t;
    t->tid = current_tid();
    register_thread(t_self);

    pthread_attr_t attr;
    void* stack_lo;
    size_t stack_size;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &stack_lo, &stack_size);
    pthread_attr_destroy(&attr);
    loader::attach_thread(stack_lo, static_cast<uint8_t*>(stack_lo) + stack_size);

    // Tell CreateThread our tid, then honor CREATE_SUSPENDED.
    t->started.store(1);
    futex_wake(&t->started, 1);
    while (int32_t c = t->suspend_count.load()) futex_wait(&t->suspend_count, c);

    FL_LOG(thread, "thread %u start %p(%p)", t->tid.load(), reinterpret_cast<void*>(t->start), t->param);
    DWORD rc = t->start(t->param);
    FL_LOG(thread, "thread %u exited with %u", t->tid.load(), rc);

    {
        std::lock_guard lock(wait_lock());
        t->exit_code = rc;
        t->exited = true;
        t->wake_all();
    }
    loader::detach_thread();
    t_self_raw = nullptr;
    t_self.reset();
    return nullptr;
}

// ---------------------------------------------------------------- threads

WINAPI HANDLE kx_CreateThread(void* sa, SIZE_T stack, ThreadStart start, void* param, DWORD flags, DWORD* out_tid) {
    install_suspend_handler();
    auto t = std::make_shared<Thread>();
    t->start = start;
    t->param = param;
    clock_gettime(CLOCK_REALTIME, &t->created);
    if (flags & CREATE_SUSPENDED) t->suspend_count = 1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>(stack, kMinThreadStack));
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    auto* holder = new std::shared_ptr<Thread>(t);
    if (pthread_create(&pt, &attr, thread_trampoline, holder) != 0) {
        delete holder;
        pthread_attr_destroy(&attr);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    pthread_attr_destroy(&attr);
    while (!t->started.load()) futex_wait(&t->started, 0);

    if (out_tid) *out_tid = t->tid;
    FL_LOG(thread, "CreateThread -> tid %u entry %p stack 0x%llx flags 0x%x", t->tid.load(),
           reinterpret_cast<void*>(start), static_cast<unsigned long long>(stack), flags);
    return handle_add(t);
}

WINAPI HANDLE kx_GetCurrentThread() { return kCurrentThread; }
WINAPI HANDLE kx_GetCurrentProcess() { return kCurrentProcess; }
WINAPI DWORD kx_GetCurrentThreadId() { return current_tid(); }

WINAPI HANDLE kx_OpenThread(DWORD access, BOOL inherit, DWORD tid) {
    std::shared_ptr<Thread> t;
    {
        std::lock_guard lock(g_threads_mutex);
        auto it = g_threads.find(tid);
        if (it != g_threads.end()) t = it->second.lock();
    }
    if (!t) return set_last_error(ERROR_INVALID_PARAMETER), nullptr;
    return handle_add(t);
}

WINAPI DWORD kx_ResumeThread(HANDLE h) {
    auto t = thread_from_handle(h);
    if (!t) return set_last_error(ERROR_INVALID_HANDLE), static_cast<DWORD>(-1);
    int32_t prev = t->suspend_count.load();
    if (prev > 0 && t->suspend_count.fetch_sub(1) == 1) futex_wake(&t->suspend_count, 1);
    return static_cast<DWORD>(prev);
}

WINAPI DWORD kx_SuspendThread(HANDLE h) {
    auto t = thread_from_handle(h);
    if (!t) return set_last_error(ERROR_INVALID_HANDLE), static_cast<DWORD>(-1);
    if (t.get() == t_self_raw) {
        FL_ERROR("SuspendThread on the current thread is not supported");
        return static_cast<DWORD>(-1);
    }
    int32_t prev = t->suspend_count.fetch_add(1);
    if (prev == 0) {
        bool exited;
        {
            std::lock_guard lock(wait_lock());
            exited = t->exited;
        }
        if (!exited && syscall(SYS_tgkill, getpid(), t->tid.load(), kSuspendSignal) == 0)
            while (!t->parked.load() && t->suspend_count.load() > 0) futex_wait(&t->parked, 0);
    }
    return static_cast<DWORD>(prev);
}

WINAPI BOOL kx_GetExitCodeThread(HANDLE h, DWORD* code) {
    auto t = thread_from_handle(h);
    if (!t) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    std::lock_guard lock(wait_lock());
    *code = t->exit_code;
    return TRUE_;
}

WINAPI BOOL kx_SetThreadPriority(HANDLE h, int prio) {
    auto t = thread_from_handle(h);
    if (!t) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    t->priority = prio;
    FL_LOG(thread, "SetThreadPriority tid %u -> %d", t->tid.load(), prio);
    return TRUE_;
}

WINAPI int kx_GetThreadPriority(HANDLE h) {
    auto t = thread_from_handle(h);
    return t ? t->priority : 0x7FFFFFFF /* THREAD_PRIORITY_ERROR_RETURN */;
}

// The Xbox One gives games 6 cores (later 7). Report 6, but never actually pin.
constexpr DWORD_PTR kAffinityMask = 0x3F;

WINAPI DWORD_PTR kx_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    FL_LOG(thread, "SetThreadAffinityMask 0x%llx (ignored)", static_cast<unsigned long long>(mask));
    return kAffinityMask;
}

WINAPI BOOL kx_SetProcessAffinityMask(HANDLE h, DWORD_PTR mask) { return TRUE_; }

WINAPI BOOL kx_GetProcessAffinityMask(HANDLE h, DWORD_PTR* process, DWORD_PTR* system) {
    if (process) *process = kAffinityMask;
    if (system) *system = kAffinityMask;
    return TRUE_;
}

struct ProcessorNumber {
    WORD Group;
    BYTE Number;
    BYTE Reserved;
};

WINAPI BOOL kx_SetThreadIdealProcessorEx(HANDLE h, ProcessorNumber* ideal, ProcessorNumber* prev) {
    if (prev) *prev = {0, 0, 0};
    return TRUE_;
}

uint64_t to_filetime_100ns(const timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 10000000ull + ts.tv_nsec / 100;
}

WINAPI BOOL kx_GetThreadTimes(HANDLE h, FILETIME* creation, FILETIME* exit, FILETIME* kernel, FILETIME* user) {
    auto t = thread_from_handle(h);
    if (!t) return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    auto put = [](FILETIME* f, uint64_t v) {
        if (f) *f = {static_cast<DWORD>(v), static_cast<DWORD>(v >> 32)};
    };
    constexpr uint64_t kEpochDelta = 116444736000000000ull;  // 1601 -> 1970
    put(creation, to_filetime_100ns(t->created) + kEpochDelta);
    put(exit, 0);
    put(kernel, 0);
    timespec cpu{};
    if (t.get() == t_self_raw || h == kCurrentThread) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    put(user, to_filetime_100ns(cpu));
    return TRUE_;
}

// ---------------------------------------------------------------- TLS slots
// TlsGetValue/TlsSetValue are not imported: the game reads TEB.TlsSlots
// directly, so slots must live there.

std::mutex g_tls_mutex;
bool g_tls_used[loader::kTlsMinimumSlots];

WINAPI DWORD kx_TlsAlloc() {
    std::lock_guard lock(g_tls_mutex);
    for (int i = 0; i < loader::kTlsMinimumSlots; i++) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = true;
            FL_LOG(thread, "TlsAlloc -> %d", i);
            return static_cast<DWORD>(i);
        }
    }
    FL_ERROR("TlsAlloc: out of slots");
    return 0xFFFFFFFF;  // TLS_OUT_OF_INDEXES
}

// ---------------------------------------------------------------- fibers
// TEB.FiberData points at the current fiber; its first field is the fiber
// parameter (the game's inlined GetFiberData() reads it directly).

using FiberStart = void(WINAPI*)(void* param);

struct Fiber {
    void* param;  // must be first
    void* stack_base = nullptr;
    void* stack_limit = nullptr;
    uint64_t rsp = 0;
    void* mem = nullptr;
    size_t mem_size = 0;
    FiberStart start = nullptr;
};

Fiber* current_fiber() {
    return static_cast<Fiber*>(loader::current_teb()->field<void*>(FiberData));
}

WINAPI void* kx_ConvertThreadToFiber(void* param) {
    auto* teb = loader::current_teb();
    auto* f = new Fiber{param};
    f->stack_base = teb->field<void*>(StackBase);
    f->stack_limit = teb->field<void*>(StackLimit);
    teb->field<void*>(FiberData) = f;
    FL_LOG(fiber, "ConvertThreadToFiber -> %p", static_cast<void*>(f));
    return f;
}

WINAPI BOOL kx_ConvertFiberToThread() {
    auto* teb = loader::current_teb();
    delete current_fiber();
    teb->field<void*>(FiberData) = nullptr;
    return TRUE_;
}

WINAPI void* kx_CreateFiber(SIZE_T stack, FiberStart start, void* param) {
    size_t size = (std::max<size_t>(stack ? stack : (1u << 20), 64u << 10) + 0xFFFF) & ~size_t(0xFFFF);
    size_t guard = 4096;
    auto* mem = static_cast<uint8_t*>(
        mmap(nullptr, size + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
    if (mem == MAP_FAILED) return set_last_error(ERROR_NOT_ENOUGH_MEMORY), nullptr;
    mprotect(mem, guard, PROT_NONE);

    auto* f = new Fiber{param};
    f->mem = mem;
    f->mem_size = size + guard;
    f->stack_limit = mem + guard;
    f->stack_base = mem + guard + size;
    f->start = start;

    // Build the initial frame fl_fiber_switch will "return" through.
    auto* top = reinterpret_cast<uint64_t*>(f->stack_base);
    uint64_t* sp = top - 2;  // keep some slack at the very top
    *--sp = reinterpret_cast<uint64_t>(&fl_fiber_entry);  // return address
    for (int i = 0; i < 8; i++) *--sp = 0;                 // rbx rbp rdi rsi r12 r13 r14 r15
    sp[3] = reinterpret_cast<uint64_t>(f);                  // r12 slot (pushed 5th, popped 4th)
    sp -= 176 / 8;
    memset(sp, 0, 176);
    reinterpret_cast<uint32_t*>(sp)[40] = 0x1F80;  // mxcsr at +160
    reinterpret_cast<uint16_t*>(sp)[82] = 0x027F;  // x87 cw at +164
    f->rsp = reinterpret_cast<uint64_t>(sp);
    FL_LOG(fiber, "CreateFiber %p entry %p stack %zu", static_cast<void*>(f), reinterpret_cast<void*>(start), size);
    return f;
}

WINAPI void kx_SwitchToFiber(void* target) {
    auto* teb = loader::current_teb();
    auto* cur = current_fiber();
    auto* next = static_cast<Fiber*>(target);
    if (cur == next) return;
    if (!cur) fl::fatal("SwitchToFiber from a thread that is not a fiber");
    cur->stack_base = teb->field<void*>(StackBase);
    cur->stack_limit = teb->field<void*>(StackLimit);
    teb->field<void*>(StackBase) = next->stack_base;
    teb->field<void*>(StackLimit) = next->stack_limit;
    teb->field<void*>(DeallocationStack) = next->stack_limit;
    teb->field<void*>(FiberData) = next;
    fl_fiber_switch(&cur->rsp, next->rsp);
}

WINAPI void kx_DeleteFiber(void* fiber) {
    auto* f = static_cast<Fiber*>(fiber);
    if (f == current_fiber()) fl::fatal("DeleteFiber on the running fiber (would exit thread): not supported");
    if (f->mem) munmap(f->mem, f->mem_size);
    delete f;
}

}  // namespace

struct HostThreadArgs {
    void (*fn)(void*);
    void* arg;
};

WINAPI DWORD host_thread_tramp(void* p) {
    auto* a = static_cast<HostThreadArgs*>(p);
    auto fn = a->fn;
    auto arg = a->arg;
    delete a;
    fn(arg);
    return 0;
}

HANDLE spawn_game_thread(void (*fn)(void*), void* arg) {
    return kx_CreateThread(nullptr, 0, host_thread_tramp, new HostThreadArgs{fn, arg}, 0, nullptr);
}

extern "C" [[noreturn]] void fl_fiber_main(Fiber* f) {
    f->start(f->param);
    fl::fatal("fiber %p returned from its start routine (Windows would exit the thread)", static_cast<void*>(f));
}

HLE_EXPORT(kernelx, CreateThread, kx_CreateThread);
HLE_EXPORT(kernelx, GetCurrentThread, kx_GetCurrentThread);
HLE_EXPORT(kernelx, GetCurrentProcess, kx_GetCurrentProcess);
HLE_EXPORT(kernelx, GetCurrentThreadId, kx_GetCurrentThreadId);
HLE_EXPORT(kernelx, OpenThread, kx_OpenThread);
HLE_EXPORT(kernelx, ResumeThread, kx_ResumeThread);
HLE_EXPORT(kernelx, SuspendThread, kx_SuspendThread);
HLE_EXPORT(kernelx, GetExitCodeThread, kx_GetExitCodeThread);
HLE_EXPORT(kernelx, SetThreadPriority, kx_SetThreadPriority);
HLE_EXPORT(kernelx, GetThreadPriority, kx_GetThreadPriority);
HLE_EXPORT(kernelx, SetThreadAffinityMask, kx_SetThreadAffinityMask);
HLE_EXPORT(kernelx, SetProcessAffinityMask, kx_SetProcessAffinityMask);
HLE_EXPORT(kernelx, GetProcessAffinityMask, kx_GetProcessAffinityMask);
HLE_EXPORT(kernelx, SetThreadIdealProcessorEx, kx_SetThreadIdealProcessorEx);
HLE_EXPORT(kernelx, GetThreadTimes, kx_GetThreadTimes);
HLE_EXPORT(kernelx, TlsAlloc, kx_TlsAlloc);
HLE_EXPORT(kernelx, ConvertThreadToFiber, kx_ConvertThreadToFiber);
HLE_EXPORT(kernelx, ConvertFiberToThread, kx_ConvertFiberToThread);
HLE_EXPORT(kernelx, CreateFiber, kx_CreateFiber);
HLE_EXPORT(kernelx, SwitchToFiber, kx_SwitchToFiber);
HLE_EXPORT(kernelx, DeleteFiber, kx_DeleteFiber);

}  // namespace fl::kx
