#ifndef CFIBER_TEST_H
#define CFIBER_TEST_H

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

/* ANSI colors for output. */
#define RED "\033[0;31m"
#define GREEN "\033[0;32m"
#define YELLOW "\033[1;33m"
#define BLUE "\033[0;34m"
#define BLUE_BOLD "\033[1;34m"
#define NC "\033[0m"

[[maybe_unused]] static void on_test_success(const char* testName) {
    printf(
        "  ............................................................................. \033[1;37;42mPassed\033[0m");
    printf("\r" BLUE_BOLD "  Test:" NC " [%s] \n", testName);
}

/* NOLINTNEXTLINE */
#define TEST_FAILED(testName, testMessage, ...)                                                                        \
    printf(                                                                                                            \
        "  ............................................................................. \033[1;37;41mFail\033[0m");   \
    printf("\r" BLUE_BOLD "  Test:" NC " [%s] \n", testName);                                                          \
    printf("\t" testMessage "'\n", __VA_ARGS__)

#if defined(__arm__) && defined(CFIBER_ARM_FPU)

static float abs_single(const float value) {
    return value >= 0.0f ? value : value * -1.0f;
}

[[maybe_unused]] static bool nearly_equal(const float rhs, const float lhs, const float tolerance) {
    return abs_single(lhs - rhs) <= tolerance;
}

#else

[[maybe_unused]] static bool nearly_equal(const double rhs, const double lhs, const double tolerance) {
    return fabs(lhs - rhs) <= tolerance;
}

#endif

#define TEST_NEARLY_EQUAL(testName, value, expected, precision)                                                        \
    if (!nearly_equal(value, expected, precision)) {                                                                   \
        TEST_FAILED(testName, "expected value to be %.3f, but was %.3f", expected, value);                             \
    } else {                                                                                                           \
        on_test_success(testName);                                                                                     \
    }

#define TEST_EQUAL_U32(testName, value, expected)                                                                      \
    if ((value) != (expected)) {                                                                                       \
        TEST_FAILED(testName, "expected value to be %" PRIu32 ", but was %" PRIu32, expected, value);                  \
    } else {                                                                                                           \
        on_test_success(testName);                                                                                     \
    }

#define TEST_EQUAL_U64(testName, value, expected)                                                                      \
    if ((value) != (expected)) {                                                                                       \
        TEST_FAILED(testName, "expected value to be %" PRIu64 ", but was %" PRIu64, expected, value);                  \
    } else {                                                                                                           \
        on_test_success(testName);                                                                                     \
    }


#endif // CFIBER_TEST_H
