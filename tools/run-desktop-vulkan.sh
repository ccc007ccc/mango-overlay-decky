#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
run_dir="$(mktemp -d "${runtime_base}/mango-overlay-vulkan.XXXXXX")"
socket_path="${run_dir}/broker.sock"
layer_dir="${run_dir}/vulkan/implicit_layer.d"
source_manifest="${MANGO_OVERLAY_VULKAN_MANIFEST:-${repo_root}/build/overlay/src/MangoHud.x86_64.json}"
layer_manifest="${layer_dir}/$(basename "${source_manifest}")"
layer_library="${MANGO_OVERLAY_VULKAN_LIBRARY:-${repo_root}/build/overlay/src/libMangoHud.so}"
broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
demo_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
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
    "${source_manifest}" \
    "${layer_library}" \
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

mkdir -p "${layer_dir}"
sed \
    "s|\"library_path\": \"[^\"]*\"|\"library_path\": \"${layer_library}\"|" \
    "${source_manifest}" >"${layer_manifest}"
if ! grep -Fq "\"library_path\": \"${layer_library}\"" "${layer_manifest}"; then
    printf 'Could not point the temporary Vulkan layer manifest at the development library.\n' >&2
    exit 65
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

if (( $# == 0 )); then
    test_application=(
        vkcube
        --wsi xcb
        --width "${MANGO_OVERLAY_DESKTOP_WIDTH:-1280}"
        --height "${MANGO_OVERLAY_DESKTOP_HEIGHT:-800}"
    )
    if [[ -n "${MANGO_OVERLAY_VKCUBE_FRAME_COUNT:-}" ]]; then
        test_application+=(--c "${MANGO_OVERLAY_VKCUBE_FRAME_COUNT}")
    fi
else
    test_application=("$@")
fi

printf 'Launching the KDE Vulkan test with layer manifest: %s\n' "${layer_manifest}"
printf 'Native MangoHud config: %s\n' \
    "${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}"

if [[ "${MANGO_OVERLAY_STEAM_RUNTIME:-0}" == '1' ]]; then
    runtime_preloads=()
    for private_library_name in \
        libgif.so.7 \
        libsharpyuv.so.0 \
        libwebp.so.7; do
        private_library="$(dirname "${layer_library}")/${private_library_name}"
        if [[ -f "${private_library}" ]]; then
            runtime_preloads+=(--ld-preload "${private_library}")
        fi
    done
    env \
        -u DISABLE_MANGOHUD \
        -u VK_INSTANCE_LAYERS \
        -u VK_LOADER_LAYERS_DISABLE \
        -u VK_LOADER_LAYERS_ENABLE \
        MANGOHUD=1 \
        MANGOHUD_CONFIGFILE=/dev/null \
        MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}" \
        MANGO_OVERLAY_SOCKET="${socket_path}" \
        VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
        "${repo_root}/tools/run-in-steam-runtime.sh" \
        "${runtime_preloads[@]}" \
        --filesystem "${run_dir}" \
        -- \
        "${test_application[@]}"
else
    distrobox enter dev -- env \
        -u DISABLE_MANGOHUD \
        -u VK_INSTANCE_LAYERS \
        -u VK_LOADER_LAYERS_DISABLE \
        -u VK_LOADER_LAYERS_ENABLE \
        MANGOHUD=1 \
        MANGOHUD_CONFIGFILE=/dev/null \
        MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-fps,frametime,cpu_stats,gpu_stats}" \
        MANGO_OVERLAY_SOCKET="${socket_path}" \
        VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
        "${test_application[@]}"
fi
