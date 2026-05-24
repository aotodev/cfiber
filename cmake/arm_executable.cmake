# ---------------------------------------------------------------------------------------
# add_arm_executable(name source...)
#
# Creates an ARM Cortex-M executable wired up with the appropriate QEMU startup
# code, linker script, and post-build .bin generation. The startup + syscalls
# helpers are compiled once and shared across all callers via OBJECT libraries.
# ---------------------------------------------------------------------------------------
function(add_arm_executable name)
    set(_cortex_dir "${CMAKE_SOURCE_DIR}/utils/cortex")

    if(CFIBER_TARGET_CPU MATCHES "^cortex-m0$")
        set(_startup_src   "${_cortex_dir}/startup_qemu_armv6-m.S")
        set(_linker_script "${_cortex_dir}/m0/microbit.ld")
    elseif(CFIBER_TARGET_CPU MATCHES "^cortex-m(3|4)")
        set(_startup_src   "${_cortex_dir}/startup_qemu_armv7-m.S")
        set(_linker_script "${_cortex_dir}/m3/mps2-an385.ld")
    elseif(CFIBER_TARGET_CPU MATCHES "^cortex-m7")
        set(_startup_src   "${_cortex_dir}/startup_qemu_armv7-m.S")
        set(_linker_script "${_cortex_dir}/m7/mps2-an500.ld")
    else()
        message(FATAL_ERROR "Unsupported CPU: ${CFIBER_TARGET_CPU}")
    endif()

    # Shared between tests and samples — create once.
    if(NOT TARGET cfiber_cortex_syscalls)
        add_library(cfiber_cortex_syscalls OBJECT ${_cortex_dir}/syscalls.c)
    endif()

    if(NOT TARGET cfiber_cortex_startup)
        add_library(cfiber_cortex_startup OBJECT ${_startup_src})
    endif()

    add_executable(${name}
        ${ARGN}
        $<TARGET_OBJECTS:cfiber_cortex_syscalls>
        $<TARGET_OBJECTS:cfiber_cortex_startup>
    )

    target_link_options(${name} PRIVATE
        -Wl,--gc-sections
        -specs=nano.specs
        -nostartfiles
        -T${_linker_script}
    )

    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.bin
        COMMENT "Generating .bin from ELF"
    )
endfunction()
