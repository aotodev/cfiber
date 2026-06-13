/**
 * @file  test_multislab_alloc.c
 * @brief Unit tests for the slab and multislab allocators.
 *
 * @details
 * Covers behavior that is exercised on every scheduler spawn/free cycle:
 *   - slab: allocation, exhaustion, release/reuse, reset, pointer layout.
 *   - multislab: lazy growth, max_slabs cap, full<->active list transitions,
 *     empty-slab hysteresis, and (via a counting backing allocator) that every
 *     byte handed out is returned by destroy.
 *
 * Negative-input paths that fire ASSERT() (e.g. slab_init with a bad block
 * size, releasing a foreign pointer) are intentionally NOT tested here: ASSERT
 * is __builtin_trap() in debug builds, so driving them would abort the process
 * rather than return an error. multislab_init validation is testable because it
 * returns -1 without asserting.
 */

#include "cfiber/memory/multislab_alloc.h"
#include "cfiber/memory/slab_alloc.h"
#include "test/test.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>

/* A block size the slab allocator accepts (must be a multiple of the cache
 * line) and small enough that several blocks fit comfortably on the stack. */
#define BLOCK CACHE_LINE_SIZE

/* ============================================================================
 * Helpers
 * ============================================================================ */

static bool ptr_in_range(const void* p, const void* base, size_t size) {
    const uintptr_t v = (uintptr_t)p;
    const uintptr_t b = (uintptr_t)base;
    return v >= b && v < b + size;
}

static uint32_t list_len(const slab_node_t* n) {
    uint32_t c = 0;
    for (; n; n = n->next) {
        c++;
    }
    return c;
}

/* slab_count must always equal len(active) + len(full). */
static bool multislab_invariants_hold(const multislab_t* ms) {
    return ms->slab_count == list_len(ms->active) + list_len(ms->full);
}

/* ---- counting backing allocator: proves destroy returns every byte ---- */

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

/* ============================================================================
 * slab_t tests
 * ============================================================================ */

static int test_slab_alloc_exhaust_and_layout(void) {
    constexpr uint32_t N = 8;
    alignas(BLOCK) uint8_t mem[BLOCK * N];

    slab_t slab;
    ASSERT_EQ_U32(slab_init(&slab, BLOCK, mem, sizeof(mem)), 0);
    ASSERT_EQ_U32(slab.block_count, N);

    void* blocks[N];
    for (uint32_t i = 0; i < N; i++) {
        blocks[i] = slab_alloc(&slab);
        ASSERT_NOT_NULL(blocks[i]);
        ASSERT_TRUE(ptr_in_range(blocks[i], mem, sizeof(mem)));
        /* every block sits on a block_size boundary from the base */
        ASSERT_EQ_U64((uint64_t)(((uintptr_t)blocks[i] - (uintptr_t)mem) % BLOCK), 0);
        /* distinct from all previously handed-out blocks */
        for (uint32_t j = 0; j < i; j++) {
            ASSERT_NE_PTR(blocks[i], blocks[j]);
        }
    }

    /* fully exhausted: next allocation must fail */
    ASSERT_NULL(slab_alloc(&slab));

    return 0;
}

static int test_slab_release_reuses_block(void) {
    constexpr uint32_t N = 4;
    alignas(BLOCK) uint8_t mem[BLOCK * N];

    slab_t slab;
    ASSERT_EQ_U32(slab_init(&slab, BLOCK, mem, sizeof(mem)), 0);

    void* blocks[N];
    for (uint32_t i = 0; i < N; i++) {
        blocks[i] = slab_alloc(&slab);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_NULL(slab_alloc(&slab)); /* full */

    /* free one, the next allocation must succeed and reuse that slot */
    slab_release(&slab, blocks[2]);
    void* reused = slab_alloc(&slab);
    ASSERT_EQ_PTR(reused, blocks[2]);

    return 0;
}

static int test_slab_reset(void) {
    constexpr uint32_t N = 3;
    alignas(BLOCK) uint8_t mem[BLOCK * N];

    slab_t slab;
    ASSERT_EQ_U32(slab_init(&slab, BLOCK, mem, sizeof(mem)), 0);

    for (uint32_t i = 0; i < N; i++) {
        ASSERT_NOT_NULL(slab_alloc(&slab));
    }
    ASSERT_NULL(slab_alloc(&slab));

    slab_reset(&slab);

    /* after reset the whole slab is allocatable again */
    for (uint32_t i = 0; i < N; i++) {
        ASSERT_NOT_NULL(slab_alloc(&slab));
    }

    return 0;
}

/* ============================================================================
 * multislab_t tests
 * ============================================================================ */

static int test_multislab_init_validation(void) {
    multislab_t ms;

    /* block_size below a cache line is rejected */
    ASSERT_TRUE(multislab_init(&ms, CACHE_LINE_SIZE - 1, 4, 0, 0) != 0);
    /* zero blocks per slab is rejected */
    ASSERT_TRUE(multislab_init(&ms, BLOCK, 0, 0, 0) != 0);
    /* more blocks than the bitmap can track is rejected */
    ASSERT_TRUE(multislab_init(&ms, BLOCK, MAX_BLOCK_COUNT + 1, 0, 0) != 0);
    /* a sane configuration succeeds */
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, 4, 0, 0), 0);

    multislab_destroy(&ms);
    return 0;
}

static int test_multislab_lazy_growth(void) {
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, 4, 0, 0), 0);

    /* no backing memory is reserved until the first allocation */
    ASSERT_EQ_U32(ms.slab_count, 0);

    void* p = multislab_alloc(&ms);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(ms.slab_count, 1);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    multislab_release(&ms, p);
    multislab_destroy(&ms);
    return 0;
}

static int test_multislab_grows_across_slabs(void) {
    constexpr uint32_t PER_SLAB = 2;
    constexpr uint32_t COUNT = 5; /* needs ceil(5/2) = 3 slabs */
    multislab_t ms;
    /* high reserve so nothing is freed mid-test */
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, PER_SLAB, 0, 16), 0);

    void* blocks[COUNT];
    for (uint32_t i = 0; i < COUNT; i++) {
        blocks[i] = multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
        for (uint32_t j = 0; j < i; j++) {
            ASSERT_NE_PTR(blocks[i], blocks[j]);
        }
    }

    ASSERT_EQ_U32(ms.slab_count, 3);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    for (uint32_t i = 0; i < COUNT; i++) {
        multislab_release(&ms, blocks[i]);
    }
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    multislab_destroy(&ms);
    return 0;
}

static int test_multislab_max_slabs_cap(void) {
    constexpr uint32_t PER_SLAB = 2;
    constexpr uint32_t MAX = 2; /* capacity == 4 blocks */
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, PER_SLAB, MAX, 16), 0);

    void* blocks[PER_SLAB * MAX];
    for (uint32_t i = 0; i < PER_SLAB * MAX; i++) {
        blocks[i] = multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }

    /* capacity reached: further allocation fails, no extra slab is created */
    ASSERT_NULL(multislab_alloc(&ms));
    ASSERT_EQ_U32(ms.slab_count, MAX);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    for (uint32_t i = 0; i < PER_SLAB * MAX; i++) {
        multislab_release(&ms, blocks[i]);
    }
    multislab_destroy(&ms);
    return 0;
}

/* Releasing a block from a slab that filled up must return that slab to a
 * usable state without leaking any other slab, and a subsequent allocation
 * must reuse existing capacity rather than growing. */
static int test_multislab_full_to_active_transition(void) {
    constexpr uint32_t PER_SLAB = 2;
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, PER_SLAB, 0, 16), 0);

    void* blocks[4];
    for (uint32_t i = 0; i < 4; i++) {
        blocks[i] = multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_EQ_U32(ms.slab_count, 2);

    const uint32_t count_before = ms.slab_count;

    /* free one slot, then re-allocate: capacity exists, so no growth */
    multislab_release(&ms, blocks[0]);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    void* again = multislab_alloc(&ms);
    ASSERT_NOT_NULL(again);
    ASSERT_EQ_U32(ms.slab_count, count_before);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    /* every still-live block must remain owned by the allocator (releasable) */
    multislab_release(&ms, again);
    multislab_release(&ms, blocks[1]);
    multislab_release(&ms, blocks[2]);
    multislab_release(&ms, blocks[3]);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    multislab_destroy(&ms);
    return 0;
}

static int test_multislab_hysteresis_frees_empty(void) {
    constexpr uint32_t PER_SLAB = 2;
    multislab_t ms;
    /* reserve 0 empty slabs: an emptied slab is freed (but never the last one) */
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, PER_SLAB, 0, 0), 0);

    void* blocks[4];
    for (uint32_t i = 0; i < 4; i++) {
        blocks[i] = multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_EQ_U32(ms.slab_count, 2);

    for (uint32_t i = 0; i < 4; i++) {
        multislab_release(&ms, blocks[i]);
        ASSERT_TRUE(multislab_invariants_hold(&ms));
    }

    /* with zero reserve, all-but-one empty slab is reclaimed */
    ASSERT_EQ_U32(ms.slab_count, 1);

    multislab_destroy(&ms);
    return 0;
}

static int test_multislab_single_slab_not_freed_when_empty(void) {
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init(&ms, BLOCK, 4, 0, 0), 0);

    void* p = multislab_alloc(&ms);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(ms.slab_count, 1);

    /* releasing the last live block must keep the sole slab around */
    multislab_release(&ms, p);
    ASSERT_EQ_U32(ms.slab_count, 1);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    multislab_destroy(&ms);
    return 0;
}

/* End-to-end leak check: every byte the multislab requests from its backing
 * allocator must be returned by destroy, regardless of the alloc/free pattern. */
static int test_multislab_no_leak_via_counting_allocator(void) {
    counting_ctx ctx = {0};
    multislab_t ms;
    ASSERT_EQ_U32(multislab_init_ext(&ms, BLOCK, 3, 0, 1, counting_alloc, counting_free, &ctx), 0);

    void* blocks[10];
    for (uint32_t i = 0; i < 10; i++) {
        blocks[i] = multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    /* free half, allocate more, free the rest — churn the lists */
    for (uint32_t i = 0; i < 10; i += 2) {
        multislab_release(&ms, blocks[i]);
    }
    for (uint32_t i = 0; i < 10; i += 2) {
        blocks[i] = multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    for (uint32_t i = 0; i < 10; i++) {
        multislab_release(&ms, blocks[i]);
    }

    ASSERT_TRUE(ctx.allocs > 0);
    multislab_destroy(&ms);

    /* destroy must have returned everything */
    ASSERT_EQ_U32(ctx.allocs, ctx.frees);
    ASSERT_EQ_U64((uint64_t)ctx.live_bytes, 0);
    return 0;
}

/* ============================================================================
 * Runner
 * ============================================================================ */

int main(void) {
    cfiber_test_suite_begin("memory allocators (slab / multislab)");

    RUN_TEST(test_slab_alloc_exhaust_and_layout);
    RUN_TEST(test_slab_release_reuses_block);
    RUN_TEST(test_slab_reset);

    RUN_TEST(test_multislab_init_validation);
    RUN_TEST(test_multislab_lazy_growth);
    RUN_TEST(test_multislab_grows_across_slabs);
    RUN_TEST(test_multislab_max_slabs_cap);
    RUN_TEST(test_multislab_full_to_active_transition);
    RUN_TEST(test_multislab_hysteresis_frees_empty);
    RUN_TEST(test_multislab_single_slab_not_freed_when_empty);
    RUN_TEST(test_multislab_no_leak_via_counting_allocator);

    return cfiber_test_report();
}
