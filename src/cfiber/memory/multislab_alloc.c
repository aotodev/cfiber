#include "cfiber/memory/multislab_alloc.h"

#include <stdlib.h>
#include <string.h>

static void* default_alloc(const size_t size, void* ctx) {
    (void)ctx;
    return malloc(size);
}

static void default_free(void* const ptr, size_t size, void* ctx) {
    (void)size;
    (void)ctx;
    free(ptr);
}

static slab_node_t* multislab_grow(multislab_t* ms) {
    if (ms->max_slabs && ms->slab_count >= ms->max_slabs) {
        return nullptr;
    }

    slab_node_t* const node = ms->mem_alloc(sizeof(slab_node_t), ms->mem_ctx);
    if (UNLIKELY(!node)) {
        return nullptr;
    }

    void* const mem = ms->mem_alloc(ms->slab_memory_size, ms->mem_ctx);
    if (UNLIKELY(!mem)) {
        ms->mem_free(node, sizeof(slab_node_t), ms->mem_ctx);
        return nullptr;
    }

    const int res = slab_init(&node->slab, ms->block_size, mem, ms->slab_memory_size);
    if (UNLIKELY(res != 0)) {
        ms->mem_free(mem, ms->slab_memory_size, ms->mem_ctx);
        ms->mem_free(node, sizeof(slab_node_t), ms->mem_ctx);
        return nullptr;
    }

    node->raw_memory = mem;
    node->used_count = 0;
    node->is_full = false;
    node->next = ms->active;
    node->prev = nullptr;

    if (ms->active) {
        ms->active->prev = node;
    }

    ms->active = node;
    ms->slab_count++;

    return node;
}

static slab_node_t* find_owning_slab(const multislab_t* const ms, const void* const ptr) {
    const uintptr_t p = (uintptr_t)ptr;

    /* check active list */
    for (slab_node_t* n = ms->active; n; n = n->next) {
        const uintptr_t base = (uintptr_t)n->raw_memory;
        if (p >= base && p < base + ms->slab_memory_size) {
            return n;
        }
    }

    /* check full list */
    for (slab_node_t* n = ms->full; n; n = n->next) {
        const uintptr_t base = (uintptr_t)n->raw_memory;
        if (p >= base && p < base + ms->slab_memory_size) {
            return n;
        }
    }

    return nullptr;
}

int multislab_init_ext(multislab_t* const ms,
                       const size_t block_size,
                       const uint32_t blocks_per_slab,
                       const uint32_t max_slabs,
                       const uint32_t hysteresis_threshold,
                       void* (*mem_alloc)(size_t, void*),
                       void (*mem_free)(void*, size_t, void*),
                       void* mem_ctx) {
    if (block_size < CACHE_LINE_SIZE || blocks_per_slab == 0 || blocks_per_slab > MAX_BLOCK_COUNT) {
        return -1;
    }

    memset(ms, 0, sizeof(*ms));
    ms->block_size = block_size;
    ms->blocks_per_slab = blocks_per_slab;
    ms->slab_memory_size = block_size * blocks_per_slab;
    ms->max_slabs = max_slabs;
    ms->max_empty_reserve = hysteresis_threshold;
    ms->mem_alloc = mem_alloc;
    ms->mem_free = mem_free;
    ms->mem_ctx = mem_ctx;

    return 0;
}

int multislab_init(multislab_t* ms,
                   const size_t block_size,
                   const uint32_t blocks_per_slab,
                   const uint32_t max_slabs,
                   const uint32_t hysteresis_threshold) {
    return multislab_init_ext(
        ms, block_size, blocks_per_slab, max_slabs, hysteresis_threshold, default_alloc, default_free, nullptr);
}

void* multislab_alloc(multislab_t* ms) {
    /* try the active list */
    if (UNLIKELY(!ms->active)) {
        if (UNLIKELY(!multislab_grow(ms))) {
            return nullptr;
        }
    }

    slab_node_t* node = ms->active;
    void* ptr = slab_alloc(&node->slab);

    if (UNLIKELY(!ptr)) {
        /* this node is full, move it to the full list */
        ms->active = node->next;
        if (ms->active) {
            ms->active->prev = nullptr;
        }

        node->next = ms->full;
        node->prev = nullptr;
        if (ms->full) {
            ms->full->prev = node;
        }
        ms->full = node;
        node->is_full = true;

        /* grow and retry */
        if (!multislab_grow(ms)) {
            return nullptr;
        }
        node = ms->active;
        ptr = slab_alloc(&node->slab);
    }

    if (LIKELY(ptr)) {
        node->used_count++;
    }
    return ptr;
}

void multislab_release(multislab_t* const ms, void* const ptr) {
    slab_node_t* node = find_owning_slab(ms, ptr);
    if (UNLIKELY(!node)) {
        ASSERT(false && "pointer not owned by this allocator");
        return;
    }

    /* Use the tracked list membership rather than inferring it from
     * used_count: a slab can be full (used_count == blocks_per_slab) while
     * still on the active list, because slabs migrate to the full list
     * lazily on the next allocation. */
    const bool was_full = node->is_full;

    slab_release(&node->slab, ptr);
    node->used_count--;

    /* If it was full, move it back to the active list */
    if (UNLIKELY(was_full)) {
        /* Unlink from full list */
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            ms->full = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        }

        /* Prepend to active list */
        node->next = ms->active;
        node->prev = nullptr;

        if (ms->active) {
            ms->active->prev = node;
        }

        ms->active = node;
        node->is_full = false;
    }

    /* empty: apply hysteresis */
    if (UNLIKELY(node->used_count == 0)) {
        ms->empty_count++;

        if (ms->empty_count > ms->max_empty_reserve && ms->slab_count > 1) {
            /* too many empties, free this one */

            /* unlink from active list */
            if (node->prev) {
                node->prev->next = node->next;
            } else {
                ms->active = node->next;
            }

            if (node->next) {
                node->next->prev = node->prev;
            }

            ms->mem_free(node->raw_memory, ms->slab_memory_size, ms->mem_ctx);
            ms->mem_free(node, sizeof(slab_node_t), ms->mem_ctx);
            ms->slab_count--;
            ms->empty_count--;
        }
    }
}

static void free_node_list(multislab_t* ms, slab_node_t* head) {
    while (head) {
        slab_node_t* next = head->next;
        ms->mem_free(head->raw_memory, ms->slab_memory_size, ms->mem_ctx);
        ms->mem_free(head, sizeof(slab_node_t), ms->mem_ctx);
        head = next;
    }
}

void multislab_destroy(multislab_t* ms) {
    free_node_list(ms, ms->active);
    free_node_list(ms, ms->full);
    memset(ms, 0, sizeof(*ms));
}
