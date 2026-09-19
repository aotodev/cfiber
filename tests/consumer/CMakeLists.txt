# Consumer smoke test: an out-of-tree project that finds an installed cfiber
# through find_package and pkg-config exports. Built by CI against a staged
# install; not part of the main build.
cmake_minimum_required(VERSION 3.28 FATAL_ERROR)
project(cfiber_consumer LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(cfiber CONFIG REQUIRED)

add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE cfiber::cfiber)
target_compile_options(consumer PRIVATE -Wall -Wextra -Werror)
