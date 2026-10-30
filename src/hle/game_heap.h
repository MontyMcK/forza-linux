// The game's heap: msvcr110 malloc/calloc/realloc/free and new/delete,
// kernelx HeapAlloc/HeapFree and ConcRT's Alloc/Free all come here.
//
// Blocks of 512 KiB and more get whole pages of their own (os::map_anon),
// the way Windows' HeapAlloc serves large blocks with VirtualAlloc. On the PS5
// the platform heap (libc malloc there) returned a ~16 MiB block whose last
// bytes were reserved but not committed, and the game faulted filling it
// (game rva 0x125efda, FINDINGS 7.7). Smaller blocks use libc malloc.
#pragma once

#include <cstddef>

namespace fl::heap {

void* alloc(size_t n, bool zero);
void* realloc(void* p, size_t n);
void free(void* p);

}  // namespace fl::heap
