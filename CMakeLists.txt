cmake_minimum_required(VERSION 3.28 FATAL_ERROR)
project(cfiber VERSION 0.1.0 LANGUAGES C ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------------------
# Global properties and options
# ---------------------------------------------------------------------------------------
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

option(CFIBER_BUILD_EXAMPLES            "Build the standalone examples"                      OFF)
option(BUILD_TESTS                      "Build unit tests"                                   OFF)
option(CFIBER_STACK_SANITIZER           "Enable stack canary + watermark for non-MMU stacks" OFF)
option(CFIBER_ASAN                      "Build with AddressSanitizer + fiber-aware instrumentation (hosted only)" OFF)
option(CFIBER_UBSAN                     "Build with UndefinedBehaviorSanitizer (hosted only; combinable with CFIBER_ASAN)" OFF)
option(CFIBER_TSAN                      "Build with ThreadSanitizer (hosted only; for the reactor's concurrency; excludes CFIBER_ASAN)" OFF)
option(CFIBER_FUZZ                      "Build libFuzzer fuzz targets under ASan + UBSan (Clang + hosted only)" OFF)
option(CFIBER_REACTOR                   "Build the optional epoll(7) reactor scheduler (Linux only)" OFF)

set(CFIBER_ASAN_REDZONE "" CACHE STRING
    "Size in bytes of the poisoned guard below each fiber stack (default: one cache line). Only used when CFIBER_ASAN is ON.")
option(CFIBER_BUILD_SHARED              "Build cfiber as a shared library (default: static)" OFF)
option(CFIBER_POSITION_INDEPENDENT_CODE "Build the static library with -fPIC (ignored when shared)" OFF)

set(CFIBER_TARGET_CPU "" CACHE STRING
    "Target CPU when cross-compiling for ARM (e.g. cortex-m0, cortex-m3, cortex-m4, cortex-m7)")

set(CFIBER_ARM_FLOAT_ABI "" CACHE STRING "ARM float ABI (soft, softfp, hard)")
set(CFIBER_ARM_FPU_NAME  "" CACHE STRING "ARM FPU forwarded to -mfpu (e.g. fpv4-sp-d16, fpv5-sp-d16)")

set(CFIBER_SYSTEM_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING
    "Target arch (x86_64, aarch64, arm, ...)")

# Freestanding mode is selected by architecture: bare-metal ARM Cortex-M.
if(CFIBER_SYSTEM_PROCESSOR MATCHES "arm")
    set(CFIBER_FREESTANDING ON)
else()
    set(CFIBER_FREESTANDING OFF)
endif()

if(CFIBER_FREESTANDING AND CFIBER_BUILD_SHARED)
    message(FATAL_ERROR
        "CFIBER_BUILD_SHARED is incompatible with the freestanding (bare-metal) build: "
        "no dynamic loader is available on Cortex-M targets.")
endif()

# ---------------------------------------------------------------------------------------
# ccache (optional)
# ---------------------------------------------------------------------------------------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# ---------------------------------------------------------------------------------------
# project_flags: shared compiler / linker flags consumed by every target
# ---------------------------------------------------------------------------------------
add_library(project_flags INTERFACE)

target_compile_options(project_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-Wextra>
    $<$<COMPILE_LANGUAGE:C>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:C>:-Wshadow>
    $<$<COMPILE_LANGUAGE:C>:-Wconversion>
    $<$<COMPILE_LANGUAGE:C>:-Werror>
    $<$<CONFIG:Debug>:-ggdb3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O2>
)

if(CFIBER_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(project_flags INTERFACE
        $<$<COMPILE_LANGUAGE:C>:-masm=intel>
    )
endif()

# ---------------------------------------------------------------------------------------
# cfiber library (fibers + memory/slab allocators + stack + scheduler)
# ---------------------------------------------------------------------------------------
if(CFIBER_BUILD_SHARED)
    add_library(cfiber SHARED)
    set_target_properties(cfiber PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
else()
    add_library(cfiber STATIC)
    if(CFIBER_POSITION_INDEPENDENT_CODE)
        set_target_properties(cfiber PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

set(_cfiber_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/src/cfiber)

target_sources(cfiber PRIVATE
    ${_cfiber_src_dir}/fiber/fiber.c
    ${_cfiber_src_dir}/memory/slab_alloc.c
    ${_cfiber_src_dir}/memory/multislab_alloc.c
    ${_cfiber_src_dir}/scheduler/scheduler.c
    ${_cfiber_src_dir}/debug/asan.c
)

target_include_directories(cfiber PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(cfiber PRIVATE project_flags)

# Fixed-size stack allocator: hosted and freestanding both.
target_sources(cfiber PRIVATE
    ${_cfiber_src_dir}/stack/fixed_size_stack_allocator.c
)

# Growable stacks need mmap + SIGSEGV; hosted only.
if(NOT CFIBER_FREESTANDING)
    target_sources(cfiber PRIVATE
        ${_cfiber_src_dir}/stack/growable_stack.c
        ${_cfiber_src_dir}/stack/growable_stack_allocator.c
    )
    target_compile_definitions(cfiber PRIVATE _GNU_SOURCE)
else()
    target_compile_definitions(cfiber PUBLIC CFIBER_FREESTANDING=1)
endif()

if(CFIBER_STACK_SANITIZER)
    target_sources(cfiber PRIVATE
        ${_cfiber_src_dir}/stack/debug/canary.c
        ${_cfiber_src_dir}/stack/debug/watermark.c
    )
    target_compile_definitions(cfiber PUBLIC CFIBER_STACK_SANITIZER=1)
endif()

# ---------------------------------------------------------------------------------------
# AddressSanitizer (hosted only). Flags are PUBLIC because a partially instrumented
# program misbehaves: consumers of the static library must build with ASan too.
# ---------------------------------------------------------------------------------------
if(CFIBER_ASAN)
    if(CFIBER_STACK_SANITIZER)
        message(FATAL_ERROR
            "CFIBER_ASAN and CFIBER_STACK_SANITIZER are mutually exclusive: the canary "
            "word lives where the ASan redzone is placed, and the two detectors target "
            "different platforms (canary = freestanding/Cortex-M, ASan = hosted).")
    endif()
    if(CFIBER_FREESTANDING)
        message(FATAL_ERROR
            "CFIBER_ASAN requires a hosted target; AddressSanitizer is not available on "
            "bare-metal Cortex-M. Use CFIBER_STACK_SANITIZER there instead.")
    endif()

    target_compile_options(cfiber PUBLIC
        $<$<COMPILE_LANGUAGE:C,ASM>:-fsanitize=address>
        $<$<COMPILE_LANGUAGE:C>:-fno-omit-frame-pointer>
    )
    target_link_options(cfiber PUBLIC -fsanitize=address)

    # PUBLIC: the layout must match in any consumer including cfiber/debug/asan.h.
    if(CFIBER_ASAN_REDZONE)
        target_compile_definitions(cfiber PUBLIC
            "CFIBER_ASAN_REDZONE=((size_t)${CFIBER_ASAN_REDZONE})")
    endif()
endif()

# ---------------------------------------------------------------------------------------
# UndefinedBehaviorSanitizer (hosted). Composes with ASan.
# ---------------------------------------------------------------------------------------
if(CFIBER_UBSAN)
    if(CFIBER_FREESTANDING)
        message(FATAL_ERROR
            "CFIBER_UBSAN requires a hosted target; the UBSan runtime is unavailable on "
            "bare-metal Cortex-M.")
    endif()
    if(CFIBER_FUZZ)
        message(FATAL_ERROR
            "CFIBER_FUZZ already enables UndefinedBehaviorSanitizer; do not set CFIBER_UBSAN too.")
    endif()

    target_compile_options(cfiber PUBLIC
        $<$<COMPILE_LANGUAGE:C,ASM>:-fsanitize=undefined>
        $<$<COMPILE_LANGUAGE:C>:-fno-sanitize-recover=undefined>
        $<$<COMPILE_LANGUAGE:C>:-fno-omit-frame-pointer>
    )
    target_link_options(cfiber PUBLIC -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------------------
# ThreadSanitizer (hosted). Validation only: the flags go on cfiber_reactor, never
# library-wide. TSan's instrumentation uses the callee-saved registers the
# register-preservation tests poke.
# ---------------------------------------------------------------------------------------
if(CFIBER_TSAN)
    if(CFIBER_FREESTANDING)
        message(FATAL_ERROR
            "CFIBER_TSAN requires a hosted target; the ThreadSanitizer runtime is "
            "unavailable on bare-metal Cortex-M.")
    endif()
    if(CFIBER_ASAN)
        message(FATAL_ERROR
            "CFIBER_TSAN and CFIBER_ASAN are mutually exclusive: the two sanitizers use "
            "incompatible shadow-memory runtimes and cannot be linked together.")
    endif()
    if(CFIBER_FUZZ)
        message(FATAL_ERROR
            "CFIBER_FUZZ enables AddressSanitizer, which is incompatible with "
            "ThreadSanitizer; do not combine CFIBER_TSAN with CFIBER_FUZZ.")
    endif()
    if(NOT CFIBER_REACTOR)
        message(FATAL_ERROR
            "CFIBER_TSAN targets the reactor's concurrency; enable it together with "
            "-DCFIBER_REACTOR=ON.")
    endif()
endif()

# ---------------------------------------------------------------------------------------
# Fuzzing (libFuzzer), under ASan + UBSan.
# ---------------------------------------------------------------------------------------
if(CFIBER_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "CFIBER_FUZZ requires Clang (libFuzzer). Re-configure with -DCMAKE_C_COMPILER=clang.")
    endif()
    if(CFIBER_FREESTANDING)
        message(FATAL_ERROR "CFIBER_FUZZ requires a hosted target; libFuzzer is not available bare-metal.")
    endif()
    if(CFIBER_ASAN OR CFIBER_STACK_SANITIZER)
        message(FATAL_ERROR
            "CFIBER_FUZZ already enables AddressSanitizer; do not combine it with "
            "CFIBER_ASAN or CFIBER_STACK_SANITIZER.")
    endif()

    # fuzzer-no-link: coverage without the libFuzzer entry point, which the fuzz
    # executables supply themselves via -fsanitize=fuzzer.
    target_compile_options(cfiber PUBLIC
        $<$<COMPILE_LANGUAGE:C,ASM>:-fsanitize=address>
        $<$<COMPILE_LANGUAGE:C>:-fsanitize=undefined,fuzzer-no-link>
        $<$<COMPILE_LANGUAGE:C>:-fno-sanitize-recover=undefined>
        $<$<COMPILE_LANGUAGE:C>:-fno-omit-frame-pointer>
    )
    target_link_options(cfiber PUBLIC -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------------------
# Architecture-specific assembly + ABI flags
# ---------------------------------------------------------------------------------------
if(CFIBER_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(cfiber PRIVATE
        ${_cfiber_src_dir}/fiber/context_x86_64.S
        ${_cfiber_src_dir}/fiber/fiber_prologue_x86_64_fiber.S
    )

elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_sources(cfiber PRIVATE
        ${_cfiber_src_dir}/fiber/context_aarch64.S
        ${_cfiber_src_dir}/fiber/fiber_prologue_aarch64_fiber.S
    )

elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "arm")
    target_compile_options(cfiber PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-ffunction-sections>
        $<$<COMPILE_LANGUAGE:C>:-fdata-sections>
    )

    if(CFIBER_TARGET_CPU MATCHES "^cortex-m0$")
        target_sources(cfiber PRIVATE
            ${_cfiber_src_dir}/fiber/context_armv6-m.S
            ${_cfiber_src_dir}/fiber/fiber_prologue_arm_cortex_fiber.S
        )
    elseif(CFIBER_TARGET_CPU MATCHES "^cortex-m(3|4|7)")
        target_sources(cfiber PRIVATE
            ${_cfiber_src_dir}/fiber/context_armv7-m.S
            ${_cfiber_src_dir}/fiber/fiber_prologue_arm_cortex_fiber.S
        )
    else()
        message(FATAL_ERROR "Unsupported or unset CFIBER_TARGET_CPU: '${CFIBER_TARGET_CPU}'")
    endif()

    # ABI/arch-affecting flags must reach every translation unit (including
    # assembly) and the link line (libgcc multilib selection). The FPU half of
    # context_t follows the compiler's __ARM_FP, so consumers must use the same
    # -mfloat-abi/-mfpu as the library.
    if(CFIBER_ARM_FLOAT_ABI)
        add_compile_options(-mfloat-abi=${CFIBER_ARM_FLOAT_ABI})
        add_link_options(-mfloat-abi=${CFIBER_ARM_FLOAT_ABI})
        target_compile_options(cfiber PRIVATE -mfloat-abi=${CFIBER_ARM_FLOAT_ABI})
    endif()

    if(CFIBER_ARM_FPU_NAME)
        add_compile_options(-mfpu=${CFIBER_ARM_FPU_NAME})
        add_link_options(-mfpu=${CFIBER_ARM_FPU_NAME})
        target_compile_options(cfiber PRIVATE -mfpu=${CFIBER_ARM_FPU_NAME})
    endif()
else()
    message(FATAL_ERROR "Unsupported architecture: '${CFIBER_SYSTEM_PROCESSOR}'")
endif()

if(CFIBER_TARGET_CPU)
    add_compile_options(-mcpu=${CFIBER_TARGET_CPU})
    add_link_options(-mcpu=${CFIBER_TARGET_CPU})
    target_compile_options(cfiber PRIVATE -mcpu=${CFIBER_TARGET_CPU})
endif()

# ---------------------------------------------------------------------------------------
# Optional epoll(7) reactor (Linux only): a second scheduler over the same fiber
# mechanism, registering its return hook at run time.
# ---------------------------------------------------------------------------------------
if(CFIBER_REACTOR)
    if(CFIBER_FREESTANDING)
        message(FATAL_ERROR
            "CFIBER_REACTOR requires a hosted target; epoll/eventfd are unavailable on "
            "bare-metal Cortex-M.")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "CFIBER_REACTOR is Linux only (it uses epoll(7) and eventfd(2)); "
            "current target is '${CMAKE_SYSTEM_NAME}'.")
    endif()

    find_package(Threads REQUIRED)

    if(CFIBER_BUILD_SHARED)
        add_library(cfiber_reactor SHARED)
        set_target_properties(cfiber_reactor PROPERTIES
            VERSION   ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}
        )
    else()
        add_library(cfiber_reactor STATIC)
        if(CFIBER_POSITION_INDEPENDENT_CODE)
            set_target_properties(cfiber_reactor PROPERTIES POSITION_INDEPENDENT_CODE ON)
        endif()
    endif()

    target_sources(cfiber_reactor PRIVATE
        ${_cfiber_src_dir}/reactor/reactor.c
    )
    target_compile_definitions(cfiber_reactor PRIVATE _GNU_SOURCE)
    target_link_libraries(cfiber_reactor PUBLIC cfiber Threads::Threads PRIVATE project_flags)

    # PUBLIC so the reactor's dependents (tests, ws_echo) compile and link with TSan.
    if(CFIBER_TSAN)
        target_compile_options(cfiber_reactor PUBLIC
            $<$<COMPILE_LANGUAGE:C>:-fsanitize=thread>
            $<$<COMPILE_LANGUAGE:C>:-fno-omit-frame-pointer>
        )
        target_link_options(cfiber_reactor PUBLIC -fsanitize=thread)
    endif()
endif()

# ---------------------------------------------------------------------------------------
# Optional targets
# ---------------------------------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Unconditional: the subdirectory gates each example itself, and ws_echo's self-test
# is the reactor's end-to-end test.
add_subdirectory(examples)

if(CFIBER_FUZZ)
    add_subdirectory(fuzz)
endif()
