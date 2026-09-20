/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  test.c
 * @brief Shared state and runner implementation for the cfiber test framework.
 * @see   test.h
 */

#include "test/test.h"

#ifndef CFIBER_FREESTANDING
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

unsigned int cfiber_tests_run = 0;
unsigned int cfiber_tests_failed = 0;
unsigned int cfiber_checks_passed = 0;
unsigned int cfiber_checks_failed = 0;

void cfiber_test_suite_begin(const char* suite_name) {
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
    /* A sanitizer that aborts at exit (LSan) skips the stdio flush; keep the
     * failure text ahead of its report. */
    fflush(stdout);
}

void cfiber_run_test(const char* name, int (*fn)(void)) {
    cfiber_tests_run++;

    const unsigned int failed_before = cfiber_checks_failed;
    const int rc = fn();

    if (rc != 0 || cfiber_checks_failed != failed_before) {
        cfiber_tests_failed++;
        printf("  " RED "[ FAIL ]" NC " %s\n", name);
        fflush(stdout);
    } else {
        printf("  " GREEN "[  OK  ]" NC " %s\n", name);
    }
}

int cfiber_test_report(void) {
    printf("--------------------------------------------------------------------------------------\n");
    printf("  tests:  %u run, " RED "%u failed" NC "\n", cfiber_tests_run, cfiber_tests_failed);
    printf("  checks: %u passed, " RED "%u failed" NC "\n", cfiber_checks_passed, cfiber_checks_failed);

    if (cfiber_tests_run == 0 && cfiber_checks_passed == 0 && cfiber_checks_failed == 0) {
        printf("  " RED "RESULT: FAIL (nothing ran)" NC "\n\n");
        return 1;
    }
    if (cfiber_tests_failed == 0 && cfiber_checks_failed == 0) {
        printf("  " GREEN "RESULT: PASS" NC "\n\n");
        return 0;
    }

    printf("  " RED "RESULT: FAIL" NC "\n\n");
    return 1;
}

#ifndef CFIBER_FREESTANDING
bool cfiber_test_dies(void (*fn)(void*), void* arg) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        /* The child is meant to die; no core dump for it, native or under qemu-user. */
        setrlimit(RLIMIT_CORE, &(struct rlimit){.rlim_cur = 0, .rlim_max = 0});
        fn(arg);
        _exit(0); /* no atexit, no second flush of the parent's buffers */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif
