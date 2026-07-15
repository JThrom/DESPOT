#!/usr/bin/env bash
# Build DESPOT firmware with the pinned ESP-IDF version.
#
# Usage:
#   scripts/build.sh                 # default board (CrowPanel Advanced 9")
#   scripts/build.sh --waveshare     # secondary Waveshare P4 7" board
#   scripts/build.sh <idf.py args>   # any extra args pass through to idf.py
#
# Environment:
#   IDF_PATH     path to ESP-IDF (default: ~/projects/esp-idf)
#   REQUIRE_IDF  1 (default) to hard-fail on IDF version mismatch, 0 to warn
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=scripts/idf_env.sh
source "$SCRIPT_DIR/idf_env.sh"

cd "$PROJECT_DIR"

# Ensure submodules (the vendored libssh2_esp fork) are present.
if [ ! -f third_party/libssh2_esp/CMakeLists.txt ]; then
    echo "libssh2_esp submodule missing; initializing submodules..."
    git submodule update --init --recursive
fi

SDKCONFIG_ARG=()
EXTRA_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --waveshare)
            SDKCONFIG_ARG=(-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.waveshare_p4_7")
            ;;
        *)
            EXTRA_ARGS+=("$arg")
            ;;
    esac
done

idf.py set-target esp32p4
idf.py "${SDKCONFIG_ARG[@]}" build "${EXTRA_ARGS[@]}"
