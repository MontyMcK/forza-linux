// libkernel functions the PS5 backend calls (no SDK header declares them).
// Measured behaviour: docs/FINDINGS.md section 7.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t* phys);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t* phys, size_t* size);
int sceKernelMprotect(const void* addr, size_t len, int prot);
int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t align);
int sceKernelMunmap(void* addr, size_t len);
int sceKernelQueryMemoryProtection(void* addr, void** start, void** end, uint32_t* prot);
int sceKernelAvailableFlexibleMemorySize(size_t* size);
int sceKernelConfiguredFlexibleMemorySize(size_t* size);
int sysarch(int number, void* args);
int _umtx_op(void* obj, int op, unsigned long val, void* uaddr, void* uaddr2);
}

namespace fl::os::ps5 {
constexpr size_t kPage = 16384;       // host page
constexpr size_t kUnit = 64 << 10;    // direct memory is allocated in 64 KiB units
constexpr int kMapFixed = 0x10;
constexpr int kDirectTypeCpu = 12;
// Our access bits (os/memory.h kRead/kWrite/kExec = 1/2/4) are the kernel's CPU bits.

// write_watch.cpp: watched pages are read-only until written; before memory
// is written by the kernel or by a checked copy, make them writable (and mark
// them written).
void write_watch_unprotect(uint64_t start, uint64_t end);
}  // namespace fl::os::ps5
