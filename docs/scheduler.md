# The built-in scheduler

A cooperative, non-preemptive, first-come-first-served scheduler over a FIFO
ready queue. Header:
[include/cfiber/scheduler/scheduler.h](../include/cfiber/scheduler/scheduler.h).

It is the simplest thing that drives fibers correctly, and it is the one
component that runs unchanged on every supported target, x86_64 down to
Cortex-M0. For I/O-driven work on Linux, use the [epoll reactor](reactor.md)
instead.

## Model

`cfiber_scheduler_t` owns two multislab allocators: one for task records and one
for fiber stacks. Both grow on demand, so there is no compile-time cap on the
number of concurrent fibers; `max_slabs` sets one when a cap is wanted.

`cfiber_scheduler_run()` dispatches the head of the ready queue and blocks until
every fiber has completed. A fiber that calls `cfiber_yield()` goes to the back
of the queue. A fiber whose entry function returns is reaped through the return
hook the scheduler registers for the duration of the run (see
[fibers.md](fibers.md)); its task record and stack go back to the slabs.

The struct is fully visible, so a scheduler can live on the stack or in static
storage rather than being heap-allocated.

## Configuration

```c
cfiber_scheduler_t sched;
cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){
    .stack_size      = 8192,   /* per fiber; >= 256 and a multiple of the cache line */
    .fibers_per_slab = 16,     /* 0 selects the default of 16 */
    .max_slabs       = 0,      /* 0 means unlimited */
});
```

`stack_size` is fixed per scheduler, since the stacks come out of a fixed-size
slab. Size it for the deepest fiber, or use the
[stack sanitizer](sanitizers.md) to measure the real peak.

## API

| Function | Purpose |
| -------- | ------- |
| `cfiber_scheduler_init` | Initialise, backed by `malloc`/`free` |
| `cfiber_scheduler_init_ext` | Initialise with a user `(alloc, free, ctx)` triple |
| `cfiber_scheduler_spawn` | Add a fiber; valid before and during the run |
| `cfiber_scheduler_run` | Dispatch until every fiber completes |
| `cfiber_scheduler_destroy` | Release all backing memory |
| `cfiber_scheduler_current` | The thread-local running scheduler, or NULL |
| `cfiber_yield` | From inside a fiber: go to the back of the ready queue |
| `cfiber_spawn` | From inside a fiber: spawn on the current scheduler |

`spawn` returns `false` when allocation fails (slab exhaustion under a
`max_slabs` cap, or the backing allocator refusing), so a fiber can be spawned
speculatively and the failure handled. `destroy` requires that every fiber has
completed.

Dynamic spawning is the interesting half: a running fiber can call
`cfiber_spawn()` and the new fiber joins the same queue, which is how a task
tree is built without knowing its shape up front. See
[examples/scheduler/runtime_example.c](../examples/scheduler/runtime_example.c)
for nested spawns and fibers that spawn other fibers.

## Bring your own allocator

`cfiber_scheduler_init_ext()` takes an allocation callback, a free callback and
an opaque context. Every multislab request, task records and fiber stacks alike,
goes through them, so the whole scheduler can run out of a region the user owns
and `malloc` is never called. This is what makes the scheduler usable on bare
metal; see [freestanding.md](freestanding.md) for a worked example.

```c
void* (*mem_alloc)(size_t size, void* ctx);
void  (*mem_free)(void* ptr, size_t size, void* ctx);
```

The free callback receives the size, so a user allocator does not have to record
block sizes itself. Passing NULL for both callbacks falls back to `malloc` and
`free`, making `init_ext` equivalent to `init`.

## Threading

One scheduler per thread. `cfiber_scheduler_current()`, `cfiber_yield()` and
`cfiber_spawn()` act on the calling thread's running scheduler, which is stored
thread-locally on hosted targets and as a plain global on bare-metal Cortex-M.
Fibers are not migrated between threads.
