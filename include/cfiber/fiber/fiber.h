/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

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
 *   2. Create a cfiber_t structure and set stack/stack_size.
 *   3. Call cfiber_init() with your fiber function and user data.
 *   4. Use cfiber_switch_context() to switch between fibers.
 *   5. Register a fiber-return hook with cfiber_set_return_hook() to handle
 *      fiber completion.
 *
 * @section example Example
 * @code
 * static cfiber_context_t main_ctx;
 * static cfiber_t fiber;
 * static uint8_t stack[8192];
 *
 * static void on_return(void* ctx) {
 *     (void)ctx;
 *     cfiber_switch_context(&fiber.ctx, &main_ctx); // never resumed
 * }
 *
 * static void my_fiber_func(void* data) {
 *     printf("running with %p\n", data);
 *     cfiber_switch_context(&fiber.ctx, &main_ctx); // yield to main
 *     printf("resumed\n");
 * }
 *
 * fiber.stack = stack;
 * fiber.stack_size = sizeof stack;
 * cfiber_init(&fiber, my_fiber_func, my_data);
 * cfiber_set_return_hook(on_return, nullptr);
 *
 * cfiber_switch_context(&main_ctx, &fiber.ctx); // runs until the yield
 * cfiber_switch_context(&main_ctx, &fiber.ctx); // resumes; comes back via on_return
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
 *          cfiber_init().
 */
typedef struct {
    /**
     * @brief CPU context (registers, stack pointer, etc).
     * @details Stores the fiber's execution state when it is not running.
     *          Managed automatically by cfiber_switch_context() and cfiber_init().
     */
    cfiber_context_t ctx;

    /**
     * @brief Pointer to the fiber's stack memory.
     * @details Must be allocated by the user before calling cfiber_init().
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
} cfiber_t;

/**
 * @brief Function signature for fiber entry points.
 * @param user_data Pointer to user-defined data, passed from cfiber_init().
 *
 * @details The fiber function receives a single void pointer argument which can
 *          point to any user-defined data structure. The fiber runs until this
 *          function returns, at which point the registered fiber-return hook
 *          (see cfiber_set_return_hook()) is automatically invoked.
 *
 * @note Fiber functions should not return values directly. Use the user_data
 *       parameter or shared state to communicate results.
 *
 * @warning Do not perform long-running operations without yielding, as this
 *          blocks all other fibers in a cooperative scheduling system.
 */
typedef void (*cfiber_fn)(void*);

/**
 * @brief Initializes a fiber with a function and user data.
 * @param fiber     Pointer to fiber structure (stack must already be allocated).
 * @param func      Entry point function for the fiber.
 * @param user_data Pointer passed to the fiber function when it starts.
 *
 * @details Sets up the fiber's initial execution state:
 *            - Zeroes the context, then:
 *            - Configures the stack pointer to the top of the stack.
 *            - Sets up initial register values according to the architecture ABI.
 *            - Arranges for cfiber_prologue to be called on first context switch.
 *            - Stores function pointer and user data in callee-saved registers.
 *            - Copies the caller's floating-point control state.
 *
 *          After initialization, use cfiber_switch_context() to start executing the fiber.
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
CFIBER_EXPORT void cfiber_init(cfiber_t* fiber, cfiber_fn func, void* user_data) __attribute__((nonnull(1, 2)));

/**
 * @brief Signature of the fiber-return hook.
 * @param ctx Opaque pointer registered alongside the hook (typically the
 *            scheduler that owns the returning fiber).
 *
 * @details Invoked by the fiber epilogue, on the returning fiber's stack, when a
 *          fiber's entry function returns. The implementation must:
 *            1. Mark the current fiber as completed / available for reuse.
 *            2. Select the next fiber (or the caller context) to switch to.
 *            3. Call cfiber_switch_context(). It must never return normally.
 *
 * @warning Must not return; there is no valid return address on the stack.
 */
typedef void (*cfiber_return_hook_fn)(void* ctx);

/**
 * @brief A registered fiber-return hook (function + its opaque context).
 */
typedef struct {
    cfiber_return_hook_fn fn;
    void* ctx;
} cfiber_return_hook_t;

/**
 * @brief Registers the calling thread's fiber-return hook.
 * @param fn  Hook invoked when a fiber's entry function returns (must not
 *            return). Pass NULL to clear.
 * @param ctx Opaque pointer passed to @p fn on each invocation.
 * @return The hook that was previously registered, so the caller can restore it
 *         on exit (supporting nesting and multiple coexisting schedulers).
 *
 * @details The hook is dispatched at run time rather than resolved as a link
 *          time symbol, so it works through a shared library and lets any number
 *          of schedulers share a process: each registers its own hook while it
 *          runs and restores the previous one when it returns.
 *
 *          A scheduler-free user driving fibers directly with cfiber_init() and
 *          cfiber_switch_context() registers a hook here instead of defining a symbol.
 *
 * @note The registration is thread-local on hosted targets and a plain global on
 *       bare-metal ARM Cortex-M (which has no TLS), mirroring the built-in
 *       scheduler's current-scheduler storage.
 */
CFIBER_EXPORT cfiber_return_hook_t cfiber_set_return_hook(cfiber_return_hook_fn fn, void* ctx);

/**
 * @brief Returns the calling thread's currently registered fiber-return hook.
 */
CFIBER_EXPORT cfiber_return_hook_t cfiber_get_return_hook(void);

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_FIBER_H */
