#include "cfiber/stack/debug/stack_sanitize.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Fake fixed stack backed by a local buffer. */
    uint8_t buf[4096];

    cstack_t s = {
        .mem_base = buf,
        .stack_top = buf + sizeof(buf),
        .total_size = sizeof(buf),
    };

    cstack_debug_stack_init(&s);
    if (!cstack_debug_stack_check_canary(&s)) {
        fputs("canary check failed after init\n", stderr);
        return 1;
    }

    /* Simulate stack usage by scribbling near the top (stacks grow down). */
    memset(buf + sizeof(buf) - 256, 0, 256);

    if (cstack_debug_stack_used_bytes(&s) == 0) {
        fputs("watermark did not detect any usage\n", stderr);
        return 1;
    }

    /* Smash canary */
    *cstack_debug_canary_addr(&s) = 0;
    if (cstack_debug_stack_check_canary(&s)) {
        fputs("canary check did not detect overflow\n", stderr);
        return 1;
    }

    return 0;
}
