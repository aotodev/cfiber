/**
 * @file slab_alloc.h
 * @brief Simple slab allocator for fixed-size stacks with user-provided memory.
 * @details Intended for freestanding/embedded environments where the user
 *          provides a contiguous block of memory to be carved into stacks.
 */

#ifndef CFIBER_SLAB_ALLOC_H
#define CFIBER_SLAB_ALLOC_H

#include "cfiber/core/macros.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BITMAP_SIZE
#define BITMAP_SIZE 8U
#endif

constexpr uint32_t MAX_BLOCK_COUNT = BITMAP_WORD_BITS * BITMAP_SIZE;

typedef struct slab_struct {
    /** User-provided slab base. */
    void* memory;
    /** Size of each block. */
    size_t block_size;
    /** Total number of blocks. */
    uint32_t block_count;
    bitmap_t bitmap[BITMAP_SIZE];
    uint32_t bitmap_count;
} slab_t;

/**
 * @brief Initialize a slab allocator with user-provided memory.
 * @param alloc       The allocator to initialize.
 * @param block_size  Size of each block (must be a multiple of CACHE_LINE_SIZE).
 * @param memory      Pointer to the memory region.
 * @param memory_size Size of the memory region in bytes.
 * @return 0 on success, -1 on error.
 */
CFIBER_EXPORT int slab_init(slab_t* alloc, size_t block_size, void* memory, size_t memory_size)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Allocate a block from the slab.
 * @param alloc The allocator to use.
 * @return Pointer to the block, or nullptr if no free block is available.
 */
[[nodiscard]] CFIBER_EXPORT void* slab_alloc(slab_t* alloc) __attribute__((nonnull(1)));

/**
 * @brief Release a block back to the slab.
 * @param alloc The allocator to use.
 * @param block The block to release.
 */
CFIBER_EXPORT void slab_release(slab_t* alloc, void* block) __attribute__((nonnull(1, 2)));

/**
 * @brief Reset the allocator bookkeeping (user still owns the backing memory).
 * @param alloc The allocator to reset.
 */
CFIBER_EXPORT void slab_reset(slab_t* alloc) __attribute__((nonnull(1)));

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_SLAB_ALLOC_H */
