# Project layout

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
  fiber/       register-preservation tests, assembly helper per architecture
  memory/      slab + multislab allocator tests
  reactor/     epoll reactor tests
  scheduler/   scheduler tests
  stack/       fixed-size + growable stack tests, canary/watermark sanitizer
  defensive/   misuse / error-path tests (built with NDEBUG)
  cortex/      Cortex-M support code tests (RAM layout, _sbrk)
  test/        the minimal test framework (test.h + test.c)
fuzz/          libFuzzer harnesses for the allocators, scheduler and reactor
utils/
  make.sh      build + run convenience script
  cortex/      startup code and linker scripts for QEMU Cortex-M targets
cmake/         toolchain files and helpers
ci/            Containerfile for the CI toolchain image
.github/
  workflows/   per-target CI workflows
```

## Header conventions

Every subsystem has a real header in its own directory and a one-line umbrella
header beside it:

| Umbrella | Pulls in |
| -------- | -------- |
| `cfiber/fiber.h` | `cfiber/fiber/fiber.h` |
| `cfiber/context.h` | `cfiber/fiber/context.h` |
| `cfiber/scheduler.h` | `cfiber/scheduler/scheduler.h` |
| `cfiber/reactor.h` | `cfiber/reactor/reactor.h` |

The two forms are interchangeable. The umbrella is the shorter include for
consumers; the nested path is what the library uses internally and what the docs
link to, since it is where the declarations and their documentation live.

Only symbols marked `CFIBER_EXPORT` in the public headers are exported from a
shared build; everything else is hidden by `-fvisibility=hidden`. The `static
inline` helpers in `stack/debug/stack_sanitize.h` need no marking, since they
are compiled into the consumer rather than linked against. A few ordinary
functions are also unmarked, `ms_stack_alloc()` and `ms_stack_release()` among
them, which makes them reachable from a static build only.

## Per-architecture assembly

`src/cfiber/fiber/` holds the machine-specific half, one file per concern:

| File | Purpose |
| ---- | ------- |
| `context_x86_64.S`, `context_aarch64.S` | `switch_context` for the 64-bit hosts |
| `context_armv6-m.S`, `context_armv7-m.S` | `switch_context` for Thumb-1 and Thumb-2 |
| `fiber_prologue_x86_64_fiber.S` | first-entry prologue, System V AMD64 |
| `fiber_prologue_aarch64_fiber.S` | first-entry prologue, AAPCS64 |
| `fiber_prologue_arm_cortex_fiber.S` | first-entry prologue, Cortex-M |

The split between "switch" and "prologue" is deliberate: the switch is the hot
path and runs on every yield, while the prologue runs once per fiber and carries
the ABI setup a fresh stack needs. Adding an architecture means adding one of
each plus a `context_t` in
[context.h](../include/cfiber/fiber/context.h), and nothing above that layer
changes.
