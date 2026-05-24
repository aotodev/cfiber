.syntax unified
.thumb
.text

.thumb_func
.global fiber_prologue
.hidden fiber_prologue
.type fiber_prologue, %function

fiber_prologue:
    // r5 = user_data, r4 = user_func
    mov     r0, r5              // arg0 = user_data (void*)
    blx     r4                  // user_func, void (*fiber_fn)(void*)
    bl      fiber_epilogue      // noreturn
    bkpt    #0                  // should not reach

.size fiber_prologue, .-fiber_prologue
