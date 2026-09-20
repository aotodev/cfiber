/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

#include "cfiber/stack/growable_stack_allocator.h"

#include "cfiber/core/internal.h"
#include "cfiber/stack/growable_stack.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

struct cfiber_growable_stack_allocator {
    size_t max_stack_size;
    size_t total_size; /* max_stack_size + guard page: what our stacks measure */
    cfiber_stack_t* cache;
    size_t cache_capacity;
    size_t cache_count;

    /** Debug counter: number of stacks currently outstanding. */
    size_t active_count;
};

static void growable_allocator_cleanup(cfiber_growable_stack_allocator_t* alloc) {
    if (!alloc) {
        return;
    }

    if (alloc->cache) {
        for (size_t i = 0; i < alloc->cache_count; i++) {
            cfiber_growable_stack_destroy(&alloc->cache[i]);
        }
        free(alloc->cache);
    }

    free(alloc);
}

cfiber_growable_stack_allocator_t* cfiber_growable_stack_allocator_create(cfiber_growable_stack_allocator_args_t args) {
    const long raw_page_size = sysconf(_SC_PAGESIZE);
    if (UNLIKELY(raw_page_size <= 0)) {
        return nullptr;
    }
    const size_t page_size = (size_t)raw_page_size;

    if (!args.max_stack_size || (args.max_stack_size & (page_size - 1))) {
        assert(args.max_stack_size && !(args.max_stack_size & (page_size - 1)));
        return nullptr;
    }

    if (!args.cache_capacity || (args.initial_cached > args.cache_capacity)) {
        assert(args.cache_capacity && !(args.initial_cached > args.cache_capacity));
        return nullptr;
    }

    cfiber_growable_stack_allocator_t* alloc = calloc(1, sizeof(cfiber_growable_stack_allocator_t));
    if (!alloc) {
        return nullptr;
    }

    alloc->max_stack_size = args.max_stack_size;
    alloc->total_size = args.max_stack_size + page_size;
    alloc->cache_capacity = args.cache_capacity;
    alloc->cache_count = 0;
    alloc->active_count = 0;

    alloc->cache = calloc(alloc->cache_capacity, sizeof(cfiber_stack_t));
    if (!alloc->cache) {
        growable_allocator_cleanup(alloc);
        return nullptr;
    }

    /* Pre-allocate initial stacks if requested */
    if (args.initial_cached) {
        for (size_t i = 0; i < args.initial_cached; i++) {
            alloc->cache[i] = cfiber_growable_stack_create(args.max_stack_size);
            if (!alloc->cache[i].mem_base) {
                growable_allocator_cleanup(alloc);
                return nullptr;
            }
            alloc->cache_count++;
        }
    }

    return alloc;
}

int cfiber_growable_stack_allocator_destroy(cfiber_growable_stack_allocator_t* alloc) {
    int res = 0;
    if (alloc->active_count) {
        assert(!alloc->active_count && "Memory leak: stacks were allocated but never released");
        res = -1;
    }
    growable_allocator_cleanup(alloc);
    return res;
}

cfiber_stack_t cfiber_growable_stack_alloc(cfiber_growable_stack_allocator_t* alloc) {
    if (alloc->cache_count > 0) {
        alloc->active_count++;
        return alloc->cache[--alloc->cache_count];
    }

    cfiber_stack_t stack = cfiber_growable_stack_create(alloc->max_stack_size);
    if (stack.mem_base) {
        alloc->active_count++; /* only a stack that exists is outstanding */
    }
    return stack;
}

void cfiber_growable_stack_release(cfiber_growable_stack_allocator_t* alloc, cfiber_stack_t* stack) {
    if (stack->total_size != alloc->total_size) {
        /* Not ours: pooling it would hand out a stack of the wrong size later. */
        assert(false && "stack released to a pool it was not allocated from");
        cfiber_growable_stack_destroy(stack);
        return;
    }

    assert(alloc->active_count > 0 && "Double-free or unallocated stack");
    alloc->active_count--;

    if (alloc->cache_count >= alloc->cache_capacity) {
        cfiber_growable_stack_destroy(stack);
    } else {
        cfiber_growable_stack_recycle(stack);
        alloc->cache[alloc->cache_count++] = *stack;
    }

    stack->mem_base = nullptr;
    stack->usable_base = nullptr;
    stack->stack_top = nullptr;
    stack->total_size = 0;
}
