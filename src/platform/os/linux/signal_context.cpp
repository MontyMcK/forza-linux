#include "platform/os/signal_context.h"

#include <cstddef>
#include <ucontext.h>

namespace fl::os {
namespace {
constexpr int greg(int r) { return int(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, gregs) + 8 * r); }
}  // namespace

const int kRegOffset[kRegCount] = {
    greg(REG_RAX), greg(REG_RBX), greg(REG_RCX), greg(REG_RDX), greg(REG_RSI), greg(REG_RDI), greg(REG_RBP), greg(REG_RSP),
    greg(REG_R8),  greg(REG_R9),  greg(REG_R10), greg(REG_R11), greg(REG_R12), greg(REG_R13), greg(REG_R14), greg(REG_R15),
    greg(REG_RIP), greg(REG_EFL), greg(REG_ERR),
};

const uint8_t* fxsave_area(const void* uc) {
    return reinterpret_cast<const uint8_t*>(static_cast<const ucontext_t*>(uc)->uc_mcontext.fpregs);
}

}  // namespace fl::os
