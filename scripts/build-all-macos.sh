#!/usr/bin/env bash

set -euo pipefail

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "$1 was not found in PATH"
}

usage() {
    printf '%s\n' \
        "Usage: $0 [SDK options]" \
        "" \
        "Required SDK options (or use the environment variable shown):" \
        "  --android-ndk PATH        SOC_ANDROID_NDK" \
        "  --macos-sdk PATH          SOC_MACOS_SDK" \
        "  --ios-device-sdk PATH     SOC_IOS_DEVICE_SDK" \
        "  --ios-simulator-sdk PATH  SOC_IOS_SIMULATOR_SDK" \
        "  --validate-inputs-only    Validate paths without building"
}

require_absolute_directory() {
    local label="$1"
    local path="$2"
    local source_hint="$3"

    [[ -n "${path}" ]] || fail \
        "${label} path is required; use ${source_hint}"
    [[ "${path}" == /* ]] || fail \
        "${label} path must be absolute: ${path}"
    [[ -d "${path}" ]] || fail \
        "${label} directory does not exist: ${path}"
}

validate_apple_sdk_platform() {
    local label="$1"
    local path="$2"
    local expected_platform="$3"

    cmake \
        "-DSOC_APPLE_SDK_PATH:PATH=${path}" \
        "-DSOC_APPLE_SDK_PLATFORM=${expected_platform}" \
        -P "${repo_root}/cmake/ValidateAppleSDK.cmake" || fail \
        "${label} platform validation failed: ${path}"
}

verify_file_description() {
    local artifact="$1"
    shift
    local description
    local expected

    description="$(file -b "${artifact}")"
    for expected in "$@"; do
        [[ "${description}" == *"${expected}"* ]] || fail \
            "unexpected format for ${artifact}: ${description}"
    done
}

build_and_install() {
    local preset="$1"
    shift

    printf '\n==> Configuring %s\n' "${preset}"
    cmake --preset "${preset}" -DSOC_WARNINGS_AS_ERRORS=ON "$@"

    printf '==> Building %s\n' "${preset}"
    cmake --build --preset "${preset}" --parallel

    printf '==> Installing %s\n' "${preset}"
    cmake --install "${repo_root}/build/platform/${preset}"
}

android_ndk="${SOC_ANDROID_NDK:-}"
macos_sdk="${SOC_MACOS_SDK:-}"
ios_device_sdk="${SOC_IOS_DEVICE_SDK:-}"
ios_simulator_sdk="${SOC_IOS_SIMULATOR_SDK:-}"
validate_inputs_only=false

while (( $# > 0 )); do
    case "$1" in
        --android-ndk)
            [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a path"
            android_ndk="$2"
            shift 2
            ;;
        --macos-sdk)
            [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a path"
            macos_sdk="$2"
            shift 2
            ;;
        --ios-device-sdk)
            [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a path"
            ios_device_sdk="$2"
            shift 2
            ;;
        --ios-simulator-sdk)
            [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a path"
            ios_simulator_sdk="$2"
            shift 2
            ;;
        --validate-inputs-only)
            validate_inputs_only=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || fail \
    "this entry point must run on macOS"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

require_command cmake

require_absolute_directory \
    "Android NDK" "${android_ndk}" "--android-ndk or SOC_ANDROID_NDK"
require_absolute_directory \
    "macOS SDK" "${macos_sdk}" "--macos-sdk or SOC_MACOS_SDK"
require_absolute_directory \
    "iOS device SDK" "${ios_device_sdk}" \
    "--ios-device-sdk or SOC_IOS_DEVICE_SDK"
require_absolute_directory \
    "iOS simulator SDK" "${ios_simulator_sdk}" \
    "--ios-simulator-sdk or SOC_IOS_SIMULATOR_SDK"

validate_apple_sdk_platform "macOS SDK" "${macos_sdk}" macosx
validate_apple_sdk_platform \
    "iOS device SDK" "${ios_device_sdk}" iphoneos
validate_apple_sdk_platform \
    "iOS simulator SDK" "${ios_simulator_sdk}" iphonesimulator

[[ -f "${android_ndk}/build/cmake/android.toolchain.cmake" ]] || fail \
    "Android NDK toolchain was not found under ${android_ndk}"

if [[ "${validate_inputs_only}" == true ]]; then
    printf 'SDK and NDK input paths are valid\n'
    exit 0
fi

require_command file
require_command lipo
require_command ninja
require_command zig

cd "${repo_root}"

export SOC_ANDROID_NDK="${android_ndk}"

android_presets=(
    android-armeabi-v7a
    android-arm64-v8a
    android-x86
    android-x86_64
)

for preset in "${android_presets[@]}"; do
    build_and_install \
        "${preset}" \
        "-DSOC_ANDROID_NDK_PATH:PATH=${android_ndk}"
done

non_apple_release_presets=(
    linux-x86_64
    linux-aarch64
    windows-zig-x86_64
    windows-zig-arm64
)

for preset in "${non_apple_release_presets[@]}"; do
    build_and_install "${preset}"
done

export SOC_APPLE_SDK="${macos_sdk}"
build_and_install \
    macos-universal \
    "-DSOC_APPLE_SDK_PATH:PATH=${macos_sdk}" \
    "-DCMAKE_OSX_SYSROOT:PATH=${macos_sdk}"
export SOC_APPLE_SDK="${ios_device_sdk}"
build_and_install \
    ios-device-arm64 \
    "-DSOC_APPLE_SDK_PATH:PATH=${ios_device_sdk}" \
    "-DCMAKE_OSX_SYSROOT:PATH=${ios_device_sdk}"
export SOC_APPLE_SDK="${ios_simulator_sdk}"
build_and_install \
    ios-simulator-universal \
    "-DSOC_APPLE_SDK_PATH:PATH=${ios_simulator_sdk}" \
    "-DCMAKE_OSX_SYSROOT:PATH=${ios_simulator_sdk}"

expected_artifacts=(
    build/install/android-armeabi-v7a/lib/libsoc.so
    build/install/android-arm64-v8a/lib/libsoc.so
    build/install/android-x86/lib/libsoc.so
    build/install/android-x86_64/lib/libsoc.so
    build/install/linux-x86_64/lib/libsoc.so
    build/install/linux-aarch64/lib/libsoc.so
    build/install/macos-universal/lib/libsoc.dylib
    build/install/ios-device-arm64/lib/libsoc.a
    build/install/ios-simulator-universal/lib/libsoc.a
    build/install/windows-zig-x86_64/bin/libsoc.dll
    build/install/windows-zig-arm64/bin/libsoc.dll
)

for artifact in "${expected_artifacts[@]}"; do
    [[ -e "${artifact}" ]] || fail "expected artifact is missing: ${artifact}"
done

verify_file_description \
    build/install/android-armeabi-v7a/lib/libsoc.so \
    "ELF 32-bit" "ARM" "EABI5"
verify_file_description \
    build/install/android-arm64-v8a/lib/libsoc.so \
    "ELF 64-bit" "ARM aarch64"
verify_file_description \
    build/install/android-x86/lib/libsoc.so \
    "ELF 32-bit" "Intel 80386"
verify_file_description \
    build/install/android-x86_64/lib/libsoc.so \
    "ELF 64-bit" "x86-64"
verify_file_description \
    build/install/linux-x86_64/lib/libsoc.so \
    "ELF 64-bit" "x86-64"
verify_file_description \
    build/install/linux-aarch64/lib/libsoc.so \
    "ELF 64-bit" "ARM aarch64"
verify_file_description \
    build/install/windows-zig-x86_64/bin/libsoc.dll \
    "PE32+" "DLL" "x86-64"
verify_file_description \
    build/install/windows-zig-arm64/bin/libsoc.dll \
    "PE32+" "DLL" "Aarch64"

lipo build/install/macos-universal/lib/libsoc.dylib \
    -verify_arch arm64 x86_64
lipo build/install/ios-device-arm64/lib/libsoc.a \
    -verify_arch arm64
lipo build/install/ios-simulator-universal/lib/libsoc.a \
    -verify_arch arm64 x86_64

printf '\nBuilt, installed, and verified %d macOS-hosted platform variants under %s\n' \
    "${#expected_artifacts[@]}" \
    "${repo_root}/build/install"
