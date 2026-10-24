// msvcp110: container/iterator debugging bases, _Lockit, throw helpers,
// misc small functions.
//
// Layouts (MSVC x64, VS2012):
//   _Container_base0  {}                                   (release containers)
//   _Container_base12 { _Container_proxy* _Myproxy; }
//   _Container_proxy  { const _Container_base12* _Mycont; _Iterator_base12* _Myfirstiter; }
//   _Iterator_base12  { _Container_proxy* _Myproxy; _Iterator_base12* _Mynextiter; }

#include <cmath>
#include <cstring>
#include <mutex>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"

namespace fl::crt {
namespace {

// ---------------------------------------------------------------- _Lockit (global recursive locks by kind)

std::recursive_mutex g_lockit[16];

struct Lockit {
    int locktype;
};

WINAPI Lockit* lockit_ctor(Lockit* self, int kind) {
    self->locktype = kind;
    g_lockit[kind & 15].lock();
    return self;
}

WINAPI void lockit_dtor(Lockit* self) { g_lockit[self->locktype & 15].unlock(); }

// ---------------------------------------------------------------- container bases

struct IteratorBase12;
struct ContainerProxy {
    const void* mycont;
    IteratorBase12* first;
};
struct IteratorBase12 {
    ContainerProxy* proxy;
    IteratorBase12* next;
};
struct ContainerBase12 {
    ContainerProxy* proxy;
};

constexpr int kLockDebug = 3;  // _LOCK_DEBUG

WINAPI void cb0_orphan_all(void* self) {}
WINAPI void cb0_swap_all(void* self, void* other) {}

WINAPI ContainerBase12* cb12_ctor(ContainerBase12* self) {
    self->proxy = nullptr;
    return self;
}

WINAPI void cb12_orphan_all(ContainerBase12* self) {
    if (!self->proxy) return;
    std::lock_guard lock(g_lockit[kLockDebug]);
    for (IteratorBase12** it = &self->proxy->first; *it; *it = (*it)->next) (*it)->proxy = nullptr;
    self->proxy->first = nullptr;
}

WINAPI void cb12_dtor(ContainerBase12* self) { cb12_orphan_all(self); }

WINAPI void cb12_swap_all(ContainerBase12* self, ContainerBase12* other) {
    std::lock_guard lock(g_lockit[kLockDebug]);
    std::swap(self->proxy, other->proxy);
    if (self->proxy) self->proxy->mycont = self;
    if (other->proxy) other->proxy->mycont = other;
}

// ---------------------------------------------------------------- throw helpers
// These signal genuine bugs (length/out-of-range errors). We don't have
// ThrowInfo for these std types in our runtime yet, so stop loudly.

[[noreturn]] WINAPI void x_length_error(const char* msg) { fl::fatal("std::length_error: %s", msg); }
[[noreturn]] WINAPI void x_out_of_range(const char* msg) { fl::fatal("std::out_of_range: %s", msg); }
[[noreturn]] WINAPI void x_invalid_argument(const char* msg) { fl::fatal("std::invalid_argument: %s", msg); }
[[noreturn]] WINAPI void x_overflow_error(const char* msg) { fl::fatal("std::overflow_error: %s", msg); }
[[noreturn]] WINAPI void x_bad_alloc() { fl::fatal("std::bad_alloc"); }
[[noreturn]] WINAPI void x_bad_function_call() { fl::fatal("std::bad_function_call"); }
[[noreturn]] WINAPI void x_regex_error(int code) { fl::fatal("std::regex_error %d", code); }

// ---------------------------------------------------------------- misc

const char* error_text(int code) {
    const char* s = strerror(code);
    return s ? s : "unknown error";
}

WINAPI const char* syserror_map(int code) { return error_text(code); }
WINAPI const char* winerror_map(int code) { return nullptr; }  // "no message": callers fall back
WINAPI double xlgamma(double x) { return lgamma(x); }

// float infinity data object (_FInf is a _Dconst-like union; the value is what matters)
union FConst {
    uint32_t w;
    float f;
};
FConst g_finf = {0x7F800000};
int64_t g_badoff = -1;  // std::_BADOFF

}  // namespace

HLE_EXPORT_AS(msvcp110, "??0_Lockit@std@@QEAA@H@Z", lockit_ctor);
HLE_EXPORT_AS(msvcp110, "??1_Lockit@std@@QEAA@XZ", lockit_dtor);
HLE_EXPORT_AS(msvcp110, "?_Orphan_all@_Container_base0@std@@QEAAXXZ", cb0_orphan_all);
HLE_EXPORT_AS(msvcp110, "?_Swap_all@_Container_base0@std@@QEAAXAEAU12@@Z", cb0_swap_all);
HLE_EXPORT_AS(msvcp110, "??0_Container_base12@std@@QEAA@XZ", cb12_ctor);
HLE_EXPORT_AS(msvcp110, "??1_Container_base12@std@@QEAA@XZ", cb12_dtor);
HLE_EXPORT_AS(msvcp110, "?_Orphan_all@_Container_base12@std@@QEAAXXZ", cb12_orphan_all);
HLE_EXPORT_AS(msvcp110, "?_Swap_all@_Container_base12@std@@QEAAXAEAU12@@Z", cb12_swap_all);
HLE_EXPORT_AS(msvcp110, "?_Xlength_error@std@@YAXPEBD@Z", x_length_error);
HLE_EXPORT_AS(msvcp110, "?_Xout_of_range@std@@YAXPEBD@Z", x_out_of_range);
HLE_EXPORT_AS(msvcp110, "?_Xinvalid_argument@std@@YAXPEBD@Z", x_invalid_argument);
HLE_EXPORT_AS(msvcp110, "?_Xoverflow_error@std@@YAXPEBD@Z", x_overflow_error);
HLE_EXPORT_AS(msvcp110, "?_Xbad_alloc@std@@YAXXZ", x_bad_alloc);
HLE_EXPORT_AS(msvcp110, "?_Xbad_function_call@std@@YAXXZ", x_bad_function_call);
HLE_EXPORT_AS(msvcp110, "?_Xregex_error@std@@YAXW4error_type@regex_constants@1@@Z", x_regex_error);
HLE_EXPORT_AS(msvcp110, "?_Syserror_map@std@@YAPEBDH@Z", syserror_map);
HLE_EXPORT_AS(msvcp110, "?_Winerror_map@std@@YAPEBDH@Z", winerror_map);
HLE_EXPORT_AS(msvcp110, "?_XLgamma@std@@YANN@Z", xlgamma);
HLE_EXPORT(msvcp110, _FInf, g_finf);
HLE_EXPORT_AS(msvcp110, "?_BADOFF@std@@3_JB", g_badoff);

}  // namespace fl::crt
