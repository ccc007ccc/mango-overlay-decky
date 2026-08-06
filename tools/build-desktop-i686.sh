#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${MANGO_OVERLAY_I686_BUILD_DIR:-${repo_root}/build/overlay-i686}"
pkg_config_path='/usr/lib32/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig'

setup_command=(meson setup)
if [[ -f "${build_dir}/build.ninja" ]]; then
    setup_command+=(--reconfigure)
fi
setup_command+=(
    "${build_dir}"
    "${repo_root}"
    -Dbuildtype=debug
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

distrobox enter dev -- env \
    CC='gcc -m32' \
    CXX='g++ -m32' \
    PKG_CONFIG_PATH="${pkg_config_path}" \
    "${setup_command[@]}"

machine_file="${build_dir}/meson-info/intro-machines.json"
if ! grep -Eq '"cpu_family"[[:space:]]*:[[:space:]]*"x86"' "${machine_file}" \
    || ! grep -Eq '"is_64_bit"[[:space:]]*:[[:space:]]*false' "${machine_file}"; then
    printf 'Meson build directory is not configured for i686: %s\n' \
        "${build_dir}" >&2
    printf '%s\n' \
        'Use a fresh MANGO_OVERLAY_I686_BUILD_DIR and run this command again.' >&2
    exit 65
fi

artifacts=(
    "${build_dir}/src/libMangoHud.so"
    "${build_dir}/src/libMangoHud_opengl.so"
    "${build_dir}/src/libMangoHud_shim.so"
)

distrobox enter dev -- ninja -C "${build_dir}" \
    src/libMangoHud.so \
    src/libMangoHud_opengl.so \
    src/libMangoHud_shim.so

distrobox enter dev -- bash -s -- "${artifacts[@]}" <<'VALIDATE_ARTIFACTS'
set -euo pipefail

for artifact in "$@"; do
    file_description="$(file -Lb "${artifact}")"
    if [[ "${file_description}" != *'ELF 32-bit LSB shared object, Intel i386'* ]]; then
        printf 'Expected an i686 shared object, got: %s: %s\n' \
            "${artifact}" "${file_description}" >&2
        exit 65
    fi

    elf_header="$(readelf -h "${artifact}")"
    if ! grep -Eq 'Class:[[:space:]]+ELF32' <<<"${elf_header}" \
        || ! grep -Eq 'Machine:[[:space:]]+Intel 80386' <<<"${elf_header}"; then
        printf 'readelf did not identify %s as ELF32/i386.\n' "${artifact}" >&2
        exit 65
    fi

    dependencies="$(ldd "${artifact}")"
    if grep -Fq 'not found' <<<"${dependencies}"; then
        printf 'Unresolved i686 dependency for %s:\n%s\n' \
            "${artifact}" "${dependencies}" >&2
        exit 69
    fi

    while IFS= read -r dependency; do
        [[ -n "${dependency}" ]] || continue
        dependency_description="$(file -Lb "${dependency}")"
        if [[ "${dependency_description}" != *'ELF 32-bit'* ]]; then
            printf 'Non-ELF32 dependency linked by %s: %s: %s\n' \
                "${artifact}" "${dependency}" "${dependency_description}" >&2
            exit 65
        fi
    done < <(
        awk '/=> \// { print $3 } /^\// { print $1 }' <<<"${dependencies}" \
            | sort -u
    )

    printf 'Verified i686 artifact: %s\n' "${artifact}"
done
VALIDATE_ARTIFACTS

manifest="${build_dir}/src/MangoHud.x86.json"
if [[ ! -f "${manifest}" ]] \
    || ! grep -Fq 'VK_LAYER_MANGOHUD_overlay_x86' "${manifest}"; then
    printf 'Expected i686 Vulkan layer manifest is missing or invalid: %s\n' \
        "${manifest}" >&2
    exit 65
fi

printf 'Verified i686 Vulkan layer manifest: %s\n' "${manifest}"
