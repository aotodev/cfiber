/**
 * @file  fixed_size_stack_allocator.h
 * @brief Thin wrapper that pairs a multislab allocator with cstack_t descriptors.
 * @details Allocates fixed-size stack memory from a multislab and fills in a
 *          cstack_t descriptor.  Optionally instruments stacks with canary /
 *          watermark checks when the stack sanitizer is enabled.
 */

#ifndef CFIBER_FIXED_SIZE_STACK_ALLOCATOR_H
#define CFIBER_FIXED_SIZE_STACK_ALLOCATOR_H

#include "cfiber/memory/multislab_alloc.h"
#include "cfiber/stack/stack.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate a fixed-size stack from a multislab.
 * @param stack Output descriptor filled on success.
 * @param ms    The multislab to allocate from.
 * @return 0 on success, -1 if the multislab is exhausted.
 */
int ms_stack_alloc(cstack_t* stack, multislab_t* ms);

/**
 * @brief Release a fixed-size stack back to a multislab.
 * @param stack The stack descriptor to release.
 * @param ms    The multislab the stack was allocated from.
 */
void ms_stack_release(cstack_t* stack, multislab_t* ms);

#ifdef __cplusplus
}
#endif

#endif // CFIBER_FIXED_SIZE_STACK_ALLOCATOR_H
