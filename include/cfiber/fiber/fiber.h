/**
 * @file  fiber.h
 * @brief High-level fiber (stackful coroutine) management interface.
 *
 * @details Provides the main API for creating and managing fibers. Fibers are
 *          lightweight, cooperatively-scheduled execution contexts that enable
 *          writing concurrent code in a sequential style.
 *
 *          A fiber has its own stack and execution context, and explicitly
 *          yields control to other fibers. This is in contrast to OS threads,
 *          which are preemptively scheduled by the operating system.
 *
 * @section usage Basic Usage
 *   1. Allocate a stack for the fiber.
 *   2. Create a fiber_t structure and set stack/stack_size.
 *   3. Call init_fiber() with your fiber function and user data.
 *   4. Use switch_context() to switch between fibers.
 *   5. Implement scheduler_return_fiber() to handle fiber completion.
 *
 * @section example Example
 * @code
 * void my_fiber_func(void* data) {
 *     printf("Fiber running with data: %p\n", data);
 *     yield();
 * }
 *
 * fiber_t fiber = {
 *     .stack = malloc(8192),
 *     .stack_size = 8192
 * };
 * init_fiber(&fiber, my_fiber_func, my_data);
 * @endcode
 */

#ifndef CFIBER_FIBER_H
#define CFIBER_FIBER_H

#include "cfiber/core/macros.h"
#include "cfiber/fiber/context.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fiber structure containing execution state and stack.
 * @details A fiber is a unit of execution with its own stack and CPU context.
 *          Before using a fiber, allocate its stack and initialize it with
 *          init_fiber().
 */
typedef struct {
    /**
     * @brief CPU context (registers, stack pointer, etc).
     * @details Stores the fiber's execution state when it is not running.
     *          Managed automatically by switch_context() and init_fiber().
     */
    context_t ctx;

    /**
     * @brief Pointer to the fiber's stack memory.
     * @details Must be allocated by the user before calling init_fiber().
     *          The stack grows downward from (stack + stack_size).
     *
     * @warning Must remain valid for the entire lifetime of the fiber.
     * @note Typically allocated via malloc(), a static array, or a custom allocator.
     */
    uint8_t* stack;

    /**
     * @brief Size of the stack in bytes.
     * @details Determines how much memory is available for the fiber's
     *          function calls and local variables.
     *
     *          Recommended minimum sizes:
     *            - x86_64 / AArch64:  8192 bytes (8 KB)
     *            - ARM Cortex-M:      512-4096 bytes (0.5-4 KB depending on SRAM)
     *
     * @note Stack size must accommodate maximum call depth and local variables.
     * @warning Stack overflow leads to undefined behavior (crashes, corruption).
     */
    size_t stack_size;
} fiber_t;

/**
 * @brief Function signature for fiber entry points.
 * @param user_data Pointer to user-defined data, passed from init_fiber().
 *
 * @details The fiber function receives a single void pointer argument which can
 *          point to any user-defined data structure. The fiber runs until this
 *          function returns, at which point scheduler_return_fiber() is
 *          automatically invoked.
 *
 * @note Fiber functions should not return values directly. Use the user_data
 *       parameter or shared state to communicate results.
 *
 * @warning Do not perform long-running operations without yielding, as this
 *          blocks all other fibers in a cooperative scheduling system.
 */
typedef void (*fiber_fn)(void*);

/**
 * @brief Initializes a fiber with a function and user data.
 * @param fiber     Pointer to fiber structure (stack must already be allocated).
 * @param func      Entry point function for the fiber.
 * @param user_data Pointer passed to the fiber function when it starts.
 *
 * @details Sets up the fiber's initial execution state:
 *            - Configures the stack pointer to the top of the stack.
 *            - Sets up initial register values according to the architecture ABI.
 *            - Arranges for fiber_prologue to be called on first context switch.
 *            - Stores function pointer and user data in callee-saved registers.
 *
 *          After initialization, use switch_context() to start executing the fiber.
 *
 * @pre fiber->stack must be allocated and valid.
 * @pre fiber->stack_size must be set to the stack size in bytes.
 * @pre func must not be NULL.
 *
 * @note This function does not start the fiber; it only prepares it.
 * @note The stack must maintain alignment required by the platform ABI.
 *
 * @warning Calling this function on an already-running fiber causes undefined
 *          behavior. Only initialize fibers before first use.
 */
CFIBER_EXPORT void init_fiber(fiber_t* fiber, fiber_fn func, void* user_data) __attribute__((nonnull(1, 2)));

/**
 * @brief Callback invoked when a fiber's entry function returns.
 *
 * @details Called automatically by the fiber epilogue when a fiber's function
 *          returns. The built-in scheduler module (cfiber/scheduler/scheduler.h)
 *          provides the default implementation. If fibers are used without the
 *          built-in scheduler, the user must supply a definition of this symbol.
 *
 *          The implementation must:
 *            1. Mark the current fiber as completed / available for reuse.
 *            2. Select the next fiber (or the caller context) to switch to.
 *            3. Call switch_context() — it must never return normally.
 *
 * @note Runs on the returning fiber's stack.
 * @warning Must not return; there is no valid return address on the stack.
 */
[[noreturn]] CFIBER_EXPORT extern void scheduler_return_fiber(void);

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_FIBER_H */
