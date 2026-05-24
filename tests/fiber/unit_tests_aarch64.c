#include "context_switch_unit_tests.h"
#include "test/test.h"

/* magic values for general-purpose callee-saved registers */
constexpr uint64_t X19_VALUE = 0xAAAAAAAAAAAAAAAA;
constexpr uint64_t X20_VALUE = 0xBBBBBBBBBBBBBBBB;
constexpr uint64_t X21_VALUE = 0xCCCCCCCCCCCCCCCC;
constexpr uint64_t X22_VALUE = 0xDDDDDDDDDDDDDDDD;
constexpr uint64_t X23_VALUE = 0xEEEEEEEEEEEEEEEE;
constexpr uint64_t X24_VALUE = 0xFFFFFFFFFFFFFFFF;
constexpr uint64_t X25_VALUE = 0x1111111111111111;
constexpr uint64_t X26_VALUE = 0x2222222222222222;
constexpr uint64_t X27_VALUE = 0x3333333333333333;
constexpr uint64_t X28_VALUE = 0x4444444444444444;

/* magic values for floating-point callee-saved registers */
constexpr double V8_VALUE = 0x0.AAAAAAAAAAAAP+8;
constexpr double V9_VALUE = 0x1.BBBBBBBBBBBBP+8;
constexpr double V10_VALUE = 0x2.CCCCCCCCCCCCP+8;
constexpr double V11_VALUE = 0x3.DDDDDDDDDDDDP+8;
constexpr double V12_VALUE = 0x4.EEEEEEEEEEEEP+8;
constexpr double V13_VALUE = 0x5.FFFFFFFFFFFFP+8;
constexpr double V14_VALUE = 0x6.111111111111P+8;
constexpr double V15_VALUE = 0x7.222222222222P+8;


const size_t UNIT_TEST_STACK_SIZE = 8096;
constexpr double PRECISION = 0.001;

/* Function for the first context */
void test_register_preservation_function(void* userData) {
    struct user_data* data = (struct user_data*)userData;

    TEST_EQUAL_U32("correct ctx switch from main", data->state, TEST_STATE_INIT);
    data->state = TEST_STATE_TEST_FIBER_REGISTERS_SET;

    /* STEP 1 & 2: Set callee-saved registers with our magic values.
     * These must be preserved by the context switch
     */
    __asm__ volatile( //

        // general-purpose
        "mov x19, %0\n"
        "mov x20, %1\n"
        "mov x21, %2\n"
        "mov x22, %3\n"
        "mov x23, %4\n"
        "mov x24, %5\n"
        "mov x25, %6\n"
        "mov x26, %7\n"
        "mov x27, %8\n"
        "mov x28, %9\n"

        // floating-point - load from memory
        "ldr d8, %10\n"
        "ldr d9, %11\n"
        "ldr d10, %12\n"
        "ldr d11, %13\n"
        "ldr d12, %14\n"
        "ldr d13, %15\n"
        "ldr d14, %16\n"
        "ldr d15, %17\n"
        :
        : "r"(X19_VALUE),
          "r"(X20_VALUE),
          "r"(X21_VALUE),
          "r"(X22_VALUE),
          "r"(X23_VALUE),
          "r"(X24_VALUE),
          "r"(X25_VALUE),
          "r"(X26_VALUE),
          "r"(X27_VALUE),
          "r"(X28_VALUE),
          "m"(V8_VALUE),
          "m"(V9_VALUE),
          "m"(V10_VALUE),
          "m"(V11_VALUE),
          "m"(V12_VALUE),
          "m"(V13_VALUE),
          "m"(V14_VALUE),
          "m"(V15_VALUE)
        : "x19",
          "x20",
          "x21",
          "x22",
          "x23",
          "x24",
          "x25",
          "x26",
          "x27",
          "x28",
          "v8",
          "v9",
          "v10",
          "v11",
          "v12",
          "v13",
          "v14",
          "v15");

    /* STEP 3: Switch away to the intermediate function and then wait for it to switch back. */
    switch_context(data->ctxs.test_fiber_context, data->ctxs.intermediary_fiber_context);

    /* STEP 4: We are back from the intermediate function. Read the registers back into variables. */

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    volatile uint64_t x19_val, x20_val, x21_val, x22_val, x23_val, x24_val, x25_val, x26_val, x27_val, x28_val;

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    volatile double v8_val, v9_val, v10_val, v11_val, v12_val, v13_val, v14_val, v15_val;

    /* load general-purpose callee-saved and floating-point registers.  */
    __asm__ volatile( //

        // general-purpose
        "mov %0, x19\n"
        "mov %1, x20\n"
        "mov %2, x21\n"
        "mov %3, x22\n"
        "mov %4, x23\n"
        "mov %5, x24\n"
        "mov %6, x25\n"
        "mov %7, x26\n"
        "mov %8, x27\n"
        "mov %9, x28\n"

        // floating-point - store to memory
        "str d8, %10\n"
        "str d9, %11\n"
        "str d10, %12\n"
        "str d11, %13\n"
        "str d12, %14\n"
        "str d13, %15\n"
        "str d14, %16\n"
        "str d15, %17\n"
        : "=r"(x19_val),
          "=r"(x20_val),
          "=r"(x21_val),
          "=r"(x22_val),
          "=r"(x23_val),
          "=r"(x24_val),
          "=r"(x25_val),
          "=r"(x26_val),
          "=r"(x27_val),
          "=r"(x28_val),
          "=m"(v8_val),
          "=m"(v9_val),
          "=m"(v10_val),
          "=m"(v11_val),
          "=m"(v12_val),
          "=m"(v13_val),
          "=m"(v14_val),
          "=m"(v15_val));

    /* STEP 5: Assert that the restored values match the original magic values. */
    TEST_EQUAL_U64("x19 register", x19_val, X19_VALUE);
    TEST_EQUAL_U64("x20 register", x20_val, X20_VALUE);
    TEST_EQUAL_U64("x21 register", x21_val, X21_VALUE);
    TEST_EQUAL_U64("x22 register", x22_val, X22_VALUE);
    TEST_EQUAL_U64("x23 register", x23_val, X23_VALUE);
    TEST_EQUAL_U64("x24 register", x24_val, X24_VALUE);
    TEST_EQUAL_U64("x25 register", x25_val, X25_VALUE);
    TEST_EQUAL_U64("x26 register", x26_val, X26_VALUE);
    TEST_EQUAL_U64("x27 register", x27_val, X27_VALUE);
    TEST_EQUAL_U64("x28 register", x28_val, X28_VALUE);

    TEST_NEARLY_EQUAL("v8 register", v8_val, V8_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v9 register", v9_val, V9_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v10 register", v10_val, V10_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v11 register", v11_val, V11_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v12 register", v12_val, V12_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v13 register", v13_val, V13_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v14 register", v14_val, V14_VALUE, PRECISION);
    TEST_NEARLY_EQUAL("v15 register", v15_val, V15_VALUE, PRECISION);

    /* STEP 6: Tests state to assert correct execution order. */
    TEST_EQUAL_U32("correct ctx switch from intermediate", data->state, TEST_STATE_INTERMEDIARY_FIBER);
    data->state = TEST_STATE_TEST_FIBER_REGISTERS_LOAD;

    /* End the test by switching back to the main thread. */
    switch_context(data->ctxs.test_fiber_context, data->ctxs.main_context);
}
