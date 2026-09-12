/**
 * @file  fuzz_multislab.c
 * @brief Coverage-guided fuzzer for the slab / multislab allocator.
 *
 * @details
 * Interprets the input as: a small config header (block size, blocks-per-slab,
 * max-slabs, hysteresis, a backing-allocator failure schedule) followed by an
 * opcode stream of alloc / release operations. Only valid operations are issued
 * (releases always target a currently-live block), so the allocator's defensive
 * ASSERT paths (double-free, foreign pointer) are never tripped; this fuzzer
 * hunts logic and memory bugs, which ASan/UBSan and the structural invariants
 * below surface.
 *
 * After every operation it checks:
 *   - slab_count == len(active list) + len(full list), prev links consistent;
 *   - is_full matches list membership, and full-list slabs are full;
 *   - empty_count == number of slabs with used_count == 0;
 *   - the sum of every slab's used_count equals the number of live blocks;
 *   - each live pointer lies on a block boundary inside exactly one slab;
 *   - allocations never alias a still-live block;
 *   - allocation fails only at the max_slabs capacity or on an injected
 *     backing-allocator failure.
 * On teardown it checks (via a counting backing allocator) that destroy returns
 * every byte the allocator ever requested.
 *
 * blocks_per_slab ranges past BITMAP_WORD_BITS so the multi-word bitmap scan
 * and its capacity tail are exercised.
 */

#include "cfiber/memory/multislab_alloc.h"
#include "fuzz_input.h"

#include <stdint.h>
#include <stdlib.h>

#define LIVE_CAP 512
#define PER_SLAB_MAX (3 * BITMAP_WORD_BITS)

typedef struct {
    size_t live_bytes;
    unsigned long allocs;
    unsigned long frees;
    /* bit i set: the i-th backing allocation (mod 32) fails */
    uint32_t fail_mask;
    bool injected; /* a failure was injected since the last reset */
} counting_ctx;

static void* counting_alloc(size_t size, void* ctx) {
    counting_ctx* c = ctx;
    if ((c->fail_mask >> (c->allocs % 32u)) & 1u) {
        c->allocs++;
        c->injected = true;
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

typedef struct {
    uint32_t len;
    uint32_t used;
    uint32_t empties;
} list_stats;

/* Walks one list checking structure; the cap bounds a cyclic (corrupted)
 * list so it fails cleanly instead of spinning. */
static void check_list(const slab_node_t* n, bool full, uint32_t per_slab, uint32_t cap, list_stats* st) {
    for (const slab_node_t* prev = nullptr; n; prev = n, n = n->next) {
        st->len++;
        FUZZ_CHECK(st->len <= cap);
        FUZZ_CHECK(n->prev == prev);
        FUZZ_CHECK(n->is_full == full);
        FUZZ_CHECK(!full || n->used_count == per_slab);
        st->used += n->used_count;
        if (n->used_count == 0) {
            st->empties++;
        }
    }
}

/* Is @p a block-aligned pointer inside one of the multislab's slabs? */
static bool owned_by_some_slab(const multislab_t* ms, const void* p) {
    const slab_node_t* lists[2] = {ms->active, ms->full};
    for (size_t li = 0; li < 2; li++) {
        for (const slab_node_t* n = lists[li]; n; n = n->next) {
            const uintptr_t base = (uintptr_t)n->raw_memory;
            const uintptr_t v = (uintptr_t)p;
            if (v >= base && v < base + ms->slab_memory_size) {
                return ((v - base) % ms->block_size) == 0;
            }
        }
    }
    return false;
}

static void check_invariants(const multislab_t* ms, void* const* live, size_t n) {
    list_stats st = {0};
    check_list(ms->active, false, ms->blocks_per_slab, ms->slab_count, &st);
    check_list(ms->full, true, ms->blocks_per_slab, ms->slab_count, &st);

    FUZZ_CHECK(st.len == ms->slab_count);
    FUZZ_CHECK(st.used == n);
    FUZZ_CHECK(st.empties == ms->empty_count);

    /* each live block is genuinely owned and aligned */
    for (size_t i = 0; i < n; i++) {
        FUZZ_CHECK(owned_by_some_slab(ms, live[i]));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz_input in = fuzz_input_init(data, size);

    /* config header within ranges the allocator accepts */
    const size_t block_size = (size_t)CACHE_LINE_SIZE * fuzz_range(&in, 1, 4);
    const uint32_t per_slab = fuzz_range(&in, 1, PER_SLAB_MAX);
    const uint32_t max_slabs = fuzz_range(&in, 0, 4); /* 0 = unlimited */
    const uint32_t hysteresis = fuzz_range(&in, 0, 3);

    counting_ctx ctx = {.fail_mask = fuzz_u32(&in)};
    multislab_t ms;
    if (multislab_init_ext(&ms, block_size, per_slab, max_slabs, hysteresis, counting_alloc, counting_free, &ctx)) {
        return 0;
    }

    /* live blocks the allocator must be able to hand out, capacity permitting */
    const size_t capacity = max_slabs ? (size_t)max_slabs * per_slab : SIZE_MAX;

    void* live[LIVE_CAP];
    size_t n = 0;

    while (fuzz_remaining(&in) > 0) {
        const uint8_t op = fuzz_u8(&in);

        if ((op & 1u) == 0u) {
            /* allocate */
            if (n < LIVE_CAP) {
                ctx.injected = false;
                void* p = multislab_alloc(&ms);
                if (p) {
                    for (size_t i = 0; i < n; i++) {
                        FUZZ_CHECK(live[i] != p); /* must not alias a live block */
                    }
                    FUZZ_CHECK(owned_by_some_slab(&ms, p));
                    live[n++] = p;
                } else {
                    FUZZ_CHECK(n >= capacity || ctx.injected);
                }
            }
        } else {
            /* release a currently-live block */
            if (n > 0) {
                const size_t idx = fuzz_u8(&in) % n;
                multislab_release(&ms, live[idx]);
                live[idx] = live[--n];
            }
        }

        check_invariants(&ms, live, n);
    }

    /* drain everything that is still live */
    for (size_t i = 0; i < n; i++) {
        multislab_release(&ms, live[i]);
    }
    check_invariants(&ms, live, 0);

    multislab_destroy(&ms);

    /* destroy must return every byte the allocator requested */
    FUZZ_CHECK(ctx.live_bytes == 0);
    return 0;
}
