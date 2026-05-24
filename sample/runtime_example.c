#include "cfiber/scheduler/scheduler.h"
#include "config.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */

void nested_fiber_function(void*) {
    int count = 2;
    printf("STARTING nested fiber\n");

    while (--count) {
        printf("nested_fiber_function with count == %d\n", count);
        cfiber_yield();
    }

    printf("FINISHING nested fiber\n");
}

/* ------------------------------------------------------------------ */

typedef struct data {
    int count;
    const char* name;
} data_t;

void other_function(void* data) {
    data_t userData = *(data_t*)data;

    const int value = userData.count * 2;

    printf("CALLSTACK nested start %s with count = %d and doubled-value == %d\n", userData.name, userData.count, value);
    cfiber_yield();
    printf("CALLSTACK nested ended %s, with count = %d and doubled-value = %d\n", userData.name, userData.count, value);
}

void fiber_function(void* data) {
    data_t userData = *(data_t*)data;

    printf("STARTING fiber %s\n", userData.name);

    while (--userData.count) {
        printf("fiber '%s': %d\n", userData.name, userData.count);

        if (userData.count == 6) {
            printf("starting new CALLSTACK %s\n", userData.name);
            other_function(data);
            printf("ending new CALLSTACK %s\n", userData.name);
        }

        if (userData.count == 5) {
            cfiber_spawn(nested_fiber_function, nullptr);
        }

        cfiber_yield();
    }

    printf("FINISHING fiber %s\n", userData.name);
}

/* ------------------------------------------------------------------ */

int main(void) {
    printf("=== STARTING SIMPLE FIBER SAMPLE ===\n");

    cfiber_scheduler_t sched;
    cfiber_scheduler_config_t config = {
        .stack_size = FIBER_STACK_SIZE,
        .fibers_per_slab = FIBERS_PER_SLAB,
        .max_slabs = 0, /* unlimited */
    };

    if (cfiber_scheduler_init(&sched, config) != 0) {
        printf("failed to initialise scheduler\n");
        return 1;
    }

    data_t fiberData1 = {10, "first_fiber"};
    data_t fiberData2 = {15, "second_fiber"};

    cfiber_scheduler_spawn(&sched, fiber_function, &fiberData1);
    cfiber_scheduler_spawn(&sched, fiber_function, &fiberData2);

    /* Start scheduler (blocks until all fibers complete) */
    cfiber_scheduler_run(&sched);

    cfiber_scheduler_destroy(&sched);

    printf("=== SIMPLE FIBER SAMPLE COMPLETED ===\n");
    return 0;
}
