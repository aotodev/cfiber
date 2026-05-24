# ---------------------------------------------------------------------------------------
# Toolchain for cross-compiling cfiber to aarch64 (tested via qemu-aarch64).
# ---------------------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_toolchain_prefix aarch64-linux-gnu-)
find_program(_binutils_path ${_toolchain_prefix}gcc NO_CACHE)

if(NOT _binutils_path)
    message(FATAL_ERROR "Failed to find aarch64 gcc toolchain (looking for ${_toolchain_prefix}gcc)")
endif()

set(CMAKE_C_COMPILER   ${_toolchain_prefix}gcc)
set(CMAKE_ASM_COMPILER ${_toolchain_prefix}gcc)
set(CMAKE_OBJCOPY      ${_toolchain_prefix}objcopy)
set(CMAKE_OBJDUMP      ${_toolchain_prefix}objdump)
set(CMAKE_SIZE         ${_toolchain_prefix}size)
set(CMAKE_AR           ${_toolchain_prefix}ar)
set(CMAKE_RANLIB       ${_toolchain_prefix}ranlib)
set(CMAKE_LINKER       ${_toolchain_prefix}ld)
set(CMAKE_LINKER_TYPE  BFD)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-sysroot
    OUTPUT_VARIABLE _toolchain_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CMAKE_SYSROOT        ${_toolchain_sysroot})
set(CMAKE_FIND_ROOT_PATH ${_toolchain_sysroot})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
