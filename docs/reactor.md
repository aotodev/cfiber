# The epoll reactor (optional, Linux)

`CFIBER_REACTOR=ON` builds `libcfiber_reactor`, a Linux-only `epoll(7)` event
loop that multiplexes many fibers onto one thread. Header:
[include/cfiber/reactor/reactor.h](../include/cfiber/reactor/reactor.h).

It is an alternative scheduler, not a layer on top of the built-in one: it uses
cfiber's fiber and growable-stack mechanism directly, and registers its own
fiber-return hook at run time via `cfiber_set_return_hook()` while it runs, so it
coexists with the built-in scheduler in one process and works through the shared
library. It is excluded from freestanding builds.

```sh
cmake -B build -DCFIBER_REACTOR=ON
# or:  ./utils/make.sh -t --reactor          # build + run the reactor tests
```

## The idea

A fiber that would block on a non-blocking descriptor instead *parks*: it
registers interest with the poller and yields. The loop resumes it when the
descriptor is ready, a deadline elapses, or the fiber is cancelled. Because the
fiber owns a real stack, the parking happens wherever the blocking call was,
with no colouring of the functions above it.

```c
static void echo(void* arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    for (;;) {
        ssize_t n = cfiber_ev_read(fd, buf, sizeof buf);
        if (n <= 0) break;
        if (cfiber_ev_write(fd, buf, (size_t)n) < 0) break;
    }
    close(fd);
}

static void acceptor(void* arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = cfiber_ev_accept(lfd, nullptr, nullptr); /* already non-blocking */
        if (fd < 0) break;
        cfiber_ev_spawn(echo, (void*)(intptr_t)fd, nullptr);
    }
}

/* setup: lfd is a bound, listening, non-blocking socket */
cfiber_reactor_t* r = cfiber_reactor_create((cfiber_reactor_config_t){
    .stack_size  = 64 * 1024,
    .stack_cache = 256,
});
cfiber_reactor_spawn(r, acceptor, (void*)(intptr_t)lfd, nullptr);
cfiber_reactor_run(r);
cfiber_reactor_destroy(r);
```

`stack_size` is per fiber and rounded up to a page; the stacks are growable, so
this is a ceiling rather than a commitment. `stack_cache` is how many stacks the
loop keeps pooled for reuse instead of returning to the OS.

## One primitive, many transports

```c
cfiber_ev_status_t cfiber_ev_wait(int fd, uint32_t direction, int64_t timeout_ns);
```

Read it as: park the current fiber until `fd` is ready in `direction`
(`CFIBER_EV_IN`, `CFIBER_EV_OUT` or both), the timeout elapses, or the fiber is
cancelled. The result is `CFIBER_EV_READY`, `CFIBER_EV_TIMEOUT`,
`CFIBER_EV_CANCELLED`, or `CFIBER_EV_ERROR` with `errno` set. The direction
bits are the reactor's own, not epoll's: nothing Linux-specific is in the API,
and a stray poller flag is refused with `EINVAL` rather than silently changing
the re-arm protocol.

Everything else is layered on it. `cfiber_ev_read` / `_write` / `_accept` /
`_connect` are a thin POSIX byte-stream *transport*: they attempt the syscall,
and on `EAGAIN` call `cfiber_ev_wait` with the appropriate direction and retry.
They keep the return conventions of the syscalls they wrap, returning `-1` with
`errno == ECANCELED` on cancellation and, for the `_timed` variants, `ETIMEDOUT`
on deadline.

The loop never bakes in a direction or a protocol, so a different transport
(TLS, UDP, a message framing layer) is a different helper layer over the same
primitive rather than a change to the reactor.

## API

Two prefixes, one rule: `cfiber_reactor_*` takes the reactor and is the
host-side API (lifecycle from the owning thread, wake and cancel from any
thread); `cfiber_ev_*` is the in-fiber API, acting on the reactor that runs the
calling fiber. Where both exist, spawn, wake and cancel, they do the same thing
from the two sides.

| Call | Where from | Purpose |
| ---- | ---------- | ------- |
| `cfiber_reactor_create` / `_destroy` | host thread | Lifecycle |
| `cfiber_reactor_spawn` | host thread | Seed fibers before a run, or between runs |
| `cfiber_reactor_run` | host thread | Run until every fiber completes |
| `cfiber_reactor_wake` / `_cancel` | any thread | Resume or cancel a parked fiber |
| `cfiber_ev_spawn` | fiber | Spawn on the running reactor |
| `cfiber_ev_self` | fiber | Handle to the running fiber |
| `cfiber_ev_wake` / `_cancel` | fiber | Same-thread fast paths |
| `cfiber_ev_yield` | fiber | Yield to other ready fibers |
| `cfiber_ev_wait` | fiber | The core primitive |
| `cfiber_ev_sleep` | fiber | Park for a duration |
| `cfiber_ev_wait_async` | fiber | Park until woken, with no descriptor |
| `cfiber_ev_read` / `_write` / `_accept` / `_connect` | fiber | POSIX transport |
| `cfiber_ev_read_timed` / `_write_timed` | fiber | The same with a deadline |
| `cfiber_ev_close` | fiber | Close, cancelling a parked waiter first |
| `cfiber_ev_set_nonblocking` | anywhere | Utility |

`cfiber_ev_accept` hands back descriptors that are already non-blocking and
close-on-exec, so nothing needs to be done to them before spawning a fiber.

## Scheduling

The loop runs the fibers that are ready at the start of a round, then polls.
The poll has a zero timeout while anything is runnable and blocks only when
nothing is, so fibers that yield to each other cannot starve I/O, timers or
cross-thread commands, and `cfiber_ev_yield()` always returns to the loop even
when the caller is the only fiber. A `while (!flag) cfiber_ev_yield();` loop
therefore burns a core but does make progress.

## Timers

Deadlines go into a monotonic min-heap, and the nearest one becomes the
`epoll_wait` timeout. There is no timerfd and no per-timer descriptor, so a
deadline costs a heap entry rather than a kernel object, and `cfiber_ev_sleep`
and the `_timed` variants are the same mechanism with different entry points.
The `_timed` helpers treat their argument as a total deadline for the call,
across every partial read or write it takes. Deadline arithmetic saturates, so
`INT64_MAX` is a practical "never".

## Waking and cancelling across threads

`cfiber_reactor_wake()` and `cfiber_reactor_cancel()` are the only calls safe
from another thread. They post to a bounded lock-free ring and poke an `eventfd`
to break the loop out of `epoll_wait`; issued from the loop thread itself they
are applied directly instead. From within a fiber, `cfiber_ev_wake()` and
`cfiber_ev_cancel()` skip the ring entirely.

The ring is drained once per loop round, so commands only take effect while
`cfiber_reactor_run()` is running. If it fills (1024 outstanding commands, the
loop not running or stuck in a fiber that never yields), a post returns `false`
with `errno == EAGAIN` after a short bounded retry rather than blocking the
caller; nothing is queued and the caller decides whether to retry.

Cancelling a parked fiber resumes it with `CFIBER_EV_CANCELLED`, and an
in-flight transport helper turns that into `-1` with `errno == ECANCELED`, so
cancellation surfaces as an ordinary error return at the call site rather than
as an unwind.

A wake is narrower: it completes a `cfiber_ev_wait_async()` and nothing else. A
fiber parked on a descriptor or a timer is not disturbed by it; the wake is kept
as a permit and the fiber's next `wait_async` returns immediately. That is what
makes the usual pattern safe: hand work to a thread, read from a socket in the
meantime, then `wait_async` for the result, without the thread's wake being
consumed by the read. Wakes do not accumulate, so the permit is a flag, not a
counter.

`cfiber_reactor_handle_t` is a generation-tagged reference into an
address-stable fiber pool (cfiber's `multislab`). Waking or cancelling a fiber
that has already completed compares generations, finds them different, and does
nothing. A handle is therefore safe to hold for the reactor's lifetime, which is
what makes cross-thread control possible at all without a lock around the
fiber's lifetime. The handle does not extend that lifetime: destroy the reactor
only after every thread that might still wake or cancel through it has stopped.

## Lifecycle

`cfiber_reactor_run()` returns 0 once every fiber has completed and can be
called again, for example to run fibers spawned after the first run, or from
inside a fiber of another scheduler or reactor, which is current again when the
nested run returns. It returns -1 with `errno` if the poller fails or the
reactor is run from one of its own fibers; the fibers it did not finish stay
live. `cfiber_reactor_destroy()`
tears those down without resuming them: stacks and fiber records are released,
but a descriptor or heap block the fiber itself owned is not, so prefer
cancelling and letting the run complete when that matters.

## Descriptor ownership

One fiber waits on a given descriptor at a time; a second waiter gets
`CFIBER_EV_ERROR` with `EBUSY`. Between waits a descriptor moves freely: the
fiber that did the handshake can hand the socket to a fiber it spawns and exit.
The poller registration follows the waiter and is dropped when a fiber
finishes, so nothing is released by hand.

Closing needs care. The kernel drops a registration on the last `close()`,
silently, so a plain `close()` of a descriptor another fiber is parked on
leaves that fiber parked forever. Use `cfiber_ev_close()` when a waiter may
exist: it cancels the waiter, which sees `ECANCELED`, then closes. A descriptor
only its own fiber ever waited on needs just `close()`.

## Scope

One reactor per thread, the single-loop-per-core model. The full-duplex reader
plus writer split (two fibers on one fd) and non-POSIX transports are future
work that the `cfiber_ev_wait` primitive is designed to accommodate.

## Example and testing

A standalone WebSocket (RFC 6455) echo server built on the reactor lives in
[examples/ws_echo](../examples/ws_echo), built with the reactor when the
examples or the tests are enabled. It
covers the handshake, frame parsing, fragmentation and control frames, and ships
with a self-test client.

The reactor has its own CI job, its own libFuzzer target, and is the reason
`CFIBER_TSAN` exists: the cross-thread ring and the eventfd wakeup are the only
concurrent code in the project. See [testing.md](testing.md).
