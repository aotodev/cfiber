.intel_syntax noprefix

.global cfiber_switch_context
.type cfiber_switch_context, @function

cfiber_switch_context:
    # Save current context (first parameter in %rdi)
    mov     [rdi + 0x00], rsp
    mov     [rdi + 0x08], r15
    mov     [rdi + 0x10], r14
    mov     [rdi + 0x18], r13
    mov     [rdi + 0x20], r12
    mov     [rdi + 0x28], rbx
    mov     [rdi + 0x30], rbp
    # FP control state: callee-saved by the System V ABI
    stmxcsr [rdi + 0x38]
    fnstcw  [rdi + 0x3c]

    # Load new context (second parameter in %rsi)
    mov     rsp, [rsi + 0x00]
    mov     r15, [rsi + 0x08]
    mov     r14, [rsi + 0x10]
    mov     r13, [rsi + 0x18]
    mov     r12, [rsi + 0x20]
    mov     rbx, [rsi + 0x28]
    mov     rbp, [rsi + 0x30]
    ldmxcsr [rsi + 0x38]
    fldcw   [rsi + 0x3c]

    ret

.size cfiber_switch_context, .-cfiber_switch_context

.section .note.GNU-stack, "", @progbits
