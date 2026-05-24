#include "context_switch_unit_tests.h"
#include "test/test.h"

const size_t UNIT_TEST_STACK_SIZE = 1U << 10;

/* R7 can be used as FP in thumb mode, so we don't test it */
constexpr uint32_t R4_VALUE = 0xAAAAAAAA;
constexpr uint32_t R5_VALUE = 0xCCCCCCCC;
constexpr uint32_t R6_VALUE = 0xDDDDDDDD;
constexpr uint32_t R8_VALUE = 0xEEEEEEEE;
constexpr uint32_t R9_VALUE = 0xFFFFFFFF;
constexpr uint32_t R10_VALUE = 0x11111111;
constexpr uint32_t R11_VALUE = 0x22222222;

#if defined(CFIBER_ARM_FPU)
/* magic values for floating-point callee-saved registers */
constexpr float S16_VALUE = 0.111f;
constexpr float S17_VALUE = 0.222f;
constexpr float S18_VALUE = 0.333f;
constexpr float S19_VALUE = 0.444f;
constexpr float S20_VALUE = 0.555f;
constexpr float S21_VALUE = 0.666f;
constexpr float S22_VALUE = 0.777f;
constexpr float S23_VALUE = 0.999f;
constexpr float S24_VALUE = 0.121f;
constexpr float S25_VALUE = 0.131f;
constexpr float S26_VALUE = 0.141f;
constexpr float S27_VALUE = 0.151f;
constexpr float S28_VALUE = 0.161f;
constexpr float S29_VALUE = 0.171f;
constexpr float S30_VALUE = 0.181f;
constexpr float S31_VALUE = 0.191f;

constexpr float PRECISION = 0.001f;
#endif

/* Function for the first context */
void test_register_preservation_function(void* userData) {
    struct user_data* data = (struct user_data*)userData;

    TEST_EQUAL_U32("correct ctx switch from main", data->state, TEST_STATE_INIT);
    data->state = TEST_STATE_TEST_FIBER_REGISTERS_SET;

    /* Set callee-saved registers with magic values.
     * Split into small chunks to reduce register pressure on Cortex-M
     * These must be preserved by the context switch.
     */

    // clang-format off
    __asm__ volatile(
        "mov r4, %0\n"
        "mov r5, %1\n"
        "mov r6, %2\n"
        "mov r8, %3\n"
        :
        : "r"(R4_VALUE), "r"(R5_VALUE), "r"(R6_VALUE), "r"(R8_VALUE)
        : "r4", "r5", "r6", "r8"
    );

    __asm__ volatile(
        "mov r9, %0\n"
        "mov r10, %1\n"
        "mov r11, %2\n"
        :
        : "r"(R9_VALUE), "r"(R10_VALUE), "r"(R11_VALUE)
        : "r9", "r10", "r11"
    );
    // clang-format on

#if defined(CFIBER_ARM_FPU)
    // clang-format off
    __asm__ volatile(
        "vmov s16, %0\n"
        "vmov s17, %1\n"
        "vmov s18, %2\n"
        "vmov s19, %3\n"
        "vmov s20, %4\n"
        "vmov s21, %5\n"
        "vmov s22, %6\n"
        "vmov s23, %7\n"
        "vmov s24, %8\n"
        "vmov s25, %9\n"
        "vmov s26, %10\n"
        "vmov s27, %11\n"
        "vmov s28, %12\n"
        "vmov s29, %13\n"
        "vmov s30, %14\n"
        "vmov s31, %15\n"
        :
        : "w"(S16_VALUE), "w"(S17_VALUE), "w"(S18_VALUE), "w"(S19_VALUE),
          "w"(S20_VALUE), "w"(S21_VALUE), "w"(S22_VALUE), "w"(S23_VALUE),
          "w"(S24_VALUE), "w"(S25_VALUE), "w"(S26_VALUE), "w"(S27_VALUE),
          "w"(S28_VALUE), "w"(S29_VALUE), "w"(S30_VALUE), "w"(S31_VALUE)
        : "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
          "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31"
    );
    // clang-format on
#endif

    /* Switch away to the intermediate function and then wait for it to switch back. */
    switch_context(data->ctxs.test_fiber_context, data->ctxs.intermediary_fiber_context);

    /* We are back from the intermediate function. Read the registers back into variables. */

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    volatile uint32_t r4_val, r5_val, r6_val, r8_val, r9_val, r10_val, r11_val;

#if defined(CFIBER_ARM_FPU)
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    volatile float s16_val, s17_val, s18_val, s19_val, s20_val, s21_val, s22_val, s23_val, s24_val, s25_val, s26_val,
        s27_val, s28_val, s29_val, s30_val, s31_val;
#endif
    /* Read back GPRs in smaller groups to reduce output register pressure */

    // clang-format off
    __asm__ volatile(
        "mov %0, r4\n"
        "mov %1, r5\n"
        "mov %2, r6\n"
        "mov %3, r8\n"
        : "=r"(r4_val), "=r"(r5_val), "=r"(r6_val), "=r"(r8_val)
        :
        :
    );

    __asm__ volatile(
        "mov %0, r9\n"
        "mov %1, r10\n"
        "mov %2, r11\n"
        : "=r"(r9_val), "=r"(r10_val), "=r"(r11_val)
        :
        :
    );
    // clang-format on

#if defined(CFIBER_ARM_FPU)
    // clang-format off
    __asm__ volatile(
        "vmov %0,  s16\n"
        "vmov %1,  s17\n"
        "vmov %2,  s18\n"
        "vmov %3,  s19\n"
        "vmov %4,  s20\n"
        "vmov %5,  s21\n"
        "vmov %6,  s22\n"
        "vmov %7,  s23\n"
        "vmov %8,  s24\n"
        "vmov %9,  s25\n"
        "vmov %10, s26\n"
        "vmov %11, s27\n"
        "vmov %12, s28\n"
        "vmov %13, s29\n"
        "vmov %14, s30\n"
        "vmov %15, s31\n"
        : "=w"(s16_val), "=w"(s17_val), "=w"(s18_val), "=w"(s19_val),
          "=w"(s20_val), "=w"(s21_val), "=w"(s22_val), "=w"(s23_val),
          "=w"(s24_val), "=w"(s25_val), "=w"(s26_val), "=w"(s27_val),
          "=w"(s28_val), "=w"(s29_val), "=w"(s30_val), "=w"(s31_val)
        :
        :
    );
    // clang-format on
#endif

    /* Test that the restored values match the original magic values. */
    TEST_EQUAL_U32("r4 register", r4_val, R4_VALUE);
    TEST_EQUAL_U32("r5 register", r5_val, R5_VALUE);
    TEST_EQUAL_U32("r6 register", r6_val, R6_VALUE);
    TEST_EQUAL_U32("r8 register", r8_val, R8_VALUE);
    TEST_EQUAL_U32("r9 register", r9_val, R9_VALUE);
    TEST_EQUAL_U32("r10 register", r10_val, R10_VALUE);
    TEST_EQUAL_U32("r11 register", r11_val, R11_VALUE);

#if defined(CFIBER_ARM_FPU)
    TEST_NEARLY_EQUAL("s16 register", s16_val, S16_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s17 register", s17_val, S17_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s18 register", s18_val, S18_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s19 register", s19_val, S19_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s20 register", s20_val, S20_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s21 register", s21_val, S21_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s22 register", s22_val, S22_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s23 register", s23_val, S23_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s24 register", s24_val, S24_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s25 register", s25_val, S25_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s26 register", s26_val, S26_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s27 register", s27_val, S27_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s28 register", s28_val, S28_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s29 register", s29_val, S29_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s30 register", s30_val, S30_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("s31 register", s31_val, S31_VALUE, PRECISION);
#endif

    /* Tests state to assert correct execution order. */
    TEST_EQUAL_U32("correct ctx switch from intermediate", data->state, TEST_STATE_INTERMEDIARY_FIBER);
    data->state = TEST_STATE_TEST_FIBER_REGISTERS_LOAD;

    /* End the test by switching back to the main thread. */
    switch_context(data->ctxs.test_fiber_context, data->ctxs.main_context);
}
