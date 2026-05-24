# ---------------------------------------------------------------------------------------
# Toolchain for cross-compiling cfiber to ARM Cortex-M (tested via qemu-system-arm).
# ---------------------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_toolchain_prefix arm-none-eabi-)
find_program(_binutils_path ${_toolchain_prefix}gcc NO_CACHE)

if(NOT _binutils_path)
    message(FATAL_ERROR "Failed to find arm gcc toolchain (looking for ${_toolchain_prefix}gcc)")
endif()

set(CMAKE_C_COMPILER   ${_toolchain_prefix}gcc)
set(CMAKE_ASM_COMPILER ${_toolchain_prefix}gcc)
set(CMAKE_OBJCOPY      ${_toolchain_prefix}objcopy)
set(CMAKE_OBJDUMP      ${_toolchain_prefix}objdump)
set(CMAKE_SIZE         ${_toolchain_prefix}size)
set(CMAKE_AR           ${_toolchain_prefix}ar)
set(CMAKE_RANLIB       ${_toolchain_prefix}ranlib)
set(CMAKE_LINKER       ${_toolchain_prefix}ld)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-sysroot
    OUTPUT_VARIABLE _arm_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CMAKE_SYSROOT        ${_arm_sysroot})
set(CMAKE_FIND_ROOT_PATH ${_arm_sysroot})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")

# We provide our own syscall stubs, so try_compile must not try to link a full
# executable (the standard library link would fail at configure time).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
