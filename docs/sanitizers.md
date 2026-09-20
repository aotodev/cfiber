# Sanitizers and stack instrumentation

Two complementary tools, chosen by target rather than by taste:
`CFIBER_ASAN` on hosted builds, `CFIBER_STACK_SANITIZER` where ASan does not
exist. They are mutually exclusive and the combination is rejected at configure
time, because the canary word would land inside the ASan redzone.

The sanitizer hooks are gated by build definitions, `CFIBER_ASAN_ENABLED` and
`CFIBER_TSAN_ENABLED`, set by the corresponding CMake options for C and
assembly alike, rather than by compiler-detection macros. Compiling the library
with `-fsanitize=address` by hand, without `CFIBER_ASAN=ON`, is rejected.

## AddressSanitizer (hosted)

`CFIBER_ASAN=ON` builds the library, and via PUBLIC usage requirements the
consuming program, with AddressSanitizer plus two pieces of fiber-specific
instrumentation that plain ASan cannot supply.

**A poisoned guard below every fiber stack.** The scheduler packs many stacks
contiguously in one slab, so a downward overflow normally just corrupts the
neighbouring stack, with nothing to notice it and a crash arriving much later in
an unrelated fiber. cfiber reserves a redzone below each stack, one cache line
by default, and poisons it. The overflow becomes an immediate ASan report at the
instruction that caused it. Widen it with `CFIBER_ASAN_REDZONE` when a
single frame might step over one cache line.

**Context-switch annotations.** Swapping the stack pointer by hand hides the
switch from ASan, which then believes the old stack is still live and breaks its
stack-use-after-return tracking. The
`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` pair is
issued around every switch so ASan always knows which stack is current.

```bash
cmake -B build -DCFIBER_ASAN=ON -DCFIBER_BUILD_TESTS=ON
cmake --build build -j
ASAN_OPTIONS=detect_stack_use_after_return=1 ctest --test-dir build
# or: ./utils/make.sh -t -e --asan
```

ASan is not usable under `qemu-user`, so the AArch64 cross flow does not support
it; use a native build.

### Using the instrumentation from a custom scheduler

The instrumentation lives in
[include/cfiber/debug/asan.h](../include/cfiber/debug/asan.h), independent of the
built-in scheduler, so a custom driver can adopt it. Every entry point compiles
to nothing when ASan is disabled, so the calls can be left unconditionally in
place.

- `cfiber_asan_poison(addr, size)` / `cfiber_asan_unpoison(addr, size)`: manage a
  guard region. Poison `CFIBER_ASAN_REDZONE` bytes below each usable stack on
  allocation, unpoison on release.
- `cfiber_asan_switch(from, to, to_stack_low, to_stack_size, finishing)`:
  replaces `cfiber_switch_context()`. Pass the target stack's low address and size, and
  `finishing = true` only when the outgoing fiber is terminating, so its fake
  stack is discarded rather than saved for a resume that will never come.
- `cfiber_asan_on_fiber_entry()`: call once at the very top of a freshly started
  fiber, before its entry function, to complete the switch ASan was told about
  when the fiber was first scheduled. The built-in driver does this from its
  assembly prologue; a custom prologue must do the same.

The protocol is symmetric: the party leaving a stack issues the `start` (inside
`cfiber_asan_switch`), and the party arriving issues the `finish`, either from
`cfiber_asan_switch` when resuming a suspended fiber or from
`cfiber_asan_on_fiber_entry` when entering a fresh one. Skipping the `finish` is
the failure that produces confusing reports much later.

## Canary and watermark (any target)

`CFIBER_STACK_SANITIZER=ON` is the freestanding counterpart, and works on hosted
builds too. It is what the no-MMU Cortex-M targets get instead of ASan and
guard pages. Header:
[include/cfiber/stack/debug/stack_sanitize.h](../include/cfiber/stack/debug/stack_sanitize.h).

- **Canary**: a known 64-bit word is written at the very bottom of each stack on
  allocation and checked on release, in every build type. A changed canary
  means the fiber overflowed its stack at some point during its life.
- **Watermark**: the rest of the stack is painted `0xA5` on allocation. On
  release, scanning up from the bottom to the first byte that is no longer
  `0xA5` gives the peak usage the fiber actually reached.
  `cfiber_stack_debug_used_bytes()` returns `(size_t)-1` when the canary is
  already corrupt, since the measurement is meaningless once the stack has
  overflowed. A fiber that legitimately writes `0xA5` at its deepest point
  under-reports by that much.

A stack counts as overflowed (`cfiber_stack_debug_overflowed()`) when the canary
is gone or the watermark is used down to the canary: a large frame or an indexed
write can step clean over one word, and a stack used to its last byte has
nothing left to catch that.

The watermark is the more useful of the two in practice. On a target where stack
size is fixed at configure time and there is no MMU to catch a mistake, it turns
stack sizing from a guess into a measurement: run the worst-case workload, read
the peak, add headroom.

Both hook into the fixed-size stack allocator: `cfiber_fixed_stack_alloc()` plants and
paints, `cfiber_fixed_stack_release()` checks and returns `false` on overflow. The
built-in scheduler allocates every fiber stack through them, traps when a freed
fiber overflowed (neighbouring stacks in the slab may already be corrupt), and
accumulates the peak in `cfiber_scheduler_stack_peak()`.

```bash
./utils/make.sh -t -e --sanitizer                     # native
./utils/make.sh --arch=arm --cpu=cortex-m4 -t --sanitizer
```

CI runs the sanitizer suites natively, on AArch64 and on Cortex-M0 and M3 under QEMU.

## UndefinedBehaviorSanitizer

`CFIBER_UBSAN=ON`, hosted only, combinable with `CFIBER_ASAN`; `make.sh` allows it on
native x86_64 only, the aarch64 cross toolchain ships no libubsan. It is configured
to abort on the first finding rather than log and continue, so a CI failure
points at the first thing that went wrong.

## ThreadSanitizer

`CFIBER_TSAN=ON`, hosted x86_64, excludes `CFIBER_ASAN` and `CFIBER_FUZZ`. It
exists for the [reactor](reactor.md), whose cross-thread wake/cancel ring and
eventfd wakeup are the only concurrent code in the project. The flags go on the
reactor target and its dependents; the core library is not instrumented.

TSan follows the stack pointer, so a context switch it is not told about
interleaves the shadow stacks of different fibers and misattributes reports.
The reactor gives every fiber a TSan context and announces each switch through
[include/cfiber/debug/tsan.h](../include/cfiber/debug/tsan.h), the TSan
counterpart of `asan.h`: `cfiber_tsan_create` / `_destroy` per fiber and
`cfiber_tsan_switch_to` immediately before every switch, in both directions.
All of it compiles to nothing unless `CFIBER_TSAN_ENABLED` is defined, which the
`CFIBER_TSAN` option does. Running the built-in scheduler under TSan is not
supported.
