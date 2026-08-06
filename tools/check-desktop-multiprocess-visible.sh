#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mode="${1:-native}"
architecture="${2:-x86_64}"
build_root="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${repo_root}/build/steamrt4-desktop}"
windows_build_root="${MANGO_OVERLAY_PROTON_TEST_OUTPUT:-${repo_root}/build/proton-test/windows}"

usage() {
    printf 'Usage: tools/check-desktop-multiprocess-visible.sh [native|steamrt4-proton|proton-proton [x86_64|i686]]\n' >&2
}

case "${mode}" in
native)
    if (( $# > 1 )); then
        usage
        exit 64
    fi
    architecture='x86_64'
    source_manifest="${MANGO_OVERLAY_VULKAN_MANIFEST:-${repo_root}/build/overlay/src/MangoHud.x86_64.json}"
    layer_library="${MANGO_OVERLAY_VULKAN_LIBRARY:-${repo_root}/build/overlay/src/libMangoHud.so}"
    test_binary_a="${MANGO_OVERLAY_MULTI_TEST_BINARY_A:-${MANGO_OVERLAY_MULTI_TEST_BINARY:-${build_root}/x86_64/mango-overlay-vulkan-test}}"
    test_binary_b="${MANGO_OVERLAY_MULTI_TEST_BINARY_B:-${test_binary_a}}"
    title_prefix="${MANGO_OVERLAY_MULTI_TEST_TITLE_PREFIX:-Mango Overlay Multi Vulkan}"
    default_startup_timeout=10
    ;;
steamrt4-proton|proton-proton)
    if (( $# > 2 )); then
        usage
        exit 64
    fi
    case "${architecture}" in
    x86_64)
        manifest_name='MangoHud.x86_64.json'
        ;;
    i686)
        manifest_name='MangoHud.x86.json'
        ;;
    *)
        usage
        exit 64
        ;;
    esac
    architecture_directory="${build_root}/${architecture}"
    source_manifest="${MANGO_OVERLAY_VULKAN_MANIFEST:-${architecture_directory}/${manifest_name}}"
    layer_library="${MANGO_OVERLAY_VULKAN_LIBRARY:-${architecture_directory}/libMangoHud.so}"
    if [[ "${mode}" == 'steamrt4-proton' ]]; then
        default_test_binary_a="${architecture_directory}/mango-overlay-vulkan-test"
        default_title_prefix="Mango Overlay SteamRT4 Proton Multi ${architecture}"
    else
        default_test_binary_a="${windows_build_root}/${architecture}/mango-overlay-proton-d3d11-test.exe"
        default_title_prefix="Mango Overlay Proton Multi ${architecture}"
    fi
    test_binary_a="${MANGO_OVERLAY_MULTI_TEST_BINARY_A:-${default_test_binary_a}}"
    test_binary_b="${MANGO_OVERLAY_MULTI_TEST_BINARY_B:-${windows_build_root}/${architecture}/mango-overlay-proton-d3d12-test.exe}"
    title_prefix="${MANGO_OVERLAY_MULTI_TEST_TITLE_PREFIX:-${default_title_prefix}}"
    default_startup_timeout=60
    ;;
*)
    usage
    exit 64
    ;;
esac

run_dir="$(mktemp -d "${runtime_base}/mango-overlay-multiprocess-check.XXXXXX")"
socket_path="${run_dir}/broker.sock"
layer_dir="${run_dir}/vulkan/implicit_layer.d"
layer_manifest="${layer_dir}/$(basename "${source_manifest}")"
broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
demo_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
titles=("${title_prefix} A" "${title_prefix} B")
broker_pid_file="${run_dir}/broker.pid"
demo_pid_file="${run_dir}/provider.pid"
broker_launcher_pid=''
demo_launcher_pid=''
app_pids=()
app_pgids=()
window_ids=()
check_passed=0
startup_timeout="${MANGO_OVERLAY_TEST_STARTUP_TIMEOUT:-${default_startup_timeout}}"

if [[ ! "${startup_timeout}" =~ ^[0-9]+$ ]] || (( startup_timeout < 1 || startup_timeout > 60 )); then
    printf 'MANGO_OVERLAY_TEST_STARTUP_TIMEOUT must be 1-60 seconds.\n' >&2
    exit 64
fi

terminate_group() {
    local pid="$1"
    local pgid="$2"
    if [[ ! "${pid}" =~ ^[0-9]+$ ]]; then
        return
    fi
    if [[ "${pgid}" =~ ^[0-9]+$ ]]; then
        kill -TERM -- "-${pgid}" 2>/dev/null || true
    else
        kill -TERM "${pid}" 2>/dev/null || true
    fi
    for ((attempt = 0; attempt < 20; attempt++)); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            return
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
        return
    fi
    read -r pid <"${pid_file}" || true
    if [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
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
    local index
    for index in 0 1; do
        if [[ -n "${window_ids[index]:-}" ]]; then
            xdotool windowclose "${window_ids[index]}" 2>/dev/null || true
        fi
    done
    for index in 0 1; do
        if [[ -n "${app_pids[index]:-}" ]]; then
            terminate_group "${app_pids[index]}" "${app_pgids[index]}"
        fi
    done
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
        printf 'Multiprocess check artifacts: %s\n' "${run_dir}" >&2
    fi
    return 0
}
trap cleanup EXIT INT TERM

for command_name in distrobox ffmpeg setsid xdotool xwininfo xxd; do
    command -v "${command_name}" >/dev/null 2>&1 || {
        printf 'Required host command is missing: %s\n' "${command_name}" >&2
        exit 69
    }
done
required_files=(
    "${source_manifest}"
    "${layer_library}"
    "${broker_binary}"
    "${demo_binary}"
    "${test_binary_a}"
    "${test_binary_b}"
)
if [[ "${mode}" != 'native' ]]; then
    required_files+=(
        "${repo_root}/tools/run-in-steam-runtime.sh"
        "${repo_root}/tools/run-proton-test.sh"
    )
fi
for file in "${required_files[@]}"; do
    [[ -e "${file}" ]] || { printf 'Development artifact is missing: %s\n' "${file}" >&2; exit 66; }
done
for binary in "${broker_binary}" "${demo_binary}" "${test_binary_a}" "${test_binary_b}"; do
    [[ -x "${binary}" ]] || { printf 'Development binary is not executable: %s\n' "${binary}" >&2; exit 66; }
done

mkdir -p "${layer_dir}"
sed "s|\"library_path\": \"[^\"]*\"|\"library_path\": \"${layer_library}\"|" \
    "${source_manifest}" >"${layer_manifest}"
grep -F "\"library_path\": \"${layer_library}\"" "${layer_manifest}" >/dev/null || {
    printf 'Could not point the temporary Vulkan layer manifest at the development library.\n' >&2
    exit 65
}

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec systemd-socket-activate --seqpacket --listen="$2" "$3"' \
    sh "${broker_pid_file}" "${socket_path}" "${broker_binary}" \
    >"${run_dir}/broker.log" 2>&1 &
broker_launcher_pid=$!
for ((attempt = 0; attempt < startup_timeout * 50; attempt++)); do
    [[ -S "${socket_path}" ]] && break
    kill -0 "${broker_launcher_pid}" 2>/dev/null || { sed -n '1,120p' "${run_dir}/broker.log" >&2; exit 1; }
    sleep 0.02
done
[[ -S "${socket_path}" ]] || { printf 'Scene broker did not start in time.\n' >&2; exit 1; }

# The inner shell owns these positional parameters.
# shellcheck disable=SC2016
distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
    sh -c 'printf "%s\n" "$$" >"$1"; exec "$2"' \
    sh "${demo_pid_file}" "${demo_binary}" \
    >"${run_dir}/provider.log" 2>&1 &
demo_launcher_pid=$!
sleep 0.1
kill -0 "${demo_launcher_pid}" 2>/dev/null || { sed -n '1,120p' "${run_dir}/provider.log" >&2; exit 1; }

runtime_preloads=()
if [[ "${mode}" != 'native' ]]; then
    for private_library_name in libgif.so.7 libsharpyuv.so.0 libwebp.so.7; do
        private_library="$(dirname "${layer_library}")/${private_library_name}"
        if [[ -f "${private_library}" ]]; then
            runtime_preloads+=(--ld-preload "${private_library}")
        fi
    done
fi

launch_app() {
    local index="$1"
    local title="$2"
    local pid_file="${run_dir}/app-${index}.pid"
    local test_binary="${test_binary_a}"
    if (( index == 1 )); then
        test_binary="${test_binary_b}"
    fi
    if [[ "${mode}" == 'native' ]]; then
        # shellcheck disable=SC2016
        setsid --wait distrobox enter dev -- env \
            -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_DISABLE -u VK_LOADER_LAYERS_ENABLE \
            MANGOHUD=1 MANGOHUD_CONFIGFILE=/dev/null MANGOHUD_CONFIG='no_display=1' \
            MANGO_OVERLAY_SOCKET="${socket_path}" VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
            sh -c 'printf "%s\n" "$$" >"$1"; shift; exec "$@"' \
            sh "${pid_file}" "${test_binary}" "${title}" \
            >"${run_dir}/app-${index}.log" 2>&1 &
    elif [[ "${mode}" == 'steamrt4-proton' ]] && (( index == 0 )); then
        setsid --wait env \
            -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_DISABLE -u VK_LOADER_LAYERS_ENABLE \
            MANGOHUD=1 MANGOHUD_CONFIGFILE=/dev/null MANGOHUD_CONFIG='no_display=1' \
            MANGO_OVERLAY_SOCKET="${socket_path}" VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
            "${repo_root}/tools/run-in-steam-runtime.sh" \
            "${runtime_preloads[@]}" \
            --filesystem "${run_dir}" \
            -- "${test_binary}" "${title}" \
            >"${run_dir}/app-${index}.log" 2>&1 &
    else
        setsid --wait env \
            -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_DISABLE -u VK_LOADER_LAYERS_ENABLE \
            MANGOHUD=1 MANGOHUD_CONFIGFILE=/dev/null MANGOHUD_CONFIG='no_display=1' \
            MANGO_OVERLAY_SOCKET="${socket_path}" VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
            MANGO_OVERLAY_WINDOWS_TEST_TITLE="${title}" \
            MANGO_OVERLAY_PROTON_COMPAT_DATA="${run_dir}/proton-compatdata-${architecture}-${index}" \
            MANGO_OVERLAY_PROTON_INSTALL_PATH="${run_dir}/proton-install-${architecture}-${index}" \
            "${repo_root}/tools/run-in-steam-runtime.sh" \
            "${runtime_preloads[@]}" \
            --filesystem "${run_dir}" \
            -- "${repo_root}/tools/run-proton-test.sh" "${test_binary}" \
            >"${run_dir}/app-${index}.log" 2>&1 &
    fi
    app_pids[index]=$!
    app_pgids[index]=${app_pids[index]}
}

find_window() {
    local title="$1"
    xwininfo -root -tree 2>/dev/null | awk -v expected="\"${title}\"" \
        '!found && index($0, expected) { print $1; found = 1 }'
}

launch_app 0 "${titles[0]}"
launch_app 1 "${titles[1]}"
for index in 0 1; do
    for ((attempt = 0; attempt < startup_timeout * 20; attempt++)); do
        window_ids[index]="$(find_window "${titles[index]}")"
        [[ -n "${window_ids[index]}" ]] && break
        kill -0 "${app_pids[index]}" 2>/dev/null || { sed -n '1,120p' "${run_dir}/app-${index}.log" >&2; exit 1; }
        sleep 0.05
    done
    [[ -n "${window_ids[index]}" ]] || { printf 'Window did not open: %s\n' "${titles[index]}" >&2; exit 1; }
    xdotool windowmove --sync "${window_ids[index]}" "$((index * 980))" 0
done

capture_pixels() {
    local window_id="$1"
    local screenshot="$2"
    ffmpeg -v error -y -f x11grab -window_id "${window_id}" -i "${DISPLAY}" \
        -frames:v 1 "${screenshot}" || return 1
    ffmpeg -v error -i "${screenshot}" -f rawvideo -pix_fmt rgb24 - \
        | xxd -p -c3 | awk '
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

visible() { (( $1 >= 100 && $2 >= 500 && $3 >= 20 && $4 >= 20 )); }

stable=0
for ((attempt = 0; attempt < 60; attempt++)); do
    pixels_a="$(capture_pixels "${window_ids[0]}" "${run_dir}/window-a.png")" || pixels_a=''
    pixels_b="$(capture_pixels "${window_ids[1]}" "${run_dir}/window-b.png")" || pixels_b=''
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
    sleep 0.07
done
printf 'Multiprocess initial samples=%s A=(%s) B=(%s)\n' \
    "${stable}" "${pixels_a:-none}" "${pixels_b:-none}"
if (( stable < 3 )); then
    printf 'Both renderer processes did not show the provider canvas.\n' >&2
    exit 1
fi

xdotool windowclose "${window_ids[0]}" 2>/dev/null || true
terminate_group "${app_pids[0]}" "${app_pgids[0]}"

surviving=0
for ((attempt = 0; attempt < 20; attempt++)); do
    pixels_b="$(capture_pixels "${window_ids[1]}" "${run_dir}/window-b-after-close.png")" || pixels_b=''
    if [[ -n "${pixels_b}" ]]; then
        read -r gb rb ggb gbb <<<"${pixels_b}"
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
    sleep 0.07
done
printf 'Multiprocess surviving renderer samples=%s B=(%s)\n' \
    "${surviving}" "${pixels_b:-none}"
if (( surviving < 3 )); then
    printf 'The surviving renderer lost its provider canvas.\n' >&2
    exit 1
fi

check_passed=1
printf 'Desktop multiprocess Vulkan check passed (%s %s).\n' \
    "${mode}" "${architecture}"
