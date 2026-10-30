// Virtual memory: reserve, commit, protect, execute, page size.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl::os {

// Access bits; combine them (kRead | kWrite). kNone = address space only.
enum : unsigned { kNone = 0, kRead = 1, kWrite = 2, kExec = 4 };

// The host page size: 4 KiB on Linux, 16 KiB on the PS5. Protections apply to
// whole host pages.
size_t page_size();

// New private memory, zero-filled and backed lazily, with access `prot`
// (kNone reserves address space for commit() later). With `want`, exactly
// there or nullptr: it never replaces an existing mapping. Without, anywhere,
// aligned to `align` (0 = page size). `size` is rounded up to whole pages.
void* map_anon(void* want, size_t size, unsigned prot, size_t align = 0);

// Makes [addr, addr+size) of a map_anon range accessible with `prot`. Pages
// that were already committed keep their contents; new ones read as zero.
bool commit(void* addr, size_t size, unsigned prot);

// Drops the contents of [addr, addr+size) and makes it inaccessible; the
// address range stays reserved for a later commit().
bool decommit(void* addr, size_t size);

// Returns [addr, addr+size) to the OS.
void release(void* addr, size_t size);

// Changes the access of whole pages. Executable memory: map or commit it
// writable, write the code, then protect() it with kExec (the PS5 refuses
// execute at map time).
bool protect(void* addr, size_t size, unsigned prot);

// memcpy that stops instead of faulting: read_checked() stops at the first byte
// of `src` it can't read, write_checked() at the first byte of `dst` it can't
// write (memory never committed, GPU-only ranges). Return the bytes copied.
size_t read_checked(void* dst, const void* src, size_t n);
size_t write_checked(void* dst, const void* src, size_t n);

// This process's resident memory in bytes, or 0 if the host can't say.
uint64_t resident_bytes();

}  // namespace fl::os
