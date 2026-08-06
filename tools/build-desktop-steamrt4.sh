#!/usr/bin/env bash

set -euo pipefail

readonly STEAMRT_VERSION='4.0.20260608.242786'
readonly STEAMRT_ARCHIVE='com.valvesoftware.SteamRuntime.Sdk-amd64,i386-steamrt4-sysroot.tar.gz'
readonly STEAMRT_SHA256='91f67b5ecb3c951d14c9e1d3e764bbe3dd865f5e11ed1221dfbf79b1ea92d965'
readonly STEAMRT_URL="https://repo.steampowered.com/steamrt4/images/${STEAMRT_VERSION}/${STEAMRT_ARCHIVE}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache_root="${MANGO_OVERLAY_STEAMRT_CACHE:-/var/tmp/mango-overlay-decky/steamrt4-${STEAMRT_VERSION}}"
archive_path="${cache_root}/${STEAMRT_ARCHIVE}"
sdk_root="${cache_root}/root"
output_root="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${repo_root}/build/steamrt4-desktop}"
requested_architecture="${1:-x86_64}"
apt_lists_cache="${cache_root}/apt-lists"
apt_archives_cache="${cache_root}/apt-archives"
temporary_path=''

usage() {
    printf 'Usage: tools/build-desktop-steamrt4.sh [x86_64|i686|all]\n' >&2
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

cleanup_temporary() {
    if [[ -z "${temporary_path}" || ! -e "${temporary_path}" ]]; then
        return
    fi
    case "${temporary_path}" in
    "${cache_root}"/.sdk-* | "${output_root}"/.stage-*)
        find "${temporary_path}" -depth -delete
        ;;
    *)
        printf 'Refusing to clean unexpected temporary path: %s\n' \
            "${temporary_path}" >&2
        ;;
    esac
}
trap cleanup_temporary EXIT

if [[ ! -f /run/.containerenv ]]; then
    printf 'Run this script in the distrobox dev container.\n' >&2
    exit 1
fi
if [[ "$(uname -m)" != 'x86_64' ]]; then
    printf 'The Steam Runtime desktop build requires an x86_64 dev container.\n' >&2
    exit 1
fi
for command_name in bwrap curl install sha256sum sudo tar; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'Missing build command in the dev container: %s\n' \
            "${command_name}" >&2
        exit 1
    fi
done

mkdir -p \
    "${cache_root}" \
    "${output_root}" \
    "${apt_lists_cache}/partial" \
    "${apt_archives_cache}/partial"

if [[ -f "${archive_path}" ]]; then
    actual_sha256="$(sha256sum "${archive_path}" | cut -d ' ' -f 1)"
    if [[ "${actual_sha256}" != "${STEAMRT_SHA256}" ]]; then
        printf 'Cached Steam Runtime archive has the wrong SHA-256: %s\n' \
            "${archive_path}" >&2
        exit 1
    fi
else
    temporary_path="$(mktemp "${cache_root}/.sdk-download.XXXXXX")"
    curl \
        --fail \
        --location \
        --proto '=https' \
        --retry 3 \
        --show-error \
        --tlsv1.2 \
        --output "${temporary_path}" \
        "${STEAMRT_URL}"
    actual_sha256="$(sha256sum "${temporary_path}" | cut -d ' ' -f 1)"
    if [[ "${actual_sha256}" != "${STEAMRT_SHA256}" ]]; then
        printf 'Downloaded Steam Runtime archive failed SHA-256 verification.\n' >&2
        exit 1
    fi
    mv "${temporary_path}" "${archive_path}"
    temporary_path=''
fi

if [[ ! -x "${sdk_root}/usr/bin/meson" ]]; then
    if [[ -e "${sdk_root}" ]]; then
        printf 'Steam Runtime SDK cache exists but is incomplete: %s\n' \
            "${sdk_root}" >&2
        exit 1
    fi
    temporary_path="$(mktemp -d "${cache_root}/.sdk-root.XXXXXX")"
    tar -xf "${archive_path}" -C "${temporary_path}"
    if [[ ! -x "${temporary_path}/usr/bin/meson" ]]; then
        printf 'Steam Runtime SDK archive has an unexpected layout.\n' >&2
        exit 1
    fi
    chmod 1777 "${temporary_path}/tmp"
    mv "${temporary_path}" "${sdk_root}"
    temporary_path=''
fi
chmod 1777 "${sdk_root}/tmp"

sdk_base=(
    bwrap
    --unshare-user
    --uid 0
    --gid 0
    --unshare-pid
    --unshare-ipc
    --unshare-uts
    --unshare-cgroup-try
    --die-with-parent
    --bind "${sdk_root}" /
    --dev /dev
    --proc /proc
    --tmpfs /run
    --setenv HOME /root
    --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
)
apt_sdk_base=(
    sudo -n bwrap
    --unshare-pid
    --unshare-ipc
    --unshare-uts
    --unshare-cgroup-try
    --die-with-parent
    --bind "${sdk_root}" /
    --dev /dev
    --proc /proc
    --tmpfs /run
    --setenv HOME /root
    --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    --bind "${apt_lists_cache}" /var/lib/apt/lists
    --bind "${apt_archives_cache}" /var/cache/apt/archives
)

base_packages=(
    binutils
    file
    flatbuffers-compiler
    libegl-dev
    libflatbuffers-dev
    libgif-dev
    libgl-dev
    libjpeg-dev
    libpng-dev
    libvulkan-dev
    libwayland-dev
    libwebp-dev
    libx11-dev
    libxkbcommon-dev
    python3-mako
)
i686_packages=(
    libc6-dev:i386
    libegl-dev:i386
    libflatbuffers-dev:i386
    libgif-dev:i386
    libgl-dev:i386
    libjpeg-dev:i386
    libpng-dev:i386
    libvulkan-dev:i386
    libwayland-dev:i386
    libwebp-dev:i386
    libx11-dev:i386
    libxkbcommon-dev:i386
)

required_packages=("${base_packages[@]}")
if [[ " ${architectures[*]} " == *' i686 '* ]]; then
    required_packages+=("${i686_packages[@]}")
fi
if ! "${sdk_base[@]}" --share-net \
    /usr/bin/dpkg-query -W "${required_packages[@]}" >/dev/null 2>&1; then
    "${apt_sdk_base[@]}" --share-net \
        /usr/bin/env DEBIAN_FRONTEND=noninteractive \
        /usr/bin/apt-get -o APT::Sandbox::User=root update
    "${apt_sdk_base[@]}" --share-net \
        /usr/bin/env DEBIAN_FRONTEND=noninteractive \
        /usr/bin/apt-get -o APT::Sandbox::User=root \
        install -y --no-install-recommends \
        "${required_packages[@]}"
fi

build_architecture() {
    local architecture="$1"
    local staging_directory
    local final_directory="${output_root}/${architecture}"

    temporary_path="$(mktemp -d "${output_root}/.stage-${architecture}.XXXXXX")"
    staging_directory="${temporary_path}"

    "${sdk_base[@]}" \
        --unshare-net \
        --ro-bind "${repo_root}" /source \
        --bind "${staging_directory}" /output \
        --setenv TARGET_ARCHITECTURE "${architecture}" \
        --chdir /source \
        /bin/bash -s <<'BUILD_DESKTOP'
set -euo pipefail

case "${TARGET_ARCHITECTURE}" in
x86_64)
    build_directory='/work/mango-overlay-desktop-x86_64'
    manifest_name='MangoHud.x86_64.json'
    private_libraries=(
        /lib/x86_64-linux-gnu/libgif.so.7
    )
    expected_bits='ELF 64-bit LSB'
    expected_machine='x86-64'
    compiler=(gcc)
    ;;
i686)
    build_directory='/work/mango-overlay-desktop-i686'
    manifest_name='MangoHud.x86.json'
    private_libraries=(
        /lib/i386-linux-gnu/libgif.so.7
        /lib/i386-linux-gnu/libsharpyuv.so.0
        /lib/i386-linux-gnu/libwebp.so.7
    )
    expected_bits='ELF 32-bit LSB'
    expected_machine='Intel i386'
    compiler=(gcc -m32)
    export CC='gcc -m32'
    export CXX='g++ -m32'
    export PKG_CONFIG_LIBDIR='/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig'
    unset PKG_CONFIG_PATH
    ;;
*)
    printf 'Unsupported target architecture: %s\n' \
        "${TARGET_ARCHITECTURE}" >&2
    exit 64
    ;;
esac

meson_arguments=(
    "${build_directory}"
    /source
    --buildtype=release
    -Dinclude_doc=false
    -Dmango_overlay_decky=enabled
    -Dmangoapp=false
    -Dmangohudctl=false
    -Dmangoplot=disabled
    -Dtests=disabled
    -Dwith_dbus=disabled
    -Dwith_nvml=disabled
    -Dwith_xnvctrl=disabled
)
if [[ -f "${build_directory}/build.ninja" ]]; then
    meson setup --reconfigure "${meson_arguments[@]}"
else
    meson setup "${meson_arguments[@]}"
fi
ninja -C "${build_directory}" \
    src/libMangoHud.so \
    src/libMangoHud_opengl.so \
    src/libMangoHud_shim.so

install -m 0755 \
    "${build_directory}/src/libMangoHud.so" \
    "${build_directory}/src/libMangoHud_opengl.so" \
    "${build_directory}/src/libMangoHud_shim.so" \
    /output/
install -m 0644 "${build_directory}/src/${manifest_name}" /output/
for private_library in "${private_libraries[@]}"; do
    install -m 0644 \
        "${private_library}" \
        "/output/$(basename "${private_library}")"
done

"${compiler[@]}" \
    -std=c11 -O2 -Wall -Wextra -Werror \
    /source/tools/glx-test-window.c \
    -o /output/mango-overlay-glx-test \
    -lGL -lX11
"${compiler[@]}" \
    -std=c11 -O2 -Wall -Wextra -Werror \
    /source/tools/egl-test-window.c \
    -o /output/mango-overlay-egl-test \
    -lEGL -lGL -lX11
"${compiler[@]}" \
    -std=c11 -O2 -Wall -Wextra -Werror \
    /source/tools/vulkan-test-window.c \
    -o /output/mango-overlay-vulkan-test \
    -lvulkan -lX11
"${compiler[@]}" \
    -std=c11 -O2 -Wall -Wextra -Werror \
    /source/tools/vulkan-two-swapchain-test-window.c \
    -o /output/mango-overlay-vulkan-two-swapchains \
    -lvulkan -lX11

artifacts=(
    /output/libMangoHud.so
    /output/libMangoHud_opengl.so
    /output/libMangoHud_shim.so
    /output/mango-overlay-glx-test
    /output/mango-overlay-egl-test
    /output/mango-overlay-vulkan-test
    /output/mango-overlay-vulkan-two-swapchains
)
for private_library in "${private_libraries[@]}"; do
    artifacts+=("/output/$(basename "${private_library}")")
done
for artifact in "${artifacts[@]}"; do
    description="$(file -Lb "${artifact}")"
    if [[ "${description}" != *"${expected_bits}"* \
        || "${description}" != *"${expected_machine}"* ]]; then
        printf 'Unexpected %s artifact: %s: %s\n' \
            "${TARGET_ARCHITECTURE}" "${artifact}" "${description}" >&2
        exit 65
    fi

    dependencies="$(ldd "${artifact}")"
    if grep -Fq 'not found' <<<"${dependencies}"; then
        printf 'Unresolved dependency for %s:\n%s\n' \
            "${artifact}" "${dependencies}" >&2
        exit 69
    fi

    highest_glibc="$(
        readelf --version-info "${artifact}" 2>/dev/null \
            | sed -n 's/.*Name: GLIBC_\([0-9.]*\).*/\1/p' \
            | sort -V \
            | tail -n 1
    )"
    if [[ -n "${highest_glibc}" \
        && "$(printf '%s\n' "${highest_glibc}" '2.41' | sort -V | tail -n 1)" != '2.41' ]]; then
        printf '%s requires GLIBC_%s, newer than SteamRT4 GLIBC_2.41.\n' \
            "${artifact}" "${highest_glibc}" >&2
        exit 65
    fi
    printf 'Verified SteamRT4 %s artifact: %s (GLIBC_%s)\n' \
        "${TARGET_ARCHITECTURE}" "${artifact}" "${highest_glibc:-none}"
done

if [[ ! -f "/output/${manifest_name}" ]] \
    || ! grep -Fq "VK_LAYER_MANGOHUD_overlay_" "/output/${manifest_name}"; then
    printf 'Vulkan layer manifest is missing or invalid: %s\n' \
        "/output/${manifest_name}" >&2
    exit 65
fi
BUILD_DESKTOP

    mkdir -p "${final_directory}"
    install -m 0755 \
        "${staging_directory}/libMangoHud.so" \
        "${staging_directory}/libMangoHud_opengl.so" \
        "${staging_directory}/libMangoHud_shim.so" \
        "${staging_directory}/mango-overlay-glx-test" \
        "${staging_directory}/mango-overlay-egl-test" \
        "${staging_directory}/mango-overlay-vulkan-test" \
        "${staging_directory}/mango-overlay-vulkan-two-swapchains" \
        "${final_directory}/"
    private_runtime_libraries=("${staging_directory}/libgif.so.7")
    if [[ "${architecture}" == 'i686' ]]; then
        private_runtime_libraries+=(
            "${staging_directory}/libsharpyuv.so.0"
            "${staging_directory}/libwebp.so.7"
        )
    fi
    install -m 0644 \
        "${staging_directory}/MangoHud."*.json \
        "${private_runtime_libraries[@]}" \
        "${final_directory}/"

    find "${staging_directory}" -depth -delete
    temporary_path=''
    printf 'SteamRT4 %s desktop artifacts: %s\n' \
        "${architecture}" "${final_directory}"
}

for architecture in "${architectures[@]}"; do
    build_architecture "${architecture}"
done
