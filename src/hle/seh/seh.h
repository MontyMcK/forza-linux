// Windows x64 structured exception handling: CONTEXT, unwinding, dispatch.
//
// Only frames inside the game image are unwindable (they have .pdata). Our own
// HLE code is ELF and has no Windows unwind info, so exceptions are always
// raised from an asm stub that first captures the *game* caller's context.
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/winabi.h"

namespace fl::seh {

// Windows CONTEXT (x64), 0x4D0 bytes, 16-byte aligned.
struct alignas(16) Context {
    uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;  // 0x00
    uint32_t ContextFlags;                                      // 0x30
    uint32_t MxCsr;                                             // 0x34
    uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;          // 0x38
    uint32_t EFlags;                                            // 0x44
    uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;                      // 0x48
    uint64_t Gpr[16];  // 0x78: Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15 (unwind-code order)
    uint64_t Rip;      // 0xF8
    uint8_t FltSave[512];  // 0x100: XMM_SAVE_AREA32 (Xmm0 at +0xA0)
    uint8_t VectorRegister[26 * 16];
    uint64_t VectorControl, DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip,
        LastExceptionFromRip;

    uint64_t& reg(int i) { return Gpr[i]; }
    uint64_t& rsp() { return Gpr[4]; }
    uint64_t& rax() { return Gpr[0]; }
    uint8_t* xmm(int i) { return FltSave + 0xA0 + 16 * i; }
};
static_assert(offsetof(Context, Gpr) == 0x78);
static_assert(offsetof(Context, Rip) == 0xF8);
static_assert(offsetof(Context, FltSave) == 0x100);
static_assert(sizeof(Context) == 0x4D0);

enum Reg { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

struct ExceptionRecord {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    ExceptionRecord* ExceptionRecord_;
    void* ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
};
static_assert(sizeof(ExceptionRecord) == 152);

struct RuntimeFunction {
    uint32_t BeginAddress, EndAddress, UnwindData;
};

struct DispatcherContext {
    uint64_t ControlPc;
    uint64_t ImageBase;
    RuntimeFunction* FunctionEntry;
    uint64_t EstablisherFrame;
    uint64_t TargetIp;
    Context* ContextRecord;
    void* LanguageHandler;
    void* HandlerData;
    void* HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
};

struct ExceptionPointers {
    ExceptionRecord* rec;
    Context* ctx;
};

constexpr DWORD EXCEPTION_NONCONTINUABLE = 0x1;
constexpr DWORD EXCEPTION_UNWINDING = 0x2;
constexpr DWORD EXCEPTION_EXIT_UNWIND = 0x4;
constexpr DWORD EXCEPTION_TARGET_UNWIND = 0x20;

enum Disposition { ExceptionContinueExecution = 0, ExceptionContinueSearch = 1, ExceptionNestedException = 2,
                   ExceptionCollidedUnwind = 3 };

using LanguageHandler = int(WINAPI*)(ExceptionRecord* rec, uint64_t frame, Context* ctx, DispatcherContext* dc);

constexpr DWORD kCxxExceptionCode = 0xE06D7363;  // 'msc' | 0xE0000000

// ---------------------------------------------------------------- unwinding

// Find the RUNTIME_FUNCTION covering pc in the game image (null = leaf/unknown).
RuntimeFunction* lookup_function(uint64_t pc, uint64_t* image_base);

// Unwind one frame. Updates ctx to the caller's context; returns the language
// handler if the frame has one of the requested kind (UNW_FLAG_EHANDLER=1,
// UNW_FLAG_UHANDLER=2), filling handler_data and establisher frame.
void* virtual_unwind(int handler_type, uint64_t image_base, uint64_t pc, RuntimeFunction* fn, Context* ctx,
                     void** handler_data, uint64_t* establisher);

// Walk the game stack from ctx using .pdata. Writes up to `max` return
// addresses (the first is ctx.Rip). Stops when leaving the game image.
int walk_stack(Context ctx, uint64_t* pcs, int max);

// ---------------------------------------------------------------- dispatch

// Run the search phase for rec starting at ctx (the raiser's context).
// Returns only if no handler took the exception (or it continued execution).
// Handlers that catch never return here.
void dispatch(ExceptionRecord* rec, Context* ctx);

// Unwind phase: walk from `throw_ctx` up to the frame whose establisher is
// `target_frame`, calling termination handlers. Returns the context *of* the
// target frame (as seen inside it at the call site) in `out`.
void unwind_to(ExceptionRecord* rec, const Context* throw_ctx, uint64_t target_frame, Context* out);

// Restore a full context and jump to its Rip. Never returns.
extern "C" [[noreturn]] void fl_restore_context(const Context* ctx);

// Call a game funclet (catch/unwind/finally/filter) as fn(arg1, frame).
// Implemented in asm so the return address is a known marker for the unwinder.
extern "C" uint64_t fl_call_funclet(void* fn, uint64_t arg1, uint64_t frame);
extern "C" char fl_call_funclet_ret[];

// Call game code fn(arg) behind a catch-all: if an exception escapes it (no
// game handler takes it), the game frames are unwound (destructors run) and
// this returns false — like ConcRT's catch(...) around a chore. The exception
// object is leaked.
bool guarded_call(void(WINAPI* fn)(void*), void* arg);
extern "C" void fl_guarded_call(void* fn, void* arg);
extern "C" char fl_guarded_call_ret[];

// ---------------------------------------------------------------- active catch blocks
// While a catch funclet runs, the frames between it and its parent are dead.
// The unwinder skips them by substituting the parent's saved context when it
// reaches fl_call_funclet's return address.

struct ActiveCatch {
    Context parent_ctx;       // context of the frame owning the catch (at the throw call site)
    uint64_t frame;           // its establisher frame
    uint32_t funclet_rva;     // catch funclet BeginAddress
    int entry_state;          // EH state inside the catch (its try block's try_high + 1)
    ExceptionRecord rec;      // the exception being handled (for rethrow)
    bool rethrown = false;
};

ActiveCatch* active_catch(size_t depth_from_top);
size_t active_catch_count();
void push_active_catch(const ActiveCatch& c);
void pop_active_catch();

// For the substituted parent frame: use this EH state instead of the IP map.
struct StateOverride {
    uint64_t frame = 0;
    int state = 0;
    bool active = false;
};
StateOverride& state_override();

// True between a throw and the start of its catch block (std::uncaught_exception).
bool& unwinding_in_progress();

}  // namespace fl::seh
