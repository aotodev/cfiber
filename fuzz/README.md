# Fuzzing

Coverage-guided [libFuzzer](https://llvm.org/docs/LibFuzzer.html) targets that
exercise the allocator and scheduler under AddressSanitizer +
UndefinedBehaviorSanitizer. Requires **Clang**.

## Targets

| Target           | Exercises                                                            |
|------------------|---------------------------------------------------------------------|
| `fuzz_multislab` | slab / multislab alloc / release op-streams + structural invariants |
| `fuzz_scheduler` | spawn / yield / dynamic-spawn / run, with leak balance              |

Each target reads libFuzzer's random bytes as an opcode stream, issues only
valid operations (so the library's defensive `ASSERT` paths are never tripped),
and aborts via `FUZZ_CHECK` when a structural invariant is violated — ASan/UBSan
catch the memory/UB bugs, the invariants catch the logic bugs.

## Build & run

```sh
cmake -B build-fuzz -DCFIBER_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz -j

# run a target (Ctrl-C to stop); keep an evolving corpus on disk
mkdir -p corpus/multislab
./build-fuzz/fuzz/fuzz_multislab corpus/multislab

# time-boxed run (e.g. in CI)
./build-fuzz/fuzz/fuzz_scheduler -max_total_time=60 corpus/scheduler
```

A crash writes the triggering input to `crash-<hash>`; replay it with:

```sh
./build-fuzz/fuzz/fuzz_multislab crash-<hash>
```
