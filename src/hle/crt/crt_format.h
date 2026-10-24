// MSVC-compatible printf formatting over a Windows va_list.
#pragma once

#include <string>

namespace fl::crt {

using ms_va_list = __builtin_ms_va_list;

// Format `fmt` (narrow) and return the narrow result.
std::string format_narrow(const char* fmt, ms_va_list ap);

// Format `fmt` (UTF-16) and return the UTF-16 result.
std::u16string format_wide(const char16_t* fmt, ms_va_list ap);

}  // namespace fl::crt
