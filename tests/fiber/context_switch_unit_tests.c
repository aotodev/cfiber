#include "context_switch_unit_tests.h"

#include "test/test.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

[[noreturn]] void scheduler_return_fiber() {
    __builtin_unreachable();
}

static void setup_fiber(fiber_t* fiber, size_t stackSize) {
    /**
     * Note that we are purposely using malloc so that we easily
     * catch stack overlflows with a segfault.
     */
    fiber->stack = malloc(stackSize);
    if (!(fiber->stack)) {
        if (errno == ENOMEM) {
            printf("ENOMEM\n");
        }
        exit(-1);
    }

    fiber->stack_size = stackSize;

    memset(fiber->stack, 0, fiber->stack_size);
    memset(&fiber->ctx, 0, sizeof(context_t));
}

static void cleanup_fiber(fiber_t* fiber) {
    assert(fiber && fiber->stack);
    free(fiber->stack);

    fiber->stack = nullptr;
    fiber->stack_size = 0;

    memset(&fiber->ctx, 0, sizeof(context_t));
}

extern const size_t UNIT_TEST_STACK_SIZE;

/* intermediary function to test switchint to and from in the test function. */
void intermediary_function(void* userData) {
    struct user_data* data = (struct user_data*)userData;

    TEST_EQUAL_U32("intermediary state", data->state, TEST_STATE_TEST_FIBER_REGISTERS_SET);
    data->state = TEST_STATE_INTERMEDIARY_FIBER;

    switch_context(data->ctxs.intermediary_fiber_context, data->ctxs.test_fiber_context);
}

int main() {
    printf("\n" BLUE_BOLD "Starting context switch test\n" NC);
    printf("--------------------------------------------------------------------------------------\n");

    fiber_t test_fiber;
    fiber_t intermediary_fiber;

    /* 1. Create fibers. */
    setup_fiber(&test_fiber, UNIT_TEST_STACK_SIZE);
    setup_fiber(&intermediary_fiber, UNIT_TEST_STACK_SIZE);

    /* Initialize the contexts to point to their respective functions and stacks. */

    /* main's context to restore after the fiber functions finish running. */
    context_t main_context;

    /* set common state as user data */
    struct user_data data;
    data.ctxs.test_fiber_context = &test_fiber.ctx;
    data.ctxs.intermediary_fiber_context = &intermediary_fiber.ctx;
    data.ctxs.main_context = &main_context;
    data.state = TEST_STATE_INIT;

    init_fiber(&test_fiber, test_register_preservation_function, &data);
    init_fiber(&intermediary_fiber, intermediary_function, &data);

    /* Start the test by switching from main's context to test_fiber's context. */
    switch_context(&main_context, &test_fiber.ctx);

    /* The test is over when control returns here. Assert that all steps completed in the correct order. */
    TEST_EQUAL_U32("return to main", data.state, TEST_STATE_TEST_FIBER_REGISTERS_LOAD);
    data.state = TEST_STATE_BACK_TO_MAIN;

    /* Clean up. */
    cleanup_fiber(&test_fiber);
    cleanup_fiber(&intermediary_fiber);
}
