#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
run_dir="$(mktemp -d "${runtime_base}/mango-overlay-opengl.XXXXXX")"
socket_path="${run_dir}/broker.sock"
opengl_library="${MANGO_OVERLAY_OPENGL_LIBRARY:-${repo_root}/build/overlay/src/libMangoHud_opengl.so}"
opengl_shim_library="${MANGO_OVERLAY_OPENGL_SHIM_LIBRARY:-$(dirname "${opengl_library}")/libMangoHud_shim.so}"
broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
demo_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
broker_pid_file="${run_dir}/broker.pid"
demo_pid_file="${run_dir}/provider.pid"
application_pid_file="${run_dir}/application.pid"
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
    terminate_pid_file "${application_pid_file}"
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
    rm -rf "${run_dir}"
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

for file in \
    "${opengl_library}" \
    "${opengl_shim_library}" \
    "${broker_binary}" \
    "${demo_binary}"; do
    if [[ ! -e "${file}" ]]; then
        printf 'Development artifact is missing: %s\n' "${file}" >&2
        printf 'Build it with: distrobox enter dev -- meson compile -C build/overlay\n' >&2
        exit 66
    fi
done

for binary in "${broker_binary}" "${demo_binary}"; do
    if [[ ! -x "${binary}" ]]; then
        printf 'Development binary is not executable: %s\n' "${binary}" >&2
        exit 66
    fi
done

if (( $# == 0 )); then
    host_glxgears='/run/host/usr/bin/glxgears'
    if ! distrobox enter dev -- test -x "${host_glxgears}"; then
        printf 'The host OpenGL test application is not visible in dev: %s\n' \
            "${host_glxgears}" >&2
        exit 69
    fi
    test_application=(
        "${host_glxgears}"
        -geometry
        "${MANGO_OVERLAY_DESKTOP_WIDTH:-1280}x${MANGO_OVERLAY_DESKTOP_HEIGHT:-800}"
    )
else
    test_application=("$@")
fi

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

printf 'Launching the KDE OpenGL test with shim: %s\n' \
    "${opengl_shim_library}"
printf 'OpenGL renderer library: %s\n' "${opengl_library}"
printf 'Native MangoHud config: %s\n' \
    "${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}"

if [[ "${MANGO_OVERLAY_STEAM_RUNTIME:-0}" == '1' ]]; then
    runtime_preloads=()
    for private_library_name in \
        libgif.so.7 \
        libsharpyuv.so.0 \
        libwebp.so.7; do
        private_library="$(dirname "${opengl_library}")/${private_library_name}"
        if [[ -f "${private_library}" ]]; then
            runtime_preloads+=(--ld-preload "${private_library}")
        fi
    done
    env \
        -u DISABLE_MANGOHUD \
        MANGOHUD=1 \
        MANGOHUD_CONFIGFILE=/dev/null \
        MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}" \
        MANGOHUD_OPENGL_LIBS="${opengl_library}" \
        MANGO_OVERLAY_SOCKET="${socket_path}" \
        "${repo_root}/tools/run-in-steam-runtime.sh" \
        "${runtime_preloads[@]}" \
        --filesystem "${run_dir}" \
        --ld-preload "${opengl_shim_library}" \
        -- \
        "${test_application[@]}"
else
    # The inner shell owns its PID and positional parameters.
    # shellcheck disable=SC2016
    distrobox enter dev -- env \
        -u DISABLE_MANGOHUD \
        MANGOHUD=1 \
        MANGOHUD_CONFIGFILE=/dev/null \
        MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}" \
        MANGOHUD_OPENGL_LIBS="${opengl_library}" \
        MANGO_OVERLAY_SOCKET="${socket_path}" \
        LD_PRELOAD="${opengl_shim_library}" \
        sh -c 'printf "%s\n" "$$" >"$1"; shift; exec "$@"' \
        sh "${application_pid_file}" "${test_application[@]}"
fi
