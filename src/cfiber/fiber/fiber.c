#include "cfiber/fiber/fiber.h"

#include "cfiber/core/internal.h"

#include <string.h>

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

/* No hook, or a hook that returned: there is no return address on this stack. */
[[noreturn]] void cfiber_epilogue(void) {
    if (LIKELY(s_return_hook.fn)) {
        s_return_hook.fn(s_return_hook.ctx);
    }
    __builtin_trap();
}

void cfiber_init(cfiber_t* const fiber, cfiber_fn const func, void* const user_data) {
    ASSERT(fiber->stack);
    /* Minimal stack size to avoid certain overflow. */
    ASSERT(fiber->stack_size >= 256);

    uintptr_t stack_base = (uintptr_t)fiber->stack;
    /* Guard against pointer arithmetic overflow. */
    ASSERT(stack_base <= UINTPTR_MAX - fiber->stack_size);
    uintptr_t stack_top = stack_base + fiber->stack_size;

    /* Slots not set below start at zero: no stale frame-pointer chain, nothing
     * indeterminate for the first switch to load. */
    fiber->ctx = (cfiber_context_t){0};

#ifdef __x86_64__
    /* Align to a 16-byte boundary per System V AMD64 ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~15ULL);

    fiber->ctx.rbx = (uint64_t)func;
    fiber->ctx.r12 = (uint64_t)user_data;

    /* A new fiber inherits the creator's FP control state, as a new thread
     * would. Zero would unmask every exception. */
    __asm__ volatile("stmxcsr %0"
                     : "=m"(fiber->ctx.mxcsr));
    __asm__ volatile("fnstcw %0"
                     : "=m"(fiber->ctx.x87_cw));

    /* The first ret pops cfiber_prologue. memcpy: the stack may be a uint8_t array. */
    const uint64_t ret = (uint64_t)cfiber_prologue;
    stack_ptr -= sizeof ret;
    memcpy(stack_ptr, &ret, sizeof ret);

    fiber->ctx.rsp = (uint64_t)stack_ptr;

#elifdef __aarch64__
    /* Align to a 16-byte boundary per AAPCS64 ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~15ULL);

    fiber->ctx.sp = (uint64_t)stack_ptr;

    /* Store func + user_data in callee-saved registers so they remain valid
     * across the call into cfiber_prologue. */
    fiber->ctx.x19 = (uint64_t)func;
    fiber->ctx.x20 = (uint64_t)user_data;

    /* Link register points to the fiber startup routine. */
    fiber->ctx.x30 = (uint64_t)&cfiber_prologue;

    /* A new fiber inherits the creator's FP control state. */
    __asm__ volatile("mrs %0, fpcr"
                     : "=r"(fiber->ctx.fpcr));

#elifdef __arm__
    /* Align to an 8-byte boundary per AAPCS ABI. */
    uint8_t* stack_ptr = (uint8_t*)(stack_top & ~7U);

    fiber->ctx.sp = (uint32_t)stack_ptr;

    /* Store func + user_data in callee-saved registers (see note above). */
    fiber->ctx.r4 = (uint32_t)func;
    fiber->ctx.r5 = (uint32_t)user_data;

    fiber->ctx.lr = (uint32_t)&cfiber_prologue;

#ifdef __ARM_FP
    /* A new fiber inherits the creator's FP control state. */
    __asm__ volatile("vmrs %0, fpscr"
                     : "=r"(fiber->ctx.fpscr));
#endif
#endif
}
