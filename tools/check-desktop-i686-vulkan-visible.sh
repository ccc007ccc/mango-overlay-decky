#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${MANGO_OVERLAY_I686_BUILD_DIR:-${repo_root}/build/overlay-i686}"
test_binary="${build_dir}/tools/mango-overlay-vulkan-test"
layer_manifest="${build_dir}/src/MangoHud.x86.json"
layer_library="${build_dir}/src/libMangoHud.so"

if [[ ! -f "${layer_library}" || ! -f "${layer_manifest}" ]]; then
    "${repo_root}/tools/build-desktop-i686.sh"
fi

vulkan_headers="$(
    find "${repo_root}/subprojects" \
        -maxdepth 1 \
        -type d \
        -name 'Vulkan-Headers-*' \
        -print \
        -quit
)"
if [[ -z "${vulkan_headers}" ]]; then
    printf 'The configured Vulkan-Headers subproject is missing.\n' >&2
    exit 66
fi

mkdir -p "$(dirname "${test_binary}")"
distrobox enter dev -- gcc \
    -m32 \
    -std=c11 \
    -O2 \
    -Wall \
    -Wextra \
    -Werror \
    -I"${vulkan_headers}/include" \
    "${repo_root}/tools/vulkan-test-window.c" \
    -o "${test_binary}" \
    -L/usr/lib32 \
    -lvulkan \
    -lX11

file_description="$(distrobox enter dev -- file -Lb "${test_binary}")"
if [[ "${file_description}" != *'ELF 32-bit LSB pie executable, Intel i386'* ]]; then
    printf 'Expected an i686 Vulkan test executable, got: %s\n' \
        "${file_description}" >&2
    exit 65
fi

exec env \
    MANGO_OVERLAY_VULKAN_MANIFEST="${layer_manifest}" \
    MANGO_OVERLAY_VULKAN_LIBRARY="${layer_library}" \
    MANGO_OVERLAY_TEST_WINDOW_TITLE='Mango Overlay Vulkan i686' \
    "${repo_root}/tools/check-desktop-renderer-visible.sh" \
    vulkan \
    "${test_binary}" \
    'Mango Overlay Vulkan i686'
