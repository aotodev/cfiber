/**
 * @file  test_scheduler.c
 * @brief Unit tests for the cooperative FCFS scheduler.
 *
 * @details
 * These tests actually run fibers (real context switches), so they exercise the
 * scheduler end-to-end: ready-queue ordering, cooperative yield, dynamic
 * spawning, capacity exhaustion, and the spawn/free churn that drives the
 * underlying multislab allocators.
 *
 * Fiber functions cannot use the fatal ASSERT_* macros (those `return` from the
 * enclosing function, and a fiber returning means completion). Instead fibers
 * record what they observed into a shared struct owned by the test function's
 * stack frame — valid because cfiber_scheduler_run() runs every fiber
 * synchronously before returning — and the test asserts afterwards.
 */

#include "cfiber/scheduler/scheduler.h"
#include "test/test.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Generous per-fiber stack: these fibers do little, but ASan / deep ABI
 * prologues appreciate the headroom. */
#define STACK_SIZE 16384

/* ============================================================================
 * Shared recording structures
 * ============================================================================ */

typedef struct {
    int seq[32];
    int len;
} sequence;

static void seq_push(sequence* s, int value) {
    if (s->len < (int)(sizeof(s->seq) / sizeof(s->seq[0]))) {
        s->seq[s->len] = value;
    }
    s->len++;
}

/* A fiber that records its id once and returns. */
typedef struct {
    sequence* seq;
    int id;
} simple_arg;

static void simple_fiber(void* p) {
    simple_arg* a = p;
    seq_push(a->seq, a->id);
}

/* A fiber that records (id*10 + iteration) and yields between iterations. */
typedef struct {
    sequence* seq;
    int id;
    int iters;
} yield_arg;

static void yield_fiber(void* p) {
    yield_arg* a = p;
    for (int i = 0; i < a->iters; i++) {
        seq_push(a->seq, (a->id * 10) + i);
        cfiber_yield();
    }
}

/* ============================================================================
 * counting backing allocator (leak balance)
 * ============================================================================ */

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
 * Tests
 * ============================================================================ */

static int test_scheduler_init_validation(void) {
    cfiber_scheduler_t sched;

    /* stack_size 0 and anything below the 256-byte floor is rejected */
    ASSERT_TRUE(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = 0}) != 0);
    ASSERT_TRUE(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = 255}) != 0);

    /* a valid configuration succeeds and can be destroyed with no live fibers */
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = 256}), 0);
    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_run_with_no_fibers(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    /* running with an empty ready queue must return immediately */
    cfiber_scheduler_run(&sched);
    ASSERT_EQ_U32(sched.active_count, 0);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_runs_single_fiber(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    sequence s = {0};
    simple_arg a = {.seq = &s, .id = 7};

    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, simple_fiber, &a));
    ASSERT_EQ_U32(sched.active_count, 1);

    cfiber_scheduler_run(&sched);

    ASSERT_EQ_U32(s.len, 1);
    ASSERT_EQ_U32(s.seq[0], 7);
    ASSERT_EQ_U32(sched.active_count, 0);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_fcfs_order(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    sequence s = {0};
    simple_arg a[3] = {{&s, 0}, {&s, 1}, {&s, 2}};

    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(cfiber_scheduler_spawn(&sched, simple_fiber, &a[i]));
    }
    ASSERT_EQ_U32(sched.active_count, 3);

    cfiber_scheduler_run(&sched);

    /* fibers that never yield run to completion in spawn (FCFS) order */
    ASSERT_EQ_U32(s.len, 3);
    ASSERT_EQ_U32(s.seq[0], 0);
    ASSERT_EQ_U32(s.seq[1], 1);
    ASSERT_EQ_U32(s.seq[2], 2);
    ASSERT_EQ_U32(sched.active_count, 0);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_yield_round_robin(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    sequence s = {0};
    yield_arg a0 = {.seq = &s, .id = 0, .iters = 3};
    yield_arg a1 = {.seq = &s, .id = 1, .iters = 3};

    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, yield_fiber, &a0));
    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, yield_fiber, &a1));

    cfiber_scheduler_run(&sched);

    /* two cooperatively-yielding fibers interleave one iteration at a time:
     * A0, B0, A1, B1, A2, B2  ->  0, 10, 1, 11, 2, 12 */
    const int expected[6] = {0, 10, 1, 11, 2, 12};
    ASSERT_EQ_U32(s.len, 6);
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_U32(s.seq[i], expected[i]);
    }

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_single_fiber_yield_is_noop(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    sequence s = {0};
    yield_arg a = {.seq = &s, .id = 0, .iters = 3};

    /* the lone fiber yields with an empty ready queue: yield returns
     * immediately and the fiber runs straight through without hanging */
    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, yield_fiber, &a));
    cfiber_scheduler_run(&sched);

    ASSERT_EQ_U32(s.len, 3);
    ASSERT_EQ_U32(s.seq[0], 0);
    ASSERT_EQ_U32(s.seq[1], 1);
    ASSERT_EQ_U32(s.seq[2], 2);
    ASSERT_EQ_U32(sched.active_count, 0);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

/* ---- dynamic spawn from within a running fiber ---- */

typedef struct {
    sequence* seq;
} dyn_ctx;

static void dyn_child(void* p) {
    dyn_ctx* c = p;
    seq_push(c->seq, 2);
}

static void dyn_parent(void* p) {
    dyn_ctx* c = p;
    seq_push(c->seq, 1);
    /* spawn on the thread-local current scheduler */
    cfiber_spawn(dyn_child, c);
}

static int test_scheduler_dynamic_spawn(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    sequence s = {0};
    dyn_ctx c = {.seq = &s};

    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, dyn_parent, &c));
    cfiber_scheduler_run(&sched);

    /* parent runs first, child it spawned runs after it completes */
    ASSERT_EQ_U32(s.len, 2);
    ASSERT_EQ_U32(s.seq[0], 1);
    ASSERT_EQ_U32(s.seq[1], 2);
    ASSERT_EQ_U32(sched.active_count, 0);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

/* ---- cfiber_scheduler_current() ---- */

static void capture_current(void* p) {
    cfiber_scheduler_t** out = p;
    *out = cfiber_scheduler_current();
}

static int test_scheduler_current(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    /* no scheduler is running outside cfiber_scheduler_run() */
    ASSERT_NULL(cfiber_scheduler_current());

    cfiber_scheduler_t* seen = nullptr;
    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, capture_current, &seen));
    cfiber_scheduler_run(&sched);

    /* inside a fiber, current() is the running scheduler */
    ASSERT_EQ_PTR(seen, &sched);
    /* and it is cleared again once run() returns */
    ASSERT_NULL(cfiber_scheduler_current());

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_spawn_fails_when_capacity_exhausted(void) {
    cfiber_scheduler_t sched;
    /* one task / stack slab, one fiber each, no growth => capacity == 1 */
    const cfiber_scheduler_config_t cfg = {
        .stack_size = STACK_SIZE,
        .fibers_per_slab = 1,
        .max_slabs = 1,
    };
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, cfg), 0);

    sequence s = {0};
    simple_arg a = {.seq = &s, .id = 42};

    /* first spawn fits, second exceeds capacity and must fail cleanly */
    ASSERT_TRUE(cfiber_scheduler_spawn(&sched, simple_fiber, &a));
    ASSERT_FALSE(cfiber_scheduler_spawn(&sched, simple_fiber, &a));
    ASSERT_EQ_U32(sched.active_count, 1);

    /* drain the one accepted fiber so destroy's precondition holds */
    cfiber_scheduler_run(&sched);
    ASSERT_EQ_U32(s.len, 1);
    ASSERT_EQ_U32(sched.active_count, 0);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_reuse_after_drain(void) {
    cfiber_scheduler_t sched;
    ASSERT_EQ_U32(cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}), 0);

    sequence s = {0};

    /* run, fully drain, then run again on the same scheduler — slabs and
     * task nodes from the first batch must be reusable */
    for (int round = 0; round < 2; round++) {
        simple_arg a = {.seq = &s, .id = round};
        ASSERT_TRUE(cfiber_scheduler_spawn(&sched, simple_fiber, &a));
        cfiber_scheduler_run(&sched);
        ASSERT_EQ_U32(sched.active_count, 0);
    }

    ASSERT_EQ_U32(s.len, 2);
    ASSERT_EQ_U32(s.seq[0], 0);
    ASSERT_EQ_U32(s.seq[1], 1);

    cfiber_scheduler_destroy(&sched);
    return 0;
}

static int test_scheduler_no_leak_via_counting_allocator(void) {
    counting_ctx ctx = {0};
    cfiber_scheduler_t sched;
    const cfiber_scheduler_config_t cfg = {
        .stack_size = STACK_SIZE,
        .fibers_per_slab = 4,
    };
    ASSERT_EQ_U32(cfiber_scheduler_init_ext(&sched, cfg, counting_alloc, counting_free, &ctx), 0);

    /* a workload mixing plain fibers and yielding fibers across several slabs */
    sequence s = {0};
    simple_arg simple[6];
    yield_arg yielding[4];
    for (int i = 0; i < 6; i++) {
        simple[i] = (simple_arg){.seq = &s, .id = i};
        ASSERT_TRUE(cfiber_scheduler_spawn(&sched, simple_fiber, &simple[i]));
    }
    for (int i = 0; i < 4; i++) {
        yielding[i] = (yield_arg){.seq = &s, .id = 100 + i, .iters = 3};
        ASSERT_TRUE(cfiber_scheduler_spawn(&sched, yield_fiber, &yielding[i]));
    }

    cfiber_scheduler_run(&sched);
    ASSERT_EQ_U32(sched.active_count, 0);

    ASSERT_TRUE(ctx.allocs > 0);
    cfiber_scheduler_destroy(&sched);

    /* every byte requested from the backing allocator must be returned */
    ASSERT_EQ_U32(ctx.allocs, ctx.frees);
    ASSERT_EQ_U64((uint64_t)ctx.live_bytes, 0);
    return 0;
}

/* ============================================================================
 * Runner
 * ============================================================================ */

int main(void) {
    cfiber_test_suite_begin("cooperative scheduler");

    RUN_TEST(test_scheduler_init_validation);
    RUN_TEST(test_scheduler_run_with_no_fibers);
    RUN_TEST(test_scheduler_runs_single_fiber);
    RUN_TEST(test_scheduler_fcfs_order);
    RUN_TEST(test_scheduler_yield_round_robin);
    RUN_TEST(test_scheduler_single_fiber_yield_is_noop);
    RUN_TEST(test_scheduler_dynamic_spawn);
    RUN_TEST(test_scheduler_current);
    RUN_TEST(test_scheduler_spawn_fails_when_capacity_exhausted);
    RUN_TEST(test_scheduler_reuse_after_drain);
    RUN_TEST(test_scheduler_no_leak_via_counting_allocator);

    return cfiber_test_report();
}
