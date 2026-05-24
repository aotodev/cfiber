/**
 * @file    growable_stack.h
 *
 * @brief   Low-level MMU-based growable stack operations.
 * @details These functions work with individual stacks using mmap and SIGSEGV handling.
 *          For high-level pooled allocation, use cfiber/stack/growable_stack_allocator.h
 *          instead.
 * @warning Growable stacks are implemented using guard pages + SIGSEGV; supported on
 *          Linux and other Unix-like systems where mprotect-in-handler is known to work.
 *          Not POSIX-portable in the strict async-signal-safe sense.
 * @note    It is recommended to set fstack-clash-protection or -fstack-check flags to
 *          avoid missing pages with big stack allocations.
 */


#ifndef CFIBER_GROWABLE_STACK_H
#define CFIBER_GROWABLE_STACK_H

#include "cfiber/core/macros.h"
#include "cfiber/stack/stack.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new growable stack.
 * @param max_size Maximum size for the stack (must be page-aligned)
 * @return A cstack_t descriptor, or an invalid stack on failure
 */
[[nodiscard]] CFIBER_EXPORT cstack_t cstack_growable_create(size_t max_size);

/**
 * @brief Destroy a growable stack and release all memory.
 * @param stack The stack to destroy
 */
CFIBER_EXPORT void cstack_growable_destroy(cstack_t* stack);

/**
 * @brief Recycle a growable stack by releasing grown pages.
 * @details Keeps the VMA but releases physical pages above the top page.
 *          Useful for reusing stacks without full deallocation.
 * @param stack The stack to recycle
 */
CFIBER_EXPORT void cstack_growable_recycle(cstack_t* stack);

#ifdef __cplusplus
}
#endif


#endif // CFIBER_GROWABLE_STACK_H
