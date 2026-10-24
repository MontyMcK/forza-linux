// SEH helpers: context capture stubs, context restore, funclet calls.
//
// CONTEXT offsets: Gpr[16] at 0x78 (rax rcx rdx rbx rsp rbp rsi rdi r8..r15),
// Rip 0xF8, MxCsr 0x34, Xmm0 at 0x1A0 (FltSave 0x100 + 0xA0).

#define CTX_SIZE 0x4D0
#define GPR(i) (0x78 + 8 * (i))
#define RIP 0xF8
#define MXCSR 0x34
#define XMM(i) (0x1A0 + 16 * (i))

    .text

// Capture the caller's (the game's) registers into the CONTEXT at OFF(%rsp).
// entry_rsp_reg holds the rsp at stub entry (pointing at the return address).
// Clobbers rax only (the CONTEXT's Rax is recorded as 0).
.macro CAPTURE_CONTEXT off, entry_rsp_reg
    mov %rcx, \off+GPR(1)(%rsp)
    mov %rdx, \off+GPR(2)(%rsp)
    mov %rbx, \off+GPR(3)(%rsp)
    mov %rbp, \off+GPR(5)(%rsp)
    mov %rsi, \off+GPR(6)(%rsp)
    mov %rdi, \off+GPR(7)(%rsp)
    mov %r8, \off+GPR(8)(%rsp)
    mov %r9, \off+GPR(9)(%rsp)
    mov %r10, \off+GPR(10)(%rsp)
    mov %r11, \off+GPR(11)(%rsp)
    mov %r12, \off+GPR(12)(%rsp)
    mov %r13, \off+GPR(13)(%rsp)
    mov %r14, \off+GPR(14)(%rsp)
    mov %r15, \off+GPR(15)(%rsp)
    movq $0, \off+GPR(0)(%rsp)
    movdqa %xmm0, \off+XMM(0)(%rsp)
    movdqa %xmm1, \off+XMM(1)(%rsp)
    movdqa %xmm2, \off+XMM(2)(%rsp)
    movdqa %xmm3, \off+XMM(3)(%rsp)
    movdqa %xmm4, \off+XMM(4)(%rsp)
    movdqa %xmm5, \off+XMM(5)(%rsp)
    movdqa %xmm6, \off+XMM(6)(%rsp)
    movdqa %xmm7, \off+XMM(7)(%rsp)
    movdqa %xmm8, \off+XMM(8)(%rsp)
    movdqa %xmm9, \off+XMM(9)(%rsp)
    movdqa %xmm10, \off+XMM(10)(%rsp)
    movdqa %xmm11, \off+XMM(11)(%rsp)
    movdqa %xmm12, \off+XMM(12)(%rsp)
    movdqa %xmm13, \off+XMM(13)(%rsp)
    movdqa %xmm14, \off+XMM(14)(%rsp)
    movdqa %xmm15, \off+XMM(15)(%rsp)
    stmxcsr \off+MXCSR(%rsp)
    mov (\entry_rsp_reg), %rax         // return address into the game
    mov %rax, \off+RIP(%rsp)
    lea 8(\entry_rsp_reg), %rax        // game rsp after the return
    mov %rax, \off+GPR(4)(%rsp)
.endm

// Entry stubs called by the game (ms_abi). They capture the caller context,
// then call a SysV C++ implementation with (rcx, rdx, r8, r9, ctx).
// Frame: [rsp+0] pad, [rsp+8] entry rsp, [rsp+16] CONTEXT (16-aligned).
.macro THROW_STUB name, impl
    .globl \name
    .type \name, @function
\name:
    mov %rsp, %r11            // entry rsp (rsp % 16 == 8)
    sub $(CTX_SIZE + 8), %rsp // rsp % 16 == 0
    push %r11                 // rsp % 16 == 8
    sub $8, %rsp              // rsp % 16 == 0
    CAPTURE_CONTEXT 16, %r11
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rdx
    mov %r9, %rcx
    lea 16(%rsp), %r8
    call \impl
    // The impl returned: the exception was continued. Resume the game caller.
    lea 16(%rsp), %rdi
    call fl_restore_context
    .size \name, .-\name
.endm

THROW_STUB fl_RaiseException_entry, fl_raise_exception_impl
THROW_STUB fl_CxxThrowException_entry, fl_cxx_throw_impl
THROW_STUB fl_ExceptionPtrRethrow_entry, fl_exptr_rethrow_impl

// void fl_restore_context(const Context* ctx /*rdi*/)  -- never returns
    .globl fl_restore_context
    .type fl_restore_context, @function
fl_restore_context:
    mov %rdi, %r11
    ldmxcsr MXCSR(%r11)
    movdqa XMM(0)(%r11), %xmm0
    movdqa XMM(1)(%r11), %xmm1
    movdqa XMM(2)(%r11), %xmm2
    movdqa XMM(3)(%r11), %xmm3
    movdqa XMM(4)(%r11), %xmm4
    movdqa XMM(5)(%r11), %xmm5
    movdqa XMM(6)(%r11), %xmm6
    movdqa XMM(7)(%r11), %xmm7
    movdqa XMM(8)(%r11), %xmm8
    movdqa XMM(9)(%r11), %xmm9
    movdqa XMM(10)(%r11), %xmm10
    movdqa XMM(11)(%r11), %xmm11
    movdqa XMM(12)(%r11), %xmm12
    movdqa XMM(13)(%r11), %xmm13
    movdqa XMM(14)(%r11), %xmm14
    movdqa XMM(15)(%r11), %xmm15
    // Put Rip just below the target rsp so we can 'ret' into it.
    mov GPR(4)(%r11), %rax
    sub $8, %rax
    mov RIP(%r11), %rcx
    mov %rcx, (%rax)
    mov %rax, GPR(4)(%r11)    // (the CONTEXT is ours to modify)
    mov GPR(0)(%r11), %rax
    mov GPR(1)(%r11), %rcx
    mov GPR(2)(%r11), %rdx
    mov GPR(3)(%r11), %rbx
    mov GPR(5)(%r11), %rbp
    mov GPR(6)(%r11), %rsi
    mov GPR(7)(%r11), %rdi
    mov GPR(8)(%r11), %r8
    mov GPR(9)(%r11), %r9
    mov GPR(10)(%r11), %r10
    mov GPR(12)(%r11), %r12
    mov GPR(13)(%r11), %r13
    mov GPR(14)(%r11), %r14
    mov GPR(15)(%r11), %r15
    mov GPR(4)(%r11), %rsp
    mov GPR(11)(%r11), %r11
    ret
    .size fl_restore_context, .-fl_restore_context

// uint64_t fl_call_funclet(void* fn /*rdi*/, uint64_t arg1 /*rsi*/, uint64_t frame /*rdx*/)
// Calls fn(arg1, frame) with the Windows ABI. Funclets preserve the Windows
// non-volatile set, which includes everything SysV needs preserved.
    .globl fl_call_funclet
    .globl fl_call_funclet_ret
    .type fl_call_funclet, @function
fl_call_funclet:
    push %rbx                 // rsp % 16 == 0
    sub $0x20, %rsp           // shadow space
    mov %rdi, %rax
    mov %rsi, %rcx
    call *%rax
fl_call_funclet_ret:
    add $0x20, %rsp
    pop %rbx
    ret
    .size fl_call_funclet, .-fl_call_funclet


// void fl_guarded_call(void* fn /*rdi*/, void* arg /*rsi*/): calls fn(arg) with
// the Windows ABI. Its return address marks a native catch-all boundary for the
// exception dispatcher (seh::guarded_call).
    .globl fl_guarded_call
    .globl fl_guarded_call_ret
    .type fl_guarded_call, @function
fl_guarded_call:
    push %rbx                 // rsp % 16 == 0
    sub $0x20, %rsp           // shadow space
    mov %rdi, %rax
    mov %rsi, %rcx
    call *%rax
fl_guarded_call_ret:
    add $0x20, %rsp
    pop %rbx
    ret
    .size fl_guarded_call, .-fl_guarded_call


// C++/CX exception raisers (vccorlib __abi_WinRTraise*Exception). The game
// calls them; the throw must look like it comes from the game's frame, so each
// one loads its HRESULT and jumps (return address untouched) to a capture stub.
THROW_STUB fl_WinRTraise_entry, fl_winrt_raise_impl

.macro RAISE_STUB name, hr
    .globl \name
    .type \name, @function
\name:
    mov $\hr, %ecx
    jmp fl_WinRTraise_entry
    .size \name, .-\name
.endm

RAISE_STUB fl_raise_AccessDenied, 0x80070005
RAISE_STUB fl_raise_ChangedState, 0x8000000C
RAISE_STUB fl_raise_ClassNotRegistered, 0x80040154
RAISE_STUB fl_raise_Disconnected, 0x80010108
RAISE_STUB fl_raise_Failure, 0x80004005
RAISE_STUB fl_raise_InvalidArgument, 0x80070057
RAISE_STUB fl_raise_InvalidCast, 0x80004002
RAISE_STUB fl_raise_NotImplemented, 0x80004001
RAISE_STUB fl_raise_NullReference, 0x80004003
RAISE_STUB fl_raise_ObjectDisposed, 0x80000013
RAISE_STUB fl_raise_OperationCanceled, 0x80004004
RAISE_STUB fl_raise_OutOfBounds, 0x8000000B
RAISE_STUB fl_raise_OutOfMemory, 0x8007000E
RAISE_STUB fl_raise_WrongThread, 0x8001010E

    .section .note.GNU-stack,"",@progbits
