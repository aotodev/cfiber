# Allocators and stacks

Fibers need two things allocated: a task record and a stack. cfiber provides
both without requiring `malloc`, which is what lets the same scheduler run on a
Linux host and on a Cortex-M0.

## slab

[include/cfiber/memory/slab_alloc.h](../include/cfiber/memory/slab_alloc.h)

A fixed-size block allocator over memory the user supplies. It carves a
contiguous region into `block_count` blocks of `block_size` and tracks
occupancy in a bitmap, so allocation and release are a scan and a bit flip with
no per-block header and no free-list pointer stealing space from the payload.

`block_size` must be a multiple of `CFIBER_CACHE_LINE_SIZE`, and the region must be
aligned to at least `alignof(max_align_t)`; blocks inherit the region's
alignment, so a cache-line-aligned region keeps two fibers' stacks off the same
line. The bitmap is `CFIBER_BITMAP_SIZE` words (also the CMake option), capping
one slab at `CFIBER_SLAB_MAX_BLOCKS` blocks; the multislab exists to get past that cap.

The slab owns no memory. It is the right layer when the region is a static
array in `.bss` and the block count is known at build time.

## multislab

[include/cfiber/memory/multislab_alloc.h](../include/cfiber/memory/multislab_alloc.h)

A chain of slabs that grows on demand. Allocation walks the active list, and
only when every slab is full does it ask the backing allocator for another one,
so the common path is the slab path.

Two details are worth knowing:

- **Two lists, lazily maintained.** Slabs live on an active list or a full list,
  and a slab that becomes full is moved lazily rather than eagerly, so a
  full-then-immediately-freed block does not pay for two list splices. Each
  node records which list it is on, because "full" and "on the full list" are
  deliberately not the same thing.
- **Shrink hysteresis.** Empty slabs are not returned to the backing allocator
  immediately; a small reserve is kept so an allocation pattern that oscillates
  around a slab boundary does not turn into a stream of alloc/free pairs.

`max_slabs` caps growth. At zero, growth is bounded only by the backing
allocator.

The backing allocator is the `(alloc, free, ctx)` triple: every slab's memory
comes from it, and the free callback is handed the size back so a user allocator
does not need to record block sizes. It must return memory aligned to at least
`alignof(max_align_t)`; the default one returns cache-line-aligned memory. This
is the hook the scheduler exposes as `cfiber_scheduler_init_ext()`; see
[freestanding.md](freestanding.md).

Every stack allocator fills a `cfiber_stack_t` whose `usable_base` marks where the
fiber's stack starts: above the guard page of a growable stack, above the ASan
redzone of a fixed-size one. Schedulers hand `cfiber_init()` and the sanitizers
the usable range, never `mem_base`.

## Fixed-size stacks

[include/cfiber/stack/fixed_size_stack_allocator.h](../include/cfiber/stack/fixed_size_stack_allocator.h)

A thin pairing of a multislab with the `cfiber_stack_t` descriptor in
[stack.h](../include/cfiber/stack/stack.h): `cfiber_fixed_stack_alloc()` takes a block and
fills in the descriptor, `cfiber_fixed_stack_release()` hands it back. When the stack
sanitizer is enabled it is also where the canary is planted and the watermark
pattern written, so instrumentation costs the caller nothing at the call site.

This is the only stack allocator available on freestanding targets, and the one
the built-in scheduler uses everywhere.

## Growable stacks (hosted)

[include/cfiber/stack/growable_stack.h](../include/cfiber/stack/growable_stack.h),
[growable_stack_allocator.h](../include/cfiber/stack/growable_stack_allocator.h)

On a host with an MMU, a fiber does not have to commit its worst-case stack up
front. `cfiber_growable_stack_create(max_size)` maps `max_size` plus one page with
`MAP_NORESERVE | MAP_STACK`, then `mprotect`s the lowest page to `PROT_NONE`.

Growth is therefore demand paging, not a fault handler: the mapping is readable
and writable across its whole extent, and the kernel commits a physical page the
first time the fiber's stack pointer reaches it. `MAP_NORESERVE` keeps the
untouched remainder from counting against commit accounting. An idle fiber costs
the pages it has actually touched, not the pages it might touch, which is what
makes tens of thousands of reactor fibers with a 64 KB ceiling practical.

The `PROT_NONE` page below is a hard floor, not part of the growth path. Running
off the bottom of the stack lands in it and faults immediately, at the
instruction that overflowed, instead of silently corrupting whatever mapping
happened to sit below.

`cfiber_growable_stack_allocator_t` pools these. It keeps up to `cache_capacity` stacks
for reuse and can pre-allocate `initial_cached` of them. A stack returned to the
pool is recycled with `MADV_DONTNEED` over its grown pages, keeping the top page
warm: that drops the physical memory while leaving the mapping and the guard
page in place, so reuse costs no `mmap` and the next fiber starts from a small
resident footprint. `cfiber_growable_stack_allocator_destroy()` returns `-1` if stacks
are still outstanding rather than unmapping memory still in use.

This is the reactor's stack source, and the one component that is hosted-only:
it needs an MMU.

Compiling consumers with `-fstack-clash-protection` (or `-fstack-check`) is
recommended. A single stack frame larger than a page can otherwise step clean
over the guard page and touch memory beyond it without ever faulting.
