# Standalone examples. Each is illustrative, not library-grade: built at the
# -Wall -Wextra bar of a typical downstream consumer rather than the library's
# strict -Werror -Wconversion bar, and linking only the public libraries.
#
# The scheduler example is portable (all targets, including bare-metal ARM) and
# gated by CFIBER_BUILD_EXAMPLES. The reactor example is host/Linux only; it is
# built with CFIBER_REACTOR because its self-test doubles as the reactor's
# end-to-end test, so it must be available whenever the reactor is.

if(CFIBER_BUILD_EXAMPLES)
    add_subdirectory(scheduler)
endif()

if(CFIBER_REACTOR)
    add_subdirectory(ws_echo)
endif()
