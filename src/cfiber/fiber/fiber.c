#include "cfiber/fiber/fiber.h"

#include "cfiber/core/macros.h"

/* ============================================================================
 * Fiber-return hook registration
 *
 * The epilogue dispatches through a runtime-registered hook rather than a
 * link-time symbol, so schedulers can be swapped in at run time, coexist in one
 * process, and work through a shared library. Storage is thread-local on hosted
 * targets and a plain global on bare-metal ARM Cortex-M (no TLS), mirroring the
 * built-in scheduler's current-scheduler storage.
 * ============================================================================ */
#if defined(__arm__) && !defined(__aarch64__)
static cfiber_return_hook_t s_return_hook;
#else
static thread_local cfiber_return_hook_t s_return_hook;
#endif

cfiber_return_hook_t cfiber_set_return_hook(cfiber_return_hook_fn fn, void* ctx) {
    cfiber_return_hook_t prev = s_return_hook;
    s_return_hook.fn = fn;
    s_return_hook.ctx = ctx;
    return prev;
}

cfiber_return_hook_t cfiber_get_return_hook(void) {
    return s_return_hook;
}

/**
 * @brief Invokes the registered fiber-return hook when a fiber returns.
 * @details Runs on the returning fiber's stack. The hook (registered via
 *          cfiber_set_return_hook()) must not return.
 */
// NOLINTNEXTLINE(misc-use-internal-linkage): false positive — called from per-arch assembly
[[noreturn]] void fiber_epilogue(void) {
    ASSERT(s_return_hook.fn && "fiber returned with no fiber-return hook registered");
    s_return_hook.fn(s_return_hook.ctx);

    /* Should never reach here — scheduler bug if we do. */
    ASSERT(false);

    for (;;) {
#ifdef __x86_64__
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

#elifdef __aarch64__
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

#elifdef __arm__
    /* Align to an 8-byte boundary per AAPCS ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~7U);

    fiber->ctx.sp = (uint32_t)stack_ptr;

    /* Store func + user_data in callee-saved registers (see note above). */
    fiber->ctx.r4 = (uint32_t)func;
    fiber->ctx.r5 = (uint32_t)user_data;

    fiber->ctx.lr = (uint32_t)&fiber_prologue;
#endif
}
