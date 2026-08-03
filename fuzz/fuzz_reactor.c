/**
 * @file  fuzz_reactor.c
 * @brief Coverage-guided fuzzer for the epoll(7) reactor.
 *
 * @details
 * Interprets the input as a set of fiber "plans", each a short, deterministic
 * script of reactor operations (sleep, park on a self-owned socketpair with a
 * finite timeout, spawn leaf children, cancel a sibling). Every wait carries a
 * finite deadline and every fiber closes its own descriptors, so the loop always
 * terminates and descriptors never leak across libFuzzer iterations. This drives
 * the park/wake state machine, the timer heap, the command queue and the
 * stale-handle guard under ASan (fiber-aware redzone + switch annotations
 * active), UBSan, and LeakSanitizer.
 *
 * Invariants checked after the run:
 *   - every fiber that started also finished (none lost mid-flight);
 *   - the loop returned (no deadlock / no fiber parked forever).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cfiber/reactor/reactor.h"
#include "fuzz_input.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#define ARENA_CAP 64
#define STACK_SIZE ((size_t)64 * 1024)

typedef struct fuzz_state fuzz_state;

typedef struct {
    fuzz_state* st;
    uint8_t sleep_us;   /* 0..255 microseconds                    */
    uint8_t io_bytes;   /* bytes to push through the self-pipe    */
    uint8_t timeout_us; /* park deadline in microseconds          */
    uint8_t children;   /* leaf children to spawn                 */
    uint8_t cancel_of;  /* index of a sibling to cancel (+1; 0=no) */
} fiber_plan;

struct fuzz_state {
    cfiber_reactor_t* r;
    fiber_plan arena[ARENA_CAP];
    cfiber_reactor_handle_t handles[ARENA_CAP];
    size_t arena_next;
    uint32_t started;
    uint32_t finished;
};

static void worker_fiber(void* arg) {
    fiber_plan* plan = arg;
    fuzz_state* st = plan->st;
    st->started++;

    if (plan->sleep_us) {
        (void)cfiber_ev_sleep((uint64_t)plan->sleep_us * 1000u);
    }

    /* Self-owned socketpair: optionally make it readable, then park on it with a
     * finite timeout (so the fiber always makes progress whether or not ready). */
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0) {
        if (plan->io_bytes) {
            char b[4] = {0};
            size_t n = plan->io_bytes > sizeof b ? sizeof b : plan->io_bytes;
            (void)cfiber_ev_write_timed(fds[1], b, n, (int64_t)plan->timeout_us * 1000);
        }
        char rb[4];
        (void)cfiber_ev_read_timed(fds[0], rb, sizeof rb, (int64_t)plan->timeout_us * 1000);
        close(fds[0]);
        close(fds[1]);
    }

    /* Cancel a sibling (no-op if it is not currently parked or already gone). */
    if (plan->cancel_of) {
        size_t idx = (size_t)(plan->cancel_of - 1) % ARENA_CAP;
        if (idx < st->arena_next) {
            cfiber_reactor_cancel(st->r, st->handles[idx]);
        }
    }

    for (uint8_t i = 0; i < plan->children; i++) {
        if (st->arena_next >= ARENA_CAP) {
            break;
        }
        fiber_plan* child = &st->arena[st->arena_next];
        *child = (fiber_plan){.st = st};
        if (cfiber_ev_spawn(worker_fiber, child, &st->handles[st->arena_next])) {
            st->arena_next++;
        }
    }

    st->finished++;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz_input in = fuzz_input_init(data, size);

    cfiber_reactor_t* r = cfiber_reactor_create((cfiber_reactor_config_t){
        .stack_size = STACK_SIZE,
        .stack_cache = fuzz_range(&in, 0, 16),
    });
    if (!r) {
        return 0;
    }

    static fuzz_state st;
    st = (fuzz_state){.r = r};

    const uint32_t initial = fuzz_range(&in, 1, 16);
    for (uint32_t i = 0; i < initial && st.arena_next < ARENA_CAP; i++) {
        fiber_plan* plan = &st.arena[st.arena_next];
        plan->st = &st;
        plan->sleep_us = (uint8_t)fuzz_range(&in, 0, 50);
        plan->io_bytes = (uint8_t)fuzz_range(&in, 0, 4);
        plan->timeout_us = (uint8_t)fuzz_range(&in, 0, 100);
        plan->children = (uint8_t)fuzz_range(&in, 0, 2);
        plan->cancel_of = (uint8_t)fuzz_range(&in, 0, ARENA_CAP);
        if (cfiber_reactor_spawn(r, worker_fiber, plan, &st.handles[st.arena_next])) {
            st.arena_next++;
        }
    }

    cfiber_reactor_run(r); /* returns only when every fiber has completed */

    FUZZ_CHECK(st.started == st.finished);

    cfiber_reactor_destroy(r);
    return 0;
}
