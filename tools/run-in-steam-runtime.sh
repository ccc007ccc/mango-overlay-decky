#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_root="${MANGO_OVERLAY_STEAM_RUNTIME_ROOT:-${HOME}/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4}"
runtime_launcher="${runtime_root}/run"
runtime_arguments=(
    --no-import-vulkan-layers
    --no-systemd-scope
    --filesystem="${repo_root}"
)

while (( $# > 0 )); do
    case "$1" in
    --ld-preload)
        if (( $# < 2 )); then
            printf '%s requires a path.\n' "$1" >&2
            exit 64
        fi
        runtime_arguments+=(--ld-preload="$2")
        shift 2
        ;;
    --filesystem)
        if (( $# < 2 )); then
            printf '%s requires a path.\n' "$1" >&2
            exit 64
        fi
        runtime_arguments+=(--filesystem="$2")
        shift 2
        ;;
    --)
        shift
        break
        ;;
    *)
        printf 'Unknown Steam Runtime launcher option: %s\n' "$1" >&2
        exit 64
        ;;
    esac
done

if (( $# == 0 )); then
    printf 'A Steam Runtime command is required.\n' >&2
    exit 64
fi
if [[ ! -x "${runtime_launcher}" ]]; then
    printf 'Steam Runtime 4 launcher is missing: %s\n' "${runtime_launcher}" >&2
    exit 66
fi

exec env \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PRESSURE_VESSEL_BATCH=1 \
    PRESSURE_VESSEL_SYSTEMD_SCOPE=0 \
    "${runtime_launcher}" \
    "${runtime_arguments[@]}" \
    -- \
    "$@"
