/**
 * @file  test_stack_sanitizer.c
 * @brief Unit tests for the canary + watermark stack instrumentation.
 *
 * @details
 * The freestanding/non-MMU counterpart to ASan (see cfiber/debug/asan.h). Only
 * built when CFIBER_STACK_SANITIZER=1, where cstack_debug_* are real functions
 * rather than no-op stubs. Tests use a plain local buffer as the fake stack —
 * no OS facilities required, so this mirrors the Cortex-M usage.
 */

#include "cfiber/stack/debug/stack_sanitize.h"
#include "cfiber/stack/stack.h"
#include "test/test.h"

#include <stdint.h>
#include <string.h>

#define BUF_SIZE 4096

static cstack_t make_stack(uint8_t* buf, size_t size) {
    return (cstack_t){
        .mem_base = buf,
        .stack_top = buf + size,
        .total_size = size,
    };
}

static int test_canary_initialised_and_valid(void) {
    uint8_t buf[BUF_SIZE];
    cstack_t s = make_stack(buf, sizeof(buf));

    cstack_debug_stack_init(&s);
    ASSERT_TRUE(cstack_debug_stack_check_canary(&s));
    return 0;
}

static int test_watermark_zero_after_init(void) {
    uint8_t buf[BUF_SIZE];
    cstack_t s = make_stack(buf, sizeof(buf));

    cstack_debug_stack_init(&s);
    /* freshly painted: nothing has been used yet */
    ASSERT_EQ_U64(cstack_debug_stack_used_bytes(&s), 0);
    return 0;
}

static int test_watermark_detects_usage(void) {
    uint8_t buf[BUF_SIZE];
    cstack_t s = make_stack(buf, sizeof(buf));

    cstack_debug_stack_init(&s);

    /* Stacks grow down: simulate usage by overwriting the top 256 bytes. The
     * watermark scans upward from just above the canary, so the deepest used
     * offset corresponds to exactly the 256-byte region we scribbled. */
    memset(buf + sizeof(buf) - 256, 0, 256);
    ASSERT_EQ_U64(cstack_debug_stack_used_bytes(&s), 256);
    return 0;
}

static int test_used_bytes_signals_overflow_on_canary_smash(void) {
    uint8_t buf[BUF_SIZE];
    cstack_t s = make_stack(buf, sizeof(buf));

    cstack_debug_stack_init(&s);
    *cstack_debug_canary_addr(&s) = 0; /* smash */

    /* a corrupted canary makes usage meaningless: reported as (size_t)-1 */
    ASSERT_EQ_U64(cstack_debug_stack_used_bytes(&s), (uint64_t)(size_t)-1);
    return 0;
}

static int test_canary_smash_detected(void) {
    uint8_t buf[BUF_SIZE];
    cstack_t s = make_stack(buf, sizeof(buf));

    cstack_debug_stack_init(&s);
    ASSERT_TRUE(cstack_debug_stack_check_canary(&s));

    *cstack_debug_canary_addr(&s) = 0;
    ASSERT_FALSE(cstack_debug_stack_check_canary(&s));
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("stack sanitizer (canary / watermark)");

    RUN_TEST(test_canary_initialised_and_valid);
    RUN_TEST(test_watermark_zero_after_init);
    RUN_TEST(test_watermark_detects_usage);
    RUN_TEST(test_used_bytes_signals_overflow_on_canary_smash);
    RUN_TEST(test_canary_smash_detected);

    return cfiber_test_report();
}
