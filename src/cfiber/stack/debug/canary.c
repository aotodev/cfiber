#include "cfiber/stack/debug/stack_sanitize.h"

#if CFIBER_STACK_SANITIZER

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Intentionally constant: helps detect clobbers easily in dumps. */
static constexpr uint64_t CFIBER_CANARY_VALUE = UINT64_C(0xC5C4C3C2C1C0B0A0);

/* memcpy: mem_base is only guaranteed word-aligned, and an unaligned 64-bit
 * access faults on ARMv6-M. */
static uint64_t canary_read(const cfiber_stack_t* s) {
    uint64_t v;
    memcpy(&v, cfiber_stack_debug_canary_addr(s), sizeof v);
    return v;
}

void cfiber_stack_debug_init(const cfiber_stack_t* s) {
    /* Writes canary and paints watermark region. */
    if (!s || !s->mem_base || !s->stack_top) {
        return;
    }

    memcpy(cfiber_stack_debug_canary_addr(s), &CFIBER_CANARY_VALUE, sizeof CFIBER_CANARY_VALUE);

    /* paint watermark area */
    uint8_t* p = cfiber_stack_debug_watermark_begin(s);
    size_t n = cfiber_stack_debug_watermark_size(s);

    /* pattern chosen to be uncommon on stack (0xA5 similar to ASan) */
    for (size_t i = 0; i < n; ++i) {
        p[i] = 0xA5;
    }
}

int cfiber_stack_debug_check_canary(const cfiber_stack_t* s) {
    if (!s || !s->mem_base || !s->stack_top) {
        return 0;
    }

    return canary_read(s) == CFIBER_CANARY_VALUE;
}

int cfiber_stack_debug_overflowed(const cfiber_stack_t* s) {
    const size_t used = cfiber_stack_debug_used_bytes(s);
    return used == (size_t)-1 || used == cfiber_stack_debug_watermark_size(s);
}

#endif /* CFIBER_STACK_SANITIZER */
