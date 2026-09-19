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
 * Negative-input paths that fire ASSERT() (e.g. cfiber_slab_init with a bad block
 * size, releasing a foreign pointer) are intentionally NOT tested here: ASSERT
 * is __builtin_trap() in debug builds, so driving them would abort the process
 * rather than return an error. cfiber_multislab_init validation is testable because it
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
#define BLOCK CFIBER_CACHE_LINE_SIZE

/* ============================================================================
 * Helpers
 * ============================================================================ */

static bool ptr_in_range(const void* p, const void* base, size_t size) {
    const uintptr_t v = (uintptr_t)p;
    const uintptr_t b = (uintptr_t)base;
    return v >= b && v < b + size;
}

/* Walks one list checking prev links and is_full, accumulating its length
 * and its number of empty slabs. */
static bool list_ok(const cfiber_slab_node_t* n, bool full, uint32_t* len, uint32_t* empties) {
    for (const cfiber_slab_node_t* prev = nullptr; n; prev = n, n = n->next) {
        if (n->prev != prev || n->is_full != full) {
            return false;
        }
        (*len)++;
        if (n->used_count == 0) {
            (*empties)++;
        }
    }
    return true;
}

/* slab_count == len(active) + len(full); empty_count == slabs with no live
 * block; is_full and prev links agree with list membership. */
static bool multislab_invariants_hold(const cfiber_multislab_t* ms) {
    uint32_t len = 0;
    uint32_t empties = 0;
    if (!list_ok(ms->active, false, &len, &empties) || !list_ok(ms->full, true, &len, &empties)) {
        return false;
    }
    return ms->slab_count == len && ms->empty_count == empties;
}

/* ---- counting backing allocator: proves destroy returns every byte ---- */

typedef struct {
    size_t live_bytes;
    unsigned int allocs;
    unsigned int frees;
    unsigned int fail_at; /* 1-based index of the allocation to refuse; 0 = never */
} counting_ctx;

static void* counting_alloc(size_t size, void* ctx) {
    counting_ctx* c = ctx;
    if (c->fail_at && c->allocs + 1 == c->fail_at) {
        c->allocs++;
        return nullptr;
    }
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
 * cfiber_slab_t tests
 * ============================================================================ */

static int test_slab_alloc_exhaust_and_layout(void) {
    constexpr uint32_t N = 8;
    alignas(BLOCK) uint8_t mem[BLOCK * N];

    cfiber_slab_t slab;
    ASSERT_EQ_U32(cfiber_slab_init(&slab, BLOCK, mem, sizeof(mem)), 0);
    ASSERT_EQ_U32(slab.block_count, N);

    void* blocks[N];
    for (uint32_t i = 0; i < N; i++) {
        blocks[i] = cfiber_slab_alloc(&slab);
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
    ASSERT_NULL(cfiber_slab_alloc(&slab));

    return 0;
}

static int test_slab_release_reuses_block(void) {
    constexpr uint32_t N = 4;
    alignas(BLOCK) uint8_t mem[BLOCK * N];

    cfiber_slab_t slab;
    ASSERT_EQ_U32(cfiber_slab_init(&slab, BLOCK, mem, sizeof(mem)), 0);

    void* blocks[N];
    for (uint32_t i = 0; i < N; i++) {
        blocks[i] = cfiber_slab_alloc(&slab);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_NULL(cfiber_slab_alloc(&slab)); /* full */

    /* free one, the next allocation must succeed and reuse that slot */
    cfiber_slab_release(&slab, blocks[2]);
    void* reused = cfiber_slab_alloc(&slab);
    ASSERT_EQ_PTR(reused, blocks[2]);

    return 0;
}

static int test_slab_reset(void) {
    constexpr uint32_t N = 3;
    alignas(BLOCK) uint8_t mem[BLOCK * N];

    cfiber_slab_t slab;
    ASSERT_EQ_U32(cfiber_slab_init(&slab, BLOCK, mem, sizeof(mem)), 0);

    for (uint32_t i = 0; i < N; i++) {
        ASSERT_NOT_NULL(cfiber_slab_alloc(&slab));
    }
    ASSERT_NULL(cfiber_slab_alloc(&slab));

    cfiber_slab_reset(&slab);

    /* after reset the whole slab is allocatable again */
    for (uint32_t i = 0; i < N; i++) {
        ASSERT_NOT_NULL(cfiber_slab_alloc(&slab));
    }

    return 0;
}

/* ============================================================================
 * cfiber_multislab_t tests
 * ============================================================================ */

static int test_multislab_init_validation(void) {
    cfiber_multislab_t ms;

    /* block_size below a cache line is rejected */
    ASSERT_TRUE(cfiber_multislab_init(&ms, CFIBER_CACHE_LINE_SIZE - 1, 4, 0, 0) != 0);
    /* not a multiple of the cache line: cfiber_slab_init would refuse every slab */
    ASSERT_TRUE(cfiber_multislab_init(&ms, BLOCK + 1, 4, 0, 0) != 0);
    /* zero blocks per slab is rejected */
    ASSERT_TRUE(cfiber_multislab_init(&ms, BLOCK, 0, 0, 0) != 0);
    /* more blocks than the bitmap can track is rejected */
    ASSERT_TRUE(cfiber_multislab_init(&ms, BLOCK, CFIBER_SLAB_MAX_BLOCKS + 1, 0, 0) != 0);
    /* block_size * blocks_per_slab must not wrap */
    ASSERT_TRUE(cfiber_multislab_init(&ms, (SIZE_MAX / 2 + 1), 4, 0, 0) != 0);
    /* a backing allocator needs both callbacks */
    ASSERT_TRUE(cfiber_multislab_init_ext(&ms, BLOCK, 4, 0, 0, nullptr, counting_free, nullptr) != 0);
    ASSERT_TRUE(cfiber_multislab_init_ext(&ms, BLOCK, 4, 0, 0, counting_alloc, nullptr, nullptr) != 0);
    /* a sane configuration succeeds */
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, 4, 0, 0), 0);

    cfiber_multislab_destroy(&ms);
    return 0;
}

static int test_multislab_lazy_growth(void) {
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, 4, 0, 0), 0);

    /* no backing memory is reserved until the first allocation */
    ASSERT_EQ_U32(ms.slab_count, 0);

    void* p = cfiber_multislab_alloc(&ms);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(ms.slab_count, 1);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    cfiber_multislab_release(&ms, p);
    cfiber_multislab_destroy(&ms);
    return 0;
}

static int test_multislab_grows_across_slabs(void) {
    constexpr uint32_t PER_SLAB = 2;
    constexpr uint32_t COUNT = 5; /* needs ceil(5/2) = 3 slabs */
    cfiber_multislab_t ms;
    /* high reserve so nothing is freed mid-test */
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, 0, 16), 0);

    void* blocks[COUNT];
    for (uint32_t i = 0; i < COUNT; i++) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
        for (uint32_t j = 0; j < i; j++) {
            ASSERT_NE_PTR(blocks[i], blocks[j]);
        }
    }

    ASSERT_EQ_U32(ms.slab_count, 3);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    for (uint32_t i = 0; i < COUNT; i++) {
        cfiber_multislab_release(&ms, blocks[i]);
    }
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    cfiber_multislab_destroy(&ms);
    return 0;
}

static int test_multislab_max_slabs_cap(void) {
    constexpr uint32_t PER_SLAB = 2;
    constexpr uint32_t MAX = 2; /* capacity == 4 blocks */
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, MAX, 16), 0);

    void* blocks[PER_SLAB * MAX];
    for (uint32_t i = 0; i < PER_SLAB * MAX; i++) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }

    /* capacity reached: further allocation fails, no extra slab is created */
    ASSERT_NULL(cfiber_multislab_alloc(&ms));
    ASSERT_EQ_U32(ms.slab_count, MAX);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    for (uint32_t i = 0; i < PER_SLAB * MAX; i++) {
        cfiber_multislab_release(&ms, blocks[i]);
    }
    cfiber_multislab_destroy(&ms);
    return 0;
}

/* Releasing a block from a slab that filled up must return that slab to a
 * usable state without leaking any other slab, and a subsequent allocation
 * must reuse existing capacity rather than growing. */
static int test_multislab_full_to_active_transition(void) {
    constexpr uint32_t PER_SLAB = 2;
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, 0, 16), 0);

    void* blocks[4];
    for (uint32_t i = 0; i < 4; i++) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_EQ_U32(ms.slab_count, 2);

    const uint32_t count_before = ms.slab_count;

    /* free one slot, then re-allocate: capacity exists, so no growth */
    cfiber_multislab_release(&ms, blocks[0]);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    void* again = cfiber_multislab_alloc(&ms);
    ASSERT_NOT_NULL(again);
    ASSERT_EQ_U32(ms.slab_count, count_before);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    /* every still-live block must remain owned by the allocator (releasable) */
    cfiber_multislab_release(&ms, again);
    cfiber_multislab_release(&ms, blocks[1]);
    cfiber_multislab_release(&ms, blocks[2]);
    cfiber_multislab_release(&ms, blocks[3]);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    cfiber_multislab_destroy(&ms);
    return 0;
}

static int test_multislab_hysteresis_frees_empty(void) {
    constexpr uint32_t PER_SLAB = 2;
    cfiber_multislab_t ms;
    /* reserve 0 empty slabs: an emptied slab is freed (but never the last one) */
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, 0, 0), 0);

    void* blocks[4];
    for (uint32_t i = 0; i < 4; i++) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_EQ_U32(ms.slab_count, 2);

    for (uint32_t i = 0; i < 4; i++) {
        cfiber_multislab_release(&ms, blocks[i]);
        ASSERT_TRUE(multislab_invariants_hold(&ms));
    }

    /* with zero reserve, all-but-one empty slab is reclaimed */
    ASSERT_EQ_U32(ms.slab_count, 1);

    cfiber_multislab_destroy(&ms);
    return 0;
}

/* The active list can hold a full slab ahead of one with space: release
 * prepends a formerly full slab. Allocation must scan past it rather than
 * grow (unbounded) or fail (max_slabs). */
static int test_multislab_alloc_scans_active_list(void) {
    constexpr uint32_t PER_SLAB = 2;
    for (uint32_t max_slabs = 0; max_slabs <= 2; max_slabs += 2) {
        cfiber_multislab_t ms;
        ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, max_slabs, 16), 0);

        void* a = cfiber_multislab_alloc(&ms);
        void* b = cfiber_multislab_alloc(&ms);
        void* c = cfiber_multislab_alloc(&ms); /* slab 1 {a, b} goes to the full list */
        ASSERT_NOT_NULL(a);
        ASSERT_NOT_NULL(b);
        ASSERT_NOT_NULL(c);
        ASSERT_EQ_U32(ms.slab_count, 2);

        cfiber_multislab_release(&ms, a); /* slab 1 back to the active head */
        void* d = cfiber_multislab_alloc(&ms);
        ASSERT_EQ_PTR(d, a);

        /* slab 1 is full again but still heads the active list; the free
         * block is in slab 2 */
        void* e = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(e);
        ASSERT_NE_PTR(e, b);
        ASSERT_NE_PTR(e, c);
        ASSERT_NE_PTR(e, d);
        ASSERT_EQ_U32(ms.slab_count, 2);
        ASSERT_TRUE(multislab_invariants_hold(&ms));

        if (max_slabs) {
            ASSERT_NULL(cfiber_multislab_alloc(&ms)); /* genuinely exhausted */
        }

        cfiber_multislab_release(&ms, b);
        cfiber_multislab_release(&ms, c);
        cfiber_multislab_release(&ms, d);
        cfiber_multislab_release(&ms, e);
        ASSERT_TRUE(multislab_invariants_hold(&ms));
        cfiber_multislab_destroy(&ms);
    }
    return 0;
}

/* empty_count must track the number of empty slabs, not the number of times
 * a slab became empty, or the reserve is ignored after a few cycles. */
static int test_multislab_empty_count_tracks_reuse(void) {
    constexpr uint32_t PER_SLAB = 2;
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, 0, 1), 0); /* keep one empty */

    void* a = cfiber_multislab_alloc(&ms);
    void* b = cfiber_multislab_alloc(&ms);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    /* a second slab oscillating between one block and empty */
    for (int cycle = 0; cycle < 4; cycle++) {
        void* c = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(c);
        ASSERT_EQ_U32(ms.slab_count, 2);
        ASSERT_EQ_U32(ms.empty_count, 0);

        cfiber_multislab_release(&ms, c);
        ASSERT_EQ_U32(ms.slab_count, 2); /* the reserve keeps it */
        ASSERT_EQ_U32(ms.empty_count, 1);
        ASSERT_TRUE(multislab_invariants_hold(&ms));
    }

    cfiber_multislab_release(&ms, a);
    cfiber_multislab_release(&ms, b);
    ASSERT_TRUE(multislab_invariants_hold(&ms));
    cfiber_multislab_destroy(&ms);
    return 0;
}

static int test_multislab_single_slab_not_freed_when_empty(void) {
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, 4, 0, 0), 0);

    void* p = cfiber_multislab_alloc(&ms);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(ms.slab_count, 1);

    /* releasing the last live block must keep the sole slab around */
    cfiber_multislab_release(&ms, p);
    ASSERT_EQ_U32(ms.slab_count, 1);
    ASSERT_TRUE(multislab_invariants_hold(&ms));

    cfiber_multislab_destroy(&ms);
    return 0;
}

/* More blocks than one bitmap word: the multi-word scan and its capacity tail. */
static int test_multislab_multiword_bitmap(void) {
    constexpr uint32_t PER_SLAB = CFIBER_BITMAP_WORD_BITS + 5;
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init(&ms, BLOCK, PER_SLAB, 1, 16), 0);

    void* blocks[PER_SLAB];
    for (uint32_t i = 0; i < PER_SLAB; i++) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
        for (uint32_t j = 0; j < i; j++) {
            ASSERT_NE_PTR(blocks[i], blocks[j]);
        }
    }
    ASSERT_NULL(cfiber_multislab_alloc(&ms)); /* exactly PER_SLAB fit */
    ASSERT_EQ_U32(ms.slab_count, 1);

    /* a slot past the first word comes back and is reused */
    cfiber_multislab_release(&ms, blocks[CFIBER_BITMAP_WORD_BITS + 2]);
    ASSERT_EQ_PTR(cfiber_multislab_alloc(&ms), blocks[CFIBER_BITMAP_WORD_BITS + 2]);

    for (uint32_t i = 0; i < PER_SLAB; i++) {
        cfiber_multislab_release(&ms, blocks[i]);
    }
    ASSERT_TRUE(multislab_invariants_hold(&ms));
    cfiber_multislab_destroy(&ms);
    return 0;
}

/* A backing allocator that fails: grow reports exhaustion, frees what it had
 * taken, and the multislab stays usable and leak-free. */
static int test_multislab_backing_failure(void) {
    /* grow takes two allocations, the node then the slab memory; fail each */
    for (unsigned int fail_at = 1; fail_at <= 2; fail_at++) {
        counting_ctx ctx = {.fail_at = fail_at};
        cfiber_multislab_t ms;
        ASSERT_EQ_U32(cfiber_multislab_init_ext(&ms, BLOCK, 2, 0, 1, counting_alloc, counting_free, &ctx), 0);

        ASSERT_NULL(cfiber_multislab_alloc(&ms));
        ASSERT_EQ_U32(ms.slab_count, 0);
        ASSERT_EQ_U64((uint64_t)ctx.live_bytes, 0);
        ASSERT_TRUE(multislab_invariants_hold(&ms));

        /* the allocator recovers: the next grow succeeds */
        ctx.fail_at = 0;
        void* p = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(p);
        cfiber_multislab_release(&ms, p);
        cfiber_multislab_destroy(&ms);
        ASSERT_EQ_U64((uint64_t)ctx.live_bytes, 0);
    }
    return 0;
}

/* End-to-end leak check: every byte the multislab requests from its backing
 * allocator must be returned by destroy, regardless of the alloc/free pattern. */
static int test_multislab_no_leak_via_counting_allocator(void) {
    counting_ctx ctx = {0};
    cfiber_multislab_t ms;
    ASSERT_EQ_U32(cfiber_multislab_init_ext(&ms, BLOCK, 3, 0, 1, counting_alloc, counting_free, &ctx), 0);

    void* blocks[10];
    for (uint32_t i = 0; i < 10; i++) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    /* free half, allocate more, free the rest, churning the lists */
    for (uint32_t i = 0; i < 10; i += 2) {
        cfiber_multislab_release(&ms, blocks[i]);
    }
    for (uint32_t i = 0; i < 10; i += 2) {
        blocks[i] = cfiber_multislab_alloc(&ms);
        ASSERT_NOT_NULL(blocks[i]);
    }
    for (uint32_t i = 0; i < 10; i++) {
        cfiber_multislab_release(&ms, blocks[i]);
    }

    ASSERT_TRUE(ctx.allocs > 0);
    cfiber_multislab_destroy(&ms);

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
    RUN_TEST(test_multislab_alloc_scans_active_list);
    RUN_TEST(test_multislab_empty_count_tracks_reuse);
    RUN_TEST(test_multislab_single_slab_not_freed_when_empty);
    RUN_TEST(test_multislab_multiword_bitmap);
    RUN_TEST(test_multislab_backing_failure);
    RUN_TEST(test_multislab_no_leak_via_counting_allocator);

    return cfiber_test_report();
}
