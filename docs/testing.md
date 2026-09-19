# Testing

Every push runs the full matrix in CI (the badges on the README): native x86_64
in Release and Debug with gcc and clang, under AddressSanitizer,
UndefinedBehaviorSanitizer and the canary/watermark stack sanitizer, plus the
plain `cmake`/`ctest` path; AArch64 under `qemu-user`; bare-metal
Cortex-M0/M3/M4/M7 under `qemu-system-arm`; the epoll reactor under
ThreadSanitizer; a time-boxed fuzzing pass over a cached corpus; and the
pre-commit linters.

```bash
cmake -B build -DCFIBER_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build

./utils/make.sh -t                              # the same, native
./utils/make.sh --arch=arm --cpu=cortex-m3 -t   # under QEMU
```

The suites use the minimal framework in `tests/test/`, not an external one, so
the tests build and run on the bare-metal targets with no dependency to
cross-compile. A suite in which nothing ran fails. On hosted targets the
framework also has `ASSERT_DEATH`, which runs a function in a forked child and
passes only if the child dies, which is how a trapping `ASSERT()` or a
sanitizer report is observed.

## What the suites cover

**Context switching** (`tests/fiber/`): register preservation across
`cfiber_switch_context`, with the register traffic in a per-architecture assembly
helper so the compiler never owns the callee-saved set between load, switch and
store. Two fibers switch to each other through the helper with different
patterns; each checks the other's saved `cfiber_context_t` slots while it is parked,
which catches a save-side slot mix-up that a plain round trip hides. Also
checked: stack pointer alignment at fiber entry and that the saved stack pointer
lies inside the fiber's stack. AArch64 covers `d8`-`d15`, Cortex-M4F and M7F
`s16`-`s31`.

**Allocators** (`tests/memory/`): slab and multislab exhaustion, release and
reuse, lazy growth, `max_slabs` caps, the full/active list transitions, the
empty-slab shrink hysteresis, and, through a counting backing allocator, that
`destroy` returns every byte it took.

**Scheduler** (`tests/scheduler/`): spawn, yield and completion ordering,
dynamic spawning from inside a running fiber, capacity exhaustion, and leak
balance across repeated spawn/free churn.

**Stacks** (`tests/stack/`): the fixed-size and growable allocators, guard-page
faulting, and the canary/watermark sanitizer itself.

**Reactor** (`tests/reactor/`): spawn/yield/completion, parking on a descriptor
and waking on readiness, timer ordering, wait timeouts, cancelling a parked
fiber, cross-thread wake and cancel, the in-process fast paths, the cross-thread
ring under stress, and create/destroy churn. Also run under ThreadSanitizer.

**Defensive paths** (`tests/defensive/`): bad init parameters, double free,
foreign-pointer release. Built with `-DNDEBUG` so the guards return errors
instead of tripping an assert, which is the configuration a release consumer
actually gets.

**Death tests** (`tests/death/`): the same guards in a debug build, where they
trap, plus the in-fiber APIs outside a run, a scheduler destroyed with live
fibers or re-entered from its own fiber, and, under ASan, a fiber writing into
the redzone below its stack. Each case runs in a forked child.

## Beyond the unit tests

- **AddressSanitizer + UndefinedBehaviorSanitizer** on hosted builds, with the
  fiber-aware stack redzones and switch annotations described in
  [sanitizers.md](sanitizers.md).
- **Canary + watermark** stack instrumentation on the no-MMU bare-metal targets,
  the freestanding counterpart to ASan.
- **ThreadSanitizer** on the reactor, the only concurrent code in the project.
- **Coverage-guided fuzzing** with libFuzzer over the allocators, the scheduler
  and the reactor, under ASan + UBSan. CI runs a short time-boxed smoke pass;
  the same targets run longer locally against a persisted corpus. See
  [fuzz/README.md](../fuzz/README.md).

## CI layout

One workflow per target, so each keeps its own badge and a failure names the
platform without opening the log. The toolchain image (cross-compilers plus
QEMU) is built by its own workflow and published to ghcr.io; the test workflows
only consume it, so the per-push path never rebuilds a container. The
Containerfile is in `ci/`. The workflows pin the image by digest; the image
workflow prints the new digest to paste in after a rebuild. Every job has a
timeout, `make.sh` bounds each QEMU run, and the Cortex-M startup code exits
through semihosting on an unexpected exception instead of spinning.

## Scope

This is a young library. Treat the above as what is exercised today, not as a
guarantee of exhaustive coverage.
