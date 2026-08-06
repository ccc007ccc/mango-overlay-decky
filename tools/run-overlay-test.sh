#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    printf '%s\n' \
        'Usage: tools/run-overlay-test.sh <game|vulkan|opengl> [combined|provider-only]' \
        '' \
        '  game           Run the shared test scene in nested Gamescope/MangoApp.' \
        '  vulkan        Run the same scene in a KDE Vulkan game window.' \
        '  opengl       Run the same scene in a KDE OpenGL game window.' \
        '  combined      Show native MangoHud statistics and the provider canvas.' \
        '  provider-only Hide native statistics and keep the provider canvas visible.'
}

if (( $# > 2 )); then
    usage >&2
    exit 64
fi

renderer="${1:-}"
scenario="${2:-combined}"

if [[ "${renderer}" == '-h' || "${renderer}" == '--help' ]]; then
    usage
    exit 0
fi

case "${scenario}" in
combined)
    mangohud_config="${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}"
    ;;
provider-only)
    mangohud_config='no_display=1'
    ;;
*)
    printf 'Unknown overlay test scenario: %s\n' "${scenario}" >&2
    usage >&2
    exit 64
    ;;
esac

case "${renderer}" in
game)
    exec env \
        MANGOHUD_CONFIG="${mangohud_config}" \
        MANGO_OVERLAY_OUTPUT_WIDTH="${MANGO_OVERLAY_OUTPUT_WIDTH:-1280}" \
        MANGO_OVERLAY_OUTPUT_HEIGHT="${MANGO_OVERLAY_OUTPUT_HEIGHT:-800}" \
        MANGO_OVERLAY_GAME_WIDTH="${MANGO_OVERLAY_GAME_WIDTH:-1280}" \
        MANGO_OVERLAY_GAME_HEIGHT="${MANGO_OVERLAY_GAME_HEIGHT:-800}" \
        "${repo_root}/tools/run-nested.sh"
    ;;
vulkan)
    exec env \
        MANGOHUD_CONFIG="${mangohud_config}" \
        MANGO_OVERLAY_DESKTOP_WIDTH="${MANGO_OVERLAY_DESKTOP_WIDTH:-1280}" \
        MANGO_OVERLAY_DESKTOP_HEIGHT="${MANGO_OVERLAY_DESKTOP_HEIGHT:-800}" \
        "${repo_root}/tools/run-desktop-vulkan.sh"
    ;;
opengl)
    exec env \
        MANGOHUD_CONFIG="${mangohud_config}" \
        MANGO_OVERLAY_DESKTOP_WIDTH="${MANGO_OVERLAY_DESKTOP_WIDTH:-1280}" \
        MANGO_OVERLAY_DESKTOP_HEIGHT="${MANGO_OVERLAY_DESKTOP_HEIGHT:-800}" \
        "${repo_root}/tools/run-desktop-opengl.sh"
    ;;
*)
    printf 'Unknown overlay test renderer: %s\n' "${renderer}" >&2
    usage >&2
    exit 64
    ;;
esac
