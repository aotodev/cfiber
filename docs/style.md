# Coding style

What the tools cannot enforce. Formatting is `clang-format` (repo
`.clang-format`: LLVM base, 4 spaces, 120 columns, attached braces, left
pointer alignment), lint is `clang-tidy` (repo `.clang-tidy`), and
`pre-commit` runs both plus whitespace, shellcheck, cmake-lint and yamllint.
Everything below is the part a reviewer still has to check.

Priorities, in order: correctness, robustness, performance, readability.
When two rules conflict, the earlier priority wins.

## Language

- C23. In use: `constexpr`, `nullptr`, `static_assert`, `thread_local`,
  `[[noreturn]]`, `[[maybe_unused]]`, `#elifdef`. No `<stdbool.h>`.
- GCC/Clang attributes are fine where they carry meaning: `nonnull` on every
  pointer parameter that must not be NULL, `visibility` through `CFIBER_EXPORT`
  and `CFIBER_HIDDEN`, `aligned` where the layout needs it.
- `[[nodiscard]]` only on functions that allocate or hand out ownership.
- No heap allocation outside the allocators and the documented backing-allocator
  hooks. No stdio, `abort` or `exit` in the library.
- Type punning through `memcpy`, never through a cast. Pointer arithmetic on
  `uintptr_t`, with the overflow checked before the add.
- `LIKELY` / `UNLIKELY` on hot paths only (the switch, the allocators' fast
  path), not as decoration.

## Naming

- Every public identifier is prefixed: functions and types `cfiber_`, macros and
  constants `CFIBER_`. Types end in `_t`, function-pointer typedefs in `_fn`.
- Functions read `cfiber_<module>_<verb>`: `cfiber_slab_alloc`,
  `cfiber_scheduler_spawn`, `cfiber_growable_stack_release`. The reactor keeps
  two prefixes by design: `cfiber_reactor_*` takes the reactor and is the
  host-side API, `cfiber_ev_*` is the in-fiber API acting on the reactor that
  runs the caller (see [reactor.md](reactor.md)).
- Private code (static functions, `src/cfiber/core/internal.h`) is unprefixed:
  `bitmap_find_free`, `ASSERT`, `align_up`.
- File-scope statics: `s_` in the library, `g_` in tests and examples.
- Constants are `constexpr` when a type fits (`CFIBER_SLAB_MAX_BLOCKS`), macros
  when they must work in the preprocessor (`CFIBER_CACHE_LINE_SIZE`).
- Header guards `CFIBER_<PATH>_H`. Files and directories `snake_case`;
  per-architecture assembly is `<concern>_<arch>.S`.
- Do not misspell; a public name is forever.

## Headers

Every file opens with the two SPDX lines (copyright text, then the `MIT`
license identifier) in the file's comment syntax; copy them from any neighbour.
Markdown and the dotfiles are covered by `REUSE.toml` instead, and `reuse lint`
runs in pre-commit. Then, in a header: doxygen `@file` / `@brief` block, include guard,
includes, `extern "C"`, declarations. Includes come in three groups separated by
a blank line: the file's own header (in `.c` files), project headers, system
headers; `clang-format` sorts within each group.

- One real header per subsystem under `include/cfiber/<module>/`, a one-line
  umbrella beside it (`include/cfiber/<module>.h`).
- Every exported function carries `CFIBER_EXPORT` in its declaration.
- Public headers include only public headers. Library-private helpers live in
  `src/cfiber/core/internal.h`; `src/` is a private include directory.
- Every public declaration has a doxygen comment: `@brief` always; `@param`,
  `@return`, `@pre`, `@note`, `@warning` when they add a fact. Document the
  contract (alignment, ownership, may it fail, what happens on misuse), not the
  implementation.
- Public structs stay fully visible so they can be embedded or allocated
  statically; the price is that layout-affecting build options are PUBLIC
  defines (`CFIBER_BITMAP_SIZE`, `CFIBER_ASAN_REDZONE`) and are listed in
  [building.md](building.md).

## Errors and contracts

- Setup and init functions return `int`: 0 or -1. `errno` is set only where a
  syscall is involved (growable stacks, reactor); pure-configuration failures
  return -1 with nothing allocated.
- Release, wake and cancel return `bool`: `false` means the caller's misuse or a
  full queue was detected and nothing was done.
- Allocation returns the object or `NULL`.
- Public API guards return an error under `CFIBER_DEFENSIVE` (the default) and
  are `ASSERT`s otherwise. `ASSERT` traps in debug builds and compiles out under
  `NDEBUG`; it is for library invariants, never for user input.
- A state the library cannot recover from traps in every build with
  `__builtin_trap()`: a fiber returning with no hook, the scheduler finding no
  runnable fiber while some are live. Silent continuation is worse than a crash.
- Nothing in the library prints, aborts or exits.

## Code shape

- Early return; one level of nesting per idea.
- `if (rc)`, `if (!p)` for booleans and pointers. Keep the explicit comparison
  where 0 is a real value: `strcmp(...) == 0`, `n != 0` for counts, `x % y == 0`.
- Declare at first use; `const` on locals that do not change.
- Multi-step setup unwinds through a single `fail:` label; everywhere else,
  return early.
- Check every arithmetic on sizes for overflow before it happens, then use the
  result.
- Section banners (`/* ===== */`) split a `.c` file into its concerns; use them
  for files above roughly a hundred lines.

## Comments

A comment earns its line by telling the reader something the signature and body
do not: a precondition, an invariant, an ordering constraint, a decision, a trap
an innocent cleanup would spring. No narration of what the code does, no design
rationale (that goes to `docs/`), no selling.

- `/* ... */` for comments, `/** ... */` for doxygen. `//` only in `// NOLINT`
  markers and the `@code` samples.
- Short trailing comments are fine: `/* errno from sysconf */`.
- Suppress a lint with the narrowest `NOLINT` form (`NOLINTNEXTLINE(check)` or a
  `NOLINTBEGIN`/`NOLINTEND` pair) and say why in the same comment.

## Assembly

- One `.S` per architecture and concern; the C-level signature in a comment at
  the top; every function ends with `.size`.
- x86_64: `.intel_syntax noprefix`. Cortex-M: `.syntax unified`, `.thumb`,
  `.thumb_func`. Hosted files end with a `.note.GNU-stack` section.
- Internal entry points are `.hidden`. Field offsets follow
  `cfiber_context_t` in `context.h`; change both together.
- Sanitizer hooks are gated on `#if CFIBER_ASAN_ENABLED`, the same define the
  C side uses.

## Tests

- Framework in `tests/test/test.h`. A test is `static int test_<area>_<behaviour>(void)`
  returning 0, run with `RUN_TEST`, using the fatal `ASSERT_*` macros. A suite
  with no tests fails.
- Fibers only record into a struct; `main` (or the test function) asserts after
  the run. No asserting from inside a fiber.
- Misuse that must trap is a death test (`ASSERT_DEATH`, forked child, hosted
  only). `tests/defensive` rebuilds the library sources with `NDEBUG` to cover
  the release-mode guards.
- A behaviour change ships with a test that fails on the previous sources. Run
  it against them before claiming it does.
- Every configuration in the matrix must pass before a change is done: gcc and
  clang Release, Debug with ASan, ASan+UBSan, TSan, shared, the stack sanitizer,
  aarch64 under QEMU and Cortex-M0/M3/M4/M7 under QEMU. `utils/make.sh` is the
  entry point; see [testing.md](testing.md).

## Documentation

- The README says what the project is, what it offers, how to build it and that
  it is tested. Everything else is one page per topic under `docs/`, linked
  from the README in a line each.
- Terse and technical: proper vocabulary over explanation, shorter when nothing
  is lost. Link to the header where the declaration lives rather than restating
  it.
- Build files (CMake, workflows, Containerfile) are configuration, not
  narrative: one-line comments, only for what an innocent cleanup would break.

## Branches and commits

- One issue per branch, named `<gh-number>-<slug>`; one pull request per issue.
- Commit subject `<area>: <summary>`, imperative, lower case, no trailing period:
  `reactor: wake targets async parks only, total deadlines, saturating timers`.
  The body, when needed, says what changed and why in plain sentences and ends
  with `Closes #N`.
- CI: one workflow per target so each keeps its badge; the toolchain image has
  its own producer workflow; test workflows only consume it; push triggers are
  constrained to branches; each workflow watches its own file.
