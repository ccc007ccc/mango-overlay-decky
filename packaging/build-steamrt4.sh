#!/usr/bin/env bash
set -euo pipefail

readonly STEAMRT_VERSION="4.0.20260608.242786"
readonly STEAMRT_ARCHIVE="com.valvesoftware.SteamRuntime.Sdk-amd64,i386-steamrt4-sysroot.tar.gz"
readonly STEAMRT_SHA256="91f67b5ecb3c951d14c9e1d3e764bbe3dd865f5e11ed1221dfbf79b1ea92d965"
readonly STEAMRT_URL="https://repo.steampowered.com/steamrt4/images/${STEAMRT_VERSION}/${STEAMRT_ARCHIVE}"
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SOURCE_ROOT
readonly CACHE_ROOT="${MANGO_OVERLAY_STEAMRT_CACHE:-/var/tmp/mango-overlay-decky/steamrt4-${STEAMRT_VERSION}}"
readonly ARCHIVE_PATH="${CACHE_ROOT}/${STEAMRT_ARCHIVE}"
readonly SDK_ROOT="${CACHE_ROOT}/root"
readonly OUTPUT_DIRECTORY="${MANGO_OVERLAY_PACKAGE_OUTPUT:-${SOURCE_ROOT}/build/package}"
readonly DESKTOP_OUTPUT="${MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT:-${SOURCE_ROOT}/build/steamrt4-desktop}"

temporary_path=""

cleanup_temporary() {
    if [[ -z "${temporary_path}" || ! -e "${temporary_path}" ]]; then
        return
    fi
    if [[ "${temporary_path}" != "${CACHE_ROOT}"/.sdk-* ]]; then
        printf 'Refusing to clean unexpected temporary path: %s\n' "${temporary_path}" >&2
        return
    fi
    if [[ -d "${temporary_path}" ]]; then
        find "${temporary_path}" -depth -delete
    else
        rm -f -- "${temporary_path}"
    fi
}
trap cleanup_temporary EXIT

if [[ ! -f /run/.containerenv ]]; then
    printf 'Run this script in the distrobox dev container.\n' >&2
    exit 1
fi
if [[ "$(uname -m)" != "x86_64" ]]; then
    printf 'The SteamOS package currently supports x86_64 only.\n' >&2
    exit 1
fi
for command in bwrap cargo curl meson ninja pnpm python3 sha256sum systemd-analyze tar; do
    if ! command -v "${command}" >/dev/null; then
        printf 'Missing build command in the dev container: %s\n' "${command}" >&2
        exit 1
    fi
done

mkdir -p "${CACHE_ROOT}" "${OUTPUT_DIRECTORY}" "${DESKTOP_OUTPUT}"

if [[ -f "${ARCHIVE_PATH}" ]]; then
    actual_sha256="$(sha256sum "${ARCHIVE_PATH}" | cut -d ' ' -f 1)"
    if [[ "${actual_sha256}" != "${STEAMRT_SHA256}" ]]; then
        printf 'Cached Steam Runtime archive has the wrong SHA-256: %s\n' "${ARCHIVE_PATH}" >&2
        exit 1
    fi
else
    temporary_path="$(mktemp "${CACHE_ROOT}/.sdk-download.XXXXXX")"
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
    mv "${temporary_path}" "${ARCHIVE_PATH}"
    temporary_path=""
fi

if [[ ! -x "${SDK_ROOT}/usr/bin/meson" ]]; then
    if [[ -e "${SDK_ROOT}" ]]; then
        printf 'Steam Runtime SDK cache exists but is incomplete: %s\n' "${SDK_ROOT}" >&2
        exit 1
    fi
    temporary_path="$(mktemp -d "${CACHE_ROOT}/.sdk-root.XXXXXX")"
    tar -xf "${ARCHIVE_PATH}" -C "${temporary_path}"
    if [[ ! -x "${temporary_path}/usr/bin/meson" ]]; then
        printf 'Steam Runtime SDK archive has an unexpected layout.\n' >&2
        exit 1
    fi
    chmod 1777 "${temporary_path}/tmp"
    mv "${temporary_path}" "${SDK_ROOT}"
    temporary_path=""
fi
chmod 1777 "${SDK_ROOT}/tmp"

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
    --bind "${SDK_ROOT}" /
    --dev /dev
    --proc /proc
    --tmpfs /run
    --setenv HOME /root
    --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
)

sdk_packages=(
    flatbuffers-compiler
    libflatbuffers-dev
    libgif-dev
    libglfw3-dev
    python3-mako
)

if ! "${sdk_base[@]}" --share-net /usr/bin/dpkg-query -W "${sdk_packages[@]}" >/dev/null 2>&1; then
    "${sdk_base[@]}" --share-net \
        /usr/bin/env DEBIAN_FRONTEND=noninteractive \
        /usr/bin/apt-get update
    "${sdk_base[@]}" --share-net \
        /usr/bin/env DEBIAN_FRONTEND=noninteractive \
        /usr/bin/apt-get install -y --no-install-recommends "${sdk_packages[@]}"
fi

(
    cd "${SOURCE_ROOT}/plugin"
    pnpm install --frozen-lockfile
    pnpm verify
)

MANGO_OVERLAY_STEAMRT_CACHE="${CACHE_ROOT}" \
MANGO_OVERLAY_STEAMRT_DESKTOP_OUTPUT="${DESKTOP_OUTPUT}" \
    "${SOURCE_ROOT}/tools/build-desktop-steamrt4.sh" all

source_date_epoch="$(git -C "${SOURCE_ROOT}" log -1 --format=%ct)"
build_command=$(cat <<'EOF'
set -euo pipefail
if [[ -d /work/build ]]; then
    meson setup --wipe /work/build /source \
        --buildtype=release \
        -Dinclude_doc=false \
        -Dmango_overlay_decky=enabled \
        -Dmangoapp=true \
        -Dmangohudctl=false \
        -Dmangoplot=disabled \
        -Dtests=enabled \
        -Dwith_nvml=disabled \
        -Dwith_xnvctrl=disabled
else
    meson setup /work/build /source \
        --buildtype=release \
        -Dinclude_doc=false \
        -Dmango_overlay_decky=enabled \
        -Dmangoapp=true \
        -Dmangohudctl=false \
        -Dmangoplot=disabled \
        -Dtests=enabled \
        -Dwith_nvml=disabled \
        -Dwith_xnvctrl=disabled
fi
meson compile -C /work/build \
    mangoapp \
    mango-overlayd \
    mango-overlayctl \
    mango-overlay-test-provider \
    mango-overlay-client \
    mango-overlay-provider-client-process-test \
    mango-overlay-cpp-client-process-test

mkdir -p /work/build/runtime-deps
install -m 0644 \
    /lib/x86_64-linux-gnu/libjpeg.so.62 \
    /work/build/runtime-deps/libjpeg.so.62
install -m 0644 \
    /usr/share/doc/libjpeg62-turbo/copyright \
    /work/build/runtime-deps/libjpeg62-turbo-copyright
install -m 0644 \
    /usr/share/doc/libgif7/copyright \
    /work/build/runtime-deps/libgif7-copyright
install -m 0644 \
    /usr/share/doc/libsharpyuv0/copyright \
    /work/build/runtime-deps/libsharpyuv0-copyright
install -m 0644 \
    /usr/share/doc/libwebp7/copyright \
    /work/build/runtime-deps/libwebp7-copyright

mkdir -p /work/build/desktop/x86_64 /work/build/desktop/i686
install -m 0644 \
    /desktop/x86_64/libMangoHud.so \
    /desktop/x86_64/libMangoHud_opengl.so \
    /desktop/x86_64/libMangoHud_shim.so \
    /desktop/x86_64/libgif.so.7 \
    /work/build/desktop/x86_64/
install -m 0644 \
    /desktop/i686/libMangoHud.so \
    /desktop/i686/libMangoHud_opengl.so \
    /desktop/i686/libMangoHud_shim.so \
    /desktop/i686/libgif.so.7 \
    /desktop/i686/libsharpyuv.so.0 \
    /desktop/i686/libwebp.so.7 \
    /work/build/desktop/i686/

export LD_LIBRARY_PATH=/work/build/runtime-deps:/work/build/client
for executable in \
    /work/build/src/mangoapp \
    /work/build/broker/mango-overlayd \
    /work/build/broker/mango-overlayctl \
    /work/build/tools/mango-overlay-test-provider
do
    "${executable}" --mango-overlay-self-test
done

python3 /source/packaging/build_decky_package.py \
    --source-root /source \
    --build-root /work/build \
    --output-directory /output \
    --source-date-epoch "${SOURCE_DATE_EPOCH}"
EOF
)

"${sdk_base[@]}" \
    --unshare-net \
    --ro-bind "${SOURCE_ROOT}" /source \
    --ro-bind "${DESKTOP_OUTPUT}" /desktop \
    --bind "${OUTPUT_DIRECTORY}" /output \
    --setenv SOURCE_DATE_EPOCH "${source_date_epoch}" \
    --chdir /source \
    /bin/bash -c "${build_command}"

package_version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["version"])' "${SOURCE_ROOT}/plugin/package.json")"
package_path="${OUTPUT_DIRECTORY}/mango-overlay-decky-${package_version}.zip"
if [[ ! -f "${package_path}" ]]; then
    printf 'Expected package was not produced: %s\n' "${package_path}" >&2
    exit 1
fi
python3 "${SOURCE_ROOT}/packaging/verify-packaged-sdk.py" \
    --archive "${package_path}" \
    --source-root "${SOURCE_ROOT}" \
    --development-build "${SDK_ROOT}/work/build"
printf 'Decky package: %s\n' "${package_path}"
printf 'SHA-256: %s\n' "$(sha256sum "${package_path}" | cut -d ' ' -f 1)"
