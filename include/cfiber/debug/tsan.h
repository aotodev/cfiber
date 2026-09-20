/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  tsan.h
 * @brief ThreadSanitizer fiber annotations for hand-rolled context switches.
 *
 * @details TSan keeps a per-thread shadow stack and vector clock. Swapping the
 *          stack pointer underneath it interleaves frames of different fibers
 *          and misattributes reports. The __tsan_*_fiber calls give each fiber
 *          its own TSan context and are issued right before every switch.
 *
 *          Active only when the build defines CFIBER_TSAN_ENABLED, which
 *          CFIBER_TSAN does for the reactor and everything linked to it; the
 *          core library is not instrumented. Otherwise every entry point is an
 *          empty inline.
 */

#ifndef CFIBER_DEBUG_TSAN_H
#define CFIBER_DEBUG_TSAN_H

#include <stddef.h>

#ifndef CFIBER_TSAN_ENABLED
#define CFIBER_TSAN_ENABLED 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CFIBER_TSAN_INLINE static inline __attribute__((always_inline))

#if CFIBER_TSAN_ENABLED

#include <sanitizer/tsan_interface.h>

/** @brief The TSan context of the calling stack (a thread or a fiber). */
CFIBER_TSAN_INLINE void* cfiber_tsan_current(void) {
    return __tsan_get_current_fiber();
}

/** @brief A TSan context for a new fiber. */
CFIBER_TSAN_INLINE void* cfiber_tsan_create(void) {
    return __tsan_create_fiber(0);
}

/** @brief Frees a context; must not be the current one. NULL-safe. */
CFIBER_TSAN_INLINE void cfiber_tsan_destroy(void* fiber) {
    if (fiber) {
        __tsan_destroy_fiber(fiber);
    }
}

/** @brief Call immediately before switching to the stack that owns @p fiber. */
CFIBER_TSAN_INLINE void cfiber_tsan_switch_to(void* fiber) {
    __tsan_switch_to_fiber(fiber, 0);
}

#else /* !CFIBER_TSAN_ENABLED */

CFIBER_TSAN_INLINE void* cfiber_tsan_current(void) {
    return nullptr;
}

CFIBER_TSAN_INLINE void* cfiber_tsan_create(void) {
    return nullptr;
}

CFIBER_TSAN_INLINE void cfiber_tsan_destroy(void* fiber) {
    (void)fiber;
}

CFIBER_TSAN_INLINE void cfiber_tsan_switch_to(void* fiber) {
    (void)fiber;
}

#endif /* CFIBER_TSAN_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_DEBUG_TSAN_H */
