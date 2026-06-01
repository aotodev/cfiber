#include "cfiber/stack/fixed_size_stack_allocator.h"

#include "cfiber/debug/asan.h"

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

    /* Under ASan, poison a guard at the bottom of the block. The usable stack
     * then starts at mem_base + CFIBER_ASAN_REDZONE; a downward overflow past
     * it trips ASan. No-op when ASan is disabled. Mutually exclusive with the
     * canary sanitizer (whose word would otherwise land inside this guard). */
    cfiber_asan_poison(stack->mem_base, CFIBER_ASAN_REDZONE);

    return 0;
}

void ms_stack_release(cstack_t* stack, multislab_t* ms) {
#if CFIBER_STACK_SANITIZER
    ASSERT(cstack_debug_stack_check_canary(stack));
#endif
    cfiber_asan_unpoison(stack->mem_base, CFIBER_ASAN_REDZONE);
    multislab_release(ms, stack->mem_base);
}
