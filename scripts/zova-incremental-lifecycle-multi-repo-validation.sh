#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPOS=("$ROOT" "$ROOT/../motive" "$ROOT/../rvault" "$ROOT/../tops")
GATE_ROOT=${CBM_ZOVA_INCREMENTAL_LIFECYCLE_GATE_ROOT:-"$ROOT/build/zova-incremental-lifecycle/gate"}
GATE_RUNS=${CBM_ZOVA_INCREMENTAL_LIFECYCLE_GATE_RUNS:-3}
BINARY=${CBM_ZOVA_REAL_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_REAL_TEST_RUNNER:-"$ROOT/build/c/test-runner"}

case "$GATE_RUNS" in
  ''|*[!0-9]*) echo "error: CBM_ZOVA_INCREMENTAL_LIFECYCLE_GATE_RUNS must be a positive integer" >&2; exit 1 ;;
esac
if (( GATE_RUNS <= 0 )); then
  echo "error: CBM_ZOVA_INCREMENTAL_LIFECYCLE_GATE_RUNS must be a positive integer" >&2
  exit 1
fi

if [[ "${CBM_ZOVA_VALIDATION_SKIP_BUILD:-0}" != "1" ]]; then
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
fi

mkdir -p "$GATE_ROOT"
for ((attempt = 1; attempt <= GATE_RUNS; attempt++)); do
  attempt_root="$GATE_ROOT/run-$attempt"
  mkdir -p "$attempt_root"
  echo "=== ZOVA INCREMENTAL LIFECYCLE GATE: run $attempt/$GATE_RUNS ==="
  for repo in "${REPOS[@]}"; do
    if [[ ! -d "$repo/.git" ]]; then
      echo "error: approved validation repository is missing: $repo" >&2
      exit 1
    fi
    name=$(basename "$repo")
    echo "VALIDATE: run=$attempt repo=$name"
    CBM_ZOVA_VALIDATION_REPO="$repo" \
    CBM_ZOVA_INCREMENTAL_LIFECYCLE_RUN_DIR="$attempt_root/$name" \
    CBM_ZOVA_VALIDATION_SKIP_BUILD=1 \
    CBM_ZOVA_REAL_BINARY="$BINARY" \
    CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
      bash "$ROOT/scripts/zova-incremental-lifecycle-validation.sh"
  done
done

echo "LIFECYCLE GATE PASS: $GATE_RUNS fresh four-repository runs"
