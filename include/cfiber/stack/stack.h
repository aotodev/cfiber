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

/**
 * @brief A stack block and the part of it a fiber may use.
 * @details [mem_base, stack_top) is the whole allocation; [usable_base,
 *          stack_top) is the fiber's stack. What lies below usable_base is the
 *          allocator's: the PROT_NONE guard page of a growable stack, the ASan
 *          redzone of a fixed-size one. Give init_fiber() and sanitizer bounds
 *          the usable range, never mem_base.
 */
typedef struct {
    /** Lowest address of the allocation. */
    void* mem_base;
    /** Lowest address a fiber may touch. */
    void* usable_base;
    /** The user's initial SP (high address, exclusive). */
    void* stack_top;
    /** Size of the whole allocation, stack_top - mem_base. */
    size_t total_size;
} cstack_t;

/** @brief Basic sanity check for a stack descriptor. */
static inline int is_valid_cstack(const cstack_t* const stack) {
    return stack && stack->mem_base && stack->usable_base && stack->stack_top && stack->total_size;
}

/** @brief Bytes a fiber may use: stack_top - usable_base. */
static inline size_t cstack_usable_size(const cstack_t* const stack) {
    return (size_t)((const char*)stack->stack_top - (const char*)stack->usable_base);
}

#ifdef __cplusplus
}
#endif

#endif // CFIBER_STACK_H
