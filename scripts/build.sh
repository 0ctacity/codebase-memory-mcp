#!/usr/bin/env bash
# build.sh — Clean build of production binary (standard or with UI).
#
# Usage:
#   scripts/build.sh                              # Standard binary
#   scripts/build.sh --with-ui                    # Binary with embedded UI
#   scripts/build.sh --version v0.8.0             # With version stamp
#   scripts/build.sh --arch x86_64                # Force x86_64 build
#   scripts/build.sh CC=gcc-14 CXX=g++-14        # Override compiler
#
# This script is the SINGLE source of truth for building release binaries.
# Used identically in local development and CI workflows.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Pre-parse --arch flag before sourcing env.sh
for arg in "$@"; do
    case "$arg" in
        --arch=*) export CBM_ARCH="${arg#--arch=}" ;;
    esac
done
prev_arg=""
for arg in "$@"; do
    if [[ "${prev_arg:-}" == "--arch" ]]; then
        export CBM_ARCH="$arg"
    fi
    prev_arg="$arg"
done

# shellcheck source=env.sh
source "$ROOT/scripts/env.sh"

# Parse remaining arguments
WITH_UI=false
WITH_ZOVA=false
VERSION=""
EXTRA_MAKE_ARGS=()

prev_arg=""
for arg in "$@"; do
    # Skip --arch and its value (already handled)
    if [[ "${prev_arg:-}" == "--arch" ]]; then
        prev_arg="$arg"
        continue
    fi
    case "$arg" in
        --with-ui)
            WITH_UI=true
            ;;
        CBM_WITH_ZOVA=1)
            WITH_ZOVA=true
            EXTRA_MAKE_ARGS+=("$arg")
            ;;
        --version)
            prev_arg="$arg"
            continue
            ;;
        --arch|--arch=*)
            ;; # already handled
        CC=*|CXX=*)
            export "${arg}"
            EXTRA_MAKE_ARGS+=("$arg")
            ;;
        *)
            # Check if this is the value for --version
            if [[ "${prev_arg:-}" == "--version" ]]; then
                VERSION="$arg"
            else
                EXTRA_MAKE_ARGS+=("$arg")
            fi
            ;;
    esac
    prev_arg="$arg"
done

# Version flag
CFLAGS_EXTRA=""
if [[ -n "$VERSION" ]]; then
    CLEAN_VERSION="${VERSION#v}"
    CFLAGS_EXTRA="-DCBM_VERSION=\"\\\"$CLEAN_VERSION\\\"\""
fi

print_env "build.sh"
echo "  ui=$WITH_UI zova=$WITH_ZOVA version=${VERSION:-dev}"

# Verify compiler supports target arch
verify_compiler "$CC"

# Step 1: Clean C build artifacts only (not node_modules — npm ci handles that)
rm -rf "$ROOT/build/c"
if [[ -n "${CBM_PREBUILT_ZIG_DIR:-}" ]]; then
    for artifact in libcbm_cli_zig.a cbm_zova_bridge.o; do
        [[ -s "$CBM_PREBUILT_ZIG_DIR/$artifact" ]] || {
            echo "error: missing prebuilt Zig artifact: $CBM_PREBUILT_ZIG_DIR/$artifact" >&2
            exit 1
        }
    done
    mkdir -p "$ROOT/build/c"
    cp "$CBM_PREBUILT_ZIG_DIR/libcbm_cli_zig.a" "$ROOT/build/c/"
    cp "$CBM_PREBUILT_ZIG_DIR/cbm_zova_bridge.o" "$ROOT/build/c/"
    export CBM_PREBUILT_ZIG=1
fi

# Step 2: Build (Makefile applies $ARCHFLAGS for the target arch on macOS)
if $WITH_UI; then
    make -j"$NPROC" -f Makefile.cbm cbm-with-ui \
        CFLAGS_EXTRA="$CFLAGS_EXTRA" "${EXTRA_MAKE_ARGS[@]+"${EXTRA_MAKE_ARGS[@]}"}"
elif $WITH_ZOVA; then
    make -j"$NPROC" -f Makefile.cbm build/c/codebase-memory-mcp \
        CFLAGS_EXTRA="$CFLAGS_EXTRA" "${EXTRA_MAKE_ARGS[@]+"${EXTRA_MAKE_ARGS[@]}"}"
else
    make -j"$NPROC" -f Makefile.cbm cbm \
        CFLAGS_EXTRA="$CFLAGS_EXTRA" "${EXTRA_MAKE_ARGS[@]+"${EXTRA_MAKE_ARGS[@]}"}"
fi

echo "=== Build complete: build/c/codebase-memory-mcp ==="
