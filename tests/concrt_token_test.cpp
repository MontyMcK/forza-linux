// Checks the PPL cancellation-token registration reference counts against
// msvcr110 (FINDINGS §5c): _RegisterCallback(proc, data, 1) leaves 2
// references (the caller's and the list's), and the caller drops its own after
// _DeregisterCallback, the way the game's inlined ppltasks code does.
#include <cstdint>
#include <cstdio>
#include <thread>

#include "common/winabi.h"
#include "hle/crt/crt_cxx.h"
#include "hle/kernelx/kobject.h"
#include "hle/registry.h"
#include "hle/seh/seh.h"

// What concrt.cpp needs from the rest of the runtime (not used by these paths).
namespace fl::seh {
bool guarded_call(void(WINAPI* fn)(void*), void* arg) {
    fn(arg);
    return true;
}
}  // namespace fl::seh
namespace fl::kx {
HANDLE spawn_game_thread(void (*fn)(void*), void* arg) {
    std::thread(fn, arg).detach();
    return nullptr;
}
}  // namespace fl::kx
namespace fl::crt {
void std_exception_init(StdException* e, const char* msg) { e->what = msg; }
}  // namespace fl::crt

namespace {

using TaskProc = void(WINAPI*)(void*);
using NewToken = void*(WINAPI*)();
using Cancel = void(WINAPI*)(void* token);
using Register = void*(WINAPI*)(void* token, TaskProc, void* data, int initial_refs);
using Deregister = void(WINAPI*)(void* token, void* reg);

int g_failed = 0;
int g_calls = 0;

WINAPI void callback(void*) { g_calls++; }

int32_t refs(void* o) { return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(o) + 8); }

// The inlined _RefCounter::_Release: lock dec [o+8], vfptr[1] (_Destroy) at zero.
void release(void* o) {
    auto* r = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(o) + 8);
    if (__atomic_sub_fetch(r, 1, __ATOMIC_SEQ_CST) == 0)
        reinterpret_cast<void(WINAPI*)(void*)>((*static_cast<void***>(o))[1])(o);
}

void expect(bool ok, const char* what) {
    g_failed += !ok;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

template <class T>
T fn(const char* name) {
    return reinterpret_cast<T>(fl::hle::lookup("msvcr110", name));
}

}  // namespace

int main() {
    auto new_token = fn<NewToken>("?_NewTokenState@_CancellationTokenState@details@Concurrency@@SAPEAV123@XZ");
    auto cancel = fn<Cancel>("?_Cancel@_CancellationTokenState@details@Concurrency@@QEAAXXZ");
    auto reg = fn<Register>(
        "?_RegisterCallback@_CancellationTokenState@details@Concurrency@@QEAAPEAV_CancellationTokenRegistration@23@"
        "P6AXPEAX@Z0H@Z");
    auto dereg = fn<Deregister>(
        "?_DeregisterCallback@_CancellationTokenState@details@Concurrency@@QEAAXPEAV_CancellationTokenRegistration@23@"
        "@Z");
    if (!new_token || !cancel || !reg || !dereg) {
        std::printf("FAIL  ConcRT token exports not registered\n");
        return 1;
    }

    // A task that finishes normally: deregister, then the caller's release.
    void* t = new_token();
    void* r = reg(t, &callback, nullptr, 1);
    expect(refs(r) == 2, "registered: the caller's reference and the list's");
    dereg(t, r);
    expect(refs(r) == 1, "deregistered: the caller's reference is left");
    expect(g_calls == 0, "deregistered: the callback never ran");
    release(r);
    release(t);

    // A canceled task: the cancel runs the callback and drops the list's reference.
    g_calls = 0;
    t = new_token();
    r = reg(t, &callback, nullptr, 1);
    cancel(t);
    expect(g_calls == 1 && refs(r) == 1, "canceled: callback ran once, the caller's reference is left");
    dereg(t, r);
    expect(refs(r) == 1, "deregistered after the cancel: still the caller's reference");
    release(r);

    // Registering on a canceled token runs the callback at once.
    g_calls = 0;
    r = reg(t, &callback, nullptr, 1);
    expect(g_calls == 1 && refs(r) == 1, "already canceled: callback ran at once, the caller's reference is left");
    dereg(t, r);
    release(r);
    release(t);

    // A token released with registrations still listed drops only the list's.
    t = new_token();
    r = reg(t, &callback, nullptr, 1);
    release(t);
    expect(refs(r) == 1, "token destroyed: the caller's reference is left");
    release(r);

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
