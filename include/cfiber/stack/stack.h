/**
 * @file  stack.h
 * @brief Descriptor for a stack.
 */

#ifndef CFIBER_STACK_H
#define CFIBER_STACK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /**
     * @brief The bottom of the allocation.
     */
    void* mem_base;

    /**
     * @brief The user's initial SP (high address).
     */
    void* stack_top;

    /**
     * @brief Total memory allocation size.
     */
    size_t total_size;
} cstack_t;

/** @brief Basic sanity check for a stack descriptor. */
static inline int is_valid_cstack(const cstack_t* const stack) {
    return stack && stack->mem_base && stack->stack_top && stack->total_size;
}

#ifdef __cplusplus
}
#endif

#endif // CFIBER_STACK_H
