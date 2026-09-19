.intel_syntax noprefix

.global cfiber_prologue
.hidden cfiber_prologue
.type cfiber_prologue, @function

cfiber_prologue:
    # r12 = user_data, rbx = user_func (set by cfiber_init)
#if CFIBER_ASAN_ENABLED
    # Issue the finish_switch_fiber that pairs with the scheduler's
    # start_switch_fiber when entering this fresh stack. rbx/r12 are
    # callee-saved and survive the call; rdi is loaded afterwards.
    call    cfiber_asan_on_fiber_entry
#endif
    mov     rdi, r12       // arg0 = user_data (void*)
    call    rbx            // user_func, void (*cfiber_fn)(void*)
    call    cfiber_epilogue // noreturn
    ud2                    // should not reach

.size cfiber_prologue, .-cfiber_prologue

.section .note.GNU-stack, "", @progbits
