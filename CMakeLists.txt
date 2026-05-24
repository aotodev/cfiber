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

option(BUILD_SAMPLE                     "Build sample scheduler"                             OFF)
option(BUILD_TESTS                      "Build unit tests"                                   OFF)
option(CFIBER_STACK_SANITIZER           "Enable stack canary + watermark for non-MMU stacks" OFF)
option(CFIBER_BUILD_SHARED              "Build cfiber as a shared library (default: static)" OFF)
option(CFIBER_POSITION_INDEPENDENT_CODE "Build the static library with -fPIC (ignored when shared)" OFF)

set(CFIBER_TARGET_CPU "" CACHE STRING
    "Target CPU when cross-compiling for ARM (e.g. cortex-m0, cortex-m3, cortex-m4, cortex-m7)")

set(CFIBER_ARM_FLOAT_ABI "" CACHE STRING "ARM float ABI (soft, softfp, hard)")
set(CFIBER_ARM_FPU       "" CACHE STRING "ARM FPU (e.g. fpv4-sp-d16, fpv5-sp-d16)")

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
# project_flags — shared compiler / linker flags consumed by every target
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
)

target_include_directories(cfiber PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(cfiber PRIVATE project_flags)

# Fixed-size stack allocator works on both hosted and freestanding; it only
# wraps a multislab and a cstack_t descriptor.
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

    # ABI-affecting flags must be visible to every translation unit (including
    # assembly), so they go on add_compile_options. They also need to be on the
    # link line for libgcc selection.
    if(CFIBER_ARM_FLOAT_ABI)
        add_compile_options(-mfloat-abi=${CFIBER_ARM_FLOAT_ABI})
        add_link_options(-mfloat-abi=${CFIBER_ARM_FLOAT_ABI})
        if(CFIBER_ARM_FLOAT_ABI STREQUAL "hard")
            target_compile_definitions(cfiber PUBLIC CFIBER_ARM_FPU)
        endif()
    endif()

    if(CFIBER_ARM_FPU)
        add_compile_options(-mfpu=${CFIBER_ARM_FPU})
        add_link_options(-mfpu=${CFIBER_ARM_FPU})
    endif()
else()
    message(FATAL_ERROR "Unsupported architecture: '${CFIBER_SYSTEM_PROCESSOR}'")
endif()

if(CFIBER_TARGET_CPU)
    add_compile_options(-mcpu=${CFIBER_TARGET_CPU})
    add_link_options(-mcpu=${CFIBER_TARGET_CPU})
endif()

# ---------------------------------------------------------------------------------------
# Optional targets
# ---------------------------------------------------------------------------------------
if(BUILD_SAMPLE)
    add_subdirectory(sample)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
