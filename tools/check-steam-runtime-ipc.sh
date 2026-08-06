#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_root="${MANGO_OVERLAY_STEAM_RUNTIME_ROOT:-${HOME}/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4}"
runtime_launcher="${runtime_root}/run"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

if [[ -n "${MANGO_OVERLAY_PACKAGE_ARCHIVE:-}" ]]; then
    archive="${MANGO_OVERLAY_PACKAGE_ARCHIVE}"
else
    mapfile -t archives < <(
        find "${repo_root}/build/package" \
            -maxdepth 1 \
            -type f \
            -name 'mango-overlay-decky-*.zip' \
            -print \
            | sort
    )
    if (( ${#archives[@]} != 1 )); then
        printf 'Expected exactly one Decky package in build/package; found %s.\n' \
            "${#archives[@]}" >&2
        exit 66
    fi
    archive="${archives[0]}"
fi

for path in "${runtime_launcher}" "${archive}"; do
    if [[ ! -e "${path}" ]]; then
        printf 'Required Steam Runtime IPC artifact is missing: %s\n' \
            "${path}" >&2
        exit 66
    fi
done

test_root="$(mktemp -d "${runtime_base}/mango-overlay-steamrt-ipc.XXXXXX")"
broker_pid=''
cleanup() {
    if [[ -n "${broker_pid}" ]]; then
        kill "${broker_pid}" 2>/dev/null || true
        wait "${broker_pid}" 2>/dev/null || true
    fi
    if [[ "${test_root}" == "${runtime_base}"/mango-overlay-steamrt-ipc.* ]]; then
        find "${test_root}" -depth -delete
    fi
}
trap cleanup EXIT INT TERM

unzip -q "${archive}" 'mango-overlay-decky/runtime/*' -d "${test_root}"
package_root="${test_root}/mango-overlay-decky/runtime"
broker_binary="${package_root}/bin/mango-overlayd"
controller_binary="${package_root}/bin/mango-overlayctl"
library_directory="${package_root}/lib"
socket_path="${test_root}/broker.sock"
broker_log="${test_root}/broker.log"

for path in \
    "${broker_binary}" \
    "${controller_binary}" \
    "${library_directory}/libjpeg.so.62"; do
    if [[ ! -e "${path}" ]]; then
        printf 'Decky package runtime member is missing: %s\n' "${path}" >&2
        exit 65
    fi
done

mkdir -p "${test_root}/home" "${test_root}/config" "${test_root}/state"
systemd-socket-activate \
    --seqpacket \
    --listen="${socket_path}" \
    --setenv="HOME=${test_root}/home" \
    --setenv="XDG_CONFIG_HOME=${test_root}/config" \
    --setenv="XDG_STATE_HOME=${test_root}/state" \
    --setenv="LD_LIBRARY_PATH=${library_directory}" \
    "${broker_binary}" \
    >"${broker_log}" 2>&1 &
broker_pid=$!

for ((attempt = 0; attempt < 100; attempt++)); do
    if [[ -S "${socket_path}" ]]; then
        break
    fi
    if ! kill -0 "${broker_pid}" 2>/dev/null; then
        printf 'Packaged broker exited before creating its socket.\n' >&2
        sed -n '1,160p' "${broker_log}" >&2
        exit 1
    fi
    sleep 0.02
done
if [[ ! -S "${socket_path}" ]]; then
    printf 'Packaged broker did not create its socket within two seconds.\n' >&2
    exit 1
fi

status_json="$(
    env \
        LANG=C.UTF-8 \
        LC_ALL=C.UTF-8 \
        PRESSURE_VESSEL_BATCH=1 \
        PRESSURE_VESSEL_SYSTEMD_SCOPE=0 \
        "${runtime_launcher}" \
        --no-import-vulkan-layers \
        --no-systemd-scope \
        --filesystem="${test_root}" \
        -- \
        /usr/bin/env \
        LD_LIBRARY_PATH="${library_directory}" \
        "${controller_binary}" \
        --socket "${socket_path}" \
        status
)"

if ! kill -0 "${broker_pid}" 2>/dev/null; then
    printf 'Packaged broker exited during the Steam Runtime handshake.\n' >&2
    sed -n '1,200p' "${broker_log}" >&2
    exit 1
fi

python3 - "${status_json}" <<'PY'
import json
import sys

status = json.loads(sys.argv[1])
if not isinstance(status.get("enabled"), bool):
    raise SystemExit("controller status is missing enabled")
if not isinstance(status.get("require_approval"), bool):
    raise SystemExit("controller status is missing require_approval")
if not isinstance(status.get("scene_revision"), int):
    raise SystemExit("controller status is missing scene_revision")
if not isinstance(status.get("applications"), list):
    raise SystemExit("controller status is missing applications")
PY

printf 'Steam Runtime 4 controller IPC check passed: %s\n' "${status_json}"
