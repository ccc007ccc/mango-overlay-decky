#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
architecture="${MANGO_OVERLAY_EGL_ARCHITECTURE:-x86_64}"

case "${architecture}" in
x86_64)
    build_dir="${repo_root}/build/overlay"
    compiler_arguments=()
    linker_arguments=()
    expected_file_description='ELF 64-bit LSB pie executable, x86-64'
    ;;
i686)
    build_dir="${MANGO_OVERLAY_I686_BUILD_DIR:-${repo_root}/build/overlay-i686}"
    compiler_arguments=(-m32)
    linker_arguments=(-L/usr/lib32)
    expected_file_description='ELF 32-bit LSB pie executable, Intel i386'
    if [[ ! -f "${build_dir}/src/libMangoHud_opengl.so" ]]; then
        "${repo_root}/tools/build-desktop-i686.sh"
    fi
    ;;
*)
    printf 'Unknown EGL test architecture: %s\n' "${architecture}" >&2
    exit 64
    ;;
esac

test_binary="${build_dir}/tools/mango-overlay-egl-test"
opengl_library="${build_dir}/src/libMangoHud_opengl.so"
window_title="Mango Overlay EGL ${architecture}"

if [[ ! -f "${opengl_library}" ]]; then
    printf 'Development OpenGL library is missing: %s\n' \
        "${opengl_library}" >&2
    exit 66
fi

mkdir -p "$(dirname "${test_binary}")"
distrobox enter dev -- gcc \
    "${compiler_arguments[@]}" \
    -std=c11 \
    -O2 \
    -Wall \
    -Wextra \
    -Werror \
    "${repo_root}/tools/egl-test-window.c" \
    -o "${test_binary}" \
    "${linker_arguments[@]}" \
    -lEGL \
    -lGL \
    -lX11

file_description="$(distrobox enter dev -- file -Lb "${test_binary}")"
if [[ "${file_description}" != *"${expected_file_description}"* ]]; then
    printf 'Unexpected EGL test executable: %s\n' "${file_description}" >&2
    exit 65
fi

exec env \
    MANGO_OVERLAY_OPENGL_LIBRARY="${opengl_library}" \
    MANGO_OVERLAY_TEST_WINDOW_TITLE="${window_title}" \
    "${repo_root}/tools/check-desktop-renderer-visible.sh" \
    opengl \
    "${test_binary}" \
    "${window_title}"
