#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
run_dir="$(mktemp -d "${runtime_base}/mango-overlay-decky.XXXXXX")"
socket_path="${run_dir}/broker.sock"
broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
demo_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
mangoapp_binary="${MANGO_OVERLAY_MANGOAPP_BINARY:-${repo_root}/build/overlay/src/mangoapp}"
broker_pid_file="${run_dir}/broker.pid"
demo_pid_file="${run_dir}/provider.pid"
broker_launcher_pid=''
demo_launcher_pid=''

terminate_pid_file() {
    local pid_file="$1"
    local pid=''

    if [[ ! -f "${pid_file}" ]]; then
        return
    fi
    read -r pid <"${pid_file}" || true
    if [[ ! "${pid}" =~ ^[0-9]+$ ]] || ! kill -0 "${pid}" 2>/dev/null; then
        return
    fi

    kill "${pid}" 2>/dev/null || true
    for ((attempt = 0; attempt < 20; attempt++)); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return
        fi
        sleep 0.05
    done
    kill -KILL "${pid}" 2>/dev/null || true
}

cleanup() {
    terminate_pid_file "${demo_pid_file}"
    terminate_pid_file "${broker_pid_file}"
    if [[ -n "${demo_launcher_pid}" ]]; then
        kill "${demo_launcher_pid}" 2>/dev/null || true
        wait "${demo_launcher_pid}" 2>/dev/null || true
    fi
    if [[ -n "${broker_launcher_pid}" ]]; then
        kill "${broker_launcher_pid}" 2>/dev/null || true
        wait "${broker_launcher_pid}" 2>/dev/null || true
    fi
    rm -f \
        "${socket_path}" \
        "${broker_pid_file}" \
        "${demo_pid_file}" \
        "${run_dir}/broker.log" \
        "${run_dir}/provider.log"
    rmdir "${run_dir}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail_with_log() {
    local message="$1"
    local log_path="$2"

    printf '%s\n' "${message}" >&2
    if [[ -s "${log_path}" ]]; then
        printf '%s\n' '--- process log ---' >&2
        sed -n '1,120p' "${log_path}" >&2
    fi
    exit 70
}

for binary in "${broker_binary}" "${demo_binary}" "${mangoapp_binary}"; do
    if [[ ! -x "${binary}" ]]; then
        printf 'Development binary is missing: %s\n' "${binary}" >&2
        printf 'Build it with: distrobox enter dev -- meson compile -C build/overlay\n' >&2
        exit 66
    fi
done

export MANGO_OVERLAY_SOCKET="${socket_path}"
# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env \
    MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec systemd-socket-activate --seqpacket --listen="$2" "$3"' \
    sh "${broker_pid_file}" "${socket_path}" "${broker_binary}" \
    >"${run_dir}/broker.log" 2>&1 &
broker_launcher_pid=$!

for ((attempt = 0; attempt < 100; attempt++)); do
    if [[ -S "${socket_path}" ]]; then
        break
    fi
    if ! kill -0 "${broker_launcher_pid}" 2>/dev/null; then
        fail_with_log 'Scene broker exited before creating its socket.' \
            "${run_dir}/broker.log"
    fi
    sleep 0.02
done

if [[ ! -S "${socket_path}" ]]; then
    fail_with_log 'Scene broker did not create its socket within two seconds.' \
        "${run_dir}/broker.log"
fi

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env \
    MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec "$2"' \
    sh "${demo_pid_file}" "${demo_binary}" \
    >"${run_dir}/provider.log" 2>&1 &
demo_launcher_pid=$!

sleep 0.1
if ! kill -0 "${demo_launcher_pid}" 2>/dev/null; then
    fail_with_log 'Demo provider exited during startup.' "${run_dir}/provider.log"
fi

MANGO_OVERLAY_MANGOAPP_BINARY="${mangoapp_binary}" \
    "${repo_root}/tools/run-nested-baseline.sh" "$@"
