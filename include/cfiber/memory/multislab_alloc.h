/**
 * @file  multislab_alloc.h
 * @brief Auto-expanding multi-slab allocator for fixed-size blocks.
 * @details Chains multiple slab_t instances together so that allocation never
 *          fails as long as the backing allocator can supply more memory.
 *          Empty slabs are released according to a configurable hysteresis
 *          policy, keeping a small reserve to avoid repeated alloc/free cycles.
 */

#ifndef CFIBER_MULTISLAB_ALLOC_H
#define CFIBER_MULTISLAB_ALLOC_H

#include "cfiber/memory/slab_alloc.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A slab plus its bookkeeping in the multislab chain.
 */
typedef struct slab_node {
    slab_t slab;
    struct slab_node* next;
    struct slab_node* prev;
    /** Fast "is full?" / "is empty?" check. */
    uint32_t used_count;
    /** Pointer to the slab's backing memory (so it can be freed). */
    void* raw_memory;
} slab_node_t;

/**
 * @brief Top-level multislab allocator.
 */
typedef struct {
    /** Head of list of slabs that still have free space. */
    slab_node_t* active;
    /** Head of list of full slabs. */
    slab_node_t* full;
    size_t block_size;
    uint32_t blocks_per_slab;
    size_t slab_memory_size;

    uint32_t slab_count;
    /** 0 = unlimited. */
    uint32_t max_slabs;

    /** Hysteresis: track empty slabs explicitly. */
    uint32_t empty_count;
    uint32_t max_empty_reserve;

    /* Backing allocator */
    void* (*mem_alloc)(size_t size, void* ctx);
    void (*mem_free)(void* ptr, size_t size, void* ctx);
    void* mem_ctx;
} multislab_t;

CFIBER_EXPORT int multislab_init_ext(multislab_t* ms,
                                     size_t block_size,
                                     uint32_t blocks_per_slab,
                                     uint32_t max_slabs,
                                     uint32_t hysteresis_threshold,
                                     void* (*mem_alloc)(size_t, void*),
                                     void (*mem_free)(void*, size_t, void*),
                                     void* mem_ctx);

CFIBER_EXPORT int multislab_init(multislab_t* ms,
                                 size_t block_size,
                                 uint32_t blocks_per_slab,
                                 uint32_t max_slabs,
                                 uint32_t hysteresis_threshold);

[[nodiscard]] CFIBER_EXPORT void* multislab_alloc(multislab_t* ms) __attribute__((nonnull(1)));

CFIBER_EXPORT void multislab_release(multislab_t* ms, void* ptr) __attribute__((nonnull(1, 2)));

/**
 * @brief Destroy a multislab, freeing every slab node and its backing memory.
 * @param ms The multislab to destroy.
 * @note All blocks previously handed out become invalid after this call.
 */
CFIBER_EXPORT void multislab_destroy(multislab_t* ms) __attribute__((nonnull(1)));

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_MULTISLAB_ALLOC_H */
