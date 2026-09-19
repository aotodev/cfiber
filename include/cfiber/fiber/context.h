/**
 * @file  context.h
 * @brief Low-level context switching primitives for fiber implementation.
 *
 * @details This header defines the architecture-specific context structures
 *          and the core context switching function. The context stores all
 *          callee-saved registers required by each platform's ABI.
 *
 * Supported architectures:
 *   - x86_64:  System V AMD64 ABI
 *   - AArch64: AAPCS64 (ARM 64-bit Procedure Call Standard)
 *   - ARM 32:  AAPCS (ARM Procedure Call Standard) for Cortex-M series
 */

#ifndef CFIBER_CONTEXT_H
#define CFIBER_CONTEXT_H

#include "cfiber/core/macros.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * x86_64 (System V AMD64 ABI)
 * ============================================================================ */
#if defined(__x86_64__)

/**
 * @brief x86_64 context structure.
 * @details Stores callee-saved registers according to the System V AMD64 ABI,
 *          including the floating-point control state (MXCSR control bits and
 *          the x87 control word), so a fiber's rounding mode and exception
 *          masks stay with the fiber. Caller-saved registers (rax, rcx, rdx,
 *          rsi, rdi, r8-r11) are saved by the caller and not preserved here.
 */
typedef struct {
    /** Stack pointer. */
    uint64_t rsp;
    /** Callee-saved general purpose register. */
    uint64_t r15;
    /** Callee-saved general purpose register. */
    uint64_t r14;
    /** Callee-saved general purpose register. */
    uint64_t r13;
    /** Callee-saved general purpose register. */
    uint64_t r12;
    /** Callee-saved general purpose register. */
    uint64_t rbx;
    /** Frame pointer (base pointer). */
    uint64_t rbp;
    /** SSE control and status register. */
    uint32_t mxcsr;
    /** x87 FPU control word. */
    uint16_t x87_cw;
} __attribute__((aligned(16))) cfiber_context_t;

/* ============================================================================
 * AArch64 (AAPCS64)
 * ============================================================================ */
#elif defined(__aarch64__)

/**
 * @brief AArch64 context structure.
 * @details Stores callee-saved registers according to AAPCS64: general purpose
 *          registers x19-x30, the lower 64 bits of v8-v15 and the FPCR, so a
 *          fiber's rounding mode stays with the fiber. Caller-saved registers
 *          (x0-x18) are not preserved as per the calling convention.
 */
typedef struct {
    /** Stack pointer. */
    uint64_t sp;
    /** Callee-saved general purpose register. */
    uint64_t x19;
    /** Callee-saved general purpose register. */
    uint64_t x20;
    /** Callee-saved general purpose register. */
    uint64_t x21;
    /** Callee-saved general purpose register. */
    uint64_t x22;
    /** Callee-saved general purpose register. */
    uint64_t x23;
    /** Callee-saved general purpose register. */
    uint64_t x24;
    /** Callee-saved general purpose register. */
    uint64_t x25;
    /** Callee-saved general purpose register. */
    uint64_t x26;
    /** Callee-saved general purpose register. */
    uint64_t x27;
    /** Callee-saved general purpose register. */
    uint64_t x28;
    /** Frame pointer. */
    uint64_t x29;
    /** Link register (return address). */
    uint64_t x30;

    /* Callee-saved floating point registers (lower 64 bits) */
    double v8;
    double v9;
    double v10;
    double v11;
    double v12;
    double v13;
    double v14;
    double v15;

    /** Floating-point control register (rounding mode, trap enables). */
    uint64_t fpcr;
} __attribute__((aligned(16))) cfiber_context_t;

/* ============================================================================
 * ARM 32-bit (AAPCS for Cortex-M series)
 * ============================================================================ */
#elif defined(__arm__)

/**
 * @brief 32-bit ARM Cortex-M context structure.
 * @details Stores callee-saved registers according to AAPCS. Supports
 *          Cortex-M0/M0+/M3/M4/M7 microcontrollers.
 *
 *          For Cortex-M4F/M7F with FPU:
 *            - When the compiler targets an FPU (__ARM_FP, set by -mfpu with
 *              -mfloat-abi=softfp or hard), s16-s31 and the FPSCR are saved
 *              as well, so a fiber's rounding mode stays with the fiber.
 *            - Registers s0-s15 are caller-saved and not preserved.
 *
 *          The layout follows the compile flags, so the library and its
 *          consumers must agree on -mfloat-abi and -mfpu.
 *
 *          Cortex-M0/M0+/M3 do not have FPU support.
 */
typedef struct {
    /** Stack pointer (r13). */
    uint32_t sp;
    /** Callee-saved general purpose register. */
    uint32_t r4;
    /** Callee-saved general purpose register. */
    uint32_t r5;
    /** Callee-saved general purpose register. */
    uint32_t r6;
    /** Callee-saved register, frame pointer in Thumb mode. */
    uint32_t r7;
    /** Callee-saved general purpose register. */
    uint32_t r8;
    /** Callee-saved general purpose register. */
    uint32_t r9;
    /** Callee-saved general purpose register. */
    uint32_t r10;
    /** Callee-saved general purpose register. */
    uint32_t r11;
    /** Link register (r14) - return address. */
    uint32_t lr;

#ifdef __ARM_FP
    /* Callee-saved floating point registers (Cortex-M4F, M7F) */
    float s16;
    float s17;
    float s18;
    float s19;
    float s20;
    float s21;
    float s22;
    float s23;
    float s24;
    float s25;
    float s26;
    float s27;
    float s28;
    float s29;
    float s30;
    float s31;
    /** Floating-point status and control register. */
    uint32_t fpscr;
#endif
} __attribute__((aligned(8))) cfiber_context_t;

#else
#error "Unsupported architecture"
#endif

/**
 * @brief Switches execution context from one fiber to another.
 * @param old_ctx Receives the current state.
 * @param new_ctx Restored and switched to; only read.
 *
 * @details Performs a low-level context switch by:
 *            1. Saving callee-saved registers to the 'old' context.
 *            2. Restoring callee-saved registers from the 'new' context.
 *            3. Jumping to the restored instruction pointer.
 *
 *          Implemented in architecture-specific assembly and follows the
 *          platform's calling convention. After this function returns,
 *          execution continues where the 'new' context last yielded.
 *
 * @note Typically called by scheduler code, not directly by users.
 * @note new_ctx must hold a state saved by this function or set by cfiber_init().
 *
 * @warning This function modifies the CPU state directly. Ensure proper
 *          stack alignment and valid stack pointers to avoid UB.
 */
CFIBER_EXPORT void cfiber_switch_context(cfiber_context_t* old_ctx, const cfiber_context_t* new_ctx)
    __attribute__((nonnull(1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_CONTEXT_H */
