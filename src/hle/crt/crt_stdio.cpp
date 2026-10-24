// msvcr110: FILE streams, printf family, scanf family.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/log.h"
#include "common/winabi.h"
#include "common/wstr.h"
#include "hle/crt/crt_format.h"
#include "hle/kernelx/vfs.h"
#include "hle/registry.h"

namespace fl::crt {

void invalid_parameter();

namespace {

using errno_t = int;
using wchar = char16_t;
constexpr size_t kTruncate = static_cast<size_t>(-1);

// ---------------------------------------------------------------- FILE
// The game sees MSVC-sized FILE objects (48 bytes). We keep our host FILE*
// inside. __iob_func() returns the stdin/stdout/stderr array.

struct WinFile {
    uint64_t magic;
    FILE* host;
    bool text;
    bool is_std;
    uint8_t pad[30];
};
static_assert(sizeof(WinFile) == 48);
constexpr uint64_t kFileMagic = 0x454C49464C46ull;  // "FLFILE"

WinFile g_iob[20];

void init_iob() {
    static bool done = false;
    if (done) return;
    done = true;
    FILE* std[3] = {stdin, stdout, stderr};
    for (int i = 0; i < 3; i++) g_iob[i] = {kFileMagic, std[i], true, true, {}};
}

WINAPI WinFile* c_iob_func() {
    init_iob();
    return g_iob;
}

WinFile* check(WinFile* f) {
    init_iob();
    if (!f || f->magic != kFileMagic) {
        FL_ERROR("bad FILE* %p passed to the CRT", static_cast<void*>(f));
        return nullptr;
    }
    return f;
}

// Write text produced by the game to a stream. stdout/stderr go to the log.
void write_text(WinFile* f, const std::string& s) {
    if (f->is_std && f->host != stdin) {
        std::string m = s;
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
        if (!m.empty()) fl::log_write("game", "%s", m.c_str());
        return;
    }
    fwrite(s.data(), 1, s.size(), f->host);
}

WinFile* open_file(const char* name, const char* mode) {
    std::string m;
    bool text = true;
    for (const char* p = mode; *p && *p != ','; p++) {
        if (*p == 'b') text = false;
        if (strchr("rwa+b", *p)) m += *p;
    }
    bool writing = m.find_first_of("wa+") != std::string::npos;
    std::string host = vfs::to_host(name, writing);
    FILE* h = fopen(host.c_str(), m.c_str());
    FL_LOG(file, "fopen '%s' (%s) -> %s %s", name, mode, host.c_str(), h ? "ok" : "FAILED");
    if (!h) return nullptr;
    auto* f = new WinFile{kFileMagic, h, text, false, {}};
    return f;
}

WINAPI WinFile* c_fopen(const char* name, const char* mode) { return open_file(name, mode); }

WINAPI errno_t c_fopen_s(WinFile** out, const char* name, const char* mode) {
    if (!out) return invalid_parameter(), 22;
    *out = open_file(name, mode);
    return *out ? 0 : 2;
}

WINAPI int c_fclose(WinFile* f) {
    if (!check(f)) return -1;
    if (f->is_std) return 0;
    int r = fclose(f->host);
    f->magic = 0;
    delete f;
    return r;
}

WINAPI char* c_fgets(char* buf, int n, WinFile* f) {
    if (!check(f)) return nullptr;
    char* r = fgets(buf, n, f->host);
    if (r && f->text) {
        size_t len = strlen(buf);
        if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') {
            buf[len - 2] = '\n';
            buf[len - 1] = 0;
        }
    }
    return r;
}

WINAPI size_t c_fwrite(const void* p, size_t size, size_t count, WinFile* f) {
    if (!check(f)) return 0;
    if (f->is_std) {
        write_text(f, std::string(static_cast<const char*>(p), size * count));
        return count;
    }
    return fwrite(p, size, count, f->host);
}

WINAPI int c_fseek(WinFile* f, int32_t off, int origin) {
    if (!check(f)) return -1;
    return fseek(f->host, off, origin);
}

WINAPI int c_fflush(WinFile* f) {
    if (!f) return fflush(nullptr);
    if (!check(f)) return -1;
    return fflush(f->host);
}

// ---------------------------------------------------------------- printf family

WINAPI int c_printf(const char* fmt, ...) {
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    std::string s = format_narrow(fmt, ap);
    __builtin_ms_va_end(ap);
    write_text(&c_iob_func()[1], s);
    return static_cast<int>(s.size());
}

WINAPI int c_fprintf(WinFile* f, const char* fmt, ...) {
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    std::string s = format_narrow(fmt, ap);
    __builtin_ms_va_end(ap);
    if (!check(f)) return -1;
    write_text(f, s);
    return static_cast<int>(s.size());
}

// Copy formatted output into a caller buffer with the various MSVC rules.
template <typename C>
int copy_unbounded(C* buf, const std::basic_string<C>& s) {
    memcpy(buf, s.c_str(), (s.size() + 1) * sizeof(C));
    return static_cast<int>(s.size());
}

// _snprintf / _vsnprintf: no terminator when truncated; -1 on truncation.
template <typename C>
int copy_count(C* buf, size_t count, const std::basic_string<C>& s) {
    if (s.size() < count) return copy_unbounded(buf, s);
    memcpy(buf, s.data(), count * sizeof(C));
    return s.size() == count ? static_cast<int>(count) : -1;
}

// sprintf_s / vsprintf_s: error (empty buffer) if it doesn't fit.
template <typename C>
int copy_secure(C* buf, size_t size, const std::basic_string<C>& s) {
    if (!buf || size == 0) return invalid_parameter(), -1;
    if (s.size() + 1 > size) {
        buf[0] = 0;
        invalid_parameter();
        return -1;
    }
    return copy_unbounded(buf, s);
}

// _snprintf_s family: count may be _TRUNCATE.
template <typename C>
int copy_secure_count(C* buf, size_t size, size_t count, const std::basic_string<C>& s) {
    if (!buf || size == 0) return invalid_parameter(), -1;
    size_t limit = count == kTruncate ? size - 1 : count;
    if (s.size() <= limit && s.size() < size) return copy_unbounded(buf, s);
    if (count == kTruncate || count < size) {
        size_t n = std::min(limit, size - 1);
        memcpy(buf, s.data(), n * sizeof(C));
        buf[n] = 0;
        return -1;
    }
    buf[0] = 0;
    invalid_parameter();
    return -1;
}

// _vswprintf_c: always terminated, -1 if truncated.
template <typename C>
int copy_c(C* buf, size_t count, const std::basic_string<C>& s) {
    if (!buf || count == 0) return -1;
    if (s.size() < count) return copy_unbounded(buf, s);
    memcpy(buf, s.data(), (count - 1) * sizeof(C));
    buf[count - 1] = 0;
    return -1;
}

#define FMT_N(fmtarg) \
    ms_va_list ap;    \
    __builtin_ms_va_start(ap, fmtarg); \
    std::string s = format_narrow(fmtarg, ap); \
    __builtin_ms_va_end(ap);
#define FMT_W(fmtarg) \
    ms_va_list ap;    \
    __builtin_ms_va_start(ap, fmtarg); \
    std::u16string s = format_wide(fmtarg, ap); \
    __builtin_ms_va_end(ap);

WINAPI int c_sprintf(char* buf, const char* fmt, ...) { FMT_N(fmt) return copy_unbounded(buf, s); }
WINAPI int c_sprintf_s(char* buf, size_t size, const char* fmt, ...) { FMT_N(fmt) return copy_secure(buf, size, s); }
WINAPI int c_snprintf(char* buf, size_t count, const char* fmt, ...) { FMT_N(fmt) return copy_count(buf, count, s); }
WINAPI int c_snprintf_s(char* buf, size_t size, size_t count, const char* fmt, ...) {
    FMT_N(fmt) return copy_secure_count(buf, size, count, s);
}
WINAPI int c_vsnprintf(char* buf, size_t count, const char* fmt, ms_va_list ap) {
    return copy_count(buf, count, format_narrow(fmt, ap));
}
WINAPI int c_vsnprintf_s(char* buf, size_t size, size_t count, const char* fmt, ms_va_list ap) {
    return copy_secure_count(buf, size, count, format_narrow(fmt, ap));
}
WINAPI int c_vsprintf_s(char* buf, size_t size, const char* fmt, ms_va_list ap) {
    return copy_secure(buf, size, format_narrow(fmt, ap));
}

WINAPI int c_swprintf_s(wchar* buf, size_t size, const wchar* fmt, ...) { FMT_W(fmt) return copy_secure(buf, size, s); }
WINAPI int c_snwprintf(wchar* buf, size_t count, const wchar* fmt, ...) { FMT_W(fmt) return copy_count(buf, count, s); }
WINAPI int c_snwprintf_s(wchar* buf, size_t size, size_t count, const wchar* fmt, ...) {
    FMT_W(fmt) return copy_secure_count(buf, size, count, s);
}
WINAPI int c_vsnwprintf_s(wchar* buf, size_t size, size_t count, const wchar* fmt, ms_va_list ap) {
    return copy_secure_count(buf, size, count, format_wide(fmt, ap));
}
WINAPI int c_vswprintf_s(wchar* buf, size_t size, const wchar* fmt, ms_va_list ap) {
    return copy_secure(buf, size, format_wide(fmt, ap));
}
WINAPI int c_vswprintf_c_l(wchar* buf, size_t count, const wchar* fmt, void* locale, ms_va_list ap) {
    return copy_c(buf, count, format_wide(fmt, ap));
}

// ---------------------------------------------------------------- scanf family

template <typename C>
bool is_space(C c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Minimal MSVC scanf: %d %i %u %x %o %f %e %g %s %c %[...] %n %%,
// modifiers * width h hh l ll I64 L. `secure` = the _s variants (buffer sizes follow %s %c %[).
template <typename C>
int scan_impl(const C* in, size_t in_len, const C* fmt, ms_va_list ap, bool secure) {
    constexpr bool kWide = sizeof(C) == 2;
    size_t pos = 0;
    int assigned = 0;
    bool any_conversion = false;
    auto at_end = [&] { return pos >= in_len || in[pos] == 0; };

    while (*fmt) {
        if (is_space(*fmt)) {
            while (is_space(*fmt)) fmt++;
            while (!at_end() && is_space(in[pos])) pos++;
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (at_end()) return any_conversion ? assigned : -1;
            if (in[pos] != *fmt) return assigned;
            pos++;
            fmt++;
            continue;
        }
        fmt++;
        bool suppress = false;
        if (*fmt == '*') {
            suppress = true;
            fmt++;
        }
        size_t width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int size = 0;  // 0 int/float, 1 short, 2 char, 4 long(32)/double, 8 int64
        int strw = 0;  // 1 narrow, 2 wide
        if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') { size = 8; fmt += 3; }
        else if (fmt[0] == 'l' && fmt[1] == 'l') { size = 8; fmt += 2; }
        else if (fmt[0] == 'h' && fmt[1] == 'h') { size = 2; fmt += 2; }
        else if (*fmt == 'l' || *fmt == 'w') { size = 4; strw = 2; fmt++; }
        else if (*fmt == 'h') { size = 1; strw = 1; fmt++; }
        else if (*fmt == 'L') { size = 4; fmt++; }
        C conv = *fmt++;

        if (conv == 'n') {
            if (!suppress) *__builtin_va_arg(ap, int*) = static_cast<int>(pos);
            continue;
        }
        if (conv != 'c' && conv != '[')
            while (!at_end() && is_space(in[pos])) pos++;
        if (at_end()) return any_conversion ? assigned : -1;
        any_conversion = true;

        // Gather the candidate token (narrowed) up to `width`.
        size_t maxw = width ? width : SIZE_MAX;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                std::string tok;
                while (!at_end() && tok.size() < maxw && !is_space(in[pos]) && in[pos] < 0x80) tok += static_cast<char>(in[pos++]);
                int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
                char* end;
                unsigned long long v = (conv == 'd' || conv == 'i') ? static_cast<unsigned long long>(strtoll(tok.c_str(), &end, base))
                                                                    : strtoull(tok.c_str(), &end, base);
                size_t used = end - tok.c_str();
                pos -= tok.size() - used;
                if (used == 0) return assigned;
                if (!suppress) {
                    void* p = __builtin_va_arg(ap, void*);
                    switch (size) {
                        case 8: *static_cast<uint64_t*>(p) = v; break;
                        case 1: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(v); break;
                        case 2: *static_cast<uint8_t*>(p) = static_cast<uint8_t>(v); break;
                        default: *static_cast<uint32_t*>(p) = static_cast<uint32_t>(v); break;
                    }
                    assigned++;
                }
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
                std::string tok;
                while (!at_end() && tok.size() < maxw && !is_space(in[pos]) && in[pos] < 0x80) tok += static_cast<char>(in[pos++]);
                char* end;
                double v = strtod(tok.c_str(), &end);
                size_t used = end - tok.c_str();
                pos -= tok.size() - used;
                if (used == 0) return assigned;
                if (!suppress) {
                    void* p = __builtin_va_arg(ap, void*);
                    if (size == 4) *static_cast<double*>(p) = v;
                    else *static_cast<float*>(p) = static_cast<float>(v);
                    assigned++;
                }
                break;
            }
            case 's': case 'S': case 'c': case 'C': case '[': {
                bool wide_dst = strw == 2 || (strw == 0 && ((conv == 's' || conv == 'c' || conv == '[') == kWide));
                bool is_char = conv == 'c' || conv == 'C';
                if (is_char && !width) maxw = 1;
                // Scanset.
                bool negate = false;
                std::u16string set;
                if (conv == '[') {
                    if (*fmt == '^') { negate = true; fmt++; }
                    if (*fmt == ']') set += static_cast<char16_t>(*fmt++);
                    while (*fmt && *fmt != ']') {
                        if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                            for (C c = fmt[0]; c <= fmt[2]; c++) set += static_cast<char16_t>(c);
                            fmt += 3;
                        } else {
                            set += static_cast<char16_t>(*fmt++);
                        }
                    }
                    if (*fmt == ']') fmt++;
                }
                void* dst = suppress ? nullptr : __builtin_va_arg(ap, void*);
                size_t cap = SIZE_MAX;
                if (!suppress && secure) cap = __builtin_va_arg(ap, unsigned);
                size_t n = 0;
                auto accept = [&](C c) {
                    if (is_char) return true;
                    if (conv == '[') return (set.find(static_cast<char16_t>(c)) != std::u16string::npos) != negate;
                    return !is_space(c);
                };
                while (!at_end() && n < maxw && accept(in[pos])) {
                    if (dst) {
                        if (n + (is_char ? 0 : 1) >= cap) {
                            if (wide_dst) static_cast<char16_t*>(dst)[0] = 0; else static_cast<char*>(dst)[0] = 0;
                            return assigned;
                        }
                        if (wide_dst) static_cast<char16_t*>(dst)[n] = static_cast<char16_t>(in[pos]);
                        else static_cast<char*>(dst)[n] = static_cast<char>(in[pos]);
                    }
                    n++;
                    pos++;
                }
                if (n == 0) return assigned;
                if (dst) {
                    if (!is_char) {
                        if (wide_dst) static_cast<char16_t*>(dst)[n] = 0; else static_cast<char*>(dst)[n] = 0;
                    }
                    assigned++;
                }
                break;
            }
            default:
                FL_ERROR("scanf: unsupported conversion '%c'", static_cast<char>(conv));
                return assigned;
        }
    }
    return assigned;
}

#define SCAN(C, in, len, fmt, secure)                            \
    ms_va_list ap;                                               \
    __builtin_ms_va_start(ap, fmt);                              \
    int r = scan_impl<C>(in, len, fmt, ap, secure);              \
    __builtin_ms_va_end(ap);                                     \
    return r;

WINAPI int c_sscanf(const char* in, const char* fmt, ...) { SCAN(char, in, SIZE_MAX, fmt, false) }
WINAPI int c_sscanf_s(const char* in, const char* fmt, ...) { SCAN(char, in, SIZE_MAX, fmt, true) }
WINAPI int c_swscanf(const wchar* in, const wchar* fmt, ...) { SCAN(wchar, in, SIZE_MAX, fmt, false) }
WINAPI int c_swscanf_s(const wchar* in, const wchar* fmt, ...) { SCAN(wchar, in, SIZE_MAX, fmt, true) }
WINAPI int c_snwscanf_s(const wchar* in, size_t count, const wchar* fmt, ...) { SCAN(wchar, in, count, fmt, true) }

}  // namespace

HLE_EXPORT(msvcr110, __iob_func, c_iob_func);
HLE_EXPORT(msvcr110, fopen, c_fopen);
HLE_EXPORT(msvcr110, fopen_s, c_fopen_s);
HLE_EXPORT(msvcr110, fclose, c_fclose);
HLE_EXPORT(msvcr110, fgets, c_fgets);
HLE_EXPORT(msvcr110, fwrite, c_fwrite);
HLE_EXPORT(msvcr110, fseek, c_fseek);
HLE_EXPORT(msvcr110, fflush, c_fflush);
HLE_EXPORT(msvcr110, printf, c_printf);
HLE_EXPORT(msvcr110, fprintf, c_fprintf);
HLE_EXPORT(msvcr110, sprintf, c_sprintf);
HLE_EXPORT(msvcr110, sprintf_s, c_sprintf_s);
HLE_EXPORT(msvcr110, _snprintf, c_snprintf);
HLE_EXPORT(msvcr110, _snprintf_s, c_snprintf_s);
HLE_EXPORT(msvcr110, _vsnprintf, c_vsnprintf);
HLE_EXPORT(msvcr110, _vsnprintf_s, c_vsnprintf_s);
HLE_EXPORT(msvcr110, vsprintf_s, c_vsprintf_s);
HLE_EXPORT(msvcr110, swprintf_s, c_swprintf_s);
HLE_EXPORT(msvcr110, _snwprintf, c_snwprintf);
HLE_EXPORT(msvcr110, _snwprintf_s, c_snwprintf_s);
HLE_EXPORT(msvcr110, vswprintf_s, c_vswprintf_s);
HLE_EXPORT(msvcr110, _vsnwprintf_s, c_vsnwprintf_s);
HLE_EXPORT(msvcr110, _vswprintf_c_l, c_vswprintf_c_l);
HLE_EXPORT(msvcr110, sscanf, c_sscanf);
HLE_EXPORT(msvcr110, sscanf_s, c_sscanf_s);
HLE_EXPORT(msvcr110, swscanf, c_swscanf);
HLE_EXPORT(msvcr110, swscanf_s, c_swscanf_s);
HLE_EXPORT(msvcr110, _snwscanf_s, c_snwscanf_s);

}  // namespace fl::crt
