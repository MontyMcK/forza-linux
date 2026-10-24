#include "hle/crt/crt_format.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/wstr.h"

namespace fl::crt {
namespace {

template <typename C>
struct Out {
    std::basic_string<C> s;
    void put_ascii(const char* p, size_t n) {
        for (size_t i = 0; i < n; i++) s += static_cast<C>(static_cast<unsigned char>(p[i]));
    }
    void pad(size_t n, char c) { s.append(n, static_cast<C>(c)); }
};

// Exponent fix-up: MSVC (pre-2015) always prints at least 3 exponent digits.
void fix_exponent(std::string& num) {
    size_t e = num.find_first_of("eE");
    if (e == std::string::npos || e + 2 > num.size()) return;
    size_t digits = num.size() - (e + 2);
    if (digits < 3) num.insert(e + 2, 3 - digits, '0');
}

template <typename C>
void format_impl(Out<C>& out, const C* fmt, ms_va_list ap) {
    constexpr bool kWide = sizeof(C) == 2;
    while (*fmt) {
        if (*fmt != '%') {
            out.s += *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out.s += *fmt++;
            continue;
        }
        // Flags.
        std::string flags;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0') flags += static_cast<char>(*fmt++);
        bool left = flags.find('-') != std::string::npos;
        // Width.
        int width = -1;
        if (*fmt == '*') {
            width = __builtin_va_arg(ap, int);
            if (width < 0) {
                left = true;
                flags += '-';
                width = -width;
            }
            fmt++;
        } else if (*fmt >= '0' && *fmt <= '9') {
            width = 0;
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        // Precision.
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = __builtin_va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        // Length. size: 0 = int, 1 = short, 2 = char, 4 = 32-bit long, 8 = 64-bit.
        int isize = 0;
        int strwidth = 0;  // 0 = default for this printf, 1 = narrow (h), 2 = wide (l/w)
        if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') {
            isize = 8;
            fmt += 3;
        } else if (fmt[0] == 'I' && fmt[1] == '3' && fmt[2] == '2') {
            isize = 4;
            fmt += 3;
        } else if (*fmt == 'I' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            isize = 8;
            fmt++;
        } else if (fmt[0] == 'l' && fmt[1] == 'l') {
            isize = 8;
            fmt += 2;
        } else if (fmt[0] == 'h' && fmt[1] == 'h') {
            isize = 2;
            fmt += 2;
        } else if (*fmt == 'l' || *fmt == 'w') {
            isize = 4;  // Windows long is 32-bit
            strwidth = 2;
            fmt++;
        } else if (*fmt == 'h') {
            isize = 1;
            strwidth = 1;
            fmt++;
        } else if (*fmt == 'L') {
            fmt++;  // long double == double on MSVC
        }

        char conv = static_cast<char>(*fmt);
        if (!*fmt) break;
        fmt++;

        auto emit_padded = [&](auto&& body, size_t len) {
            size_t padn = width > 0 && static_cast<size_t>(width) > len ? width - len : 0;
            if (!left) out.pad(padn, ' ');
            body();
            if (left) out.pad(padn, ' ');
        };

        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                bool is_signed = conv == 'd' || conv == 'i';
                uint64_t u;
                int64_t sv;
                if (isize == 8) {
                    sv = __builtin_va_arg(ap, int64_t);
                } else {
                    int v = __builtin_va_arg(ap, int);
                    sv = isize == 1 ? static_cast<int64_t>(static_cast<short>(v))
                       : isize == 2 ? static_cast<int64_t>(static_cast<signed char>(v))
                                    : static_cast<int64_t>(v);
                    if (!is_signed)
                        sv = isize == 1 ? static_cast<uint16_t>(v) : isize == 2 ? static_cast<uint8_t>(v) : static_cast<uint32_t>(v);
                }
                u = static_cast<uint64_t>(sv);
                std::string spec = "%" + flags;
                if (width >= 0) spec += std::to_string(width);
                if (prec >= 0) spec += "." + std::to_string(prec);
                spec += "ll";
                spec += conv;
                char buf[128];
                int n = is_signed ? snprintf(buf, sizeof buf, spec.c_str(), static_cast<long long>(sv))
                                  : snprintf(buf, sizeof buf, spec.c_str(), static_cast<unsigned long long>(u));
                out.put_ascii(buf, n);
                break;
            }
            case 'p': {
                void* p = __builtin_va_arg(ap, void*);
                char buf[32];
                int n = snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
                emit_padded([&] { out.put_ascii(buf, n); }, n);
                break;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                double v = __builtin_va_arg(ap, double);
                std::string spec = "%" + flags;
                if (width >= 0) spec += std::to_string(width);
                if (prec >= 0) spec += "." + std::to_string(prec);
                spec += conv;
                char buf[512];
                int n = snprintf(buf, sizeof buf, spec.c_str(), v);
                std::string num(buf, std::min<int>(n, sizeof buf - 1));
                if (conv != 'a' && conv != 'A' && conv != 'f' && conv != 'F') {
                    size_t before = num.size();
                    fix_exponent(num);
                    // Keep the requested width: drop padding we just displaced.
                    size_t grew = num.size() - before;
                    while (grew && !num.empty() && num[0] == ' ') {
                        num.erase(0, 1);
                        grew--;
                    }
                }
                out.put_ascii(num.data(), num.size());
                break;
            }
            case 'c': case 'C': {
                int v = __builtin_va_arg(ap, int);
                bool wide_char = strwidth == 2 || (strwidth == 0 && ((conv == 'c') == kWide));
                char16_t ch = wide_char ? static_cast<char16_t>(v) : static_cast<char16_t>(static_cast<unsigned char>(v));
                emit_padded([&] {
                    if constexpr (kWide) out.s += ch;
                    else out.s += ch < 0x100 ? static_cast<char>(ch) : '?';
                }, 1);
                break;
            }
            case 's': case 'S': {
                void* p = __builtin_va_arg(ap, void*);
                bool wide_str = strwidth == 2 || (strwidth == 0 && ((conv == 's') == kWide));
                std::basic_string<C> str;
                if (!p) {
                    const char* nul = "(null)";
                    for (const char* q = nul; *q; q++) str += static_cast<C>(*q);
                } else if (wide_str) {
                    auto* w = static_cast<const char16_t*>(p);
                    size_t n = 0;
                    while ((prec < 0 || n < static_cast<size_t>(prec)) && w[n]) n++;
                    if constexpr (kWide) str.assign(w, n);
                    else str = utf16_to_latin1(std::u16string_view(w, n));
                } else {
                    auto* a = static_cast<const char*>(p);
                    size_t n = 0;
                    while ((prec < 0 || n < static_cast<size_t>(prec)) && a[n]) n++;
                    if constexpr (kWide) str = latin1_to_utf16(std::string_view(a, n));
                    else str.assign(a, n);
                }
                emit_padded([&] { out.s += str; }, str.size());
                break;
            }
            case 'n': {
                (void)__builtin_va_arg(ap, void*);  // %n is disabled in MSVC by default
                break;
            }
            default: {
                // Unknown conversion: print it literally, like MSVC does.
                out.s += static_cast<C>('%');
                out.s += static_cast<C>(conv);
                break;
            }
        }
    }
}

}  // namespace

std::string format_narrow(const char* fmt, ms_va_list ap) {
    Out<char> out;
    format_impl(out, fmt, ap);
    return out.s;
}

std::u16string format_wide(const char16_t* fmt, ms_va_list ap) {
    Out<char16_t> out;
    format_impl(out, fmt, ap);
    return out.s;
}

}  // namespace fl::crt
