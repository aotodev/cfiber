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
[![lint](https://github.com/aotodev/cfiber/actions/workflows/lint.yml/badge.svg?branch=master)](https://github.com/aotodev/cfiber/actions/workflows/lint.yml)

Stackful coroutines (fibers) for C23, with hand-written context switches, a
cooperative scheduler and the allocators to back them. Two targets, one API:

- **Bare-metal ARM Cortex-M.** No OS, no MMU, no heap required: fibers, the
  scheduler and the slab allocators run over user-supplied memory on a
  Cortex-M0.
- **Hosted Linux.** The same core plus demand-paged stacks with a guard page,
  AddressSanitizer integration, and an optional `epoll(7)` reactor that parks a
  fiber on a descriptor instead of blocking the thread.

## What's here

| Component | Description |
|-----------|-------------|
| `fiber` | A stack plus the callee-saved register set. `init_fiber` and `switch_context`, no syscall on the switch path. |
| `scheduler` | Cooperative FCFS scheduler with dynamic spawn and no compile-time fiber cap. |
| `reactor` | Optional, Linux only. Single-threaded `epoll(7)` loop; a parked fiber resumes on readiness, deadline or cancellation. |
| `slab`, `multislab` | Fixed-size block allocator with bitmap tracking, and an auto-expanding chain of slabs with a hysteresis shrink policy. |
| `growable_stack` | Demand-paged `mmap` stack with a `PROT_NONE` guard page and a recycling pool. Hosted only. |
| `stack sanitizer` | Canary and watermark for stacks without an MMU: overflow detection and peak usage measurement. |
| `asan integration` | Poisoned redzone below every stack and fiber-aware switch annotations. |

## Platform support

| Target              | ABI            | Tested with                          |
| ------------------- | -------------- | ------------------------------------ |
| x86_64 Linux        | System V AMD64 | Native                               |
| AArch64 Linux       | AAPCS64        | `qemu-aarch64` user-mode emulation   |
| ARM Cortex-M0 / M0+ | AAPCS Thumb-1  | `qemu-system-arm -M microbit`        |
| ARM Cortex-M3       | AAPCS Thumb-2  | `qemu-system-arm -M mps2-an385`      |
| ARM Cortex-M4 / M7  | AAPCS Thumb-2  | `qemu-system-arm -M mps2-an386/an500`|

No other host is supported. The reactor and the fuzz targets are Linux only.

## Why fibers

Fibers are not coloured. A fiber's body is an ordinary C function, and so is
anything it calls. It can yield from any depth, including from inside a
third-party library, without an `async` marker propagating through every caller
as stackless coroutines require. An existing blocking codebase keeps its call
sites and swaps the blocking calls for ones that park.

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

`worker` is a plain `void (*)(void*)` that calls `cfiber_yield()` where it
likes. Pass an `(alloc, free, ctx)` triple to `cfiber_scheduler_init_ext()` and
`malloc` is never called. See
[examples/scheduler/runtime_example.c](examples/scheduler/runtime_example.c).

## The reactor

`CFIBER_REACTOR=ON` builds a separate library, `cfiber_reactor`, that
multiplexes fibers over `epoll(7)`. Everything reduces to one primitive,
`cfiber_ev_wait(fd, direction, timeout)`; the POSIX byte-stream helpers, timers
and cross-thread wake and cancel are built on it. A WebSocket (RFC 6455) echo
server ships in [examples/ws_echo](examples/ws_echo). Details in
[docs/reactor.md](docs/reactor.md).

## Stack overflow

Caught where it happens: a `PROT_NONE` guard page where there is an MMU, a
canary and watermark where there is not, a poisoned redzone under `CFIBER_ASAN`.
The watermark reports peak usage, so stack sizing is a measurement. See
[docs/memory.md](docs/memory.md) and [docs/sanitizers.md](docs/sanitizers.md).

## Building

Needs CMake 3.28+ and a GNU-compatible C23 compiler. CI builds with GCC 15.

```bash
cmake -B build -DCFIBER_BUILD_EXAMPLES=ON -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build
```

`utils/make.sh` handles native and cross builds plus QEMU execution:

```bash
./utils/make.sh -t -e                              # native, tests + examples
./utils/make.sh -t --reactor                       # build + test the epoll reactor
./utils/make.sh -t -e --asan                       # with AddressSanitizer
./utils/make.sh --arch=aarch64 -t -e               # AArch64 via qemu-user
./utils/make.sh --arch=arm --cpu=cortex-m7 -t      # Cortex-M7
./utils/make.sh --help
```

Cross-compilation, the full option table and integration notes are in
[docs/building.md](docs/building.md).

## Testing

Every push runs the CI matrix above: x86_64 under AddressSanitizer,
UndefinedBehaviorSanitizer and the stack sanitizer, AArch64 under `qemu-user`,
Cortex-M0/M3/M4/M7 under `qemu-system-arm`, the reactor under ThreadSanitizer,
and libFuzzer harnesses for the allocators, the scheduler and the reactor.
Per-architecture tests check the callee-saved set across a context switch.

This is a young library. Treat the above as what is exercised today, not as a
guarantee. Details in [docs/testing.md](docs/testing.md).

## Docs

- [Fibers](docs/fibers.md): the context layer, ABI details, the return hook.
- [Scheduler](docs/scheduler.md): the FCFS model, configuration, bringing your own allocator.
- [Reactor](docs/reactor.md): the epoll loop, the core primitive, timers, cross-thread wake and cancel.
- [Memory](docs/memory.md): slab, multislab, fixed-size and growable stack allocators.
- [Freestanding](docs/freestanding.md): running the library on bare-metal Cortex-M.
- [Sanitizers](docs/sanitizers.md): ASan integration, canary and watermark, UBSan and TSan.
- [Testing](docs/testing.md): what the suites and fuzzers cover.
- [Building](docs/building.md): CMake options, cross builds, consuming cfiber from another project.
- [Layout](docs/layout.md): source tree, header conventions, per-architecture assembly.

## License

MIT, see [LICENSE](LICENSE).
