#include "platform/os/thread.h"

#include <asm/prctl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fl::os {

bool set_gs_base(void* base) { return syscall(SYS_arch_prctl, ARCH_SET_GS, base) == 0; }

void prepare_game_thread() {}

uint64_t thread_id() { return static_cast<uint64_t>(syscall(SYS_gettid)); }

void current_stack(void** lo, size_t* size) {
    pthread_attr_t attr;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, lo, size);
    pthread_attr_destroy(&attr);
}

uint64_t process_id() { return static_cast<uint64_t>(getpid()); }

bool signal_thread(uint64_t tid, int sig) {
    return syscall(SYS_tgkill, getpid(), static_cast<pid_t>(tid), sig) == 0;
}

size_t min_thread_stack() { return 2u << 20; }

}  // namespace fl::os
