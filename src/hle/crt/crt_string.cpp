// msvcr110: memory, narrow/wide string, ctype and number conversion functions.
// Windows `long` is 32-bit and wchar_t is UTF-16: never forward to the glibc
// wide-char or long functions directly.

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>

#include "common/log.h"
#include "common/winabi.h"
#include "common/wstr.h"
#include "hle/registry.h"

namespace fl::crt {

void invalid_parameter();

namespace {

using errno_t = int;
constexpr errno_t kEINVAL = 22, kERANGE = 34, kSTRUNCATE = 80;
constexpr size_t kTruncate = static_cast<size_t>(-1);
using wchar = char16_t;

// ---------------------------------------------------------------- memory

WINAPI void* c_memset(void* d, int c, size_t n) { return memset(d, c, n); }
WINAPI void* c_memcpy(void* d, const void* s, size_t n) { return memmove(d, s, n); }  // tolerate overlap
WINAPI void* c_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
WINAPI int c_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
WINAPI const void* c_memchr(const void* s, int c, size_t n) { return memchr(s, c, n); }

WINAPI errno_t c_memcpy_s(void* d, size_t dsz, const void* s, size_t n) {
    if (n == 0) return 0;
    if (!d || !s || dsz < n) {
        if (d) memset(d, 0, dsz);
        invalid_parameter();
        return !d || !s ? kEINVAL : kERANGE;
    }
    memmove(d, s, n);
    return 0;
}

// ---------------------------------------------------------------- narrow strings

WINAPI int c_strcmp(const char* a, const char* b) { return strcmp(a, b); }
WINAPI int c_strncmp(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
WINAPI const char* c_strchr(const char* s, int c) { return strchr(s, c); }
WINAPI const char* c_strrchr(const char* s, int c) { return strrchr(s, c); }
WINAPI const char* c_strstr(const char* a, const char* b) { return strstr(a, b); }
WINAPI char* c_strncpy(char* d, const char* s, size_t n) { return strncpy(d, s, n); }
WINAPI int c_stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
WINAPI int c_strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }

template <typename C>
size_t slen(const C* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

template <typename C>
errno_t t_strcpy_s(C* d, size_t dsz, const C* s) {
    if (!d || dsz == 0) return invalid_parameter(), kEINVAL;
    if (!s) return d[0] = 0, invalid_parameter(), kEINVAL;
    size_t n = slen(s);
    if (n >= dsz) return d[0] = 0, invalid_parameter(), kERANGE;
    memcpy(d, s, (n + 1) * sizeof(C));
    return 0;
}

template <typename C>
errno_t t_strcat_s(C* d, size_t dsz, const C* s) {
    if (!d || dsz == 0) return invalid_parameter(), kEINVAL;
    size_t dl = 0;
    while (dl < dsz && d[dl]) dl++;
    if (dl == dsz || !s) return d[0] = 0, invalid_parameter(), kEINVAL;
    size_t n = slen(s);
    if (dl + n >= dsz) return d[0] = 0, invalid_parameter(), kERANGE;
    memcpy(d + dl, s, (n + 1) * sizeof(C));
    return 0;
}

template <typename C>
errno_t t_strncpy_s(C* d, size_t dsz, const C* s, size_t count) {
    if (count == 0 && !d && dsz == 0) return 0;
    if (!d || dsz == 0) return invalid_parameter(), kEINVAL;
    if (count == 0) return d[0] = 0, 0;
    if (!s) return d[0] = 0, invalid_parameter(), kEINVAL;
    size_t n = 0;
    while (n < count && s[n]) n++;
    if (n >= dsz) {
        if (count == kTruncate) {
            memcpy(d, s, (dsz - 1) * sizeof(C));
            d[dsz - 1] = 0;
            return kSTRUNCATE;
        }
        d[0] = 0;
        invalid_parameter();
        return kERANGE;
    }
    memcpy(d, s, n * sizeof(C));
    d[n] = 0;
    return 0;
}

WINAPI errno_t c_strcpy_s(char* d, size_t dsz, const char* s) { return t_strcpy_s(d, dsz, s); }
WINAPI errno_t c_strcat_s(char* d, size_t dsz, const char* s) { return t_strcat_s(d, dsz, s); }
WINAPI errno_t c_strncpy_s(char* d, size_t dsz, const char* s, size_t n) { return t_strncpy_s(d, dsz, s, n); }

// ---------------------------------------------------------------- wide strings

WINAPI size_t c_wcslen(const wchar* s) { return slen(s); }

WINAPI const wchar* c_wcschr(const wchar* s, wchar c) {
    for (;; s++) {
        if (*s == c) return s;
        if (!*s) return nullptr;
    }
}

WINAPI int c_wcsncmp(const wchar* a, const wchar* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}

WINAPI const wchar* c_wcsstr(const wchar* h, const wchar* n) {
    size_t nl = slen(n);
    if (!nl) return h;
    for (; *h; h++)
        if (c_wcsncmp(h, n, nl) == 0) return h;
    return nullptr;
}

wchar wlower(wchar c) { return c < 0x80 ? static_cast<wchar>(tolower(c)) : static_cast<wchar>(towlower(c)); }
wchar wupper(wchar c) { return c < 0x80 ? static_cast<wchar>(toupper(c)) : static_cast<wchar>(towupper(c)); }

WINAPI int c_wcsnicmp(const wchar* a, const wchar* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        wchar x = wlower(a[i]), y = wlower(b[i]);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

WINAPI int c_wcsicmp(const wchar* a, const wchar* b) { return c_wcsnicmp(a, b, SIZE_MAX); }
WINAPI errno_t c_wcscpy_s(wchar* d, size_t dsz, const wchar* s) { return t_strcpy_s(d, dsz, s); }
WINAPI errno_t c_wcscat_s(wchar* d, size_t dsz, const wchar* s) { return t_strcat_s(d, dsz, s); }
WINAPI errno_t c_wcsncpy_s(wchar* d, size_t dsz, const wchar* s, size_t n) { return t_strncpy_s(d, dsz, s, n); }
WINAPI unsigned short c_towlower(unsigned short c) { return wlower(c); }
WINAPI unsigned short c_towupper(unsigned short c) { return wupper(c); }
WINAPI int c_iswspace(unsigned short c) { return c < 0x80 ? isspace(c) != 0 : iswspace(c) != 0; }

WINAPI errno_t c_wcstombs_s(size_t* converted, char* d, size_t dsz, const wchar* s, size_t count) {
    std::u16string_view sv(s, count == kTruncate ? slen(s) : std::min(count, slen(s)));
    std::string out = utf16_to_latin1(sv);
    if (!d) {
        if (converted) *converted = out.size() + 1;
        return 0;
    }
    if (out.size() + 1 > dsz) {
        if (count != kTruncate) return d[0] = 0, invalid_parameter(), kERANGE;
        out.resize(dsz - 1);
    }
    memcpy(d, out.c_str(), out.size() + 1);
    if (converted) *converted = out.size() + 1;
    return 0;
}

// ---------------------------------------------------------------- ctype

WINAPI int c_tolower(int c) { return tolower(c); }
WINAPI int c_toupper(int c) { return toupper(c); }
WINAPI int c_isalpha(int c) { return isalpha(c); }
WINAPI int c_isupper(int c) { return isupper(c); }
WINAPI int c_isspace(int c) { return isspace(c); }
WINAPI int c_isxdigit(int c) { return isxdigit(c); }

// ---------------------------------------------------------------- number parsing

template <typename C>
std::string narrow(const C* s, size_t max = 128) {
    std::string out;
    for (size_t i = 0; i < max && s[i]; i++) out += s[i] < 0x80 ? static_cast<char>(s[i]) : '?';
    return out;
}

int32_t clamp_long(long long v) {
    if (v > INT32_MAX) return errno = ERANGE, INT32_MAX;
    if (v < INT32_MIN) return errno = ERANGE, INT32_MIN;
    return static_cast<int32_t>(v);
}

WINAPI int32_t c_strtol(const char* s, char** end, int base) { return clamp_long(strtoll(s, end, base)); }

WINAPI uint32_t c_strtoul(const char* s, char** end, int base) {
    unsigned long long v = strtoull(s, end, base);
    return v > UINT32_MAX ? (errno = ERANGE, UINT32_MAX) : static_cast<uint32_t>(v);
}

WINAPI uint64_t c_strtoui64(const char* s, char** end, int base) { return strtoull(s, end, base); }
WINAPI double c_strtod(const char* s, char** end) { return strtod(s, end); }
WINAPI int c_atoi(const char* s) { return static_cast<int>(clamp_long(strtoll(s, nullptr, 10))); }
WINAPI int32_t c_atol(const char* s) { return clamp_long(strtoll(s, nullptr, 10)); }
WINAPI int64_t c_atoi64(const char* s) { return strtoll(s, nullptr, 10); }
WINAPI double c_atof(const char* s) { return strtod(s, nullptr); }

WINAPI int32_t c_wcstol(const wchar* s, wchar** end, int base) {
    std::string n = narrow(s);
    char* e;
    long long v = strtoll(n.c_str(), &e, base);
    if (end) *end = const_cast<wchar*>(s) + (e - n.c_str());
    return clamp_long(v);
}

WINAPI uint32_t c_wcstoul(const wchar* s, wchar** end, int base) {
    std::string n = narrow(s);
    char* e;
    unsigned long long v = strtoull(n.c_str(), &e, base);
    if (end) *end = const_cast<wchar*>(s) + (e - n.c_str());
    return v > UINT32_MAX ? (errno = ERANGE, UINT32_MAX) : static_cast<uint32_t>(v);
}

WINAPI uint64_t c_wcstoui64(const wchar* s, wchar** end, int base) {
    std::string n = narrow(s);
    char* e;
    unsigned long long v = strtoull(n.c_str(), &e, base);
    if (end) *end = const_cast<wchar*>(s) + (e - n.c_str());
    return v;
}

WINAPI int c_wtoi(const wchar* s) { return static_cast<int>(clamp_long(strtoll(narrow(s).c_str(), nullptr, 10))); }
WINAPI int32_t c_wtol(const wchar* s) { return clamp_long(strtoll(narrow(s).c_str(), nullptr, 10)); }

// ---------------------------------------------------------------- number formatting

template <typename C>
errno_t to_str(uint64_t v, bool neg, C* buf, size_t size, int radix) {
    if (!buf || size == 0 || radix < 2 || radix > 36) return invalid_parameter(), kEINVAL;
    char tmp[72];
    int n = 0;
    do {
        int d = static_cast<int>(v % radix);
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        v /= radix;
    } while (v);
    if (neg) tmp[n++] = '-';
    if (static_cast<size_t>(n) + 1 > size) return buf[0] = 0, invalid_parameter(), kERANGE;
    for (int i = 0; i < n; i++) buf[i] = static_cast<C>(tmp[n - 1 - i]);
    buf[n] = 0;
    return 0;
}

// Windows _itoa: negative numbers are signed only in radix 10.
WINAPI errno_t c_itoa_s(int v, char* buf, size_t size, int radix) {
    bool neg = radix == 10 && v < 0;
    uint64_t u = neg ? static_cast<uint64_t>(-static_cast<int64_t>(v)) : static_cast<uint32_t>(v);
    return to_str(u, neg, buf, size, radix);
}
WINAPI char* c_itoa(int v, char* buf, int radix) {
    c_itoa_s(v, buf, 34, radix);
    return buf;
}
WINAPI errno_t c_i64toa_s(int64_t v, char* buf, size_t size, int radix) {
    bool neg = radix == 10 && v < 0;
    uint64_t u = neg ? static_cast<uint64_t>(0) - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    return to_str(u, neg, buf, size, radix);
}
WINAPI errno_t c_ui64toa_s(uint64_t v, char* buf, size_t size, int radix) { return to_str(v, false, buf, size, radix); }
WINAPI errno_t c_ultow_s(uint32_t v, wchar* buf, size_t size, int radix) { return to_str(v, false, buf, size, radix); }

WINAPI errno_t c_itow_s(int v, wchar* buf, size_t size, int radix) {
    bool neg = radix == 10 && v < 0;
    uint64_t u = neg ? static_cast<uint64_t>(-static_cast<int64_t>(v)) : static_cast<uint32_t>(v);
    return to_str(u, neg, buf, size, radix);
}

}  // namespace

HLE_EXPORT(msvcr110, memset, c_memset);
HLE_EXPORT(msvcr110, memcpy, c_memcpy);
HLE_EXPORT(msvcr110, memmove, c_memmove);
HLE_EXPORT(msvcr110, memcmp, c_memcmp);
HLE_EXPORT(msvcr110, memchr, c_memchr);
HLE_EXPORT(msvcr110, memcpy_s, c_memcpy_s);
HLE_EXPORT(msvcr110, strcmp, c_strcmp);
HLE_EXPORT(msvcr110, strncmp, c_strncmp);
HLE_EXPORT(msvcr110, strchr, c_strchr);
HLE_EXPORT(msvcr110, strrchr, c_strrchr);
HLE_EXPORT(msvcr110, strstr, c_strstr);
HLE_EXPORT(msvcr110, strncpy, c_strncpy);
HLE_EXPORT(msvcr110, _stricmp, c_stricmp);
HLE_EXPORT(msvcr110, _strnicmp, c_strnicmp);
HLE_EXPORT(msvcr110, strcpy_s, c_strcpy_s);
HLE_EXPORT(msvcr110, strcat_s, c_strcat_s);
HLE_EXPORT(msvcr110, strncpy_s, c_strncpy_s);
HLE_EXPORT(msvcr110, wcslen, c_wcslen);
HLE_EXPORT(msvcr110, wcschr, c_wcschr);
HLE_EXPORT(msvcr110, wcsncmp, c_wcsncmp);
HLE_EXPORT(msvcr110, wcsstr, c_wcsstr);
HLE_EXPORT(msvcr110, _wcsnicmp, c_wcsnicmp);
HLE_EXPORT(msvcr110, _wcsicmp, c_wcsicmp);
HLE_EXPORT(msvcr110, wcscpy_s, c_wcscpy_s);
HLE_EXPORT(msvcr110, wcscat_s, c_wcscat_s);
HLE_EXPORT(msvcr110, wcsncpy_s, c_wcsncpy_s);
HLE_EXPORT(msvcr110, towlower, c_towlower);
HLE_EXPORT(msvcr110, towupper, c_towupper);
HLE_EXPORT(msvcr110, iswspace, c_iswspace);
HLE_EXPORT(msvcr110, wcstombs_s, c_wcstombs_s);
HLE_EXPORT(msvcr110, tolower, c_tolower);
HLE_EXPORT(msvcr110, toupper, c_toupper);
HLE_EXPORT(msvcr110, isalpha, c_isalpha);
HLE_EXPORT(msvcr110, isupper, c_isupper);
HLE_EXPORT(msvcr110, isspace, c_isspace);
HLE_EXPORT(msvcr110, isxdigit, c_isxdigit);
HLE_EXPORT(msvcr110, strtol, c_strtol);
HLE_EXPORT(msvcr110, strtoul, c_strtoul);
HLE_EXPORT(msvcr110, _strtoui64, c_strtoui64);
HLE_EXPORT(msvcr110, strtod, c_strtod);
HLE_EXPORT(msvcr110, atoi, c_atoi);
HLE_EXPORT(msvcr110, atol, c_atol);
HLE_EXPORT(msvcr110, _atoi64, c_atoi64);
HLE_EXPORT(msvcr110, atof, c_atof);
HLE_EXPORT(msvcr110, wcstol, c_wcstol);
HLE_EXPORT(msvcr110, _wcstoui64, c_wcstoui64);
HLE_EXPORT(msvcr110, _wtoi, c_wtoi);
HLE_EXPORT(msvcr110, _wtol, c_wtol);
HLE_EXPORT(msvcr110, _itoa_s, c_itoa_s);
HLE_EXPORT(msvcr110, _itoa, c_itoa);
HLE_EXPORT(msvcr110, _i64toa_s, c_i64toa_s);
HLE_EXPORT(msvcr110, _ui64toa_s, c_ui64toa_s);
HLE_EXPORT(msvcr110, _itow_s, c_itow_s);
HLE_EXPORT(msvcr110, wcstoul, c_wcstoul);
HLE_EXPORT(msvcr110, _ultow_s, c_ultow_s);

}  // namespace fl::crt
