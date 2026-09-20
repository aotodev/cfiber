/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

.syntax unified
.thumb
.text

.thumb_func
.global cfiber_switch_context
.type cfiber_switch_context, %function

# void cfiber_switch_context(cfiber_context_t* old, const cfiber_context_t* new)
# r0 = old context pointer
# r1 = new context pointer

cfiber_switch_context:
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

#ifdef __ARM_FP
    vstmia r0!, {s16-s31}
    vmrs r2, fpscr
    str r2, [r0], #4
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

#ifdef __ARM_FP
    vldmia r1!, {s16-s31}
    ldr r2, [r1], #4
    vmsr fpscr, r2
#endif

    # return
    bx lr

.size cfiber_switch_context, .-cfiber_switch_context
