#!/usr/bin/env bash
# Flash DESPOT firmware with the pinned ESP-IDF version.
#
# Usage:
#   scripts/flash.sh                 # auto-detect port (prefers /dev/ttyUSB0)
#   scripts/flash.sh -p /dev/ttyACM0 # override port explicitly
#   scripts/flash.sh monitor         # flash then monitor
#
# Environment:
#   IDF_PATH     path to ESP-IDF (default: ~/projects/esp-idf)
#   REQUIRE_IDF  1 (default) to hard-fail on IDF version mismatch, 0 to warn
#   PORT         serial port (default: auto-detect, preferring /dev/ttyUSB0)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Preferred default port for this board (CrowPanel enumerates as ttyUSB0).
DEFAULT_PORT="/dev/ttyUSB0"

# shellcheck source=scripts/idf_env.sh
source "$SCRIPT_DIR/idf_env.sh"

cd "$PROJECT_DIR"

# If the caller already passed -p, let idf.py use it and don't auto-detect.
if printf '%s\0' "$@" | grep -qzx -- '-p'; then
    idf.py flash "$@"
    exit $?
fi

# Resolve the port: honor PORT if set, else prefer the default, else fall back
# to the first available USB/ACM serial device.
resolve_port() {
    if [ -n "${PORT:-}" ]; then
        if [ -e "$PORT" ]; then
            printf '%s' "$PORT"
            return 0
        fi
        echo "ERROR: requested PORT '$PORT' not found." >&2
        return 1
    fi

    if [ -e "$DEFAULT_PORT" ]; then
        printf '%s' "$DEFAULT_PORT"
        return 0
    fi

    # Fall back to the first candidate serial device found.
    for dev in /dev/ttyUSB* /dev/ttyACM*; do
        [ -e "$dev" ] || continue
        echo "Default port $DEFAULT_PORT not found; using $dev" >&2
        printf '%s' "$dev"
        return 0
    done

    return 1
}

if ! port="$(resolve_port)"; then
    echo "ERROR: no serial device found (looked for $DEFAULT_PORT, /dev/ttyUSB*, /dev/ttyACM*)." >&2
    echo "Available serial devices:" >&2
    found=0
    for dev in /dev/ttyUSB* /dev/ttyACM* /dev/tty.usb* /dev/serial/by-id/*; do
        [ -e "$dev" ] || continue
        echo "  $dev" >&2
        found=1
    done
    [ "$found" = "1" ] || echo "  (none)" >&2
    echo "Pass one explicitly:  scripts/flash.sh -p /dev/ttyXXX" >&2
    exit 1
fi

echo "Flashing on $port"
idf.py -p "$port" flash "$@"
