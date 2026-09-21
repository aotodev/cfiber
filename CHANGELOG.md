# Changelog

All notable changes to cfiber are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/); until 1.0 a minor release may
change the API.

## [Unreleased]

### Changed

- The hosted assembly files take their object-format bracketing (symbol
  naming, visibility, function type and size, the GNU-stack note) from
  `src/cfiber/fiber/asm_defs.inc`, with an ELF and a Mach-O branch. No
  change to the emitted ELF objects.

## [0.1.0] - 2026-09-20

First tagged release. Everything below is what the tag contains; the history
before it was a pre-release hardening pass over an unversioned tree.

### Added

- Fiber core: `cfiber_t`, `cfiber_init`, `cfiber_switch_context`, a runtime
  fiber-return hook. x86_64 (System V), AArch64 (AAPCS64), ARM Cortex-M0/M3/M4/M7
  (AAPCS, Thumb-1 and Thumb-2, optional FPU register set). The saved context
  includes the floating-point control state on every target.
- Cooperative FCFS scheduler with dynamic spawn, a user-supplied backing
  allocator, nesting of schedulers, and a stack-usage peak under the stack
  sanitizer.
- Slab and multislab allocators over user-supplied memory; fixed-size stack
  allocator; growable `mmap` stacks with a guard page and a recycling pool
  (hosted).
- Optional Linux `epoll(7)` reactor: `cfiber_ev_wait` as the single primitive,
  POSIX transport helpers with total deadlines, timers, cross-thread wake and
  cancel, per-holder descriptor registration, `cfiber_ev_close`, forced teardown
  on destroy. WebSocket echo example with a self-test.
- Stack sanitizer (canary and watermark) for targets without an MMU;
  AddressSanitizer integration with a poisoned redzone below every stack and
  fiber-aware switch annotations; ThreadSanitizer fiber annotations for the
  reactor.
- Freestanding Cortex-M support: linker scripts, startup code and syscalls for
  the QEMU boards, bounded `_sbrk`, semihosting exit on faults.
- CMake install and export (`cfiber::cfiber`, `cfiber::reactor`), relocatable
  `cfiber.pc`, generated `cfiber/version.h` with `cfiber_version()` and
  `cfiber_version_string()`, `CFIBER_BITMAP_SIZE` as a build option, C23 test
  framework with death tests, libFuzzer harnesses, per-target CI workflows on a
  pinned toolchain image.

### Changed

- Every public identifier carries the `cfiber_` / `CFIBER_` prefix; the
  private helpers moved to `src/cfiber/core/internal.h`.
- `cfiber_init` zeroes the context before filling it.
- Reactor: direction bits are the reactor's own (`CFIBER_EV_IN`/`OUT`), a wake
  resumes only an async park and is otherwise kept as a permit, `_timed`
  helpers take a total deadline, `cfiber_reactor_run` reports failures.
- `BUILD_TESTS` is `CFIBER_BUILD_TESTS` (the old name is accepted, deprecated).

[Unreleased]: https://github.com/aotodev/cfiber/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/aotodev/cfiber/releases/tag/v0.1.0
