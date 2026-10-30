// Registers of the thread a signal interrupted, read and written through the
// handler's context (the third argument of an SA_SIGINFO handler).
#pragma once

#include <cstdint>

namespace fl::os {

enum Reg : int {
    kRax, kRbx, kRcx, kRdx, kRsi, kRdi, kRbp, kRsp,
    kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15,
    kRip, kRflags, kErr, kRegCount
};

// Byte offset of each register in the host's signal context.
extern const int kRegOffset[kRegCount];

// g[kRip] etc.: a register of the interrupted thread, writable (a handler that
// returns resumes with the changed values).
struct Regs {
    explicit Regs(void* uc) : uc(static_cast<char*>(uc)) {}
    uint64_t& operator[](int r) const { return *reinterpret_cast<uint64_t*>(uc + kRegOffset[r]); }
    char* uc;
};

// The interrupted thread's FXSAVE image (x87, MXCSR at +24, xmm0-15 at +160).
const uint8_t* fxsave_area(const void* uc);

}  // namespace fl::os
