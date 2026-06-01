.global fiber_prologue
.hidden fiber_prologue
.type fiber_prologue, %function

fiber_prologue:
    # x20 = user_data, x19 = user_func (set in init_fiber)
#if defined(__SANITIZE_ADDRESS__)
    // Issue the finish_switch_fiber that pairs with the scheduler's
    // start_switch_fiber when entering this fresh stack. x19/x20 are
    // callee-saved and survive the call; the incoming lr is unused (the
    // prologue never returns). sp is 16-byte aligned per init_fiber.
    bl      cfiber_asan_on_fiber_entry
#endif
    mov     x0, x20             // arg0 = user_data (void*)
    blr     x19                 // user_func, void (*fiber_fn)(void*)
    bl      fiber_epilogue      // noreturn
    brk     #0                  // should not reach

.size fiber_prologue, .-fiber_prologue
