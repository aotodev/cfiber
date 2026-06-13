/**
 * @file  test.h
 * @brief Minimal, dependency-free unit-test framework for cfiber.
 *
 * @details
 * Designed for both hosted and freestanding (bare-metal Cortex-M via
 * semihosting) targets: it relies only on <stdio.h> and <stdarg.h>, never on
 * malloc, longjmp, atexit, or constructors. Output goes through printf, which
 * the Cortex-M test runners route over semihosting.
 *
 * Two usage styles share the same counters and final verdict:
 *
 *   1. Structured tests (preferred for new code):
 *
 *          static int my_test(void) {
 *              ASSERT_TRUE(some_condition);
 *              ASSERT_EQ_U32(actual, expected);
 *              return 0;            // reached only if no ASSERT_* fired
 *          }
 *
 *          int main(void) {
 *              cfiber_test_suite_begin("my suite");
 *              RUN_TEST(my_test);
 *              return cfiber_test_report();   // non-zero if anything failed
 *          }
 *
 *      ASSERT_* macros are fatal: on failure they print file:line plus a
 *      message and `return 1` from the enclosing test function, so the test
 *      function must return int and be invoked through RUN_TEST.
 *
 *   2. Legacy inline checks (TEST_EQUAL_U32 / TEST_EQUAL_U64 /
 *      TEST_NEARLY_EQUAL): non-fatal, used by the per-architecture
 *      register-preservation tests. They record pass/fail and keep going.
 *
 * Either way, the process exit code is what matters: cfiber_test_report()
 * returns 0 only when every check and every test passed, so ctest / CI can
 * trust it.
 */

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

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Framework state (defined in test.c)
 * ============================================================================ */

/** Number of test functions invoked through RUN_TEST. */
extern unsigned int cfiber_tests_run;
/** Number of those test functions that reported a failure. */
extern unsigned int cfiber_tests_failed;
/** Individual assertions / checks that passed. */
extern unsigned int cfiber_checks_passed;
/** Individual assertions / checks that failed. */
extern unsigned int cfiber_checks_failed;

/** Print a suite banner and reset the counters. */
void cfiber_test_suite_begin(const char* suite_name);

/**
 * Print the final summary.
 * @return 0 if every check and every test passed, 1 otherwise. Intended to be
 *         returned directly from main() so the exit code reflects the result.
 */
int cfiber_test_report(void);

/** Record a passing check (no output). */
void cfiber_check_pass(void);

/** Record a failing check and print a printf-style detail at file:line. */
void cfiber_check_fail(const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/** Run a single `int (*)(void)` test (0 == pass) and print its verdict. */
void cfiber_run_test(const char* name, int (*fn)(void));

/** Invoke a structured test function through the runner. */
#define RUN_TEST(fn) cfiber_run_test(#fn, (fn))

/* ============================================================================
 * Fatal assertions — for use inside RUN_TEST functions.
 * On failure they record the failure and `return 1` from the test function.
 * ============================================================================ */

/* NOLINTBEGIN(bugprone-macro-parentheses) — stringized args are intentional. */

#define CFIBER_FAIL_(...)                                                                                              \
    do {                                                                                                               \
        cfiber_check_fail(__FILE__, __LINE__, __VA_ARGS__);                                                            \
        return 1;                                                                                                      \
    } while (0)

#define ASSERT_TRUE(cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            CFIBER_FAIL_("ASSERT_TRUE failed: %s", #cond);                                                             \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_FALSE(cond)                                                                                             \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            CFIBER_FAIL_("ASSERT_FALSE failed: %s", #cond);                                                            \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_NULL(ptr)                                                                                               \
    do {                                                                                                               \
        const void* cfiber_p_ = (ptr);                                                                                 \
        if (cfiber_p_ != nullptr) {                                                                                    \
            CFIBER_FAIL_("ASSERT_NULL failed: %s = %p", #ptr, cfiber_p_);                                              \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_NOT_NULL(ptr)                                                                                           \
    do {                                                                                                               \
        const void* cfiber_p_ = (ptr);                                                                                 \
        if (cfiber_p_ == nullptr) {                                                                                    \
            CFIBER_FAIL_("ASSERT_NOT_NULL failed: %s is null", #ptr);                                                  \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_EQ_PTR(a, b)                                                                                            \
    do {                                                                                                               \
        const void* cfiber_a_ = (a);                                                                                   \
        const void* cfiber_b_ = (b);                                                                                   \
        if (cfiber_a_ != cfiber_b_) {                                                                                  \
            CFIBER_FAIL_("ASSERT_EQ_PTR(%s, %s): %p != %p", #a, #b, cfiber_a_, cfiber_b_);                             \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_NE_PTR(a, b)                                                                                            \
    do {                                                                                                               \
        const void* cfiber_a_ = (a);                                                                                   \
        const void* cfiber_b_ = (b);                                                                                   \
        if (cfiber_a_ == cfiber_b_) {                                                                                  \
            CFIBER_FAIL_("ASSERT_NE_PTR(%s, %s): both %p", #a, #b, cfiber_a_);                                         \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_EQ_U32(a, b)                                                                                            \
    do {                                                                                                               \
        const uint32_t cfiber_a_ = (uint32_t)(a);                                                                      \
        const uint32_t cfiber_b_ = (uint32_t)(b);                                                                      \
        if (cfiber_a_ != cfiber_b_) {                                                                                  \
            CFIBER_FAIL_("ASSERT_EQ_U32(%s, %s): %" PRIu32 " != %" PRIu32, #a, #b, cfiber_a_, cfiber_b_);              \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

#define ASSERT_EQ_U64(a, b)                                                                                            \
    do {                                                                                                               \
        const uint64_t cfiber_a_ = (uint64_t)(a);                                                                      \
        const uint64_t cfiber_b_ = (uint64_t)(b);                                                                      \
        if (cfiber_a_ != cfiber_b_) {                                                                                  \
            CFIBER_FAIL_("ASSERT_EQ_U64(%s, %s): %" PRIu64 " != %" PRIu64, #a, #b, cfiber_a_, cfiber_b_);              \
        }                                                                                                              \
        cfiber_check_pass();                                                                                           \
    } while (0)

/* NOLINTEND(bugprone-macro-parentheses) */

/* ============================================================================
 * Legacy non-fatal checks — kept for the per-architecture register tests.
 * These do NOT abort the enclosing function; they record and continue.
 * ============================================================================ */

[[maybe_unused]] static void on_test_success(const char* testName) {
    printf(
        "  ............................................................................. \033[1;37;42mPassed\033[0m");
    printf("\r" BLUE_BOLD "  Test:" NC " [%s] \n", testName);
    cfiber_check_pass();
}

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
    do {                                                                                                               \
        if (!nearly_equal(value, expected, precision)) {                                                               \
            cfiber_check_fail(__FILE__,                                                                                \
                              __LINE__,                                                                                \
                              "[%s] expected %.3f, but was %.3f",                                                      \
                              testName,                                                                                \
                              (double)(expected),                                                                      \
                              (double)(value));                                                                        \
        } else {                                                                                                       \
            on_test_success(testName);                                                                                 \
        }                                                                                                              \
    } while (0)

#define TEST_EQUAL_U32(testName, value, expected)                                                                      \
    do {                                                                                                               \
        if ((uint32_t)(value) != (uint32_t)(expected)) {                                                               \
            cfiber_check_fail(__FILE__,                                                                                \
                              __LINE__,                                                                                \
                              "[%s] expected %" PRIu32 ", but was %" PRIu32,                                           \
                              testName,                                                                                \
                              (uint32_t)(expected),                                                                    \
                              (uint32_t)(value));                                                                      \
        } else {                                                                                                       \
            on_test_success(testName);                                                                                 \
        }                                                                                                              \
    } while (0)

#define TEST_EQUAL_U64(testName, value, expected)                                                                      \
    do {                                                                                                               \
        if ((uint64_t)(value) != (uint64_t)(expected)) {                                                               \
            cfiber_check_fail(__FILE__,                                                                                \
                              __LINE__,                                                                                \
                              "[%s] expected %" PRIu64 ", but was %" PRIu64,                                           \
                              testName,                                                                                \
                              (uint64_t)(expected),                                                                    \
                              (uint64_t)(value));                                                                      \
        } else {                                                                                                       \
            on_test_success(testName);                                                                                 \
        }                                                                                                              \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // CFIBER_TEST_H
