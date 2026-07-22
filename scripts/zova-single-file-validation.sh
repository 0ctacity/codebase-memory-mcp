#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-$ROOT}
RUN_ROOT=${CBM_ZOVA_SINGLE_FILE_RUN_ROOT:-$ROOT/build/zova-single-file}
LOCK_DIR=${CBM_ZOVA_SINGLE_FILE_LOCK_DIR:-$RUN_ROOT/.validation.lock}
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-$$
RUN_DIR="$RUN_ROOT/runs/$RUN_ID"
REPORT="$RUN_DIR/report.json"
GATE="$RUN_DIR/gate.json"

mkdir -p "$RUN_DIR"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "error: single-file validation is already running: $LOCK_DIR" >&2
  exit 1
fi
cleanup() {
  rm -rf "$LOCK_DIR"
}
trap cleanup EXIT INT TERM

bash "$ROOT/scripts/zova-disk-guard.sh" "$RUN_ROOT"
TEST_ENV_OWNED=0
if [[ -z "${CBM_ZOVA_TEST_CACHE_DIR:-}" ]]; then
  export CBM_ZOVA_TEST_CACHE_DIR="$RUN_DIR/test-environment"
  TEST_ENV_OWNED=1
fi
CBM_ZOVA_VALIDATION_REPO="$REPO" \
CBM_ZOVA_SINGLE_FILE_REPORT="$REPORT" \
CBM_ZOVA_BUILD_SKIP=1 \
  "$ROOT/scripts/zova-run-tests.sh" zova_single_file_real_repo >"$RUN_DIR/run.log" 2>&1 || {
    tail -80 "$RUN_DIR/run.log" >&2 || true
    echo "error: single-file validation failed; diagnostics retained in $RUN_DIR" >&2
    exit 1
  }

if [[ "$TEST_ENV_OWNED" -eq 1 ]]; then
  rm -rf "$CBM_ZOVA_TEST_CACHE_DIR"
fi

python3 "$ROOT/scripts/zova-single-file-gate.py" --output "$GATE" \
  --report "$(basename "$REPO")=$REPORT"
rm -f "$RUN_DIR/run.log"
cp "$GATE" "$RUN_ROOT/latest-gate.json"
cp "$REPORT" "$RUN_ROOT/latest-report.json"
echo "PASS: single-file validation report=$REPORT gate=$GATE"
