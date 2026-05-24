.syntax unified
.thumb
.text

.thumb_func
.global switch_context
.type switch_context, %function

# void switch_context(context_t* old, const context_t* new)
# r0 = old context pointer
# r1 = new context pointer

switch_context:
    # -------------------------------------
    # 1) Save current context to old (r0)
    # -------------------------------------

    # Get current stack pointer
    mov r2, sp

    # Save SP and increment r0 by 4
    str r2, [r0], #4

    # Save callee-saved registers r4-r11
    stmia r0!, {r4-r11}

    # Save link register
    str lr, [r0], #4

#if defined(CFIBER_ARM_FPU)
    vstmia r0!, {s16-s31}
#endif

    # -------------------------------------
    # 2) Load new context from new (r1)
    # -------------------------------------

    # Load stack pointer
    ldr r2, [r1], #4

    # Set stack pointer
    mov sp, r2

    # Load callee-saved registers r4-r11
    ldmia r1!, {r4-r11}

    # Load link register
    ldr lr, [r1], #4

#if defined(CFIBER_ARM_FPU)
    vldmia r1!, {s16-s31}
#endif

    # return
    bx lr

.size switch_context, .-switch_context
