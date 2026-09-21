/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

#include "asm_defs.inc"

FUNC_HIDDEN(cfiber_prologue)
FUNC_BEGIN(cfiber_prologue)
    # x20 = user_data, x19 = user_func (set in cfiber_init)
#if CFIBER_ASAN_ENABLED
    // Issue the finish_switch_fiber that pairs with the scheduler's
    // start_switch_fiber when entering this fresh stack. x19/x20 are
    // callee-saved and survive the call; the incoming lr is unused (the
    // prologue never returns). sp is 16-byte aligned per cfiber_init.
    bl      SYM(cfiber_asan_on_fiber_entry)
#endif
    mov     x0, x20             // arg0 = user_data (void*)
    blr     x19                 // user_func, void (*cfiber_fn)(void*)
    bl      SYM(cfiber_epilogue) // noreturn
    brk     #0                  // should not reach

FUNC_END(cfiber_prologue)

ASM_FILE_END
