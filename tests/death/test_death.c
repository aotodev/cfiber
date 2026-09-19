/**
 * @file  test_death.c
 * @brief Misuse that must trap or abort, observed through a forked child.
 *
 * @details
 * The library's guards are ASSERT() in debug builds, a __builtin_trap, and a
 * libc assert in the growable stack pool. test_defensive covers the release
 * behaviour of the same paths under NDEBUG; this suite covers the debug
 * behaviour, so each guard is exercised in both configurations. ASSERT_DEATH
 * runs the misuse in a child and passes only if the child dies. States the
 * library traps in every build are covered here in both. Hosted only.
 */

#include "cfiber/memory/multislab_alloc.h"
#include "cfiber/memory/slab_alloc.h"
#include "cfiber/scheduler/scheduler.h"
#include "cfiber/stack/growable_stack.h"
#include "cfiber/stack/growable_stack_allocator.h"
#include "cfiber/stack/stack.h"
#include "test/test.h"

#ifdef CFIBER_TEST_HAVE_REACTOR
#include "cfiber/reactor/reactor.h"
#endif

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define BLOCK CFIBER_CACHE_LINE_SIZE
#define STACK_SIZE 16384

/* ============================================================================
 * the helper itself
 * ============================================================================ */

static void child_aborts(void* arg) {
    (void)arg;
    abort();
}

static void child_returns(void* arg) {
    (void)arg;
}

static void child_exits_nonzero(void* arg) {
    (void)arg;
    _exit(3);
}

static int test_death_helper(void) {
    ASSERT_TRUE(cfiber_test_dies(child_aborts, nullptr));
    ASSERT_FALSE(cfiber_test_dies(child_returns, nullptr));
    ASSERT_TRUE(cfiber_test_dies(child_exits_nonzero, nullptr));
    return 0;
}

/* ============================================================================
 * unrecoverable states: trap in every build
 * ============================================================================ */

static cfiber_context_t g_main_ctx;
static cfiber_t g_fiber;
static uint8_t g_stack[STACK_SIZE];

static void fiber_returns(void* arg) {
    (void)arg;
}

static void return_without_hook(void* arg) {
    (void)arg;
    g_fiber.stack = g_stack;
    g_fiber.stack_size = sizeof g_stack;
    cfiber_init(&g_fiber, fiber_returns, nullptr);
    (void)cfiber_set_return_hook(nullptr, nullptr);
    cfiber_switch_context(&g_main_ctx, &g_fiber.ctx); /* returns into the epilogue */
}

static void switch_to_scheduler_by_hand(void* arg) {
    (void)arg;
    cfiber_scheduler_t* s = cfiber_scheduler_current();
    /* Bypasses cfiber_yield: not re-enqueued, active_count stays 1. */
    cfiber_switch_context(&s->current->fiber.ctx, &s->sched_ctx);
}

static void run_with_lost_fiber(void* arg) {
    (void)arg;
    cfiber_scheduler_t sched;
    if (cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}) != 0) {
        _exit(0);
    }
    (void)cfiber_scheduler_spawn(&sched, switch_to_scheduler_by_hand, nullptr);
    cfiber_scheduler_run(&sched); /* ready queue empty, one live fiber */
}

static int test_unrecoverable_states_trap(void) {
    ASSERT_DEATH(return_without_hook, nullptr);
    ASSERT_DEATH(run_with_lost_fiber, nullptr);
    return 0;
}

#ifndef NDEBUG
/* ============================================================================
 * allocators
 * ============================================================================ */

static void slab_foreign_release(void* arg) {
    (void)arg;
    alignas(BLOCK) static uint8_t mem[BLOCK * 2];
    alignas(BLOCK) static uint8_t other[BLOCK];
    cfiber_slab_t s;
    if (cfiber_slab_init(&s, BLOCK, mem, sizeof mem) != 0) {
        _exit(0);
    }
    (void)cfiber_slab_alloc(&s);
    (void)cfiber_slab_release(&s, other);
}

static void slab_double_free(void* arg) {
    (void)arg;
    alignas(BLOCK) static uint8_t mem[BLOCK * 2];
    cfiber_slab_t s;
    if (cfiber_slab_init(&s, BLOCK, mem, sizeof mem) != 0) {
        _exit(0);
    }
    void* a = cfiber_slab_alloc(&s);
    (void)cfiber_slab_release(&s, a);
    (void)cfiber_slab_release(&s, a);
}

static void multislab_foreign_release(void* arg) {
    (void)arg;
    cfiber_multislab_t ms;
    if (cfiber_multislab_init(&ms, BLOCK, 4, 0, 1) != 0) {
        _exit(0);
    }
    (void)cfiber_multislab_alloc(&ms);
    int stranger = 0;
    cfiber_multislab_release(&ms, &stranger);
}

static void growable_release_to_wrong_pool(void* arg) {
    (void)arg;
    const long ps = sysconf(_SC_PAGESIZE);
    cfiber_growable_stack_allocator_t* pool = cfiber_growable_stack_allocator_create(
        (cfiber_growable_stack_allocator_args_t){.max_stack_size = (size_t)ps * 4, .cache_capacity = 2});
    cfiber_stack_t foreign = cfiber_growable_stack_create((size_t)ps * 8);
    if (!pool || !cfiber_stack_is_valid(&foreign)) {
        _exit(0);
    }
    cfiber_growable_stack_release(pool, &foreign); /* libc assert */
}

static int test_allocator_guards_trap(void) {
    ASSERT_DEATH(slab_foreign_release, nullptr);
    ASSERT_DEATH(slab_double_free, nullptr);
    ASSERT_DEATH(multislab_foreign_release, nullptr);
    ASSERT_DEATH(growable_release_to_wrong_pool, nullptr);
    return 0;
}

/* ============================================================================
 * scheduler
 * ============================================================================ */

static void yield_outside_run(void* arg) {
    (void)arg;
    cfiber_yield();
}

static void spawn_outside_run(void* arg) {
    (void)arg;
    (void)cfiber_spawn(yield_outside_run, nullptr);
}

static void noop_fiber(void* arg) {
    (void)arg;
}

static void destroy_with_live_fiber(void* arg) {
    (void)arg;
    cfiber_scheduler_t sched;
    if (cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}) != 0) {
        _exit(0);
    }
    (void)cfiber_scheduler_spawn(&sched, noop_fiber, nullptr);
    cfiber_scheduler_destroy(&sched); /* never run: one live fiber */
}

static void reenter_run_fiber(void* arg) {
    cfiber_scheduler_run(arg); /* the scheduler that is running us */
}

static void run_reentered(void* arg) {
    (void)arg;
    cfiber_scheduler_t sched;
    if (cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}) != 0) {
        _exit(0);
    }
    (void)cfiber_scheduler_spawn(&sched, reenter_run_fiber, &sched);
    cfiber_scheduler_run(&sched);
}

static int test_scheduler_guards_trap(void) {
    ASSERT_DEATH(yield_outside_run, nullptr);
    ASSERT_DEATH(spawn_outside_run, nullptr);
    ASSERT_DEATH(destroy_with_live_fiber, nullptr);
    ASSERT_DEATH(run_reentered, nullptr);
    return 0;
}

#ifdef CFIBER_TEST_HAVE_REACTOR
static void reactor_api_off_thread(void* arg) {
    (void)arg;
    (void)cfiber_ev_wait_async(0);
}

static int test_reactor_guards_trap(void) {
    ASSERT_DEATH(reactor_api_off_thread, nullptr);
    return 0;
}
#endif
#endif /* !NDEBUG */

#if CFIBER_ASAN_ENABLED
/* ============================================================================
 * ASan redzone below a scheduler fiber stack
 * ============================================================================ */

static void overflow_into_redzone(void* arg) {
    (void)arg;
    cfiber_task_t* me = cfiber_scheduler_current()->current;
    volatile uint8_t* below = (volatile uint8_t*)me->stack.usable_base - 1;
    *below = 0xFF; /* poisoned: ASan reports and exits non-zero */
}

static void run_overflowing_fiber(void* arg) {
    (void)arg;
    cfiber_scheduler_t sched;
    if (cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = STACK_SIZE}) != 0) {
        _exit(0);
    }
    (void)cfiber_scheduler_spawn(&sched, overflow_into_redzone, nullptr);
    cfiber_scheduler_run(&sched);
    cfiber_scheduler_destroy(&sched);
}

static int test_asan_redzone_catches_overflow(void) {
    ASSERT_DEATH(run_overflowing_fiber, nullptr);
    return 0;
}
#endif

int main(void) {
    cfiber_test_suite_begin("death tests (traps, aborts, sanitizer reports)");

    RUN_TEST(test_death_helper);
    RUN_TEST(test_unrecoverable_states_trap);
#ifndef NDEBUG
    RUN_TEST(test_allocator_guards_trap);
    RUN_TEST(test_scheduler_guards_trap);
#ifdef CFIBER_TEST_HAVE_REACTOR
    RUN_TEST(test_reactor_guards_trap);
#endif
#endif
#if CFIBER_ASAN_ENABLED
    RUN_TEST(test_asan_redzone_catches_overflow);
#endif

    return cfiber_test_report();
}
