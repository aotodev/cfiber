#include "cfiber/stack/growable_stack.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static thread_local size_t tl_page_size = 0;

static size_t get_page_size(void) {
    if (LIKELY(tl_page_size)) {
        return tl_page_size;
    }

    const long ps = sysconf(_SC_PAGESIZE);
    if (UNLIKELY(ps <= 0)) {
        perror("sysconf(_SC_PAGESIZE)");
        _exit(EXIT_FAILURE);
    }

    tl_page_size = (size_t)ps;
    return tl_page_size;
}

cstack_t cstack_growable_create(const size_t max_size) {
    assert(get_page_size() && "page size must be queryable before allocating a growable stack");

    cstack_t stack = {};

    /* stack + bottom guard page */
    size_t total_vma_size = max_size + get_page_size();

    /* allocates physical memory lazily with MAP_NORESERVE */
    void* vma_start = mmap(nullptr,
                           total_vma_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE,
                           -1,
                           0);

    if (UNLIKELY(vma_start == MAP_FAILED)) {
        return stack;
    }

    /* Hard guard page at the bottom */
    mprotect(vma_start, get_page_size(), PROT_NONE);

    stack.mem_base = vma_start;
    stack.stack_top = (char*)vma_start + total_vma_size;
    stack.total_size = total_vma_size;

    return stack;
}

void cstack_growable_destroy(cstack_t* stack) {
    assert(is_valid_cstack(stack));

    munmap(stack->mem_base, stack->total_size);

    stack->mem_base = nullptr;
    stack->stack_top = nullptr;
    stack->total_size = 0;
}

void cstack_growable_recycle(cstack_t* const stack) {
    assert(stack && stack->mem_base && "Invalid stack in recycle");

    /* We need at least 2 pages (guard page + 1 usable page) to do anything meaningful */
    if (stack->total_size <= 2 * get_page_size()) {
        return;
    }

    /* the actual usable bottom of the stack (just above the PROT_NONE guard page) */
    void* const valid_bottom = (char*)stack->mem_base + get_page_size();

    /* we want to keep the top page warm */
    const void* const keep_limit = (char*)stack->stack_top - get_page_size();

    const size_t length = (uintptr_t)keep_limit - (uintptr_t)valid_bottom;

    (void)madvise(valid_bottom, length, MADV_DONTNEED);
}
