#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${MANGO_OVERLAY_I686_BUILD_DIR:-${repo_root}/build/overlay-i686}"
test_binary="${build_dir}/tools/mango-overlay-glx-test"
opengl_library="${build_dir}/src/libMangoHud_opengl.so"

if [[ ! -f "${opengl_library}" ]]; then
    "${repo_root}/tools/build-desktop-i686.sh"
fi

mkdir -p "$(dirname "${test_binary}")"
distrobox enter dev -- gcc \
    -m32 \
    -std=c11 \
    -O2 \
    -Wall \
    -Wextra \
    -Werror \
    "${repo_root}/tools/glx-test-window.c" \
    -o "${test_binary}" \
    -L/usr/lib32 \
    -lGL \
    -lX11

file_description="$(distrobox enter dev -- file -Lb "${test_binary}")"
if [[ "${file_description}" != *'ELF 32-bit LSB pie executable, Intel i386'* ]]; then
    printf 'Expected an i686 GLX test executable, got: %s\n' \
        "${file_description}" >&2
    exit 65
fi

exec env \
    MANGO_OVERLAY_OPENGL_LIBRARY="${opengl_library}" \
    MANGO_OVERLAY_TEST_WINDOW_TITLE='Mango Overlay GLX i686' \
    "${repo_root}/tools/check-desktop-renderer-visible.sh" \
    opengl \
    "${test_binary}" \
    'Mango Overlay GLX i686'
