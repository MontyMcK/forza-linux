// x64 virtual unwinding over .pdata/.xdata (public PE/COFF x64 exception
// handling spec): RUNTIME_FUNCTION lookup, UNWIND_INFO codes, epilog detection.

#include <cstring>

#include "common/log.h"
#include "hle/seh/seh.h"
#include "loader/modules.h"
#include "loader/pe_image.h"

namespace fl::seh {
namespace {

enum UnwindOp {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE = 1,
    UWOP_ALLOC_SMALL = 2,
    UWOP_SET_FPREG = 3,
    UWOP_SAVE_NONVOL = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_EPILOG = 6,  // version 2 only; ignored
    UWOP_SPARE = 7,
    UWOP_SAVE_XMM128 = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACHFRAME = 10,
};

constexpr uint8_t UNW_FLAG_EHANDLER = 1;
constexpr uint8_t UNW_FLAG_UHANDLER = 2;
constexpr uint8_t UNW_FLAG_CHAININFO = 4;

struct UnwindCode {
    uint8_t offset;
    uint8_t op : 4;
    uint8_t info : 4;
};

struct UnwindInfo {
    uint8_t version : 3;
    uint8_t flags : 5;
    uint8_t prolog_size;
    uint8_t count;
    uint8_t frame_reg : 4;
    uint8_t frame_offset : 4;
    UnwindCode codes[1];
};

int opcode_slots(const UnwindCode& c) {
    switch (c.op) {
        case UWOP_ALLOC_LARGE: return c.info == 0 ? 2 : 3;
        case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: case UWOP_EPILOG: return 2;
        case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: return 3;
        default: return 1;
    }
}

// Is pc at an epilog (add/lea rsp; pops; ret/tail-jmp)?
bool is_inside_epilog(const uint8_t* pc, uint64_t base, const RuntimeFunction* fn) {
    if ((pc[0] & 0xF8) == 0x48) {
        switch (pc[1]) {
            case 0x81:  // add rsp, imm32
                if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 7; break; }
                return false;
            case 0x83:  // add rsp, imm8
                if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 4; break; }
                return false;
            case 0x8D:  // lea rsp, [reg + disp]
                if (pc[0] & 0x06) return false;
                if (((pc[2] >> 3) & 7) != 4) return false;
                if ((pc[2] & 7) == 4) return false;
                if ((pc[2] >> 6) == 1) { pc += 4; break; }
                if ((pc[2] >> 6) == 2) { pc += 7; break; }
                return false;
            default: break;
        }
    }
    for (;;) {
        if ((*pc & 0xF0) == 0x40) pc++;  // REX
        uint8_t op = *pc;
        if (op >= 0x58 && op <= 0x5F) { pc++; continue; }  // pop reg
        switch (op) {
            case 0xC2: case 0xC3: return true;               // ret
            case 0xF3: return pc[1] == 0xC3;                  // rep ret
            case 0xE9: {                                      // jmp rel32 (tail call out of the function)
                uint64_t target = reinterpret_cast<uint64_t>(pc) + 5 + *reinterpret_cast<const int32_t*>(pc + 1);
                return target < base + fn->BeginAddress || target >= base + fn->EndAddress;
            }
            case 0xEB: {                                      // jmp rel8
                uint64_t target = reinterpret_cast<uint64_t>(pc) + 2 + static_cast<int8_t>(pc[1]);
                return target < base + fn->BeginAddress || target >= base + fn->EndAddress;
            }
            case 0xFF:                                        // jmp [mem] (indirect tail call)
                return ((pc[1] >> 3) & 7) == 4;
            default: return false;
        }
    }
}

void interpret_epilog(const uint8_t* pc, Context* ctx) {
    if ((pc[0] & 0xF8) == 0x48) {
        switch (pc[1]) {
            case 0x81: ctx->rsp() += *reinterpret_cast<const int32_t*>(pc + 3); pc += 7; break;
            case 0x83: ctx->rsp() += static_cast<int8_t>(pc[3]); pc += 4; break;
            case 0x8D: {
                int reg = (pc[2] & 7) + (pc[0] & 1) * 8;
                if ((pc[2] >> 6) == 1) {
                    ctx->rsp() = ctx->reg(reg) + static_cast<int8_t>(pc[3]);
                    pc += 4;
                } else {
                    ctx->rsp() = ctx->reg(reg) + *reinterpret_cast<const int32_t*>(pc + 3);
                    pc += 7;
                }
                break;
            }
            default: break;
        }
    }
    for (;;) {
        uint8_t rex = 0;
        if ((*pc & 0xF0) == 0x40) rex = *pc++ & 0x0F;
        uint8_t op = *pc;
        if (op >= 0x58 && op <= 0x5F) {
            ctx->reg(op - 0x58 + (rex & 1) * 8) = *reinterpret_cast<uint64_t*>(ctx->rsp());
            ctx->rsp() += 8;
            pc++;
            continue;
        }
        if (op == 0xC2) {
            ctx->Rip = *reinterpret_cast<uint64_t*>(ctx->rsp());
            ctx->rsp() += 8 + *reinterpret_cast<const uint16_t*>(pc + 1);
            return;
        }
        // ret / rep ret / tail jmp: the return address is on top of the stack.
        ctx->Rip = *reinterpret_cast<uint64_t*>(ctx->rsp());
        ctx->rsp() += 8;
        return;
    }
}

}  // namespace

RuntimeFunction* lookup_function(uint64_t pc, uint64_t* image_base) {
    // The main executable or one of the package DLLs (loader/modules.h).
    loader::PeImage* img = loader::image_for(reinterpret_cast<void*>(pc));
    if (!img || !img->exception_dir_rva) return nullptr;
    *image_base = reinterpret_cast<uint64_t>(img->base);
    auto* table = img->at<RuntimeFunction>(img->exception_dir_rva);
    size_t n = img->exception_dir_size / sizeof(RuntimeFunction);
    uint32_t rva = static_cast<uint32_t>(pc - *image_base);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < table[mid].BeginAddress)
            hi = mid;
        else if (rva >= table[mid].EndAddress)
            lo = mid + 1;
        else
            return &table[mid];
    }
    return nullptr;
}

void* virtual_unwind(int handler_type, uint64_t base, uint64_t pc, RuntimeFunction* fn, Context* ctx,
                     void** handler_data, uint64_t* establisher) {
    *handler_data = nullptr;
    uint64_t frame = ctx->rsp();
    *establisher = frame;
    void* handler = nullptr;

    for (bool primary = true;; primary = false) {
        auto* info = reinterpret_cast<UnwindInfo*>(base + fn->UnwindData);
        auto* tail = reinterpret_cast<uint8_t*>(&info->codes[(info->count + 1) & ~1]);

        uint64_t prolog_offset = ~0ull;
        if (pc >= base + fn->BeginAddress && pc < base + fn->BeginAddress + info->prolog_size) {
            prolog_offset = pc - base - fn->BeginAddress;
        } else if (primary && is_inside_epilog(reinterpret_cast<const uint8_t*>(pc), base, fn)) {
            interpret_epilog(reinterpret_cast<const uint8_t*>(pc), ctx);
            return nullptr;  // no handler is called for a frame in its epilog
        }

        // Establisher frame: the frame register if it has been set up, else rsp.
        if (info->frame_reg) {
            bool fp_set = prolog_offset == ~0ull;
            for (int i = 0; i < info->count && !fp_set; i += opcode_slots(info->codes[i]))
                if (info->codes[i].op == UWOP_SET_FPREG && prolog_offset >= info->codes[i].offset) fp_set = true;
            if (fp_set) frame = ctx->reg(info->frame_reg) - info->frame_offset * 16;
        }
        if (primary) *establisher = frame;

        for (int i = 0; i < info->count; i += opcode_slots(info->codes[i])) {
            const UnwindCode& c = info->codes[i];
            if (prolog_offset < c.offset) continue;  // not executed yet
            const uint16_t* next16 = reinterpret_cast<const uint16_t*>(&info->codes[i + 1]);
            const uint32_t next32 = next16[0] | (static_cast<uint32_t>(next16[1]) << 16);
            switch (c.op) {
                case UWOP_PUSH_NONVOL:
                    ctx->reg(c.info) = *reinterpret_cast<uint64_t*>(ctx->rsp());
                    ctx->rsp() += 8;
                    break;
                case UWOP_ALLOC_LARGE:
                    ctx->rsp() += c.info ? next32 : static_cast<uint64_t>(next16[0]) * 8;
                    break;
                case UWOP_ALLOC_SMALL:
                    ctx->rsp() += (c.info + 1) * 8;
                    break;
                case UWOP_SET_FPREG:
                    ctx->rsp() = ctx->reg(info->frame_reg) - info->frame_offset * 16;
                    break;
                case UWOP_SAVE_NONVOL:
                    ctx->reg(c.info) = *reinterpret_cast<uint64_t*>(frame + next16[0] * 8ull);
                    break;
                case UWOP_SAVE_NONVOL_FAR:
                    ctx->reg(c.info) = *reinterpret_cast<uint64_t*>(frame + next32);
                    break;
                case UWOP_SAVE_XMM128:
                    memcpy(ctx->xmm(c.info), reinterpret_cast<void*>(frame + next16[0] * 16ull), 16);
                    break;
                case UWOP_SAVE_XMM128_FAR:
                    memcpy(ctx->xmm(c.info), reinterpret_cast<void*>(frame + next32), 16);
                    break;
                case UWOP_PUSH_MACHFRAME: {
                    uint64_t sp = ctx->rsp() + (c.info ? 8 : 0);
                    ctx->Rip = *reinterpret_cast<uint64_t*>(sp);
                    ctx->rsp() = *reinterpret_cast<uint64_t*>(sp + 24);
                    return nullptr;
                }
                case UWOP_EPILOG:
                    break;
                default:
                    fl::fatal("unwind: unknown opcode %d at function rva 0x%x", c.op, fn->BeginAddress);
            }
        }

        // The handler belongs to the last info in a chain (EHANDLER/UHANDLER and
        // CHAININFO are mutually exclusive), and is not called inside a prolog.
        if (!(info->flags & UNW_FLAG_CHAININFO)) {
            if (prolog_offset == ~0ull && (info->flags & handler_type & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))) {
                handler = reinterpret_cast<void*>(base + *reinterpret_cast<uint32_t*>(tail));
                *handler_data = tail + 4;
            }
            break;
        }
        fn = reinterpret_cast<RuntimeFunction*>(tail);
    }

    ctx->Rip = *reinterpret_cast<uint64_t*>(ctx->rsp());
    ctx->rsp() += 8;
    return handler;
}

}  // namespace fl::seh
