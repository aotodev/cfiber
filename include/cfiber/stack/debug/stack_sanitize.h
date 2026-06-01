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
static inline uint64_t* cstack_debug_canary_addr(const cstack_t* s) {
    return (uint64_t*)s->mem_base;
}

/** @return First byte after canary used for watermark painting. */
static inline uint8_t* cstack_debug_watermark_begin(const cstack_t* s) {
    return (uint8_t*)cstack_debug_canary_addr(s) + sizeof(uint64_t);
}

/** @return High limit (exclusive) for watermark accounting. */
static inline uint8_t* cstack_debug_watermark_end(const cstack_t* s) {
    return (uint8_t*)s->stack_top;
}

/** @return Total bytes available for watermark painting. */
static inline size_t cstack_debug_watermark_size(const cstack_t* s) {
    uint8_t* begin = cstack_debug_watermark_begin(s);
    uint8_t* end = cstack_debug_watermark_end(s);
    return (end > begin) ? (size_t)(end - begin) : 0u;
}

#if CFIBER_STACK_SANITIZER

CFIBER_EXPORT void cstack_debug_stack_init(const cstack_t* s);
CFIBER_EXPORT int cstack_debug_stack_check_canary(const cstack_t* s);
CFIBER_EXPORT size_t cstack_debug_stack_used_bytes(const cstack_t* s);

#else
static inline void cstack_debug_stack_init(const cstack_t* s) {
    (void)s;
}
static inline int cstack_debug_stack_check_canary(const cstack_t* s) {
    (void)s;
    return 1;
}
static inline size_t cstack_debug_stack_used_bytes(const cstack_t* s) {
    (void)s;
    return 0u;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_DEBUG_STACK_SANITIZE_H */
