// Write tracking for game memory that the GPU reads (placement resources).
//
// On Xbox the CPU and GPU share memory: the game writes vertex/constant data
// and streams textures straight into memory it placed resources over. DXVK's
// resources live in their own memory, so we copy game memory to them, and only
// when it changed. To know what changed, uploaded pages are write-protected;
// the first write to a page faults, the fault hook marks it dirty and makes it
// writable again.
//
// Code of ours that writes into game memory through the KERNEL (read(), recv()
// ...) must call prepare_write() first: a syscall into a protected page fails
// with EFAULT instead of faulting.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl::d3d::gpumem {

void init();  // installs the fault hook

// Start tracking a range (page granular). All its pages start dirty.
void track(const void* addr, size_t size);

// True if any page of the range was written since it was last cleaned.
bool dirty(const void* addr, size_t size);

// Call right BEFORE copying the range to the GPU: protects it and clears the
// dirty bits of pages that stay protected. Writes that race with the copy
// re-mark their page dirty, so nothing is lost.
void begin_upload(const void* addr, size_t size);

// See the header comment. Safe on untracked memory (does nothing).
void prepare_write(void* addr, size_t size);

}  // namespace fl::d3d::gpumem
