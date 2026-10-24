// Windows x64 ABI types and calling convention for code the game calls.
//
// Everything the game calls directly (imports, vtable methods we hand out,
// callbacks we register) MUST be declared WINAPI.
//
// Remember: LONG/ULONG are 32-bit on Windows, WCHAR is UTF-16.
// Never use Linux `long` or `wchar_t` in a struct shared with the game.
#pragma once

#include <cstddef>
#include <cstdint>

#define WINAPI __attribute__((ms_abi))

using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using QWORD = uint64_t;
using BOOL = int32_t;
using BOOLEAN = uint8_t;
using LONG = int32_t;
using ULONG = uint32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using UINT = uint32_t;
using INT = int32_t;
using SIZE_T = uint64_t;
using SSIZE_T = int64_t;
using ULONG_PTR = uint64_t;
using LONG_PTR = int64_t;
using DWORD_PTR = uint64_t;
using UINT_PTR = uint64_t;
using WCHAR = char16_t;
using HRESULT = int32_t;
using NTSTATUS = int32_t;
using HANDLE = void*;
using HMODULE = void*;
using LPVOID = void*;
using LPCVOID = const void*;
using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = WCHAR*;
using LPCWSTR = const WCHAR*;

constexpr BOOL TRUE_ = 1;
constexpr BOOL FALSE_ = 0;
inline HANDLE const INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(-1);

constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001);
constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002);
constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003);
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);

struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
static_assert(sizeof(GUID) == 16);
inline bool operator==(const GUID& a, const GUID& b) { return __builtin_memcmp(&a, &b, sizeof(GUID)) == 0; }

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
};
static_assert(sizeof(LARGE_INTEGER) == 8);

struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

// Windows error codes (SetLastError values) we use.
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
constexpr DWORD ERROR_ACCESS_DENIED = 5;
constexpr DWORD ERROR_INVALID_HANDLE = 6;
constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr DWORD ERROR_NO_MORE_FILES = 18;
constexpr DWORD ERROR_HANDLE_EOF = 38;
constexpr DWORD ERROR_NOT_SUPPORTED = 50;
constexpr DWORD ERROR_FILE_EXISTS = 80;
constexpr DWORD ERROR_INVALID_PARAMETER = 87;
constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
constexpr DWORD ERROR_ALREADY_EXISTS = 183;
constexpr DWORD ERROR_IO_PENDING = 997;
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_ABANDONED_0 = 0x80;
constexpr DWORD WAIT_IO_COMPLETION = 0xC0;
constexpr DWORD WAIT_TIMEOUT = 258;
constexpr DWORD WAIT_FAILED = 0xFFFFFFFF;
constexpr DWORD INFINITE = 0xFFFFFFFF;
