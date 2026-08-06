#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
state_root="${runtime_base}/mango-overlay-desktop-recovery"
controller_pid_file="${state_root}/controller.pid"

controller_is_running() {
    local pid="$1"
    if [[ ! "${pid}" =~ ^[0-9]+$ || ! -r "/proc/${pid}/cmdline" ]]; then
        return 1
    fi
    tr '\0' ' ' <"/proc/${pid}/cmdline" \
        | grep -Fq 'tools/run-desktop-recovery-test.sh'
}

stop_test() {
    local pid=''
    if [[ ! -f "${controller_pid_file}" ]]; then
        printf 'No desktop recovery test is running.\n'
        return 0
    fi
    read -r pid <"${controller_pid_file}" || true
    if ! controller_is_running "${pid}"; then
        find "${controller_pid_file}" -delete
        printf 'Removed a stale desktop recovery test state file.\n'
        return 0
    fi
    kill -TERM "${pid}"
    for ((attempt = 0; attempt < 60; attempt++)); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            printf 'Desktop recovery test stopped.\n'
            return 0
        fi
        sleep 0.05
    done
    printf 'Desktop recovery test controller did not stop: PID %s\n' \
        "${pid}" >&2
    return 1
}

if (( $# == 1 )) && [[ "$1" == '--stop' ]]; then
    stop_test
    exit $?
fi
if (( $# != 0 )); then
    printf 'Usage: tools/run-desktop-recovery-test.sh [--stop]\n' >&2
    exit 64
fi

mkdir -p "${state_root}"
if [[ -f "${controller_pid_file}" ]]; then
    existing_pid=''
    read -r existing_pid <"${controller_pid_file}" || true
    if controller_is_running "${existing_pid}"; then
        printf 'A desktop recovery test is already running: PID %s\n' \
            "${existing_pid}" >&2
        exit 75
    fi
    find "${controller_pid_file}" -delete
fi

run_dir="$(mktemp -d "${state_root}/run.XXXXXX")"
socket_path="${run_dir}/broker.sock"
layer_dir="${run_dir}/vulkan/implicit_layer.d"
source_manifest="${repo_root}/build/steamrt4-desktop/x86_64/MangoHud.x86_64.json"
layer_manifest="${layer_dir}/MangoHud.x86_64.json"
layer_library="${repo_root}/build/steamrt4-desktop/x86_64/libMangoHud.so"
opengl_library="${repo_root}/build/steamrt4-desktop/x86_64/libMangoHud_opengl.so"
opengl_shim="${repo_root}/build/steamrt4-desktop/x86_64/libMangoHud_shim.so"
glx_test="${repo_root}/build/steamrt4-desktop/x86_64/mango-overlay-glx-test"
d3d12_test="${repo_root}/build/proton-test/windows/x86_64/mango-overlay-proton-d3d12-test.exe"
broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
provider_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
proton_state_root="${repo_root}/build/proton-test/recovery"
title_opengl='Mango Overlay Recovery OpenGL'
title_vulkan='Mango Overlay Recovery Proton D3D12'
broker_pid_file="${run_dir}/broker.pid"
provider_pid_file="${run_dir}/provider.pid"
broker_launcher_pid=''
provider_launcher_pid=''
opengl_launcher_pid=''
vulkan_launcher_pid=''
window_opengl=''
window_vulkan=''

terminate_group() {
    local pid="$1"
    if [[ ! "${pid}" =~ ^[0-9]+$ ]]; then
        return 0
    fi
    kill -TERM -- "-${pid}" 2>/dev/null || true
    for ((attempt = 0; attempt < 40; attempt++)); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        sleep 0.05
    done
    kill -KILL -- "-${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

terminate_pid_file() {
    local pid_file="$1"
    local pid=''
    if [[ ! -f "${pid_file}" ]]; then
        return 0
    fi
    read -r pid <"${pid_file}" || true
    if [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -TERM "${pid}" 2>/dev/null || true
        for ((attempt = 0; attempt < 20; attempt++)); do
            if ! kill -0 "${pid}" 2>/dev/null; then
                return 0
            fi
            sleep 0.05
        done
        kill -KILL "${pid}" 2>/dev/null || true
    fi
    return 0
}

cleanup() {
    if [[ -n "${window_opengl}" ]]; then
        xdotool windowclose "${window_opengl}" 2>/dev/null || true
    fi
    if [[ -n "${window_vulkan}" ]]; then
        xdotool windowclose "${window_vulkan}" 2>/dev/null || true
    fi
    terminate_group "${opengl_launcher_pid}"
    terminate_group "${vulkan_launcher_pid}"
    terminate_pid_file "${provider_pid_file}"
    terminate_pid_file "${broker_pid_file}"
    if [[ -n "${provider_launcher_pid}" ]]; then
        kill "${provider_launcher_pid}" 2>/dev/null || true
        wait "${provider_launcher_pid}" 2>/dev/null || true
    fi
    if [[ -n "${broker_launcher_pid}" ]]; then
        kill "${broker_launcher_pid}" 2>/dev/null || true
        wait "${broker_launcher_pid}" 2>/dev/null || true
    fi
    current_pid=''
    if [[ -f "${controller_pid_file}" ]]; then
        read -r current_pid <"${controller_pid_file}" || true
    fi
    if [[ "${current_pid}" == "$$" ]]; then
        find "${controller_pid_file}" -delete
    fi
    printf 'Desktop recovery test logs: %s\n' "${run_dir}"
}
trap cleanup EXIT
trap 'exit 0' INT TERM

for command_name in distrobox setsid xdotool xwininfo; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'Required host command is missing: %s\n' "${command_name}" >&2
        exit 69
    fi
done
for artifact in \
    "${source_manifest}" \
    "${layer_library}" \
    "${opengl_library}" \
    "${opengl_shim}" \
    "${glx_test}" \
    "${d3d12_test}" \
    "${broker_binary}" \
    "${provider_binary}"; do
    if [[ ! -e "${artifact}" ]]; then
        printf 'Desktop recovery test artifact is missing: %s\n' \
            "${artifact}" >&2
        exit 66
    fi
done

printf '%s\n' "$$" >"${controller_pid_file}"
mkdir -p "${layer_dir}" "${proton_state_root}"
sed "s|\"library_path\": \"[^\"]*\"|\"library_path\": \"${layer_library}\"|" \
    "${source_manifest}" >"${layer_manifest}"

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec systemd-socket-activate --seqpacket --listen="$2" "$3"' \
    sh "${broker_pid_file}" "${socket_path}" "${broker_binary}" \
    >"${run_dir}/broker.log" 2>&1 &
broker_launcher_pid=$!
for ((attempt = 0; attempt < 1200; attempt++)); do
    if [[ -S "${socket_path}" ]]; then
        break
    fi
    if ! kill -0 "${broker_launcher_pid}" 2>/dev/null; then
        sed -n '1,160p' "${run_dir}/broker.log" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ ! -S "${socket_path}" ]]; then
    printf 'Desktop recovery broker did not start.\n' >&2
    exit 1
fi

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec "$2"' \
    sh "${provider_pid_file}" "${provider_binary}" \
    >"${run_dir}/provider.log" 2>&1 &
provider_launcher_pid=$!
sleep 0.1
if ! kill -0 "${provider_launcher_pid}" 2>/dev/null; then
    sed -n '1,160p' "${run_dir}/provider.log" >&2
    exit 1
fi

runtime_preloads=()
for private_library_name in libgif.so.7 libsharpyuv.so.0 libwebp.so.7; do
    private_library="$(dirname "${layer_library}")/${private_library_name}"
    if [[ -f "${private_library}" ]]; then
        runtime_preloads+=(--ld-preload "${private_library}")
    fi
done

setsid --wait env \
    -u DISABLE_MANGOHUD \
    MANGOHUD=1 \
    MANGOHUD_CONFIGFILE=/dev/null \
    MANGOHUD_CONFIG='no_display=1' \
    MANGOHUD_OPENGL_LIBS="${opengl_library}" \
    MANGO_OVERLAY_SOCKET="${socket_path}" \
    "${repo_root}/tools/run-in-steam-runtime.sh" \
    "${runtime_preloads[@]}" \
    --filesystem "${run_dir}" \
    --ld-preload "${opengl_shim}" \
    -- "${glx_test}" "${title_opengl}" \
    >"${run_dir}/opengl.log" 2>&1 &
opengl_launcher_pid=$!

setsid --wait env \
    -u DISABLE_MANGOHUD \
    -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_DISABLE -u VK_LOADER_LAYERS_ENABLE \
    MANGOHUD=1 \
    MANGOHUD_CONFIGFILE=/dev/null \
    MANGOHUD_CONFIG='no_display=1' \
    MANGO_OVERLAY_SOCKET="${socket_path}" \
    VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
    MANGO_OVERLAY_WINDOWS_TEST_TITLE="${title_vulkan}" \
    MANGO_OVERLAY_PROTON_COMPAT_DATA="${proton_state_root}/compatdata" \
    MANGO_OVERLAY_PROTON_INSTALL_PATH="${proton_state_root}/install" \
    "${repo_root}/tools/run-in-steam-runtime.sh" \
    "${runtime_preloads[@]}" \
    --filesystem "${run_dir}" \
    -- "${repo_root}/tools/run-proton-test.sh" "${d3d12_test}" \
    >"${run_dir}/vulkan.log" 2>&1 &
vulkan_launcher_pid=$!

find_window() {
    local title="$1"
    xwininfo -root -tree 2>/dev/null \
        | awk -v expected="\"${title}\"" \
            '!found && index($0, expected) { print $1; found = 1 }'
}

for ((attempt = 0; attempt < 1200; attempt++)); do
    window_opengl="$(find_window "${title_opengl}")"
    window_vulkan="$(find_window "${title_vulkan}")"
    if [[ -n "${window_opengl}" && -n "${window_vulkan}" ]]; then
        break
    fi
    if ! kill -0 "${opengl_launcher_pid}" 2>/dev/null; then
        sed -n '1,200p' "${run_dir}/opengl.log" >&2
        exit 1
    fi
    if ! kill -0 "${vulkan_launcher_pid}" 2>/dev/null; then
        sed -n '1,200p' "${run_dir}/vulkan.log" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ -z "${window_opengl}" || -z "${window_vulkan}" ]]; then
    printf 'Desktop recovery windows did not open within 60 seconds.\n' >&2
    exit 1
fi

xdotool windowmove --sync "${window_opengl}" 20 40
xdotool windowmove --sync "${window_vulkan}" 1040 40

printf 'Desktop recovery test is ready.\n'
printf '  A: %s\n' "${title_opengl}"
printf '  B: %s\n' "${title_vulkan}"
printf '  Stop: tools/run-desktop-recovery-test.sh --stop\n'
printf '  Logs: %s\n' "${run_dir}"

while kill -0 "${opengl_launcher_pid}" 2>/dev/null \
    && kill -0 "${vulkan_launcher_pid}" 2>/dev/null; do
    sleep 1
done
