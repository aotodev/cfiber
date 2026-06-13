/**
 * @file  test_fixed_size_stack_allocator.c
 * @brief Unit tests for the multislab-backed fixed-size stack allocator.
 *
 * @details
 * ms_stack_alloc()/ms_stack_release() pair a multislab block with a cstack_t
 * descriptor. These tests verify descriptor layout, distinctness, exhaustion,
 * reuse, and that every block is returned to the backing allocator.
 *
 * The tests only inspect descriptor fields; they never write into the stack
 * memory, so they behave identically whether or not the ASan redzone / canary
 * instrumentation is compiled in.
 */

#include "cfiber/memory/multislab_alloc.h"
#include "cfiber/stack/fixed_size_stack_allocator.h"
#include "cfiber/stack/stack.h"
#include "test/test.h"

#include <stdint.h>
#include <stdlib.h>

/* Block size: a multiple of the cache line, large enough for the optional
 * canary word + redzone instrumentation. */
#define SBLOCK 256

typedef struct {
    size_t live_bytes;
    unsigned int allocs;
    unsigned int frees;
} counting_ctx;

static void* counting_alloc(size_t size, void* ctx) {
    counting_ctx* c = ctx;
    void* p = malloc(size);
    if (p) {
        c->allocs++;
        c->live_bytes += size;
    }
    return p;
}

static void counting_free(void* ptr, size_t size, void* ctx) {
    counting_ctx* c = ctx;
    c->frees++;
    c->live_bytes -= size;
    free(ptr);
}

static int test_ms_stack_alloc_fills_descriptor(void) {
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, SBLOCK, 4, 0, 1), 0);

    cstack_t s = {0};
    ASSERT_EQ_U32(ms_stack_alloc(&s, &ms), 0);

    ASSERT_TRUE(is_valid_cstack(&s));
    ASSERT_EQ_U64(s.total_size, SBLOCK);
    ASSERT_EQ_PTR(s.stack_top, (char*)s.mem_base + SBLOCK);

    ms_stack_release(&s, &ms);
    multislab_destroy(&ms);
    return 0;
}

static int test_ms_stack_distinct_and_exhaustion(void) {
    multislab_t ms;
    /* capacity == 2 blocks (per_slab 2, max 1 slab) */
    ASSERT_EQ_U32(multislab_init(&ms, SBLOCK, 2, 1, 16), 0);

    cstack_t a = {0};
    cstack_t b = {0};
    cstack_t c = {0};

    ASSERT_EQ_U32(ms_stack_alloc(&a, &ms), 0);
    ASSERT_EQ_U32(ms_stack_alloc(&b, &ms), 0);
    ASSERT_NE_PTR(a.mem_base, b.mem_base);

    /* capacity exhausted: allocation fails with -1 */
    ASSERT_TRUE(ms_stack_alloc(&c, &ms) == -1);

    ms_stack_release(&a, &ms);
    ms_stack_release(&b, &ms);
    multislab_destroy(&ms);
    return 0;
}

static int test_ms_stack_release_reuse(void) {
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, SBLOCK, 2, 1, 16), 0);

    cstack_t a = {0};
    ASSERT_EQ_U32(ms_stack_alloc(&a, &ms), 0);
    void* base = a.mem_base;
    ms_stack_release(&a, &ms);

    cstack_t b = {0};
    ASSERT_EQ_U32(ms_stack_alloc(&b, &ms), 0);
    ASSERT_EQ_PTR(b.mem_base, base);

    ms_stack_release(&b, &ms);
    multislab_destroy(&ms);
    return 0;
}

static int test_ms_stack_no_leak_via_counting_allocator(void) {
    counting_ctx ctx = {0};
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init_ext(&ms, SBLOCK, 3, 0, 1, counting_alloc, counting_free, &ctx), 0);

    cstack_t stacks[7];
    for (int i = 0; i < 7; i++) {
        ASSERT_EQ_U32(ms_stack_alloc(&stacks[i], &ms), 0);
    }
    for (int i = 0; i < 7; i++) {
        ms_stack_release(&stacks[i], &ms);
    }

    ASSERT_TRUE(ctx.allocs > 0);
    multislab_destroy(&ms);

    ASSERT_EQ_U32(ctx.allocs, ctx.frees);
    ASSERT_EQ_U64((uint64_t)ctx.live_bytes, 0);
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("fixed-size stack allocator");

    RUN_TEST(test_ms_stack_alloc_fills_descriptor);
    RUN_TEST(test_ms_stack_distinct_and_exhaustion);
    RUN_TEST(test_ms_stack_release_reuse);
    RUN_TEST(test_ms_stack_no_leak_via_counting_allocator);

    return cfiber_test_report();
}
