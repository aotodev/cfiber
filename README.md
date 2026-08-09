# cfiber

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![x86_64](https://github.com/aotodev/cfiber/actions/workflows/x86_64.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/x86_64.yml)
[![aarch64](https://github.com/aotodev/cfiber/actions/workflows/aarch64.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/aarch64.yml)
[![cortex-m0](https://github.com/aotodev/cfiber/actions/workflows/cortex-m0.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/cortex-m0.yml)
[![cortex-m3](https://github.com/aotodev/cfiber/actions/workflows/cortex-m3.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/cortex-m3.yml)
[![cortex-m4](https://github.com/aotodev/cfiber/actions/workflows/cortex-m4.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/cortex-m4.yml)
[![cortex-m7](https://github.com/aotodev/cfiber/actions/workflows/cortex-m7.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/cortex-m7.yml)
[![reactor](https://github.com/aotodev/cfiber/actions/workflows/reactor.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/reactor.yml)
[![fuzz](https://github.com/aotodev/cfiber/actions/workflows/fuzz.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/fuzz.yml)

A C library for cooperative concurrency: stackful coroutines (fibers), a
cooperative scheduler, an optional epoll reactor, and stack/memory allocators
tuned for both hosted Linux and bare-metal ARM Cortex-M. Written in C23 with
hand-written assembly for context switches.

## What's here

| Component | Description |
|-----------|-------------|
| `fiber` | A stack plus a saved callee-saved register set. `init_fiber` + `switch_context`, no syscall on the switch path. |
| `scheduler` | Cooperative FCFS scheduler with dynamic spawn and no compile-time fiber cap. |
| `reactor` | Optional Linux `epoll(7)` event loop: parks a fiber on a descriptor and resumes it on readiness, deadline, or cancellation. |
| `slab` | Fixed-size block allocator with bitmap tracking over user-supplied memory. |
| `multislab` | Auto-expanding chain of slabs with hysteresis-based shrink policy. |
| `growable_stack` | Demand-paged `mmap` stack with a `PROT_NONE` guard page, plus a recycling pool. |
| `stack sanitizer` | Canary check on release and watermark-based peak usage measurement. |
| `asan integration` | Poisoned redzone below every stack and fiber-aware switch annotations. |

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

## What makes it different

**Fibers are not coloured.** A fiber's body is an ordinary C function, and so is
anything it calls. It can yield from any depth, including from inside a
third-party library, without `async` propagating outwards through every caller
until it reaches `main`. That is what makes an existing blocking codebase
convertible: keep the call sites, swap the blocking calls for ones that park.

```c
cfiber_scheduler_t sched;
cfiber_scheduler_init(&sched, (cfiber_scheduler_config_t){
    .stack_size      = 8192,
    .fibers_per_slab = 16,
});

cfiber_scheduler_spawn(&sched, worker, "A");
cfiber_scheduler_spawn(&sched, worker, "B");

cfiber_scheduler_run(&sched);        /* blocks until both complete */
cfiber_scheduler_destroy(&sched);
```

`worker` is a plain `void (*)(void*)` that calls `cfiber_yield()` wherever it
likes. See [examples/scheduler/runtime_example.c](examples/scheduler/runtime_example.c)
for nested spawns and fibers that spawn other fibers.

**There is an epoll reactor.** Optional and Linux-only, but the reason the
non-coloured argument is more than theory. `CFIBER_REACTOR=ON` builds a
single-threaded event loop that multiplexes many fibers over `epoll(7)`; a
fiber that would block instead parks, and the loop resumes it when the
descriptor is ready, a deadline elapses, or it is cancelled. Everything reduces
to one primitive, `cfiber_ev_wait(fd, direction, timeout)`, so the POSIX
byte-stream helpers are a transport layer rather than the design. A WebSocket
(RFC 6455) echo server built on it ships in [examples/ws_echo](examples/ws_echo).
Details in [docs/reactor.md](docs/reactor.md).

**One API from Cortex-M0 to x86_64, bare metal included.** Not a reduced subset:
fibers, the scheduler, both allocators and the stack sanitizer all work on a
Cortex-M0, over ABI-compliant switches for System V AMD64, AAPCS64 and AAPCS in
both Thumb-1 and Thumb-2. Pass any `(alloc, free, ctx)` triple to
`cfiber_scheduler_init_ext()` and `malloc` is never called. No dependencies: the
hosted portion uses POSIX, the freestanding portion only `<stdint.h>` and
`<stddef.h>`. See [docs/freestanding.md](docs/freestanding.md) and
[docs/fibers.md](docs/fibers.md).

**Stack overflow is caught, not discovered later.** A `PROT_NONE` guard page
where there is an MMU, a canary plus a watermark where there is not, and a
poisoned redzone under `CFIBER_ASAN`. The watermark also reports real peak
usage, so stack sizing becomes a measurement rather than a guess. See
[docs/memory.md](docs/memory.md) and [docs/sanitizers.md](docs/sanitizers.md).

## Building

Needs CMake 3.28+ and a GNU-compatible C23 compiler (tested with GCC 15 and
Clang 22).

```bash
cmake -B build -DCFIBER_BUILD_EXAMPLES=ON -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build
```

A convenience script handles native and cross builds plus QEMU execution:

```bash
./utils/make.sh -t -e                              # native, tests + examples
./utils/make.sh -t --reactor                       # build + test the epoll reactor
./utils/make.sh -t -e --asan                       # with AddressSanitizer
./utils/make.sh --arch=aarch64 -t -e               # AArch64 via qemu-user
./utils/make.sh --arch=arm --cpu=cortex-m7 -t      # Cortex-M7 with FPU
./utils/make.sh --help
```

Full option table, cross-compilation and integration notes are in
[docs/building.md](docs/building.md).

## Tested and fuzzed

Every push runs the whole matrix in CI (the badges above): native x86_64 under
**AddressSanitizer + UndefinedBehaviorSanitizer** and the canary/watermark stack
sanitizer, AArch64 under `qemu-user`, bare-metal **Cortex-M0/M3/M4/M7** under
`qemu-system-arm`, and the reactor under **ThreadSanitizer**. Coverage-guided
**libFuzzer** harnesses drive the allocators, the scheduler and the reactor.
Per-architecture register-preservation tests pin the callee-saved set that the
hand-written assembly is responsible for.

This is a young library, so treat that as what is exercised today rather than a
guarantee of exhaustive coverage. Details in [docs/testing.md](docs/testing.md).

## Docs

- [Fibers](docs/fibers.md): the context layer, ABI details, the return hook, stackful vs stackless.
- [Scheduler](docs/scheduler.md): the FCFS model, configuration, bringing your own allocator.
- [Reactor](docs/reactor.md): the epoll loop, the core primitive, timers, cross-thread wake and cancel.
- [Memory](docs/memory.md): slab, multislab, and the fixed-size and growable stack allocators.
- [Freestanding](docs/freestanding.md): running the full library on bare-metal Cortex-M.
- [Sanitizers](docs/sanitizers.md): ASan integration, canary and watermark, UBSan and TSan.
- [Testing](docs/testing.md): what the suites and fuzzers actually cover.
- [Building](docs/building.md): CMake options, cross builds, consuming cfiber from another project.
- [Layout](docs/layout.md): source tree, header conventions, per-architecture assembly.

## License

MIT, see [LICENSE](LICENSE).
