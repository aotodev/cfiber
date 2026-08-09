/**
 * @file  reactor.h
 * @brief Optional, Linux-only epoll(7) reactor for cfiber fibers.
 *
 * @details A cooperative scheduler that multiplexes many fibers onto one thread
 *          with an epoll event loop. A fiber that would block on a non-blocking
 *          file descriptor instead *parks* (it registers interest with the
 *          poller and yields), and the loop resumes it when the descriptor is
 *          ready, a deadline elapses, or it is cancelled.
 *
 *          Does not use the built-in FCFS scheduler. It registers its own
 *          fiber-return hook via cfiber_set_return_hook() for the duration of
 *          cfiber_reactor_run() and restores the previous one on return, so it
 *          coexists with other schedulers in one process.
 *
 * @section model Threading model
 *          One reactor per thread. The in-fiber API operates on the calling
 *          thread's running reactor implicitly. Only cfiber_reactor_wake() and
 *          cfiber_reactor_cancel() are safe to call from another thread;
 *          everything else must run on the loop thread (typically from within a
 *          fiber). One fiber owns a given descriptor at a time.
 *
 * @note Linux only (epoll, eventfd). Built only when the CFIBER_REACTOR option
 *       is enabled; excluded on freestanding targets.
 *
 * @see docs/reactor.md
 */

#ifndef CFIBER_REACTOR_H
#define CFIBER_REACTOR_H

#include "cfiber/core/macros.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque reactor instance. */
typedef struct cfiber_reactor cfiber_reactor_t;

/** @brief Fiber entry-point signature for reactor fibers. */
typedef void (*cfiber_reactor_fn)(void* arg);

/** @brief Internal fiber type; referenced only through cfiber_reactor_handle_t. */
struct cfiber_reactor_fiber;

/**
 * @brief Stable, opaque reference to a spawned fiber.
 * @details Remains valid for the fiber's whole lifetime and is safe to hold
 *          (and to pass to cfiber_reactor_wake() / cfiber_reactor_cancel()) even
 *          after the fiber has completed; operations on a stale handle are
 *          no-ops. Treat the fields as opaque.
 */
typedef struct {
    struct cfiber_reactor_fiber* f;
    uint64_t gen;
} cfiber_reactor_handle_t;

/**
 * @brief Outcome of a parking operation (cfiber_ev_wait and friends).
 */
typedef enum {
    CFIBER_EV_READY = 0,     /**< The descriptor is ready / the fiber was woken. */
    CFIBER_EV_TIMEOUT = 1,   /**< The supplied deadline elapsed first.           */
    CFIBER_EV_CANCELLED = 2, /**< The fiber was cancelled while parked.          */
    CFIBER_EV_ERROR = -1,    /**< Registration failed (see errno).               */
} cfiber_ev_status_t;

/** @brief Reactor construction parameters. */
typedef struct {
    /** Per-fiber stack size in bytes (rounded up to a page). */
    size_t stack_size;
    /** Number of growable stacks to keep pooled for reuse (0 -> a default). */
    size_t stack_cache;
} cfiber_reactor_config_t;

/* ============================================================================
 * Lifecycle (call from the host thread)
 * ============================================================================ */

/**
 * @brief Creates a reactor.
 * @return A new reactor, or NULL on failure (errno set).
 */
CFIBER_EXPORT cfiber_reactor_t* cfiber_reactor_create(cfiber_reactor_config_t config);

/** @brief Destroys a reactor and releases its resources. NULL-safe. */
CFIBER_EXPORT void cfiber_reactor_destroy(cfiber_reactor_t* r);

/**
 * @brief Spawns a fiber on @p r before (or during) the run loop.
 * @param out_handle If non-NULL, receives a stable handle to the new fiber.
 * @return true on success, false on allocation failure.
 */
CFIBER_EXPORT bool
cfiber_reactor_spawn(cfiber_reactor_t* r, cfiber_reactor_fn fn, void* arg, cfiber_reactor_handle_t* out_handle)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Runs the event loop until every fiber has completed.
 * @details Registers the reactor's fiber-return hook for the duration of the
 *          call and restores the previous hook on return. Blocks the calling
 *          thread.
 */
CFIBER_EXPORT void cfiber_reactor_run(cfiber_reactor_t* r) __attribute__((nonnull(1)));

/* ============================================================================
 * Cross-thread control (safe from any thread)
 * ============================================================================ */

/**
 * @brief Wakes a fiber parked in cfiber_ev_wait_async().
 * @details Thread-safe. The parked fiber resumes with CFIBER_EV_READY. A no-op
 *          if the handle is stale or the fiber is not parked async.
 */
CFIBER_EXPORT void cfiber_reactor_wake(cfiber_reactor_t* r, cfiber_reactor_handle_t h) __attribute__((nonnull(1)));

/**
 * @brief Cancels a parked fiber.
 * @details Thread-safe. A fiber parked in any cfiber_ev_* wait resumes with
 *          CFIBER_EV_CANCELLED (an in-flight transport helper returns -1 with
 *          errno == ECANCELED). A no-op if the handle is stale or the fiber is
 *          not currently parked.
 */
CFIBER_EXPORT void cfiber_reactor_cancel(cfiber_reactor_t* r, cfiber_reactor_handle_t h) __attribute__((nonnull(1)));

/* ============================================================================
 * In-fiber API (implicit current reactor; call only from a fiber)
 * ============================================================================ */

/** @brief Spawns a fiber on the running reactor. @see cfiber_reactor_spawn. */
CFIBER_EXPORT bool cfiber_ev_spawn(cfiber_reactor_fn fn, void* arg, cfiber_reactor_handle_t* out_handle)
    __attribute__((nonnull(1)));

/** @brief Returns a handle to the currently running fiber. */
CFIBER_EXPORT cfiber_reactor_handle_t cfiber_ev_self(void);

/**
 * @brief Same-thread fast paths for waking / cancelling a fiber from within
 *        another fiber on the same reactor.
 * @details Apply directly, skipping the cross-thread ring + eventfd round trip
 *          that cfiber_reactor_wake()/_cancel() use. Call only from the loop
 *          thread (i.e. from within a fiber). A no-op on a stale handle or a
 *          fiber that is not currently parked.
 */
CFIBER_EXPORT void cfiber_ev_wake(cfiber_reactor_handle_t h);
CFIBER_EXPORT void cfiber_ev_cancel(cfiber_reactor_handle_t h);

/** @brief Cooperatively yields to other ready fibers. */
CFIBER_EXPORT void cfiber_ev_yield(void);

/**
 * @brief The core primitive: parks until @p fd is ready, the timeout elapses,
 *        or the fiber is cancelled.
 * @param fd         A non-blocking file descriptor.
 * @param direction  EPOLLIN and/or EPOLLOUT.
 * @param timeout_ns Deadline in nanoseconds from now, or a negative value for no
 *                   timeout.
 * @return One of cfiber_ev_status_t.
 */
CFIBER_EXPORT cfiber_ev_status_t cfiber_ev_wait(int fd, uint32_t direction, int64_t timeout_ns);

/**
 * @brief Parks the fiber for @p ns nanoseconds.
 * @return CFIBER_EV_TIMEOUT on normal elapse, or CFIBER_EV_CANCELLED.
 */
CFIBER_EXPORT cfiber_ev_status_t cfiber_ev_sleep(uint64_t ns);

/**
 * @brief Parks the fiber until woken (cfiber_reactor_wake), the timeout elapses,
 *        or it is cancelled, without any descriptor.
 * @param timeout_ns Deadline in nanoseconds from now, or negative for none.
 */
CFIBER_EXPORT cfiber_ev_status_t cfiber_ev_wait_async(int64_t timeout_ns);

/* ---- POSIX byte-stream transport helpers (layered over cfiber_ev_wait) ----
 * Same return conventions as the underlying syscalls. On cancellation they
 * return -1 with errno == ECANCELED; the _timed variants return -1 with
 * errno == ETIMEDOUT when their deadline elapses first. */

CFIBER_EXPORT ssize_t cfiber_ev_read(int fd, void* buf, size_t n);
CFIBER_EXPORT ssize_t cfiber_ev_write(int fd, const void* buf, size_t n);
CFIBER_EXPORT int cfiber_ev_accept(int lfd, struct sockaddr* addr, socklen_t* alen);
CFIBER_EXPORT int cfiber_ev_connect(int fd, const struct sockaddr* addr, socklen_t alen);

CFIBER_EXPORT ssize_t cfiber_ev_read_timed(int fd, void* buf, size_t n, int64_t timeout_ns);
CFIBER_EXPORT ssize_t cfiber_ev_write_timed(int fd, const void* buf, size_t n, int64_t timeout_ns);

/** @brief Utility: put @p fd into non-blocking mode. */
CFIBER_EXPORT int cfiber_ev_set_nonblocking(int fd);

#ifdef __cplusplus
}
#endif

#endif /* CFIBER_REACTOR_H */
