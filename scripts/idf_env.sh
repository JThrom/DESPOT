#!/usr/bin/env bash
# Shared ESP-IDF environment setup and version enforcement for DESPOT.
#
# This is meant to be sourced by build.sh / flash.sh (and can be sourced
# directly in an interactive shell). It:
#   1. Resolves IDF_PATH (defaults to ~/projects/esp-idf).
#   2. Verifies the checked-out ESP-IDF matches the version DESPOT is
#      developed and validated against, failing fast on a mismatch.
#   3. Sources ESP-IDF's export.sh so idf.py is on PATH.
#
# Override REQUIRE_IDF=0 to downgrade the version mismatch to a warning.

# --- Pinned ESP-IDF version ---------------------------------------------------
# DESPOT is built and validated against this exact ESP-IDF snapshot.
# Keep this in sync with the README "Building From Scratch" section.
DESPOT_IDF_VERSION="v6.1-dev"
DESPOT_IDF_COMMIT="f21b4c238152dc9e3a24fbad9afe33a3d15f6cfd"
# -----------------------------------------------------------------------------

: "${IDF_PATH:=$HOME/projects/esp-idf}"
export IDF_PATH
: "${REQUIRE_IDF:=1}"

if [ ! -d "$IDF_PATH" ]; then
    echo "ERROR: IDF_PATH does not exist: $IDF_PATH" >&2
    echo "       Install ESP-IDF $DESPOT_IDF_VERSION (commit ${DESPOT_IDF_COMMIT:0:10})" >&2
    echo "       or set IDF_PATH to your checkout." >&2
    return 1 2>/dev/null || exit 1
fi

# Verify the ESP-IDF version before sourcing anything expensive.
if command -v git >/dev/null 2>&1 && git -C "$IDF_PATH" rev-parse --git-dir >/dev/null 2>&1; then
    actual_commit="$(git -C "$IDF_PATH" rev-parse HEAD 2>/dev/null)"
    actual_desc="$(git -C "$IDF_PATH" describe --tags --always 2>/dev/null)"
    if [ "$actual_commit" != "$DESPOT_IDF_COMMIT" ]; then
        echo "WARNING: ESP-IDF version mismatch." >&2
        echo "  expected: $DESPOT_IDF_VERSION @ $DESPOT_IDF_COMMIT" >&2
        echo "  found:    $actual_desc @ $actual_commit" >&2
        echo "  in:       $IDF_PATH" >&2
        if [ "$REQUIRE_IDF" = "1" ]; then
            echo "  Set REQUIRE_IDF=0 to build anyway (unsupported)." >&2
            return 1 2>/dev/null || exit 1
        fi
        echo "  REQUIRE_IDF=0 set; continuing on an unsupported ESP-IDF." >&2
    else
        echo "ESP-IDF OK: $DESPOT_IDF_VERSION @ ${DESPOT_IDF_COMMIT:0:10}"
    fi
else
    echo "WARNING: could not verify ESP-IDF version (not a git checkout at $IDF_PATH)." >&2
    if [ "$REQUIRE_IDF" = "1" ]; then
        echo "  Set REQUIRE_IDF=0 to proceed without verification." >&2
        return 1 2>/dev/null || exit 1
    fi
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh"
