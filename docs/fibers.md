# Fibers and context switching

The lowest layer of the library: a fiber is a stack plus a saved register set,
and a context switch swaps one for the other. Everything above it (the FCFS
scheduler, the epoll reactor, any custom driver) is built on these two headers:

- [include/cfiber/fiber/context.h](../include/cfiber/fiber/context.h): `context_t` and `switch_context()`
- [include/cfiber/fiber/fiber.h](../include/cfiber/fiber/fiber.h): `fiber_t`, `init_fiber()` and the return hook

## What a switch costs

`switch_context(old, new)` is hand-written assembly and issues no syscall. It
saves the callee-saved set to `old`, restores it from `new`, and jumps. Because
the caller-saved registers are the caller's problem by definition, they are not
touched, which is what keeps the saved context small:

| Target | Saved | Notes |
| ------ | ----- | ----- |
| x86_64 | `rsp`, `rbx`, `rbp`, `r12`-`r15`, `MXCSR`, x87 control word | System V AMD64 |
| AArch64 | `sp`, `x19`-`x30`, low 64 bits of `v8`-`v15`, `FPCR` | AAPCS64 |
| ARM Cortex-M | `sp`, `r4`-`r11`, `lr` | AAPCS, Thumb-1 and Thumb-2 |
| Cortex-M4F / M7F | the above plus `s16`-`s31`, `FPSCR` | when built for an FPU (`__ARM_FP`), `softfp` or `hard` |

The FPU half of the Cortex-M context is conditional, so a soft-float build pays
nothing for registers it does not have.

The floating-point control state is part of the context on every target, as
the ABIs require of a callee: a fiber that changes the rounding mode or the
exception masks keeps that change to itself, and a new fiber starts with its
creator's settings, as a new thread would.

## Starting a fiber

`init_fiber()` does not run anything. It sets the stack pointer to the top of
the stack with the alignment the ABI requires, stores the entry function and
user data in callee-saved registers, and arranges for the first
`switch_context()` into the fiber to land in an architecture-specific prologue.
The prologue reads those two registers back out, calls the entry function with
`user_data` as its only argument, and on return calls the epilogue, which never
comes back.

```c
fiber_t fiber = { .stack = stack_memory, .stack_size = 8192 };
init_fiber(&fiber, my_fiber_fn, user_data);

context_t caller;
switch_context(&caller, &fiber.ctx);   /* runs my_fiber_fn */
```

The stack grows downward from `stack + stack_size` and must stay valid for the
whole life of the fiber. Recommended minimums are 8 KB on x86_64 and AArch64,
and 512 bytes to 4 KB on Cortex-M depending on available SRAM.

## The fiber-return hook

A fiber's entry function has no caller to return to: there is no valid return
address on its stack. When it returns, the epilogue instead calls the hook
registered with `cfiber_set_return_hook()`, running on the returning fiber's
stack. The hook must mark the fiber complete, pick the next context, and call
`switch_context()`. It must never return normally.

```c
cfiber_return_hook_t prev = cfiber_set_return_hook(my_hook, my_ctx);
/* ... run fibers ... */
cfiber_set_return_hook(prev.fn, prev.ctx);
```

Registration returns the previous hook so drivers nest. Dispatch happens at run
time rather than through a link-time symbol override, which has two
consequences: it works identically against the static and the shared build, and
several schedulers can coexist in one process, each registering its hook while
running and restoring the previous one on exit. The built-in scheduler and the
reactor both do exactly this.

The registration is thread-local on hosted targets and a plain global on
bare-metal Cortex-M, which has no TLS.

## Driving fibers without a scheduler

The scheduler is optional. `init_fiber()`, `switch_context()` and a return hook
are enough to build a driver with whatever policy is wanted: priorities,
run-to-completion, a state machine. The one obligation beyond those three is
that a custom assembly prologue, if any, must call
`cfiber_asan_on_fiber_entry()` when built under `CFIBER_ASAN`; see
[sanitizers.md](sanitizers.md).

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
caller is none the wiser. The [epoll reactor](reactor.md) is that idea built
out.

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
