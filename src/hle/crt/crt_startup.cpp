// msvcr110: process startup/shutdown, CRT globals, handlers.

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"

namespace fl::crt {
namespace {

using PVFV = void(WINAPI*)();
using PIFV = int(WINAPI*)();
using OnExit = int(WINAPI*)();

// ---------------------------------------------------------------- initializers

WINAPI void c_initterm(PVFV* begin, PVFV* end) {
    for (PVFV* p = begin; p < end; p++)
        if (*p) (*p)();
}

WINAPI int c_initterm_e(PIFV* begin, PIFV* end) {
    for (PIFV* p = begin; p < end; p++) {
        if (*p) {
            int rc = (*p)();
            if (rc) {
                FL_ERROR("_initterm_e: initializer %p failed with %d", reinterpret_cast<void*>(*p), rc);
                return rc;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- args / globals

char g_cmdline[] = "Forza_Durango_Release_Final.exe";
char* g_acmdln = g_cmdline;
char* g_argv_storage[] = {g_cmdline, nullptr};
char* g_envp_storage[] = {nullptr};
int g_fmode = 0;
int g_commode = 0;

WINAPI int c_getmainargs(int* argc, char*** argv, char*** envp, int wildcard, void* startinfo) {
    *argc = 1;
    *argv = g_argv_storage;
    *envp = g_envp_storage;
    return 0;
}

WINAPI void c_set_app_type(int type) {}
WINAPI void c_setusermatherr(void* fn) {}
WINAPI int c_configthreadlocale(int flag) { return 2; /* _DISABLE_PER_THREAD_LOCALE */ }

// ---------------------------------------------------------------- exit

std::mutex g_exit_mutex;
std::vector<OnExit> g_onexit;

void run_onexit() {
    std::vector<OnExit> fns;
    {
        std::lock_guard lock(g_exit_mutex);
        fns.swap(g_onexit);
    }
    for (auto it = fns.rbegin(); it != fns.rend(); ++it) (*it)();
}

WINAPI OnExit c_onexit(OnExit fn) {
    std::lock_guard lock(g_exit_mutex);
    g_onexit.push_back(fn);
    return fn;
}

// The game is a single EXE, so DLL onexit tables are just the process table.
WINAPI OnExit c_dllonexit(OnExit fn, PVFV** begin, PVFV** end) { return c_onexit(fn); }

[[noreturn]] WINAPI void c_exit(int code) {
    fl::log_write("crt", "game called exit(%d)", code);
    run_onexit();
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

[[noreturn]] WINAPI void c__exit(int code) {
    fl::log_write("crt", "game called _exit(%d)", code);
    _exit(code);
}

WINAPI void c_cexit() { run_onexit(); }

[[noreturn]] WINAPI void c_amsg_exit(int msg) { fl::fatal("CRT _amsg_exit(%d)", msg); }

WINAPI int c_XcptFilter(DWORD code, void* ptrs) { return 0; /* EXCEPTION_CONTINUE_SEARCH */ }
// DLL CRT init's __except filter. MSVC calls terminate() for C++ exceptions;
// letting the search continue reports them as unhandled, which is as loud.
WINAPI int c_CppXcptFilter(DWORD code, void* ptrs) { return 0; }
WINAPI void c_clean_type_info_names_internal(void*) {}  // type_info name cache: we keep nothing

// ---------------------------------------------------------------- handlers

using InvalidParamHandler = void(WINAPI*)(const char16_t*, const char16_t*, const char16_t*, unsigned, uintptr_t);
using PurecallHandler = void(WINAPI*)();
InvalidParamHandler g_invalid_param = nullptr;
PurecallHandler g_purecall = nullptr;

WINAPI InvalidParamHandler c_set_invalid_parameter_handler(InvalidParamHandler h) {
    auto old = g_invalid_param;
    g_invalid_param = h;
    return old;
}

WINAPI PurecallHandler c_set_purecall_handler(PurecallHandler h) {
    auto old = g_purecall;
    g_purecall = h;
    return old;
}

[[noreturn]] WINAPI void c_purecall() {
    if (g_purecall) g_purecall();
    fl::fatal("pure virtual function call");
}

[[noreturn]] WINAPI void c_terminate() { fl::fatal("std::terminate() called by the game"); }

// ---------------------------------------------------------------- locks / misc

std::recursive_mutex g_crt_locks[64];

WINAPI void c_lock(int n) { g_crt_locks[n & 63].lock(); }
WINAPI void c_unlock(int n) { g_crt_locks[n & 63].unlock(); }
WINAPI void* c_calloc_crt(size_t n, size_t size) { return calloc(n, size); }

}  // namespace

// Called by other CRT code when a "secure" function gets bad arguments.
void invalid_parameter() {
    if (g_invalid_param) {
        g_invalid_param(nullptr, nullptr, nullptr, 0, 0);
        return;
    }
    FL_ERROR("CRT invalid parameter (no handler installed)");
}

HLE_EXPORT(msvcr110, _initterm, c_initterm);
HLE_EXPORT(msvcr110, _initterm_e, c_initterm_e);
HLE_EXPORT(msvcr110, __getmainargs, c_getmainargs);
HLE_EXPORT(msvcr110, __set_app_type, c_set_app_type);
HLE_EXPORT(msvcr110, __setusermatherr, c_setusermatherr);
HLE_EXPORT(msvcr110, _configthreadlocale, c_configthreadlocale);
HLE_EXPORT(msvcr110, _acmdln, g_acmdln);
HLE_EXPORT(msvcr110, _fmode, g_fmode);
HLE_EXPORT(msvcr110, _commode, g_commode);
HLE_EXPORT(msvcr110, _onexit, c_onexit);
HLE_EXPORT(msvcr110, __dllonexit, c_dllonexit);
HLE_EXPORT(msvcr110, exit, c_exit);
HLE_EXPORT(msvcr110, _exit, c__exit);
HLE_EXPORT(msvcr110, _cexit, c_cexit);
HLE_EXPORT(msvcr110, _amsg_exit, c_amsg_exit);
HLE_EXPORT(msvcr110, _XcptFilter, c_XcptFilter);
HLE_EXPORT(msvcr110, __CppXcptFilter, c_CppXcptFilter);
HLE_EXPORT(msvcr110, __clean_type_info_names_internal, c_clean_type_info_names_internal);
HLE_EXPORT(msvcr110, _set_invalid_parameter_handler, c_set_invalid_parameter_handler);
HLE_EXPORT(msvcr110, _set_purecall_handler, c_set_purecall_handler);
HLE_EXPORT(msvcr110, _purecall, c_purecall);
HLE_EXPORT_AS(msvcr110, "?terminate@@YAXXZ", c_terminate);
HLE_EXPORT(msvcr110, _lock, c_lock);
HLE_EXPORT(msvcr110, _unlock, c_unlock);
HLE_EXPORT(msvcr110, _calloc_crt, c_calloc_crt);

}  // namespace fl::crt
