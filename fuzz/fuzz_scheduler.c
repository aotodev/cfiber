/**
 * @file  fuzz_scheduler.c
 * @brief Coverage-guided fuzzer for the cooperative scheduler.
 *
 * @details
 * Interprets the input as a scheduler configuration plus a set of fiber "plans"
 * (how many times each fiber yields and how many children it spawns). It then
 * runs the scheduler to completion. Because fibers run real context switches,
 * this exercises spawn / yield / dynamic-spawn / completion and the spawn-free
 * churn of the underlying multislab allocators — under ASan (with the
 * fiber-aware redzone + switch annotations active) and UBSan.
 *
 * Invariants checked after the run:
 *   - every fiber that started also finished (no fiber lost mid-flight);
 *   - active_count returns to zero;
 *   - the counting backing allocator is balanced after destroy (no leak).
 *
 * Plans are drawn from a fixed-size arena, which bounds both the number of
 * fibers and the dynamic-spawn recursion depth (spawned children are leaves).
 */

#include "cfiber/scheduler/scheduler.h"
#include "fuzz_input.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define ARENA_CAP 256
#define STACK_SIZE 8192

typedef struct fuzz_state fuzz_state;

typedef struct {
    fuzz_state* st;
    uint8_t yields;
    uint8_t children;
} fiber_plan;

struct fuzz_state {
    fiber_plan arena[ARENA_CAP];
    size_t arena_next;
    uint32_t started;
    uint32_t finished;
};

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

static void worker_fiber(void* arg) {
    fiber_plan* plan = arg;
    fuzz_state* st = plan->st;

    st->started++;

    for (uint8_t i = 0; i < plan->yields; i++) {
        cfiber_yield();
    }

    /* spawn leaf children from the shared arena (bounded) */
    for (uint8_t i = 0; i < plan->children; i++) {
        if (st->arena_next >= ARENA_CAP) {
            break;
        }
        fiber_plan* child = &st->arena[st->arena_next++];
        child->st = st;
        child->yields = 0;
        child->children = 0;
        cfiber_spawn(worker_fiber, child);
    }

    st->finished++;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz_input in = fuzz_input_init(data, size);

    const cfiber_scheduler_config_t cfg = {
        .stack_size = STACK_SIZE,
        .fibers_per_slab = fuzz_range(&in, 1, 8),
        .max_slabs = fuzz_range(&in, 0, 8), /* 0 = unlimited */
    };

    counting_ctx ctx = {0};
    cfiber_scheduler_t sched;
    if (cfiber_scheduler_init_ext(&sched, cfg, counting_alloc, counting_free, &ctx) != 0) {
        return 0;
    }

    /* zero-initialised: arena_next, started, finished all start at 0 */
    static fuzz_state st;
    st.arena_next = 0;
    st.started = 0;
    st.finished = 0;

    const uint32_t initial = fuzz_range(&in, 1, 32);
    for (uint32_t i = 0; i < initial && st.arena_next < ARENA_CAP; i++) {
        fiber_plan* plan = &st.arena[st.arena_next++];
        plan->st = &st;
        plan->yields = (uint8_t)fuzz_range(&in, 0, 7);
        plan->children = (uint8_t)fuzz_range(&in, 0, 3);
        cfiber_scheduler_spawn(&sched, worker_fiber, plan);
    }

    cfiber_scheduler_run(&sched);

    /* every fiber that began running also ran to completion */
    FUZZ_CHECK(st.started == st.finished);
    FUZZ_CHECK(sched.active_count == 0);

    cfiber_scheduler_destroy(&sched);

    FUZZ_CHECK(ctx.allocs == ctx.frees);
    FUZZ_CHECK(ctx.live_bytes == 0);
    return 0;
}
