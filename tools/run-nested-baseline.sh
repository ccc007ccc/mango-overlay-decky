#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mangoapp_binary="${MANGO_OVERLAY_MANGOAPP_BINARY:-${repo_root}/build/baseline/src/mangoapp}"

if [[ ! -x "${mangoapp_binary}" ]]; then
    printf 'Build the baseline first: distrobox enter dev -- meson compile -C build/baseline\n' >&2
    exit 66
fi

if ! command -v gamescope >/dev/null 2>&1; then
    printf 'SteamOS gamescope was not found on the host.\n' >&2
    exit 69
fi

if (( $# == 0 )); then
    test_application=(
        distrobox enter dev --
        vkcube
        --wsi xcb
        --width "${MANGO_OVERLAY_GAME_WIDTH:-1280}"
        --height "${MANGO_OVERLAY_GAME_HEIGHT:-800}"
    )
else
    test_application=("$@")
fi

export MANGO_OVERLAY_MANGOAPP_BINARY="${mangoapp_binary}"
export MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}"
export PATH="${repo_root}/tools/nested-bin:${PATH}"

exec gamescope \
    -W "${MANGO_OVERLAY_OUTPUT_WIDTH:-1280}" \
    -H "${MANGO_OVERLAY_OUTPUT_HEIGHT:-800}" \
    -w "${MANGO_OVERLAY_GAME_WIDTH:-1280}" \
    -h "${MANGO_OVERLAY_GAME_HEIGHT:-800}" \
    --mangoapp \
    -- "${test_application[@]}"
