// msvcr110: heap, math, sorting, rand, time, paths, errno.
//
// Math wrappers exist because libm is SysV: it may clobber xmm6-15/rsi/rdi,
// which the Windows caller expects preserved. A WINAPI wrapper saves them.

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"

namespace fl::crt {

void invalid_parameter();

namespace {

using errno_t = int;

// ---------------------------------------------------------------- heap

WINAPI void* c_malloc(size_t n) { return malloc(n ? n : 1); }
WINAPI void* c_calloc(size_t n, size_t s) { return calloc(n ? n : 1, s ? s : 1); }
WINAPI void* c_realloc(void* p, size_t n) {
    if (n == 0) {
        free(p);
        return nullptr;
    }
    return realloc(p, n);
}
WINAPI void c_free(void* p) { free(p); }

// ---------------------------------------------------------------- math

#define MATH1(name, T) \
    WINAPI T c_##name(T x) { return name(x); } \
    HLE_EXPORT(msvcr110, name, c_##name);
#define MATH2(name, T) \
    WINAPI T c_##name(T x, T y) { return name(x, y); } \
    HLE_EXPORT(msvcr110, name, c_##name);

MATH1(sin, double) MATH1(cos, double) MATH1(tan, double) MATH1(acos, double) MATH1(sinh, double)
MATH1(exp, double) MATH1(log, double) MATH1(log10, double) MATH1(sqrt, double)
MATH2(pow, double) MATH2(fmod, double) MATH2(atan2, double)
MATH1(sinf, float) MATH1(cosf, float) MATH1(tanf, float) MATH1(asinf, float) MATH1(acosf, float)
MATH1(atanf, float) MATH1(tanhf, float) MATH1(expf, float) MATH1(logf, float) MATH1(log10f, float)
MATH2(powf, float) MATH2(fmodf, float) MATH2(atan2f, float)

WINAPI double c_ldexp(double x, int e) { return ldexp(x, e); }
WINAPI int c_finite(double x) { return std::isfinite(x); }
WINAPI int c_finitef(float x) { return std::isfinite(x); }
WINAPI int c_isnan(double x) { return std::isnan(x); }
WINAPI int c_isnanf(float x) { return std::isnan(x); }

struct LDiv {
    int32_t quot, rem;
};
WINAPI LDiv c_ldiv(int32_t a, int32_t b) { return {a / b, a % b}; }

// ---------------------------------------------------------------- sort / search

using Compare = int(WINAPI*)(const void*, const void*);

// qsort_r's argument order differs between glibc and BSD (the PS5): the
// game's comparator goes through a thread_local instead (sorts may nest).
thread_local Compare t_qsort_cmp = nullptr;
int qsort_tramp(const void* a, const void* b) { return t_qsort_cmp(a, b); }

WINAPI void c_qsort(void* base, size_t n, size_t size, Compare cmp) {
    Compare outer = t_qsort_cmp;
    t_qsort_cmp = cmp;
    qsort(base, n, size, qsort_tramp);
    t_qsort_cmp = outer;
}

WINAPI void* c_bsearch(const void* key, const void* base, size_t n, size_t size, Compare cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const void* p = static_cast<const char*>(base) + mid * size;
        int r = cmp(key, p);
        if (r == 0) return const_cast<void*>(p);
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return nullptr;
}

// ---------------------------------------------------------------- rand (MSVC LCG, per thread)

thread_local uint32_t t_seed = 1;
WINAPI void c_srand(unsigned s) { t_seed = s; }
WINAPI int c_rand() {
    t_seed = t_seed * 214013u + 2531011u;
    return static_cast<int>((t_seed >> 16) & 0x7FFF);
}

// ---------------------------------------------------------------- time

WINAPI int64_t c_time64(int64_t* out) {
    int64_t t = time(nullptr);
    if (out) *out = t;
    return t;
}

struct WinTm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

WINAPI errno_t c_localtime64_s(WinTm* out, const int64_t* t) {
    if (!out || !t) return invalid_parameter(), 22;
    time_t tt = static_cast<time_t>(*t);
    struct tm tm;
    localtime_r(&tt, &tm);
    *out = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    return 0;
}

// ---------------------------------------------------------------- paths

// Splits "C:\dir\name.ext" into its four parts. Null outputs are skipped.
void split_path(const char* path, char* drive, size_t dsz, char* dir, size_t dirsz, char* fname, size_t fsz,
                char* ext, size_t esz) {
    const char* p = path;
    auto put = [](char* out, size_t sz, const char* b, const char* e) {
        if (!out) return;
        size_t n = std::min(static_cast<size_t>(e - b), sz ? sz - 1 : 0);
        memcpy(out, b, n);
        out[n] = 0;
    };
    if (p[0] && p[1] == ':') {
        put(drive, dsz, p, p + 2);
        p += 2;
    } else {
        put(drive, dsz, p, p);
    }
    const char* last_sep = nullptr;
    for (const char* q = p; *q; q++)
        if (*q == '\\' || *q == '/') last_sep = q;
    const char* name = last_sep ? last_sep + 1 : p;
    put(dir, dirsz, p, name);
    const char* dot = strrchr(name, '.');
    const char* end = name + strlen(name);
    put(fname, fsz, name, dot ? dot : end);
    put(ext, esz, dot ? dot : end, end);
}

WINAPI void c_splitpath(const char* path, char* drive, char* dir, char* fname, char* ext) {
    split_path(path, drive, 3, dir, 256, fname, 256, ext, 256);
}

WINAPI errno_t c_splitpath_s(const char* path, char* drive, size_t dsz, char* dir, size_t dirsz, char* fname,
                             size_t fsz, char* ext, size_t esz) {
    split_path(path, drive, dsz, dir, dirsz, fname, fsz, ext, esz);
    return 0;
}

WINAPI errno_t c_makepath_s(char* out, size_t size, const char* drive, const char* dir, const char* fname,
                            const char* ext) {
    std::string s;
    if (drive && *drive) {
        s += drive[0];
        s += ':';
    }
    if (dir && *dir) {
        s += dir;
        if (s.back() != '\\' && s.back() != '/') s += '\\';
    }
    if (fname) s += fname;
    if (ext && *ext) {
        if (ext[0] != '.') s += '.';
        s += ext;
    }
    if (s.size() + 1 > size) return out[0] = 0, invalid_parameter(), 34;
    memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

WINAPI void c_makepath(char* out, const char* drive, const char* dir, const char* fname, const char* ext) {
    c_makepath_s(out, 260, drive, dir, fname, ext);
}

// ---------------------------------------------------------------- errno

// Windows and Linux agree on the small errno values the game is likely to check
// (ENOENT 2, EINVAL 22, ERANGE 34...), so we expose the glibc errno directly.
WINAPI int* c_errno() { return &errno; }

}  // namespace

// operator new/delete as exported by msvcr110 (used by package DLLs). Allocation
// failure would throw std::bad_alloc; we never return null (malloc aborts first).
HLE_EXPORT_AS(msvcr110, "??2@YAPEAX_K@Z", c_malloc);
HLE_EXPORT_AS(msvcr110, "??_U@YAPEAX_K@Z", c_malloc);
HLE_EXPORT_AS(msvcr110, "??3@YAXPEAX@Z", c_free);
HLE_EXPORT_AS(msvcr110, "??_V@YAXPEAX@Z", c_free);
HLE_EXPORT(msvcr110, malloc, c_malloc);
HLE_EXPORT(msvcr110, _malloc_crt, c_malloc);
HLE_EXPORT(msvcr110, calloc, c_calloc);
HLE_EXPORT(msvcr110, realloc, c_realloc);
HLE_EXPORT(msvcr110, free, c_free);
HLE_EXPORT(msvcr110, ldexp, c_ldexp);
HLE_EXPORT(msvcr110, _finite, c_finite);
HLE_EXPORT(msvcr110, _finitef, c_finitef);
HLE_EXPORT(msvcr110, _isnan, c_isnan);
HLE_EXPORT(msvcr110, _isnanf, c_isnanf);
HLE_EXPORT(msvcr110, ldiv, c_ldiv);
HLE_EXPORT(msvcr110, qsort, c_qsort);
HLE_EXPORT(msvcr110, bsearch, c_bsearch);
HLE_EXPORT(msvcr110, srand, c_srand);
HLE_EXPORT(msvcr110, rand, c_rand);
HLE_EXPORT(msvcr110, _time64, c_time64);
HLE_EXPORT(msvcr110, _localtime64_s, c_localtime64_s);
HLE_EXPORT(msvcr110, _splitpath, c_splitpath);
HLE_EXPORT(msvcr110, _splitpath_s, c_splitpath_s);
HLE_EXPORT(msvcr110, _makepath, c_makepath);
HLE_EXPORT(msvcr110, _makepath_s, c_makepath_s);
HLE_EXPORT(msvcr110, _errno, c_errno);

}  // namespace fl::crt
