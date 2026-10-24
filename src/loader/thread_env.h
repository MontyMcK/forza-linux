// Windows thread environment: TEB (via the GS segment), PEB, static TLS.
//
// Every thread that runs game code must call attach_thread() first.
// Our own C++ code never uses GS (glibc uses FS), so GS is free for the TEB.
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/winabi.h"

namespace fl::loader {

// x64 TEB field offsets (NT layout; the game reads these directly via gs:).
namespace teb_off {
constexpr size_t ExceptionList = 0x00;
constexpr size_t StackBase = 0x08;   // highest address (stack top)
constexpr size_t StackLimit = 0x10;  // lowest committed address
constexpr size_t FiberData = 0x20;
constexpr size_t ArbitraryUserPointer = 0x28;
constexpr size_t Self = 0x30;
constexpr size_t ClientIdProcess = 0x40;
constexpr size_t ClientIdThread = 0x48;
constexpr size_t ThreadLocalStoragePointer = 0x58;
constexpr size_t Peb = 0x60;
constexpr size_t LastErrorValue = 0x68;  // DWORD
constexpr size_t DeallocationStack = 0x1478;
constexpr size_t TlsSlots = 0x1480;  // void*[64]
constexpr size_t TlsExpansionSlots = 0x1780;
constexpr size_t Size = 0x2000;
}  // namespace teb_off

constexpr int kTlsMinimumSlots = 64;

struct Teb {
    uint8_t raw[teb_off::Size];

    template <typename T>
    T& field(size_t off) { return *reinterpret_cast<T*>(raw + off); }
};

// The calling thread's TEB, or null if the thread was never attached.
Teb* current_teb();

// Set up the TEB + static TLS for the calling thread and point GS at it.
// stack_lo/stack_hi describe the thread's stack.
Teb* attach_thread(void* stack_lo, void* stack_hi, bool main_thread = false);

// Free the TEB/TLS of the calling thread.
void detach_thread();

// Initialize process-wide state (PEB, TLS template). Call once after mapping.
void init_process();

// Per-thread last error, as stored in the TEB.
inline DWORD& last_error() { return current_teb()->field<DWORD>(teb_off::LastErrorValue); }

}  // namespace fl::loader
