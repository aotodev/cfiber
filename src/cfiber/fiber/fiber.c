#include "cfiber/fiber/fiber.h"

/* assert.h is not a freestanding header; roll our own. */
#ifdef NDEBUG
#define ASSERT(expr) ((void)0)
#else
#define ASSERT(expr) ((expr) ? (void)0 : __builtin_trap())
#endif

/**
 * @brief Calls the scheduler-defined epilogue when a fiber returns.
 * @details Implemented by the scheduler (see scheduler_return_fiber()).
 */
[[noreturn]] void fiber_epilogue(void) {
    scheduler_return_fiber();

    /* Should never reach here — scheduler bug if we do. */
    ASSERT(false);

    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile("hlt");
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ volatile("wfi");
#endif
    }
}

/**
 * @brief Fiber init prologue.
 * @details Sets the user function prolog and invokes it with the user data
 *          pointer as its first argument; after it returns, calls the noreturn
 *          fiber_epilogue.
 * @note Implemented in assembly rather than inline asm because this routine
 *       makes function calls, which can clobber all registers. Setting it up
 *       in inline asm would be verbose and error prone.
 */
extern void fiber_prologue(void);

void init_fiber(fiber_t* const fiber, fiber_fn const func, void* const user_data) {
    ASSERT(fiber->stack);
    /* Minimal stack size to avoid certain overflow. */
    ASSERT(fiber->stack_size >= 256);

    uintptr_t stack_base = (uintptr_t)fiber->stack;
    /* Guard against pointer arithmetic overflow. */
    ASSERT(stack_base <= UINTPTR_MAX - fiber->stack_size);
    uintptr_t stack_top = stack_base + fiber->stack_size;

#ifdef __x86_64__
    /* Align to a 16-byte boundary per System V AMD64 ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~15ULL);

    fiber->ctx.rbp = 0;
    fiber->ctx.rbx = (uint64_t)func;
    fiber->ctx.r12 = (uint64_t)user_data;

    /* Arrange for the first ret to jump into fiber_prologue. */
    stack_ptr -= 8;
    *(uint64_t*)stack_ptr = (uint64_t)fiber_prologue;

    fiber->ctx.rsp = (uint64_t)stack_ptr;

#elif defined(__aarch64__)
    /* Align to a 16-byte boundary per AAPCS64 ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~15ULL);

    fiber->ctx.sp = (uint64_t)stack_ptr;
    fiber->ctx.x29 = 0;

    /* Store func + user_data in callee-saved registers so they remain valid
     * across the call into fiber_prologue. */
    fiber->ctx.x19 = (uint64_t)func;
    fiber->ctx.x20 = (uint64_t)user_data;

    /* Link register points to the fiber startup routine. */
    fiber->ctx.x30 = (uint64_t)&fiber_prologue;

#elif defined(__arm__)
    /* Align to an 8-byte boundary per AAPCS ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~7U);

    fiber->ctx.sp = (uint32_t)stack_ptr;

    /* Store func + user_data in callee-saved registers (see note above). */
    fiber->ctx.r4 = (uint32_t)func;
    fiber->ctx.r5 = (uint32_t)user_data;

    fiber->ctx.lr = (uint32_t)&fiber_prologue;
#endif
}
