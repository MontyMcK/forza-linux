#include "common/wstr.h"

#include <cstdint>

namespace fl {

size_t wstrlen(const char16_t* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

std::string utf16_to_utf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u16string utf8_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        uint32_t c;
        int extra;
        if (b < 0x80) { c = b; extra = 0; }
        else if ((b >> 5) == 6) { c = b & 0x1F; extra = 1; }
        else if ((b >> 4) == 14) { c = b & 0x0F; extra = 2; }
        else if ((b >> 3) == 30) { c = b & 0x07; extra = 3; }
        else { out += u'�'; i++; continue; }
        if (extra && i + extra >= s.size()) { out += u'�'; break; }
        for (int k = 1; k <= extra; k++) c = (c << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        i += extra + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

std::u16string latin1_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (char ch : s) out += static_cast<char16_t>(static_cast<uint8_t>(ch));
    return out;
}

std::string utf16_to_latin1(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) out += c < 0x100 ? static_cast<char>(c) : '?';
    return out;
}

}  // namespace fl
