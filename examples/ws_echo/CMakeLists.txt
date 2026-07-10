# WebSocket (RFC 6455) echo server on the epoll reactor: a standalone example.
#
# Example code is illustrative, not library-grade: it is built with -Wall -Wextra
# (like a typical downstream consumer) rather than the library's strict
# -Werror -Wconversion bar, and links the public cfiber_reactor library only.

add_library(ws_echo_obj OBJECT ws_echo.c)
target_link_libraries(ws_echo_obj PUBLIC cfiber_reactor)
target_compile_options(ws_echo_obj PRIVATE -Wall -Wextra)

add_executable(ws_echo ws_echo_server.c)
target_link_libraries(ws_echo PRIVATE ws_echo_obj cfiber_reactor)
target_compile_options(ws_echo PRIVATE -Wall -Wextra)

add_executable(ws_selftest ws_selftest.c)
target_link_libraries(ws_selftest PRIVATE ws_echo_obj cfiber_reactor)
target_compile_options(ws_selftest PRIVATE -Wall -Wextra)

# Register the self-test with CTest only when the test build is on (so
# enable_testing() has run) and we can run the binary on the build host.
if(BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME ws_echo_selftest COMMAND ws_selftest)
endif()
