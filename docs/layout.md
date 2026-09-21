# Project layout

```
include/cfiber/
  core/        export attribute, defensive toggle, cache line size, bitmap word type
  debug/       AddressSanitizer integration (poisoning + switch annotations)
  fiber/       low-level context + fiber API (context.h, fiber.h)
  memory/      slab + multislab allocators
  reactor/     optional epoll(7) reactor (Linux only)
  scheduler/   cooperative FCFS scheduler
  stack/       stack descriptor + fixed-size and growable allocators
    debug/     canary + watermark sanitizer
src/cfiber/    matching implementation files; per-arch assembly under fiber/
  core/        internal.h: private macros and the hidden prologue/epilogue; version.c
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
  death/       the same guards in debug builds, observed through a forked child
  cortex/      Cortex-M support code tests (RAM layout, _sbrk)
  test/        the minimal test framework (test.h + test.c)
  consumer/    out-of-tree find_package smoke test (CI, against a staged install)
fuzz/          libFuzzer harnesses for the allocators, scheduler and reactor
utils/
  make.sh      build + run convenience script
  cortex/      startup code and linker scripts for QEMU Cortex-M targets
cmake/         toolchain files, helpers, package-config and version templates
LICENSES/      license texts for REUSE; LICENSE at the root is the same MIT text
REUSE.toml     license metadata for files that carry no SPDX header
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

Every public identifier carries the `cfiber_` / `CFIBER_` prefix. `ASSERT`,
`LIKELY`, `UNLIKELY` and `align_up` are library-private, in
`src/cfiber/core/internal.h`; it is not installed and no public header includes
it.

Only symbols marked `CFIBER_EXPORT` in the public headers are exported from a
shared build; everything else is hidden by `-fvisibility=hidden`. The `static
inline` helpers in `stack/debug/stack_sanitize.h` need no marking, since they
are compiled into the consumer rather than linked against.

## Per-architecture assembly

`src/cfiber/fiber/` holds the machine-specific half, one file per concern:

| File | Purpose |
| ---- | ------- |
| `asm_defs.inc` | object-format bracketing for the hosted files: ELF or Mach-O, selected on `__APPLE__` |
| `context_x86_64.S`, `context_aarch64.S` | `cfiber_switch_context` for the 64-bit hosts |
| `context_armv6-m.S`, `context_armv7-m.S` | `cfiber_switch_context` for Thumb-1 and Thumb-2 |
| `fiber_prologue_x86_64_fiber.S` | first-entry prologue, System V AMD64 |
| `fiber_prologue_aarch64_fiber.S` | first-entry prologue, AAPCS64 |
| `fiber_prologue_arm_cortex_fiber.S` | first-entry prologue, Cortex-M |

The split between "switch" and "prologue" is deliberate: the switch is the hot
path and runs on every yield, while the prologue runs once per fiber and carries
the ABI setup a fresh stack needs. Adding an architecture means adding one of
each plus a `cfiber_context_t` in
[context.h](../include/cfiber/fiber/context.h), and nothing above that layer
changes.
