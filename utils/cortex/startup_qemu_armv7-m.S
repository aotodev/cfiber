.syntax unified
.thumb

.extern main
.extern _exit

/* Global symbols */
.global _sdata, _edata, _sbss, _ebss, _stack_top, _data_load_start

/* ============================================================================= */
/* Vector table section                                                          */
/* ============================================================================= */
.section .isr_vector, "a"

/* The Vector Table.The linker script places this at the beginning of FLASH (0x00000000). */
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

/* ============================================================================= */
/* Code section                                                                  */
/* ============================================================================= */
.section .text

.thumb_func
.global _start
.type _start, %function

_start:
    /* Set up the stack pointer */
    ldr r0, =_stack_top
    mov sp, r0

    /* Copy initialized data from Flash to RAM */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_data_load_start
    bl copy_data

    /* Zero out the BSS section */
    ldr r0, =_sbss
    ldr r1, =_ebss
    bl zero_bss

    /* Enable the FPU (if present) before entering main */
#if defined(CFIBER_ARM_FPU)
    /* Grant full access to CP10 and CP11 in CPACR */
    ldr r0, =0xE000ED88       /* SCB->CPACR */
    ldr r1, [r0]
    orr r1, r1, #(0xF << 20)  /* CP10/CP11 full access */
    str r1, [r0]
    dsb
    isb

    /* Enable automatic FP context preservation and lazy stacking (FPCCR.ASPEN|LSPEN) */
    ldr r0, =0xE000EF34       /* FPU->FPCCR */
    ldr r1, [r0]
    orr r1, r1, #(3 << 30)    /* ASPEN (bit31) | LSPEN (bit30) */
    str r1, [r0]

    /* warm the fpu */
    vmov s0, s0
    vmrs r0, fpscr
#endif

    /* Call the C entry point */
    bl main
    /* On return from main, exit via semihosting */
    /* r0 already holds main's return value */
    bl _exit

.size _start, .-_start

/*
 * @brief  Copies data from Flash to RAM using LDM/STM.
 * @param  r0: Destination start address (_sdata)
 * @param  r1: Destination end address (_edata)
 * @param  r2: Source address (_data_load_start)
 */
copy_data:
    cmp r0, r1          /* Check if there's anything to copy */
    bhs copy_done       /* Branch if start >= end (unsigned) */

copy_loop:
    /* Load 8 words (32 bytes) from source (r2) into r3-r10 */
    ldmia r2!, {r3-r10}

    /* Store 8 words (32 bytes) from r3-r10 into destination (r0) */
    stmia r0!, {r3-r10}

    cmp r0, r1          /* Check if we've reached the end */
    blo copy_loop       /* If not, loop again */

copy_done:
    bx lr

/*
 * @brief  Zeros out the BSS section using STM.
 * @param  r0: BSS start address (_sbss)
 * @param  r1: BSS end address (_ebss)
 */
zero_bss:
    /* Zero out registers to be used for storing */
    movs r2, #0
    movs r3, #0
    movs r4, #0
    movs r5, #0
    movs r6, #0
    movs r7, #0
    movs r8, #0
    movs r9, #0

    /* Check if there's anything to zero */
    cmp r0, r1
    bhs zero_done

zero_loop:
    /* Store 8 words (32 bytes) of zeros into the BSS section */
    stmia r0!, {r2-r9}

    /* Check if we've reached the end */
    cmp r0, r1
    blo zero_loop

zero_done:
    bx lr

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
