// Write tracking for game memory that the GPU reads (placement resources).
//
// On Xbox the CPU and GPU share memory: the game writes vertex/constant data
// and streams textures straight into memory it placed resources over. DXVK's
// resources live in their own memory, so we copy game memory to them, and only
// when it changed.
//
// Changes are found the way Windows' GetWriteWatch finds them (Azimuth uses
// that, FINDINGS §6): userfaultfd write-protection in async mode. The kernel
// records the first write to each protected page itself (no fault reaches us,
// and kernel writes such as read() into the page work), PAGEMAP_SCAN reports
// written pages and re-protects them. Replaced an mprotect + SIGSEGV scheme
// that broke kernel writes and raced between threads (2026-10-25).
#pragma once

#include <mutex>
#include <shared_mutex>

#include <cstddef>
#include <cstdint>

namespace fl::d3d::gpumem {

void init();

// Start tracking a range (page granular). It starts out dirty.
void track(const void* addr, size_t size);

// True if any page of the range was written since it was last uploaded (or
// was never uploaded, or the game remapped it).
bool dirty(const void* addr, size_t size);

// False if any page of the range is unmapped (the game released it).
bool mapped(const void* addr, size_t size);

// False if any page of the range cannot be read: unmapped, or reserved but
// not committed (the game commits a streamed texture's small mips first and
// clamps its LOD until the big ones arrive).
bool readable(const void* addr, size_t size);

// Call right BEFORE copying the range to the GPU: re-arms tracking. Writes
// that race with the copy mark their page written again, so nothing is lost.
// on_written (optional) gets each page range that was written and is now
// re-armed: other resources sharing those pages must be marked dirty by the
// caller, since the re-arm also clears the pages for them.
using WrittenFn = void (*)(uint64_t start, uint64_t end, void* user);
// Returns false if the range could not be armed (it then reads as dirty).
bool begin_upload(const void* addr, size_t size, WrittenFn on_written = nullptr, void* user = nullptr);

// The game decommitted or released [addr, addr + size): our VirtualFree maps
// fresh pages there, which drops their write tracking, and the frame scan
// skips untracked memory. The hook (placement.cpp) marks every resource over
// the range dirty, so its next sync uploads and re-registers it.
using RemapFn = void (*)(uint64_t start, uint64_t end);
// Placement uploads read game memory that other game threads may free at the
// same time (a streamed texture decommitted mid-untile faulted on a PROT_NONE
// page). Uploads hold read_guard() while they touch it; VirtualFree and
// XMemFree take unmap_guard() before unmapping.
std::shared_lock<std::shared_mutex> read_guard();
std::unique_lock<std::shared_mutex> unmap_guard();

void set_remap_hook(RemapFn fn);
void notify_remap(const void* addr, size_t size);

// Re-arm part `part` of `parts` equal slices of the tracked address range,
// reporting each page range written since its last re-arm. The caller must
// carry that over (placement.cpp's frame scan marks the resources dirty).
// Returns the number of scan calls.
size_t rearm_all(WrittenFn on_written, void* user, uint32_t part = 0, uint32_t parts = 1);

}  // namespace fl::d3d::gpumem
