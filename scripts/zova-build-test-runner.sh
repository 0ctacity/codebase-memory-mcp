#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
GROUP=${1:-full}
case "$GROUP" in
  full) TARGET=test-runner ;;
  zova|mcp|pipeline) TARGET="test-runner-$GROUP" ;;
  *)
    echo "error: unknown test runner group: $GROUP" >&2
    exit 1
    ;;
esac

ZOVA_ROOT=${CBM_ZOVA_PIN_ROOT:-"$ROOT/.zova-sdk"}
CACHE_ROOT=${CBM_ZOVA_BUILD_CACHE:-"$ROOT/build/.zova-build-cache"}
LOCK_DIR=${CBM_ZOVA_BUILD_LOCK_DIR:-"$CACHE_ROOT/.build.lock"}
RUNNER_DIR=${CBM_ZOVA_TEST_RUNNER_DIR:-"$ROOT/build/c"}
RUNNER="$RUNNER_DIR/$TARGET"

for path in \
  "$ZOVA_ROOT/include/zova.h" \
  "$ZOVA_ROOT/zig-out/lib/libzova_c.a" \
  "$ZOVA_ROOT/src/root.zig"; do
  [[ -f "$path" ]] || { echo "error: pinned Zova SDK is incomplete: $path" >&2; exit 1; }
done
ZOVA_ROOT=$(cd "$ZOVA_ROOT" && pwd -P)
mkdir -p "$CACHE_ROOT" "$RUNNER_DIR"

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  owner=""
  [[ -f "$LOCK_DIR/owner" ]] && owner=$(cat "$LOCK_DIR/owner" 2>/dev/null || true)
  echo "error: another Zova/CBM build is already running${owner:+ (pid $owner)}" >&2
  exit 1
fi
printf '%s\n' "$$" > "$LOCK_DIR/owner"
cleanup() { rm -f "$LOCK_DIR/owner"; rmdir "$LOCK_DIR" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

ZIG_GLOBAL_CACHE="$CACHE_ROOT/zig-global-cache"
CBM_ZIG_CACHE="$CACHE_ROOT/cbm-zig-cache"
mkdir -p "$ZIG_GLOBAL_CACHE" "$CBM_ZIG_CACHE"
"$ROOT/scripts/zova-cache-repair.sh" "$CBM_ZIG_CACHE" "$ZIG_GLOBAL_CACHE"

echo "BUILD TEST RUNNER: $GROUP" >&2
zig build --cache-dir "$CBM_ZIG_CACHE" \
  --global-cache-dir "$ZIG_GLOBAL_CACHE" \
  "$TARGET" -Dwith-zova=true -Dzova-root="$ZOVA_ROOT"

[[ -x "$RUNNER" ]] || { echo "error: build did not produce $RUNNER" >&2; exit 1; }
printf '%s\n' "$RUNNER"
