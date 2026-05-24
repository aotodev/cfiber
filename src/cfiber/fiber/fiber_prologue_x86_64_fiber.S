.intel_syntax noprefix

.global fiber_prologue
.hidden fiber_prologue
.type fiber_prologue, @function

fiber_prologue:
    # r12 = user_data, rbx = user_func (set by init_fiber)
    mov     rdi, r12       // arg0 = user_data (void*)
    call    rbx            // user_func, void (*fiber_fn)(void*)
    call    fiber_epilogue // noreturn
    ud2                    // should not reach

.size fiber_prologue, .-fiber_prologue
