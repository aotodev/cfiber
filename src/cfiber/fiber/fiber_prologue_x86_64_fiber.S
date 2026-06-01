.intel_syntax noprefix

.global fiber_prologue
.hidden fiber_prologue
.type fiber_prologue, @function

fiber_prologue:
    # r12 = user_data, rbx = user_func (set by init_fiber)
#if defined(__SANITIZE_ADDRESS__)
    # Issue the finish_switch_fiber that pairs with the scheduler's
    # start_switch_fiber when entering this fresh stack. rbx/r12 are
    # callee-saved and survive the call; rdi is loaded afterwards.
    call    cfiber_asan_on_fiber_entry
#endif
    mov     rdi, r12       // arg0 = user_data (void*)
    call    rbx            // user_func, void (*fiber_fn)(void*)
    call    fiber_epilogue // noreturn
    ud2                    // should not reach

.size fiber_prologue, .-fiber_prologue
