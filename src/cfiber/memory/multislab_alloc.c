#include "cfiber/memory/multislab_alloc.h"

#include <stdlib.h>
#include <string.h>

/* Cache-line aligned so blocks, whose size is a multiple of the line, never
 * share one. aligned_alloc needs the size rounded up to the alignment. */
static void* default_alloc(const size_t size, void* ctx) {
    (void)ctx;
    return aligned_alloc(CACHE_LINE_SIZE, align_up(size, CACHE_LINE_SIZE));
}

static void default_free(void* const ptr, size_t size, void* ctx) {
    (void)size;
    (void)ctx;
    free(ptr);
}

/* Intrusive doubly linked list, head pointer passed by address. */
static void list_push(slab_node_t** head, slab_node_t* const node) {
    node->next = *head;
    node->prev = nullptr;
    if (*head) {
        (*head)->prev = node;
    }
    *head = node;
}

static void list_unlink(slab_node_t** head, slab_node_t* const node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        *head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
}

/* Invariant: empty_count == number of nodes with used_count == 0. A fresh
 * slab counts until its first allocation. */
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
    if (UNLIKELY(res)) {
        ms->mem_free(mem, ms->slab_memory_size, ms->mem_ctx);
        ms->mem_free(node, sizeof(slab_node_t), ms->mem_ctx);
        return nullptr;
    }

    node->raw_memory = mem;
    node->used_count = 0;
    node->is_full = false;
    list_push(&ms->active, node);
    ms->slab_count++;
    ms->empty_count++;

    return node;
}

static void multislab_free_node(multislab_t* ms, slab_node_t* const node) {
    ms->mem_free(node->raw_memory, ms->slab_memory_size, ms->mem_ctx);
    ms->mem_free(node, sizeof(slab_node_t), ms->mem_ctx);
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
    if (block_size < CACHE_LINE_SIZE || (block_size % CACHE_LINE_SIZE) || blocks_per_slab == 0
        || blocks_per_slab > MAX_BLOCK_COUNT || blocks_per_slab > SIZE_MAX / block_size || !mem_alloc || !mem_free) {
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
    /* Slabs are moved to the full list lazily, so the active list can hold
     * full slabs ahead of ones with space (release prepends). Walk it before
     * growing; only an empty active list means every slab is full. */
    void* ptr = nullptr;
    while (ms->active) {
        ptr = slab_alloc(&ms->active->slab);
        if (LIKELY(ptr)) {
            break;
        }
        slab_node_t* const full = ms->active;
        list_unlink(&ms->active, full);
        list_push(&ms->full, full);
        full->is_full = true;
    }

    if (UNLIKELY(!ptr)) {
        if (UNLIKELY(!multislab_grow(ms))) {
            return nullptr;
        }
        ptr = slab_alloc(&ms->active->slab);
    }

    slab_node_t* const node = ms->active;
    if (node->used_count++ == 0) {
        ms->empty_count--;
    }
    return ptr;
}

void multislab_release(multislab_t* const ms, void* const ptr) {
    slab_node_t* node = find_owning_slab(ms, ptr);
    if (UNLIKELY(!node)) {
        ASSERT(false && "pointer not owned by this allocator");
        return;
    }

    /* A rejected release (double free, misaligned) must not touch the
     * bookkeeping: decrementing used_count for a block still live would let
     * the empty-slab path free a slab in use. */
    if (UNLIKELY(!slab_release(&node->slab, ptr))) {
        return;
    }
    node->used_count--;

    /* Tracked membership, not inferred from used_count: a full slab can still
     * be on the active list (see multislab_alloc). */
    if (UNLIKELY(node->is_full)) {
        list_unlink(&ms->full, node);
        list_push(&ms->active, node);
        node->is_full = false;
    }

    /* empty: apply hysteresis */
    if (UNLIKELY(node->used_count == 0)) {
        ms->empty_count++;

        if (ms->empty_count > ms->max_empty_reserve && ms->slab_count > 1) {
            list_unlink(&ms->active, node);
            multislab_free_node(ms, node);
            ms->slab_count--;
            ms->empty_count--;
        }
    }
}

static void free_node_list(multislab_t* ms, slab_node_t* head) {
    while (head) {
        slab_node_t* next = head->next;
        multislab_free_node(ms, head);
        head = next;
    }
}

void multislab_destroy(multislab_t* ms) {
    free_node_list(ms, ms->active);
    free_node_list(ms, ms->full);
    memset(ms, 0, sizeof(*ms));
}
