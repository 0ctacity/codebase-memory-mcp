#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-"$(cd "$ROOT/../tops" 2>/dev/null && pwd -P || true)"}
NAME=${CBM_ZOVA_SECTION9_REPOSITORY:-tops}
CALIBRATION_ROOT=${CBM_ZOVA_SECTION9_CALIBRATION_ROOT:-"$ROOT/build/zova-section9-promotion/calibration"}
BUILD=${CBM_ZOVA_SECTION9_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
REPOSITORY_RUNNER=${CBM_ZOVA_SECTION9_REPOSITORY_RUNNER:-"$ROOT/scripts/zova-section9-promotion-validation.sh"}
GATE=${CBM_ZOVA_SECTION9_GATE:-"$ROOT/scripts/zova-full-authority-promotion-gate.py"}

fail() { echo "error: $*" >&2; exit 1; }
[[ "$NAME" == tops ]] || fail "calibration is restricted to tops"
git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
  fail "TOPS repository is missing or is not a Git repository: $REPO"
[[ -x "$BUILD" ]] || fail "missing built binary: $BUILD"
[[ -x "$REPOSITORY_RUNNER" ]] || fail "missing repository runner: $REPOSITORY_RUNNER"
[[ -f "$GATE" && ( -x "$GATE" || "$GATE" == *.py ) ]] || fail "missing gate: $GATE"

mkdir -p "$CALIBRATION_ROOT"
CALIBRATION_ROOT=$(cd "$CALIBRATION_ROOT" && pwd -P)
OUTPUT="$CALIBRATION_ROOT/calibration.json"
LOCK="$CALIBRATION_ROOT/.calibration.lock"
[[ ! -e "$OUTPUT" ]] || fail "calibration is immutable and already exists: $OUTPUT"
for sample in 1 2 3 4 5; do
  [[ ! -e "$CALIBRATION_ROOT/candidate-$sample" ]] ||
    fail "calibration candidate already exists: $CALIBRATION_ROOT/candidate-$sample"
done
if ! mkdir "$LOCK" 2>/dev/null; then fail "calibration is already running: $LOCK"; fi
printf 'pid=%s\n' "$$" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

REPORT_ARGS=()
for sample in 1 2 3 4 5; do
  CANDIDATE_ROOT="$CALIBRATION_ROOT/candidate-$sample"
  CANDIDATE_REPORT="$CANDIDATE_ROOT/repository-report.json"
  echo "SECTION 9 calibration phase=tops-candidate sample=$sample/5" >&2
  CBM_ZOVA_VALIDATION_REPO="$REPO" CBM_ZOVA_SECTION9_REPOSITORY=tops \
  CBM_ZOVA_SECTION9_ATTEMPT=0 CBM_ZOVA_SECTION9_RUN_ROOT="$CANDIDATE_ROOT" \
  CBM_ZOVA_SECTION9_CALIBRATION_MODE=1 CBM_ZOVA_SECTION9_CALIBRATION_SAMPLE="$sample" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$BUILD" \
    "$REPOSITORY_RUNNER" || fail "TOPS calibration candidate $sample failed"
  [[ -s "$CANDIDATE_REPORT" ]] ||
    fail "TOPS calibration candidate $sample report is missing"
  REPORT_ARGS+=(--report "tops=$CANDIDATE_REPORT")
done

BUILD_SHA=$(python3 - "$BUILD" <<'PY'
import hashlib, pathlib, sys
print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())
PY
)
echo "SECTION 9 calibration phase=freeze-limits" >&2
if [[ "$GATE" == *.py ]]; then GATE_COMMAND=(python3 "$GATE"); else GATE_COMMAND=("$GATE"); fi
"${GATE_COMMAND[@]}" --calibrate --build-sha256 "$BUILD_SHA" --output "$OUTPUT" \
  "${REPORT_ARGS[@]}" ||
  fail "TOPS calibration was rejected; candidate diagnostics retained in $CANDIDATE_ROOT"
echo "SECTION 9 calibration phase=complete" >&2
