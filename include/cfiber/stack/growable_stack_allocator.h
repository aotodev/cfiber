/**
 * @file  growable_stack_allocator.h
 * @brief Pooled allocator for MMU-protected growable stacks.
 */

#ifndef CFIBER_GROWABLE_STACK_ALLOCATOR_H
#define CFIBER_GROWABLE_STACK_ALLOCATOR_H

#include "cfiber/core/macros.h"
#include "cfiber/stack/stack.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @details Manages a cache of demand-paged growable stacks. Recycling madvises the
 *          grown pages away instead of unmapping, so reuse costs no mmap.
 */
typedef struct cfiber_growable_stack_allocator cfiber_growable_stack_allocator_t;

typedef struct {
    /** Maximum size for each stack. */
    size_t max_stack_size;
    /** Maximum number of stacks to cache. */
    size_t cache_capacity;
    /** Number of stacks to pre-allocate in the cache. */
    size_t initial_cached;
} cfiber_growable_stack_allocator_args_t;

/**
 * @brief Create a new growable stack allocator with pooling.
 * @param args Configuration parameters.
 * @return Pointer to the allocator, or nullptr on failure.
 */
[[nodiscard]] CFIBER_EXPORT cfiber_growable_stack_allocator_t*
cfiber_growable_stack_allocator_create(cfiber_growable_stack_allocator_args_t args);

/**
 * @brief Destroy the growable stack allocator.
 * @param alloc The allocator to destroy.
 * @return 0 on success, -1 if there are leaked stacks.
 */
CFIBER_EXPORT int cfiber_growable_stack_allocator_destroy(cfiber_growable_stack_allocator_t* alloc)
    __attribute__((nonnull(1)));

/**
 * @brief Allocate a growable stack from the pool.
 * @param alloc The allocator to use.
 * @return A cfiber_stack_t descriptor, or an invalid descriptor on failure.
 */
[[nodiscard]] CFIBER_EXPORT cfiber_stack_t cfiber_growable_stack_alloc(cfiber_growable_stack_allocator_t* alloc)
    __attribute__((nonnull(1)));

/**
 * @brief Release a growable stack back to the pool.
 * @details The stack is recycled (physical pages released but VMA kept)
 *          and placed back in the cache if space is available. A stack that
 *          did not come from this pool (its size differs) asserts in debug
 *          builds; in release it is unmapped rather than pooled.
 * @param alloc The allocator to use.
 * @param stack The stack to release.
 */
CFIBER_EXPORT void cfiber_growable_stack_release(cfiber_growable_stack_allocator_t* alloc, cfiber_stack_t* stack)
    __attribute__((nonnull(1, 2)));

#ifdef __cplusplus
}
#endif

#endif // CFIBER_GROWABLE_STACK_ALLOCATOR_H
