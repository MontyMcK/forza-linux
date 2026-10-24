// kernelx: time, strings/locale, debug output, modules, pointers, errors.

#include <cstring>
#include <ctime>
#include <string>

#include "common/log.h"
#include "common/config.h"
#include "loader/modules.h"
#include "common/wstr.h"
#include "hle/kernelx/kobject.h"
#include "hle/registry.h"
#include "loader/imports.h"
#include "loader/thread_env.h"

namespace fl::kx {
namespace {

// ---------------------------------------------------------------- time

constexpr uint64_t kEpochDelta = 116444736000000000ull;  // 100ns ticks 1601 -> 1970

uint64_t now_filetime() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 10000000ull + ts.tv_nsec / 100 + kEpochDelta;
}

void put_ft(FILETIME* f, uint64_t v) { *f = {static_cast<DWORD>(v), static_cast<DWORD>(v >> 32)}; }
uint64_t get_ft(const FILETIME* f) { return (uint64_t(f->dwHighDateTime) << 32) | f->dwLowDateTime; }

WINAPI void kx_GetSystemTimeAsFileTime(FILETIME* ft) { put_ft(ft, now_filetime()); }

WINAPI BOOL kx_QueryPerformanceFrequency(LARGE_INTEGER* f) {
    f->QuadPart = 10000000;  // 10 MHz, like modern Windows
    return TRUE_;
}

WINAPI BOOL kx_QueryPerformanceCounter(LARGE_INTEGER* c) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = static_cast<LONGLONG>(ts.tv_sec) * 10000000 + ts.tv_nsec / 100;
    return TRUE_;
}

WINAPI uint64_t kx_GetTickCount64() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

WINAPI DWORD kx_GetTickCount() { return static_cast<DWORD>(kx_GetTickCount64()); }

void unix_to_systemtime(time_t secs, uint32_t ms, SYSTEMTIME* st, bool local) {
    struct tm tm;
    if (local)
        localtime_r(&secs, &tm);
    else
        gmtime_r(&secs, &tm);
    st->wYear = static_cast<WORD>(tm.tm_year + 1900);
    st->wMonth = static_cast<WORD>(tm.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(tm.tm_wday);
    st->wDay = static_cast<WORD>(tm.tm_mday);
    st->wHour = static_cast<WORD>(tm.tm_hour);
    st->wMinute = static_cast<WORD>(tm.tm_min);
    st->wSecond = static_cast<WORD>(tm.tm_sec);
    st->wMilliseconds = static_cast<WORD>(ms);
}

WINAPI void kx_GetSystemTime(SYSTEMTIME* st) {
    uint64_t ft = now_filetime() - kEpochDelta;
    unix_to_systemtime(static_cast<time_t>(ft / 10000000), (ft % 10000000) / 10000, st, false);
}

WINAPI BOOL kx_FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    uint64_t v = get_ft(ft);
    if (v < kEpochDelta) v = kEpochDelta;
    v -= kEpochDelta;
    unix_to_systemtime(static_cast<time_t>(v / 10000000), (v % 10000000) / 10000, st, false);
    return TRUE_;
}

WINAPI BOOL kx_SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    struct tm tm = {};
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    time_t secs = timegm(&tm);
    put_ft(ft, static_cast<uint64_t>(secs) * 10000000ull + st->wMilliseconds * 10000ull + kEpochDelta);
    return TRUE_;
}

WINAPI BOOL kx_FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    put_ft(out, get_ft(in) + static_cast<int64_t>(tm.tm_gmtoff) * 10000000);
    return TRUE_;
}

WINAPI LONG kx_CompareFileTime(const FILETIME* a, const FILETIME* b) {
    uint64_t x = get_ft(a), y = get_ft(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// ---------------------------------------------------------------- strings / locale

constexpr UINT CP_UTF8 = 65001;

WINAPI int kx_MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen, LPWSTR dst, int dstlen) {
    size_t n = srclen < 0 ? strlen(src) + 1 : static_cast<size_t>(srclen);
    std::u16string w = cp == CP_UTF8 ? utf8_to_utf16(std::string_view(src, n)) : latin1_to_utf16(std::string_view(src, n));
    if (dstlen == 0) return static_cast<int>(w.size());
    if (static_cast<int>(w.size()) > dstlen) return set_last_error(ERROR_INSUFFICIENT_BUFFER), 0;
    memcpy(dst, w.data(), w.size() * 2);
    return static_cast<int>(w.size());
}

WINAPI int kx_WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen, LPSTR dst, int dstlen,
                                  LPCSTR defchar, BOOL* used_default) {
    size_t n = srclen < 0 ? wstrlen(src) + 1 : static_cast<size_t>(srclen);
    std::string s = cp == CP_UTF8 ? utf16_to_utf8(std::u16string_view(src, n))
                                  : utf16_to_latin1(std::u16string_view(src, n));
    if (used_default) *used_default = FALSE_;
    if (dstlen == 0) return static_cast<int>(s.size());
    if (static_cast<int>(s.size()) > dstlen) return set_last_error(ERROR_INSUFFICIENT_BUFFER), 0;
    memcpy(dst, s.data(), s.size());
    return static_cast<int>(s.size());
}

WINAPI int kx_GetUserDefaultLocaleName(LPWSTR out, int len) {
    static const char16_t kName[] = u"en-US";
    if (len < 6) return 0;
    memcpy(out, kName, sizeof kName);
    return 6;
}

WINAPI DWORD kx_LocaleNameToLCID(LPCWSTR name, DWORD flags) {
    std::string n = name ? utf16_to_utf8(name) : "";
    FL_LOG(kernelx, "LocaleNameToLCID(%s)", n.c_str());
    return 0x0409;  // en-US
}

// ---------------------------------------------------------------- debug

WINAPI void kx_OutputDebugStringA(LPCSTR s) {
    std::string m = s ? s : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    fl::log_write("game", "%s", m.c_str());
}

WINAPI void kx_OutputDebugStringW(LPCWSTR s) {
    std::string m = s ? utf16_to_utf8(s) : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    fl::log_write("game", "%s", m.c_str());
}

WINAPI void kx_DebugBreak() { FL_ERROR("game called DebugBreak (continuing)"); }

WINAPI DWORD kx_GetLastError() { return loader::current_teb()->field<DWORD>(loader::teb_off::LastErrorValue); }

// ---------------------------------------------------------------- pointers

WINAPI void* kx_EncodePointer(void* p) { return p; }
WINAPI void* kx_DecodePointer(void* p) { return p; }

// ---------------------------------------------------------------- modules
// HMODULEs for HLE DLLs are KObjects holding the DLL name.

struct Module : KObject {
    Module() : KObject(ObjType::Module) {}
    std::string name;
};

HMODULE load_module(const std::string& raw) {
    std::string name = raw;
    if (auto slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
    // PE DLLs shipped in the game package are loaded for real; their HMODULE
    // is the image base, as on Windows.
    if (loader::PeImage* img = loader::load_pe_module(config().game_dir + "/" + name)) {
        fl::log_write("kernelx", "LoadLibrary(%s) -> package DLL at %p", raw.c_str(), img->base);
        return img->base;
    }
    auto m = std::make_shared<Module>();
    m->name = name;
    // TODO: PE DLLs from the game package (Microsoft.Xbox.Services.dll...) should be
    // mapped by the loader. For now only HLE modules are "loadable".
    static const char* const kHle[] = {"xaudio2_9.dll", "xaudio2_9"};
    bool known = false;
    for (const char* k : kHle) known = known || strcasecmp(k, name.c_str()) == 0;
    fl::log_write("kernelx", "LoadLibrary(%s) -> %s", raw.c_str(), known ? "HLE module" : "NOT FOUND");
    if (!known) {
        set_last_error(126 /* ERROR_MOD_NOT_FOUND */);
        return nullptr;
    }
    return handle_add(m);
}

WINAPI HMODULE kx_LoadLibraryW(LPCWSTR name) { return load_module(utf16_to_utf8(name)); }
WINAPI HMODULE kx_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) { return load_module(utf16_to_utf8(name)); }

WINAPI void* kx_GetProcAddress(HMODULE h, LPCSTR name) {
    if (loader::PeImage* img = loader::module_by_base(h)) {
        std::string sym = reinterpret_cast<uint64_t>(name) < 0x10000
                              ? "#" + std::to_string(reinterpret_cast<uint64_t>(name))
                              : std::string(name);
        void* p = loader::pe_export(*img, sym);
        if (!p) set_last_error(127 /* ERROR_PROC_NOT_FOUND */);
        return p;
    }
    auto m = handle_get_as<Module>(h, ObjType::Module);
    if (!m) return set_last_error(ERROR_INVALID_HANDLE), nullptr;
    std::string sym = reinterpret_cast<uint64_t>(name) < 0x10000 ? "#" + std::to_string(reinterpret_cast<uint64_t>(name))
                                                                 : std::string(name);
    void* p = loader::resolve_dynamic(m->name, sym);
    fl::log_write("kernelx", "GetProcAddress(%s, %s) -> %p", m->name.c_str(), sym.c_str(), p);
    if (!p) set_last_error(127 /* ERROR_PROC_NOT_FOUND */);
    return p;
}

WINAPI BOOL kx_DisableThreadLibraryCalls(HMODULE) { return TRUE_; }  // we never send thread attach/detach

WINAPI BOOL kx_FreeLibrary(HMODULE h) {
    if (loader::module_by_base(h)) return TRUE_;  // package DLLs stay loaded
    return handle_close(h) ? TRUE_ : FALSE_;
}

WINAPI BOOL kx_CloseHandle(HANDLE h) {
    if (h == kCurrentThread || h == kCurrentProcess) return TRUE_;
    if (!handle_close(h)) {
        FL_LOG(kernelx, "CloseHandle(%p): invalid handle", h);
        return set_last_error(ERROR_INVALID_HANDLE), FALSE_;
    }
    return TRUE_;
}

}  // namespace

HLE_EXPORT(kernelx, GetSystemTimeAsFileTime, kx_GetSystemTimeAsFileTime);
HLE_EXPORT(kernelx, QueryPerformanceFrequency, kx_QueryPerformanceFrequency);
HLE_EXPORT(kernelx, QueryPerformanceCounter, kx_QueryPerformanceCounter);
HLE_EXPORT(kernelx, GetTickCount64, kx_GetTickCount64);
HLE_EXPORT(kernelx, GetTickCount, kx_GetTickCount);
HLE_EXPORT(kernelx, GetSystemTime, kx_GetSystemTime);
HLE_EXPORT(kernelx, FileTimeToSystemTime, kx_FileTimeToSystemTime);
HLE_EXPORT(kernelx, SystemTimeToFileTime, kx_SystemTimeToFileTime);
HLE_EXPORT(kernelx, FileTimeToLocalFileTime, kx_FileTimeToLocalFileTime);
HLE_EXPORT(kernelx, CompareFileTime, kx_CompareFileTime);
HLE_EXPORT(kernelx, MultiByteToWideChar, kx_MultiByteToWideChar);
HLE_EXPORT(kernelx, WideCharToMultiByte, kx_WideCharToMultiByte);
HLE_EXPORT(kernelx, GetUserDefaultLocaleName, kx_GetUserDefaultLocaleName);
HLE_EXPORT(kernelx, LocaleNameToLCID, kx_LocaleNameToLCID);
HLE_EXPORT(kernelx, OutputDebugStringA, kx_OutputDebugStringA);
HLE_EXPORT(kernelx, OutputDebugStringW, kx_OutputDebugStringW);
HLE_EXPORT(kernelx, DebugBreak, kx_DebugBreak);
HLE_EXPORT(kernelx, GetLastError, kx_GetLastError);
HLE_EXPORT(kernelx, EncodePointer, kx_EncodePointer);
HLE_EXPORT(kernelx, DecodePointer, kx_DecodePointer);
HLE_EXPORT(kernelx, LoadLibraryW, kx_LoadLibraryW);
HLE_EXPORT(kernelx, LoadLibraryExW, kx_LoadLibraryExW);
HLE_EXPORT(kernelx, GetProcAddress, kx_GetProcAddress);
HLE_EXPORT(kernelx, DisableThreadLibraryCalls, kx_DisableThreadLibraryCalls);
HLE_EXPORT(kernelx, FreeLibrary, kx_FreeLibrary);
HLE_EXPORT(kernelx, CloseHandle, kx_CloseHandle);

}  // namespace fl::kx
