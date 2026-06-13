include(arm_executable)

# ======================================================================================
# Shared test framework (counters, runner, assertions)
# ======================================================================================
add_library(cfiber_test STATIC test/test.c)
target_include_directories(cfiber_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cfiber_test PUBLIC project_flags)

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
set(_cfiber_test_name "unit_tests_${CFIBER_SYSTEM_PROCESSOR}")

if(CFIBER_SYSTEM_PROCESSOR MATCHES "arm")
    add_arm_executable(${_cfiber_test_name}
        fiber/context_switch_unit_tests.c
        fiber/unit_tests_arm.c
    )
elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_executable(${_cfiber_test_name}
        fiber/context_switch_unit_tests.c
        fiber/unit_tests_aarch64.c
    )
elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(${_cfiber_test_name}
        fiber/context_switch_unit_tests.c
        fiber/unit_tests_x86_64.c
    )
else()
    message(FATAL_ERROR "Unsupported architecture: ${CFIBER_SYSTEM_PROCESSOR}")
endif()

target_include_directories(${_cfiber_test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${_cfiber_test_name} PRIVATE cfiber cfiber_test project_flags)

if(_cfiber_register_ctest)
    add_test(NAME context_switch COMMAND ${_cfiber_test_name})
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
# leaving the rest of the suite (and the shared cfiber library) untouched.
# ======================================================================================
if(NOT CFIBER_FREESTANDING)
    set(_cfiber_src ${CMAKE_SOURCE_DIR}/src/cfiber)
    add_executable(test_defensive
        defensive/test_defensive.c
        ${_cfiber_src}/memory/slab_alloc.c
        ${_cfiber_src}/memory/multislab_alloc.c
        ${_cfiber_src}/stack/growable_stack.c
        ${_cfiber_src}/stack/growable_stack_allocator.c
    )
    target_include_directories(test_defensive
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(test_defensive PRIVATE NDEBUG CFIBER_DEFENSIVE=1 _GNU_SOURCE)
    target_link_libraries(test_defensive PRIVATE cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME defensive COMMAND test_defensive)
    endif()
endif()

# ======================================================================================
# Stack sanitizer tests (hosted-only, requires the canary/watermark sanitizer)
# ======================================================================================
if(NOT CFIBER_FREESTANDING AND CFIBER_STACK_SANITIZER)
    add_executable(test_stack_sanitizer stack/test_stack_sanitizer.c)
    target_include_directories(test_stack_sanitizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_stack_sanitizer PRIVATE cfiber cfiber_test project_flags)

    if(_cfiber_register_ctest)
        add_test(NAME stack_sanitizer COMMAND test_stack_sanitizer)
    endif()
endif()
