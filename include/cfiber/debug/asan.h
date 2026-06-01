/**
 * @file  asan.h
 * @brief AddressSanitizer integration for fiber stacks and context switches.
 *
 * @details Two complementary pieces, both active only when the translation unit
 *          is compiled with AddressSanitizer (`-fsanitize=address`):
 *
 *            1. Manual poisoning of a guard "redzone" placed below each fiber
 *               stack.  The scheduler packs many fiber stacks contiguously in a
 *               single malloc'd slab; without a poisoned guard, a fiber that
 *               overruns its stack silently corrupts its neighbour and ASan sees
 *               nothing.  A poisoned redzone turns that overflow into an
 *               instruction-accurate ASan report.
 *
 *            2. The fiber-switch annotation pair
 *               (__sanitizer_start_switch_fiber / __sanitizer_finish_switch_fiber).
 *               When we swap the stack pointer in switch_context(), ASan's notion
 *               of "the current stack" goes stale, which breaks
 *               detect_stack_use_after_return and produces false reports.  The
 *               annotations keep ASan in sync across every context switch.
 *
 * @note This is the hosted counterpart to the canary/watermark stack sanitizer
 *       (cfiber/stack/debug/stack_sanitize.h), which targets freestanding /
 *       non-MMU (Cortex-M) builds where ASan is unavailable.  The two are
 *       mutually exclusive (the canary word would land inside the ASan redzone);
 *       the build system enforces this.
 *
 * @note When ASan is not enabled every entry point degrades to a zero-overhead
 *       inline that simply forwards to switch_context(), so callers need no
 *       conditional compilation of their own.
 */

#ifndef CFIBER_DEBUG_ASAN_H
#define CFIBER_DEBUG_ASAN_H

#include "cfiber/core/macros.h"
#include "cfiber/fiber/context.h"

#include <stddef.h>

/* GCC and modern Clang define __SANITIZE_ADDRESS__ for -fsanitize=address.
 * Older Clang only exposes it through __has_feature(address_sanitizer). */
#if defined(__SANITIZE_ADDRESS__)
#define CFIBER_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CFIBER_ASAN_ENABLED 1
#endif
#endif

#ifndef CFIBER_ASAN_ENABLED
#define CFIBER_ASAN_ENABLED 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* always_inline is honoured even at -O0 by GCC and Clang, so the wrappers below
 * leave no call behind regardless of optimisation level: when ASan is disabled
 * they vanish entirely, and when enabled they collapse straight into the
 * underlying __asan_* call with no cfiber frame in the way. */
#define CFIBER_ASAN_INLINE static inline __attribute__((always_inline))

#if CFIBER_ASAN_ENABLED

#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>

/**
 * @brief Size of the poisoned guard placed below each fiber stack, in bytes.
 * @details Defaults to one cache line, which is enough to catch the common
 *          small downward overflow before it corrupts the adjacent stack. A
 *          runaway that overshoots by more than this still faults, just via the
 *          hard stack-overflow path rather than a clean redzone report.
 *
 *          Override at build time to widen the guard, e.g.
 *          `-DCFIBER_ASAN_REDZONE='((size_t)256)'` or the CMake cache variable
 *          `CFIBER_ASAN_REDZONE`. It must be defined consistently across the
 *          library and any consumer, since it determines the stack block layout.
 */
#ifndef CFIBER_ASAN_REDZONE
#define CFIBER_ASAN_REDZONE ((size_t)CACHE_LINE_SIZE)
#endif

/** @brief Poison @p size bytes starting at @p addr (mark as off-limits). */
CFIBER_ASAN_INLINE void cfiber_asan_poison(const void* addr, size_t size) {
    ASAN_POISON_MEMORY_REGION(addr, size);
}

/** @brief Unpoison @p size bytes starting at @p addr (mark as usable). */
CFIBER_ASAN_INLINE void cfiber_asan_unpoison(const void* addr, size_t size) {
    ASAN_UNPOISON_MEMORY_REGION(addr, size);
}

/**
 * @brief ASan-aware context switch wrapper around switch_context().
 * @param from          Context to save the outgoing fiber into.
 * @param to            Context to restore and switch to.
 * @param to_stack_low  Lowest address of the target fiber's usable stack.
 * @param to_stack_size Size of the target fiber's usable stack in bytes.
 * @param finishing     true if the outgoing fiber is terminating; ASan then
 *                      discards its fake stack instead of saving it.
 *
 * @details Brackets switch_context() with __sanitizer_start_switch_fiber()
 *          (before) and __sanitizer_finish_switch_fiber() (after we are resumed
 *          on the outgoing fiber's stack).  The fake-stack save slot is a frame
 *          local, which is correct because start/finish always bracket the same
 *          switch within one stack frame.
 *
 * @note Fresh fibers do not resume inside this wrapper; their first
 *       __sanitizer_finish_switch_fiber() is issued by
 *       cfiber_asan_on_fiber_entry() from the assembly prologue instead.
 */
CFIBER_EXPORT void
cfiber_asan_switch(context_t* from, context_t* to, const void* to_stack_low, size_t to_stack_size, bool finishing)
    __attribute__((nonnull(1, 2)));

/**
 * @brief First-entry hook invoked from the fiber assembly prologue.
 * @details Issues the __sanitizer_finish_switch_fiber() that pairs with the
 *          start() the scheduler made when entering this fresh fiber, and
 *          records the host (scheduler) stack bounds the first time it runs.
 *          Fibers are only ever entered from the scheduler loop, so the stack we
 *          were switched away from is always the host stack.
 */
CFIBER_EXPORT void cfiber_asan_on_fiber_entry(void);

/**
 * @brief Retrieve the captured host (scheduler) stack bounds.
 * @param low  Out: lowest address of the host stack (NULL/0 if not yet known).
 * @param size Out: size of the host stack in bytes.
 * @details Used by fiber->host switches to tell ASan which stack we are
 *          returning to.
 */
CFIBER_EXPORT void cfiber_asan_host_bounds(const void** low, size_t* size) __attribute__((nonnull));

#else /* !CFIBER_ASAN_ENABLED */

/* Force the guard to zero even if the build injected a size: with no ASan there
 * is nothing to poison, and a nonzero value would needlessly grow stack blocks. */
#ifdef CFIBER_ASAN_REDZONE
#undef CFIBER_ASAN_REDZONE
#endif
#define CFIBER_ASAN_REDZONE ((size_t)0)

CFIBER_ASAN_INLINE void cfiber_asan_poison(const void* addr, size_t size) {
    (void)addr;
    (void)size;
}

CFIBER_ASAN_INLINE void cfiber_asan_unpoison(const void* addr, size_t size) {
    (void)addr;
    (void)size;
}

CFIBER_ASAN_INLINE void
cfiber_asan_switch(context_t* from, context_t* to, const void* to_stack_low, size_t to_stack_size, bool finishing) {
    (void)to_stack_low;
    (void)to_stack_size;
    (void)finishing;
    switch_context(from, to);
}

CFIBER_ASAN_INLINE void cfiber_asan_host_bounds(const void** low, size_t* size) {
    *low = nullptr;
    *size = 0;
}

#endif /* CFIBER_ASAN_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_DEBUG_ASAN_H */
