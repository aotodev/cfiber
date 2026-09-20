/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  stack_sanitize.h
 * @brief Debug-only stack instrumentation for stacks without MMU protection.
 * @details Provides canary overflow detection and watermark-based usage tracking.
 *          Enabled by defining CFIBER_STACK_SANITIZER=1 at build time.
 *          Not intended for use with growable (MMU-backed) stacks.
 *
 *          This is the freestanding / non-MMU (e.g. Cortex-M) counterpart to the
 *          AddressSanitizer integration in cfiber/debug/asan.h, which provides
 *          stronger, instruction-accurate detection on hosted targets. The two
 *          are mutually exclusive: the canary word at mem_base would land inside
 *          the ASan redzone, so enabling both CFIBER_STACK_SANITIZER and
 *          CFIBER_ASAN is rejected at configure time.
 */

#ifndef CFIBER_DEBUG_STACK_SANITIZE_H
#define CFIBER_DEBUG_STACK_SANITIZE_H

#include "cfiber/core/macros.h"
#include "cfiber/stack/stack.h"

#include <stddef.h>
#include <stdint.h>

#ifndef CFIBER_STACK_SANITIZER
#define CFIBER_STACK_SANITIZER 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @return Address of the canary word within the stack memory. */
static inline uint64_t* cfiber_stack_debug_canary_addr(const cfiber_stack_t* s) {
    return (uint64_t*)s->mem_base;
}

/** @return First byte after canary used for watermark painting. */
static inline uint8_t* cfiber_stack_debug_watermark_begin(const cfiber_stack_t* s) {
    return (uint8_t*)cfiber_stack_debug_canary_addr(s) + sizeof(uint64_t);
}

/** @return High limit (exclusive) for watermark accounting. */
static inline uint8_t* cfiber_stack_debug_watermark_end(const cfiber_stack_t* s) {
    return (uint8_t*)s->stack_top;
}

/** @return Total bytes available for watermark painting. */
static inline size_t cfiber_stack_debug_watermark_size(const cfiber_stack_t* s) {
    uint8_t* begin = cfiber_stack_debug_watermark_begin(s);
    uint8_t* end = cfiber_stack_debug_watermark_end(s);
    return (end > begin) ? (size_t)(end - begin) : 0u;
}

#if CFIBER_STACK_SANITIZER

/** @brief Writes the canary and paints the watermark region. */
CFIBER_EXPORT void cfiber_stack_debug_init(const cfiber_stack_t* s);

/** @return Non-zero if the canary word is intact. */
CFIBER_EXPORT int cfiber_stack_debug_check_canary(const cfiber_stack_t* s);

/**
 * @return Peak bytes used since init, from the highest byte of the watermark
 *         region no longer holding the pattern; (size_t)-1 if the canary is
 *         gone. A fiber that legitimately writes the pattern byte at its
 *         deepest point under-reports by that much.
 */
CFIBER_EXPORT size_t cfiber_stack_debug_used_bytes(const cfiber_stack_t* s);

/**
 * @return Non-zero if the stack overflowed: the canary is gone, or the
 *         watermark is used down to the canary, which a frame stepping past
 *         a single word would leave intact.
 */
CFIBER_EXPORT int cfiber_stack_debug_overflowed(const cfiber_stack_t* s);

#else
static inline void cfiber_stack_debug_init(const cfiber_stack_t* s) {
    (void)s;
}
static inline int cfiber_stack_debug_check_canary(const cfiber_stack_t* s) {
    (void)s;
    return 1;
}
static inline size_t cfiber_stack_debug_used_bytes(const cfiber_stack_t* s) {
    (void)s;
    return 0u;
}
static inline int cfiber_stack_debug_overflowed(const cfiber_stack_t* s) {
    (void)s;
    return 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_DEBUG_STACK_SANITIZE_H */
