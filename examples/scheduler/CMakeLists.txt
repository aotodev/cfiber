# Scheduler example: the built-in FCFS scheduler on every supported target,
# hosted and bare-metal ARM. Like a downstream consumer, it builds at the
# -Wall -Wextra bar and links only the public cfiber library.
include(arm_executable)

if(CFIBER_SYSTEM_PROCESSOR MATCHES "arm")
    add_arm_executable(runtime_example runtime_example.c)
elseif(CFIBER_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|x86_64|AMD64")
    add_executable(runtime_example runtime_example.c)
else()
    message(FATAL_ERROR "Unsupported architecture: ${CFIBER_SYSTEM_PROCESSOR}")
endif()

target_link_libraries(runtime_example PRIVATE cfiber)
target_compile_options(runtime_example PRIVATE -Wall -Wextra)
