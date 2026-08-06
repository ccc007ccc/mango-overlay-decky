#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_root="${MANGO_OVERLAY_PROTON_TEST_OUTPUT:-${repo_root}/build/proton-test/windows}"
requested_architecture="${1:-all}"

usage() {
    printf 'Usage: tools/build-proton-test-windows.sh [x86_64|i686|all]\n' >&2
}

case "${requested_architecture}" in
x86_64)
    architectures=(x86_64)
    ;;
i686)
    architectures=(i686)
    ;;
all)
    architectures=(x86_64 i686)
    ;;
*)
    usage
    exit 64
    ;;
esac

if [[ ! -f /run/.containerenv ]]; then
    printf 'Run this script in the distrobox dev container.\n' >&2
    exit 1
fi

for architecture in "${architectures[@]}"; do
    case "${architecture}" in
    x86_64)
        compiler='x86_64-w64-mingw32-gcc'
        expected_class='PE32+ executable'
        expected_machine='x86-64'
        ;;
    i686)
        compiler='i686-w64-mingw32-gcc'
        expected_class='PE32 executable'
        expected_machine='Intel i386'
        ;;
    esac
    if ! command -v "${compiler}" >/dev/null 2>&1; then
        printf 'Missing MinGW compiler in dev: %s\n' "${compiler}" >&2
        exit 69
    fi

    output_directory="${output_root}/${architecture}"
    mkdir -p "${output_directory}"
    "${compiler}" \
        -std=c11 -O2 -Wall -Wextra -Werror -mwindows \
        "${repo_root}/tools/windows-opengl-test-window.c" \
        -o "${output_directory}/mango-overlay-proton-opengl-test.exe" \
        -lopengl32 -lgdi32 -luser32
    "${compiler}" \
        -std=c11 -O2 -Wall -Wextra -Werror -mwindows \
        "${repo_root}/tools/windows-d3d11-test-window.c" \
        -o "${output_directory}/mango-overlay-proton-d3d11-test.exe" \
        -ld3d11 -ld3dcompiler -ldxgi -ldxguid -luser32
    "${compiler}" \
        -std=c11 -O2 -Wall -Wextra -Werror -mwindows \
        "${repo_root}/tools/windows-d3d12-test-window.c" \
        -o "${output_directory}/mango-overlay-proton-d3d12-test.exe" \
        -ld3d12 -ldxgi -ldxguid -luser32

    for executable in \
        "${output_directory}/mango-overlay-proton-opengl-test.exe" \
        "${output_directory}/mango-overlay-proton-d3d11-test.exe" \
        "${output_directory}/mango-overlay-proton-d3d12-test.exe"; do
        description="$(file -Lb "${executable}")"
        if [[ "${description}" != *"${expected_class}"* \
            || "${description}" != *'(GUI)'* \
            || "${description}" != *"${expected_machine}"* ]]; then
            printf 'Unexpected %s Windows test executable: %s: %s\n' \
                "${architecture}" "${executable}" "${description}" >&2
            exit 65
        fi
        printf 'Verified Proton %s test executable: %s\n' \
            "${architecture}" "${executable}"
    done
done
