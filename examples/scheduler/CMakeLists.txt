# SPDX-FileCopyrightText: 2026 Aoto
# SPDX-License-Identifier: MIT

# Scheduler example: the built-in FCFS scheduler on every supported target,
# hosted and bare-metal ARM. Like a downstream consumer, it builds at the
# -Wall -Wextra bar and links only the public cfiber library.
include(arm_executable)

if(_cfiber_arch STREQUAL "arm")
    add_arm_executable(runtime_example runtime_example.c)
else()
    add_executable(runtime_example runtime_example.c)
endif()

target_link_libraries(runtime_example PRIVATE cfiber)
target_compile_options(runtime_example PRIVATE -Wall -Wextra)
