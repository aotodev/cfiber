# cfiber

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![x86_64](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/x86_64.yml/badge.svg?branch=master&label=x86_64)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=x86_64.yml)
[![aarch64](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/aarch64.yml/badge.svg?branch=master&label=aarch64)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=aarch64.yml)
[![cortex-m0](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/cortex-m0.yml/badge.svg?branch=master&label=cortex-m0)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=cortex-m0.yml)
[![cortex-m3](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/cortex-m3.yml/badge.svg?branch=master&label=cortex-m3)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=cortex-m3.yml)
[![cortex-m4](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/cortex-m4.yml/badge.svg?branch=master&label=cortex-m4)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=cortex-m4.yml)
[![cortex-m7](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/cortex-m7.yml/badge.svg?branch=master&label=cortex-m7)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=cortex-m7.yml)
[![fuzz](https://git.vernizzi.io/vernizzi/cfiber/actions/workflows/fuzz.yml/badge.svg?branch=master&label=fuzz)](https://git.vernizzi.io/vernizzi/cfiber/actions?workflow=fuzz.yml)

A C library for cooperative concurrency: stackful coroutines (fibers), a
cooperative scheduler and stack/memory allocators tuned for both hosted Linux
and bare-metal ARM Cortex-M. Written in C23 with hand-written assembly for
context switches.

## Features

- Context switches in hand-written assembly with no syscalls; only the
  callee-saved set is preserved.
- ABI-compliant prologues and switches for System V AMD64, AAPCS64, AAPCS
  (Thumb-1 and Thumb-2). Optional FPU save/restore on Cortex-M4F/M7F.
- Cooperative FCFS scheduler with dynamic spawn; no compile-time fiber-count
  cap.
- Slab and multislab fixed-size allocators usable without `malloc`.
- Growable hosted stacks: `mmap` + `PROT_NONE` guard page + SIGSEGV-driven
  growth, with a pool that recycles `MADV_DONTNEED` pages on release.
- Pluggable backing allocator on the scheduler: pass any `(alloc, free, ctx)`
  triple instead of `malloc`/`free`. The full library (scheduler included)
  runs on bare metal this way.
- Optional stack sanitizer: canary check on release + watermark-based peak
  usage measurement. Available on hosted and freestanding builds.
- Optional AddressSanitizer integration (hosted): a poisoned guard below every
  fiber stack turns silent inter-stack overflow into an instruction-accurate
  report, and the `__sanitizer_*_switch_fiber` annotations keep ASan's
  stack-use-after-return tracking correct across context switches. Reusable from
  a custom scheduler via [include/cfiber/debug/asan.h](include/cfiber/debug/asan.h).
- No dependencies. The hosted portion uses POSIX (`mmap`, `mprotect`,
  `sigaction`); the freestanding portion uses only `<stdint.h>` and `<stddef.h>`.

## Platform support

| Target              | ABI            | Status   | How it is tested                     |
| ------------------- | -------------- | -------- | ------------------------------------ |
| x86_64 Linux        | System V AMD64 | Tested   | Native, register-preservation tests  |
| AArch64 Linux       | AAPCS64        | Tested   | `qemu-aarch64` user-mode emulation   |
| ARM Cortex-M0 / M0+ | AAPCS Thumb-1  | Tested   | `qemu-system-arm -M microbit`        |
| ARM Cortex-M3       | AAPCS Thumb-2  | Tested   | `qemu-system-arm -M mps2-an385`      |
| ARM Cortex-M4 / M7  | AAPCS Thumb-2  | Tested   | `qemu-system-arm -M mps2-an386/an500`|

macOS is expected to work on x86_64 and AArch64 (System V / AAPCS64 are the
same) but is not part of the test matrix. Windows is not supported.

## Requirements

- CMake 3.28 or newer
- A GNU-compatible C23 compiler. Tested with GCC 15 and Clang 22; older
  versions back to GCC 14 / Clang 19 should work but are unverified.
- For ARM cross-builds: `arm-none-eabi-gcc` and `qemu-system-arm`
- For AArch64 cross-builds: `aarch64-linux-gnu-gcc` and `qemu-user`

Select a compiler at configure time with `CC=clang cmake ...` or
`CC=clang ./utils/make.sh ...`.

## Building

```bash
cmake -B build -DCFIBER_BUILD_EXAMPLES=ON -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build
```

A convenience script handles native and cross builds plus QEMU execution:

```bash
./utils/make.sh -t -e                              # native, tests + examples
./utils/make.sh -t -e --sanitizer                  # with stack sanitizer (canary)
./utils/make.sh -t -e --asan                       # with AddressSanitizer (x86_64)
./utils/make.sh -t -e --ubsan                      # with UndefinedBehaviorSanitizer
./utils/make.sh -t -e -d                           # Debug build
./utils/make.sh -t -e --shared                     # shared library
./utils/make.sh -t -e --pic                        # static + PIC
./utils/make.sh --arch=aarch64 -t -e               # AArch64 via qemu-user
./utils/make.sh --arch=arm --cpu=cortex-m0 -t      # Cortex-M0
./utils/make.sh --arch=arm --cpu=cortex-m7 -t      # Cortex-M7 with FPU
./utils/make.sh --help
```

### CMake options

| Option                              | Default | Description                                                  |
| ----------------------------------- | ------- | ------------------------------------------------------------ |
| `CFIBER_BUILD_EXAMPLES`             | `OFF`   | Build the standalone examples (`examples/`)                  |
| `BUILD_TESTS`                       | `OFF`   | Build the unit-test executables                              |
| `CFIBER_STACK_SANITIZER`            | `OFF`   | Enable canary + watermark instrumentation                    |
| `CFIBER_ASAN`                       | `OFF`   | Build with AddressSanitizer + fiber-aware instrumentation (hosted only; excludes `CFIBER_STACK_SANITIZER`) |
| `CFIBER_ASAN_REDZONE`               | -       | Guard size in bytes below each fiber stack (default: one cache line). Only used with `CFIBER_ASAN` |
| `CFIBER_UBSAN`                      | `OFF`   | Build with UndefinedBehaviorSanitizer; aborts on the first finding (hosted only; combinable with `CFIBER_ASAN`) |
| `CFIBER_TSAN`                       | `OFF`   | Build with ThreadSanitizer for the reactor's concurrency (hosted x86_64; excludes `CFIBER_ASAN`/`CFIBER_FUZZ`) |
| `CFIBER_FUZZ`                       | `OFF`   | Build the libFuzzer targets under ASan + UBSan (Clang + hosted only)        |
| `CFIBER_REACTOR`                    | `OFF`   | Build the optional epoll(7) reactor (`libcfiber_reactor`); Linux only       |
| `CFIBER_BUILD_SHARED`               | `OFF`   | Build as a shared library (`libcfiber.so`) instead of static |
| `CFIBER_POSITION_INDEPENDENT_CODE`  | `OFF`   | Build the static library with `-fPIC` (ignored when shared)  |
| `CFIBER_TARGET_CPU`                 | -       | `cortex-m0` / `cortex-m3` / `cortex-m4` / `cortex-m7`        |
| `CFIBER_ARM_FLOAT_ABI`              | -       | `soft` / `softfp` / `hard`                                   |
| `CFIBER_ARM_FPU`                    | -       | FPU name forwarded to `-mfpu` (e.g. `fpv5-sp-d16`)           |

The default is a static archive. Shared builds export only the documented API
(everything declared with `CFIBER_EXPORT` in the public headers) and hide
everything else via `-fvisibility=hidden`. Shared builds carry the project
version as `SOVERSION`. The shared build is not available on the freestanding
target (bare-metal Cortex-M has no dynamic loader).

If the static library will be linked into a downstream shared object, enable
`CFIBER_POSITION_INDEPENDENT_CODE` to avoid text-relocation errors.

A custom scheduler supplies its fiber-completion behaviour by registering a
fiber-return hook at run time with `cfiber_set_return_hook()` (rather than
overriding a link-time symbol). Because the hook is dispatched at run time, this
works identically against the static and shared builds, and several schedulers
can coexist in one process; each registers its own hook while running and
restores the previous one on exit.

### Consuming as a subdirectory

```cmake
add_subdirectory(external/cfiber)
target_link_libraries(my_app PRIVATE cfiber)
```

To switch to a shared build from a parent project:

```cmake
set(CFIBER_BUILD_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory(external/cfiber)
target_link_libraries(my_app PRIVATE cfiber)
```

## Testing

Every push runs the matrix in CI (the badges above): native x86_64 under
AddressSanitizer, UndefinedBehaviorSanitizer, and the canary/watermark stack
sanitizer; AArch64 under `qemu-user`; and bare-metal Cortex-M0/M3/M4/M7 under
`qemu-system-arm`. Run the suites locally with `ctest --test-dir build` (native)
or `./utils/make.sh -t ...` (any target, including the QEMU flows).

What is covered:

- **Context switching**: per-architecture register-preservation tests for the
  callee-saved set (plus S16–S31 on Cortex-M7F).
- **Allocators**: slab and multislab: exhaustion, release/reuse, lazy growth,
  `max_slabs` caps, full/active list transitions, empty-slab hysteresis, and
  (via a counting backing allocator) that `destroy` returns every byte.
- **Scheduler**: spawn / yield / completion ordering, dynamic spawning,
  capacity exhaustion, and leak balance across spawn/free churn.
- **Stacks**: fixed-size and growable allocators, guard-page faulting, and the
  canary/watermark sanitizer.
- **Defensive paths**: bad init parameters, double-free, and foreign-pointer
  release (built with `-DNDEBUG` so the guards return errors instead of trapping).

Beyond the unit tests, the project leans on:

- **AddressSanitizer + UndefinedBehaviorSanitizer** on hosted builds
  (`CFIBER_ASAN`, `CFIBER_UBSAN`).
- **Canary + watermark** stack instrumentation for the no-MMU bare-metal targets
  (`CFIBER_STACK_SANITIZER`), the freestanding counterpart to ASan.
- **Coverage-guided fuzzing** (libFuzzer, `CFIBER_FUZZ`, Clang) of the allocator
  and scheduler under ASan + UBSan. CI runs a short, time-boxed smoke pass; the
  same targets can be run longer locally against a persisted corpus; see
  [fuzz/README.md](fuzz/README.md).

This is a young library, so treat it accordingly: the above is what is exercised
today, not a guarantee of exhaustive coverage.

## Stackful vs stackless coroutines

A stackful coroutine owns a real call stack. It can yield from any point in any
function it calls, including from deep inside a third-party library, and the
caller's frames are paused as a whole.

A stackless coroutine is a compiler-rewritten state machine. It can only
suspend at points the compiler can see, which means every function on the
suspension path has to be coloured: in C++ it has to be a `co_await`able, in
Rust it has to be `async`. Calling synchronous code from async code is fine;
calling async code from synchronous code is not. The colour propagates outwards
through every caller until it reaches `main`.

cfiber avoids that. A fiber's body is an ordinary C function and any function
it calls is also ordinary C. That makes it easy to turn an existing blocking
codebase into a cooperative one: keep the call sites the same and hook the
blocking syscalls (`read`, `write`, `accept`, ...) so that, instead of blocking
the thread, they register interest with a poller and yield the current fiber.
When the poller wakes the fiber up the syscall returns its result and the
caller is none the wiser.

That flexibility is not free, and stackless coroutines remain the better fit
for some workloads. A stackless coroutine's saved state is a small,
compiler-known struct, often well under 100 bytes, packed contiguously with
its siblings. A fiber, by contrast, owns a full stack: at minimum one page,
typically several. At the scale of hundreds of thousands of concurrent tasks,
that difference dominates: the stackless layout fits in cache, the stackful
one does not, and resuming a fiber tends to touch a cold stack page while
resuming a stackless coroutine pulls in a single cache line. If raw
per-coroutine throughput on a tight inner loop is the priority, or if the
working set is enormous, stackless is usually the right tool.

cfiber narrows the gap where it can. On hosted targets, stacks grow one page
at a time on demand (backed by a guard-page SIGSEGV handler) so an idle fiber
costs only the pages it has actually touched. On freestanding targets, the
user sizes a slab of fixed stacks for the worst-case workload and pays no
heap or page-fault cost at runtime. Neither closes the gap entirely; both make
fibers practical for the cases where their flexibility is worth it.

## Hosted example

A scheduler-managed fiber on Linux. The scheduler owns both the task records
and the fiber stacks; nothing else needs to be allocated.

```c
#include "cfiber/scheduler/scheduler.h"
#include <stdio.h>

static void worker(void* arg) {
    const char* name = arg;
    for (int i = 0; i < 3; i++) {
        printf("%s: step %d\n", name, i);
        cfiber_yield();
    }
}

int main(void) {
    cfiber_scheduler_t sched;
    cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){
        .stack_size      = 8192,
        .fibers_per_slab = 16,
    });

    cfiber_scheduler_spawn(&sched, worker, "A");
    cfiber_scheduler_spawn(&sched, worker, "B");

    cfiber_scheduler_run(&sched);
    cfiber_scheduler_destroy(&sched);
    return 0;
}
```

See [examples/scheduler/runtime_example.c](examples/scheduler/runtime_example.c) for nested spawns and
fibers that spawn other fibers.

## Freestanding example

The full feature set (scheduler, slab/multislab allocators, fixed-size stack
manager, and the canary/watermark sanitizer) works on bare metal. The only
hosted-only components are the growable-stack allocator and its SIGSEGV handler
(both require an MMU and POSIX signals).

To use the scheduler without `malloc`/`free`, pass a backing allocator to
`cfiber_scheduler_init_ext`. The scheduler hands every multislab request
through that callback, so task records and fiber stacks all come out of a
region the user owns.

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

The freestanding build is selected automatically when the target architecture
is `arm`. In that mode the public macro `CFIBER_FREESTANDING=1` is defined and
the growable-stack sources are excluded. Everything else, including the
fixed-size stack allocator at [include/cfiber/stack/fixed_size_stack_allocator.h](include/cfiber/stack/fixed_size_stack_allocator.h)
and the sanitizer, is available.

If a scheduler is not desired, fibers can be driven directly with `init_fiber`
and `switch_context`, and the user registers a fiber-return hook with
`cfiber_set_return_hook()` (called on the returning fiber's stack when its entry
function returns).

## epoll reactor (optional, Linux)

`CFIBER_REACTOR` builds an optional, Linux-only `epoll(7)` reactor as a separate
library, `libcfiber_reactor`. It is an alternative scheduler (built on cfiber's
fiber + growable-stack mechanism rather than the built-in FCFS scheduler) that
multiplexes many fibers onto one thread. A fiber that would block on a
non-blocking descriptor instead *parks* (registers interest with the poller and
yields); the loop resumes it when the descriptor is ready, a deadline elapses, or
it is cancelled. It registers its fiber-return hook at run time via
`cfiber_set_return_hook()`, so it coexists with the built-in scheduler and works
through the shared library too.

```sh
cmake -B build -DCFIBER_REACTOR=ON
# or:  ./utils/make.sh -t --reactor          # build + run the reactor tests
```

The header is [include/cfiber/reactor/reactor.h](include/cfiber/reactor/reactor.h).

The irreducible primitive is

```c
cfiber_ev_status_t cfiber_ev_wait(int fd, uint32_t direction, int64_t timeout_ns);
```

which reads "park the current fiber until `fd` is ready in `direction` (`EPOLLIN` /
`EPOLLOUT`), the timeout elapses, or the fiber is cancelled." The byte-stream
helpers `cfiber_ev_read` / `_write` / `_accept` / `_connect` are a thin POSIX
*transport layer* over that primitive: they loop on `EAGAIN` and call
`cfiber_ev_wait` with the appropriate direction. A different transport (TLS, UDP)
is just a different helper layer over the same primitive; the loop never bakes
in a direction or a protocol. The reactor also provides `cfiber_ev_sleep`,
deadline-bearing waits (`cfiber_ev_read_timed` / `_write_timed`), cooperative
`cfiber_ev_yield`, and `cfiber_ev_spawn`.

Timers are a monotonic deadline min-heap that feeds the `epoll_wait` timeout.
Cross-thread wakeup and cancellation (`cfiber_reactor_wake()` and
`cfiber_reactor_cancel()`, safe from any thread) post to a bounded lock-free
ring and poke an `eventfd` to break the loop out of `epoll_wait`; issued from the
loop thread itself they are applied directly. From within a fiber, the
`cfiber_ev_wake()` / `cfiber_ev_cancel()` fast paths skip the ring entirely.
Cancelling a parked fiber resumes it with `CFIBER_EV_CANCELLED`, and an in-flight
transport helper then returns `-1` with `errno == ECANCELED`.

Handles are generation-tagged references into an address-stable fiber pool
(cfiber's `multislab`), so waking or cancelling a fiber that has already
completed is a safe no-op rather than a use-after-free.

A standalone WebSocket (RFC 6455) echo server built on the reactor lives in
[examples/ws_echo](examples/ws_echo) (built when `CFIBER_REACTOR=ON`).

Scope: one reactor per thread (the single-loop-per-core model), and one fiber
owns a given descriptor at a time. The full-duplex reader+writer split (two
fibers on one fd) and non-POSIX transports (TLS, UDP) are future work that the
`cfiber_ev_wait` primitive is designed to accommodate.

## Debugging with AddressSanitizer

On hosted targets, `CFIBER_ASAN` builds the library (and, via PUBLIC usage
requirements, the consuming program) with AddressSanitizer plus two pieces of
fiber-specific instrumentation:

- **A poisoned guard below every fiber stack.** The scheduler packs many stacks
  contiguously in one allocation, so a downward overflow normally just corrupts
  the neighbouring stack with nothing to notice it. cfiber reserves a redzone
  below each stack (one cache line by default, `CFIBER_ASAN_REDZONE` to widen)
  and poisons it, turning that overflow into an immediate ASan report.
- **Context-switch annotations.** Swapping the stack pointer by hand hides the
  switch from ASan and breaks its stack-use-after-return tracking. The
  `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` pair is
  issued around every switch so ASan always knows which stack is live.

```bash
cmake -B build -DCFIBER_ASAN=ON -DBUILD_TESTS=ON
cmake --build build -j
ASAN_OPTIONS=detect_stack_use_after_return=1 ctest --test-dir build
# or: ./utils/make.sh -t -e --asan
```

This is the hosted counterpart to `CFIBER_STACK_SANITIZER` (canary + watermark),
which targets freestanding / non-MMU builds where ASan is unavailable. The two
are mutually exclusive, the canary word would land inside the ASan redzone, so
enabling both is rejected at configure time. AddressSanitizer is not usable
under `qemu-user`, so the AArch64 cross/QEMU flow does not support it; use a
native build.

### Using the instrumentation with a custom scheduler

The instrumentation lives in [include/cfiber/debug/asan.h](include/cfiber/debug/asan.h),
independent of the built-in scheduler, so a custom driver can adopt it. Every
entry point compiles to nothing when ASan is disabled, so calls can be left
unconditionally in place.

- `cfiber_asan_poison(addr, size)` / `cfiber_asan_unpoison(addr, size)`: manage
  a guard region; poison `CFIBER_ASAN_REDZONE` bytes below each usable stack on
  allocation and unpoison on release.
- `cfiber_asan_switch(from, to, to_stack_low, to_stack_size, finishing)`:
  replaces `switch_context`. Pass the target stack's low address and size, and
  `finishing = true` only when the outgoing fiber is terminating (so its fake
  stack is discarded rather than saved for a resume that never comes).
- `cfiber_asan_on_fiber_entry()`: call once at the very top of a freshly
  started fiber, before its entry function, to complete the switch ASan was told
  about when the fiber was first scheduled. The built-in driver does this from
  its assembly prologue; a custom prologue must do the same.

The protocol is: the party leaving a stack issues the `start` (inside
`cfiber_asan_switch`), and the party arriving issues the `finish`, either from
`cfiber_asan_switch` when resuming a suspended fiber, or from
`cfiber_asan_on_fiber_entry` when entering a fresh one.

## Project layout

```
include/cfiber/
  core/        compiler attributes, branch hints, bitmap word type
  debug/       AddressSanitizer integration (poisoning + switch annotations)
  fiber/       low-level context + fiber API (context.h, fiber.h)
  memory/      slab + multislab allocators
  reactor/     optional epoll(7) reactor (Linux only)
  scheduler/   cooperative FCFS scheduler
  stack/       stack descriptor + fixed-size and growable allocators
    debug/     canary + watermark sanitizer
src/cfiber/    matching implementation files; per-arch assembly under fiber/
examples/
  scheduler/   built-in scheduler example (all targets, incl. bare-metal ARM)
  ws_echo/     WebSocket echo on the reactor (Linux, needs CFIBER_REACTOR)
tests/
  fiber/       register-preservation tests, one per architecture
  memory/      slab + multislab allocator tests
  reactor/     epoll reactor tests
  scheduler/   scheduler tests
  stack/       fixed-size + growable stack tests, canary/watermark sanitizer
  defensive/   misuse / error-path tests (built with NDEBUG)
  test/        the minimal test framework (test.h + test.c)
fuzz/          libFuzzer harnesses for the allocator, scheduler and reactor
utils/
  make.sh      build + run convenience script
  cortex/      startup code and linker scripts for QEMU Cortex-M targets
cmake/         toolchain files and helpers
ci/            Containerfile for the CI toolchain image
.forgejo/      per-target CI workflows
```

## License

MIT, see [LICENSE](LICENSE).
