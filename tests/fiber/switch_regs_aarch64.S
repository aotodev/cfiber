.text

// void cfiber_test_switch_regs(context_t* self, context_t* other,
//                              const uintptr_t magic[19], uintptr_t out[19])
// Slot order: x19-x28, x29, d8-d15.
// Loads magic into the callee-saved set, switches, stores the set on return.
// Nothing compiler-generated runs between load, switch and store.

.global cfiber_test_switch_regs
.type cfiber_test_switch_regs, %function

cfiber_test_switch_regs:
    sub     sp, sp, #176
    stp     x29, x30, [sp, #0]
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8, d9, [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]
    str     x3, [sp, #160]          // out

    ldp     x19, x20, [x2, #0]
    ldp     x21, x22, [x2, #16]
    ldp     x23, x24, [x2, #32]
    ldp     x25, x26, [x2, #48]
    ldp     x27, x28, [x2, #64]
    ldr     x29, [x2, #80]
    ldp     d8, d9, [x2, #88]
    ldp     d10, d11, [x2, #104]
    ldp     d12, d13, [x2, #120]
    ldp     d14, d15, [x2, #136]

    bl      switch_context

    ldr     x3, [sp, #160]
    stp     x19, x20, [x3, #0]
    stp     x21, x22, [x3, #16]
    stp     x23, x24, [x3, #32]
    stp     x25, x26, [x3, #48]
    stp     x27, x28, [x3, #64]
    str     x29, [x3, #80]
    stp     d8, d9, [x3, #88]
    stp     d10, d11, [x3, #104]
    stp     d12, d13, [x3, #120]
    stp     d14, d15, [x3, #136]

    ldp     x29, x30, [sp, #0]
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     d8, d9, [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    add     sp, sp, #176
    ret

.size cfiber_test_switch_regs, .-cfiber_test_switch_regs

// void cfiber_test_fiber_entry(void* user_data)
// Fiber entry point: forwards the untouched entry sp to
// cfiber_test_fiber_main(user_data, sp).

.global cfiber_test_fiber_entry
.type cfiber_test_fiber_entry, %function

cfiber_test_fiber_entry:
    mov     x1, sp
    b       cfiber_test_fiber_main

.size cfiber_test_fiber_entry, .-cfiber_test_fiber_entry

.section .note.GNU-stack, "", %progbits
