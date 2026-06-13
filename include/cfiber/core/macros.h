/**
 * @file  macros.h
 * @brief Compiler attributes, branch-prediction hints and bitmap word type.
 */

#ifndef CFIBER_MACROS_H
#define CFIBER_MACROS_H

#include <stddef.h>
#include <stdint.h>

#define CFIBER_EXPORT __attribute__((visibility("default")))

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#ifdef NDEBUG
#define ASSERT(expr) ((void)0)
#else
#define ASSERT(expr) ((expr) ? (void)0 : __builtin_trap())
#endif

/**
 * @brief Toggle for defensive runtime checks (bounds, double-free, etc).
 * @details Always on by default; define to 0 at build time to opt out.
 */
#ifndef CFIBER_DEFENSIVE
#define CFIBER_DEFENSIVE 1
#endif

#if defined(__arm__) && !defined(__aarch64__)
#define CACHE_LINE_SIZE 32
#else
#define CACHE_LINE_SIZE 64
#endif

#if defined(__arm__) || defined(__thumb__)
#define BITMAP_WORD_BITS 32
typedef uint32_t bitmap_t;
#else
static_assert(sizeof(void*) == sizeof(uint64_t));
#define BITMAP_WORD_BITS 64
typedef uint64_t bitmap_t;
#endif

static inline size_t align_up(size_t v, size_t align) {
    return (v + align - 1) & ~(align - 1);
}

#endif /* CFIBER_MACROS_H */
