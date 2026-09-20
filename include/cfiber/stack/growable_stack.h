/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file    growable_stack.h
 *
 * @brief   Low-level MMU-based growable stack operations.
 * @details These functions work with individual stacks. The mapping covers the whole
 *          requested extent with MAP_NORESERVE, so growth is demand paging rather than
 *          a fault handler; the PROT_NONE page below is an overflow trap, not part of
 *          the growth path. For high-level pooled allocation, use
 *          cfiber/stack/growable_stack_allocator.h instead.
 * @warning Requires an MMU and the Linux mmap flags used here (MAP_NORESERVE,
 *          MAP_STACK). Not available on freestanding targets.
 * @note    It is recommended to set fstack-clash-protection or -fstack-check flags, as a
 *          frame larger than a page can step over the guard page without faulting.
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
 * @param max_size Usable size of the stack: non-zero and page-aligned.
 * @return A cfiber_stack_t descriptor, or a zeroed one on failure with errno set:
 *         EINVAL for a bad size, otherwise the mmap/mprotect error. The guard
 *         page lies below usable_base, inside the allocation.
 */
[[nodiscard]] CFIBER_EXPORT cfiber_stack_t cfiber_growable_stack_create(size_t max_size);

/**
 * @brief Destroy a growable stack and release all memory.
 * @param stack The stack to destroy
 */
CFIBER_EXPORT void cfiber_growable_stack_destroy(cfiber_stack_t* stack);

/**
 * @brief Recycle a growable stack by releasing grown pages.
 * @details Keeps the VMA but releases physical pages above the top page.
 *          Useful for reusing stacks without full deallocation.
 * @param stack The stack to recycle
 */
CFIBER_EXPORT void cfiber_growable_stack_recycle(cfiber_stack_t* stack);

#ifdef __cplusplus
}
#endif


#endif // CFIBER_GROWABLE_STACK_H
