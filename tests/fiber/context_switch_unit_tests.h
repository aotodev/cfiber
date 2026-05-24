#ifndef CFIBER_CONTEXT_SWITCH_UNIT_TESTS_H
#define CFIBER_CONTEXT_SWITCH_UNIT_TESTS_H

#include "cfiber/fiber.h"

enum test_state : uint32_t {
    TEST_STATE_INIT = 0,
    TEST_STATE_TEST_FIBER_REGISTERS_SET,
    TEST_STATE_INTERMEDIARY_FIBER,
    TEST_STATE_TEST_FIBER_REGISTERS_LOAD,
    TEST_STATE_BACK_TO_MAIN,
};

struct test_contexts {
    context_t* test_fiber_context;
    context_t* intermediary_fiber_context;
    context_t* main_context;
};

struct user_data {
    struct test_contexts ctxs;
    enum test_state state;
};

/* architecture dependent test function. */
extern void test_register_preservation_function(void*);


#endif // CFIBER_CONTEXT_SWITCH_UNIT_TESTS_H
