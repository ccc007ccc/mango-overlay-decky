#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${repo_root}/build/steamrt4-desktop}"
requested_architecture="${1:-x86_64}"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

case "${requested_architecture}" in
x86_64)
    manifest_name='MangoHud.x86_64.json'
    ;;
i686)
    manifest_name='MangoHud.x86.json'
    ;;
*)
    printf 'Usage: tools/check-desktop-multiswapchain-visible.sh [x86_64|i686]\n' >&2
    exit 64
    ;;
esac

architecture_directory="${build_root}/${requested_architecture}"
run_dir="$(mktemp -d "${runtime_base}/mango-overlay-multiswapchain-check.XXXXXX")"
socket_path="${run_dir}/broker.sock"
layer_dir="${run_dir}/vulkan/implicit_layer.d"
source_manifest="${MANGO_OVERLAY_VULKAN_MANIFEST:-${architecture_directory}/${manifest_name}}"
layer_manifest="${layer_dir}/$(basename "${source_manifest}")"
layer_library="${MANGO_OVERLAY_VULKAN_LIBRARY:-${architecture_directory}/libMangoHud.so}"
broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
demo_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
test_binary="${MANGO_OVERLAY_MULTI_TEST_BINARY:-${architecture_directory}/mango-overlay-vulkan-two-swapchains}"
title_prefix="${MANGO_OVERLAY_MULTI_TEST_TITLE_PREFIX:-Mango Overlay Multi Swapchain ${requested_architecture}}"
title_a="${title_prefix} A"
title_b="${title_prefix} B"
broker_pid_file="${run_dir}/broker.pid"
demo_pid_file="${run_dir}/provider.pid"
launcher_pid=''
launcher_pgid=''
broker_launcher_pid=''
demo_launcher_pid=''
window_a=''
window_b=''
check_passed=0
startup_timeout="${MANGO_OVERLAY_TEST_STARTUP_TIMEOUT:-60}"

if [[ ! "${startup_timeout}" =~ ^[0-9]+$ ]] \
    || (( startup_timeout < 1 || startup_timeout > 120 )); then
    printf 'MANGO_OVERLAY_TEST_STARTUP_TIMEOUT must be 1-120 seconds.\n' >&2
    exit 64
fi
startup_attempts=$((startup_timeout * 20))

terminate_group() {
    local pid="$1"
    local pgid="$2"
    if [[ ! "${pid}" =~ ^[0-9]+$ ]]; then
        return 0
    fi
    if [[ "${pgid}" =~ ^[0-9]+$ ]]; then
        kill -TERM -- "-${pgid}" 2>/dev/null || true
    else
        kill -TERM "${pid}" 2>/dev/null || true
    fi
    for ((attempt = 0; attempt < 40; attempt++)); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        sleep 0.05
    done
    if [[ "${pgid}" =~ ^[0-9]+$ ]]; then
        kill -KILL -- "-${pgid}" 2>/dev/null || true
    else
        kill -KILL "${pid}" 2>/dev/null || true
    fi
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
        kill "${pid}" 2>/dev/null || true
        for ((attempt = 0; attempt < 40; attempt++)); do
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
    if [[ -n "${window_a}" ]]; then
        xdotool key --window "${window_a}" Escape 2>/dev/null || true
    fi
    if [[ -n "${window_b}" ]]; then
        xdotool key --window "${window_b}" Escape 2>/dev/null || true
    fi
    if [[ -n "${launcher_pid}" ]]; then
        terminate_group "${launcher_pid}" "${launcher_pgid}"
    fi
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
    if (( check_passed )); then
        rm -rf "${run_dir}"
    else
        printf 'Multi-swapchain check artifacts: %s\n' "${run_dir}" >&2
    fi
    return 0
}
trap cleanup EXIT INT TERM

for command_name in distrobox ffmpeg setsid xdotool xwininfo xxd; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'Required host command is missing: %s\n' "${command_name}" >&2
        exit 69
    fi
done

for file in \
    "${source_manifest}" \
    "${layer_library}" \
    "${broker_binary}" \
    "${demo_binary}" \
    "${test_binary}"; do
    if [[ ! -e "${file}" ]]; then
        printf 'Development artifact is missing: %s\n' "${file}" >&2
        printf 'Build it with: distrobox enter dev -- tools/build-desktop-multiswapchain.sh %s\n' \
            "${requested_architecture}" >&2
        exit 66
    fi
done
for binary in "${broker_binary}" "${demo_binary}" "${test_binary}"; do
    if [[ ! -x "${binary}" ]]; then
        printf 'Development binary is not executable: %s\n' "${binary}" >&2
        exit 66
    fi
done

if xwininfo -root -tree 2>/dev/null \
    | grep -F "\"${title_a}\"" >/dev/null \
    || xwininfo -root -tree 2>/dev/null \
        | grep -F "\"${title_b}\"" >/dev/null; then
    printf 'Close existing multi-swapchain test windows before running this check.\n' >&2
    exit 75
fi

mkdir -p "${layer_dir}"
sed "s|\"library_path\": \"[^\"]*\"|\"library_path\": \"${layer_library}\"|" \
    "${source_manifest}" >"${layer_manifest}"
if ! grep -Fq "\"library_path\": \"${layer_library}\"" "${layer_manifest}"; then
    printf 'Could not point the temporary Vulkan layer manifest at the development library.\n' >&2
    exit 65
fi

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec systemd-socket-activate --seqpacket --listen="$2" "$3"' \
    sh "${broker_pid_file}" "${socket_path}" "${broker_binary}" \
    >"${run_dir}/broker.log" 2>&1 &
broker_launcher_pid=$!
for ((attempt = 0; attempt < startup_attempts; attempt++)); do
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
    printf 'Scene broker did not start in time.\n' >&2
    exit 1
fi

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec "$2"' \
    sh "${demo_pid_file}" "${demo_binary}" \
    >"${run_dir}/provider.log" 2>&1 &
demo_launcher_pid=$!
sleep 0.1
if ! kill -0 "${demo_launcher_pid}" 2>/dev/null; then
    sed -n '1,160p' "${run_dir}/provider.log" >&2
    exit 1
fi

runtime_preloads=()
for private_library_name in libgif.so.7 libsharpyuv.so.0 libwebp.so.7; do
    private_library="${architecture_directory}/${private_library_name}"
    if [[ -f "${private_library}" ]]; then
        runtime_preloads+=(--ld-preload "${private_library}")
    fi
done

launch_test() {
    # The test binary is started once. It owns both X11 windows and both
    # swapchains; closing A below must not terminate this process or B.
    # shellcheck disable=SC2016
    setsid --wait env \
        -u VK_INSTANCE_LAYERS \
        -u VK_LOADER_LAYERS_DISABLE \
        -u VK_LOADER_LAYERS_ENABLE \
        MANGOHUD=1 \
        MANGOHUD_CONFIGFILE=/dev/null \
        MANGOHUD_CONFIG='no_display=1' \
        MANGO_OVERLAY_SOCKET="${socket_path}" \
        VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
        "${repo_root}/tools/run-in-steam-runtime.sh" \
        "${runtime_preloads[@]}" \
        --filesystem "${run_dir}" \
        -- "${test_binary}" "${title_a}" "${title_b}"
}

launch_test >"${run_dir}/app.log" 2>&1 &
launcher_pid=$!
launcher_pgid="${launcher_pid}"

find_window() {
    local title="$1"
    xwininfo -root -tree 2>/dev/null \
        | awk -v expected="\"${title}\"" \
            '!found && index($0, expected) { print $1; found = 1 }'
}

for title in "${title_a}" "${title_b}"; do
    found_window=''
    for ((attempt = 0; attempt < startup_attempts; attempt++)); do
        found_window="$(find_window "${title}")"
        if [[ -n "${found_window}" ]]; then
            break
        fi
        if ! kill -0 "${launcher_pid}" 2>/dev/null; then
            printf 'Multi-swapchain test exited before opening window: %s\n' \
                "${title}" >&2
            sed -n '1,200p' "${run_dir}/app.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    if [[ -z "${found_window}" ]]; then
        printf 'Multi-swapchain window did not open in time: %s\n' "${title}" >&2
        exit 1
    fi
    if [[ "${title}" == "${title_a}" ]]; then
        window_a="${found_window}"
    else
        window_b="${found_window}"
    fi
done

capture_pixels() {
    local window_id="$1"
    local screenshot="$2"
    ffmpeg -v error -y -f x11grab -window_id "${window_id}" -i "${DISPLAY}" \
        -frames:v 1 "${screenshot}" || return 1
    ffmpeg -v error -i "${screenshot}" -f rawvideo -pix_fmt rgb24 - \
        | xxd -p -c3 \
        | awk '
            $0 == "1fc78a" { green++ }
            $0 == "ff0000" { red++ }
            {
                r = strtonum("0x" substr($0, 1, 2)); g = strtonum("0x" substr($0, 3, 2)); b = strtonum("0x" substr($0, 5, 2))
                gd = (r - 0) ^ 2 + (g - 220) ^ 2 + (b - 150) ^ 2
                bd = (r - 40) ^ 2 + (g - 90) ^ 2 + (b - 255) ^ 2
                if (gd <= 400) { gif_green++ }; if (bd <= 400) { gif_blue++ }
            }
            END { print green + 0, red + 0, gif_green + 0, gif_blue + 0 }'
}

visible() {
    (( $1 >= 100 && $2 >= 500 && $3 >= 20 && $4 >= 20 ))
}

stable=0
pixels_a=''
pixels_b=''
for ((attempt = 0; attempt < 60; attempt++)); do
    pixels_a="$(capture_pixels "${window_a}" "${run_dir}/window-a.png")" || pixels_a=''
    pixels_b="$(capture_pixels "${window_b}" "${run_dir}/window-b.png")" || pixels_b=''
    if [[ -n "${pixels_a}" && -n "${pixels_b}" ]]; then
        read -r ga ra gga gba <<<"${pixels_a}"
        read -r gb rb ggb gbb <<<"${pixels_b}"
        if visible "${ga}" "${ra}" "${gga}" "${gba}" \
            && visible "${gb}" "${rb}" "${ggb}" "${gbb}"; then
            ((stable += 1))
            if (( stable >= 3 )); then
                break
            fi
        else
            stable=0
        fi
    else
        stable=0
    fi
    if ! kill -0 "${launcher_pid}" 2>/dev/null; then
        break
    fi
    sleep 0.07
done
printf 'Multi-swapchain initial samples=%s A=(%s) B=(%s)\n' \
    "${stable}" "${pixels_a:-none}" "${pixels_b:-none}"
if (( stable < 3 )); then
    printf 'Both swapchains did not show the provider canvas.\n' >&2
    exit 1
fi

# Send the event the fixture handles itself. `xdotool windowclose` asks KWin to
# close the X11 window and can destroy it before the fixture consumes
# WM_DELETE_WINDOW, which turns the intended per-swapchain teardown into an
# unrelated BadWindow process abort.
xdotool key --window "${window_a}" Escape
window_a_closed=0
for ((attempt = 0; attempt < startup_attempts; attempt++)); do
    if [[ -z "$(find_window "${title_a}")" ]]; then
        window_a_closed=1
        break
    fi
    if ! kill -0 "${launcher_pid}" 2>/dev/null; then
        printf 'The two-swapchain test process exited while closing A.\n' >&2
        sed -n '1,200p' "${run_dir}/app.log" >&2
        exit 1
    fi
    sleep 0.05
done
if (( ! window_a_closed )); then
    printf 'Window A did not close while the shared test process was running.\n' >&2
    exit 1
fi
window_a=''

surviving=0
pixels_b_after=''
for ((attempt = 0; attempt < 30; attempt++)); do
    pixels_b_after="$(capture_pixels "${window_b}" "${run_dir}/window-b-after-close.png")" \
        || pixels_b_after=''
    if [[ -n "${pixels_b_after}" ]]; then
        read -r gb rb ggb gbb <<<"${pixels_b_after}"
        if visible "${gb}" "${rb}" "${ggb}" "${gbb}"; then
            ((surviving += 1))
            if (( surviving >= 3 )); then
                break
            fi
        else
            surviving=0
        fi
    else
        surviving=0
    fi
    if ! kill -0 "${launcher_pid}" 2>/dev/null; then
        break
    fi
    sleep 0.07
done
printf 'Multi-swapchain surviving B samples=%s B=(%s)\n' \
    "${surviving}" "${pixels_b_after:-none}"
if (( surviving < 3 )); then
    printf 'The surviving swapchain lost its provider canvas.\n' >&2
    exit 1
fi
if ! kill -0 "${launcher_pid}" 2>/dev/null; then
    printf 'The shared two-swapchain process exited before B was closed.\n' >&2
    exit 1
fi

xdotool key --window "${window_b}" Escape 2>/dev/null || true
check_passed=1
printf 'Desktop %s multi-swapchain Vulkan check passed.\n' "${requested_architecture}"
