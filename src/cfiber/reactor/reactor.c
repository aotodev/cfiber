/*
 * Linux epoll(7) reactor for cfiber fibers.
 *
 * Layering (top of file comment in reactor.h has the full rationale):
 *   - run loop + ready queue + park/wake bookkeeping  (the generic scheduler)
 *   - cfiber_ev_wait(fd, direction, timeout)          (the irreducible primitive)
 *   - cfiber_ev_read/_write/_accept/_connect          (POSIX transport helpers)
 *
 * A parked fiber may be waiting on, at most: one fd (registered EPOLLONESHOT in
 * the epoll set) and one deadline (an entry in the monotonic timer min-heap).
 * Whichever fires first, or a cancellation, resumes it; the resume path
 * detaches it from the other wait source so it is enqueued exactly once.
 *
 * Registrations outlive the wait: after a ready wake the one-shot is disarmed
 * but the entry stays, so the next wait on the same fd is a MOD, not a DEL +
 * ADD. fd_owner maps each registered fd to the fiber holding it, so a handoff
 * to another fiber takes the entry over, a finished fiber drops its entry, and
 * cfiber_ev_close can find the waiter to cancel.
 *
 * Threading: the loop, ready queue, epoll set, timer heap and fiber pool are
 * touched only by the loop thread. Cross-thread wake/cancel post a command to a
 * mutex-guarded queue and poke an eventfd; the loop drains and applies them. The
 * loop thread itself uses the same command path, so apply logic has a single
 * home and never reenters the running fiber.
 */
#include "cfiber/reactor/reactor.h"

#include "cfiber/core/macros.h"
#include "cfiber/debug/asan.h"
#include "cfiber/debug/tsan.h"
#include "cfiber/fiber/context.h"
#include "cfiber/fiber/fiber.h"
#include "cfiber/memory/multislab_alloc.h"
#include "cfiber/stack/growable_stack_allocator.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Types
 * ============================================================================ */

#define NO_TIMER ((size_t)-1)

/* Fiber structs come from a multislab pool: fixed-size blocks, O(1) alloc/
 * release, address-stable for the pool's lifetime. */
#define DEFAULT_FIBERS_PER_SLAB 32u

/* Timer heap: a contiguous binary heap grown geometrically, std::vector style
 * (start at INIT_CAP, multiply by GROWTH each time it fills). */
#define TIMER_HEAP_INIT_CAP ((size_t)16)
#define TIMER_HEAP_GROWTH 2

/* Cross-thread command ring: a fixed-capacity lock-free MPMC queue (Vyukov).
 * Must be a power of two. Drained once per loop round; a producer that finds
 * it full retries a few times, then fails with EAGAIN. */
#define CMD_RING_CAP ((size_t)1024)
#define POST_RETRIES 16

typedef enum {
    FB_FREE = 0, /* pooled, not in use                       */
    FB_READY,    /* on the ready queue                       */
    FB_RUNNING,  /* the current fiber                        */
    FB_PARKED,   /* blocked on fd / timer / async wakeup     */
    FB_ZOMBIE,   /* completed, awaiting stack release        */
} fiber_state_t;

/* What a parked fiber waits for. A wake resumes only an async park; cancel and
 * the fd/timer sources resume their own. */
typedef enum { PARK_FD, PARK_TIMER, PARK_ASYNC } park_kind_t;

struct cfiber_reactor_fiber {
    fiber_t fiber;  /* cfiber context + stack pointer/size        */
    cstack_t stack; /* backing growable stack (mmap + guard page) */

    struct cfiber_reactor_fiber* next; /* ready-queue / free-list link */

    /* Every spawned, not yet recycled fiber, so destroy can release the
     * stacks of fibers the loop never finished. */
    struct cfiber_reactor_fiber* live_next;
    struct cfiber_reactor_fiber* live_prev;

    fiber_state_t state;
    park_kind_t park;    /* valid while FB_PARKED */
    bool wake_pending;   /* a wake arrived outside an async park; the next
                          * cfiber_ev_wait_async returns at once */
    void* tsan;          /* ThreadSanitizer context, NULL without TSan */
    uint64_t generation; /* bumped on recycle; matches handle.gen. 64-bit so it
                          * cannot wrap within the reactor's lifetime, even under
                          * sustained high-churn reuse of a single block. */

    int reg_fd;     /* fd whose epoll registration this fiber holds, or -1 */
    int wait_fd;    /* fd currently parked on, or -1; a subset of reg_fd   */
    size_t timer_i; /* index into the timer heap, or NO_TIMER     */
    uint64_t deadline_ns;

    cfiber_ev_status_t wait_status; /* delivered to the resumed fiber */
};

typedef struct cfiber_reactor_fiber ev_fiber_t;

/* Cross-thread command: wake or cancel a parked fiber. Passed by value through
 * the ring (no per-command allocation). */
typedef enum { CMD_WAKE, CMD_CANCEL } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    ev_fiber_t* target;
    uint64_t gen;
} cmd_t;

/* Bounded lock-free MPMC ring (Vyukov). Any thread may enqueue; the loop thread
 * is the sole consumer. Each cell carries a sequence counter that sequences
 * producers and consumers without a lock. */
typedef struct {
    _Atomic size_t seq;
    cmd_t cmd;
} cmd_cell_t;

typedef struct {
    /* Producer and consumer cursors on their own cache lines; cells and mask
     * are read-only after init, so sharing the producer line costs nothing. */
    _Alignas(CACHE_LINE_SIZE) _Atomic size_t enqueue_pos;
    cmd_cell_t* cells;
    size_t mask; /* capacity - 1 (capacity is a power of two) */
    _Alignas(CACHE_LINE_SIZE) _Atomic size_t dequeue_pos;
} cmd_ring_t;

struct cfiber_reactor {
    /* Lock-free cross-thread command ring. A wake/cancel issued from the loop
     * thread itself (detected via the thread-local g_reactor) is applied
     * directly instead of being routed through the ring + eventfd. */
    cmd_ring_t cmds;

    context_t loop_ctx; /* the run loop's own (host) context */
    void* loop_tsan;    /* TSan context of the stack running the loop */
    ev_fiber_t* current;
    ev_fiber_t* zombie;

    ev_fiber_t* ready_head;
    ev_fiber_t* ready_tail;
    ev_fiber_t* live_head;

    /* Fiber-struct pool. Grow-only: empty slabs are retained (never returned to
     * the OS) because handles point into these blocks and may outlive their
     * fiber; a freed slab would dangle an outstanding handle. The slab's
     * bitmap free list leaves block contents intact, so a recycled block keeps
     * its generation counter. */
    multislab_t fibers;

    growable_stack_allocator_t* stacks;
    size_t stack_size;

    /* Timer min-heap keyed by deadline_ns. */
    ev_fiber_t** heap;
    size_t heap_len;
    size_t heap_cap;

    /* fd -> fiber holding its epoll registration; indexed by fd, grown on
     * demand. Detach and close act only on the current holder. */
    ev_fiber_t** fd_owner;
    size_t fd_owner_cap;

    int epfd;
    int evfd;    /* eventfd: cross-thread wakeup of the loop */
    int active;  /* live fibers: ready + running + parked */
    int blocked; /* parked fibers */
};

/* One reactor per thread; the in-fiber API finds it here. */
static thread_local cfiber_reactor_t* g_reactor;

/* ============================================================================
 * Time
 * ============================================================================ */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* Absolute deadline for a relative timeout. Saturates instead of wrapping;
 * UINT64_MAX is a deadline that never comes. */
#define NO_DEADLINE UINT64_MAX

static uint64_t deadline_after(uint64_t ns) {
    const uint64_t now = now_ns();
    return ns > NO_DEADLINE - now ? NO_DEADLINE : now + ns;
}

static uint64_t deadline_after_opt(int64_t timeout_ns) {
    return timeout_ns < 0 ? NO_DEADLINE : deadline_after((uint64_t)timeout_ns);
}

/* Time left to a deadline, in cfiber_ev_wait's timeout convention. */
static int64_t remaining_ns(uint64_t deadline) {
    if (deadline == NO_DEADLINE) {
        return -1;
    }
    const uint64_t now = now_ns();
    if (deadline <= now) {
        return 0;
    }
    const uint64_t left = deadline - now;
    return left > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)left;
}

/* ============================================================================
 * Ready queue (singly linked FIFO)
 * ============================================================================ */

static void enqueue(cfiber_reactor_t* s, ev_fiber_t* f) {
    f->next = nullptr;
    if (s->ready_tail) {
        s->ready_tail->next = f;
    } else {
        s->ready_head = f;
    }
    s->ready_tail = f;
    f->state = FB_READY;
}

static ev_fiber_t* dequeue(cfiber_reactor_t* s) {
    ev_fiber_t* f = s->ready_head;
    if (!f) {
        return nullptr;
    }
    s->ready_head = f->next;
    if (!s->ready_head) {
        s->ready_tail = nullptr;
    }
    f->next = nullptr;
    return f;
}

/* ============================================================================
 * Timer min-heap
 * ============================================================================ */

static bool heap_reserve(cfiber_reactor_t* s, size_t need) {
    if (need <= s->heap_cap) {
        return true;
    }
    size_t cap = s->heap_cap ? s->heap_cap * TIMER_HEAP_GROWTH : TIMER_HEAP_INIT_CAP;
    while (cap < need) {
        cap *= TIMER_HEAP_GROWTH;
    }
    ev_fiber_t** h = (ev_fiber_t**)realloc((void*)s->heap, cap * sizeof(*h));
    if (!h) {
        return false;
    }
    s->heap = h;
    s->heap_cap = cap;
    return true;
}

static void heap_swap(cfiber_reactor_t* s, size_t a, size_t b) {
    ev_fiber_t* tmp = s->heap[a];
    s->heap[a] = s->heap[b];
    s->heap[b] = tmp;
    s->heap[a]->timer_i = a;
    s->heap[b]->timer_i = b;
}

static void heap_sift_up(cfiber_reactor_t* s, size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (s->heap[parent]->deadline_ns <= s->heap[i]->deadline_ns) {
            break;
        }
        heap_swap(s, i, parent);
        i = parent;
    }
}

static void heap_sift_down(cfiber_reactor_t* s, size_t i) {
    for (;;) {
        size_t l = (2 * i) + 1;
        size_t r = (2 * i) + 2;
        size_t smallest = i;
        if (l < s->heap_len && s->heap[l]->deadline_ns < s->heap[smallest]->deadline_ns) {
            smallest = l;
        }
        if (r < s->heap_len && s->heap[r]->deadline_ns < s->heap[smallest]->deadline_ns) {
            smallest = r;
        }
        if (smallest == i) {
            break;
        }
        heap_swap(s, i, smallest);
        i = smallest;
    }
}

/* Returns false (and does not park on a timer) if the heap cannot grow. */
static bool timer_add(cfiber_reactor_t* s, ev_fiber_t* f, uint64_t deadline) {
    if (!heap_reserve(s, s->heap_len + 1)) {
        return false;
    }
    f->deadline_ns = deadline;
    f->timer_i = s->heap_len;
    s->heap[s->heap_len++] = f;
    heap_sift_up(s, f->timer_i);
    return true;
}

static void timer_remove(cfiber_reactor_t* s, ev_fiber_t* f) {
    size_t i = f->timer_i;
    if (i == NO_TIMER) {
        return;
    }
    f->timer_i = NO_TIMER;
    size_t last = --s->heap_len;
    if (i != last) {
        s->heap[i] = s->heap[last];
        s->heap[i]->timer_i = i;
        heap_sift_down(s, i);
        heap_sift_up(s, i);
    }
}

/* ============================================================================
 * Context-switch wrappers (ASan-aware; no-ops when ASan is off)
 * ============================================================================ */

static void enter_fiber(cfiber_reactor_t* s, ev_fiber_t* f) {
    s->current = f;
    f->state = FB_RUNNING;
    cfiber_tsan_switch_to(f->tsan);
    cfiber_asan_switch(&s->loop_ctx, &f->fiber.ctx, f->stack.usable_base, cstack_usable_size(&f->stack), false);
    /* control returns here when f yields, parks, or completes */
}

static void leave_to_loop(cfiber_reactor_t* s, ev_fiber_t* from, bool finishing) {
    const void* hlow;
    size_t hsz;
    cfiber_asan_host_bounds(&hlow, &hsz);
    cfiber_tsan_switch_to(s->loop_tsan);
    cfiber_asan_switch(&from->fiber.ctx, &s->loop_ctx, hlow, hsz, finishing);
}

/* ============================================================================
 * fiber-return hook: called on the fiber's stack when its fn returns
 * ============================================================================ */

[[noreturn]] static void reactor_return_hook(void* ctx) {
    cfiber_reactor_t* s = ctx;
    ev_fiber_t* dead = s->current;

    s->current = nullptr;
    s->zombie = dead;
    dead->state = FB_ZOMBIE;
    s->active--;

    leave_to_loop(s, dead, /*finishing=*/true);
    __builtin_unreachable();
}

/* ============================================================================
 * Park / resume
 * ============================================================================ */

#define FD_OWNER_INIT_CAP ((size_t)64)

static bool fd_owner_reserve(cfiber_reactor_t* s, int fd) {
    const size_t need = (size_t)fd + 1;
    if (need <= s->fd_owner_cap) {
        return true;
    }
    size_t cap = s->fd_owner_cap ? s->fd_owner_cap : FD_OWNER_INIT_CAP;
    while (cap < need) {
        cap *= 2;
    }
    ev_fiber_t** m = (ev_fiber_t**)realloc((void*)s->fd_owner, cap * sizeof(*m));
    if (!m) {
        return false;
    }
    memset((void*)(m + s->fd_owner_cap), 0, (cap - s->fd_owner_cap) * sizeof(*m));
    s->fd_owner = m;
    s->fd_owner_cap = cap;
    return true;
}

static ev_fiber_t* fd_owner_get(const cfiber_reactor_t* s, int fd) {
    return (fd >= 0 && (size_t)fd < s->fd_owner_cap) ? s->fd_owner[fd] : nullptr;
}

/* Drops f's registration if f still holds it; a taken-over entry is left to
 * its new holder. */
static void detach_fd(cfiber_reactor_t* s, ev_fiber_t* f) {
    const int fd = f->reg_fd;
    if (fd == -1) {
        return;
    }
    f->reg_fd = -1;
    if (fd_owner_get(s, fd) == f) {
        s->fd_owner[fd] = nullptr;
        epoll_ctl(s->epfd, EPOLL_CTL_DEL, fd, nullptr);
    }
}

/* Registers f as the holder of fd, taking over an entry another fiber left
 * behind. The kernel's view can differ from the map: it drops an entry on the
 * last close (the number comes back with ENOENT on MOD), so each op falls back
 * to the other once. */
static int arm_fd(cfiber_reactor_t* s, ev_fiber_t* f, int fd, uint32_t events) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (!fd_owner_reserve(s, fd)) {
        errno = ENOMEM;
        return -1;
    }
    if (f->reg_fd != fd) {
        detach_fd(s, f); /* moved to a different fd */
    }

    ev_fiber_t* const holder = s->fd_owner[fd];
    if (holder && holder != f) {
        if (holder->state == FB_PARKED && holder->wait_fd == fd) {
            errno = EBUSY; /* one fiber waits on a descriptor at a time */
            return -1;
        }
        holder->reg_fd = -1;
    }

    struct epoll_event e = {.events = events | (uint32_t)EPOLLONESHOT};
    e.data.ptr = f;
    int op = holder ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(s->epfd, op, fd, &e) < 0) {
        if (errno == ENOENT) {
            op = EPOLL_CTL_ADD;
        } else if (errno == EEXIST) {
            op = EPOLL_CTL_MOD;
        } else {
            op = -1;
        }
        if (op < 0 || epoll_ctl(s->epfd, op, fd, &e) < 0) {
            s->fd_owner[fd] = nullptr; /* state unknown; start over next time */
            f->reg_fd = -1;
            return -1;
        }
    }
    s->fd_owner[fd] = f;
    f->reg_fd = fd;
    return 0;
}

/* Move a parked fiber back onto the ready queue with the given status. Idempotent
 * via the FB_PARKED guard, so a fiber whose fd and timer fire in the same loop
 * turn is woken once. Does not detach wait sources; callers do that. */
static void resume(cfiber_reactor_t* s, ev_fiber_t* f, cfiber_ev_status_t status) {
    if (f->state != FB_PARKED) {
        return;
    }
    f->wait_status = status;
    s->blocked--;
    enqueue(s, f);
}

/* Detach a parked fiber from both wait sources and resume it. */
static void unpark(cfiber_reactor_t* s, ev_fiber_t* f, cfiber_ev_status_t status) {
    detach_fd(s, f);
    timer_remove(s, f);
    resume(s, f, status);
}

/* The fiber calling an in-fiber API, or NULL off the loop thread: asserts in
 * debug builds, lets the caller fail with EINVAL in release. */
static ev_fiber_t* running_fiber(void) {
    cfiber_reactor_t* s = g_reactor;
    ASSERT(s && s->current && "in-fiber reactor API used outside a reactor fiber");
    return s ? s->current : nullptr;
}

/* Park the current fiber and switch to the loop. Returns the delivered status. */
static cfiber_ev_status_t park_current(cfiber_reactor_t* s, park_kind_t kind) {
    ev_fiber_t* f = s->current;
    f->park = kind;
    f->state = FB_PARKED;
    f->wait_status = CFIBER_EV_READY;
    s->blocked++;
    s->current = nullptr;
    leave_to_loop(s, f, /*finishing=*/false);
    return f->wait_status;
}

/* ============================================================================
 * The core primitive
 * ============================================================================ */

cfiber_ev_status_t cfiber_ev_wait(int fd, uint32_t direction, int64_t timeout_ns) {
    ev_fiber_t* f = running_fiber();
    if (UNLIKELY(!f)) {
        errno = EINVAL;
        return CFIBER_EV_ERROR;
    }
    cfiber_reactor_t* s = g_reactor;

    /* Public bits only: an epoll flag such as EPOLLET passed through here
     * would break the ONESHOT re-arm protocol. */
    if (UNLIKELY(!direction || (direction & ~(uint32_t)(CFIBER_EV_IN | CFIBER_EV_OUT)))) {
        errno = EINVAL;
        return CFIBER_EV_ERROR;
    }
    const uint32_t events =
        ((direction & CFIBER_EV_IN) ? (uint32_t)EPOLLIN : 0u) | ((direction & CFIBER_EV_OUT) ? (uint32_t)EPOLLOUT : 0u);

    if (arm_fd(s, f, fd, events) < 0) {
        return CFIBER_EV_ERROR;
    }

    if (timeout_ns >= 0) {
        if (!timer_add(s, f, deadline_after((uint64_t)timeout_ns))) {
            detach_fd(s, f);
            errno = ENOMEM;
            return CFIBER_EV_ERROR;
        }
    }

    f->wait_fd = fd;
    cfiber_ev_status_t st = park_current(s, PARK_FD);
    f->wait_fd = -1;
    /* Ready: the one-shot is disarmed, the registration stays for the next
     * wait. Timeout/cancel: the loop already detached the fd. */
    return st;
}

cfiber_ev_status_t cfiber_ev_sleep(uint64_t ns) {
    ev_fiber_t* f = running_fiber();
    if (UNLIKELY(!f)) {
        errno = EINVAL;
        return CFIBER_EV_ERROR;
    }
    cfiber_reactor_t* s = g_reactor;

    if (!timer_add(s, f, deadline_after(ns))) {
        errno = ENOMEM;
        return CFIBER_EV_ERROR;
    }
    return park_current(s, PARK_TIMER);
}

cfiber_ev_status_t cfiber_ev_wait_async(int64_t timeout_ns) {
    ev_fiber_t* f = running_fiber();
    if (UNLIKELY(!f)) {
        errno = EINVAL;
        return CFIBER_EV_ERROR;
    }
    cfiber_reactor_t* s = g_reactor;

    if (f->wake_pending) {
        f->wake_pending = false; /* woken before parking: consume the permit */
        return CFIBER_EV_READY;
    }

    if (timeout_ns >= 0) {
        if (!timer_add(s, f, deadline_after((uint64_t)timeout_ns))) {
            errno = ENOMEM;
            return CFIBER_EV_ERROR;
        }
    }
    return park_current(s, PARK_ASYNC);
}

/* Always returns to the loop, even with nothing else ready: the loop polls
 * between rounds, so timers, I/O and cross-thread commands progress inside a
 * yield loop. */
void cfiber_ev_yield(void) {
    ev_fiber_t* cur = running_fiber();
    if (UNLIKELY(!cur)) {
        return;
    }
    cfiber_reactor_t* s = g_reactor;

    s->current = nullptr;
    enqueue(s, cur);
    leave_to_loop(s, cur, false);
}

/* ============================================================================
 * POSIX byte-stream transport helpers (layered over cfiber_ev_wait)
 * ============================================================================ */

/* Map a non-READY wait status onto errno + a -1 return. */
static int wait_failed(cfiber_ev_status_t st) {
    if (st == CFIBER_EV_CANCELLED) {
        errno = ECANCELED;
    } else if (st == CFIBER_EV_TIMEOUT) {
        errno = ETIMEDOUT;
    }
    return -1; /* CFIBER_EV_ERROR leaves errno from epoll_ctl */
}

/* timeout_ns is a total deadline for the whole call, not per retry. */
static ssize_t read_impl(int fd, void* buf, size_t n, int64_t timeout_ns) {
    const uint64_t deadline = deadline_after_opt(timeout_ns);
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0) {
            return r;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cfiber_ev_status_t st = cfiber_ev_wait(fd, CFIBER_EV_IN, remaining_ns(deadline));
            if (st == CFIBER_EV_READY) {
                continue;
            }
            return wait_failed(st);
        }
        return -1;
    }
}

static ssize_t write_impl(int fd, const void* buf, size_t n, int64_t timeout_ns) {
    const uint64_t deadline = deadline_after_opt(timeout_ns);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char*)buf + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w == 0) {
            errno = EIO; /* no progress on a non-empty write: not a valid stream state */
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cfiber_ev_status_t st = cfiber_ev_wait(fd, CFIBER_EV_OUT, remaining_ns(deadline));
            if (st == CFIBER_EV_READY) {
                continue;
            }
            return wait_failed(st);
        }
        return -1;
    }
    return (ssize_t)off;
}

ssize_t cfiber_ev_read(int fd, void* buf, size_t n) {
    return read_impl(fd, buf, n, -1);
}

ssize_t cfiber_ev_write(int fd, const void* buf, size_t n) {
    return write_impl(fd, buf, n, -1);
}

ssize_t cfiber_ev_read_timed(int fd, void* buf, size_t n, int64_t timeout_ns) {
    return read_impl(fd, buf, n, timeout_ns);
}

ssize_t cfiber_ev_write_timed(int fd, const void* buf, size_t n, int64_t timeout_ns) {
    return write_impl(fd, buf, n, timeout_ns);
}

int cfiber_ev_accept(int lfd, struct sockaddr* addr, socklen_t* alen) {
    for (;;) {
        int c = accept4(lfd, addr, alen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c >= 0) {
            return c;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cfiber_ev_status_t st = cfiber_ev_wait(lfd, CFIBER_EV_IN, -1);
            if (st == CFIBER_EV_READY) {
                continue;
            }
            return wait_failed(st);
        }
        return -1;
    }
}

int cfiber_ev_connect(int fd, const struct sockaddr* addr, socklen_t alen) {
    int r = connect(fd, addr, alen);
    if (r == 0) {
        return 0;
    }
    if (errno != EINPROGRESS) {
        return -1;
    }
    cfiber_ev_status_t st = cfiber_ev_wait(fd, CFIBER_EV_OUT, -1);
    if (st != CFIBER_EV_READY) {
        return wait_failed(st);
    }
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) {
        return -1;
    }
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

int cfiber_ev_set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int cfiber_ev_close(int fd) {
    cfiber_reactor_t* s = g_reactor;
    ASSERT(s && "cfiber_ev_close outside a reactor");
    if (UNLIKELY(!s)) {
        errno = EINVAL;
        return -1;
    }

    ev_fiber_t* holder = fd_owner_get(s, fd);
    if (holder) {
        if (holder->state == FB_PARKED && holder->wait_fd == fd) {
            unpark(s, holder, CFIBER_EV_CANCELLED);
        } else {
            detach_fd(s, holder);
        }
    }
    return close(fd);
}

/* ============================================================================
 * Fiber-struct pool (multislab-backed)
 *
 * Blocks are address-stable for the reactor's lifetime and the slab's bitmap
 * free list leaves block contents untouched on release, so a recycled block
 * keeps its generation counter, which is what makes stale handles detectable.
 * ============================================================================ */

static ev_fiber_t* fiber_obtain(cfiber_reactor_t* s) {
    ev_fiber_t* f = multislab_alloc(&s->fibers);
    if (!f) {
        return nullptr;
    }
    /* The block is zeroed on first use (zeroing backing allocator, so the
     * generation starts at 0) and retains its bumped generation across
     * recycling; preserve it across the field wipe. */
    uint64_t gen = f->generation;
    memset(f, 0, sizeof(*f));
    f->generation = gen;
    return f;
}

static void live_link(cfiber_reactor_t* s, ev_fiber_t* f) {
    f->live_prev = nullptr;
    f->live_next = s->live_head;
    if (s->live_head) {
        s->live_head->live_prev = f;
    }
    s->live_head = f;
}

static void live_unlink(cfiber_reactor_t* s, ev_fiber_t* f) {
    if (f->live_prev) {
        f->live_prev->live_next = f->live_next;
    } else {
        s->live_head = f->live_next;
    }
    if (f->live_next) {
        f->live_next->live_prev = f->live_prev;
    }
}

/* Return a struct to the pool (invalidating outstanding handles) without
 * touching its stack; used when no stack was ever attached. */
static void fiber_repool(cfiber_reactor_t* s, ev_fiber_t* f) {
    f->generation++; /* invalidate outstanding handles */
    f->state = FB_FREE;
    multislab_release(&s->fibers, f);
}

static void fiber_recycle(cfiber_reactor_t* s, ev_fiber_t* f) {
    detach_fd(s, f); /* a registration left by the last wait */
    live_unlink(s, f);
    growable_stack_release(s->stacks, &f->stack);
    cfiber_tsan_destroy(f->tsan);
    fiber_repool(s, f);
}

/* ============================================================================
 * Command ring (bounded lock-free MPMC; cross-thread wake / cancel)
 *
 * Vyukov's bounded MPMC queue: each cell carries a sequence counter that orders
 * producers and consumers without a lock. Any thread may enqueue; the loop
 * thread is the sole consumer. Commands are passed by value, so there is no
 * per-command allocation.
 * ============================================================================ */

static int ring_init(cmd_ring_t* r, size_t cap /* power of two */) {
    r->cells = calloc(cap, sizeof(*r->cells));
    if (!r->cells) {
        return -1;
    }
    r->mask = cap - 1;
    for (size_t i = 0; i < cap; i++) {
        atomic_store_explicit(&r->cells[i].seq, i, memory_order_relaxed);
    }
    atomic_store_explicit(&r->enqueue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&r->dequeue_pos, 0, memory_order_relaxed);
    return 0;
}

static void ring_destroy(cmd_ring_t* r) {
    free(r->cells);
    r->cells = nullptr;
}

static bool ring_enqueue(cmd_ring_t* r, cmd_t cmd) {
    size_t pos = atomic_load_explicit(&r->enqueue_pos, memory_order_relaxed);
    cmd_cell_t* cell;
    for (;;) {
        cell = &r->cells[pos & r->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &r->enqueue_pos, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; /* full */
        } else {
            pos = atomic_load_explicit(&r->enqueue_pos, memory_order_relaxed);
        }
    }
    cell->cmd = cmd;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
    return true;
}

static bool ring_dequeue(cmd_ring_t* r, cmd_t* out) {
    size_t pos = atomic_load_explicit(&r->dequeue_pos, memory_order_relaxed);
    cmd_cell_t* cell;
    for (;;) {
        cell = &r->cells[pos & r->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &r->dequeue_pos, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; /* empty */
        } else {
            pos = atomic_load_explicit(&r->dequeue_pos, memory_order_relaxed);
        }
    }
    *out = cell->cmd;
    atomic_store_explicit(&cell->seq, pos + r->mask + 1, memory_order_release);
    return true;
}

/* Apply a wake/cancel to a parked fiber. Stale-handle safe: the block is pooled
 * (address-stable) so the read is valid, and the generation reveals recycling. */
static void apply_cmd(cfiber_reactor_t* s, cmd_t c) {
    ev_fiber_t* f = c.target;
    if (!f || f->generation != c.gen || f->state == FB_FREE || f->state == FB_ZOMBIE) {
        return; /* stale handle: fiber completed or was recycled */
    }
    if (c.kind == CMD_CANCEL) {
        if (f->state == FB_PARKED) {
            unpark(s, f, CFIBER_EV_CANCELLED);
        }
        return;
    }
    /* Wake resumes an async park only. Anywhere else (running, ready, parked
     * on an fd or timer) it is kept as a permit for the next wait_async, so a
     * wake that races the park is neither lost nor delivered to the wrong wait. */
    if (f->state == FB_PARKED && f->park == PARK_ASYNC) {
        unpark(s, f, CFIBER_EV_READY);
    } else {
        f->wake_pending = true;
    }
}

/* Post a command from another thread: enqueue, then poke the eventfd to break
 * the loop out of epoll_wait. A full ring means the loop is not draining (not
 * running, or inside a fiber that does not yield); bounded retries cover a
 * momentary burst, then the command is refused with EAGAIN. */
static bool post_cmd(cfiber_reactor_t* s, cmd_t cmd) {
    for (int i = 0; i < POST_RETRIES; i++) {
        if (ring_enqueue(&s->cmds, cmd)) {
            uint64_t one = 1;
            ssize_t rc = write(s->evfd, &one, sizeof one);
            (void)rc; /* only fails if the 64-bit counter saturates; the loop drains it */
            return true;
        }
        sched_yield();
    }
    errno = EAGAIN;
    return false;
}

static void drain_cmds(cfiber_reactor_t* s) {
    uint64_t sink;
    while (read(s->evfd, &sink, sizeof sink) == (ssize_t)sizeof sink) {
        /* clear the counter */
    }
    cmd_t c;
    while (ring_dequeue(&s->cmds, &c)) {
        apply_cmd(s, c);
    }
}

/* ============================================================================
 * Spawning
 * ============================================================================ */

static bool spawn_locked(cfiber_reactor_t* s, cfiber_reactor_fn fn, void* arg, cfiber_reactor_handle_t* out) {
    ev_fiber_t* f = fiber_obtain(s);
    if (!f) {
        return false;
    }

    f->stack = growable_stack_alloc(s->stacks);
    if (!f->stack.mem_base) {
        fiber_repool(s, f); /* no stack to release */
        return false;
    }

    f->fiber.stack = f->stack.usable_base; /* above the guard page */
    f->fiber.stack_size = cstack_usable_size(&f->stack);
    f->reg_fd = -1;
    f->wait_fd = -1;
    f->timer_i = NO_TIMER;
    memset(&f->fiber.ctx, 0, sizeof(context_t));

    init_fiber(&f->fiber, (fiber_fn)fn, arg);
    f->tsan = cfiber_tsan_create();
    live_link(s, f);
    enqueue(s, f);
    s->active++;

    if (out) {
        out->f = f;
        out->gen = f->generation;
    }
    return true;
}

bool cfiber_reactor_spawn(cfiber_reactor_t* r, cfiber_reactor_fn fn, void* arg, cfiber_reactor_handle_t* out_handle) {
    return spawn_locked(r, fn, arg, out_handle);
}

bool cfiber_ev_spawn(cfiber_reactor_fn fn, void* arg, cfiber_reactor_handle_t* out_handle) {
    cfiber_reactor_t* s = g_reactor;
    ASSERT(s && "cfiber_ev_spawn outside a reactor");
    if (UNLIKELY(!s)) {
        errno = EINVAL;
        return false;
    }
    return spawn_locked(s, fn, arg, out_handle);
}

cfiber_reactor_handle_t cfiber_ev_self(void) {
    ev_fiber_t* f = running_fiber();
    if (UNLIKELY(!f)) {
        return (cfiber_reactor_handle_t){.f = nullptr, .gen = 0}; /* stale by construction */
    }
    return (cfiber_reactor_handle_t){.f = f, .gen = f->generation};
}

/* ============================================================================
 * Cross-thread control
 * ============================================================================ */

/* True when the caller is the reactor's own loop thread (g_reactor is set for
 * the duration of cfiber_reactor_run on that thread only). */
static bool on_loop_thread(const cfiber_reactor_t* s) {
    return g_reactor == s;
}

bool cfiber_reactor_wake(cfiber_reactor_t* r, cfiber_reactor_handle_t h) {
    cmd_t c = {.kind = CMD_WAKE, .target = h.f, .gen = h.gen};
    if (on_loop_thread(r)) {
        apply_cmd(r, c); /* no ring / eventfd needed on our own thread */
        return true;
    }
    return post_cmd(r, c);
}

bool cfiber_reactor_cancel(cfiber_reactor_t* r, cfiber_reactor_handle_t h) {
    cmd_t c = {.kind = CMD_CANCEL, .target = h.f, .gen = h.gen};
    if (on_loop_thread(r)) {
        apply_cmd(r, c);
        return true;
    }
    return post_cmd(r, c);
}

/* Same-thread fast paths, for use from within a fiber: apply directly, skipping
 * the ring + eventfd round trip. Undefined if called off the loop thread. */
void cfiber_ev_wake(cfiber_reactor_handle_t h) {
    cfiber_reactor_t* s = g_reactor;
    ASSERT(s && "cfiber_ev_wake outside a reactor");
    if (LIKELY(s)) {
        apply_cmd(s, (cmd_t){.kind = CMD_WAKE, .target = h.f, .gen = h.gen});
    }
}

void cfiber_ev_cancel(cfiber_reactor_handle_t h) {
    cfiber_reactor_t* s = g_reactor;
    ASSERT(s && "cfiber_ev_cancel outside a reactor");
    if (LIKELY(s)) {
        apply_cmd(s, (cmd_t){.kind = CMD_CANCEL, .target = h.f, .gen = h.gen});
    }
}

/* ============================================================================
 * Run loop
 * ============================================================================ */

/* Compute the epoll_wait timeout (ms) from the earliest pending deadline. */
static int next_timeout_ms(cfiber_reactor_t* s) {
    if (s->heap_len == 0) {
        return -1; /* infinite: wait for fd readiness or a command */
    }
    uint64_t deadline = s->heap[0]->deadline_ns;
    uint64_t now = now_ns();
    if (deadline <= now) {
        return 0;
    }
    const uint64_t left = deadline - now;
    if (left >= (uint64_t)INT32_MAX * 1000000ULL) {
        return INT32_MAX; /* also keeps the round-up below from overflowing */
    }
    return (int)((left + 999999ULL) / 1000000ULL); /* round up */
}

static void fire_expired_timers(cfiber_reactor_t* s) {
    uint64_t now = now_ns();
    while (s->heap_len > 0 && s->heap[0]->deadline_ns <= now) {
        ev_fiber_t* f = s->heap[0];
        timer_remove(s, f); /* pops the root, sets f->timer_i = NO_TIMER */
        detach_fd(s, f);    /* drop any fd it was also waiting on */
        resume(s, f, CFIBER_EV_TIMEOUT);
    }
}

int cfiber_reactor_run(cfiber_reactor_t* r) {
    cfiber_reactor_t* s = r;
    if (g_reactor == s) {
        errno = EBUSY; /* re-entered from one of its own fibers */
        return -1;
    }

    /* Saved and restored like the hook: a fiber may run another reactor to
     * completion and find its own current again afterwards. */
    cfiber_reactor_t* const prev_reactor = g_reactor;
    g_reactor = s;
    cfiber_return_hook_t prev_hook = cfiber_set_return_hook(reactor_return_hook, s);
    const cfiber_asan_host_t prev_host = cfiber_asan_host_begin();
    void* const prev_loop_tsan = s->loop_tsan;
    s->loop_tsan = cfiber_tsan_current(); /* a nested run's host is the outer fiber */

    struct epoll_event evs[64];
    int rc = 0;

    while (s->active > 0) {
        /* One round: the fibers ready now. Those enqueued meanwhile (yields,
         * spawns) wait for the next round, so the poll below runs between any
         * two rounds and ready work cannot starve I/O, timers or commands. */
        ev_fiber_t* const round_end = s->ready_tail;
        while (s->ready_head) {
            ev_fiber_t* f = dequeue(s);
            enter_fiber(s, f);
            if (s->zombie) {
                fiber_recycle(s, s->zombie);
                s->zombie = nullptr;
            }
            if (f == round_end) {
                break;
            }
        }

        if (s->active == 0) {
            break;
        }
        /* active == ready + parked here, so something is always waitable. */
        ASSERT(s->ready_head || s->blocked > 0);

        /* Zero timeout while there is runnable work; block only when idle. */
        const int timeout_ms = s->ready_head ? 0 : next_timeout_ms(s);
        int n = epoll_wait(s->epfd, evs, (int)(sizeof evs / sizeof evs[0]), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = -1; /* errno from epoll_wait; live fibers stay parked/ready */
            break;
        }

        for (int i = 0; i < n; i++) {
            void* ptr = evs[i].data.ptr;
            if (ptr == s) {
                drain_cmds(s); /* the eventfd control channel */
                continue;
            }
            ev_fiber_t* w = ptr;
            if (w->state != FB_PARKED) {
                continue; /* already woken this round by a timer/cancel */
            }
            timer_remove(s, w); /* drop the paired deadline, if any */
            /* leave the fd registered (one-shot disarmed); MOD re-arms it next */
            resume(s, w, CFIBER_EV_READY);
        }

        fire_expired_timers(s);
    }

    s->loop_tsan = prev_loop_tsan;
    cfiber_asan_host_end(prev_host);
    cfiber_set_return_hook(prev_hook.fn, prev_hook.ctx);
    g_reactor = prev_reactor;
    return rc;
}

/* ============================================================================
 * Construction / destruction
 * ============================================================================ */

/* Zeroing backing allocator for the fiber pool: fresh slab memory starts at
 * zero, so a block's generation counter reads 0 on its first use (no
 * uninitialized read) and only ever moves forward from there. */
static void* zeroing_alloc(size_t size, void* ctx) {
    (void)ctx;
    return calloc(1, size);
}

static void zeroing_free(void* ptr, size_t size, void* ctx) {
    (void)size;
    (void)ctx;
    free(ptr);
}

cfiber_reactor_t* cfiber_reactor_create(cfiber_reactor_config_t config) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0 || config.stack_size == 0) {
        errno = EINVAL;
        return nullptr;
    }
    size_t page = (size_t)ps;
    size_t maxs = (config.stack_size + page - 1) & ~(page - 1);
    if (maxs < page) {
        maxs = page;
    }

    /* The reactor embeds a cache-line-aligned command ring, so the struct is
     * over-aligned; calloc only guarantees max_align_t. Allocate at the struct's
     * real alignment (aligned_alloc needs a size that is a multiple of it) and
     * zero it. Still released with free(). */
    size_t rsz = align_up(sizeof(cfiber_reactor_t), _Alignof(cfiber_reactor_t));
    cfiber_reactor_t* s = aligned_alloc(_Alignof(cfiber_reactor_t), rsz);
    if (!s) {
        return nullptr;
    }
    memset(s, 0, sizeof *s);
    s->epfd = -1;
    s->evfd = -1;

    /* Grow-only fiber pool: max_slabs = 0 (unlimited), and an effectively
     * infinite empty-slab reserve so no slab is ever returned to the OS
     * (outstanding handles point into these blocks). */
    const size_t block = align_up(sizeof(ev_fiber_t), CACHE_LINE_SIZE);
    if (multislab_init_ext(
            &s->fibers, block, DEFAULT_FIBERS_PER_SLAB, 0, UINT32_MAX, zeroing_alloc, zeroing_free, nullptr)) {
        free(s);
        return nullptr;
    }

    if (ring_init(&s->cmds, CMD_RING_CAP)) {
        goto fail;
    }

    s->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (s->epfd < 0) {
        goto fail;
    }

    s->evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->evfd < 0) {
        goto fail;
    }
    struct epoll_event ee = {.events = EPOLLIN};
    ee.data.ptr = s; /* sentinel: distinguishes the control channel from fibers */
    if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, s->evfd, &ee) < 0) {
        goto fail;
    }

    s->stacks = growable_stack_allocator_create((growable_stack_allocator_args_t){
        .max_stack_size = maxs,
        .cache_capacity = config.stack_cache ? config.stack_cache : 64,
        .initial_cached = 0,
    });
    if (!s->stacks) {
        goto fail;
    }

    s->stack_size = maxs;
    return s;

fail:
    if (s->evfd >= 0) {
        close(s->evfd);
    }
    if (s->epfd >= 0) {
        close(s->epfd);
    }
    ring_destroy(&s->cmds);
    multislab_destroy(&s->fibers);
    free(s);
    return nullptr;
}

void cfiber_reactor_destroy(cfiber_reactor_t* r) {
    cfiber_reactor_t* s = r;
    if (!s) {
        return;
    }
    ASSERT(g_reactor != s && "destroying the running reactor");

    /* Forced teardown: fibers the loop never finished (never run, or left
     * parked/ready by a failed run) are discarded without resuming, so their
     * stacks are released here rather than leaked. */
    for (ev_fiber_t* f = s->live_head; f; f = f->live_next) {
        growable_stack_release(s->stacks, &f->stack);
        cfiber_tsan_destroy(f->tsan);
    }
    s->live_head = nullptr;

    multislab_destroy(&s->fibers); /* frees every fiber block */
    ring_destroy(&s->cmds);
    free((void*)s->heap);
    free((void*)s->fd_owner);

    if (s->stacks) {
        const int leaked = growable_stack_allocator_destroy(s->stacks);
        ASSERT(leaked == 0);
        (void)leaked;
    }
    if (s->evfd >= 0) {
        close(s->evfd);
    }
    if (s->epfd >= 0) {
        close(s->epfd);
    }
    free(s);
}
