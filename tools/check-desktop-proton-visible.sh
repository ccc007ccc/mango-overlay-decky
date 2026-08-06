#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
windows_build_root="${MANGO_OVERLAY_PROTON_TEST_OUTPUT:-${repo_root}/build/proton-test/windows}"
runtime_build_root="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${repo_root}/build/steamrt4-desktop}"
requested_architecture="${1:-all}"
startup_timeout="${MANGO_OVERLAY_TEST_STARTUP_TIMEOUT:-60}"

usage() {
    printf 'Usage: tools/check-desktop-proton-visible.sh [x86_64|i686|all]\n' >&2
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

for architecture in "${architectures[@]}"; do
    windows_directory="${windows_build_root}/${architecture}"
    runtime_directory="${runtime_build_root}/${architecture}"
    opengl_executable="${windows_directory}/mango-overlay-proton-opengl-test.exe"
    d3d11_executable="${windows_directory}/mango-overlay-proton-d3d11-test.exe"
    d3d12_executable="${windows_directory}/mango-overlay-proton-d3d12-test.exe"
    manifest_name='MangoHud.x86_64.json'
    if [[ "${architecture}" == 'i686' ]]; then
        manifest_name='MangoHud.x86.json'
    fi

    for artifact in \
        "${opengl_executable}" \
        "${d3d11_executable}" \
        "${d3d12_executable}" \
        "${runtime_directory}/libMangoHud.so" \
        "${runtime_directory}/libMangoHud_opengl.so" \
        "${runtime_directory}/libMangoHud_shim.so" \
        "${runtime_directory}/mango-overlay-vulkan-test" \
        "${runtime_directory}/${manifest_name}"; do
        if [[ ! -f "${artifact}" ]]; then
            printf 'Proton test artifact is missing: %s\n' "${artifact}" >&2
            exit 66
        fi
    done

    opengl_title="Mango Overlay Proton OpenGL ${architecture}"
    printf 'Checking Proton %s OpenGL.\n' "${architecture}"
    env \
        MANGO_OVERLAY_STEAM_RUNTIME=1 \
        MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        MANGO_OVERLAY_OPENGL_LIBRARY="${runtime_directory}/libMangoHud_opengl.so" \
        MANGO_OVERLAY_OPENGL_SHIM_LIBRARY="${runtime_directory}/libMangoHud_shim.so" \
        MANGO_OVERLAY_TEST_REQUIRE_BASE_CONTENT=1 \
        MANGO_OVERLAY_TEST_WINDOW_TITLE="${opengl_title}" \
        MANGO_OVERLAY_WINDOWS_TEST_TITLE="${opengl_title}" \
        "${repo_root}/tools/check-desktop-renderer-visible.sh" \
        opengl \
        "${repo_root}/tools/run-proton-test.sh" \
        "${opengl_executable}"

    d3d11_title="Mango Overlay Proton D3D11 ${architecture}"
    printf 'Checking Proton %s D3D11/DXVK.\n' "${architecture}"
    env \
        MANGO_OVERLAY_STEAM_RUNTIME=1 \
        MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        MANGO_OVERLAY_VULKAN_LIBRARY="${runtime_directory}/libMangoHud.so" \
        MANGO_OVERLAY_VULKAN_MANIFEST="${runtime_directory}/${manifest_name}" \
        MANGO_OVERLAY_TEST_REQUIRE_BASE_CONTENT=1 \
        MANGO_OVERLAY_TEST_WINDOW_TITLE="${d3d11_title}" \
        MANGO_OVERLAY_WINDOWS_TEST_TITLE="${d3d11_title}" \
        "${repo_root}/tools/check-desktop-renderer-visible.sh" \
        vulkan \
        "${repo_root}/tools/run-proton-test.sh" \
        "${d3d11_executable}"

    d3d12_title="Mango Overlay Proton D3D12 ${architecture}"
    printf 'Checking Proton %s D3D12/VKD3D.\n' "${architecture}"
    env \
        MANGO_OVERLAY_STEAM_RUNTIME=1 \
        MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        MANGO_OVERLAY_VULKAN_LIBRARY="${runtime_directory}/libMangoHud.so" \
        MANGO_OVERLAY_VULKAN_MANIFEST="${runtime_directory}/${manifest_name}" \
        MANGO_OVERLAY_TEST_REQUIRE_BASE_CONTENT=1 \
        MANGO_OVERLAY_TEST_WINDOW_TITLE="${d3d12_title}" \
        MANGO_OVERLAY_WINDOWS_TEST_TITLE="${d3d12_title}" \
        "${repo_root}/tools/check-desktop-renderer-visible.sh" \
        vulkan \
        "${repo_root}/tools/run-proton-test.sh" \
        "${d3d12_executable}"

    printf 'Checking SteamRT4 + Proton %s multiprocess isolation.\n' \
        "${architecture}"
    env MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        "${repo_root}/tools/check-desktop-multiprocess-visible.sh" \
        steamrt4-proton \
        "${architecture}"

    printf 'Checking two Proton %s processes with separate prefixes.\n' \
        "${architecture}"
    env MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        "${repo_root}/tools/check-desktop-multiprocess-visible.sh" \
        proton-proton \
        "${architecture}"
done

printf 'Proton desktop renderer matrix passed.\n'
