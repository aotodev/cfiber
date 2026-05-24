#include "cfiber/stack/growable_stack_allocator.h"

#include "cfiber/stack/growable_stack.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct growable_stack_allocator {
    size_t max_stack_size;
    cstack_t* cache;
    size_t cache_capacity;
    size_t cache_count;

    /** Debug counter: number of stacks currently outstanding. */
    size_t active_count;
};

static void growable_allocator_cleanup(growable_stack_allocator_t* alloc) {
    if (!alloc) {
        return;
    }

    if (alloc->cache) {
        for (size_t i = 0; i < alloc->cache_count; i++) {
            cstack_growable_destroy(&alloc->cache[i]);
        }
        free(alloc->cache);
    }

    free(alloc);
}

growable_stack_allocator_t* growable_stack_allocator_create(growable_stack_allocator_args_t args) {
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

    growable_stack_allocator_t* alloc = calloc(1, sizeof(growable_stack_allocator_t));
    if (!alloc) {
        return nullptr;
    }

    alloc->max_stack_size = args.max_stack_size;
    alloc->cache_capacity = args.cache_capacity;
    alloc->cache_count = 0;
    alloc->active_count = 0;

    alloc->cache = calloc(alloc->cache_capacity, sizeof(cstack_t));
    if (!alloc->cache) {
        growable_allocator_cleanup(alloc);
        return nullptr;
    }

    /* Pre-allocate initial stacks if requested */
    if (args.initial_cached) {
        for (size_t i = 0; i < args.initial_cached; i++) {
            alloc->cache[i] = cstack_growable_create(args.max_stack_size);
            if (!alloc->cache[i].mem_base) {
                growable_allocator_cleanup(alloc);
                return nullptr;
            }
            alloc->cache_count++;
        }
    }

    return alloc;
}

int growable_stack_allocator_destroy(growable_stack_allocator_t* alloc) {
    int res = 0;
    if (alloc->active_count) {
        assert(!alloc->active_count && "Memory leak: stacks were allocated but never released");
        res = -1;
    }
    growable_allocator_cleanup(alloc);
    return res;
}

cstack_t growable_stack_alloc(growable_stack_allocator_t* alloc) {
    alloc->active_count++;

    if (alloc->cache_count > 0) {
        return alloc->cache[--alloc->cache_count];
    }

    return cstack_growable_create(alloc->max_stack_size);
}

void growable_stack_release(growable_stack_allocator_t* alloc, cstack_t* stack) {
    assert(alloc->active_count > 0 && "Double-free or unallocated stack");
    alloc->active_count--;

    if (alloc->cache_count >= alloc->cache_capacity) {
        cstack_growable_destroy(stack);
    } else {
        cstack_growable_recycle(stack);
        alloc->cache[alloc->cache_count++] = *stack;
    }

    stack->mem_base = nullptr;
    stack->stack_top = nullptr;
    stack->total_size = 0;
}
