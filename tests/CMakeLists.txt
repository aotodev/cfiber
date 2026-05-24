include(arm_executable)

# ======================================================================================
# cfiber tests (context switch / register preservation)
# ======================================================================================
set(_cfiber_test_name "unit_tests_${CFIBER_SYSTEM_PROCESSOR}")

if(CFIBER_SYSTEM_PROCESSOR MATCHES "arm")
    add_arm_executable(${_cfiber_test_name} fiber/context_switch_unit_tests.c fiber/unit_tests_arm.c)
elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_executable(${_cfiber_test_name} fiber/context_switch_unit_tests.c fiber/unit_tests_aarch64.c)
elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(${_cfiber_test_name} fiber/context_switch_unit_tests.c fiber/unit_tests_x86_64.c)
else()
    message(FATAL_ERROR "Unsupported architecture: ${CFIBER_SYSTEM_PROCESSOR}")
endif()

target_include_directories(${_cfiber_test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${_cfiber_test_name} PRIVATE cfiber project_flags)

# ======================================================================================
# Stack sanitizer tests (hosted-only)
# ======================================================================================
if(NOT CFIBER_FREESTANDING AND CFIBER_STACK_SANITIZER)
    add_executable(test_stack_sanitizer stack/test_stack_sanitizer.c)
    target_link_libraries(test_stack_sanitizer PRIVATE cfiber project_flags)
endif()
