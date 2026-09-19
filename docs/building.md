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
cmake -B build -DCFIBER_BUILD_EXAMPLES=ON -DCFIBER_BUILD_TESTS=ON
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
./utils/make.sh -t --tsan                          # with ThreadSanitizer (implies --reactor)
./utils/make.sh -t --reactor                       # build + run the reactor tests
./utils/make.sh -t -e -d                           # Debug build
./utils/make.sh -t -e --shared                     # shared library
./utils/make.sh -t -e --pic                        # static + PIC
./utils/make.sh --arch=aarch64 -t -e               # AArch64 via qemu-user
./utils/make.sh --arch=arm --cpu=cortex-m0 -t      # Cortex-M0
./utils/make.sh --arch=arm --cpu=cortex-m3 -t      # Cortex-M3
./utils/make.sh --arch=arm --cpu=cortex-m4 -t      # Cortex-M4, soft float
./utils/make.sh --arch=arm --cpu=cortex-m4 -t --float-abi=softfp --fpu=fpv4-sp-d16  # M4F
./utils/make.sh --arch=arm --cpu=cortex-m7 -t      # Cortex-M7, hard float + FPU
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
| `CFIBER_BUILD_TESTS`                | `OFF`   | Build the unit-test executables (`BUILD_TESTS` still accepted, deprecated) |
| `CFIBER_INSTALL`                    | top-level | Generate install rules, the CMake package and `cfiber.pc` |
| `CFIBER_STACK_SANITIZER`            | `OFF`   | Enable canary + watermark instrumentation                    |
| `CFIBER_ASAN`                       | `OFF`   | Build with AddressSanitizer + fiber-aware instrumentation (hosted only; excludes `CFIBER_STACK_SANITIZER`) |
| `CFIBER_ASAN_REDZONE`               | -       | Guard size in bytes below each fiber stack (default: one cache line). Only used with `CFIBER_ASAN` |
| `CFIBER_BITMAP_SIZE`                | `8`     | Bitmap words per slab, so the per-slab block cap. Layout-affecting: propagated to consumers as a PUBLIC define |
| `CFIBER_UBSAN`                      | `OFF`   | Build with UndefinedBehaviorSanitizer; aborts on the first finding (hosted only; combinable with `CFIBER_ASAN`) |
| `CFIBER_TSAN`                       | `OFF`   | Build with ThreadSanitizer for the reactor's concurrency (hosted x86_64; excludes `CFIBER_ASAN`/`CFIBER_FUZZ`) |
| `CFIBER_FUZZ`                       | `OFF`   | Build the libFuzzer targets under ASan + UBSan (Clang + hosted only)        |
| `CFIBER_REACTOR`                    | `OFF`   | Build the optional epoll(7) reactor (`libcfiber_reactor`); Linux only       |
| `CFIBER_BUILD_SHARED`               | `OFF`   | Build as a shared library (`libcfiber.so`) instead of static |
| `CFIBER_POSITION_INDEPENDENT_CODE`  | `OFF`   | Build the static library with `-fPIC` (ignored when shared)  |
| `CFIBER_TARGET_CPU`                 | -       | `cortex-m0` / `cortex-m3` / `cortex-m4` / `cortex-m7`        |
| `CFIBER_ARM_FLOAT_ABI`              | -       | `soft` / `softfp` / `hard`                                   |
| `CFIBER_ARM_FPU_NAME`               | -       | FPU name forwarded to `-mfpu` (e.g. `fpv5-sp-d16`)           |
| `CFIBER_SYSTEM_PROCESSOR`           | host    | Target architecture; `arm` selects the freestanding build    |

On Cortex-M the saved context includes `s16`-`s31` whenever the compiler
targets an FPU (`__ARM_FP`, i.e. `-mfpu` with `softfp` or `hard`), so the
library and everything linked against it must be built with the same
`CFIBER_ARM_FLOAT_ABI` and `CFIBER_ARM_FPU_NAME`.

## Static, shared and PIC

The default is a static archive. Shared builds export only the documented API
(everything declared with `CFIBER_EXPORT` in the public headers) and hide
everything else via `-fvisibility=hidden`. Shared builds carry the major
version as `SOVERSION`: once 1.0 is tagged, the ABI is stable within a major,
and the layout-affecting options (`CFIBER_BITMAP_SIZE`, `CFIBER_ASAN_REDZONE`,
`CFIBER_STACK_SANITIZER`, the ARM float ABI) must match between library and
consumer. Before 1.0 any release may change it.

The shared build is not available on the freestanding target, since bare-metal
Cortex-M has no dynamic loader.

If the static library will be linked into a downstream shared object, enable
`CFIBER_POSITION_INDEPENDENT_CODE` to avoid text-relocation errors.

## Installing and find_package

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/cfiber
cmake --build build -j
cmake --install build
```

installs the library, the headers (including the generated `cfiber/version.h`
with `CFIBER_VERSION_MAJOR/MINOR/PATCH` and `CFIBER_VERSION_STRING`), a CMake
package and `cfiber.pc`. A consumer then needs only:

```cmake
find_package(cfiber CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE cfiber::cfiber)        # and cfiber::reactor
```

or `pkg-config --cflags --libs cfiber`. The exported targets carry the PUBLIC
definitions and options of the build that was installed (`CFIBER_BITMAP_SIZE`,
sanitizer flags, the ARM float ABI), so a consumer matches it without repeating
them. `tests/consumer/` is the smoke test CI runs against a staged install.

## Consuming as a subdirectory

```cmake
add_subdirectory(external/cfiber)
target_link_libraries(my_app PRIVATE cfiber::cfiber)
```

As a subproject cfiber builds only the library: tests and examples stay off
unless `CFIBER_BUILD_TESTS` / `CFIBER_BUILD_EXAMPLES` are set, and install
rules are off unless `CFIBER_INSTALL` is. The ARM ABI flags are PUBLIC on the
target, so the parent's own translation units and link line get the same
`-mcpu`, `-mfloat-abi` and `-mfpu`.

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
