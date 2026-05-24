/**
 * @file  scheduler.h
 * @brief Cooperative FCFS fiber scheduler backed by multislab allocation.
 *
 * @details Provides a non-preemptive, first-come-first-served scheduler that
 *          manages fiber lifecycle entirely through the cfiber multislab
 *          allocator.  Both task metadata and fiber stacks are slab-allocated,
 *          so no external heap (malloc/free) is required when a custom backing
 *          allocator is supplied via cfiber_scheduler_init_ext().
 *
 *          The scheduler grows dynamically as fibers are spawned — there is
 *          no compile-time limit on the number of concurrent fibers.
 *
 * @section usage Usage
 * @code
 * cfiber_scheduler_t sched;
 * cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){
 *     .stack_size      = 8192,
 *     .fibers_per_slab = 16,
 * });
 *
 * cfiber_scheduler_spawn(&sched, my_fiber_fn, user_data);
 * cfiber_scheduler_run(&sched);     // blocks until all fibers complete
 * cfiber_scheduler_destroy(&sched);
 * @endcode
 */

#ifndef CFIBER_SCHEDULER_H
#define CFIBER_SCHEDULER_H

#include "cfiber/core/macros.h"
#include "cfiber/fiber/fiber.h"
#include "cfiber/memory/multislab_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Internal task node linking a fiber to the scheduler's ready queue.
 * @details Each spawned fiber is represented by a cfiber_task_t that embeds the
 *          fiber_t and a singly-linked-list pointer for the FIFO ready queue.
 */
typedef struct cfiber_task {
    /** Embedded fiber (context + stack pointer). */
    fiber_t fiber;
    /** Next node in the ready queue (or NULL). */
    struct cfiber_task* next;
} cfiber_task_t;

/**
 * @brief Cooperative FCFS fiber scheduler.
 * @details Manages a set of fibers using two multislab allocators: one for
 *          task metadata (cfiber_task_t) and one for fiber stacks. The struct
 *          is fully visible so it can be stack- or statically allocated.
 */
typedef struct cfiber_scheduler {
    /** Slab allocator for cfiber_task_t nodes. */
    multislab_t task_alloc;
    /** Slab allocator for fiber stack memory. */
    multislab_t stack_alloc;

    /** Currently executing task (not in the queue). */
    cfiber_task_t* current;
    /** Completed task pending deallocation. */
    cfiber_task_t* zombie;

    /** Head of the FIFO ready queue. */
    cfiber_task_t* ready_head;
    /** Tail of the FIFO ready queue. */
    cfiber_task_t* ready_tail;

    /** Saved context of the scheduler loop. */
    context_t sched_ctx;
    /** Per-fiber stack size in bytes. */
    size_t stack_size;
    /** Number of live (ready + running) fibers. */
    uint32_t active_count;
} cfiber_scheduler_t;

/**
 * @brief Configuration for scheduler initialisation.
 */
typedef struct {
    /** Size of each fiber stack in bytes. Must be >= 256 and a multiple of CACHE_LINE_SIZE. */
    size_t stack_size;
    /** Fibers (and stacks) per slab. 0 selects the default (16). */
    uint32_t fibers_per_slab;
    /** Maximum number of slabs (0 = unlimited). */
    uint32_t max_slabs;
} cfiber_scheduler_config_t;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * @brief Initialise a scheduler with the default (malloc-backed) allocator.
 * @param sched  Pointer to the scheduler to initialise (must not be NULL).
 * @param config Configuration parameters.
 * @return 0 on success, -1 on invalid configuration.
 */
CFIBER_EXPORT int cfiber_scheduler_init(cfiber_scheduler_t* sched, cfiber_scheduler_config_t config)
    __attribute__((nonnull(1)));

/**
 * @brief Initialise a scheduler with a user-provided backing allocator.
 * @param sched      Pointer to the scheduler to initialise.
 * @param config     Configuration parameters.
 * @param mem_alloc  Allocation callback (receives size and user context).
 * @param mem_free   De-allocation callback.
 * @param mem_ctx    Opaque context forwarded to callbacks.
 * @return 0 on success, -1 on error.
 *
 * @note Passing NULL for both @p mem_alloc and @p mem_free falls back to
 *       the default malloc/free implementation (equivalent to
 *       cfiber_scheduler_init()).
 */
CFIBER_EXPORT int cfiber_scheduler_init_ext(cfiber_scheduler_t* sched,
                                            cfiber_scheduler_config_t config,
                                            void* (*mem_alloc)(size_t, void*),
                                            void (*mem_free)(void*, size_t, void*),
                                            void* mem_ctx) __attribute__((nonnull(1)));

/**
 * @brief Destroy a scheduler and release all backing memory.
 * @param sched Pointer to the scheduler to destroy.
 * @pre All fibers must have completed (active_count == 0).
 */
CFIBER_EXPORT void cfiber_scheduler_destroy(cfiber_scheduler_t* sched) __attribute__((nonnull(1)));

/* ============================================================================
 * Spawning & Running
 * ============================================================================ */

/**
 * @brief Spawn a new fiber on the given scheduler.
 * @param sched     The scheduler to spawn the fiber on.
 * @param func      Entry-point function for the fiber.
 * @param user_data Pointer forwarded to @p func when the fiber starts.
 * @return true on success, false if allocation fails.
 *
 * @note Safe to call both before cfiber_scheduler_run() (to seed the initial
 *       set of fibers) and from within a running fiber (dynamic spawning).
 */
CFIBER_EXPORT bool cfiber_scheduler_spawn(cfiber_scheduler_t* sched, fiber_fn func, void* user_data)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Run the scheduler until every fiber has completed.
 * @param sched The scheduler to run.
 * @details Blocks the calling thread.  Sets the thread-local "current
 *          scheduler" so that cfiber_yield() and cfiber_spawn() are usable
 *          from within fibers.
 */
CFIBER_EXPORT void cfiber_scheduler_run(cfiber_scheduler_t* sched) __attribute__((nonnull(1)));

/* ============================================================================
 * In-fiber helpers
 * ============================================================================ */

/**
 * @brief Return the thread-local scheduler that is currently executing.
 * @return The active scheduler, or NULL if no scheduler is running.
 */
CFIBER_EXPORT cfiber_scheduler_t* cfiber_scheduler_current(void);

/**
 * @brief Yield the current fiber back to the scheduler.
 * @details The fibre is placed at the back of the ready queue and the
 *          scheduler dispatches the next one.  If no other fiber is ready
 *          this is a no-op.
 * @pre Must be called from within a running fiber.
 */
CFIBER_EXPORT void cfiber_yield(void);

/**
 * @brief Convenience wrapper: spawn on the current thread-local scheduler.
 * @param func      Entry-point function.
 * @param user_data Pointer forwarded to @p func.
 * @return true on success.
 * @pre Must be called from within a running fiber (scheduler active).
 */
CFIBER_EXPORT bool cfiber_spawn(fiber_fn func, void* user_data) __attribute__((nonnull(1)));

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_SCHEDULER_H */
