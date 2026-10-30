#include "platform/os/signal_context.h"

namespace fl::os {

// FreeBSD amd64 mcontext at ucontext+64 (48 bytes after uc_sigmask), measured
// with markers in every register (FINDINGS 7.3): rdi +8, rsi +16, rdx +24,
// rcx +32, r8 +40, r9 +48, rax +56, rbx +64, rbp +72, r10..r15 +80..+120,
// rip +160. err +152, rflags +176 and rsp +184 follow the same FreeBSD layout
// (not measured directly).
namespace {
constexpr int mc(int off) { return 64 + off; }
}  // namespace

const int kRegOffset[kRegCount] = {
    mc(56), mc(64), mc(32), mc(24), mc(16), mc(8),   mc(72),  mc(184),
    mc(40), mc(48), mc(80), mc(88), mc(96), mc(104), mc(112), mc(120),
    mc(160), mc(176), mc(152),
};

// The FXSAVE image sits inside the ucontext (FreeBSD mc_fpstate), at +320.
const uint8_t* fxsave_area(const void* uc) { return static_cast<const uint8_t*>(uc) + 320; }

}  // namespace fl::os
