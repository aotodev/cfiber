.global fiber_prologue
.hidden fiber_prologue
.type fiber_prologue, %function

fiber_prologue:
    # x20 = user_data, x19 = user_func (set in init_fiber)
    mov     x0, x20             // arg0 = user_data (void*)
    blr     x19                 // user_func, void (*fiber_fn)(void*)
    bl      fiber_epilogue      // noreturn
    brk     #0                  // should not reach

.size fiber_prologue, .-fiber_prologue
