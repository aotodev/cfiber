#!/usr/bin/env bash
# Build script for cfiber. Run from anywhere; auto-detects project root.
#
# Usage: ./utils/make.sh [options]
#   -a, --arch=<arch>  Target architecture (x86_64, aarch64, arm)
#   -c, --cpu=<cpu>    Target CPU (cortex-m0, cortex-m3, cortex-m4, cortex-m7)
#   -d, --debug        Build in Debug mode (default: Release)
#   -s, --samples      Build the sample
#   -t, --tests        Build and run unit tests
#   -v, --verbose      Verbose build output
#       --sanitizer    Enable the stack sanitizer (canary + watermark)
#       --asan         Enable AddressSanitizer (hosted x86_64 only)
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
  -s, --samples      Build the sample
  -t, --tests        Build and run unit tests
  -v, --verbose      Verbose build output
      --sanitizer    Enable the stack sanitizer (canary + watermark)
      --asan         Enable AddressSanitizer (hosted x86_64 only; mutually
                     exclusive with --sanitizer)
      --shared       Build cfiber as a shared library (default: static)
      --pic          Build the static library with -fPIC (ignored with --shared)
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
# Host OS / job-count detection. cfiber supports Linux hosts (and macOS in
# practice, untested); Windows is not supported.
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
# Defaults. Only invariants are declared up front; everything else is set
# conditionally by the argument parser and falls back to OFF via `${var:-OFF}`
# at the use site, so unused options never become bound variables.
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
        -s|--samples)   build_sample=ON ;;
        -t|--tests)     build_tests=ON ;;
        -v|--verbose)   verbose=--verbose ;;
        --sanitizer)    stack_sanitizer=ON ;;
        --asan)         asan=ON ;;
        --shared)       build_shared=ON ;;
        --pic)          build_pic=ON ;;
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
# AddressSanitizer validation. ASan is a hosted instrumentation kept separate
# from the canary/watermark sanitizer (they are mutually exclusive, and target
# different platforms). It is only wired up for the native x86_64 build here:
# the aarch64 path runs under qemu-user, whose address-space handling does not
# support ASan's shadow memory, and the arm path is bare metal where ASan does
# not exist.
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
# Build directory — one canonical path per (os, arch, cpu, config) so toggling
# options between runs reuses the incremental build.
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
    -DBUILD_SAMPLE="${build_sample:-OFF}" \
    -DCFIBER_STACK_SANITIZER="${stack_sanitizer:-OFF}" \
    -DCFIBER_ASAN="${asan:-OFF}" \
    -DCFIBER_BUILD_SHARED="${build_shared:-OFF}" \
    -DCFIBER_POSITION_INDEPENDENT_CODE="${build_pic:-OFF}" \
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
# Emulation helpers (target_arch != host)
# --------------------------------------------------------------------------------------
emulate_arm_with_qemu() {
    require_tool qemu-system-arm "Install qemu-system-arm to run the ARM binary."

    info "emulating ${machine} with qemu to run ${1}"
    qemu-system-arm \
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

    local sysroot
    sysroot=$(aarch64-linux-gnu-gcc -print-sysroot)
    if [[ -z "${sysroot}" || "${sysroot}" == "/" || ! -f "${sysroot}/lib/ld-linux-aarch64.so.1" ]]; then
        sysroot="/usr/aarch64-linux-gnu"
    fi

    info "emulating aarch64 with qemu to run ${1}"
    qemu-aarch64 -cpu "${target_cpu}" -L "${sysroot}" "${1}"
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
# Post-build: run sample / tests
# --------------------------------------------------------------------------------------
if [[ "${build_sample:-OFF}" == ON ]]; then
    section "running sample for ${target_arch}${target_cpu:+/${target_cpu}}"
    run_executable sample/runtime_example
    ok "sample finished"
fi

if [[ "${build_tests:-OFF}" == ON ]]; then
    section "running cfiber tests for ${target_arch}${target_cpu:+/${target_cpu}}"
    run_executable "tests/unit_tests_${target_arch}"
    ok "tests finished"

    if [[ "${stack_sanitizer:-OFF}" == ON && "${target_arch}" != "arm" ]]; then
        section "running stack sanitizer tests"
        run_executable "tests/test_stack_sanitizer"
        ok "stack sanitizer tests finished"
    fi
fi
