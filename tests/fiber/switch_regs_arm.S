.syntax unified
.thumb
.text

// void cfiber_test_switch_regs(cfiber_context_t* self, cfiber_context_t* other,
//                              const uintptr_t magic[N], uintptr_t out[N])
// Slot order: r4-r11, then s16-s31 when __ARM_FP (N = 8 or 24).
// Loads magic into the callee-saved set, switches, stores the set on return.
// Nothing compiler-generated runs between load, switch and store.
// Thumb-1 only (Cortex-M0): r8-r11 go through r4-r7.

.thumb_func
.global cfiber_test_switch_regs
.type cfiber_test_switch_regs, %function

cfiber_test_switch_regs:
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}
#ifdef __ARM_FP
    vpush   {s16-s31}
#endif
    push    {r3}                    // out

    ldr     r4, [r2, #16]
    mov     r8, r4
    ldr     r4, [r2, #20]
    mov     r9, r4
    ldr     r4, [r2, #24]
    mov     r10, r4
    ldr     r4, [r2, #28]
    mov     r11, r4
    ldr     r4, [r2, #0]
    ldr     r5, [r2, #4]
    ldr     r6, [r2, #8]
    ldr     r7, [r2, #12]
#ifdef __ARM_FP
    adds    r2, r2, #32
    vldmia  r2, {s16-s31}
#endif

    bl      cfiber_switch_context

    pop     {r3}
    stmia   r3!, {r4-r7}
    mov     r4, r8
    str     r4, [r3, #0]
    mov     r4, r9
    str     r4, [r3, #4]
    mov     r4, r10
    str     r4, [r3, #8]
    mov     r4, r11
    str     r4, [r3, #12]
#ifdef __ARM_FP
    adds    r3, r3, #16
    vstmia  r3, {s16-s31}
    vpop    {s16-s31}
#endif

    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}

.size cfiber_test_switch_regs, .-cfiber_test_switch_regs

// void cfiber_test_fiber_entry(void* user_data)
// Fiber entry point: forwards the untouched entry sp to
// cfiber_test_fiber_main(user_data, sp).

.thumb_func
.global cfiber_test_fiber_entry
.type cfiber_test_fiber_entry, %function

cfiber_test_fiber_entry:
    mov     r1, sp
    ldr     r2, =cfiber_test_fiber_main
    bx      r2

.size cfiber_test_fiber_entry, .-cfiber_test_fiber_entry
