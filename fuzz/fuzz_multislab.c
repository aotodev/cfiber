/**
 * @file  fuzz_multislab.c
 * @brief Coverage-guided fuzzer for the slab / multislab allocator.
 *
 * @details
 * Interprets the input as: a small config header (block size, blocks-per-slab,
 * max-slabs, hysteresis) followed by an opcode stream of alloc / release
 * operations. Only valid operations are issued — releases always target a
 * currently-live block — so the allocator's defensive ASSERT paths (double-free,
 * foreign pointer) are never tripped; this fuzzer hunts logic and memory bugs,
 * which ASan/UBSan and the structural invariants below surface.
 *
 * After every operation it checks:
 *   - slab_count == len(active list) + len(full list);
 *   - the sum of every slab's used_count equals the number of live blocks;
 *   - each live pointer lies on a block boundary inside exactly one slab;
 *   - allocations never alias a still-live block.
 * On teardown it checks (via a counting backing allocator) that destroy returns
 * every byte the allocator ever requested.
 *
 * The sum-of-used-counts and uniqueness invariants are exactly what caught the
 * lazy full-list corruption bug; this drives that class of bug across random
 * configurations and interleavings.
 */

#include "cfiber/memory/multislab_alloc.h"
#include "fuzz_input.h"

#include <stdint.h>
#include <stdlib.h>

#define LIVE_CAP 256

typedef struct {
    size_t live_bytes;
    unsigned long allocs;
    unsigned long frees;
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

/* Bounded length: if the list is longer than @p cap nodes it must contain a
 * cycle (list corruption), which we report as a clean failure rather than
 * spinning forever. */
static uint32_t list_len(const slab_node_t* n, uint32_t cap) {
    uint32_t c = 0;
    for (; n; n = n->next) {
        c++;
        FUZZ_CHECK(c <= cap);
    }
    return c;
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
    /* every slab is on exactly one of the two lists (cap traversal at
     * slab_count so a corrupted/cyclic list fails cleanly instead of hanging) */
    FUZZ_CHECK(ms->slab_count == list_len(ms->active, ms->slab_count) + list_len(ms->full, ms->slab_count));

    /* the slabs collectively account for exactly the live blocks */
    uint32_t used_sum = 0;
    for (const slab_node_t* node = ms->active; node; node = node->next) {
        used_sum += node->used_count;
    }
    for (const slab_node_t* node = ms->full; node; node = node->next) {
        used_sum += node->used_count;
    }
    FUZZ_CHECK(used_sum == n);

    /* each live block is genuinely owned and aligned */
    for (size_t i = 0; i < n; i++) {
        FUZZ_CHECK(owned_by_some_slab(ms, live[i]));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz_input in = fuzz_input_init(data, size);

    /* config header within ranges the allocator accepts */
    const size_t block_size = (size_t)CACHE_LINE_SIZE * fuzz_range(&in, 1, 4);
    const uint32_t per_slab = fuzz_range(&in, 1, 32);
    const uint32_t max_slabs = fuzz_range(&in, 0, 4); /* 0 = unlimited */
    const uint32_t hysteresis = fuzz_range(&in, 0, 3);

    counting_ctx ctx = {0};
    multislab_t ms;
    if (multislab_init_ext(&ms, block_size, per_slab, max_slabs, hysteresis, counting_alloc, counting_free, &ctx)
        != 0) {
        return 0;
    }

    void* live[LIVE_CAP];
    size_t n = 0;

    while (fuzz_remaining(&in) > 0) {
        const uint8_t op = fuzz_u8(&in);

        if ((op & 1u) == 0u) {
            /* allocate */
            if (n < LIVE_CAP) {
                void* p = multislab_alloc(&ms);
                if (p) {
                    for (size_t i = 0; i < n; i++) {
                        FUZZ_CHECK(live[i] != p); /* must not alias a live block */
                    }
                    FUZZ_CHECK(owned_by_some_slab(&ms, p));
                    live[n++] = p;
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
    FUZZ_CHECK(ctx.allocs == ctx.frees);
    FUZZ_CHECK(ctx.live_bytes == 0);
    return 0;
}
