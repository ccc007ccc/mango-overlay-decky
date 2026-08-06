#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

exec env \
    MANGO_OVERLAY_EGL_ARCHITECTURE=i686 \
    "${repo_root}/tools/check-desktop-egl-visible.sh"
