// Write watching, like Windows' GetWriteWatch: which pages of watched memory
// were written since they were last reset. Page granular (os::page_size()).
// Used by the D3D layer to re-upload only game memory that changed
// (src/hle/d3d11x/gpumem.h has the policy around it).
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl::os {

// False if the host can't watch writes (callers then treat memory as always written).
bool write_watch_init();

// Starts watching [start, end) (page aligned). Its pages read as written until
// the first reset. Registering a range again is harmless. False on failure.
bool write_watch_register(uint64_t start, uint64_t end);

using WrittenFn = void (*)(uint64_t start, uint64_t end, void* user);

// Reports every written page range of [start, end) to on_written and resets
// those pages, so only later writes count. strict: fail (errno EPERM) if part
// of the range isn't registered (the game remapped it); otherwise memory that
// isn't watched is skipped. *calls (optional) gets the number of host calls.
bool write_watch_reset(uint64_t start, uint64_t end, bool strict, WrittenFn on_written, void* user,
                       size_t* calls = nullptr);

// True if a page of [start, end) was written since its last reset (or on error).
bool write_watch_written(uint64_t start, uint64_t end);

// Hosts that watch writes with page protection (the PS5) fault on the first
// write to a watched page: the runtime's SIGSEGV handler calls this first and
// resumes if it returns true. Linux (userfaultfd) never faults: false.
bool write_watch_on_fault(uint64_t addr, bool write);

// Call before the kernel writes into memory that may be watched (read() into
// a game buffer): a protected page would fail the write (EFAULT) instead of
// faulting. Linux: nothing to do.
void write_watch_before_kernel_write(void* addr, size_t size);

// False if a page of [start, end) is unmapped / can't be read (reserved but
// never committed).
bool pages_mapped(uint64_t start, uint64_t end);
bool pages_readable(uint64_t start, uint64_t end);

}  // namespace fl::os
