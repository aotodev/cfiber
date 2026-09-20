/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  test_defensive.c
 * @brief Tests for the library's defensive error paths, built with NDEBUG.
 *
 * @details
 * The allocators and stack helpers guard against misuse (bad init parameters,
 * double-free, releasing a foreign pointer, destroying with live stacks). In a
 * debug build those guards fire ASSERT()/assert(), which is __builtin_trap() /
 * abort(), which is impossible to test without killing the process. Compiled with
 * NDEBUG, the asserts disarm and the functions fall through to their defined
 * error behaviour (return -1 / nullptr, or a safe no-op). This translation unit
 * and the library sources it links are built with -DNDEBUG specifically so those
 * paths become observable.
 *
 * Hosted only (the growable-stack tests need mmap).
 */

#ifndef NDEBUG
#error "test_defensive.c must be compiled with NDEBUG so the defensive asserts disarm"
#endif

#include "cfiber/memory/multislab_alloc.h"
#include "cfiber/memory/slab_alloc.h"
#include "cfiber/stack/growable_stack.h"
#include "cfiber/stack/growable_stack_allocator.h"
#include "cfiber/stack/stack.h"
#include "test/test.h"

#include <stdalign.h>
#include <stdint.h>
#include <unistd.h>

#define BLOCK CFIBER_CACHE_LINE_SIZE

static size_t page_size(void) {
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096u;
}

/* ============================================================================
 * slab
 * ============================================================================ */

static int test_slab_init_rejects_bad_params(void) {
    alignas(BLOCK) uint8_t mem[BLOCK * 4];
    cfiber_slab_t s;

    /* zero block size */
    ASSERT_TRUE(cfiber_slab_init(&s, 0, mem, sizeof(mem)) == -1);
    /* block size not a multiple of the cache line */
    ASSERT_TRUE(cfiber_slab_init(&s, BLOCK + 1, mem, sizeof(mem)) == -1);
    /* memory smaller than a single block */
    ASSERT_TRUE(cfiber_slab_init(&s, BLOCK, mem, BLOCK - 1) == -1);
    /* more blocks than the bitmap can track (no user memory is touched here) */
    ASSERT_TRUE(cfiber_slab_init(&s, BLOCK, mem, (size_t)BLOCK * (CFIBER_SLAB_MAX_BLOCKS + 1)) == -1);
    /* a block count that wraps a 32-bit narrowing must still be rejected */
    ASSERT_TRUE(cfiber_slab_init(&s, BLOCK, mem, (size_t)BLOCK << 32) == -1);
    /* memory below max_align_t alignment: the canary store would be UB */
    ASSERT_TRUE(cfiber_slab_init(&s, BLOCK, mem + 1, sizeof(mem) - BLOCK) == -1);

    /* a valid configuration still succeeds */
    ASSERT_EQ_U32(cfiber_slab_init(&s, BLOCK, mem, sizeof(mem)), 0);
    return 0;
}

static int test_slab_double_free_is_safe_noop(void) {
    alignas(BLOCK) uint8_t mem[BLOCK * 2];
    cfiber_slab_t s;
    ASSERT_EQ_U32(cfiber_slab_init(&s, BLOCK, mem, sizeof(mem)), 0);

    void* a = cfiber_slab_alloc(&s);
    void* b = cfiber_slab_alloc(&s);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NULL(cfiber_slab_alloc(&s)); /* both blocks taken */

    ASSERT_TRUE(cfiber_slab_release(&s, a));
    ASSERT_FALSE(cfiber_slab_release(&s, a)); /* double free: rejected under NDEBUG */

    /* exactly one slot is free; the double free must NOT have freed a second */
    void* c = cfiber_slab_alloc(&s);
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(cfiber_slab_alloc(&s));
    return 0;
}

static int test_slab_rejects_foreign_and_misaligned(void) {
    alignas(BLOCK) uint8_t mem[BLOCK * 2];
    alignas(BLOCK) uint8_t other[BLOCK];
    cfiber_slab_t s;
    ASSERT_EQ_U32(cfiber_slab_init(&s, BLOCK, mem, sizeof(mem)), 0);

    void* a = cfiber_slab_alloc(&s);
    ASSERT_NOT_NULL(a);
    ASSERT_FALSE(cfiber_slab_release(&s, other));
    ASSERT_FALSE(cfiber_slab_release(&s, (uint8_t*)a + 1));
    ASSERT_TRUE(cfiber_slab_release(&s, a));
    return 0;
}

/* ============================================================================
 * multislab
 * ============================================================================ */

static int test_multislab_foreign_release_is_noop(void) {
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, 4, 0, 1), 0);

    void* p = cfiber_multislab_alloc(&ms);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(ms.slab_count, 1);

    /* a pointer this allocator never handed out: must be a no-op, not corruption */
    uint32_t stranger = 0;
    cfiber_multislab_release(&ms, &stranger);

    ASSERT_EQ_U32(ms.slab_count, 1);
    ASSERT_NOT_NULL(ms.active);
    ASSERT_EQ_U32(ms.active->used_count, 1);

    /* the genuine block still releases cleanly afterwards */
    cfiber_multislab_release(&ms, p);
    cfiber_multislab_destroy(&ms);
    return 0;
}

/* A rejected release must not reach the multislab's bookkeeping: with the
 * count decremented anyway, the slab reads as empty and is freed under b. */
static int test_multislab_double_release_keeps_live_slab(void) {
    constexpr uint32_t PER_SLAB = 2;
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, 0, 0), 0); /* free empties eagerly */

    void* a = cfiber_multislab_alloc(&ms);
    void* b = cfiber_multislab_alloc(&ms);
    void* c = cfiber_multislab_alloc(&ms); /* second slab, so slab 1 is freeable */
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_U32(ms.slab_count, 2);

    cfiber_multislab_release(&ms, a);
    cfiber_multislab_release(&ms, a); /* double free */
    ASSERT_EQ_U32(ms.slab_count, 2);
    ASSERT_EQ_U32(ms.empty_count, 0);

    /* b's slab is intact: its one free block comes back, then slab 2's */
    void* d = cfiber_multislab_alloc(&ms);
    void* e = cfiber_multislab_alloc(&ms);
    ASSERT_EQ_PTR(d, a);
    ASSERT_NOT_NULL(e);
    ASSERT_NE_PTR(e, b);
    ASSERT_NE_PTR(e, c);
    ASSERT_EQ_U32(ms.slab_count, 2);

    cfiber_multislab_release(&ms, b);
    cfiber_multislab_release(&ms, c);
    cfiber_multislab_release(&ms, d);
    cfiber_multislab_release(&ms, e);
    cfiber_multislab_destroy(&ms);
    return 0;
}

/* ============================================================================
 * growable stack allocator
 * ============================================================================ */

static int test_growable_create_rejects_bad_args(void) {
    const size_t ps = page_size();

    /* zero max size */
    ASSERT_NULL(cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = 0, .cache_capacity = 4, .initial_cached = 0}));
    /* non-page-aligned max size */
    ASSERT_NULL(cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = ps + 1, .cache_capacity = 4, .initial_cached = 0}));
    /* zero cache capacity */
    ASSERT_NULL(cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = ps, .cache_capacity = 0, .initial_cached = 0}));
    /* initial_cached exceeds capacity */
    ASSERT_NULL(cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = ps, .cache_capacity = 2, .initial_cached = 4}));
    return 0;
}

/* A stack that did not come from the pool is unmapped, not pooled: pooling it
 * would hand out a stack of the wrong size later. */
static int test_growable_release_foreign_stack_not_pooled(void) {
    const size_t ps = page_size();
    cfiber_growable_stack_allocator_t* alloc = cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = ps * 4, .cache_capacity = 2, .initial_cached = 0});
    ASSERT_NOT_NULL(alloc);

    cfiber_stack_t foreign = cfiber_growable_stack_create(ps * 8);
    ASSERT_TRUE(cfiber_stack_is_valid(&foreign));
    cfiber_growable_stack_release(alloc, &foreign); /* wrong size: destroyed */
    ASSERT_NULL(foreign.mem_base);

    /* the pool hands out its own size, and nothing was counted as outstanding */
    cfiber_stack_t own = cfiber_growable_stack_alloc(alloc);
    ASSERT_TRUE(cfiber_stack_is_valid(&own));
    ASSERT_EQ_U64(own.total_size, ps * 5);
    cfiber_growable_stack_release(alloc, &own);
    ASSERT_EQ_U32((uint32_t)cfiber_growable_stack_allocator_destroy(alloc), 0);
    return 0;
}

static int test_growable_destroy_reports_leak(void) {
    const size_t ps = page_size();
    cfiber_growable_stack_allocator_t* alloc = cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = ps * 4, .cache_capacity = 2, .initial_cached = 0});
    ASSERT_NOT_NULL(alloc);

    cfiber_stack_t s = cfiber_growable_stack_alloc(alloc);
    ASSERT_TRUE(cfiber_stack_is_valid(&s));

    /* destroying with a stack still outstanding reports the leak via -1 */
    ASSERT_TRUE(cfiber_growable_stack_allocator_destroy(alloc) == -1);

    /* the outstanding mapping is the caller's to release now; do so to keep the
     * test process itself leak-free */
    cfiber_growable_stack_destroy(&s);
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("defensive error paths (NDEBUG)");

    RUN_TEST(test_slab_init_rejects_bad_params);
    RUN_TEST(test_slab_double_free_is_safe_noop);
    RUN_TEST(test_slab_rejects_foreign_and_misaligned);
    RUN_TEST(test_multislab_foreign_release_is_noop);
    RUN_TEST(test_multislab_double_release_keeps_live_slab);
    RUN_TEST(test_growable_create_rejects_bad_args);
    RUN_TEST(test_growable_release_foreign_stack_not_pooled);
    RUN_TEST(test_growable_destroy_reports_leak);

    return cfiber_test_report();
}
