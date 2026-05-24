#include "context_switch_unit_tests.h"
#include "test/test.h"

const size_t UNIT_TEST_STACK_SIZE = 4096;

constexpr uint64_t RBX_VALUE = 0xAAAAAAAAAAAAAAAA;
constexpr uint64_t R12_VALUE = 0xCCCCCCCCCCCCCCCC;
constexpr uint64_t R13_VALUE = 0xDDDDDDDDDDDDDDDD;
constexpr uint64_t R14_VALUE = 0xEEEEEEEEEEEEEEEE;
constexpr uint64_t R15_VALUE = 0xFFFFFFFFFFFFFFFF;

/* Function for the first context */
void test_register_preservation_function(void* userData) {
    struct user_data* data = (struct user_data*)userData;

    TEST_EQUAL_U32("correct ctx switch from main", data->state, TEST_STATE_INIT);
    data->state = TEST_STATE_TEST_FIBER_REGISTERS_SET;

    /* STEP 1 & 2: Set callee-saved registers with our magic values.
     * These must be preserved by the context switch
     */
    __asm__ volatile( //
        "mov rbx, %0\n"
        "mov r12, %1\n"
        "mov r13, %2\n"
        "mov r14, %3\n"
        "mov r15, %4\n"
        :
        : "r"(RBX_VALUE), "r"(R12_VALUE), "r"(R13_VALUE), "r"(R14_VALUE), "r"(R15_VALUE)
        : "rbx", "r12", "r13", "r14", "r15");

    /* STEP 3: Switch away to the intermediate function and then wait for it to switch back. */
    switch_context(data->ctxs.test_fiber_context, data->ctxs.intermediary_fiber_context);

    /* STEP 4: We are back from the intermediate function. Read the registers back into variables. */

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    volatile uint64_t rbx_val, r12_val, r13_val, r14_val, r15_val;

    __asm__ volatile( //
        "mov %0, rbx\n"
        "mov %1, r12\n"
        "mov %2, r13\n"
        "mov %3, r14\n"
        "mov %4, r15\n"
        : "=r"(rbx_val), "=r"(r12_val), "=r"(r13_val), "=r"(r14_val), "=r"(r15_val));

    /* STEP 5: Assert that the restored values match the original magic values. */
    TEST_EQUAL_U64("rbx", rbx_val, RBX_VALUE);
    TEST_EQUAL_U64("r12", r12_val, R12_VALUE);
    TEST_EQUAL_U64("r12", r13_val, R13_VALUE);
    TEST_EQUAL_U64("r14", r14_val, R14_VALUE);
    TEST_EQUAL_U64("r15", r15_val, R15_VALUE);

    /* STEP 6: Tests state to assert correct execution order. */
    TEST_EQUAL_U32("correct ctx switch from intermediate", data->state, TEST_STATE_INTERMEDIARY_FIBER);
    data->state = TEST_STATE_TEST_FIBER_REGISTERS_LOAD;

    /* End the test by switching back to the main thread. */
    switch_context(data->ctxs.test_fiber_context, data->ctxs.main_context);
}
