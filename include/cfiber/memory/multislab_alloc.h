/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  multislab_alloc.h
 * @brief Auto-expanding multi-slab allocator for fixed-size blocks.
 * @details Chains multiple cfiber_slab_t instances together so that allocation never
 *          fails as long as the backing allocator can supply more memory.
 *          Empty slabs are released according to a configurable hysteresis
 *          policy, keeping a small reserve to avoid repeated alloc/free cycles.
 */

#ifndef CFIBER_MULTISLAB_ALLOC_H
#define CFIBER_MULTISLAB_ALLOC_H

#include "cfiber/memory/slab_alloc.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A slab plus its bookkeeping in the multislab chain.
 */
typedef struct cfiber_slab_node {
    cfiber_slab_t slab;
    struct cfiber_slab_node* next;
    struct cfiber_slab_node* prev;
    /** Fast "is full?" / "is empty?" check. */
    uint32_t used_count;
    /** Which list the node currently lives in (true = full list).
     *  Tracked explicitly because a slab can be full while still on the
     *  active list (slabs are moved to the full list lazily). */
    bool is_full;
    /** Pointer to the slab's backing memory (so it can be freed). */
    void* raw_memory;
} cfiber_slab_node_t;

/**
 * @brief Top-level multislab allocator.
 */
typedef struct {
    /** Head of list of slabs that still have free space. */
    cfiber_slab_node_t* active;
    /** Head of list of full slabs. */
    cfiber_slab_node_t* full;
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
} cfiber_multislab_t;

/**
 * @brief Initialise a multislab over a caller-supplied backing allocator.
 * @param block_size           Multiple of CFIBER_CACHE_LINE_SIZE.
 * @param blocks_per_slab      1..CFIBER_SLAB_MAX_BLOCKS; block_size * blocks_per_slab
 *                             must not overflow.
 * @param max_slabs            Growth cap; 0 = unlimited.
 * @param hysteresis_threshold Empty slabs kept before one is returned.
 * @param mem_alloc            Returns a block of the requested size aligned to
 *                             at least alignof(max_align_t), or NULL. Cache-line
 *                             alignment keeps fiber stacks off shared lines.
 * @param mem_free             Receives the pointer and the size it was
 *                             allocated with.
 * @return 0, or -1 on an invalid configuration (nothing is allocated).
 */
CFIBER_EXPORT int cfiber_multislab_init_ext(cfiber_multislab_t* ms,
                                            size_t block_size,
                                            uint32_t blocks_per_slab,
                                            uint32_t max_slabs,
                                            uint32_t hysteresis_threshold,
                                            void* (*mem_alloc)(size_t, void*),
                                            void (*mem_free)(void*, size_t, void*),
                                            void* mem_ctx) __attribute__((nonnull(1)));

/** @brief cfiber_multislab_init_ext() over a cache-line-aligned heap allocator. */
CFIBER_EXPORT int cfiber_multislab_init(cfiber_multislab_t* ms,
                                        size_t block_size,
                                        uint32_t blocks_per_slab,
                                        uint32_t max_slabs,
                                        uint32_t hysteresis_threshold);

/**
 * @brief Take a block, growing by one slab when none is free.
 * @return The block, or NULL when max_slabs is reached or mem_alloc fails.
 */
[[nodiscard]] CFIBER_EXPORT void* cfiber_multislab_alloc(cfiber_multislab_t* ms) __attribute__((nonnull(1)));

/**
 * @brief Return a block to its slab. A slab left empty is freed once more than
 *        hysteresis_threshold empty slabs are held.
 */
CFIBER_EXPORT void cfiber_multislab_release(cfiber_multislab_t* ms, void* ptr) __attribute__((nonnull(1, 2)));

/**
 * @brief Destroy a multislab, freeing every slab node and its backing memory.
 * @param ms The multislab to destroy.
 * @note All blocks previously handed out become invalid after this call.
 */
CFIBER_EXPORT void cfiber_multislab_destroy(cfiber_multislab_t* ms) __attribute__((nonnull(1)));

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_MULTISLAB_ALLOC_H */
