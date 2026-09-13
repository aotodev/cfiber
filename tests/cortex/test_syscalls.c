/**
 * @file  test_syscalls.c
 * @brief Cortex-M support code (utils/cortex): RAM layout and _sbrk bounds.
 *
 * @details
 * RAM is .data, .bss, the heap [_heap_start, _heap_end) and a fixed main stack
 * at the top. _sbrk must bound the break by _heap_end, not by SP: fiber stacks
 * are carved from the heap, so inside a fiber SP is below the break and an
 * SP-based guard rejects every allocation.
 *
 * Fibers only record into the fixture; main asserts once control is back.
 */

#include "cfiber/fiber/fiber.h"
#include "test/test.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* NOLINTBEGIN(cert-dcl37-c): linker script and newlib symbols */
extern char _ebss;
extern char _heap_start;
extern char _heap_end;
extern char _stack_bottom;
extern char _stack_top;

/* utils/cortex/syscalls.c */
void* _sbrk(ptrdiff_t increment);
/* NOLINTEND(cert-dcl37-c) */

#define FIBER_STACK_SIZE 1024
#define SBRK_PROBE 64

static bool in_heap(const void* p) {
    const uintptr_t v = (uintptr_t)p;
    return v >= (uintptr_t)&_heap_start && v < (uintptr_t)&_heap_end;
}

static int test_layout(void) {
    ASSERT_TRUE((uintptr_t)&_heap_start >= (uintptr_t)&_ebss);
    ASSERT_EQ_PTR(&_heap_end, &_stack_bottom);
    ASSERT_TRUE((uintptr_t)&_stack_bottom < (uintptr_t)&_stack_top);

    /* main runs on the main stack, above the heap */
    int probe;
    ASSERT_TRUE((uintptr_t)&probe >= (uintptr_t)&_heap_end);
    ASSERT_TRUE((uintptr_t)&probe < (uintptr_t)&_stack_top);
    return 0;
}

static int test_sbrk_bounds(void) {
    char* const before = _sbrk(0);
    ASSERT_TRUE(in_heap(before) || before == &_heap_end);
    const ptrdiff_t room = &_heap_end - before;

    /* one byte past the heap is rejected and the break does not move */
    ASSERT_EQ_PTR(_sbrk(room + 1), (void*)-1);
    ASSERT_EQ_U32(errno, ENOMEM);
    ASSERT_EQ_PTR(_sbrk(0), before);

    /* the whole remainder is usable and can be given back */
    ASSERT_EQ_PTR(_sbrk(room), before);
    ASSERT_EQ_PTR(_sbrk(-room), before + room);
    ASSERT_EQ_PTR(_sbrk(0), before);

    /* shrinking below the heap start is rejected too */
    ASSERT_EQ_PTR(_sbrk(&_heap_start - before - 1), (void*)-1);
    ASSERT_EQ_PTR(_sbrk(0), before);
    return 0;
}

/* ============================================================================
 * Heap growth from a fiber stack
 * ============================================================================ */

typedef struct {
    context_t main_ctx;
    fiber_t fiber;
    void* brk;   /* _sbrk result inside the fiber */
    void* block; /* malloc result inside the fiber */
} fixture;

static fixture fx;

static void grow_heap_in_fiber(void* user_data) {
    fixture* f = user_data;
    f->brk = _sbrk(SBRK_PROBE);
    if (f->brk != (void*)-1) {
        _sbrk(-SBRK_PROBE);
    }
    f->block = malloc(SBRK_PROBE);
    switch_context(&f->fiber.ctx, &f->main_ctx);
}

/* The fiber switches back to main and is never resumed. */
static void return_hook(void* ctx) {
    (void)ctx;
    abort();
}

static int test_sbrk_inside_fiber(void) {
    fx.fiber.stack = malloc(FIBER_STACK_SIZE);
    ASSERT_NOT_NULL(fx.fiber.stack);
    ASSERT_TRUE(in_heap(fx.fiber.stack)); /* so SP is below the break in the fiber */
    fx.fiber.stack_size = FIBER_STACK_SIZE;
    memset(&fx.fiber.ctx, 0, sizeof fx.fiber.ctx);
    init_fiber(&fx.fiber, grow_heap_in_fiber, &fx);

    switch_context(&fx.main_ctx, &fx.fiber.ctx);

    ASSERT_NE_PTR(fx.brk, (void*)-1);
    ASSERT_TRUE(in_heap(fx.brk));
    ASSERT_NOT_NULL(fx.block);
    ASSERT_TRUE(in_heap(fx.block));

    free(fx.block);
    free(fx.fiber.stack);
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("cortex-m syscalls (heap layout / _sbrk)");

    cfiber_set_return_hook(return_hook, nullptr);

    RUN_TEST(test_layout);
    RUN_TEST(test_sbrk_bounds);
    RUN_TEST(test_sbrk_inside_fiber);

    return cfiber_test_report();
}
