/**
 * @file  test.c
 * @brief Shared state and runner implementation for the cfiber test framework.
 * @see   test.h
 */

#include "test/test.h"

unsigned int cfiber_tests_run = 0;
unsigned int cfiber_tests_failed = 0;
unsigned int cfiber_checks_passed = 0;
unsigned int cfiber_checks_failed = 0;

void cfiber_test_suite_begin(const char* suite_name) {
    cfiber_tests_run = 0;
    cfiber_tests_failed = 0;
    cfiber_checks_passed = 0;
    cfiber_checks_failed = 0;

    printf("\n" BLUE_BOLD "Running suite: %s" NC "\n", suite_name);
    printf("--------------------------------------------------------------------------------------\n");
}

void cfiber_check_pass(void) {
    cfiber_checks_passed++;
}

void cfiber_check_fail(const char* file, int line, const char* fmt, ...) {
    cfiber_checks_failed++;

    printf("    " RED "assertion failed" NC " (%s:%d)\n      ", file, line);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
}

void cfiber_run_test(const char* name, int (*fn)(void)) {
    cfiber_tests_run++;

    const unsigned int failed_before = cfiber_checks_failed;
    const int rc = fn();

    if (rc != 0 || cfiber_checks_failed != failed_before) {
        cfiber_tests_failed++;
        printf("  " RED "[ FAIL ]" NC " %s\n", name);
    } else {
        printf("  " GREEN "[  OK  ]" NC " %s\n", name);
    }
}

int cfiber_test_report(void) {
    printf("--------------------------------------------------------------------------------------\n");
    printf("  tests:  %u run, " RED "%u failed" NC "\n", cfiber_tests_run, cfiber_tests_failed);
    printf("  checks: %u passed, " RED "%u failed" NC "\n", cfiber_checks_passed, cfiber_checks_failed);

    if (cfiber_tests_failed == 0 && cfiber_checks_failed == 0) {
        printf("  " GREEN "RESULT: PASS" NC "\n\n");
        return 0;
    }

    printf("  " RED "RESULT: FAIL" NC "\n\n");
    return 1;
}
