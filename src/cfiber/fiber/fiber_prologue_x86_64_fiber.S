/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

#include "asm_defs.inc"

.intel_syntax noprefix

FUNC_HIDDEN(cfiber_prologue)
FUNC_BEGIN(cfiber_prologue)
    # r12 = user_data, rbx = user_func (set by cfiber_init)
#if CFIBER_ASAN_ENABLED
    # Issue the finish_switch_fiber that pairs with the scheduler's
    # start_switch_fiber when entering this fresh stack. rbx/r12 are
    # callee-saved and survive the call; rdi is loaded afterwards.
    call    SYM(cfiber_asan_on_fiber_entry)
#endif
    mov     rdi, r12       // arg0 = user_data (void*)
    call    rbx            // user_func, void (*cfiber_fn)(void*)
    call    SYM(cfiber_epilogue) // noreturn
    ud2                    // should not reach

FUNC_END(cfiber_prologue)

ASM_FILE_END
