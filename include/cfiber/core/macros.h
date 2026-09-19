/**
 * @file  macros.h
 * @brief Public build constants: export attribute, defensive toggle, cache line
 *        size and the slab bitmap word type.
 */

#ifndef CFIBER_MACROS_H
#define CFIBER_MACROS_H

#include <stdint.h>

#define CFIBER_EXPORT __attribute__((visibility("default")))

/**
 * @brief Toggle for defensive runtime checks (bounds, double-free, etc).
 * @details Always on by default; define to 0 at build time to opt out.
 */
#ifndef CFIBER_DEFENSIVE
#define CFIBER_DEFENSIVE 1
#endif

#if defined(__arm__) && !defined(__aarch64__)
#define CFIBER_CACHE_LINE_SIZE 32
#else
#define CFIBER_CACHE_LINE_SIZE 64
#endif

#if defined(__arm__) || defined(__thumb__)
#define CFIBER_BITMAP_WORD_BITS 32
typedef uint32_t cfiber_bitmap_t;
#else
static_assert(sizeof(void*) == sizeof(uint64_t));
#define CFIBER_BITMAP_WORD_BITS 64
typedef uint64_t cfiber_bitmap_t;
#endif

#endif /* CFIBER_MACROS_H */
