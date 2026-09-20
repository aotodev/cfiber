# SPDX-FileCopyrightText: 2026 Aoto
# SPDX-License-Identifier: MIT

# Standalone examples. Each is illustrative, not library-grade: built at the
# -Wall -Wextra bar of a typical downstream consumer rather than the library's
# strict -Werror -Wconversion bar, and linking only the public libraries.
#
# The scheduler example is portable (all targets, including bare-metal ARM) and
# gated by CFIBER_BUILD_EXAMPLES. The reactor example is host/Linux only; its
# self-test doubles as the reactor's end-to-end test, so it is also built with
# the tests. Neither is built for a parent project that only wants the library.

if(CFIBER_BUILD_EXAMPLES)
    add_subdirectory(scheduler)
endif()

if(CFIBER_REACTOR AND (CFIBER_BUILD_EXAMPLES OR CFIBER_BUILD_TESTS))
    add_subdirectory(ws_echo)
endif()
