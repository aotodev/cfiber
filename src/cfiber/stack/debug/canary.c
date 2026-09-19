#include "cfiber/stack/debug/stack_sanitize.h"

#if CFIBER_STACK_SANITIZER

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Intentionally constant: helps detect clobbers easily in dumps. */
static constexpr uint64_t CFIBER_CANARY_VALUE = UINT64_C(0xC5C4C3C2C1C0B0A0);

/* memcpy: mem_base is only guaranteed word-aligned, and an unaligned 64-bit
 * access faults on ARMv6-M. */
static uint64_t canary_read(const cstack_t* s) {
    uint64_t v;
    memcpy(&v, cstack_debug_canary_addr(s), sizeof v);
    return v;
}

void cstack_debug_stack_init(const cstack_t* s) {
    /* Writes canary and paints watermark region. */
    if (!s || !s->mem_base || !s->stack_top) {
        return;
    }

    memcpy(cstack_debug_canary_addr(s), &CFIBER_CANARY_VALUE, sizeof CFIBER_CANARY_VALUE);

    /* paint watermark area */
    uint8_t* p = cstack_debug_watermark_begin(s);
    size_t n = cstack_debug_watermark_size(s);

    /* pattern chosen to be uncommon on stack (0xA5 similar to ASan) */
    for (size_t i = 0; i < n; ++i) {
        p[i] = 0xA5;
    }
}

int cstack_debug_stack_check_canary(const cstack_t* s) {
    if (!s || !s->mem_base || !s->stack_top) {
        return 0;
    }

    return canary_read(s) == CFIBER_CANARY_VALUE;
}

int cstack_debug_stack_overflowed(const cstack_t* s) {
    const size_t used = cstack_debug_stack_used_bytes(s);
    return used == (size_t)-1 || used == cstack_debug_watermark_size(s);
}

#endif /* CFIBER_STACK_SANITIZER */
