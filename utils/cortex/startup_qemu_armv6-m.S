.syntax unified
.thumb

/* Global symbols */
.global _sdata, _edata, _sbss, _ebss, _data_load_start, _stack_top

/* External function references */
.extern main
.extern _exit

/* ======================================================================= */
/* Vector table section                                                    */
/* ======================================================================= */
.section .isr_vector, "a"

/* The linker script places this at the beginning of FLASH (0x00000000). */
.word _stack_top
.word _start + 1
.word NMI_Handler + 1
.word HardFault_Handler + 1
.word 0 // Reserved
.word 0 // Reserved
.word 0 // Reserved
.word 0 // Reserved
.word 0 // Reserved
.word 0 // Reserved
.word 0 // Reserved
.word SVC_Handler + 1
.word 0 // Reserved
.word 0 // Reserved
.word PendSV_Handler + 1
.word SysTick_Handler + 1

/* ======================================================================= */
/* Code section                                                            */
/* ======================================================================= */
.section .text

.thumb_func
.global _start
.type _start, %function

/**
 * @brief The Reset Handler, which is the entry point of the application.
 */
_start:
    /* Set up the stack pointer */
    ldr r0, =_stack_top
    mov sp, r0

    /*
     * Copy initialized data from Flash to RAM.
     * This loop is simple because the Cortex-M0 lacks LDM/STM instructions.
     */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_data_load_start

copy_loop:
    cmp r0, r1
    bhs copy_done // Branch if r0 >= r1 (unsigned)
    ldr r3, [r2]
    str r3, [r0]
    adds r0, #4
    adds r2, #4
    b copy_loop

copy_done:
    /*
     * Zero out the BSS section in RAM.
     */
    ldr r0, =_sbss
    ldr r1, =_ebss

    movs r2, #0

zero_loop:
    cmp r0, r1
    bhs zero_done
    str r2, [r0]
    adds r0, #4
    b zero_loop

zero_done:
    /* Call the C entry point */
    bl main
    /* On return from main, exit via semihosting */
    /* r0 already holds main's return value */
    bl _exit

/*
 * Default handlers for system exceptions.
 * These are defined as weak symbols, so they can be overridden by user-defined functions.
 * They default to an infinite loop, which is useful for debugging unexpected faults.
 */
.thumb_func
Default_Handler:
    b .

.weak NMI_Handler
.thumb_set NMI_Handler, Default_Handler

.weak HardFault_Handler
.thumb_set HardFault_Handler, Default_Handler

.weak SVC_Handler
.thumb_set SVC_Handler, Default_Handler

.weak PendSV_Handler
.thumb_set PendSV_Handler, Default_Handler

.weak SysTick_Handler
.thumb_set SysTick_Handler, Default_Handler
