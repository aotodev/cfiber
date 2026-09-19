# Freestanding (bare-metal ARM Cortex-M)

The full feature set runs on bare metal: fibers, the scheduler, the slab and
multislab allocators, the fixed-size stack manager, and the canary/watermark
sanitizer. The only hosted-only component is the growable-stack allocator, which
needs an MMU. The [epoll reactor](reactor.md) is Linux-only and also excluded.

Freestanding mode is selected by architecture: `CFIBER_SYSTEM_PROCESSOR`
matching `arm` sets it. In that mode `CFIBER_FREESTANDING=1` is defined as a
public macro, the growable-stack sources are dropped, and the shared-library
option is unavailable (there is no dynamic loader). Nothing needs to be turned
on by hand. Build invocations are in [building.md](building.md).

The hosted portion of the library uses POSIX (`mmap`, `mprotect`); the
freestanding portion uses only `<stdint.h>` and `<stddef.h>`. There is no libc
dependency to satisfy beyond that.

## Running without malloc

The default backing allocator is the C library's `aligned_alloc`, which on a
newlib target means the heap. `cfiber_scheduler_init_ext()` takes a backing
allocator instead. The scheduler routes
every multislab request through it, task records and fiber stacks alike, so all
of the scheduler's memory comes out of a region the user owns.

```c
#include "cfiber/scheduler/scheduler.h"

/* User-owned arena. Sized for: 2 slabs of 4 cfiber_task_t + 2 slabs of 4
 * stacks of 512 bytes + bookkeeping. Real applications should derive this
 * from the worst-case fiber count and stack size. */
alignas(64) static uint8_t arena[8192];
static size_t arena_used;

static void* bump_alloc(size_t size, void* ctx) {
    (void)ctx;
    size = (size + 7u) & ~(size_t)7u;
    if (arena_used + size > sizeof(arena)) {
        return nullptr;
    }
    void* p = &arena[arena_used];
    arena_used += size;
    return p;
}

static void bump_free(void* ptr, size_t size, void* ctx) {
    /* Bump allocator never reclaims; release is a no-op. */
    (void)ptr; (void)size; (void)ctx;
}

static void blink(void* arg) {
    volatile int* counter = arg;
    for (int i = 0; i < 4; i++) {
        (*counter)++;
        cfiber_yield();
    }
}

int main(void) {
    cfiber_scheduler_t sched;
    cfiber_scheduler_init_ext(&sched,
        (cfiber_scheduler_config_t){
            .stack_size      = 512,
            .fibers_per_slab = 4,
            .max_slabs       = 2,
        },
        bump_alloc, bump_free, nullptr);

    int c = 0;
    cfiber_scheduler_spawn(&sched, blink, &c);
    cfiber_scheduler_spawn(&sched, blink, &c);

    cfiber_scheduler_run(&sched);
    return c;
}
```

`max_slabs` is the thing to set here. With it, memory use has a hard ceiling and
`cfiber_scheduler_spawn()` returns `false` when that ceiling is reached, which is
a failure the program can handle. Without it, exhaustion becomes the backing
allocator returning `nullptr`, which works but leaves the bound implicit.

The bump allocator above never reclaims, which is fine for a set of fibers that
lives as long as the program. A free-capable allocator only matters if slabs are
expected to be released and re-taken; the multislab's shrink hysteresis is
designed to keep that traffic low either way. See [memory.md](memory.md).

## Without the scheduler

Fibers can be driven directly with `init_fiber()` and `switch_context()`, with
the user registering a completion handler through `cfiber_set_return_hook()`.
See [fibers.md](fibers.md).

The relevant allocators for a scheduler-free setup are `slab_t` over a static
array, and `ms_stack_alloc()` if a multislab is preferred.

## Sizing stacks

Stack size is fixed per scheduler and there is no MMU to catch an overflow, so
this is the number that matters most. Build with `CFIBER_STACK_SANITIZER=ON` to
get a canary word checked on release and a watermark that reports actual peak
usage, then size from the measurement rather than from a guess. See
[sanitizers.md](sanitizers.md).

## Memory layout (QEMU targets)

The linker scripts in `utils/cortex/` lay RAM out as `.data`, `.bss`, the heap
and a fixed main stack at the top, sized by `_main_stack_size`. `_sbrk` bounds
the heap by `_heap_end`, never by the stack pointer: fiber stacks are carved
from the heap, so inside a fiber SP is below the break. Exhaustion is `ENOMEM`,
not a collision with the main stack.

## Test targets

The bare-metal path is exercised in CI on Cortex-M0/M0+, M3, M4 and M7 under
`qemu-system-arm`, including the FPU register set on M4F (softfp) and M7F
(hard). Startup code and linker
scripts for those machines are in `utils/cortex/`. See [testing.md](testing.md).
