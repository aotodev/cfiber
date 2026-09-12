#!/usr/bin/env bash
# Build script for cfiber. Run from anywhere; auto-detects project root.
#
# Usage: ./utils/make.sh [options]
#   -a, --arch=<arch>  Target architecture (x86_64, aarch64, arm)
#   -c, --cpu=<cpu>    Target CPU (cortex-m0, cortex-m3, cortex-m4, cortex-m7)
#   -d, --debug        Build in Debug mode (default: Release)
#   -e, --examples     Build (and run) the examples
#   -t, --tests        Build and run unit tests
#   -v, --verbose      Verbose build output
#       --sanitizer    Enable the stack sanitizer (canary + watermark)
#       --asan         Enable AddressSanitizer (hosted x86_64 only)
#       --ubsan        Enable UndefinedBehaviorSanitizer (hosted x86_64 only)
#       --tsan         Enable ThreadSanitizer (hosted x86_64 only; implies --reactor)
#       --reactor      Build + test the optional epoll reactor (Linux only)
#       --clean        Remove the previous build directory before configuring
#   -h, --help         Show this help message

set -euo pipefail

# --------------------------------------------------------------------------------------
# Colors / helpers
# --------------------------------------------------------------------------------------
if [[ -t 1 ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    CYAN='\033[0;36m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' CYAN='' BOLD='' NC=''
fi

info()    { printf '%b[INFO]%b  %s\n' "${CYAN}" "${NC}" "$*"; }
ok()      { printf '%b[OK]%b    %s\n' "${GREEN}" "${NC}" "$*"; }
warn()    { printf '%b[WARN]%b  %s\n' "${YELLOW}" "${NC}" "$*"; }
die()     { printf '%b[ERROR]%b %s\n' "${RED}" "${NC}" "$*" >&2; exit 1; }
section() { printf '\n%b===== %s =====%b\n' "${BOLD}${CYAN}" "$*" "${NC}"; }

usage() {
    cat <<EOF
Build script for cfiber. Run from anywhere; auto-detects project root.

Usage: $(basename "$0") [options]
  -a, --arch=<arch>  Target architecture (x86_64, aarch64, arm)
  -c, --cpu=<cpu>    Target CPU (cortex-m0, cortex-m3, cortex-m4, cortex-m7)
  -d, --debug        Build in Debug mode (default: Release)
  -e, --examples     Build (and run) the examples
  -t, --tests        Build and run unit tests
  -v, --verbose      Verbose build output
      --sanitizer    Enable the stack sanitizer (canary + watermark)
      --asan         Enable AddressSanitizer (hosted x86_64 only; mutually
                     exclusive with --sanitizer)
      --ubsan        Enable UndefinedBehaviorSanitizer (hosted x86_64 only; may
                     be combined with --asan)
      --tsan         Enable ThreadSanitizer (hosted x86_64 only; mutually
                     exclusive with --asan; implies --reactor)
      --shared       Build cfiber as a shared library (default: static)
      --pic          Build the static library with -fPIC (ignored with --shared)
      --reactor      Build and test the optional epoll(7) reactor (Linux only)
      --clean        Remove the previous build directory before configuring
  -h, --help         Show this help message
EOF
}

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        die "'$1' not found in PATH. ${2:-}"
    fi
}

# --------------------------------------------------------------------------------------
# Resolve project root
# --------------------------------------------------------------------------------------
script_dir=$(cd "$(dirname "$0")" && pwd)
project_root=$(cd "${script_dir}/.." && pwd)
cd "${project_root}"

# --------------------------------------------------------------------------------------
# Host OS / job-count detection. Linux, or macOS unverified. Not Windows.
# --------------------------------------------------------------------------------------
host_os="$(uname -s)"
case "${host_os}" in
    Linux|Darwin) ;;
    *) die "unsupported host OS: '${host_os}' (cfiber builds on Linux; macOS is unverified)" ;;
esac

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

# --------------------------------------------------------------------------------------
# Defaults. Unset options stay unbound, so every use site needs `${var:-OFF}`
# under `set -u`.
# --------------------------------------------------------------------------------------
build_type=Release
target_arch="$(uname -m)"

# --------------------------------------------------------------------------------------
# Early --help (before any tool detection)
# --------------------------------------------------------------------------------------
for arg in "$@"; do
    case "${arg}" in
        -h|--help) usage; exit 0 ;;
    esac
done

# --------------------------------------------------------------------------------------
# Parse arguments
# --------------------------------------------------------------------------------------
for arg in "$@"; do
    case "${arg}" in
        -a=*|--arch=*)  target_arch="${arg#*=}" ;;
        -c=*|--cpu=*)   target_cpu="${arg#*=}" ;;
        -d|--debug)     build_type=Debug ;;
        -e|--examples)  build_examples=ON ;;
        -t|--tests)     build_tests=ON ;;
        -v|--verbose)   verbose=--verbose ;;
        --sanitizer)    stack_sanitizer=ON ;;
        --asan)         asan=ON ;;
        --ubsan)        ubsan=ON ;;
        --tsan)         tsan=ON ;;
        --shared)       build_shared=ON ;;
        --pic)          build_pic=ON ;;
        --reactor)      reactor=ON ;;
        --clean)        clean_build=1 ;;
        -h|--help)      ;;
        -*)             usage >&2; die "unknown option: '${arg}'" ;;
    esac
done

# --------------------------------------------------------------------------------------
# Per-CPU ARM defaults (board / FPU / float-ABI). Used by QEMU emulation too.
# --------------------------------------------------------------------------------------
configure_arm_cpu() {
    case "${1}" in
        cortex-m0) machine=microbit ;;
        cortex-m3) machine=mps2-an385 ;;
        cortex-m4) machine=mps2-an386 ;;
        cortex-m7) machine=mps2-an500; fpu=fpv5-sp-d16; float_abi=hard ;;
        *)
            warn "invalid arm cpu '${1}'"
            cat >&2 <<EOF
Supported ARM cpus:
  - cortex-m0   (board: microbit)
  - cortex-m3   (board: mps2-an385)
  - cortex-m4   (board: mps2-an386,  no FPU)
  - cortex-m7   (board: mps2-an500,  with FPU)
EOF
            exit 1
            ;;
    esac
}

# --------------------------------------------------------------------------------------
# Architecture / toolchain selection
# --------------------------------------------------------------------------------------
case "${target_arch}" in
    x86_64|AMD64)
        info "building for x86_64"
        ;;

    aarch64|arm64)
        info "building for aarch64"
        require_tool aarch64-linux-gnu-gcc "Install the aarch64-linux-gnu toolchain."
        toolchain_file="cmake/toolchain-aarch64.cmake"
        target_cpu="${target_cpu:-cortex-a53}"
        ;;

    arm)
        info "building for arm (Cortex-M)"
        require_tool arm-none-eabi-gcc "Install the arm-none-eabi toolchain."
        toolchain_file="cmake/toolchain-arm.cmake"
        target_cpu="${target_cpu:-cortex-m7}"
        configure_arm_cpu "${target_cpu}"
        ;;

    *)
        cat >&2 <<EOF
Unsupported architecture: '${target_arch}'
Supported architectures:
  - x86_64 (AMD64)
  - aarch64 (arm64)
  - arm
EOF
        exit 1
        ;;
esac

require_tool cmake

# --------------------------------------------------------------------------------------
# AddressSanitizer: native x86_64 only, and excludes the canary/watermark sanitizer.
# --------------------------------------------------------------------------------------
if [[ "${asan:-OFF}" == ON ]]; then
    if [[ "${stack_sanitizer:-OFF}" == ON ]]; then
        die "--asan and --sanitizer are mutually exclusive (canary word overlaps the ASan redzone)."
    fi
    case "${target_arch}" in
        x86_64|AMD64) ;;
        *) die "--asan is only supported on native x86_64 (aarch64 runs under qemu-user, which ASan does not support; arm is bare metal)." ;;
    esac
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_stack_use_after_return=1}"
fi

# --------------------------------------------------------------------------------------
# UndefinedBehaviorSanitizer: native x86_64 only, the cross toolchains ship no libubsan.
# --------------------------------------------------------------------------------------
if [[ "${ubsan:-OFF}" == ON ]]; then
    case "${target_arch}" in
        x86_64|AMD64) ;;
        *) die "--ubsan is only supported on native x86_64 (the aarch64 cross toolchain has no libubsan; arm is bare metal)." ;;
    esac
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
fi

# --------------------------------------------------------------------------------------
# ThreadSanitizer: native x86_64 only, excludes ASan. The reactor is the only
# concurrent code, so --tsan implies --reactor.
# --------------------------------------------------------------------------------------
if [[ "${tsan:-OFF}" == ON ]]; then
    if [[ "${asan:-OFF}" == ON ]]; then
        die "--tsan and --asan are mutually exclusive (incompatible sanitizer runtimes)."
    fi
    case "${target_arch}" in
        x86_64|AMD64) ;;
        *) die "--tsan is only supported on native x86_64 (qemu-user does not support ThreadSanitizer)." ;;
    esac
    reactor=ON
    export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}"
fi

# --------------------------------------------------------------------------------------
# Reactor: Linux only (epoll/eventfd), so hosted targets but not bare-metal arm.
# --------------------------------------------------------------------------------------
if [[ "${reactor:-OFF}" == ON ]]; then
    case "${target_arch}" in
        x86_64|AMD64|aarch64|arm64) ;;
        *) die "--reactor is Linux only (epoll/eventfd); the arm target is bare metal." ;;
    esac
fi

# --------------------------------------------------------------------------------------
# Build directory: one canonical path per (os, arch, cpu, config).
# --------------------------------------------------------------------------------------
build_dir="build/${host_os}/${target_arch}${target_cpu:+/${target_cpu}}/${build_type}"

if [[ -n "${clean_build+x}" ]]; then
    warn "cleaning ${build_dir}"
    rm -rf "${build_dir}"
fi

mkdir -p "${build_dir}"

# --------------------------------------------------------------------------------------
# Configure
# --------------------------------------------------------------------------------------
section "cfiber for ${target_arch}${target_cpu:+/${target_cpu}} (${build_type})"

cmake -S "${project_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DBUILD_TESTS="${build_tests:-OFF}" \
    -DCFIBER_BUILD_EXAMPLES="${build_examples:-OFF}" \
    -DCFIBER_STACK_SANITIZER="${stack_sanitizer:-OFF}" \
    -DCFIBER_ASAN="${asan:-OFF}" \
    -DCFIBER_UBSAN="${ubsan:-OFF}" \
    -DCFIBER_TSAN="${tsan:-OFF}" \
    -DCFIBER_BUILD_SHARED="${build_shared:-OFF}" \
    -DCFIBER_POSITION_INDEPENDENT_CODE="${build_pic:-OFF}" \
    -DCFIBER_REACTOR="${reactor:-OFF}" \
    ${toolchain_file:+-DCMAKE_TOOLCHAIN_FILE="${toolchain_file}"} \
    ${target_cpu:+-DCFIBER_TARGET_CPU="${target_cpu}"} \
    ${float_abi:+-DCFIBER_ARM_FLOAT_ABI="${float_abi}"} \
    ${fpu:+-DCFIBER_ARM_FPU="${fpu}"} \
    -G "Unix Makefiles"

# --------------------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------------------
jobs=$(detect_jobs)
# shellcheck disable=SC2086 # intentional word-splitting on optional --verbose flag
cmake --build "${build_dir}" ${verbose:-} --config "${build_type}" -j"${jobs}"

# --------------------------------------------------------------------------------------
# Emulation helpers (target_arch != host). Guest runs are bounded: a hung test
# fails here instead of at the CI job timeout.
# --------------------------------------------------------------------------------------
readonly qemu_timeout=300

emulate_arm_with_qemu() {
    require_tool qemu-system-arm "Install qemu-system-arm to run the ARM binary."
    require_tool timeout "Install GNU coreutils."

    info "emulating ${machine} with qemu to run ${1}"
    timeout "${qemu_timeout}" qemu-system-arm \
        -M "${machine}" \
        -cpu "${target_cpu}" \
        -kernel "${1}" \
        -nographic \
        -semihosting-config enable=on,target=native \
        -monitor none \
        -serial stdio
}

emulate_aarch64_with_qemu() {
    require_tool qemu-aarch64 "Install qemu-user (qemu-aarch64) to run the AArch64 binary."
    require_tool timeout "Install GNU coreutils."

    local sysroot
    sysroot=$(aarch64-linux-gnu-gcc -print-sysroot)
    if [[ -z "${sysroot}" || "${sysroot}" == "/" || ! -f "${sysroot}/lib/ld-linux-aarch64.so.1" ]]; then
        sysroot="/usr/aarch64-linux-gnu"
    fi

    info "emulating aarch64 with qemu to run ${1}"
    timeout "${qemu_timeout}" qemu-aarch64 -cpu "${target_cpu}" -L "${sysroot}" "${1}"
}

run_executable() {
    local exe="${build_dir}/${1}"
    if [[ ! -f "${exe}" ]]; then
        die "executable not found: ${exe}"
    fi

    case "${target_arch}" in
        x86_64|AMD64)     "${exe}" ;;
        aarch64|arm64)    emulate_aarch64_with_qemu "${exe}" ;;
        arm)              emulate_arm_with_qemu "${exe}" ;;
        *)                die "can't run '${exe}' for arch '${target_arch}'" ;;
    esac
}

# --------------------------------------------------------------------------------------
# Post-build: run examples / tests
# --------------------------------------------------------------------------------------
if [[ "${build_examples:-OFF}" == ON ]]; then
    section "running scheduler example for ${target_arch}${target_cpu:+/${target_cpu}}"
    run_executable examples/scheduler/runtime_example
    ok "scheduler example finished"
fi

if [[ "${build_tests:-OFF}" == ON ]]; then
    section "running cfiber tests for ${target_arch}${target_cpu:+/${target_cpu}}"
    run_executable "tests/unit_tests_${target_arch}"
    ok "tests finished"

    # Allocator tests use the default malloc-backed allocator, so they only run
    # on hosted targets (arm is bare metal / freestanding).
    if [[ "${target_arch}" != "arm" ]]; then
        section "running memory allocator tests"
        run_executable "tests/test_memory_allocators"
        ok "memory allocator tests finished"

        section "running scheduler tests"
        run_executable "tests/test_scheduler"
        ok "scheduler tests finished"

        section "running stack module tests"
        run_executable "tests/test_fixed_size_stack_allocator"
        run_executable "tests/test_growable_stack"
        ok "stack module tests finished"

        section "running defensive error-path tests"
        run_executable "tests/test_defensive"
        ok "defensive tests finished"
    fi

    if [[ "${stack_sanitizer:-OFF}" == ON && "${target_arch}" != "arm" ]]; then
        section "running stack sanitizer tests"
        run_executable "tests/test_stack_sanitizer"
        ok "stack sanitizer tests finished"
    fi

    if [[ "${reactor:-OFF}" == ON && "${target_arch}" != "arm" ]]; then
        section "running reactor tests"
        run_executable "tests/test_reactor"
        ok "reactor tests finished"

        section "running WebSocket echo example self-test"
        run_executable "examples/ws_echo/ws_selftest"
        ok "WebSocket echo self-test finished"
    fi
fi
