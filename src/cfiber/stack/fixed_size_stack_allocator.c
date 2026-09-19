#include "cfiber/stack/fixed_size_stack_allocator.h"

#include "cfiber/core/internal.h"
#include "cfiber/debug/asan.h"
#include "cfiber/stack/debug/stack_sanitize.h"

#if CFIBER_STACK_SANITIZER && CFIBER_ASAN_ENABLED
#error "CFIBER_STACK_SANITIZER and AddressSanitizer are mutually exclusive: the canary lands in the ASan redzone"
#endif

int cfiber_fixed_stack_alloc(cfiber_stack_t* stack, cfiber_multislab_t* ms) {
    void* mem = cfiber_multislab_alloc(ms);
    if (UNLIKELY(!mem)) {
        return -1;
    }

    stack->mem_base = mem;
    stack->usable_base = (char*)mem + CFIBER_ASAN_REDZONE;
    stack->stack_top = (char*)mem + ms->block_size;
    stack->total_size = ms->block_size;

    cfiber_stack_debug_init(stack);

    /* Under ASan, poison a guard at the bottom of the block. The usable stack
     * then starts at mem_base + CFIBER_ASAN_REDZONE; a downward overflow past
     * it trips ASan. No-op when ASan is disabled. */
    cfiber_asan_poison(stack->mem_base, CFIBER_ASAN_REDZONE);

    return 0;
}

bool cfiber_fixed_stack_release(cfiber_stack_t* stack, cfiber_multislab_t* ms) {
    /* Checked in every build: the sanitizer is opt-in, so its cost is accepted. */
    const bool ok = !cfiber_stack_debug_overflowed(stack);
    cfiber_asan_unpoison(stack->mem_base, CFIBER_ASAN_REDZONE);
    cfiber_multislab_release(ms, stack->mem_base);
    return ok;
}
