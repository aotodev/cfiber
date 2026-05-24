#include "cfiber/stack/debug/stack_sanitize.h"

#if CFIBER_STACK_SANITIZER

#include <stddef.h>
#include <stdint.h>

size_t cstack_debug_stack_used_bytes(const cstack_t* s) {
    if (!s || !s->mem_base || !s->stack_top) {
        return 0u;
    }

    /* If canary is already corrupted, usage is meaningless (overflow). */
    if (!cstack_debug_stack_check_canary(s)) {
        return (size_t)-1;
    }

    uint8_t* begin = cstack_debug_watermark_begin(s);
    size_t n = cstack_debug_watermark_size(s);

    /* Scan from the bottom upward: first byte != 0xA5 marks deepest usage. */
    size_t i = 0;
    while (i < n && begin[i] == 0xA5) {
        i++;
    }

    /* painted bytes = i, so used ~= n - i */
    return n - i;
}

#endif /* CFIBER_STACK_SANITIZER */
