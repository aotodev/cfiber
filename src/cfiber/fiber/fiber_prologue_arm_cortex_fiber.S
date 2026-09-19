.syntax unified
.thumb
.text

.thumb_func
.global cfiber_prologue
.hidden cfiber_prologue
.type cfiber_prologue, %function

cfiber_prologue:
    // r5 = user_data, r4 = user_func
    mov     r0, r5              // arg0 = user_data (void*)
    blx     r4                  // user_func, void (*cfiber_fn)(void*)
    bl      cfiber_epilogue      // noreturn
    bkpt    #0                  // should not reach

.size cfiber_prologue, .-cfiber_prologue
