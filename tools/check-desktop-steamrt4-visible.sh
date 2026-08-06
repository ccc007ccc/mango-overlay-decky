#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${repo_root}/build/steamrt4-desktop}"
requested_architecture="${1:-all}"
startup_timeout="${MANGO_OVERLAY_TEST_STARTUP_TIMEOUT:-60}"

usage() {
    printf 'Usage: tools/check-desktop-steamrt4-visible.sh [x86_64|i686|all]\n' >&2
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

check_opengl_renderer() {
    local architecture="$1"
    local api="$2"
    local test_binary="$3"
    local title="Mango Overlay SteamRT4 ${api} ${architecture}"
    local library="${build_root}/${architecture}/libMangoHud_opengl.so"

    printf 'Checking SteamRT4 %s %s.\n' "${architecture}" "${api}"
    env \
        MANGO_OVERLAY_STEAM_RUNTIME=1 \
        MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        MANGO_OVERLAY_OPENGL_LIBRARY="${library}" \
        MANGO_OVERLAY_TEST_WINDOW_TITLE="${title}" \
        "${repo_root}/tools/check-desktop-renderer-visible.sh" \
        opengl \
        "${test_binary}" \
        "${title}"
}

check_vulkan_renderer() {
    local architecture="$1"
    local architecture_directory="${build_root}/${architecture}"
    local manifest_name='MangoHud.x86_64.json'
    local title="Mango Overlay SteamRT4 Vulkan ${architecture}"

    if [[ "${architecture}" == 'i686' ]]; then
        manifest_name='MangoHud.x86.json'
    fi

    printf 'Checking SteamRT4 %s Vulkan.\n' "${architecture}"
    env \
        MANGO_OVERLAY_STEAM_RUNTIME=1 \
        MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        MANGO_OVERLAY_VULKAN_LIBRARY="${architecture_directory}/libMangoHud.so" \
        MANGO_OVERLAY_VULKAN_MANIFEST="${architecture_directory}/${manifest_name}" \
        MANGO_OVERLAY_TEST_WINDOW_TITLE="${title}" \
        "${repo_root}/tools/check-desktop-renderer-visible.sh" \
        vulkan \
        "${architecture_directory}/mango-overlay-vulkan-test" \
        "${title}"
}

check_multiswapchain_renderer() {
    local architecture="$1"
    local architecture_directory="${build_root}/${architecture}"
    local manifest_name='MangoHud.x86_64.json'

    if [[ "${architecture}" == 'i686' ]]; then
        manifest_name='MangoHud.x86.json'
    fi

    printf 'Checking SteamRT4 %s Vulkan multi-swapchain isolation.\n' \
        "${architecture}"
    env \
        MANGO_OVERLAY_TEST_STARTUP_TIMEOUT="${startup_timeout}" \
        MANGO_OVERLAY_VULKAN_LIBRARY="${architecture_directory}/libMangoHud.so" \
        MANGO_OVERLAY_VULKAN_MANIFEST="${architecture_directory}/${manifest_name}" \
        "${repo_root}/tools/check-desktop-multiswapchain-visible.sh" \
        "${architecture}"
}

for architecture in "${architectures[@]}"; do
    architecture_directory="${build_root}/${architecture}"
    required_artifacts=(
        "${architecture_directory}/libMangoHud.so"
        "${architecture_directory}/libMangoHud_opengl.so"
        "${architecture_directory}/libMangoHud_shim.so"
        "${architecture_directory}/libgif.so.7"
        "${architecture_directory}/mango-overlay-glx-test"
        "${architecture_directory}/mango-overlay-egl-test"
        "${architecture_directory}/mango-overlay-vulkan-test"
        "${architecture_directory}/mango-overlay-vulkan-two-swapchains"
    )
    if [[ "${architecture}" == 'i686' ]]; then
        required_artifacts+=(
            "${architecture_directory}/libsharpyuv.so.0"
            "${architecture_directory}/libwebp.so.7"
        )
    fi
    for artifact in "${required_artifacts[@]}"; do
        if [[ ! -f "${artifact}" ]]; then
            printf 'SteamRT4 desktop artifact is missing: %s\n' \
                "${artifact}" >&2
            printf 'Build it with: distrobox enter dev -- tools/build-desktop-steamrt4.sh %s\n' \
                "${architecture}" >&2
            exit 66
        fi
    done

    check_opengl_renderer \
        "${architecture}" \
        GLX \
        "${architecture_directory}/mango-overlay-glx-test"
    check_opengl_renderer \
        "${architecture}" \
        EGL \
        "${architecture_directory}/mango-overlay-egl-test"
    check_vulkan_renderer "${architecture}"
    check_multiswapchain_renderer "${architecture}"
done

printf 'SteamRT4 desktop renderer matrix passed.\n'
