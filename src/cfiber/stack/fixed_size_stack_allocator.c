#include "cfiber/stack/fixed_size_stack_allocator.h"

#if CFIBER_STACK_SANITIZER
#include "cfiber/stack/debug/stack_sanitize.h"
#endif

int ms_stack_alloc(cstack_t* stack, multislab_t* ms) {
    void* mem = multislab_alloc(ms);
    if (UNLIKELY(!mem)) {
        return -1;
    }

    stack->mem_base = mem;
    stack->stack_top = (char*)mem + ms->block_size;
    stack->total_size = ms->block_size;

#if CFIBER_STACK_SANITIZER
    cstack_debug_stack_init(stack);
#endif

    return 0;
}

void ms_stack_release(cstack_t* stack, multislab_t* ms) {
#if CFIBER_STACK_SANITIZER
    ASSERT(cstack_debug_stack_check_canary(stack));
#endif
    multislab_release(ms, stack->mem_base);
}
