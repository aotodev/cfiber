/**
 * @file  test_defensive.c
 * @brief Tests for the library's defensive error paths, built with NDEBUG.
 *
 * @details
 * The allocators and stack helpers guard against misuse (bad init parameters,
 * double-free, releasing a foreign pointer, destroying with live stacks). In a
 * debug build those guards fire ASSERT()/assert(), which is __builtin_trap() /
 * abort() — impossible to test without killing the process. Compiled with
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

#define BLOCK CACHE_LINE_SIZE

static size_t page_size(void) {
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096u;
}

/* ============================================================================
 * slab
 * ============================================================================ */

static int test_slab_init_rejects_bad_params(void) {
    alignas(BLOCK) uint8_t mem[BLOCK * 4];
    slab_t s;

    /* zero block size */
    ASSERT_TRUE(slab_init(&s, 0, mem, sizeof(mem)) == -1);
    /* block size not a multiple of the cache line */
    ASSERT_TRUE(slab_init(&s, BLOCK + 1, mem, sizeof(mem)) == -1);
    /* memory smaller than a single block */
    ASSERT_TRUE(slab_init(&s, BLOCK, mem, BLOCK - 1) == -1);
    /* more blocks than the bitmap can track (no user memory is touched here) */
    ASSERT_TRUE(slab_init(&s, BLOCK, mem, (size_t)BLOCK * (MAX_BLOCK_COUNT + 1)) == -1);

    /* a valid configuration still succeeds */
    ASSERT_EQ_U32(slab_init(&s, BLOCK, mem, sizeof(mem)), 0);
    return 0;
}

static int test_slab_double_free_is_safe_noop(void) {
    alignas(BLOCK) uint8_t mem[BLOCK * 2];
    slab_t s;
    ASSERT_EQ_U32(slab_init(&s, BLOCK, mem, sizeof(mem)), 0);

    void* a = slab_alloc(&s);
    void* b = slab_alloc(&s);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NULL(slab_alloc(&s)); /* both blocks taken */

    slab_release(&s, a);
    slab_release(&s, a); /* double free: detected and ignored under NDEBUG */

    /* exactly one slot is free — the double free must NOT have freed a second */
    void* c = slab_alloc(&s);
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(slab_alloc(&s));
    return 0;
}

/* ============================================================================
 * multislab
 * ============================================================================ */

static int test_multislab_foreign_release_is_noop(void) {
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, 4, 0, 1), 0);

    void* p = multislab_alloc(&ms);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(ms.slab_count, 1);

    /* a pointer this allocator never handed out: must be a no-op, not corruption */
    uint32_t stranger = 0;
    multislab_release(&ms, &stranger);

    ASSERT_EQ_U32(ms.slab_count, 1);
    ASSERT_NOT_NULL(ms.active);
    ASSERT_EQ_U32(ms.active->used_count, 1);

    /* the genuine block still releases cleanly afterwards */
    multislab_release(&ms, p);
    multislab_destroy(&ms);
    return 0;
}

/* ============================================================================
 * growable stack allocator
 * ============================================================================ */

static int test_growable_create_rejects_bad_args(void) {
    const size_t ps = page_size();

    /* zero max size */
    ASSERT_NULL(growable_stack_allocator_create(
        (growable_stack_allocator_args_t){.max_stack_size = 0, .cache_capacity = 4, .initial_cached = 0}));
    /* non-page-aligned max size */
    ASSERT_NULL(growable_stack_allocator_create(
        (growable_stack_allocator_args_t){.max_stack_size = ps + 1, .cache_capacity = 4, .initial_cached = 0}));
    /* zero cache capacity */
    ASSERT_NULL(growable_stack_allocator_create(
        (growable_stack_allocator_args_t){.max_stack_size = ps, .cache_capacity = 0, .initial_cached = 0}));
    /* initial_cached exceeds capacity */
    ASSERT_NULL(growable_stack_allocator_create(
        (growable_stack_allocator_args_t){.max_stack_size = ps, .cache_capacity = 2, .initial_cached = 4}));
    return 0;
}

static int test_growable_destroy_reports_leak(void) {
    const size_t ps = page_size();
    growable_stack_allocator_t* alloc = growable_stack_allocator_create(
        (growable_stack_allocator_args_t){.max_stack_size = ps * 4, .cache_capacity = 2, .initial_cached = 0});
    ASSERT_NOT_NULL(alloc);

    cstack_t s = growable_stack_alloc(alloc);
    ASSERT_TRUE(is_valid_cstack(&s));

    /* destroying with a stack still outstanding reports the leak via -1 */
    ASSERT_TRUE(growable_stack_allocator_destroy(alloc) == -1);

    /* the outstanding mapping is the caller's to release now; do so to keep the
     * test process itself leak-free */
    cstack_growable_destroy(&s);
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("defensive error paths (NDEBUG)");

    RUN_TEST(test_slab_init_rejects_bad_params);
    RUN_TEST(test_slab_double_free_is_safe_noop);
    RUN_TEST(test_multislab_foreign_release_is_noop);
    RUN_TEST(test_growable_create_rejects_bad_args);
    RUN_TEST(test_growable_destroy_reports_leak);

    return cfiber_test_report();
}
