.intel_syntax noprefix
.text

# void cfiber_test_switch_regs(context_t* self, context_t* other,
#                              const uintptr_t magic[6], uintptr_t out[6])
# Slot order: rbx, rbp, r12, r13, r14, r15.
# Loads magic into the callee-saved set, switches, stores the set on return.
# Nothing compiler-generated runs between load, switch and store.

.global cfiber_test_switch_regs
.type cfiber_test_switch_regs, @function

cfiber_test_switch_regs:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    push    rcx                     # out; 7th push realigns rsp to 16

    mov     rbx, [rdx + 0x00]
    mov     rbp, [rdx + 0x08]
    mov     r12, [rdx + 0x10]
    mov     r13, [rdx + 0x18]
    mov     r14, [rdx + 0x20]
    mov     r15, [rdx + 0x28]

    call    switch_context

    pop     rcx
    mov     [rcx + 0x00], rbx
    mov     [rcx + 0x08], rbp
    mov     [rcx + 0x10], r12
    mov     [rcx + 0x18], r13
    mov     [rcx + 0x20], r14
    mov     [rcx + 0x28], r15

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

.size cfiber_test_switch_regs, .-cfiber_test_switch_regs

# void cfiber_test_fiber_entry(void* user_data)
# Fiber entry point: forwards the untouched entry rsp to
# cfiber_test_fiber_main(user_data, rsp).

.global cfiber_test_fiber_entry
.type cfiber_test_fiber_entry, @function

cfiber_test_fiber_entry:
    mov     rsi, rsp
    jmp     cfiber_test_fiber_main

.size cfiber_test_fiber_entry, .-cfiber_test_fiber_entry

.section .note.GNU-stack, "", @progbits
