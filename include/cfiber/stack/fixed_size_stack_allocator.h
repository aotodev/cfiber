/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  fixed_size_stack_allocator.h
 * @brief Thin wrapper that pairs a multislab allocator with cfiber_stack_t descriptors.
 * @details Allocates fixed-size stack memory from a multislab and fills in a
 *          cfiber_stack_t descriptor.  Optionally instruments stacks with canary /
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
 * @details With the stack sanitizer, plants the canary and paints the
 *          watermark. Under ASan, poisons CFIBER_ASAN_REDZONE bytes at
 *          mem_base; the usable stack starts above them.
 */
[[nodiscard]] CFIBER_EXPORT int cfiber_fixed_stack_alloc(cfiber_stack_t* stack, cfiber_multislab_t* ms)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Release a fixed-size stack back to a multislab.
 * @param stack The stack descriptor to release.
 * @param ms    The multislab the stack was allocated from.
 * @return false if the stack sanitizer found the stack overflowed (canary gone
 *         or watermark exhausted), in every build type; the block is released
 *         regardless. Always true without the sanitizer.
 */
CFIBER_EXPORT bool cfiber_fixed_stack_release(cfiber_stack_t* stack, cfiber_multislab_t* ms)
    __attribute__((nonnull(1, 2)));

#ifdef __cplusplus
}
#endif

#endif // CFIBER_FIXED_SIZE_STACK_ALLOCATOR_H
