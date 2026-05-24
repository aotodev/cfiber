.syntax unified
.thumb
.text

.global switch_context
.type switch_context, %function

# void switch_context(context_t* old, const context_t* new)
# r0 = old context pointer
# r1 = new context pointer

switch_context:
    # Save current context to old (r0)
    # Note: Cortex-M0 has limited instruction set, so we use basic load/store

    # Save stack pointer
    mov r2, sp
    str r2, [r0, #0]

    # Save callee-saved registers r4-r11
    str r4, [r0, #4]
    str r5, [r0, #8]
    str r6, [r0, #12]
    str r7, [r0, #16]

    # For r8-r11, we need to move them to low registers first (cortex-m0 limitation)
    mov r2, r8
    str r2, [r0, #20]
    mov r2, r9
    str r2, [r0, #24]
    mov r2, r10
    str r2, [r0, #28]
    mov r2, r11
    str r2, [r0, #32]

    # Save link register (return address)
    mov r2, lr
    str r2, [r0, #36]

    # Load new context from new (r1)
    # Load stack pointer
    ldr r2, [r1, #0]
    mov sp, r2

    # Load callee-saved registers r4-r7
    ldr r4, [r1, #4]
    ldr r5, [r1, #8]
    ldr r6, [r1, #12]
    ldr r7, [r1, #16]

    # Load r8-r11 (need to use low register as intermediate)
    ldr r2, [r1, #20]
    mov r8, r2
    ldr r2, [r1, #24]
    mov r9, r2
    ldr r2, [r1, #28]
    mov r10, r2
    ldr r2, [r1, #32]
    mov r11, r2

    # Load link register
    ldr r2, [r1, #36]
    mov lr, r2

    bx lr

.size switch_context, .-switch_context
