#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
build_root="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${repo_root}/build/steamrt4-desktop}"
windows_build_root="${MANGO_OVERLAY_PROTON_TEST_OUTPUT:-${repo_root}/build/proton-test/windows}"
requested_architecture="${1:-all}"
requested_renderer="${2:-all}"
sample_seconds="${MANGO_OVERLAY_PERF_SAMPLE_SECONDS:-2}"
warmup_seconds="${MANGO_OVERLAY_PERF_WARMUP_SECONDS:-1.5}"
output_path="${MANGO_OVERLAY_DESKTOP_PERF_OUTPUT:-${repo_root}/build/desktop-performance/latest.csv}"

usage() {
    printf 'Usage: tools/benchmark-desktop-renderers.sh [x86_64|i686|all] [vulkan|glx|egl|proton-opengl|proton-d3d11|proton-d3d12|all]\n' >&2
}

case "${requested_architecture}" in
x86_64)
    architectures=(x86_64)
    ;;
i686)
    architectures=(i686)
    ;;
all)
    architectures=(x86_64 i686)
    ;;
*)
    usage
    exit 64
    ;;
esac

case "${requested_renderer}" in
vulkan|glx|egl|proton-opengl|proton-d3d11|proton-d3d12)
    renderers=("${requested_renderer}")
    ;;
all)
    renderers=(vulkan glx egl proton-opengl proton-d3d11 proton-d3d12)
    ;;
*)
    usage
    exit 64
    ;;
esac

if [[ ! "${sample_seconds}" =~ ^[0-9]+([.][0-9]+)?$ ]] \
    || [[ ! "${warmup_seconds}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf 'Performance sample and warmup durations must be positive numbers.\n' >&2
    exit 64
fi
if ! awk -v sample="${sample_seconds}" -v warmup="${warmup_seconds}" \
    'BEGIN { exit !(sample >= 1 && sample <= 30 && warmup >= 0.2 && warmup <= 30) }'; then
    printf 'Sample duration must be 1-30 seconds and warmup 0.2-30 seconds.\n' >&2
    exit 64
fi

for command_name in awk distrobox ffmpeg perf setsid xdotool xwininfo xxd; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'Required host command is missing: %s\n' "${command_name}" >&2
        exit 69
    fi
done

broker_binary="${repo_root}/build/overlay/broker/mango-overlayd"
demo_binary="${repo_root}/build/overlay/tools/mango-overlay-test-provider"
for binary in "${broker_binary}" "${demo_binary}"; do
    if [[ ! -x "${binary}" ]]; then
        printf 'Development binary is missing or not executable: %s\n' \
            "${binary}" >&2
        exit 66
    fi
done

output_directory="$(dirname "${output_path}")"
mkdir -p "${output_directory}"
benchmark_root="$(mktemp -d "${runtime_base}/mango-overlay-performance.XXXXXX")"
storage_root="$(mktemp -d "${output_directory}/.benchmark-storage.XXXXXX")"
benchmark_passed=0
current_case_dir=''
current_window=''
app_launcher_pid=''
app_launcher_pgid=''
broker_launcher_pid=''
provider_launcher_pid=''
broker_pid_file=''
provider_pid_file=''

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
    if [[ -z "${pid_file}" || ! -f "${pid_file}" ]]; then
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

cleanup_case() {
    if [[ -n "${current_window}" ]]; then
        xdotool windowclose "${current_window}" 2>/dev/null || true
    fi
    if [[ -n "${app_launcher_pid}" ]]; then
        terminate_group "${app_launcher_pid}" "${app_launcher_pgid}"
    fi
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
    current_window=''
    app_launcher_pid=''
    app_launcher_pgid=''
    broker_launcher_pid=''
    provider_launcher_pid=''
    broker_pid_file=''
    provider_pid_file=''
}

cleanup() {
    cleanup_case
    if (( benchmark_passed )); then
        find "${benchmark_root}" -depth -delete 2>/dev/null || true
        find "${storage_root}" -depth -delete 2>/dev/null || true
    else
        printf 'Desktop performance artifacts: %s\n' "${benchmark_root}" >&2
        printf 'Desktop performance storage: %s\n' "${storage_root}" >&2
    fi
    return 0
}
trap cleanup EXIT INT TERM

find_window() {
    local title="$1"
    xwininfo -root -tree 2>/dev/null \
        | awk -v expected="\"${title}\"" \
            '!found && index($0, expected) { print $1; found = 1 }'
}

start_broker() {
    local socket_path="$1"
    broker_pid_file="${current_case_dir}/broker.pid"
    # The inner shell owns these positional parameters.
    # shellcheck disable=SC2016
    distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
        sh -c 'printf "%s\n" "$$" >"$1"; exec systemd-socket-activate --seqpacket --listen="$2" "$3"' \
        sh "${broker_pid_file}" "${socket_path}" "${broker_binary}" \
        >"${current_case_dir}/broker.log" 2>&1 &
    broker_launcher_pid=$!
    for ((attempt = 0; attempt < 1200; attempt++)); do
        if [[ -S "${socket_path}" ]]; then
            return 0
        fi
        if ! kill -0 "${broker_launcher_pid}" 2>/dev/null; then
            sed -n '1,160p' "${current_case_dir}/broker.log" >&2
            return 1
        fi
        sleep 0.05
    done
    printf 'Performance broker did not create its socket.\n' >&2
    return 1
}

start_provider() {
    local socket_path="$1"
    provider_pid_file="${current_case_dir}/provider.pid"
    # The inner shell owns these positional parameters.
    # shellcheck disable=SC2016
    distrobox enter dev -- env MANGO_OVERLAY_SOCKET="${socket_path}" \
        sh -c 'printf "%s\n" "$$" >"$1"; exec "$2"' \
        sh "${provider_pid_file}" "${demo_binary}" \
        >"${current_case_dir}/provider.log" 2>&1 &
    provider_launcher_pid=$!
    sleep 0.1
    if ! kill -0 "${provider_launcher_pid}" 2>/dev/null; then
        sed -n '1,160p' "${current_case_dir}/provider.log" >&2
        return 1
    fi
}

capture_provider_pixels() {
    local screenshot="${current_case_dir}/provider.png"
    ffmpeg -v error -y -f x11grab -window_id "${current_window}" -i "${DISPLAY}" \
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

provider_pixels_visible() {
    (( $1 >= 100 && $2 >= 500 && $3 >= 20 && $4 >= 20 ))
}

read_pid_file() {
    local pid_file="$1"
    local pid=''
    if [[ -f "${pid_file}" ]]; then
        read -r pid <"${pid_file}" || true
    fi
    if [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null; then
        printf '%s\n' "${pid}"
    fi
}

metric_value() {
    local metric_file="$1"
    local metric_name="$2"
    awk -F, -v expected="${metric_name}" \
        '$3 ~ ("^" expected) { gsub(/[[:space:]]/, "", $1); print $1; exit }' \
        "${metric_file}"
}

launch_case() {
    local architecture="$1"
    local renderer="$2"
    local case_name="$3"
    local title="$4"
    local socket_path="$5"
    local layer_dir="$6"
    local architecture_directory="${build_root}/${architecture}"
    local test_binary=''
    local renderer_kind='vulkan'
    local proton_test=0

    case "${renderer}" in
    vulkan)
        test_binary="${architecture_directory}/mango-overlay-vulkan-test"
        ;;
    glx)
        renderer_kind='opengl'
        test_binary="${architecture_directory}/mango-overlay-glx-test"
        ;;
    egl)
        renderer_kind='opengl'
        test_binary="${architecture_directory}/mango-overlay-egl-test"
        ;;
    proton-opengl)
        renderer_kind='opengl'
        proton_test=1
        test_binary="${windows_build_root}/${architecture}/mango-overlay-proton-opengl-test.exe"
        ;;
    proton-d3d11)
        proton_test=1
        test_binary="${windows_build_root}/${architecture}/mango-overlay-proton-d3d11-test.exe"
        ;;
    proton-d3d12)
        proton_test=1
        test_binary="${windows_build_root}/${architecture}/mango-overlay-proton-d3d12-test.exe"
        ;;
    esac
    if [[ ! -x "${test_binary}" ]]; then
        printf 'Performance test binary is missing: %s\n' "${test_binary}" >&2
        return 66
    fi

    local manifest_name='MangoHud.x86_64.json'
    if [[ "${architecture}" == 'i686' ]]; then
        manifest_name='MangoHud.x86.json'
    fi
    local layer_library="${architecture_directory}/libMangoHud.so"
    local opengl_library="${architecture_directory}/libMangoHud_opengl.so"
    local opengl_shim="${architecture_directory}/libMangoHud_shim.so"
    local layer_manifest="${layer_dir}/${manifest_name}"
    if [[ "${case_name}" != 'baseline' && "${renderer_kind}" == 'vulkan' ]]; then
        mkdir -p "${layer_dir}"
        sed "s|\"library_path\": \"[^\"]*\"|\"library_path\": \"${layer_library}\"|" \
            "${architecture_directory}/${manifest_name}" >"${layer_manifest}"
    fi

    local runtime_preloads=()
    if [[ "${case_name}" != 'baseline' ]]; then
        for private_library_name in libgif.so.7 libsharpyuv.so.0 libwebp.so.7; do
            private_library="${architecture_directory}/${private_library_name}"
            if [[ -f "${private_library}" ]]; then
                runtime_preloads+=(--ld-preload "${private_library}")
            fi
        done
        if [[ "${renderer_kind}" == 'opengl' ]]; then
            runtime_preloads+=(--ld-preload "${opengl_shim}")
        fi
    fi

    local app_command=("${test_binary}" "${title}")
    if (( proton_test )); then
        app_command=("${repo_root}/tools/run-proton-test.sh" "${test_binary}")
    fi

    if [[ "${case_name}" == 'baseline' ]]; then
        setsid --wait env \
            -u MANGOHUD -u MANGOHUD_CONFIG -u MANGOHUD_CONFIGFILE \
            -u MANGOHUD_OPENGL_LIBS -u MANGO_OVERLAY_SOCKET \
            -u VK_IMPLICIT_LAYER_PATH -u VK_INSTANCE_LAYERS \
            -u VK_LOADER_LAYERS_DISABLE -u VK_LOADER_LAYERS_ENABLE \
            DISABLE_MANGOHUD=1 \
            MANGO_OVERLAY_DESKTOP_WIDTH=960 \
            MANGO_OVERLAY_DESKTOP_HEIGHT=600 \
            MANGO_OVERLAY_WINDOWS_TEST_TITLE="${title}" \
            MANGO_OVERLAY_PROTON_COMPAT_DATA="${storage_root}/proton-${architecture}/compatdata" \
            MANGO_OVERLAY_PROTON_INSTALL_PATH="${storage_root}/proton-${architecture}/install" \
            "${repo_root}/tools/run-in-steam-runtime.sh" \
            --filesystem "${benchmark_root}" \
            -- "${app_command[@]}" \
            >"${current_case_dir}/app.log" 2>&1 &
    elif [[ "${renderer_kind}" == 'opengl' ]]; then
        setsid --wait env \
            -u DISABLE_MANGOHUD \
            MANGOHUD=1 \
            MANGOHUD_CONFIGFILE=/dev/null \
            MANGOHUD_CONFIG='no_display=1' \
            MANGOHUD_OPENGL_LIBS="${opengl_library}" \
            MANGO_OVERLAY_SOCKET="${socket_path}" \
            MANGO_OVERLAY_DESKTOP_WIDTH=960 \
            MANGO_OVERLAY_DESKTOP_HEIGHT=600 \
            MANGO_OVERLAY_WINDOWS_TEST_TITLE="${title}" \
            MANGO_OVERLAY_PROTON_COMPAT_DATA="${storage_root}/proton-${architecture}/compatdata" \
            MANGO_OVERLAY_PROTON_INSTALL_PATH="${storage_root}/proton-${architecture}/install" \
            "${repo_root}/tools/run-in-steam-runtime.sh" \
            "${runtime_preloads[@]}" \
            --filesystem "${benchmark_root}" \
            -- "${app_command[@]}" \
            >"${current_case_dir}/app.log" 2>&1 &
    else
        setsid --wait env \
            -u DISABLE_MANGOHUD \
            -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_DISABLE -u VK_LOADER_LAYERS_ENABLE \
            MANGOHUD=1 \
            MANGOHUD_CONFIGFILE=/dev/null \
            MANGOHUD_CONFIG='no_display=1' \
            MANGO_OVERLAY_SOCKET="${socket_path}" \
            VK_IMPLICIT_LAYER_PATH="${layer_dir}" \
            MANGO_OVERLAY_DESKTOP_WIDTH=960 \
            MANGO_OVERLAY_DESKTOP_HEIGHT=600 \
            MANGO_OVERLAY_WINDOWS_TEST_TITLE="${title}" \
            MANGO_OVERLAY_PROTON_COMPAT_DATA="${storage_root}/proton-${architecture}/compatdata" \
            MANGO_OVERLAY_PROTON_INSTALL_PATH="${storage_root}/proton-${architecture}/install" \
            "${repo_root}/tools/run-in-steam-runtime.sh" \
            "${runtime_preloads[@]}" \
            --filesystem "${benchmark_root}" \
            -- "${app_command[@]}" \
            >"${current_case_dir}/app.log" 2>&1 &
    fi
    app_launcher_pid=$!
    app_launcher_pgid="${app_launcher_pid}"
}

measure_case() {
    local architecture="$1"
    local renderer="$2"
    local case_name="$3"
    cleanup_case
    current_case_dir="${benchmark_root}/${architecture}-${renderer}-${case_name}"
    mkdir -p "${current_case_dir}"
    local socket_path="${current_case_dir}/broker.sock"
    local layer_dir="${current_case_dir}/vulkan/implicit_layer.d"
    local title="Mango Overlay Perf ${architecture} ${renderer} ${case_name}"

    if [[ "${case_name}" != 'baseline' ]]; then
        start_broker "${socket_path}"
    fi
    if [[ "${case_name}" == 'scene' ]]; then
        start_provider "${socket_path}"
    fi
    launch_case \
        "${architecture}" \
        "${renderer}" \
        "${case_name}" \
        "${title}" \
        "${socket_path}" \
        "${layer_dir}"

    for ((attempt = 0; attempt < 1200; attempt++)); do
        current_window="$(find_window "${title}")"
        if [[ -n "${current_window}" ]]; then
            break
        fi
        if ! kill -0 "${app_launcher_pid}" 2>/dev/null; then
            printf 'Performance test exited before opening its window: %s %s %s\n' \
                "${architecture}" "${renderer}" "${case_name}" >&2
            sed -n '1,200p' "${current_case_dir}/app.log" >&2
            return 1
        fi
        sleep 0.05
    done
    if [[ -z "${current_window}" ]]; then
        printf 'Performance test window did not open: %s\n' "${title}" >&2
        return 1
    fi
    local window_pid
    window_pid="$(xdotool getwindowpid "${current_window}" 2>/dev/null || true)"
    if [[ ! "${window_pid}" =~ ^[0-9]+$ ]] \
        || ! kill -0 "${window_pid}" 2>/dev/null; then
        printf 'Could not resolve the renderer PID for: %s\n' "${title}" >&2
        return 1
    fi

    sleep "${warmup_seconds}"
    local provider_pixels='not-applicable'
    if [[ "${case_name}" == 'scene' ]]; then
        provider_pixels="$(capture_provider_pixels)"
        read -r green red gif_green gif_blue <<<"${provider_pixels}"
        if ! provider_pixels_visible \
            "${green}" "${red}" "${gif_green}" "${gif_blue}"; then
            printf 'Provider scene was not visible before performance sampling: %s\n' \
                "${provider_pixels}" >&2
            return 1
        fi
        sleep 0.2
    fi

    local measured_pids=("${window_pid}")
    local broker_pid=''
    local provider_pid=''
    if [[ "${case_name}" != 'baseline' ]]; then
        broker_pid="$(read_pid_file "${broker_pid_file}")"
        if [[ -n "${broker_pid}" ]]; then
            measured_pids+=("${broker_pid}")
        fi
    fi
    if [[ "${case_name}" == 'scene' ]]; then
        provider_pid="$(read_pid_file "${provider_pid_file}")"
        if [[ -n "${provider_pid}" ]]; then
            measured_pids+=("${provider_pid}")
        fi
    fi
    local pid_list
    pid_list="$(IFS=,; printf '%s' "${measured_pids[*]}")"
    local metric_file="${current_case_dir}/perf.csv"
    LC_ALL=C perf stat -x, -o "${metric_file}" \
        -p "${pid_list}" \
        -e task-clock,cycles,instructions,context-switches,page-faults \
        -- sleep "${sample_seconds}" &
    local perf_pid=$!
    local max_rss_kib=0
    while kill -0 "${perf_pid}" 2>/dev/null; do
        local rss_sum=0
        local pid
        for pid in "${measured_pids[@]}"; do
            if [[ -r "/proc/${pid}/status" ]]; then
                rss_kib="$(awk '$1 == "VmRSS:" { print $2; exit }' "/proc/${pid}/status")"
                if [[ "${rss_kib}" =~ ^[0-9]+$ ]]; then
                    ((rss_sum += rss_kib))
                fi
            fi
        done
        if (( rss_sum > max_rss_kib )); then
            max_rss_kib=${rss_sum}
        fi
        sleep 0.1
    done
    if ! wait "${perf_pid}"; then
        printf 'perf failed for %s %s %s.\n' \
            "${architecture}" "${renderer}" "${case_name}" >&2
        return 1
    fi

    local task_clock_ms cycles instructions context_switches page_faults
    task_clock_ms="$(metric_value "${metric_file}" 'task-clock')"
    cycles="$(metric_value "${metric_file}" 'cycles')"
    instructions="$(metric_value "${metric_file}" 'instructions')"
    context_switches="$(metric_value "${metric_file}" 'context-switches')"
    page_faults="$(metric_value "${metric_file}" 'page-faults')"
    if [[ ! "${task_clock_ms}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        printf 'Could not parse task-clock for %s %s %s.\n' \
            "${architecture}" "${renderer}" "${case_name}" >&2
        sed -n '1,120p' "${metric_file}" >&2
        return 1
    fi
    local average_cpu_percent
    average_cpu_percent="$(awk -v task_ms="${task_clock_ms}" -v seconds="${sample_seconds}" \
        'BEGIN { printf "%.2f", task_ms / (seconds * 10.0) }')"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${architecture}" \
        "${renderer}" \
        "${case_name}" \
        "${task_clock_ms}" \
        "${average_cpu_percent}" \
        "${max_rss_kib}" \
        "${cycles:-0}" \
        "${instructions:-0}" \
        "${context_switches:-0}" \
        "${page_faults:-0}" \
        "${provider_pixels}" \
        >>"${output_path}"
    printf 'Measured %-6s %-14s %-8s CPU=%6s%% RSS=%7s KiB PIDs=%s\n' \
        "${architecture}" \
        "${renderer}" \
        "${case_name}" \
        "${average_cpu_percent}" \
        "${max_rss_kib}" \
        "${pid_list}"

    xdotool windowclose "${current_window}" 2>/dev/null || true
    current_window=''
    for ((attempt = 0; attempt < 40; attempt++)); do
        if ! kill -0 "${app_launcher_pid}" 2>/dev/null; then
            wait "${app_launcher_pid}" 2>/dev/null || true
            app_launcher_pid=''
            app_launcher_pgid=''
            break
        fi
        sleep 0.05
    done
    cleanup_case
}

printf 'architecture,renderer,case,task_clock_ms,average_cpu_percent,max_rss_kib,cycles,instructions,context_switches,page_faults,provider_pixels\n' \
    >"${output_path}"

for architecture in "${architectures[@]}"; do
    for renderer in "${renderers[@]}"; do
        for case_name in baseline idle scene; do
            measure_case "${architecture}" "${renderer}" "${case_name}"
        done
    done
done

printf '\nDesktop renderer performance deltas (single sample, vsync-limited):\n'
awk -F, '
    NR == 1 { next }
    {
        key = $1 SUBSEP $2
        cpu[key, $3] = $5
        rss[key, $3] = $6
        arch[key] = $1
        renderer[key] = $2
        order[++count] = key
    }
    END {
        print "architecture renderer       baseline_cpu idle_delta scene_delta baseline_rss idle_delta scene_delta"
        for (row = 1; row <= count; ++row) {
            key = order[row]
            if (seen[key]++) { continue }
            printf "%-12s %-14s %12.2f %10.2f %11.2f %12d %10d %11d\n",
                arch[key], renderer[key], cpu[key, "baseline"],
                cpu[key, "idle"] - cpu[key, "baseline"],
                cpu[key, "scene"] - cpu[key, "baseline"],
                rss[key, "baseline"],
                rss[key, "idle"] - rss[key, "baseline"],
                rss[key, "scene"] - rss[key, "baseline"]
        }
    }' "${output_path}"

benchmark_passed=1
printf '\nDesktop performance report: %s\n' "${output_path}"
