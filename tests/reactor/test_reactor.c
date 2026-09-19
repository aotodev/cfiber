/**
 * @file  test_reactor.c
 * @brief Functional tests for the optional epoll(7) reactor.
 *
 * Each test builds a reactor, spawns fibers that record results into shared
 * state, runs the loop to completion, then asserts on that state from the
 * (non-fiber) test function. Real socketpairs / pipes exercise the park/wake
 * machinery without any network.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cfiber/reactor/reactor.h"
#include "test/test.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const size_t STACK = (size_t)64 * 1024;

static cfiber_reactor_t* make_reactor(void) {
    return cfiber_reactor_create((cfiber_reactor_config_t){.stack_size = STACK, .stack_cache = 64});
}

/* A non-blocking, connected socket pair. */
static int make_pair(int fds[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
}

static int64_t ms_to_ns(int64_t ms) {
    return ms * 1000000;
}

static void trivial_fiber(void* arg) {
    (void)arg;
    cfiber_ev_yield();
}

/* ============================================================================
 * spawn / yield / completion ordering
 * ============================================================================ */

static int g_seq[8];
static int g_seq_len;

static void ordering_fiber(void* arg) {
    int id = (int)(intptr_t)arg;
    g_seq[g_seq_len++] = id; /* run */
    cfiber_ev_yield();
    g_seq[g_seq_len++] = id + 10; /* after a yield round */
}

static int test_spawn_yield_completion(void) {
    g_seq_len = 0;
    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    ASSERT_TRUE(cfiber_reactor_spawn(r, ordering_fiber, (void*)(intptr_t)1, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, ordering_fiber, (void*)(intptr_t)2, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, ordering_fiber, (void*)(intptr_t)3, nullptr));

    cfiber_reactor_run(r);

    /* FIFO: all three run, each yields once, then all three resume in order. */
    ASSERT_EQ_U32(g_seq_len, 6);
    ASSERT_EQ_U32(g_seq[0], 1);
    ASSERT_EQ_U32(g_seq[1], 2);
    ASSERT_EQ_U32(g_seq[2], 3);
    ASSERT_EQ_U32(g_seq[3], 11);
    ASSERT_EQ_U32(g_seq[4], 12);
    ASSERT_EQ_U32(g_seq[5], 13);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * fd park / wake over a socketpair
 * ============================================================================ */

static char g_recv[16];
static ssize_t g_recv_n;

static void reader_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    g_recv_n = cfiber_ev_read(fd, g_recv, sizeof g_recv); /* parks on EAGAIN */
    close(fd);
}

static void writer_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    cfiber_ev_yield(); /* let the reader park first */
    (void)cfiber_ev_write(fd, "hello", 5);
    close(fd);
}

static int test_fd_park_wake(void) {
    g_recv_n = -2;
    memset(g_recv, 0, sizeof g_recv);

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, reader_fiber, (void*)(intptr_t)fds[0], nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, writer_fiber, (void*)(intptr_t)fds[1], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_recv_n, 5);
    ASSERT_TRUE(memcmp(g_recv, "hello", 5) == 0);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * timer ordering (cfiber_ev_sleep)
 * ============================================================================ */

static int g_wake_order[4];
static int g_wake_len;

typedef struct {
    int id;
    int64_t ms;
} sleep_arg_t;

static void sleeper_fiber(void* arg) {
    sleep_arg_t* a = arg;
    cfiber_ev_status_t st = cfiber_ev_sleep((uint64_t)ms_to_ns(a->ms));
    if (st == CFIBER_EV_TIMEOUT) {
        g_wake_order[g_wake_len++] = a->id;
    }
}

static int test_timer_ordering(void) {
    g_wake_len = 0;
    static sleep_arg_t a0 = {.id = 1, .ms = 30};
    static sleep_arg_t a1 = {.id = 2, .ms = 10};
    static sleep_arg_t a2 = {.id = 3, .ms = 20};

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, sleeper_fiber, &a0, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, sleeper_fiber, &a1, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, sleeper_fiber, &a2, nullptr));

    cfiber_reactor_run(r);

    /* Wake in ascending deadline order regardless of spawn order. */
    ASSERT_EQ_U32(g_wake_len, 3);
    ASSERT_EQ_U32(g_wake_order[0], 2); /* 10ms */
    ASSERT_EQ_U32(g_wake_order[1], 3); /* 20ms */
    ASSERT_EQ_U32(g_wake_order[2], 1); /* 30ms */

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * wait timeout: a read on an fd that never becomes ready
 * ============================================================================ */

static ssize_t g_timed_rc;
static int g_timed_errno;

static void timeout_reader_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8];
    g_timed_rc = cfiber_ev_read_timed(fd, buf, sizeof buf, ms_to_ns(10));
    g_timed_errno = errno;
    close(fd);
}

static int test_wait_timeout(void) {
    g_timed_rc = 0;
    g_timed_errno = 0;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    /* Only the reader runs; the peer fd[1] is held by the test and never written. */
    ASSERT_TRUE(cfiber_reactor_spawn(r, timeout_reader_fiber, (void*)(intptr_t)fds[0], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32((uint32_t)(int)g_timed_rc, (uint32_t)-1);
    ASSERT_EQ_U32(g_timed_errno, ETIMEDOUT);

    close(fds[1]);
    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * cancellation of a parked fiber (same-thread, from another fiber)
 * ============================================================================ */

static cfiber_ev_status_t g_cancel_status;

typedef struct {
    cfiber_reactor_t* r;
    cfiber_reactor_handle_t target;
} cancel_arg_t;

static void async_waiter_fiber(void* arg) {
    (void)arg;
    g_cancel_status = cfiber_ev_wait_async(-1); /* infinite; only cancellation frees it */
}

static void canceller_fiber(void* arg) {
    cancel_arg_t* a = arg;
    cfiber_reactor_cancel(a->r, a->target);
}

static int test_cancel_parked(void) {
    g_cancel_status = CFIBER_EV_READY;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    cfiber_reactor_handle_t h;
    ASSERT_TRUE(cfiber_reactor_spawn(r, async_waiter_fiber, nullptr, &h));

    static cancel_arg_t ca;
    ca.r = r;
    ca.target = h;
    ASSERT_TRUE(cfiber_reactor_spawn(r, canceller_fiber, &ca, nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_cancel_status, CFIBER_EV_CANCELLED);

    /* The handle is now stale; a further cancel is a harmless no-op. */
    cfiber_reactor_cancel(r, h);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * cross-thread wake and cancel (from a helper pthread while the loop runs)
 * ============================================================================ */

typedef struct {
    cfiber_reactor_t* r;
    cfiber_reactor_handle_t target;
    bool cancel; /* true: cancel, false: wake */
} ctrl_arg_t;

static void* control_thread(void* arg) {
    ctrl_arg_t* a = arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20L * 1000000}; /* 20ms */
    nanosleep(&ts, nullptr);
    if (a->cancel) {
        cfiber_reactor_cancel(a->r, a->target);
    } else {
        cfiber_reactor_wake(a->r, a->target);
    }
    return nullptr;
}

static cfiber_ev_status_t g_xthread_status;

static void xthread_waiter_fiber(void* arg) {
    (void)arg;
    g_xthread_status = cfiber_ev_wait_async(-1);
}

static int run_xthread(bool cancel, cfiber_ev_status_t expect) {
    g_xthread_status = CFIBER_EV_ERROR;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    cfiber_reactor_handle_t h;
    ASSERT_TRUE(cfiber_reactor_spawn(r, xthread_waiter_fiber, nullptr, &h));

    ctrl_arg_t ca = {.r = r, .target = h, .cancel = cancel};
    pthread_t th;
    ASSERT_EQ_U32(pthread_create(&th, nullptr, control_thread, &ca), 0);

    cfiber_reactor_run(r); /* blocks until the helper wakes/cancels the fiber */

    pthread_join(th, nullptr);
    ASSERT_EQ_U32(g_xthread_status, expect);

    cfiber_reactor_destroy(r);
    return 0;
}

static int test_cross_thread_wake(void) {
    return run_xthread(/*cancel=*/false, CFIBER_EV_READY);
}

static int test_cross_thread_cancel(void) {
    return run_xthread(/*cancel=*/true, CFIBER_EV_CANCELLED);
}

/* ============================================================================
 * same-thread wake / cancel fast path (cfiber_ev_wake / cfiber_ev_cancel)
 * ============================================================================ */

static cfiber_ev_status_t g_inproc_wake_st;
static cfiber_ev_status_t g_inproc_cancel_st;

static void inproc_wake_waiter(void* arg) {
    (void)arg;
    g_inproc_wake_st = cfiber_ev_wait_async(-1);
}

static void inproc_cancel_waiter(void* arg) {
    (void)arg;
    g_inproc_cancel_st = cfiber_ev_wait_async(-1);
}

typedef struct {
    cfiber_reactor_handle_t wake_h;
    cfiber_reactor_handle_t cancel_h;
} inproc_driver_arg_t;

static void inproc_driver(void* arg) {
    inproc_driver_arg_t* a = arg;
    cfiber_ev_wake(a->wake_h);     /* direct apply, no eventfd hop */
    cfiber_ev_cancel(a->cancel_h); /* direct apply */
}

static int test_inproc_wake_cancel(void) {
    g_inproc_wake_st = CFIBER_EV_ERROR;
    g_inproc_cancel_st = CFIBER_EV_ERROR;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    static inproc_driver_arg_t da;
    ASSERT_TRUE(cfiber_reactor_spawn(r, inproc_wake_waiter, nullptr, &da.wake_h));
    ASSERT_TRUE(cfiber_reactor_spawn(r, inproc_cancel_waiter, nullptr, &da.cancel_h));
    ASSERT_TRUE(cfiber_reactor_spawn(r, inproc_driver, &da, nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_inproc_wake_st, CFIBER_EV_READY);
    ASSERT_EQ_U32(g_inproc_cancel_st, CFIBER_EV_CANCELLED);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * cross-thread ring stress: several producer threads waking many fibers
 * ============================================================================ */

#define RING_WAITERS 64
#define RING_THREADS 4

static cfiber_ev_status_t g_ring_st[RING_WAITERS];

static void ring_waiter(void* arg) {
    long i = (long)(intptr_t)arg;
    g_ring_st[i] = cfiber_ev_wait_async(-1);
}

typedef struct {
    cfiber_reactor_t* r;
    cfiber_reactor_handle_t* handles;
    int lo;
    int hi;
    int failed;
} waker_arg_t;

static void* waker_thread(void* arg) {
    waker_arg_t* a = arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50L * 1000000}; /* let waiters park */
    nanosleep(&ts, nullptr);
    for (int i = a->lo; i < a->hi; i++) {
        if (!cfiber_reactor_wake(a->r, a->handles[i])) { /* concurrent MPMC producers */
            a->failed++;
        }
    }
    return nullptr;
}

static int test_ring_stress(void) {
    for (int i = 0; i < RING_WAITERS; i++) {
        g_ring_st[i] = CFIBER_EV_ERROR;
    }

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    static cfiber_reactor_handle_t handles[RING_WAITERS];
    for (long i = 0; i < RING_WAITERS; i++) {
        ASSERT_TRUE(cfiber_reactor_spawn(r, ring_waiter, (void*)(intptr_t)i, &handles[i]));
    }

    pthread_t th[RING_THREADS];
    waker_arg_t wa[RING_THREADS];
    const int per = RING_WAITERS / RING_THREADS;
    for (int t = 0; t < RING_THREADS; t++) {
        wa[t] = (waker_arg_t){.r = r, .handles = handles, .lo = t * per, .hi = (t + 1) * per};
        ASSERT_EQ_U32(pthread_create(&th[t], nullptr, waker_thread, &wa[t]), 0);
    }

    cfiber_reactor_run(r); /* blocks until all waiters are woken across threads */

    for (int t = 0; t < RING_THREADS; t++) {
        pthread_join(th[t], nullptr);
        ASSERT_EQ_U32(wa[t].failed, 0);
    }
    for (int i = 0; i < RING_WAITERS; i++) {
        ASSERT_EQ_U32(g_ring_st[i], CFIBER_EV_READY);
    }

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * concurrency stress: many echo pairs multiplexed on one loop
 * ============================================================================ */

#define STRESS_PAIRS 200

static int g_echo_ok;

static void echo_server_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[32];
    ssize_t n = cfiber_ev_read(fd, buf, sizeof buf);
    if (n > 0) {
        (void)cfiber_ev_write(fd, buf, (size_t)n);
    }
    close(fd);
}

static void echo_client_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    const char* msg = "ping";
    char buf[32];
    if (cfiber_ev_write(fd, msg, 4) == 4) {
        ssize_t n = cfiber_ev_read(fd, buf, sizeof buf);
        if (n == 4 && memcmp(buf, msg, 4) == 0) {
            g_echo_ok++;
        }
    }
    close(fd);
}

static int test_concurrency_stress(void) {
    g_echo_ok = 0;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    for (int i = 0; i < STRESS_PAIRS; i++) {
        int fds[2];
        ASSERT_EQ_U32(make_pair(fds), 0);
        ASSERT_TRUE(cfiber_reactor_spawn(r, echo_server_fiber, (void*)(intptr_t)fds[1], nullptr));
        ASSERT_TRUE(cfiber_reactor_spawn(r, echo_client_fiber, (void*)(intptr_t)fds[0], nullptr));
    }

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_echo_ok, STRESS_PAIRS);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * lifecycle: destroy with live fibers, run failure, run twice
 * ============================================================================ */

/* Fibers spawned but never run: destroy must release their stacks (LSan/ASan
 * verify) instead of tripping the stack pool's leak check. */
static int test_destroy_without_run(void) {
    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, trivial_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, trivial_fiber, nullptr, nullptr));
    cfiber_reactor_destroy(r);
    return 0;
}

/* The reactor's epoll descriptor is private; find it through /proc so a fiber
 * can break the poller from the inside. The test process has exactly one. */
static int find_epoll_fd(void) {
    for (int fd = 3; fd < 1024; fd++) {
        char path[64];
        char target[64];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, target, sizeof target - 1);
        if (n > 0) {
            target[n] = '\0';
            if (strcmp(target, "anon_inode:[eventpoll]") == 0) {
                return fd;
            }
        }
    }
    return -1;
}

static int g_broken_epfd;

static void break_poller_fiber(void* arg) {
    (void)arg;
    g_broken_epfd = find_epoll_fd();
    if (g_broken_epfd >= 0) {
        close(g_broken_epfd);
    }
    (void)cfiber_ev_spawn(trivial_fiber, nullptr, nullptr); /* ready, never runs */
}

static cfiber_ev_status_t g_parked_forever_st;

static void parked_forever_fiber(void* arg) {
    (void)arg;
    g_parked_forever_st = cfiber_ev_wait_async(-1);
}

/* run must report the poller failure instead of returning as if complete, and
 * destroy must then tear down the parked and the ready fiber. */
static int test_run_reports_poller_failure(void) {
    g_broken_epfd = -1;
    g_parked_forever_st = CFIBER_EV_READY;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, parked_forever_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, break_poller_fiber, nullptr, nullptr));

    errno = 0;
    const int rc = cfiber_reactor_run(r);
    ASSERT_TRUE(g_broken_epfd >= 0);
    ASSERT_EQ_U32((uint32_t)rc, (uint32_t)-1);
    ASSERT_EQ_U32(errno, EBADF);
    ASSERT_EQ_U32(g_parked_forever_st, CFIBER_EV_READY); /* never resumed */

    cfiber_reactor_destroy(r); /* one parked, one ready: forced teardown */
    return 0;
}

static int g_runs_seen;

static void count_run_fiber(void* arg) {
    (void)arg;
    g_runs_seen++;
}

static int test_run_twice(void) {
    g_runs_seen = 0;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    ASSERT_TRUE(cfiber_reactor_spawn(r, count_run_fiber, nullptr, nullptr));
    ASSERT_EQ_U32(cfiber_reactor_run(r), 0);
    ASSERT_EQ_U32(g_runs_seen, 1);

    ASSERT_EQ_U32(cfiber_reactor_run(r), 0); /* nothing to do */

    ASSERT_TRUE(cfiber_reactor_spawn(r, count_run_fiber, nullptr, nullptr));
    ASSERT_EQ_U32(cfiber_reactor_run(r), 0);
    ASSERT_EQ_U32(g_runs_seen, 2);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * nested reactors: a fiber runs a second reactor to completion
 * ============================================================================ */

typedef struct {
    cfiber_reactor_t* outer;
    int inner_rc;   /* run() of the inner reactor */
    int reentry_rc; /* run() of the outer from its own fiber */
    int reentry_errno;
    cfiber_ev_status_t inner_st;    /* a sleep on the inner, from its fiber */
    cfiber_ev_status_t outer_after; /* a sleep on the outer after the nested run */
} nested_reactor_state;

static void nested_inner_fiber(void* arg) {
    nested_reactor_state* st = arg;
    st->inner_st = cfiber_ev_sleep((uint64_t)ms_to_ns(1));
}

static void nested_outer_fiber(void* arg) {
    nested_reactor_state* st = arg;

    st->reentry_rc = cfiber_reactor_run(st->outer);
    st->reentry_errno = errno;

    cfiber_reactor_t* inner = make_reactor();
    if (!inner) {
        return;
    }
    (void)cfiber_reactor_spawn(inner, nested_inner_fiber, st, nullptr);
    st->inner_rc = cfiber_reactor_run(inner);
    cfiber_reactor_destroy(inner);

    st->outer_after = cfiber_ev_sleep((uint64_t)ms_to_ns(1)); /* needs the outer current again */
}

static int test_nested_reactor(void) {
    static nested_reactor_state st;
    memset(&st, 0, sizeof st);
    st.inner_rc = -2;
    st.reentry_rc = -2;
    st.inner_st = CFIBER_EV_ERROR;
    st.outer_after = CFIBER_EV_ERROR;

    st.outer = make_reactor();
    ASSERT_NOT_NULL(st.outer);
    ASSERT_TRUE(cfiber_reactor_spawn(st.outer, nested_outer_fiber, &st, nullptr));

    ASSERT_EQ_U32(cfiber_reactor_run(st.outer), 0);

    ASSERT_EQ_U32((uint32_t)st.reentry_rc, (uint32_t)-1);
    ASSERT_EQ_U32(st.reentry_errno, EBUSY);
    ASSERT_EQ_U32(st.inner_rc, 0);
    ASSERT_EQ_U32(st.inner_st, CFIBER_EV_TIMEOUT);
    ASSERT_EQ_U32(st.outer_after, CFIBER_EV_TIMEOUT);

    cfiber_reactor_destroy(st.outer);
    return 0;
}

/* ============================================================================
 * wake semantics: only an async park is resumed; early wakes are kept
 * ============================================================================ */

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

typedef struct {
    cfiber_reactor_handle_t self;
    ssize_t read_rc; /* timed read while a wake arrives: must time out */
    int read_errno;
    cfiber_ev_status_t async_st; /* the wake is kept for this wait */
    int64_t async_ms;            /* how long that wait took */
} wake_fd_state;

static wake_fd_state g_wfd;
static int g_wfd_fd;

static void wake_fd_target_fiber(void* arg) {
    (void)arg;
    g_wfd.self = cfiber_ev_self();
    char buf[8];
    g_wfd.read_rc = cfiber_ev_read_timed(g_wfd_fd, buf, sizeof buf, ms_to_ns(30));
    g_wfd.read_errno = errno;

    const int64_t t0 = mono_ms();
    g_wfd.async_st = cfiber_ev_wait_async(ms_to_ns(500));
    g_wfd.async_ms = mono_ms() - t0;
}

static void wake_fd_waker_fiber(void* arg) {
    (void)arg;
    cfiber_ev_yield(); /* let the target park on the descriptor */
    cfiber_ev_wake(g_wfd.self);
}

static int test_wake_does_not_resume_fd_park(void) {
    memset(&g_wfd, 0, sizeof g_wfd);
    g_wfd.read_rc = -2;
    g_wfd.async_st = CFIBER_EV_ERROR;
    g_wfd.async_ms = -1;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);
    g_wfd_fd = fds[0];

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, wake_fd_target_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, wake_fd_waker_fiber, nullptr, nullptr));

    cfiber_reactor_run(r);

    /* the read ran to its own deadline, untouched by the wake */
    ASSERT_EQ_U32((uint32_t)(int)g_wfd.read_rc, (uint32_t)-1);
    ASSERT_EQ_U32(g_wfd.read_errno, ETIMEDOUT);
    /* and the wake was waiting for the async park */
    ASSERT_EQ_U32(g_wfd.async_st, CFIBER_EV_READY);
    ASSERT_TRUE(g_wfd.async_ms < 100);

    close(fds[0]);
    close(fds[1]);
    cfiber_reactor_destroy(r);
    return 0;
}

typedef struct {
    cfiber_reactor_handle_t self;
    cfiber_ev_status_t sleep_st;
    int64_t sleep_ms;
    cfiber_ev_status_t async_st;
} wake_sleep_state;

static wake_sleep_state g_wsl;

static void wake_sleep_target_fiber(void* arg) {
    (void)arg;
    g_wsl.self = cfiber_ev_self();
    const int64_t t0 = mono_ms();
    g_wsl.sleep_st = cfiber_ev_sleep((uint64_t)ms_to_ns(30));
    g_wsl.sleep_ms = mono_ms() - t0;
    g_wsl.async_st = cfiber_ev_wait_async(ms_to_ns(500)); /* satisfied by the kept wake */
}

static void wake_sleep_waker_fiber(void* arg) {
    (void)arg;
    cfiber_ev_yield();
    cfiber_ev_wake(g_wsl.self);
}

static int test_wake_does_not_cut_sleep(void) {
    memset(&g_wsl, 0, sizeof g_wsl);
    g_wsl.sleep_st = CFIBER_EV_ERROR;
    g_wsl.async_st = CFIBER_EV_ERROR;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, wake_sleep_target_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, wake_sleep_waker_fiber, nullptr, nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_wsl.sleep_st, CFIBER_EV_TIMEOUT);
    ASSERT_TRUE(g_wsl.sleep_ms >= 25);
    ASSERT_EQ_U32(g_wsl.async_st, CFIBER_EV_READY);

    cfiber_reactor_destroy(r);
    return 0;
}

/* A wake posted while the target is merely ready (not yet parked) is kept. */
static cfiber_reactor_handle_t g_early_target;
static cfiber_ev_status_t g_early_st;
static int64_t g_early_ms;

static void early_wake_target_fiber(void* arg) {
    (void)arg;
    g_early_target = cfiber_ev_self();
    cfiber_ev_yield(); /* the waker runs now, before we park */
    const int64_t t0 = mono_ms();
    g_early_st = cfiber_ev_wait_async(ms_to_ns(500));
    g_early_ms = mono_ms() - t0;
}

static void early_waker_fiber(void* arg) {
    (void)arg;
    cfiber_ev_wake(g_early_target);
}

static int test_wake_before_wait_async_is_kept(void) {
    g_early_st = CFIBER_EV_ERROR;
    g_early_ms = -1;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, early_wake_target_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, early_waker_fiber, nullptr, nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_early_st, CFIBER_EV_READY);
    ASSERT_TRUE(g_early_ms < 100);

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * deadlines: total for _timed helpers, saturating arithmetic
 * ============================================================================ */

#define SLOW_WRITE_TOTAL ((size_t)8 << 20)
#define SLOW_DRAIN_CHUNK ((size_t)256 << 10) /* empties the socket buffer in one read */

typedef struct {
    int wfd;
    int rfd;
    ssize_t write_rc;
    int write_errno;
    int64_t write_ms;
    volatile int writer_done;
} slow_drain_state;

static slow_drain_state g_slow;

static void slow_drain_writer_fiber(void* arg) {
    slow_drain_state* st = arg;
    static char big[SLOW_WRITE_TOTAL];
    const int64_t t0 = mono_ms();
    st->write_rc = cfiber_ev_write_timed(st->wfd, big, sizeof big, ms_to_ns(100));
    st->write_errno = errno;
    st->write_ms = mono_ms() - t0;
    st->writer_done = 1;
}

/* Empties the socket buffer every 40 ms, so the writer becomes writable well
 * inside a per-retry timeout of 100 ms and progresses a buffer per tick; only
 * a total deadline ends the write before the seconds the whole 8 MiB take. */
static void slow_drain_reader_fiber(void* arg) {
    slow_drain_state* st = arg;
    static char buf[SLOW_DRAIN_CHUNK];
    while (!st->writer_done) {
        (void)cfiber_ev_sleep((uint64_t)ms_to_ns(40));
        (void)read(st->rfd, buf, sizeof buf);
    }
}

static int test_timed_write_total_deadline(void) {
    memset(&g_slow, 0, sizeof g_slow);
    g_slow.write_rc = -2;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);
    g_slow.wfd = fds[0];
    g_slow.rfd = fds[1];

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, slow_drain_writer_fiber, &g_slow, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, slow_drain_reader_fiber, &g_slow, nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32((uint32_t)(int)g_slow.write_rc, (uint32_t)-1);
    ASSERT_EQ_U32(g_slow.write_errno, ETIMEDOUT);
    ASSERT_TRUE(g_slow.write_ms >= 90);
    ASSERT_TRUE(g_slow.write_ms < 800); /* per-retry semantics would take seconds */
    ASSERT_TRUE(g_slow.write_rc == -1); /* and not a completed 8 MiB */

    close(fds[0]);
    close(fds[1]);
    cfiber_reactor_destroy(r);
    return 0;
}

static cfiber_reactor_handle_t g_far_target;
static cfiber_ev_status_t g_far_sleep_st;
static cfiber_ev_status_t g_far_async_st;

static void far_deadline_fiber(void* arg) {
    (void)arg;
    g_far_target = cfiber_ev_self();
    g_far_sleep_st = cfiber_ev_sleep(UINT64_MAX);     /* would wrap: must not elapse */
    g_far_async_st = cfiber_ev_wait_async(INT64_MAX); /* same */
}

static void far_canceller_fiber(void* arg) {
    (void)arg;
    for (int i = 0; i < 2; i++) {
        (void)cfiber_ev_sleep((uint64_t)ms_to_ns(10));
        cfiber_ev_cancel(g_far_target);
    }
}

static int test_far_deadlines_do_not_wrap(void) {
    g_far_sleep_st = CFIBER_EV_ERROR;
    g_far_async_st = CFIBER_EV_ERROR;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, far_deadline_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, far_canceller_fiber, nullptr, nullptr));

    cfiber_reactor_run(r);

    /* both waits lasted until the cancel, neither timed out on the spot */
    ASSERT_EQ_U32(g_far_sleep_st, CFIBER_EV_CANCELLED);
    ASSERT_EQ_U32(g_far_async_st, CFIBER_EV_CANCELLED);

    cfiber_reactor_destroy(r);
    return 0;
}

#ifdef NDEBUG
/* Release builds: the in-fiber API off the loop thread fails, not faults. */
static int test_in_fiber_api_off_loop_thread(void) {
    errno = 0;
    ASSERT_EQ_U32(cfiber_ev_wait_async(0), CFIBER_EV_ERROR);
    ASSERT_EQ_U32(errno, EINVAL);
    errno = 0;
    ASSERT_EQ_U32(cfiber_ev_sleep(0), CFIBER_EV_ERROR);
    ASSERT_EQ_U32(errno, EINVAL);
    errno = 0;
    ASSERT_EQ_U32(cfiber_ev_wait(0, CFIBER_EV_IN, 0), CFIBER_EV_ERROR);
    ASSERT_EQ_U32(errno, EINVAL);
    errno = 0;
    ASSERT_FALSE(cfiber_ev_spawn(trivial_fiber, nullptr, nullptr));
    ASSERT_EQ_U32(errno, EINVAL);
    ASSERT_NULL(cfiber_ev_self().f);
    cfiber_ev_yield(); /* no-op */
    return 0;
}
#endif

/* ============================================================================
 * direction bits: only CFIBER_EV_IN / CFIBER_EV_OUT are accepted
 * ============================================================================ */

typedef struct {
    int fd;
    cfiber_ev_status_t none_st;
    int none_errno;
    cfiber_ev_status_t stray_st;
    int stray_errno;
    cfiber_ev_status_t both_st; /* IN|OUT on a writable socket: ready at once */
} direction_state;

static direction_state g_dir;

static void direction_fiber(void* arg) {
    direction_state* st = arg;
    st->none_st = cfiber_ev_wait(st->fd, 0, -1);
    st->none_errno = errno;
    st->stray_st = cfiber_ev_wait(st->fd, CFIBER_EV_IN | (1u << 31), -1);
    st->stray_errno = errno;
    st->both_st = cfiber_ev_wait(st->fd, CFIBER_EV_IN | CFIBER_EV_OUT, ms_to_ns(500));
}

static int test_direction_bits_validated(void) {
    memset(&g_dir, 0, sizeof g_dir);
    g_dir.none_st = CFIBER_EV_READY;
    g_dir.stray_st = CFIBER_EV_READY;
    g_dir.both_st = CFIBER_EV_ERROR;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);
    g_dir.fd = fds[0];

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, direction_fiber, &g_dir, nullptr));
    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_dir.none_st, CFIBER_EV_ERROR);
    ASSERT_EQ_U32(g_dir.none_errno, EINVAL);
    ASSERT_EQ_U32(g_dir.stray_st, CFIBER_EV_ERROR);
    ASSERT_EQ_U32(g_dir.stray_errno, EINVAL);
    ASSERT_EQ_U32(g_dir.both_st, CFIBER_EV_READY);

    close(fds[0]);
    close(fds[1]);
    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * loop fairness: yield loops must not starve timers, I/O or commands
 * ============================================================================ */

/* A spins on cfiber_ev_yield until B, asleep on a timer, sets the flag. */
static volatile int g_spin_flag;
static int g_spin_iters;

static void yield_spinner_fiber(void* arg) {
    (void)arg;
    while (!g_spin_flag) {
        cfiber_ev_yield();
        g_spin_iters++;
    }
}

static void flag_after_sleep_fiber(void* arg) {
    (void)arg;
    (void)cfiber_ev_sleep((uint64_t)ms_to_ns(10));
    g_spin_flag = 1;
}

static int test_yield_spin_lets_timer_fire(void) {
    g_spin_flag = 0;
    g_spin_iters = 0;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, yield_spinner_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, flag_after_sleep_fiber, nullptr, nullptr));

    cfiber_reactor_run(r); /* hangs forever if the timer never fires */

    ASSERT_EQ_U32(g_spin_flag, 1);
    ASSERT_TRUE(g_spin_iters > 0); /* the spinner really yielded */

    cfiber_reactor_destroy(r);
    return 0;
}

/* Two fibers ping-pong while a third is parked on a descriptor whose peer
 * writes from a sleeping fiber: the read must complete. */
static volatile int g_pp_done;
static ssize_t g_pp_read_n;

static void pingpong_fiber(void* arg) {
    (void)arg;
    while (!g_pp_done) {
        cfiber_ev_yield();
    }
}

static void pp_reader_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8];
    g_pp_read_n = cfiber_ev_read(fd, buf, sizeof buf);
    g_pp_done = 1;
    close(fd);
}

static void pp_writer_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    (void)cfiber_ev_sleep((uint64_t)ms_to_ns(10));
    (void)write(fd, "x", 1);
    close(fd);
}

static int test_pingpong_does_not_starve_io(void) {
    g_pp_done = 0;
    g_pp_read_n = -2;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, pingpong_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, pingpong_fiber, nullptr, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, pp_reader_fiber, (void*)(intptr_t)fds[0], nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, pp_writer_fiber, (void*)(intptr_t)fds[1], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_pp_read_n, 1);

    cfiber_reactor_destroy(r);
    return 0;
}

/* Nobody drains the ring while the loop is not running: posting must stop
 * with EAGAIN instead of spinning, and work again once the loop has drained. */
static int test_post_backpressure(void) {
    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);

    cfiber_reactor_handle_t h;
    ASSERT_TRUE(cfiber_reactor_spawn(r, trivial_fiber, nullptr, &h));

    int posted = 0;
    errno = 0;
    while (cfiber_reactor_wake(r, h)) {
        posted++;
        ASSERT_TRUE(posted < (1 << 16)); /* a full ring must be reported */
    }
    ASSERT_EQ_U32(errno, EAGAIN);
    ASSERT_TRUE(posted > 0);

    cfiber_reactor_run(r); /* drains the ring; the wakes are no-ops on a ready fiber */

    ASSERT_TRUE(cfiber_reactor_wake(r, h)); /* stale handle, but the post itself succeeds */

    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * descriptor ownership: handoff, close-and-reuse, peer close, close of a
 * parked descriptor
 * ============================================================================ */

/* A does the first read on fd and hands the descriptor to a fiber it spawns,
 * then exits. B's wait must take over A's registration, not fail with EEXIST. */
typedef struct {
    int fd;
    ssize_t first_n;  /* A's read */
    ssize_t second_n; /* B's read */
    int second_errno;
    char second[8];
} handoff_state_t;

static handoff_state_t g_handoff;

static void handoff_second_fiber(void* arg) {
    handoff_state_t* h = arg;
    h->second_n = cfiber_ev_read(h->fd, h->second, sizeof h->second);
    h->second_errno = errno;
    close(h->fd);
}

static void handoff_first_fiber(void* arg) {
    handoff_state_t* h = arg;
    char buf[8];
    h->first_n = cfiber_ev_read(h->fd, buf, sizeof buf); /* parks, then wakes */
    (void)cfiber_ev_spawn(handoff_second_fiber, h, nullptr);
    /* A exits while its registration for h->fd is still in the poller. */
}

static void handoff_writer_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    cfiber_ev_yield(); /* let A park */
    (void)cfiber_ev_write(fd, "one", 3);
    (void)cfiber_ev_sleep((uint64_t)ms_to_ns(10)); /* let A finish and B park */
    (void)cfiber_ev_write(fd, "two", 3);
    close(fd);
}

static int test_fd_handoff(void) {
    memset(&g_handoff, 0, sizeof g_handoff);
    g_handoff.second_n = -2;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);
    g_handoff.fd = fds[0];

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, handoff_first_fiber, &g_handoff, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, handoff_writer_fiber, (void*)(intptr_t)fds[1], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_handoff.first_n, 3);
    ASSERT_EQ_U32(g_handoff.second_n, 3);
    ASSERT_TRUE(memcmp(g_handoff.second, "two", 3) == 0);

    cfiber_reactor_destroy(r);
    return 0;
}

/* One fiber parks on a pair, closes it, opens a new pair that gets the same
 * descriptor numbers, and parks again. The kernel dropped the registration on
 * close; the second wait must not fail with ENOENT. */
typedef struct {
    int peer;     /* the writer's end for the current round */
    int reused;   /* second pair reused the first pair's numbers */
    ssize_t n[2]; /* bytes read per round */
    int err[2];
} reuse_state_t;

static reuse_state_t g_reuse;

static void reuse_reader_fiber(void* arg) {
    reuse_state_t* st = arg;
    int first[2];
    if (make_pair(first) < 0) {
        return;
    }
    char buf[8];
    st->peer = first[1];
    st->n[0] = cfiber_ev_read(first[0], buf, sizeof buf); /* parks */
    st->err[0] = errno;
    close(first[0]);
    close(first[1]);

    int second[2];
    if (make_pair(second) < 0) {
        return;
    }
    st->reused = second[0] == first[0] && second[1] == first[1];
    st->peer = second[1];
    st->n[1] = cfiber_ev_read(second[0], buf, sizeof buf); /* parks on the reused number */
    st->err[1] = errno;
    close(second[0]);
    close(second[1]);
}

static void reuse_writer_fiber(void* arg) {
    reuse_state_t* st = arg;
    for (int round = 0; round < 2; round++) {
        (void)cfiber_ev_sleep((uint64_t)ms_to_ns(10)); /* let the reader park */
        (void)write(st->peer, "x", 1);
    }
}

static int test_fd_close_and_reuse(void) {
    memset(&g_reuse, 0, sizeof g_reuse);
    g_reuse.n[0] = -2;
    g_reuse.n[1] = -2;

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, reuse_reader_fiber, &g_reuse, nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, reuse_writer_fiber, &g_reuse, nullptr));

    cfiber_reactor_run(r);

    ASSERT_TRUE(g_reuse.reused); /* lowest free numbers: the scenario is real */
    ASSERT_EQ_U32(g_reuse.n[0], 1);
    ASSERT_EQ_U32(g_reuse.n[1], 1);

    cfiber_reactor_destroy(r);
    return 0;
}

/* Peer closes without writing: the parked reader wakes and read returns 0. */
static ssize_t g_hup_n;

static void hup_reader_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8];
    g_hup_n = cfiber_ev_read(fd, buf, sizeof buf);
    close(fd);
}

static void hup_closer_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    cfiber_ev_yield(); /* let the reader park */
    close(fd);
}

static int test_peer_close_reads_zero(void) {
    g_hup_n = -2;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, hup_reader_fiber, (void*)(intptr_t)fds[0], nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, hup_closer_fiber, (void*)(intptr_t)fds[1], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_hup_n, 0);

    cfiber_reactor_destroy(r);
    return 0;
}

/* Another fiber closes the descriptor a reader is parked on through
 * cfiber_ev_close: the reader resumes with ECANCELED, well before its deadline
 * (which only bounds the test if the cancel is lost). */
static ssize_t g_closed_n;
static int g_closed_errno;
static int g_closed_rc;

static void closed_reader_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8];
    g_closed_n = cfiber_ev_read_timed(fd, buf, sizeof buf, ms_to_ns(500));
    g_closed_errno = errno;
}

static void closer_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    cfiber_ev_yield(); /* let the reader park */
    g_closed_rc = cfiber_ev_close(fd);
}

static int test_close_parked_fd(void) {
    g_closed_n = -2;
    g_closed_errno = 0;
    g_closed_rc = -2;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, closed_reader_fiber, (void*)(intptr_t)fds[0], nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, closer_fiber, (void*)(intptr_t)fds[0], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_closed_rc, 0);
    ASSERT_EQ_U32((uint32_t)(int)g_closed_n, (uint32_t)-1);
    ASSERT_EQ_U32(g_closed_errno, ECANCELED);

    close(fds[1]);
    cfiber_reactor_destroy(r);
    return 0;
}

/* A second fiber waiting on a descriptor another fiber is parked on is
 * refused with EBUSY rather than silently stealing the wait. */
static cfiber_ev_status_t g_busy_st;
static int g_busy_errno;
static ssize_t g_busy_first_n;

static void busy_first_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8];
    g_busy_first_n = cfiber_ev_read(fd, buf, sizeof buf);
}

static void busy_second_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    cfiber_ev_yield(); /* let the first fiber park */
    g_busy_st = cfiber_ev_wait(fd, CFIBER_EV_IN, -1);
    g_busy_errno = errno;
}

static void busy_writer_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;
    (void)cfiber_ev_sleep((uint64_t)ms_to_ns(10));
    (void)write(fd, "x", 1);
    close(fd);
}

static int test_second_waiter_is_busy(void) {
    g_busy_st = CFIBER_EV_READY;
    g_busy_errno = 0;
    g_busy_first_n = -2;

    int fds[2];
    ASSERT_EQ_U32(make_pair(fds), 0);

    cfiber_reactor_t* r = make_reactor();
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(cfiber_reactor_spawn(r, busy_first_fiber, (void*)(intptr_t)fds[0], nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, busy_second_fiber, (void*)(intptr_t)fds[0], nullptr));
    ASSERT_TRUE(cfiber_reactor_spawn(r, busy_writer_fiber, (void*)(intptr_t)fds[1], nullptr));

    cfiber_reactor_run(r);

    ASSERT_EQ_U32(g_busy_st, CFIBER_EV_ERROR);
    ASSERT_EQ_U32(g_busy_errno, EBUSY);
    ASSERT_EQ_U32(g_busy_first_n, 1); /* the parked fiber kept its wait */

    close(fds[0]);
    cfiber_reactor_destroy(r);
    return 0;
}

/* ============================================================================
 * create / run / destroy churn (leak balance under ASan)
 * ============================================================================ */


static int test_create_destroy_churn(void) {
    for (int i = 0; i < 32; i++) {
        cfiber_reactor_t* r = make_reactor();
        ASSERT_NOT_NULL(r);
        ASSERT_TRUE(cfiber_reactor_spawn(r, trivial_fiber, nullptr, nullptr));
        ASSERT_TRUE(cfiber_reactor_spawn(r, trivial_fiber, nullptr, nullptr));
        cfiber_reactor_run(r);
        cfiber_reactor_destroy(r);
    }
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("epoll reactor");
    signal(SIGPIPE, SIG_IGN); /* a write to a closed peer must fail, not kill the run */

    RUN_TEST(test_spawn_yield_completion);
    RUN_TEST(test_fd_park_wake);
    RUN_TEST(test_timer_ordering);
    RUN_TEST(test_wait_timeout);
    RUN_TEST(test_cancel_parked);
    RUN_TEST(test_cross_thread_wake);
    RUN_TEST(test_cross_thread_cancel);
    RUN_TEST(test_inproc_wake_cancel);
    RUN_TEST(test_ring_stress);
    RUN_TEST(test_concurrency_stress);
    RUN_TEST(test_destroy_without_run);
    RUN_TEST(test_run_reports_poller_failure);
    RUN_TEST(test_run_twice);
    RUN_TEST(test_nested_reactor);
    RUN_TEST(test_direction_bits_validated);
    RUN_TEST(test_wake_does_not_resume_fd_park);
    RUN_TEST(test_wake_does_not_cut_sleep);
    RUN_TEST(test_wake_before_wait_async_is_kept);
    RUN_TEST(test_timed_write_total_deadline);
    RUN_TEST(test_far_deadlines_do_not_wrap);
#ifdef NDEBUG
    RUN_TEST(test_in_fiber_api_off_loop_thread);
#endif
    RUN_TEST(test_yield_spin_lets_timer_fire);
    RUN_TEST(test_pingpong_does_not_starve_io);
    RUN_TEST(test_post_backpressure);
    RUN_TEST(test_fd_handoff);
    RUN_TEST(test_fd_close_and_reuse);
    RUN_TEST(test_peer_close_reads_zero);
    RUN_TEST(test_close_parked_fd);
    RUN_TEST(test_second_waiter_is_busy);
    RUN_TEST(test_create_destroy_churn);

    return cfiber_test_report();
}
