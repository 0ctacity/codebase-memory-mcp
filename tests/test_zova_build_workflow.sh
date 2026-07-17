#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-build-workflow.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

LOG="$TMP/runner.log"
ENV_LOG="$TMP/runner-env.log"
RUNNER="$TMP/test-runner"
cat > "$RUNNER" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "${CBM_ZOVA_TEST_RUNNER_LOG:?}"
printf 'HOME=%s\nCBM_CACHE_DIR=%s\n' "$HOME" "${CBM_CACHE_DIR-}" >> "${CBM_ZOVA_TEST_ENV_LOG:?}"
EOF
chmod +x "$RUNNER"

CBM_ZOVA_BUILD_SKIP=1 \
CBM_ZOVA_REAL_TEST_RUNNER="$RUNNER" \
CBM_ZOVA_TEST_RUNNER_LOG="$LOG" \
CBM_ZOVA_TEST_ENV_LOG="$ENV_LOG" \
  bash "$ROOT/scripts/zova-run-tests.sh" zova mcp

[[ "$(wc -l < "$LOG" | tr -d ' ')" == "2" ]]
grep -qx 'zova' "$LOG"
grep -qx 'mcp' "$LOG"
test_home_first=$(sed -n 's/^HOME=//p' "$ENV_LOG" | head -n 1)
test_home_last=$(sed -n 's/^HOME=//p' "$ENV_LOG" | tail -n 1)
test_cache_first=$(sed -n 's/^CBM_CACHE_DIR=//p' "$ENV_LOG" | head -n 1)
test_cache_last=$(sed -n 's/^CBM_CACHE_DIR=//p' "$ENV_LOG" | tail -n 1)
[[ "$(grep -c '^HOME=' "$ENV_LOG")" -eq 2 ]]
[[ "$(grep -c '^CBM_CACHE_DIR=' "$ENV_LOG")" -eq 2 ]]
[[ "$test_home_first" == "$test_home_last" ]]
[[ "$test_cache_first" == "$test_cache_last" ]]
[[ "$test_home_first" == "$test_cache_first"/home ]]
[[ "$test_cache_first" == */cache ]]
if [[ -e "$test_home_first" || -e "$test_cache_first" ]]; then
  echo "error: isolated test environment was not cleaned" >&2
  exit 1
fi
echo "PASS: zova test workflow builds once and runs requested suites directly"

grep -q 'Zova C ABI, v0.24.0 pre-1.0' "$ROOT/build.zig"
grep -q 'zova_graph_edge_delete_many' "$ROOT/build.zig"
grep -q 'zova_vector_delete_many' "$ROOT/build.zig"
echo "PASS: CBM build guard requires the format-8 Zova ABI surface"

FAKE_ZOVA="$TMP/zova-source"
PINNED_ZOVA="$TMP/zova-pinned"
mkdir -p "$FAKE_ZOVA/include" "$FAKE_ZOVA/zig-out/lib" "$FAKE_ZOVA/src"
cat > "$FAKE_ZOVA/include/zova.h" <<'EOF'
typedef int zova_status;
typedef struct zova_graph_edge_delete_many_request zova_graph_edge_delete_many_request;
typedef struct zova_vector_delete_many_request zova_vector_delete_many_request;
zova_status zova_graph_edge_delete_many(const zova_graph_edge_delete_many_request *request);
zova_status zova_vector_delete_many(const zova_vector_delete_many_request *request);
EOF
printf 'archive-v1\n' > "$FAKE_ZOVA/zig-out/lib/libzova_c.a"
printf 'root-v1\n' > "$FAKE_ZOVA/src/root.zig"
printf 'dependency-v1\n' > "$FAKE_ZOVA/src/zova.zig"
cat > "$FAKE_ZOVA/src/version.zig" <<'EOF'
pub const package_version = "0.24.0";
pub const abi_version_string = "0.24.0";
pub const format_version = "8";
EOF
git -C "$FAKE_ZOVA" init -q
git -C "$FAKE_ZOVA" config user.name "CBM Test"
git -C "$FAKE_ZOVA" config user.email "cbm-test@example.invalid"
git -C "$FAKE_ZOVA" add include/zova.h src/root.zig src/zova.zig src/version.zig
git -C "$FAKE_ZOVA" commit -q -m "fake format-8 sdk"
FAKE_COMMIT=$(git -C "$FAKE_ZOVA" rev-parse HEAD)

CBM_ZOVA_PIN_ROOT="$PINNED_ZOVA" \
ZOVA_SOURCE_ROOT="$FAKE_ZOVA" \
  bash "$ROOT/scripts/zova-pin-current.sh"

grep -q 'zova_graph_edge_delete_many' "$PINNED_ZOVA/include/zova.h"
grep -q 'zova_vector_delete_many' "$PINNED_ZOVA/include/zova.h"
grep -qx 'archive-v1' "$PINNED_ZOVA/zig-out/lib/libzova_c.a"
grep -qx 'root-v1' "$PINNED_ZOVA/src/root.zig"
grep -qx 'dependency-v1' "$PINNED_ZOVA/src/zova.zig"
grep -qx "source_commit=$FAKE_COMMIT" "$PINNED_ZOVA/manifest.txt"
grep -qx 'format_version=8' "$PINNED_ZOVA/manifest.txt"
grep -qx 'abi_version=0.24.0' "$PINNED_ZOVA/manifest.txt"
grep -qx 'graph_edge_delete_many_symbol=zova_graph_edge_delete_many' "$PINNED_ZOVA/manifest.txt"
grep -qx 'vector_delete_many_symbol=zova_vector_delete_many' "$PINNED_ZOVA/manifest.txt"
expected_header_hash=$(shasum -a 256 "$FAKE_ZOVA/include/zova.h" | awk '{print $1}')
expected_library_hash=$(shasum -a 256 "$FAKE_ZOVA/zig-out/lib/libzova_c.a" | awk '{print $1}')
grep -qx "header_sha256=$expected_header_hash" "$PINNED_ZOVA/manifest.txt"
grep -qx "library_sha256=$expected_library_hash" "$PINNED_ZOVA/manifest.txt"
grep -q '^source_tree_sha256=' "$PINNED_ZOVA/manifest.txt"

# A later source change must not affect the immutable pin.
printf 'archive-v2\n' > "$FAKE_ZOVA/zig-out/lib/libzova_c.a"
if CBM_ZOVA_PIN_ROOT="$PINNED_ZOVA" ZOVA_SOURCE_ROOT="$FAKE_ZOVA" \
    bash "$ROOT/scripts/zova-pin-current.sh" 2> "$TMP/pin-existing.err"; then
  echo "error: pin command unexpectedly replaced an existing snapshot" >&2
  exit 1
fi
grep -q 'already exists' "$TMP/pin-existing.err"
grep -qx 'archive-v1' "$PINNED_ZOVA/zig-out/lib/libzova_c.a"

CBM_ZOVA_PIN_ROOT="$PINNED_ZOVA" \
ZOVA_SOURCE_ROOT="$FAKE_ZOVA" \
  bash "$ROOT/scripts/zova-pin-current.sh" --refresh
grep -qx 'archive-v2' "$PINNED_ZOVA/zig-out/lib/libzova_c.a"
PINNED_ZOVA=$(cd "$PINNED_ZOVA" && pwd -P)
echo "PASS: current Zova SDK is immutable until explicitly refreshed"

FAKE_BIN="$TMP/bin"
FAKE_BUILD_LOG="$TMP/build.log"
mkdir -p "$FAKE_BIN"
cat > "$FAKE_BIN/zig" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'PWD=%s %s\n' "$PWD" "$*" >> "${CBM_ZOVA_FAKE_BUILD_LOG:?}"
mkdir -p "${CBM_ZOVA_FAKE_OUTPUT_DIR:?}"
touch "${CBM_ZOVA_FAKE_OUTPUT_DIR}/codebase-memory-mcp"
touch "${CBM_ZOVA_FAKE_OUTPUT_DIR}/test-runner"
chmod +x "${CBM_ZOVA_FAKE_OUTPUT_DIR}/codebase-memory-mcp" "${CBM_ZOVA_FAKE_OUTPUT_DIR}/test-runner"
EOF
chmod +x "$FAKE_BIN/zig"

PATH="$FAKE_BIN:$PATH" \
CBM_ZOVA_PIN_ROOT="$PINNED_ZOVA" \
CBM_ZOVA_BUILD_CACHE="$TMP/build-cache" \
CBM_ZOVA_BUILD_LOCK_DIR="$TMP/build-cache/.lock" \
CBM_ZOVA_REAL_BINARY="$TMP/output/codebase-memory-mcp" \
CBM_ZOVA_REAL_TEST_RUNNER="$TMP/output/test-runner" \
CBM_ZOVA_FAKE_BUILD_LOG="$FAKE_BUILD_LOG" \
CBM_ZOVA_FAKE_OUTPUT_DIR="$TMP/output" \
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null

grep -F -- "-Dzova-root=$PINNED_ZOVA" "$FAKE_BUILD_LOG"
[[ "$(wc -l < "$FAKE_BUILD_LOG" | tr -d ' ')" == "2" ]]
grep -q ' cbm -Dwith-zova=true ' "$FAKE_BUILD_LOG"
grep -q ' test-runner -Dwith-zova=true ' "$FAKE_BUILD_LOG"
if grep -q "PWD=$PINNED_ZOVA" "$FAKE_BUILD_LOG"; then
  echo "error: CBM build attempted to rebuild Zova" >&2
  exit 1
fi
echo "PASS: CBM builds only against the pinned Zova SDK"

mkdir -p "$TMP/local-cache/h" "$TMP/build-cache/zig-global-cache/h"
: > "$TMP/local-cache/h/interrupted.txt"
: > "$TMP/build-cache/zig-global-cache/h/interrupted.txt"
bash "$ROOT/scripts/zova-cache-repair.sh" \
  "$TMP/local-cache" "$TMP/build-cache/zig-global-cache"
if [[ -f "$TMP/local-cache/h/interrupted.txt" ||
      -f "$TMP/build-cache/zig-global-cache/h/interrupted.txt" ]]; then
  echo "error: zero-byte interrupted cache manifests were not repaired" >&2
  exit 1
fi
echo "PASS: interrupted zero-byte cache manifests are repaired without clearing the cache"

if CBM_ZOVA_MIN_FREE_GB=8 CBM_ZOVA_AVAILABLE_KB_OVERRIDE=1024 \
    bash "$ROOT/scripts/zova-disk-guard.sh" "$TMP" 2> "$TMP/disk.err"; then
  echo "error: disk guard accepted insufficient free space" >&2
  exit 1
fi
grep -q 'refusing to start' "$TMP/disk.err"
CBM_ZOVA_MIN_FREE_GB=1 CBM_ZOVA_AVAILABLE_KB_OVERRIDE=2097152 \
  bash "$ROOT/scripts/zova-disk-guard.sh" "$TMP"
echo "PASS: validation refuses to start before exhausting disk space"
