#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
architecture="${1:-x86_64}"
output_root="${MANGO_OVERLAY_MULTISWAPCHAIN_OUTPUT:-${repo_root}/build/steamrt4-desktop}"

case "${architecture}" in
x86_64)
    expected_description='ELF 64-bit LSB pie executable, x86-64'
    ;;
i686)
    expected_description='ELF 32-bit LSB pie executable, Intel i386'
    ;;
*)
    printf 'Usage: tools/build-desktop-multiswapchain.sh [x86_64|i686]\n' >&2
    exit 64
    ;;
esac

output_directory="${output_root}/${architecture}"
output_binary="${output_directory}/mango-overlay-vulkan-two-swapchains"

distrobox enter dev -- tools/build-desktop-steamrt4.sh "${architecture}"

description="$(distrobox enter dev -- file -Lb "${output_binary}")"
if [[ "${description}" != *"${expected_description}"* ]]; then
    printf 'Unexpected %s multi-swapchain test executable: %s\n' \
        "${architecture}" "${description}" >&2
    exit 65
fi
printf 'Built %s multi-swapchain test: %s\n' "${architecture}" "${output_binary}"
