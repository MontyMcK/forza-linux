// Fiber context switch (SysV-callable, saves the full Windows x64
// callee-saved set: rbx rbp rdi rsi r12-r15 xmm6-xmm15 mxcsr x87cw).
//
//   void fl_fiber_switch(uint64_t* save_rsp /*rdi*/, uint64_t new_rsp /*rsi*/);
//
// Saved frame, from the saved rsp upward:
//   +0   xmm6..xmm15 (10 x 16 bytes)
//   +160 mxcsr (4), x87 control word (2), pad
//   +176 r15 r14 r13 r12 rsi rdi rbp rbx
//   +240 return address
// A new fiber gets a hand-built frame whose return address is
// fl_fiber_entry and whose r12 slot holds the Fiber*.

    .text
    .globl fl_fiber_switch
    .type fl_fiber_switch, @function
fl_fiber_switch:
    push %rbx
    push %rbp
    push %rdi
    push %rsi
    push %r12
    push %r13
    push %r14
    push %r15
    sub $176, %rsp
    movdqu %xmm6, 0(%rsp)
    movdqu %xmm7, 16(%rsp)
    movdqu %xmm8, 32(%rsp)
    movdqu %xmm9, 48(%rsp)
    movdqu %xmm10, 64(%rsp)
    movdqu %xmm11, 80(%rsp)
    movdqu %xmm12, 96(%rsp)
    movdqu %xmm13, 112(%rsp)
    movdqu %xmm14, 128(%rsp)
    movdqu %xmm15, 144(%rsp)
    stmxcsr 160(%rsp)
    fnstcw 164(%rsp)
    mov %rsp, (%rdi)

    mov %rsi, %rsp
    ldmxcsr 160(%rsp)
    fldcw 164(%rsp)
    movdqu 0(%rsp), %xmm6
    movdqu 16(%rsp), %xmm7
    movdqu 32(%rsp), %xmm8
    movdqu 48(%rsp), %xmm9
    movdqu 64(%rsp), %xmm10
    movdqu 80(%rsp), %xmm11
    movdqu 96(%rsp), %xmm12
    movdqu 112(%rsp), %xmm13
    movdqu 128(%rsp), %xmm14
    movdqu 144(%rsp), %xmm15
    add $176, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rsi
    pop %rdi
    pop %rbp
    pop %rbx
    ret
    .size fl_fiber_switch, .-fl_fiber_switch

    .globl fl_fiber_entry
    .type fl_fiber_entry, @function
fl_fiber_entry:
    mov %r12, %rdi
    and $-16, %rsp
    call fl_fiber_main        // never returns
    ud2
    .size fl_fiber_entry, .-fl_fiber_entry

    .section .note.GNU-stack,"",@progbits
