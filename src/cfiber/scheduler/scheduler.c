#include "cfiber/scheduler/scheduler.h"

#include "cfiber/debug/asan.h"

#include <string.h>

/* ============================================================================
 * Thread-local current scheduler
 * ============================================================================ */
#if defined(__arm__) && !defined(__aarch64__)
/* Bare-metal ARM Cortex-M has no TLS support; use a plain global. */
static cfiber_scheduler_t* s_current_sched;
#else
static thread_local cfiber_scheduler_t* s_current_sched;
#endif

/* ============================================================================
 * Ready-queue helpers (singly-linked FIFO)
 * ============================================================================ */

static inline void enqueue_ready(cfiber_scheduler_t* s, cfiber_task_t* t) {
    t->next = nullptr;
    if (s->ready_tail) {
        s->ready_tail->next = t;
    } else {
        s->ready_head = t;
    }
    s->ready_tail = t;
}

static inline cfiber_task_t* dequeue_ready(cfiber_scheduler_t* s) {
    cfiber_task_t* t = s->ready_head;
    if (!t) {
        return nullptr;
    }
    s->ready_head = t->next;
    if (!s->ready_head) {
        s->ready_tail = nullptr;
    }
    t->next = nullptr;
    return t;
}

/* ============================================================================
 * Internal task lifecycle
 * ============================================================================ */

static void task_free(cfiber_scheduler_t* s, cfiber_task_t* t) {
    /* fiber.stack points just above the poisoned redzone; recover the slab
     * block base (which the allocator expects) and clear the poison so the
     * block can be reused. Both are no-ops when ASan is disabled (REDZONE 0). */
    uint8_t* block = t->fiber.stack - CFIBER_ASAN_REDZONE;
    cfiber_asan_unpoison(block, CFIBER_ASAN_REDZONE);
    multislab_release(&s->stack_alloc, block);
    multislab_release(&s->task_alloc, t);
}

/* ============================================================================
 * scheduler_return_fiber  —  fiber.h contract
 *
 * Called by the fiber prologue/epilogue assembly when a fiber's entry
 * function returns.  We mark the task as a zombie and return to the
 * scheduler loop so it can safely free the stack we are currently on.
 * ============================================================================ */

[[noreturn]] void scheduler_return_fiber(void) {
    cfiber_scheduler_t* s = s_current_sched;
    ASSERT(s && "scheduler_return_fiber: no active scheduler");

    cfiber_task_t* dead = s->current;
    s->zombie = dead;
    s->current = nullptr;
    s->active_count--;

    /* Return to the scheduler loop (cfiber_scheduler_run) which can safely
     * deallocate the zombie since it runs on the caller's stack. The fiber is
     * terminating, so tell ASan to discard its fake stack (finishing = true). */
    const void* host_low;
    size_t host_size;
    cfiber_asan_host_bounds(&host_low, &host_size);
    cfiber_asan_switch(&dead->fiber.ctx, &s->sched_ctx, host_low, host_size, true);
    __builtin_unreachable();
}

/* ============================================================================
 * Public API
 * ============================================================================ */

static constexpr uint32_t DEFAULT_PER_SLAB = 16;

int cfiber_scheduler_init_ext(cfiber_scheduler_t* sched,
                              cfiber_scheduler_config_t config,
                              void* (*mem_alloc)(size_t, void*),
                              void (*mem_free)(void*, size_t, void*),
                              void* mem_ctx) {
    if (!config.stack_size || config.stack_size < 256) {
        return -1;
    }

    memset(sched, 0, sizeof(*sched));
    sched->stack_size = config.stack_size;

    const uint32_t per_slab = config.fibers_per_slab ? config.fibers_per_slab : DEFAULT_PER_SLAB;
    const size_t task_block = align_up(sizeof(cfiber_task_t), CACHE_LINE_SIZE);

    int rc;
    if (mem_alloc && mem_free) {
        rc = multislab_init_ext(
            &sched->task_alloc, task_block, per_slab, config.max_slabs, 1, mem_alloc, mem_free, mem_ctx);
    } else {
        rc = multislab_init(&sched->task_alloc, task_block, per_slab, config.max_slabs, 1);
    }
    if (rc != 0) {
        return rc;
    }

    /* Reserve a poisoned guard below each stack so a downward overflow trips
     * ASan instead of silently corrupting the adjacent stack in the slab. The
     * redzone is added on top of the requested usable size and rounds back up
     * to the cache-line granularity the slab requires. REDZONE is 0 (and the
     * block size unchanged) when ASan is disabled. */
    const size_t stack_block = align_up(config.stack_size + CFIBER_ASAN_REDZONE, CACHE_LINE_SIZE);

    if (mem_alloc && mem_free) {
        rc = multislab_init_ext(
            &sched->stack_alloc, stack_block, per_slab, config.max_slabs, 1, mem_alloc, mem_free, mem_ctx);
    } else {
        rc = multislab_init(&sched->stack_alloc, stack_block, per_slab, config.max_slabs, 1);
    }
    if (rc != 0) {
        multislab_destroy(&sched->task_alloc);
        return rc;
    }

    return 0;
}

int cfiber_scheduler_init(cfiber_scheduler_t* sched, cfiber_scheduler_config_t config) {
    return cfiber_scheduler_init_ext(sched, config, nullptr, nullptr, nullptr);
}

void cfiber_scheduler_destroy(cfiber_scheduler_t* sched) {
    ASSERT(sched->active_count == 0 && "destroying scheduler with live fibers");

    multislab_destroy(&sched->task_alloc);
    multislab_destroy(&sched->stack_alloc);
    memset(sched, 0, sizeof(*sched));
}

bool cfiber_scheduler_spawn(cfiber_scheduler_t* sched, fiber_fn func, void* user_data) {
    cfiber_task_t* task = multislab_alloc(&sched->task_alloc);
    if (UNLIKELY(!task)) {
        return false;
    }

    uint8_t* block = multislab_alloc(&sched->stack_alloc);
    if (UNLIKELY(!block)) {
        multislab_release(&sched->task_alloc, task);
        return false;
    }

    /* Poison the redzone at the bottom of the block; the usable stack starts
     * just above it. No-op / zero offset when ASan is disabled. */
    cfiber_asan_poison(block, CFIBER_ASAN_REDZONE);

    task->fiber.stack = block + CFIBER_ASAN_REDZONE;
    task->fiber.stack_size = sched->stack_size;
    memset(&task->fiber.ctx, 0, sizeof(context_t));
    task->next = nullptr;

    init_fiber(&task->fiber, func, user_data);

    enqueue_ready(sched, task);
    sched->active_count++;

    return true;
}

void cfiber_scheduler_run(cfiber_scheduler_t* sched) {
    s_current_sched = sched;

    while (sched->active_count > 0) {
        /* Free the zombie from the previous iteration (safe — we are on the
         * caller's stack, not the zombie's). */
        if (sched->zombie) {
            task_free(sched, sched->zombie);
            sched->zombie = nullptr;
        }

        cfiber_task_t* next = dequeue_ready(sched);
        if (UNLIKELY(!next)) {
            break; /* shouldn't happen, but guard against it */
        }

        sched->current = next;
        cfiber_asan_switch(&sched->sched_ctx, &next->fiber.ctx, next->fiber.stack, next->fiber.stack_size, false);
        /* control returns here when the fiber yields or completes */
    }

    /* Clean up the very last zombie (if the final fiber completed) */
    if (sched->zombie) {
        task_free(sched, sched->zombie);
        sched->zombie = nullptr;
    }

    s_current_sched = nullptr;
}

/* ============================================================================
 * In-fiber helpers
 * ============================================================================ */

cfiber_scheduler_t* cfiber_scheduler_current(void) {
    return s_current_sched;
}

void cfiber_yield(void) {
    cfiber_scheduler_t* s = s_current_sched;
    ASSERT(s && "cfiber_yield: no active scheduler");

    cfiber_task_t* cur = s->current;
    if (!cur) {
        return;
    }

    /* If we are the only fiber, yielding is a no-op. */
    if (!s->ready_head) {
        return;
    }

    /* Re-enqueue current and return to the scheduler loop. */
    enqueue_ready(s, cur);
    s->current = nullptr;
    const void* host_low;
    size_t host_size;
    cfiber_asan_host_bounds(&host_low, &host_size);
    cfiber_asan_switch(&cur->fiber.ctx, &s->sched_ctx, host_low, host_size, false);
}

bool cfiber_spawn(fiber_fn func, void* user_data) {
    cfiber_scheduler_t* s = s_current_sched;
    ASSERT(s && "cfiber_spawn: no active scheduler");
    return cfiber_scheduler_spawn(s, func, user_data);
}
