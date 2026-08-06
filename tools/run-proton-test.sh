#!/usr/bin/env bash

set -euo pipefail

if (( $# < 1 )); then
    printf 'Usage: tools/run-proton-test.sh <windows-executable> [arguments ...]\n' >&2
    exit 64
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
proton_path="${MANGO_OVERLAY_PROTON:-/home/deck/.local/share/Steam/steamapps/common/Proton 11.0/proton}"
compat_data_path="${MANGO_OVERLAY_PROTON_COMPAT_DATA:-${repo_root}/build/proton-test/compatdata}"
install_path="${MANGO_OVERLAY_PROTON_INSTALL_PATH:-${repo_root}/build/proton-test/install}"

if [[ ! -x "${proton_path}" ]]; then
    printf 'Proton launcher is missing: %s\n' "${proton_path}" >&2
    exit 66
fi
if [[ ! -f "$1" ]]; then
    printf 'Windows test executable is missing: %s\n' "$1" >&2
    exit 66
fi

mkdir -p "${compat_data_path}" "${install_path}"
exec env \
    STEAM_COMPAT_DATA_PATH="${compat_data_path}" \
    STEAM_COMPAT_CLIENT_INSTALL_PATH='/home/deck/.local/share/Steam' \
    STEAM_COMPAT_INSTALL_PATH="${install_path}" \
    SteamGameId=0 \
    SteamAppId=0 \
    WINEDEBUG=-all \
    "${proton_path}" \
    waitforexitandrun \
    "$@"
