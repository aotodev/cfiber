.global switch_context
.type switch_context, %function

# void switch_context(context_t* old, const context_t* new)
# x0 = old context pointer
# x1 = new context pointer

switch_context:
    #----------------------------------------------------------------------------#
    # 1) Save current context to old (x0)                                        #
    #----------------------------------------------------------------------------#

    # Save frame pointer (x29)
    str x29, [x0, #88]

    # Save link register (x30)
    str x30, [x0, #96]

    # Load and save stack pointer
    mov x2, sp
    str x2, [x0, #0]

    # Save callee-saved general purpose registers in pairs using store pair
    stp x19, x20, [x0, #8]
    stp x21, x22, [x0, #24]
    stp x23, x24, [x0, #40]
    stp x25, x26, [x0, #56]
    stp x27, x28, [x0, #72]

    # Save callee-saved floating point registers in pairs using store pair
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]

    #----------------------------------------------------------------------------#
    # 2) Load new context from new (x1)                                          #
    #----------------------------------------------------------------------------#

    # Load and set new stack pointer
    ldr x2, [x1, #0]
    mov sp, x2

    # Load callee-saved general purpose registers in pairs using load pair
    ldp x19, x20, [x1, #8]
    ldp x21, x22, [x1, #24]
    ldp x23, x24, [x1, #40]
    ldp x25, x26, [x1, #56]
    ldp x27, x28, [x1, #72]

    # Load callee-saved floating point registers in pairs using store pair
    ldp d8, d9, [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]

    # Load frame pointer (x29)
    ldr x29, [x1, #88]

    # Load link register (x30)
    ldr x30, [x1, #96]

    # Return to the address in x30
    ret

.size switch_context, .-switch_context
