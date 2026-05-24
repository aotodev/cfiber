include(arm_executable)

if(CFIBER_SYSTEM_PROCESSOR MATCHES "arm")
    add_arm_executable(runtime_example runtime_example.c)
elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|x86_64|AMD64")
    add_executable(runtime_example runtime_example.c)
else()
    message(FATAL_ERROR "Unsupported architecture: ${CFIBER_SYSTEM_PROCESSOR}")
endif()

target_link_libraries(runtime_example PRIVATE cfiber project_flags)
