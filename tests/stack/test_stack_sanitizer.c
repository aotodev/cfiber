/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  test_stack_sanitizer.c
 * @brief Unit tests for the canary + watermark stack instrumentation.
 *
 * @details
 * The freestanding/non-MMU counterpart to ASan (see cfiber/debug/asan.h). Only
 * built when CFIBER_STACK_SANITIZER=1, where cstack_debug_* are real functions
 * rather than no-op stubs. Tests use a plain local buffer as the fake stack,
 * no OS facilities required, so this mirrors the Cortex-M usage.
 */

#include "cfiber/stack/debug/stack_sanitize.h"
#include "cfiber/stack/stack.h"
#include "test/test.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#define BUF_SIZE 4096

/* The canary is a 64-bit word at mem_base; stack blocks are at least
 * word-aligned, so the fake stacks are too. */
#define STACK_BUF alignas(uint64_t) uint8_t

static cfiber_stack_t make_stack(uint8_t* buf, size_t size) {
    return (cfiber_stack_t){
        .mem_base = buf,
        .usable_base = buf,
        .stack_top = buf + size,
        .total_size = size,
    };
}

static int test_canary_initialised_and_valid(void) {
    STACK_BUF buf[BUF_SIZE];
    cfiber_stack_t s = make_stack(buf, sizeof(buf));

    cfiber_stack_debug_init(&s);
    ASSERT_TRUE(cfiber_stack_debug_check_canary(&s));
    return 0;
}

static int test_watermark_zero_after_init(void) {
    STACK_BUF buf[BUF_SIZE];
    cfiber_stack_t s = make_stack(buf, sizeof(buf));

    cfiber_stack_debug_init(&s);
    /* freshly painted: nothing has been used yet */
    ASSERT_EQ_U64(cfiber_stack_debug_used_bytes(&s), 0);
    return 0;
}

static int test_watermark_detects_usage(void) {
    STACK_BUF buf[BUF_SIZE];
    cfiber_stack_t s = make_stack(buf, sizeof(buf));

    cfiber_stack_debug_init(&s);

    /* Stacks grow down: simulate usage by overwriting the top 256 bytes. The
     * watermark scans upward from just above the canary, so the deepest used
     * offset corresponds to exactly the 256-byte region we scribbled. */
    memset(buf + sizeof(buf) - 256, 0, 256);
    ASSERT_EQ_U64(cfiber_stack_debug_used_bytes(&s), 256);
    return 0;
}

static int test_used_bytes_signals_overflow_on_canary_smash(void) {
    STACK_BUF buf[BUF_SIZE];
    cfiber_stack_t s = make_stack(buf, sizeof(buf));

    cfiber_stack_debug_init(&s);
    *cfiber_stack_debug_canary_addr(&s) = 0; /* smash */

    /* a corrupted canary makes usage meaningless: reported as (size_t)-1 */
    ASSERT_EQ_U64(cfiber_stack_debug_used_bytes(&s), (uint64_t)(size_t)-1);
    return 0;
}

static int test_canary_smash_detected(void) {
    STACK_BUF buf[BUF_SIZE];
    cfiber_stack_t s = make_stack(buf, sizeof(buf));

    cfiber_stack_debug_init(&s);
    ASSERT_TRUE(cfiber_stack_debug_check_canary(&s));

    *cfiber_stack_debug_canary_addr(&s) = 0;
    ASSERT_FALSE(cfiber_stack_debug_check_canary(&s));
    return 0;
}

/* overflowed(): intact canary and headroom is fine; a smashed canary or a
 * watermark used down to the canary is an overflow. */
static int test_overflowed_predicate(void) {
    STACK_BUF buf[BUF_SIZE];
    cfiber_stack_t s = make_stack(buf, sizeof(buf));

    cfiber_stack_debug_init(&s);
    ASSERT_FALSE(cfiber_stack_debug_overflowed(&s));

    memset(buf + sizeof(buf) - 256, 0, 256);
    ASSERT_FALSE(cfiber_stack_debug_overflowed(&s));

    /* every watermark byte touched, canary still intact */
    memset(cfiber_stack_debug_watermark_begin(&s), 0, cfiber_stack_debug_watermark_size(&s));
    ASSERT_TRUE(cfiber_stack_debug_check_canary(&s));
    ASSERT_TRUE(cfiber_stack_debug_overflowed(&s));

    cfiber_stack_debug_init(&s);
    *cfiber_stack_debug_canary_addr(&s) = 0;
    ASSERT_TRUE(cfiber_stack_debug_overflowed(&s));
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("stack sanitizer (canary / watermark)");

    RUN_TEST(test_canary_initialised_and_valid);
    RUN_TEST(test_watermark_zero_after_init);
    RUN_TEST(test_watermark_detects_usage);
    RUN_TEST(test_used_bytes_signals_overflow_on_canary_smash);
    RUN_TEST(test_canary_smash_detected);
    RUN_TEST(test_overflowed_predicate);

    return cfiber_test_report();
}
