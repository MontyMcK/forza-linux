// Import-tracing trampoline.
//
// Each traced import gets a tiny generated thunk:
//     mov r11, <ImportInfo*>
//     mov rax, fl_trace_entry
//     jmp rax
// fl_trace_entry saves the Windows argument registers, calls
// fl_trace_hit(ImportInfo*) (ms_abi, so it preserves rsi/rdi/xmm6-15 itself),
// restores the arguments and tail-jumps to ImportInfo::target.
//
// Stack at entry: rsp % 16 == 8 (return address pushed by the game's call).

    .text
    .globl fl_trace_entry
    .type fl_trace_entry, @function
fl_trace_entry:
    push %rcx
    push %rdx
    push %r8
    push %r9
    push %r11                 // 5 pushes: rsp % 16 == 0
    sub $0x60, %rsp           // 32 shadow + 4*16 xmm
    movdqu %xmm0, 0x20(%rsp)
    movdqu %xmm1, 0x30(%rsp)
    movdqu %xmm2, 0x40(%rsp)
    movdqu %xmm3, 0x50(%rsp)
    mov %r11, %rcx
    call fl_trace_hit
    movdqu 0x20(%rsp), %xmm0
    movdqu 0x30(%rsp), %xmm1
    movdqu 0x40(%rsp), %xmm2
    movdqu 0x50(%rsp), %xmm3
    add $0x60, %rsp
    pop %r11
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    jmp *(%r11)               // ImportInfo::target is the first field
    .size fl_trace_entry, .-fl_trace_entry

    .section .note.GNU-stack,"",@progbits
