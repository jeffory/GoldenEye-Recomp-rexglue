// AArch64 fiber context switch for Android.
//
// bionic does not provide getcontext/makecontext/swapcontext, so
// rex::thread::Fiber uses this minimal cooperative switch (see fiber_posix.cpp,
// REX_PLATFORM_ANDROID).
//
//   void rex_fiber_swap(void** from_sp_out, void* to_sp)
//     x0 = from_sp_out, x1 = to_sp
//
// Saves the AArch64 callee-saved registers (x19-x28, fp/x29, lr/x30, and the
// low 64 bits of v8-v15) onto the current stack, writes the resulting stack
// pointer to *from_sp_out, switches sp to to_sp, restores that fiber's saved
// registers, and returns into its lr. A freshly created fiber's initial frame
// (built by Fiber::Create) has lr = Fiber::Trampoline, so the first switch
// begins executing the trampoline on the new stack.
//
// Frame layout (160 bytes, 10 register pairs):
//     0: x19,x20    16: x21,x22    32: x23,x24    48: x25,x26    64: x27,x28
//    80: x29,x30    96: d8,d9     112: d10,d11   128: d12,d13   144: d14,d15
    .text
    .global rex_fiber_swap
    .type rex_fiber_swap, %function
    .align 4
rex_fiber_swap:
    stp x19, x20, [sp, #-160]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]

    mov x2, sp
    str x2, [x0]          // *from_sp_out = sp
    mov sp, x1            // sp = to_sp

    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8,  d9,  [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret
    .size rex_fiber_swap, .-rex_fiber_swap

// Mark the stack as non-executable.
    .section .note.GNU-stack,"",%progbits
