#!/usr/bin/env bash
set -euo pipefail

# Build CBM against an immutable workspace-local snapshot of the current Zova
# C SDK. This script never builds or fingerprints the live Zova checkout.

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
ZOVA_ROOT=${CBM_ZOVA_PIN_ROOT:-"$ROOT/.zova-sdk"}
CACHE_ROOT=${CBM_ZOVA_BUILD_CACHE:-"$ROOT/build/.zova-build-cache"}
LOCK_DIR=${CBM_ZOVA_BUILD_LOCK_DIR:-"$CACHE_ROOT/.build.lock"}
ZOVA_LIB="$ZOVA_ROOT/zig-out/lib/libzova_c.a"
ZOVA_HEADER="$ZOVA_ROOT/include/zova.h"
ZOVA_ROOT_ZIG="$ZOVA_ROOT/src/root.zig"
RUNNER=${CBM_ZOVA_REAL_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
BINARY=${CBM_ZOVA_REAL_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}

if [[ ! -d "$ZOVA_ROOT" ]]; then
  echo "error: pinned Zova SDK is missing: $ZOVA_ROOT" >&2
  echo "Build/adopt Zova explicitly, then run scripts/zova-pin-current.sh." >&2
  exit 1
fi
ZOVA_ROOT=$(cd "$ZOVA_ROOT" && pwd -P)
for path in "$ZOVA_HEADER" "$ZOVA_LIB" "$ZOVA_ROOT_ZIG"; do
  [[ -f "$path" ]] || { echo "error: pinned Zova SDK is incomplete: $path" >&2; exit 1; }
done

mkdir -p "$CACHE_ROOT" "$ROOT/build/c"

if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" == "1" ]]; then
  [[ -x "$RUNNER" ]] || { echo "error: test runner is missing: $RUNNER" >&2; exit 1; }
  [[ -x "$BINARY" ]] || { echo "error: CBM binary is missing: $BINARY" >&2; exit 1; }
  printf '%s\n' "$RUNNER"
  exit 0
fi

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  owner=""
  [[ -f "$LOCK_DIR/owner" ]] && owner=$(cat "$LOCK_DIR/owner" 2>/dev/null || true)
  echo "error: another Zova/CBM build is already running${owner:+ (pid $owner)}" >&2
  exit 1
fi
printf '%s\n' "$$" > "$LOCK_DIR/owner"
cleanup() { rm -f "$LOCK_DIR/owner"; rmdir "$LOCK_DIR" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
echo "REUSE PINNED ZOVA: $ZOVA_ROOT" >&2

ZIG_GLOBAL_CACHE="$CACHE_ROOT/zig-global-cache"
CBM_ZIG_CACHE="$CACHE_ROOT/cbm-zig-cache"
mkdir -p "$ZIG_GLOBAL_CACHE"
# Interrupted Zig processes can leave zero-byte cache manifests that cause
# permanent manifest_create PermissionDenied failures. Remove only those empty
# files; all valid cached objects remain reusable.
mkdir -p "$CBM_ZIG_CACHE"
"$ROOT/scripts/zova-cache-repair.sh" "$CBM_ZIG_CACHE" "$ZIG_GLOBAL_CACHE"
# Zig 0.16 may execute duplicate intermediate libraries concurrently when both
# top-level targets are requested together. Build them sequentially under this
# script's lock while reusing the exact same cache.
for target in cbm test-runner; do
  zig build --cache-dir "$CBM_ZIG_CACHE" \
    --global-cache-dir "$ZIG_GLOBAL_CACHE" \
    "$target" -Dwith-zova=true -Dzova-root="$ZOVA_ROOT"
done

[[ -x "$RUNNER" ]] || { echo "error: build did not produce $RUNNER" >&2; exit 1; }
[[ -x "$BINARY" ]] || { echo "error: build did not produce $BINARY" >&2; exit 1; }
printf '%s\n' "$RUNNER"
