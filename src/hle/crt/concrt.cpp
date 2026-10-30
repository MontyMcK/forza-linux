// msvcr110 / msvcp110: the parts of the Concurrency Runtime (ConcRT/PPL) the
// game imports.
//
// We only keep state in the first bytes of the game-allocated objects
// (critical_section, event, scoped_lock), whose real sizes are larger.
// PPL tasks (ppltasks.h, inlined in the game and Microsoft.Xbox.Services.dll)
// use cancellation tokens and _AsyncTaskCollection; their layouts come from
// the reference msvcr110 (FINDINGS §6b).

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/crt/crt_cxx.h"
#include "hle/kernelx/futex_lock.h"
#include "hle/kernelx/kobject.h"
#include "hle/registry.h"
#include "hle/seh/seh.h"

namespace fl::crt {
namespace {

using kx::futex_lock;
using kx::futex_unlock;
using kx::futex_wait;
using kx::futex_wake;

// ---------------------------------------------------------------- critical_section (non-recursive)

struct CritSec {
    std::atomic<int32_t> lock;
};

WINAPI CritSec* cs_ctor(CritSec* self) {
    self->lock.store(0);
    return self;
}
WINAPI void cs_dtor(CritSec* self) {}

struct ScopedLock {
    CritSec* cs;
};

WINAPI ScopedLock* sl_ctor(ScopedLock* self, CritSec* cs) {
    self->cs = cs;
    futex_lock(&cs->lock, 200);
    return self;
}

WINAPI void sl_dtor(ScopedLock* self) { futex_unlock(&self->cs->lock); }

// ---------------------------------------------------------------- event (manual reset)

struct Event {
    std::atomic<int32_t> state;
};

WINAPI Event* ev_ctor(Event* self) {
    self->state.store(0);
    return self;
}
WINAPI void ev_dtor(Event* self) {}

WINAPI void ev_set(Event* self) {
    self->state.store(1);
    futex_wake(&self->state, INT32_MAX);
}

// size_t event::wait(unsigned int timeout): 0 on success, COOPERATIVE_WAIT_TIMEOUT on timeout.
WINAPI size_t ev_wait(Event* self, unsigned timeout) {
    if (timeout == 0xFFFFFFFF) {
        while (!self->state.load()) futex_wait(&self->state, 0);
        return 0;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (!self->state.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return SIZE_MAX;
        os::futex_wait(&self->state, 0, 1000000);  // poll in 1 ms steps; timeouts are rare
    }
    return 0;
}

// ---------------------------------------------------------------- _SpinLock(long volatile& flag)

struct SpinLock {
    std::atomic<int32_t>* flag;
};

WINAPI SpinLock* spin_ctor(SpinLock* self, std::atomic<int32_t>* flag) {
    self->flag = flag;
    int32_t expected = 0;
    while (!flag->compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        expected = 0;
        __builtin_ia32_pause();
    }
    return self;
}

WINAPI void spin_dtor(SpinLock* self) { self->flag->store(0, std::memory_order_release); }

// ---------------------------------------------------------------- allocation

WINAPI void* cc_alloc(size_t n) { return malloc(n ? n : 1); }
WINAPI void cc_free(void* p) { free(p); }

// ---------------------------------------------------------------- scheduler

using TaskProc = void(WINAPI*)(void*);

struct Work {
    TaskProc proc;
    void* data;
};

struct Pool {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Work> queue;
};

Pool& pool() {
    static Pool* p = [] {
        auto* p = new Pool();
        constexpr int kWorkers = 6;  // the Xbox One game cores
        for (int i = 0; i < kWorkers; i++)
            kx::spawn_game_thread(
                [](void* arg) {
                    auto* pl = static_cast<Pool*>(arg);
                    for (;;) {
                        Work w;
                        {
                            std::unique_lock lock(pl->m);
                            pl->cv.wait(lock, [&] { return !pl->queue.empty(); });
                            w = pl->queue.front();
                            pl->queue.pop_front();
                        }
                        w.proc(w.data);
                    }
                },
                p);
        FL_LOG(concrt, "ConcRT worker pool started (%d workers)", kWorkers);
        return p;
    }();
    return *p;
}

WINAPI void cc_schedule_task(TaskProc proc, void* data) {
    Pool& p = pool();
    {
        std::lock_guard lock(p.m);
        p.queue.push_back({proc, data});
    }
    p.cv.notify_one();
}

WINAPI void cc_oversubscribe(bool begin) {}

std::atomic<unsigned> g_async_id{0};
WINAPI unsigned cc_get_next_async_id() { return ++g_async_id; }

thread_local size_t t_inline_depth = 0;
WINAPI size_t* cc_get_current_inline_depth() { return &t_inline_depth; }

// ---------------------------------------------------------------- PPL: cancellation tokens
// _RefCounter {vfptr: 0 deleting dtor, 1 _Destroy; long refs @+8}. The inlined
// code calls _Reference/_Release itself (Release -> vfptr[1] at zero).
// _CancellationTokenState: state flag @+0x10 (0 none, 1 canceling, 2
// canceled; inlined _IsCanceled reads it). (_CancellationTokenState*)2 is
// _None(). Everything after +0x14 is ours: only we allocate these.

template <class T>
void refcount_release(T* o) {
    if (o->refs.fetch_sub(1) == 1) reinterpret_cast<void(WINAPI*)(T*)>(o->vt[1])(o);
}

struct TokenState;
struct Registration {  // _CancellationTokenRegistration made by _RegisterCallback(TaskProc...)
    void** vt;
    std::atomic<int32_t> refs;
    int32_t pad;
    TokenState* token;          // +0x10
    std::atomic<int32_t> state;  // 0 registered, 1 running, 2 done/removed
    TaskProc proc;
    void* data;
};

struct TokenState {
    void** vt;
    std::atomic<int32_t> refs;
    int32_t pad;
    std::atomic<int32_t> state;  // +0x10
    std::mutex m;
    std::vector<Registration*> regs;
};
static_assert(offsetof(TokenState, refs) == 8 && offsetof(TokenState, state) == 0x10);

bool token_valid(TokenState* t) { return reinterpret_cast<uintptr_t>(t) > 2; }

WINAPI void* reg_delete(Registration* r, unsigned flags) {
    if (flags & 1) delete r;
    return r;
}
WINAPI void reg_destroy(Registration* r) { delete r; }
void* g_reg_vt[] = {(void*)&reg_delete, (void*)&reg_destroy};

WINAPI void* token_delete(TokenState* t, unsigned flags) {
    if (flags & 1) delete t;
    return t;
}
WINAPI void token_destroy(TokenState* t) {
    for (Registration* r : t->regs) refcount_release(r);  // the list's references
    delete t;
}
void* g_token_vt[] = {(void*)&token_delete, (void*)&token_destroy};

WINAPI TokenState* token_new() {
    auto* t = new TokenState();
    t->vt = g_token_vt;
    t->refs = 1;
    t->state = 0;
    return t;
}

void reg_invoke(Registration* r) {
    int32_t expected = 0;
    if (!r->state.compare_exchange_strong(expected, 1)) return;
    r->proc(r->data);
    r->state = 2;
    refcount_release(r);  // the list's reference
}

WINAPI void token_cancel(TokenState* t) {
    int32_t expected = 0;
    if (!t->state.compare_exchange_strong(expected, 1)) return;
    std::vector<Registration*> regs;
    {
        std::lock_guard lock(t->m);
        regs.swap(t->regs);
    }
    for (Registration* r : regs) reg_invoke(r);
    t->state = 2;
}

// _RegisterCallback(TaskProc, void* data, int initialRefs): initialRefs counts
// the caller's reference (released by inlined code) plus the list's.
WINAPI Registration* token_register(TokenState* t, TaskProc proc, void* data, int initial_refs) {
    auto* r = new Registration();
    r->vt = g_reg_vt;
    r->refs = initial_refs;
    r->token = t;
    r->state = 0;
    r->proc = proc;
    r->data = data;
    bool run_now = true;
    if (t->state == 0) {
        std::lock_guard lock(t->m);
        if (t->state == 0) {
            t->regs.push_back(r);
            run_now = false;
        }
    }
    if (run_now) reg_invoke(r);
    return r;
}

WINAPI void token_deregister(TokenState* t, Registration* r) {
    bool removed = false;
    {
        std::lock_guard lock(t->m);
        for (auto it = t->regs.begin(); it != t->regs.end(); ++it)
            if (*it == r) {
                t->regs.erase(it);
                removed = true;
                break;
            }
    }
    if (removed) {
        int32_t expected = 0;
        if (r->state.compare_exchange_strong(expected, 2)) refcount_release(r);
        return;
    }
    // Being invoked by a cancel on another thread: wait for it to finish.
    while (r->state == 1) std::this_thread::yield();
}

// ---------------------------------------------------------------- PPL: task collections
// _AsyncTaskCollection (0xf8 in msvcr110): _RefCounter + _TaskCollection @+0x10.
// Inlined ppltasks code calls _Schedule/_RunAndWait/_Cancel with this+0x10 and
// does _Reference() before scheduling; the collection's completion (all chores
// done) does the matching _Release(). _UnrealizedChore: vfptr (0 = deleting
// dtor), TaskProc m_pFunction @+8, owning collection @+0x10, chore wrapper
// @+0x18, bool runtime-owns-lifetime @+0x20.

struct AsyncCollection;
struct TaskCollection {
    AsyncCollection* owner;
    std::mutex m;
    std::condition_variable cv;
    int pending = 0;
    std::atomic<bool> canceled{false};
};

struct AsyncCollection {
    void** vt;
    std::atomic<int32_t> refs;
    int32_t pad;
    TaskCollection col;  // +0x10
};
static_assert(offsetof(AsyncCollection, col) == 0x10);

WINAPI void* acol_delete(AsyncCollection* a, unsigned flags) {
    if (flags & 1) delete a;
    return a;
}
WINAPI void acol_destroy(AsyncCollection* a) { delete a; }
void* g_acol_vt[] = {(void*)&acol_delete, (void*)&acol_destroy};

WINAPI void col_cancel_cb(void* col) { static_cast<TaskCollection*>(col)->canceled = true; }

WINAPI AsyncCollection* acol_new(TokenState* token) {
    auto* a = new AsyncCollection();
    a->vt = g_acol_vt;
    a->refs = 1;
    a->col.owner = a;
    if (token_valid(token)) {
        if (token->state != 0) a->col.canceled = true;
        // STUB: the registration is never removed; tokens outliving their
        // collections would call back into freed memory. ppltasks passes _None().
        FL_STUB_ONCE("_AsyncTaskCollection with a real cancellation token");
        token_register(token, &col_cancel_cb, &a->col, 1);
    }
    return a;
}

void chore_run(TaskCollection* c, void* chore) {
    auto* ch = static_cast<uint8_t*>(chore);
    // ConcRT runs chores inside a catch(...): ppltasks has already stored the
    // exception in the task and rethrows it for the runtime to swallow.
    if (!c->canceled && !seh::guarded_call(*reinterpret_cast<TaskProc*>(ch + 8), chore))
        FL_LOG(concrt, "chore %p ended with an exception (stored in its task)", chore);
    if (ch[0x20]) reinterpret_cast<void*(WINAPI*)(void*, unsigned)>((*reinterpret_cast<void***>(chore))[0])(chore, 1);
    bool done;
    {
        std::lock_guard lock(c->m);
        done = --c->pending == 0;
    }
    if (done) {
        c->cv.notify_all();
        refcount_release(c->owner);  // the completion handler's _Release
    }
}

void chore_claim(TaskCollection* c, void* chore) {
    auto** owner = reinterpret_cast<TaskCollection**>(static_cast<uint8_t*>(chore) + 0x10);
    if (*owner) fl::fatal("ConcRT: chore scheduled twice (invalid_multiple_scheduling)");
    *owner = c;
    std::lock_guard lock(c->m);
    c->pending++;
}

struct ChoreJob {
    TaskCollection* col;
    void* chore;
};
WINAPI void chore_job(void* p) {
    auto* j = static_cast<ChoreJob*>(p);
    chore_run(j->col, j->chore);
    delete j;
}

WINAPI void col_schedule(TaskCollection* c, void* chore) {
    chore_claim(c, chore);
    cc_schedule_task(&chore_job, new ChoreJob{c, chore});
}

// _TaskCollectionStatus: 1 completed, 2 canceled.
WINAPI int col_run_and_wait(TaskCollection* c, void* chore) {
    if (chore) {
        chore_claim(c, chore);
        chore_run(c, chore);
    }
    std::unique_lock lock(c->m);
    c->cv.wait(lock, [&] { return c->pending == 0; });
    return c->canceled ? 2 : 1;
}

WINAPI void col_cancel(TaskCollection* c) { c->canceled = true; }

// The real msvcr110 calls the registered handler, then __fastfail(5). Offline,
// Xbox Live tasks the game drops without get() fail, which a connected console
// never sees; like the Azimuth reference we report it and carry on.
WINAPI void report_unobserved_exception() {
    FL_ERROR("ConcRT: a PPL task's exception was never observed (called from %p); continuing",
             __builtin_return_address(0));
}

WINAPI void cc_wait(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------- concurrent_queue base
// Layout (verified from the game's derived classes, FINDINGS §6):
//   { vfptr; Rep* rep; size_t items_per_page; size_t item_size; }
// Derived vtable: 0 _Move_item(page&, idx, src) 1 _Copy_item(page&, idx, src)
//   2 _Assign_and_destroy_item(dst, page&, idx) 3 deleting dtor
//   4 _Allocate_page() 5 _Deallocate_page(page*)
// We store one item per page (index 0) in a locked deque.

struct QueueRep {
    std::mutex m;
    std::deque<void*> pages;
};

struct QueueBase {
    void** vft;
    QueueRep* rep;
    size_t items_per_page;
    size_t item_size;
};

using MoveCopyFn = void(WINAPI*)(QueueBase*, void* page, size_t idx, const void* src);
using AssignFn = void(WINAPI*)(QueueBase*, void* dst, void* page, size_t idx);
using AllocPageFn = void*(WINAPI*)(QueueBase*);
using FreePageFn = void(WINAPI*)(QueueBase*, void* page);

WINAPI QueueBase* q_ctor(QueueBase* self, size_t item_size) {
    self->rep = new QueueRep();
    self->item_size = item_size;
    self->items_per_page = item_size <= 8 ? 32 : item_size <= 16 ? 16 : item_size <= 32 ? 8
                         : item_size <= 64 ? 4 : item_size <= 128 ? 2 : 1;
    return self;
}

WINAPI void q_dtor(QueueBase* self) {
    delete self->rep;
    self->rep = nullptr;
}

void q_push(QueueBase* self, const void* src, int slot) {
    void* page = reinterpret_cast<AllocPageFn>(self->vft[4])(self);
    reinterpret_cast<MoveCopyFn>(self->vft[slot])(self, page, 0, src);
    std::lock_guard lock(self->rep->m);
    self->rep->pages.push_back(page);
}

WINAPI void q_internal_push(QueueBase* self, const void* src) { q_push(self, src, 1); }
WINAPI void q_internal_move_push(QueueBase* self, void* src) { q_push(self, src, 0); }

WINAPI bool q_pop_if_present(QueueBase* self, void* dst) {
    void* page;
    {
        std::lock_guard lock(self->rep->m);
        if (self->rep->pages.empty()) return false;
        page = self->rep->pages.front();
        self->rep->pages.pop_front();
    }
    reinterpret_cast<AssignFn>(self->vft[2])(self, dst, page, 0);
    reinterpret_cast<FreePageFn>(self->vft[5])(self, page);
    return true;
}

WINAPI bool q_empty(const QueueBase* self) {
    std::lock_guard lock(self->rep->m);
    return self->rep->pages.empty();
}

WINAPI void q_finish_clear(QueueBase* self) {
    std::deque<void*> left;
    {
        std::lock_guard lock(self->rep->m);
        left.swap(self->rep->pages);
    }
    if (!left.empty()) FL_ERROR("concurrent_queue cleared with %zu items left (not destroyed)", left.size());
    for (void* p : left) reinterpret_cast<FreePageFn>(self->vft[5])(self, p);
}

[[noreturn]] WINAPI void q_throw(const QueueBase* self) { fl::fatal("concurrent_queue: _Internal_throw_exception"); }

// ---------------------------------------------------------------- exceptions

// Concurrency::task_canceled and invalid_operation derive from std::exception
// and add no state; their own vtables only matter for what()/destruction, so
// they share std::exception's.

WINAPI StdException* task_canceled_ctor(StdException* self) {
    std_exception_init(self, nullptr);
    return self;
}

WINAPI StdException* invalid_operation_ctor(StdException* self, const char* msg) {
    std_exception_init(self, msg);
    return self;
}

}  // namespace

HLE_EXPORT_AS(msvcr110, "?_NewTokenState@_CancellationTokenState@details@Concurrency@@SAPEAV123@XZ", token_new);
HLE_EXPORT_AS(msvcr110, "?_Cancel@_CancellationTokenState@details@Concurrency@@QEAAXXZ", token_cancel);
HLE_EXPORT_AS(msvcr110,
              "?_RegisterCallback@_CancellationTokenState@details@Concurrency@@QEAAPEAV_CancellationTokenRegistration@23@P6AXPEAX@Z0H@Z",
              token_register);
HLE_EXPORT_AS(msvcr110,
              "?_DeregisterCallback@_CancellationTokenState@details@Concurrency@@QEAAXPEAV_CancellationTokenRegistration@23@@Z",
              token_deregister);
HLE_EXPORT_AS(msvcr110,
              "?_NewCollection@_AsyncTaskCollection@details@Concurrency@@SAPEAV123@PEAV_CancellationTokenState@23@@Z",
              acol_new);
HLE_EXPORT_AS(msvcr110, "?_Schedule@_TaskCollection@details@Concurrency@@QEAAXPEAV_UnrealizedChore@23@@Z", col_schedule);
HLE_EXPORT_AS(msvcr110,
              "?_RunAndWait@_TaskCollection@details@Concurrency@@QEAA?AW4_TaskCollectionStatus@23@PEAV_UnrealizedChore@23@@Z",
              col_run_and_wait);
HLE_EXPORT_AS(msvcr110, "?_Cancel@_TaskCollection@details@Concurrency@@QEAAXXZ", col_cancel);
HLE_EXPORT_AS(msvcr110, "?_ReportUnobservedException@details@Concurrency@@YAXXZ", report_unobserved_exception);
HLE_EXPORT_AS(msvcr110, "?wait@Concurrency@@YAXI@Z", cc_wait);
HLE_EXPORT_AS(msvcr110, "??0critical_section@Concurrency@@QEAA@XZ", cs_ctor);
HLE_EXPORT_AS(msvcr110, "??1critical_section@Concurrency@@QEAA@XZ", cs_dtor);
HLE_EXPORT_AS(msvcr110, "??0scoped_lock@critical_section@Concurrency@@QEAA@AEAV12@@Z", sl_ctor);
HLE_EXPORT_AS(msvcr110, "??1scoped_lock@critical_section@Concurrency@@QEAA@XZ", sl_dtor);
HLE_EXPORT_AS(msvcr110, "??0event@Concurrency@@QEAA@XZ", ev_ctor);
HLE_EXPORT_AS(msvcr110, "??1event@Concurrency@@QEAA@XZ", ev_dtor);
HLE_EXPORT_AS(msvcr110, "?set@event@Concurrency@@QEAAXXZ", ev_set);
HLE_EXPORT_AS(msvcr110, "?wait@event@Concurrency@@QEAA_KI@Z", ev_wait);
HLE_EXPORT_AS(msvcr110, "??0_SpinLock@details@Concurrency@@QEAA@AECJ@Z", spin_ctor);
HLE_EXPORT_AS(msvcr110, "??1_SpinLock@details@Concurrency@@QEAA@XZ", spin_dtor);
HLE_EXPORT_AS(msvcr110, "?Alloc@Concurrency@@YAPEAX_K@Z", cc_alloc);
HLE_EXPORT_AS(msvcr110, "?Free@Concurrency@@YAXPEAX@Z", cc_free);
HLE_EXPORT_AS(msvcr110, "?_ScheduleTask@_CurrentScheduler@details@Concurrency@@SAXP6AXPEAX@Z0@Z", cc_schedule_task);
HLE_EXPORT_AS(msvcr110, "?_Oversubscribe@_Context@details@Concurrency@@SAX_N@Z", cc_oversubscribe);
HLE_EXPORT_AS(msvcp110, "?_GetNextAsyncId@details@Concurrency@@YAIXZ", cc_get_next_async_id);
HLE_EXPORT_AS(msvcr110, "?_GetCurrentInlineDepth@_StackGuard@details@Concurrency@@CAAEA_KXZ",
              cc_get_current_inline_depth);
HLE_EXPORT_AS(msvcp110, "??0_Concurrent_queue_base_v4@details@Concurrency@@IEAA@_K@Z", q_ctor);
HLE_EXPORT_AS(msvcp110, "??1_Concurrent_queue_base_v4@details@Concurrency@@MEAA@XZ", q_dtor);
HLE_EXPORT_AS(msvcp110, "?_Internal_push@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXPEBX@Z", q_internal_push);
HLE_EXPORT_AS(msvcp110, "?_Internal_move_push@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXPEAX@Z",
              q_internal_move_push);
HLE_EXPORT_AS(msvcp110, "?_Internal_pop_if_present@_Concurrent_queue_base_v4@details@Concurrency@@IEAA_NPEAX@Z",
              q_pop_if_present);
HLE_EXPORT_AS(msvcp110, "?_Internal_empty@_Concurrent_queue_base_v4@details@Concurrency@@IEBA_NXZ", q_empty);
HLE_EXPORT_AS(msvcp110, "?_Internal_finish_clear@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXXZ",
              q_finish_clear);
HLE_EXPORT_AS(msvcp110, "?_Internal_throw_exception@_Concurrent_queue_base_v4@details@Concurrency@@IEBAXXZ",
              q_throw);
HLE_EXPORT_AS(msvcr110, "??0task_canceled@Concurrency@@QEAA@XZ", task_canceled_ctor);
HLE_EXPORT_AS(msvcr110, "??0invalid_operation@Concurrency@@QEAA@PEBD@Z", invalid_operation_ctor);

}  // namespace fl::crt
