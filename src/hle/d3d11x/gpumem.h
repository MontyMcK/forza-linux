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
void begin_upload(const void* addr, size_t size);

}  // namespace fl::d3d::gpumem
