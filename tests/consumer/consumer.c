/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/* Links against an installed cfiber through find_package: the public headers
 * resolve, the version header is installed, and a scheduler round trip works. */
#include "cfiber/scheduler.h"
#include "cfiber/version.h"

#include <stdio.h>
#include <string.h>

static int g_ran;

static void hello(void* arg) {
    (void)arg;
    g_ran = 1;
    cfiber_yield();
    g_ran = 2;
}

int main(void) {
    cfiber_scheduler_t sched;
    if (cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){.stack_size = 16384}) != 0) {
        return 1;
    }
    if (!cfiber_scheduler_spawn(&sched, hello, nullptr)) {
        return 1;
    }
    cfiber_scheduler_run(&sched);
    cfiber_scheduler_destroy(&sched);

    /* The linked library must be the one the headers describe. */
    if (cfiber_version() != CFIBER_VERSION || strcmp(cfiber_version_string(), CFIBER_VERSION_STRING) != 0) {
        return 1;
    }
    printf("cfiber %s: fiber ran (%d)\n", cfiber_version_string(), g_ran);
    return g_ran == 2 ? 0 : 1;
}
