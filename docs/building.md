# Building

## Requirements

- CMake 3.28 or newer
- A GNU-compatible C23 compiler. Tested with GCC 15 and Clang 22; older
  versions back to GCC 14 / Clang 19 should work but are unverified.
- For ARM cross-builds: `arm-none-eabi-gcc` and `qemu-system-arm`
- For AArch64 cross-builds: `aarch64-linux-gnu-gcc` and `qemu-user`

Select a compiler at configure time with `CC=clang cmake ...` or
`CC=clang ./utils/make.sh ...`.

## With CMake directly

```bash
cmake -B build -DCFIBER_BUILD_EXAMPLES=ON -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build
```

## With the convenience script

`utils/make.sh` handles native and cross builds plus QEMU execution.

```bash
./utils/make.sh -t -e                              # native, tests + examples
./utils/make.sh -t -e --sanitizer                  # with stack sanitizer (canary)
./utils/make.sh -t -e --asan                       # with AddressSanitizer (x86_64)
./utils/make.sh -t -e --ubsan                      # with UndefinedBehaviorSanitizer
./utils/make.sh -t --tsan                          # with ThreadSanitizer (reactor)
./utils/make.sh -t --reactor                       # build + run the reactor tests
./utils/make.sh -t -e -d                           # Debug build
./utils/make.sh -t -e --shared                     # shared library
./utils/make.sh -t -e --pic                        # static + PIC
./utils/make.sh --arch=aarch64 -t -e               # AArch64 via qemu-user
./utils/make.sh --arch=arm --cpu=cortex-m0 -t      # Cortex-M0
./utils/make.sh --arch=arm --cpu=cortex-m3 -t      # Cortex-M3
./utils/make.sh --arch=arm --cpu=cortex-m4 -t      # Cortex-M4
./utils/make.sh --arch=arm --cpu=cortex-m7 -t      # Cortex-M7 with FPU
./utils/make.sh --help
```

The ARM flows build the freestanding library, link it against the startup code
and linker script in `utils/cortex/`, and run the result under
`qemu-system-arm`. See [freestanding.md](freestanding.md) for what changes in
that mode.

## CMake options

| Option                              | Default | Description                                                  |
| ----------------------------------- | ------- | ------------------------------------------------------------ |
| `CFIBER_BUILD_EXAMPLES`             | `OFF`   | Build the standalone examples (`examples/`)                  |
| `BUILD_TESTS`                       | `OFF`   | Build the unit-test executables                              |
| `CFIBER_STACK_SANITIZER`            | `OFF`   | Enable canary + watermark instrumentation                    |
| `CFIBER_ASAN`                       | `OFF`   | Build with AddressSanitizer + fiber-aware instrumentation (hosted only; excludes `CFIBER_STACK_SANITIZER`) |
| `CFIBER_ASAN_REDZONE`               | -       | Guard size in bytes below each fiber stack (default: one cache line). Only used with `CFIBER_ASAN` |
| `CFIBER_UBSAN`                      | `OFF`   | Build with UndefinedBehaviorSanitizer; aborts on the first finding (hosted only; combinable with `CFIBER_ASAN`) |
| `CFIBER_TSAN`                       | `OFF`   | Build with ThreadSanitizer for the reactor's concurrency (hosted x86_64; excludes `CFIBER_ASAN`/`CFIBER_FUZZ`) |
| `CFIBER_FUZZ`                       | `OFF`   | Build the libFuzzer targets under ASan + UBSan (Clang + hosted only)        |
| `CFIBER_REACTOR`                    | `OFF`   | Build the optional epoll(7) reactor (`libcfiber_reactor`); Linux only       |
| `CFIBER_BUILD_SHARED`               | `OFF`   | Build as a shared library (`libcfiber.so`) instead of static |
| `CFIBER_POSITION_INDEPENDENT_CODE`  | `OFF`   | Build the static library with `-fPIC` (ignored when shared)  |
| `CFIBER_TARGET_CPU`                 | -       | `cortex-m0` / `cortex-m3` / `cortex-m4` / `cortex-m7`        |
| `CFIBER_ARM_FLOAT_ABI`              | -       | `soft` / `softfp` / `hard`                                   |
| `CFIBER_ARM_FPU`                    | -       | FPU name forwarded to `-mfpu` (e.g. `fpv5-sp-d16`)           |
| `CFIBER_SYSTEM_PROCESSOR`           | host    | Target architecture; `arm` selects the freestanding build    |

`BUILD_TESTS` is the one option without the `CFIBER_` prefix.

## Static, shared and PIC

The default is a static archive. Shared builds export only the documented API
(everything declared with `CFIBER_EXPORT` in the public headers) and hide
everything else via `-fvisibility=hidden`. Shared builds carry the project
version as `SOVERSION`.

The shared build is not available on the freestanding target, since bare-metal
Cortex-M has no dynamic loader.

If the static library will be linked into a downstream shared object, enable
`CFIBER_POSITION_INDEPENDENT_CODE` to avoid text-relocation errors.

## Consuming as a subdirectory

```cmake
add_subdirectory(external/cfiber)
target_link_libraries(my_app PRIVATE cfiber)
```

To switch to a shared build from a parent project:

```cmake
set(CFIBER_BUILD_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory(external/cfiber)
target_link_libraries(my_app PRIVATE cfiber)
```

The reactor is a separate target and needs its own link line:

```cmake
set(CFIBER_REACTOR ON CACHE BOOL "" FORCE)
add_subdirectory(external/cfiber)
target_link_libraries(my_app PRIVATE cfiber cfiber_reactor)
```

Sanitizer options propagate as PUBLIC usage requirements, so a `CFIBER_ASAN=ON`
build also compiles and links the consuming program with AddressSanitizer.
