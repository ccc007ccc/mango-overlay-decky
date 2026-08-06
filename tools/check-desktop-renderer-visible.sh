#!/usr/bin/env bash

set -euo pipefail

if (( $# < 1 )); then
    printf 'Usage: tools/check-desktop-renderer-visible.sh <vulkan|opengl> [test-command ...]\n' >&2
    exit 64
fi

renderer="$1"
shift
test_application=("$@")
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_base="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

case "${renderer}" in
vulkan)
    renderer_label='KDE Vulkan'
    window_title="${MANGO_OVERLAY_TEST_WINDOW_TITLE:-Vkcube X11}"
    ;;
opengl)
    renderer_label='KDE OpenGL'
    window_title="${MANGO_OVERLAY_TEST_WINDOW_TITLE:-glxgears}"
    ;;
*)
    printf 'Unknown desktop renderer: %s\n' "${renderer}" >&2
    exit 64
    ;;
esac

check_dir="$(mktemp -d "${runtime_base}/mango-overlay-${renderer}-check.XXXXXX")"
run_log="${check_dir}/run.log"
screenshot="${check_dir}/active-window.png"
launcher_pid=''
launcher_pgid=''
window_id=''
check_passed=0
startup_timeout="${MANGO_OVERLAY_TEST_STARTUP_TIMEOUT:-5}"
require_base_content="${MANGO_OVERLAY_TEST_REQUIRE_BASE_CONTENT:-0}"

if [[ ! "${startup_timeout}" =~ ^[0-9]+$ ]] \
    || (( startup_timeout < 1 || startup_timeout > 60 )); then
    printf 'MANGO_OVERLAY_TEST_STARTUP_TIMEOUT must be 1-60 seconds.\n' >&2
    exit 64
fi
if [[ "${require_base_content}" != '0' \
      && "${require_base_content}" != '1' ]]; then
    printf 'MANGO_OVERLAY_TEST_REQUIRE_BASE_CONTENT must be 0 or 1.\n' >&2
    exit 64
fi
startup_attempts=$((startup_timeout * 20))

cleanup() {
    if [[ -n "${window_id}" ]]; then
        xdotool windowclose "${window_id}" 2>/dev/null || true
    fi
    if [[ -n "${launcher_pid}" ]]; then
        if [[ -n "${window_id}" ]]; then
            for ((attempt = 0; attempt < 40; attempt++)); do
                if ! kill -0 "${launcher_pid}" 2>/dev/null; then
                    break
                fi
                sleep 0.05
            done
        fi
        if kill -0 "${launcher_pid}" 2>/dev/null; then
            if [[ "${launcher_pgid}" =~ ^[0-9]+$ ]]; then
                kill -- "-${launcher_pgid}" 2>/dev/null || true
            else
                kill "${launcher_pid}" 2>/dev/null || true
            fi
        fi
        for ((attempt = 0; attempt < 20; attempt++)); do
            if ! kill -0 "${launcher_pid}" 2>/dev/null; then
                break
            fi
            sleep 0.05
        done
        if kill -0 "${launcher_pid}" 2>/dev/null; then
            if [[ "${launcher_pgid}" =~ ^[0-9]+$ ]]; then
                kill -KILL -- "-${launcher_pgid}" 2>/dev/null || true
            else
                kill -KILL "${launcher_pid}" 2>/dev/null || true
            fi
        fi
        wait "${launcher_pid}" 2>/dev/null || true
    fi
    if (( check_passed )); then
        rm -f "${run_log}" "${screenshot}"
        rmdir "${check_dir}" 2>/dev/null || true
    else
        printf '%s check artifacts: %s\n' "${renderer_label}" "${check_dir}" >&2
    fi
}
trap cleanup EXIT INT TERM

for command_name in ffmpeg setsid xdotool xwininfo xxd; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'Required host command is missing: %s\n' "${command_name}" >&2
        exit 69
    fi
done

if xwininfo -root -tree 2>/dev/null \
    | grep -F "\"${window_title}\"" >/dev/null; then
    printf 'Close the existing %s window before running this check.\n' \
        "${window_title}" >&2
    exit 75
fi

if [[ "${renderer}" == 'vulkan' ]]; then
    setsid --wait env \
        MANGO_OVERLAY_DESKTOP_WIDTH=960 \
        MANGO_OVERLAY_DESKTOP_HEIGHT=600 \
        MANGO_OVERLAY_VKCUBE_FRAME_COUNT=6000 \
        MANGOHUD_CONFIG='no_display=1' \
        "${repo_root}/tools/run-desktop-vulkan.sh" \
        "${test_application[@]}" \
        >"${run_log}" 2>&1 &
else
    setsid --wait env \
        MANGO_OVERLAY_DESKTOP_WIDTH=960 \
        MANGO_OVERLAY_DESKTOP_HEIGHT=600 \
        MANGOHUD_CONFIG='no_display=1' \
        "${repo_root}/tools/run-desktop-opengl.sh" \
        "${test_application[@]}" \
        >"${run_log}" 2>&1 &
fi
launcher_pid=$!
launcher_pgid="${launcher_pid}"

for ((attempt = 0; attempt < startup_attempts; attempt++)); do
    window_id="$(
        xwininfo -root -tree 2>/dev/null \
            | awk -v title="\"${window_title}\"" \
                '!found && index($0, title) { print $1; found = 1 }'
    )"
    if [[ -n "${window_id}" ]]; then
        break
    fi
    if ! kill -0 "${launcher_pid}" 2>/dev/null; then
        printf '%s test application exited before opening its window.\n' \
            "${renderer_label}" >&2
        sed -n '1,160p' "${run_log}" >&2
        exit 1
    fi
    sleep 0.05
done

if [[ -z "${window_id}" ]]; then
    printf '%s test application did not open within %s seconds.\n' \
        "${renderer_label}" "${startup_timeout}" >&2
    exit 1
fi

capture_provider_pixels() {
    if ! ffmpeg \
        -v error \
        -y \
        -f x11grab \
        -window_id "${window_id}" \
        -i "${DISPLAY}" \
        -frames:v 1 \
        "${screenshot}"; then
        return 1
    fi

    ffmpeg \
        -v error \
        -i "${screenshot}" \
        -f rawvideo \
        -pix_fmt rgb24 \
        - \
        | xxd -p -c3 \
        | awk '
            $0 == "1fc78a" { green++ }
            $0 == "ff0000" { red++ }
            {
                r = strtonum("0x" substr($0, 1, 2))
                g = strtonum("0x" substr($0, 3, 2))
                b = strtonum("0x" substr($0, 5, 2))
                gif_green_distance = (r - 0) ^ 2 + (g - 220) ^ 2 + (b - 150) ^ 2
                gif_blue_distance = (r - 40) ^ 2 + (g - 90) ^ 2 + (b - 255) ^ 2
                if (gif_green_distance <= 400) { gif_green++ }
                if (gif_blue_distance <= 400) { gif_blue++ }
            }
            END {
                print green + 0, red + 0, gif_green + 0, gif_blue + 0
            }'
}

provider_pixels_visible() {
    (( $1 >= 100 && $2 >= 500 && $3 >= 20 && $4 >= 20 ))
}

capture_base_nonblack_pixels() {
    ffmpeg \
        -v error \
        -i "${screenshot}" \
        -vf 'crop=400:300:(iw-400)/2:(ih-300)/2' \
        -f rawvideo \
        -pix_fmt rgb24 \
        - \
        | xxd -p -c3 \
        | awk '
            {
                r = strtonum("0x" substr($0, 1, 2))
                g = strtonum("0x" substr($0, 3, 2))
                b = strtonum("0x" substr($0, 5, 2))
                if (r >= 32 || g >= 32 || b >= 32) { nonblack++ }
            }
            END { print nonblack + 0 }'
}

green_pixels=0
red_pixels=0
gif_green_pixels=0
gif_blue_pixels=0
stable_samples=0
minimum_green_pixels=0
minimum_red_pixels=0
minimum_gif_green_pixels=0
minimum_gif_blue_pixels=0
declare -A gif_frame_hashes=()
for ((attempt = 0; attempt < 50; attempt++)); do
    if ! pixels="$(capture_provider_pixels)"; then
        break
    fi
    read -r green_pixels red_pixels gif_green_pixels gif_blue_pixels \
        <<<"${pixels}"

    if provider_pixels_visible \
        "${green_pixels}" \
        "${red_pixels}" \
        "${gif_green_pixels}" \
        "${gif_blue_pixels}"; then
        gif_frame_hash="$(
            ffmpeg \
                -v error \
                -i "${screenshot}" \
                -vf 'crop=40:60:880:75' \
                -f md5 \
                - \
                2>/dev/null
        )"
        gif_frame_hashes["${gif_frame_hash}"]=1
        if (( stable_samples == 0 )); then
            minimum_green_pixels=${green_pixels}
            minimum_red_pixels=${red_pixels}
            minimum_gif_green_pixels=${gif_green_pixels}
            minimum_gif_blue_pixels=${gif_blue_pixels}
        else
            if (( green_pixels < minimum_green_pixels )); then
                minimum_green_pixels=${green_pixels}
            fi
            if (( red_pixels < minimum_red_pixels )); then
                minimum_red_pixels=${red_pixels}
            fi
            if (( gif_green_pixels < minimum_gif_green_pixels )); then
                minimum_gif_green_pixels=${gif_green_pixels}
            fi
            if (( gif_blue_pixels < minimum_gif_blue_pixels )); then
                minimum_gif_blue_pixels=${gif_blue_pixels}
            fi
        fi
        ((stable_samples += 1))
        if (( stable_samples >= 16 )); then
            break
        fi
    elif (( stable_samples > 0 )); then
        printf '%s provider image became unstable after %s good samples: green=%s red=%s gif-green=%s gif-blue=%s\n' \
            "${renderer_label}" \
            "${stable_samples}" \
            "${green_pixels}" \
            "${red_pixels}" \
            "${gif_green_pixels}" \
            "${gif_blue_pixels}" >&2
        exit 1
    fi
    if ! kill -0 "${launcher_pid}" 2>/dev/null; then
        break
    fi
    sleep 0.07
done

printf '%s provider minima over %s frames: green=%s red=%s gif-green=%s gif-blue=%s animation-frames=%s\n' \
    "${renderer_label}" \
    "${stable_samples}" \
    "${minimum_green_pixels}" \
    "${minimum_red_pixels}" \
    "${minimum_gif_green_pixels}" \
    "${minimum_gif_blue_pixels}" \
    "${#gif_frame_hashes[@]}"
if (( stable_samples < 16 || minimum_green_pixels < 100 )); then
    printf 'The provider canvas is not visible in the %s window.\n' \
        "${renderer_label}" >&2
    exit 1
fi
if (( minimum_red_pixels < 500 || minimum_gif_green_pixels < 20
      || minimum_gif_blue_pixels < 20 )); then
    printf 'The provider PNG or GIF is not visible in the %s window.\n' \
        "${renderer_label}" >&2
    exit 1
fi
if (( ${#gif_frame_hashes[@]} < 2 )); then
    printf 'The provider GIF did not visibly advance in the %s window.\n' \
        "${renderer_label}" >&2
    exit 1
fi
if (( require_base_content )); then
    base_nonblack_pixels="$(capture_base_nonblack_pixels)"
    printf '%s base content: nonblack-pixels=%s\n' \
        "${renderer_label}" "${base_nonblack_pixels}"
    if (( base_nonblack_pixels < 1000 )); then
        printf 'The test application base image is blank in the %s window.\n' \
            "${renderer_label}" >&2
        exit 1
    fi
fi

wait_for_provider_samples() {
    local maximum_attempts="$1"
    recovered_samples=0
    for ((attempt = 0; attempt < maximum_attempts; attempt++)); do
        sleep 0.07
        if pixels="$(capture_provider_pixels)"; then
            read -r green_pixels red_pixels gif_green_pixels gif_blue_pixels \
                <<<"${pixels}"
            if provider_pixels_visible \
                "${green_pixels}" \
                "${red_pixels}" \
                "${gif_green_pixels}" \
                "${gif_blue_pixels}"; then
                ((recovered_samples += 1))
            else
                recovered_samples=0
            fi
        else
            recovered_samples=0
        fi
        if (( recovered_samples >= 3 )); then
            return 0
        fi
    done
    return 1
}

for geometry in 800x500 1200x750 640x400 960x600; do
    width="${geometry%x*}"
    height="${geometry#*x}"
    xdotool windowsize --sync "${window_id}" "${width}" "${height}"
    wait_for_provider_samples 30 || true
    printf '%s resize %s: green=%s red=%s gif-green=%s gif-blue=%s stable-frames=%s\n' \
        "${renderer_label}" \
        "${geometry}" \
        "${green_pixels}" \
        "${red_pixels}" \
        "${gif_green_pixels}" \
        "${gif_blue_pixels}" \
        "${recovered_samples}"
    if (( recovered_samples < 3 )); then
        printf '%s provider canvas did not recover after resizing to %s.\n' \
            "${renderer_label}" "${geometry}" >&2
        exit 1
    fi
done

xdotool windowstate --add FULLSCREEN "${window_id}"
wait_for_provider_samples 80 || true
fullscreen_geometry="$(
    xdotool getwindowgeometry --shell "${window_id}" \
        | awk -F= '
            $1 == "WIDTH" { width = $2 }
            $1 == "HEIGHT" { height = $2 }
            END { print width "x" height }'
)"
printf '%s fullscreen %s: green=%s red=%s gif-green=%s gif-blue=%s stable-frames=%s\n' \
    "${renderer_label}" \
    "${fullscreen_geometry}" \
    "${green_pixels}" \
    "${red_pixels}" \
    "${gif_green_pixels}" \
    "${gif_blue_pixels}" \
    "${recovered_samples}"
if (( recovered_samples < 3 )); then
    printf '%s provider canvas did not recover after entering fullscreen.\n' \
        "${renderer_label}" >&2
    exit 1
fi
if (( require_base_content )); then
    fullscreen_base_nonblack_pixels="$(capture_base_nonblack_pixels)"
    printf '%s fullscreen base content: nonblack-pixels=%s\n' \
        "${renderer_label}" "${fullscreen_base_nonblack_pixels}"
    if (( fullscreen_base_nonblack_pixels < 1000 )); then
        printf 'The test application base image is blank in fullscreen for %s.\n' \
            "${renderer_label}" >&2
        exit 1
    fi
fi

xdotool windowstate --remove FULLSCREEN "${window_id}"
wait_for_provider_samples 50 || true
printf '%s fullscreen exit: green=%s red=%s gif-green=%s gif-blue=%s stable-frames=%s\n' \
    "${renderer_label}" \
    "${green_pixels}" \
    "${red_pixels}" \
    "${gif_green_pixels}" \
    "${gif_blue_pixels}" \
    "${recovered_samples}"
if (( recovered_samples < 3 )); then
    printf '%s provider canvas did not recover after leaving fullscreen.\n' \
        "${renderer_label}" >&2
    exit 1
fi

check_passed=1
printf '%s provider canvas, resize, and fullscreen check passed.\n' \
    "${renderer_label}"
