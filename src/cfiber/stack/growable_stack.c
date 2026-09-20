/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

#include "cfiber/stack/growable_stack.h"

#include "cfiber/core/internal.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* 0 if the page size cannot be queried; callers fail with errno set. */
static size_t page_size(void) {
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 0;
}

cfiber_stack_t cfiber_growable_stack_create(const size_t max_size) {
    cfiber_stack_t stack = {};

    const size_t page = page_size();
    if (UNLIKELY(!page)) {
        return stack; /* errno from sysconf */
    }
    if (UNLIKELY(!max_size || (max_size & (page - 1)) || max_size > SIZE_MAX - page)) {
        errno = EINVAL;
        return stack;
    }

    /* stack + bottom guard page */
    const size_t total_vma_size = max_size + page;

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

    /* Hard guard page at the bottom. A stack without it is not one we hand out. */
    if (UNLIKELY(mprotect(vma_start, page, PROT_NONE) < 0)) {
        const int saved = errno;
        munmap(vma_start, total_vma_size);
        errno = saved;
        return stack;
    }

    stack.mem_base = vma_start;
    stack.usable_base = (char*)vma_start + page;
    stack.stack_top = (char*)vma_start + total_vma_size;
    stack.total_size = total_vma_size;

    return stack;
}

void cfiber_growable_stack_destroy(cfiber_stack_t* stack) {
    assert(cfiber_stack_is_valid(stack));

    munmap(stack->mem_base, stack->total_size);

    stack->mem_base = nullptr;
    stack->usable_base = nullptr;
    stack->stack_top = nullptr;
    stack->total_size = 0;
}

void cfiber_growable_stack_recycle(cfiber_stack_t* const stack) {
    assert(cfiber_stack_is_valid(stack) && "Invalid stack in recycle");

    const size_t page = page_size();
    /* Keep the top page warm; anything below it and above the guard goes. */
    if (!page || cfiber_stack_usable_size(stack) <= page) {
        return;
    }

    const size_t length = cfiber_stack_usable_size(stack) - page;
    (void)madvise(stack->usable_base, length, MADV_DONTNEED);
}
