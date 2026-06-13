# ======================================================================================
# libFuzzer targets (enabled with -DCFIBER_FUZZ=ON; Clang only).
#
# The cfiber library is already instrumented with ASan + UBSan + SanitizerCoverage
# by the CFIBER_FUZZ block in the top-level CMakeLists. Each target here adds the
# libFuzzer driver (-fsanitize=fuzzer) on top.
# ======================================================================================

function(add_cfiber_fuzzer name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE cfiber project_flags)
    target_compile_options(${name} PRIVATE
        -fsanitize=address,undefined,fuzzer-no-link
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${name} PRIVATE -fsanitize=address,undefined,fuzzer)
endfunction()

add_cfiber_fuzzer(fuzz_multislab)
add_cfiber_fuzzer(fuzz_scheduler)
