// The thread environment game code needs: GS (the TEB), CPU state, thread ids.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl::os {

// Points the calling thread's GS base at `base` (nullptr clears it); the game
// finds its TEB through gs:. False on failure.
bool set_gs_base(void* base);

// Host CPU state a thread needs before it runs game code, beyond GS. Linux:
// nothing (threads start with the Windows MXCSR, 0x1f80). The PS5 starts
// titles with flush-to-zero on (MXCSR 0x9fc0), so its backend sets 0x1f80.
void prepare_game_thread();

// OS ids of the calling thread and of this process.
uint64_t thread_id();
uint64_t process_id();

// The calling thread's stack: lowest address and size.
void current_stack(void** lo, size_t* size);

// Sends signal `sig` to thread `tid` (a thread_id()) of this process.
bool signal_thread(uint64_t tid, int sig);

}  // namespace fl::os
