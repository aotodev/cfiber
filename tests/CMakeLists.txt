# SPDX-FileCopyrightText: 2026 Aoto
# SPDX-License-Identifier: MIT

include(arm_executable)

# ======================================================================================
# Shared test framework (counters, runner, assertions)
# ======================================================================================
add_library(cfiber_test STATIC test/test.c)
target_include_directories(cfiber_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# cfiber: the framework follows the library's hosted/freestanding split
# (CFIBER_FREESTANDING) for its fork-based death helper.
target_link_libraries(cfiber_test PUBLIC cfiber project_flags)

# add_test() can only run the binary on the build host, so register CTest cases
# only for native builds. Cross-compiled targets (arm, aarch64) are run through
# QEMU by utils/make.sh.
set(_cfiber_register_ctest FALSE)
if(NOT CMAKE_CROSSCOMPILING)
    set(_cfiber_register_ctest TRUE)
endif()

# ======================================================================================
# cfiber tests (context switch / register preservation)
# ======================================================================================
set(_cfiber_test_name "unit_tests_${_cfiber_arch}")

if(_cfiber_arch STREQUAL "arm")
    add_arm_executable(${_cfiber_test_name}
        fiber/context_switch_unit_tests.c
        fiber/switch_regs_arm.S
    )
else()
    add_executable(${_cfiber_test_name}
        fiber/context_switch_unit_tests.c
        fiber/switch_regs_${_cfiber_arch}.S
    )
endif()

# src: switch_regs_*.S include cfiber/fiber/asm_defs.inc
target_include_directories(${_cfiber_test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${_cfiber_test_name} PRIVATE cfiber cfiber_test project_flags m) # m: fenv

if(_cfiber_register_ctest)
    add_test(NAME context_switch COMMAND ${_cfiber_test_name})
endif()

# ======================================================================================
# Cortex-M support code (utils/cortex): RAM layout and _sbrk from a fiber stack
# ======================================================================================
if(_cfiber_arch STREQUAL "arm")
    add_arm_executable(test_cortex_syscalls cortex/test_syscalls.c)
    target_include_directories(test_cortex_syscalls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_cortex_syscalls PRIVATE cfiber cfiber_test project_flags)
endif()

# ======================================================================================
# Memory allocator tests (hosted: rely on the default malloc-backed allocator)
# ======================================================================================
if(NOT CFIBER_FREESTANDING)
    add_executable(test_memory_allocators memory/test_multislab_alloc.c)
    target_include_directories(test_memory_allocators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_memory_allocators PRIVATE cfiber cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME memory_allocators COMMAND test_memory_allocators)
    endif()
endif()

# ======================================================================================
# Scheduler tests (hosted: run real fibers, rely on the default allocator)
# ======================================================================================
if(NOT CFIBER_FREESTANDING)
    add_executable(test_scheduler scheduler/test_scheduler.c)
    target_include_directories(test_scheduler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_scheduler PRIVATE cfiber cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME scheduler COMMAND test_scheduler)
    endif()
endif()

# ======================================================================================
# Stack module tests (hosted: fixed-size + MMU-backed growable stacks)
# ======================================================================================
if(NOT CFIBER_FREESTANDING)
    add_executable(test_fixed_size_stack_allocator stack/test_fixed_size_stack_allocator.c)
    target_include_directories(test_fixed_size_stack_allocator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_fixed_size_stack_allocator PRIVATE cfiber cfiber_test project_flags)

    add_executable(test_growable_stack stack/test_growable_stack.c)
    target_include_directories(test_growable_stack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # sigaction, sigsetjmp: hidden under strict -std=c23
    target_compile_definitions(test_growable_stack PRIVATE _POSIX_C_SOURCE=200809L)
    target_link_libraries(test_growable_stack PRIVATE cfiber cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME fixed_size_stack_allocator COMMAND test_fixed_size_stack_allocator)
        add_test(NAME growable_stack COMMAND test_growable_stack)
    endif()
endif()

# ======================================================================================
# Defensive error-path tests (hosted-only).
#
# The library's misuse guards trap/abort in debug builds, so they can only be
# tested when NDEBUG disarms them. Rather than force the whole build to Release,
# we compile just the sources under test directly into this target with NDEBUG,
# leaving the rest of the suite (and the shared cfiber library) untouched. The
# library's PUBLIC usage requirements (sanitizer flags, layout defines) are
# taken from the cfiber target so this build is instrumented like the rest.
# ======================================================================================
if(NOT CFIBER_FREESTANDING)
    set(_cfiber_src ${PROJECT_SOURCE_DIR}/src/cfiber)
    add_executable(test_defensive
        defensive/test_defensive.c
        ${_cfiber_src}/memory/slab_alloc.c
        ${_cfiber_src}/memory/multislab_alloc.c
        ${_cfiber_src}/stack/growable_stack.c
        ${_cfiber_src}/stack/growable_stack_allocator.c
    )
    target_include_directories(test_defensive PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
        $<TARGET_PROPERTY:cfiber,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(test_defensive PRIVATE
        NDEBUG CFIBER_DEFENSIVE=1 _GNU_SOURCE
        $<TARGET_PROPERTY:cfiber,INTERFACE_COMPILE_DEFINITIONS>)
    target_compile_options(test_defensive PRIVATE $<TARGET_PROPERTY:cfiber,INTERFACE_COMPILE_OPTIONS>)
    target_link_options(test_defensive PRIVATE $<TARGET_PROPERTY:cfiber,INTERFACE_LINK_OPTIONS>)
    target_link_libraries(test_defensive PRIVATE cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME defensive COMMAND test_defensive)
    endif()
endif()

# ======================================================================================
# Death tests (hosted-only): debug-mode traps and sanitizer reports, in a forked child
# ======================================================================================
if(NOT CFIBER_FREESTANDING)
    add_executable(test_death death/test_death.c)
    target_include_directories(test_death PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(test_death PRIVATE _GNU_SOURCE)
    target_link_libraries(test_death PRIVATE cfiber cfiber_test project_flags)
    if(CFIBER_REACTOR)
        target_compile_definitions(test_death PRIVATE CFIBER_TEST_HAVE_REACTOR=1)
        target_link_libraries(test_death PRIVATE cfiber_reactor)
    endif()

    if(_cfiber_register_ctest)
        add_test(NAME death COMMAND test_death)
    endif()
endif()

# ======================================================================================
# Reactor tests (Linux-only; requires the optional epoll reactor)
# ======================================================================================
if(CFIBER_REACTOR AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_reactor reactor/test_reactor.c)
    target_include_directories(test_reactor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(test_reactor PRIVATE _GNU_SOURCE)
    target_link_libraries(test_reactor PRIVATE cfiber_reactor cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME reactor COMMAND test_reactor)
    endif()
endif()

# ======================================================================================
# Stack sanitizer tests (all targets; needs no OS)
# ======================================================================================
if(CFIBER_STACK_SANITIZER)
    if(CFIBER_FREESTANDING)
        add_arm_executable(test_stack_sanitizer stack/test_stack_sanitizer.c)
    else()
        add_executable(test_stack_sanitizer stack/test_stack_sanitizer.c)
    endif()
    target_include_directories(test_stack_sanitizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_stack_sanitizer PRIVATE cfiber cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME stack_sanitizer COMMAND test_stack_sanitizer)
    endif()
endif()
