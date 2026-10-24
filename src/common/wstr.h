// UTF-16 (Windows WCHAR) string helpers. Never use Linux wchar_t (32-bit).
#pragma once

#include <string>
#include <string_view>

namespace fl {

size_t wstrlen(const char16_t* s);
std::string utf16_to_utf8(std::u16string_view s);
inline std::string utf16_to_utf8(const char16_t* s) { return utf16_to_utf8(std::u16string_view(s, wstrlen(s))); }
std::u16string utf8_to_utf16(std::string_view s);
std::u16string latin1_to_utf16(std::string_view s);
std::string utf16_to_latin1(std::u16string_view s);

}  // namespace fl
