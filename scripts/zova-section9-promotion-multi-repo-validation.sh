#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
MULTI_ROOT=${CBM_ZOVA_SECTION9_MULTI_ROOT:-"$ROOT/build/zova-section9-promotion"}
ATTEMPTS=${CBM_ZOVA_SECTION9_ATTEMPTS:-3}
TOPS=${CBM_ZOVA_SECTION9_TOPS_REPO:-"$ROOT/../tops"}
MOTIVE=${CBM_ZOVA_SECTION9_MOTIVE_REPO:-"$ROOT/../motive"}
RVAULT=${CBM_ZOVA_SECTION9_RVAULT_REPO:-"$ROOT/../rvault"}
CBM=${CBM_ZOVA_SECTION9_CBM_REPO:-"$ROOT"}
BUILD=${CBM_ZOVA_SECTION9_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_SECTION9_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
BUILD_ONCE=${CBM_ZOVA_SECTION9_BUILD_ONCE:-"$ROOT/scripts/zova-build-once.sh"}
PREFLIGHT=${CBM_ZOVA_SECTION9_PREFLIGHT_COMMAND:-}
CALIBRATOR=${CBM_ZOVA_SECTION9_CALIBRATOR:-"$ROOT/scripts/zova-section9-calibrate.sh"}
FOCUSED_RUNNER=${CBM_ZOVA_SECTION9_FOCUSED_RUNNER:-"$ROOT/scripts/zova-section9-focused-validation.sh"}
REPOSITORY_RUNNER=${CBM_ZOVA_SECTION9_REPOSITORY_RUNNER:-"$ROOT/scripts/zova-section9-promotion-validation.sh"}
GATE=${CBM_ZOVA_SECTION9_GATE:-"$ROOT/scripts/zova-full-authority-promotion-gate.py"}
export CBM_ZOVA_MIN_FREE_GB=${CBM_ZOVA_MIN_FREE_GB:-8}

fail() { echo "error: $*" >&2; exit 1; }
[[ "$ATTEMPTS" == 3 ]] || fail "Section 9 requires exactly three attempts"
for item in "$TOPS" "$MOTIVE" "$RVAULT" "$CBM"; do
  git -C "$item" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "missing Git repository: $item"
done
for tool in "$BUILD_ONCE" "$CALIBRATOR" "$FOCUSED_RUNNER" "$REPOSITORY_RUNNER"; do
  [[ -x "$tool" ]] || fail "missing executable: $tool"
done
[[ -f "$GATE" && ( -x "$GATE" || "$GATE" == *.py ) ]] || fail "missing gate: $GATE"

mkdir -p "$MULTI_ROOT"
MULTI_ROOT=$(cd "$MULTI_ROOT" && pwd -P)
LOCK="$MULTI_ROOT/.section9-multi.lock"
[[ ! -e "$MULTI_ROOT/calibration" && ! -e "$MULTI_ROOT/attempt-1" &&
   ! -e "$MULTI_ROOT/attempt-2" && ! -e "$MULTI_ROOT/attempt-3" ]] ||
  fail "multi-repository run root is not fresh: $MULTI_ROOT"
if ! mkdir "$LOCK" 2>/dev/null; then fail "multi-repository validation is already running: $LOCK"; fi
printf 'pid=%s\n' "$$" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

echo "SECTION 9 phase=build" >&2
if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" == 1 ]]; then
  [[ -x "$BUILD" && -x "$TEST_RUNNER" ]] ||
    fail "build skip requires both built executables"
else
  "$BUILD_ONCE"
fi
[[ -x "$BUILD" && -x "$TEST_RUNNER" ]] || fail "build did not produce required executables"

echo "SECTION 9 phase=preflight" >&2
if [[ -n "$PREFLIGHT" ]]; then
  [[ -x "$PREFLIGHT" ]] || fail "missing preflight command: $PREFLIGHT"
  "$PREFLIGHT"
else
  python3 "$ROOT/tests/test_zova_full_authority_promotion_gate.py"
  bash "$ROOT/tests/test_zova_section9_focused_validation.sh"
  bash "$ROOT/tests/test_zova_section9_promotion_validation.sh"
  bash "$ROOT/tests/test_zova_section9_calibrate.sh"
  git -C "$ROOT" diff --check
fi

CALIBRATION_ROOT="$MULTI_ROOT/calibration"
echo "SECTION 9 phase=calibration" >&2
CBM_ZOVA_VALIDATION_REPO="$TOPS" \
CBM_ZOVA_SECTION9_CALIBRATION_ROOT="$CALIBRATION_ROOT" \
CBM_ZOVA_SECTION9_BUILD_BINARY="$BUILD" \
  "$CALIBRATOR"
CALIBRATION="$CALIBRATION_ROOT/calibration.json"
[[ -s "$CALIBRATION" ]] || fail "calibration output is missing"

if [[ "$GATE" == *.py ]]; then GATE_COMMAND=(python3 "$GATE"); else GATE_COMMAND=("$GATE"); fi
ATTEMPT_GATES=()
for attempt in 1 2 3; do
  ATTEMPT_ROOT="$MULTI_ROOT/attempt-$attempt"
  FOCUSED_ROOT="$ATTEMPT_ROOT/focused"
  mkdir -p "$ATTEMPT_ROOT"
  echo "SECTION 9 attempt=$attempt phase=focused" >&2
  "$FOCUSED_RUNNER" "$attempt" "$FOCUSED_ROOT"
  FOCUSED="$FOCUSED_ROOT/focused-evidence.json"
  [[ -s "$FOCUSED" ]] || fail "focused evidence is missing for attempt $attempt"

  REPORT_ARGUMENTS=()
  for name in tops motive rvault CBM; do
    case "$name" in
      tops) repo=$TOPS;; motive) repo=$MOTIVE;; rvault) repo=$RVAULT;; CBM) repo=$CBM;;
    esac
    REPO_ROOT="$ATTEMPT_ROOT/$name"
    echo "SECTION 9 attempt=$attempt repo=$name phase=validation" >&2
    CBM_ZOVA_VALIDATION_REPO="$repo" CBM_ZOVA_SECTION9_REPOSITORY="$name" \
    CBM_ZOVA_SECTION9_ATTEMPT="$attempt" CBM_ZOVA_SECTION9_RUN_ROOT="$REPO_ROOT" \
    CBM_ZOVA_SECTION9_CALIBRATION="$CALIBRATION" \
    CBM_ZOVA_SECTION9_FOCUSED_EVIDENCE="$FOCUSED" \
    CBM_ZOVA_SECTION9_BUILD_BINARY="$BUILD" \
    CBM_ZOVA_SECTION9_TEST_RUNNER="$TEST_RUNNER" \
      "$REPOSITORY_RUNNER"
    report="$REPO_ROOT/repository-report.json"
    decision="$REPO_ROOT/repository-gate.json"
    [[ -s "$report" && -s "$decision" ]] || fail "$name attempt $attempt output missing"
    python3 - "$decision" <<'PY'
import json, sys
if json.load(open(sys.argv[1])).get("passed") is not True: raise SystemExit(1)
PY
    REPORT_ARGUMENTS+=(--report "$name=$report")
  done

  ATTEMPT_GATE="$ATTEMPT_ROOT/attempt-gate.json"
  echo "SECTION 9 attempt=$attempt phase=attempt-gate" >&2
  CBM_ZOVA_SECTION9_ATTEMPT="$attempt" "${GATE_COMMAND[@]}" \
    --calibration "$CALIBRATION" --focused-evidence "$FOCUSED" --attempt "$attempt" \
    --output "$ATTEMPT_GATE" "${REPORT_ARGUMENTS[@]}"
  ATTEMPT_GATES+=("$ATTEMPT_GATE")
done

AGGREGATE="$MULTI_ROOT/aggregate-gate.json"
echo "SECTION 9 phase=aggregate-gate" >&2
"${GATE_COMMAND[@]}" --aggregate --output "$AGGREGATE" \
  --attempt-report "1=${ATTEMPT_GATES[0]}" \
  --attempt-report "2=${ATTEMPT_GATES[1]}" \
  --attempt-report "3=${ATTEMPT_GATES[2]}"
python3 - "$AGGREGATE" <<'PY'
import json, sys
if json.load(open(sys.argv[1])).get("passed") is not True: raise SystemExit(1)
PY
LATEST="$MULTI_ROOT/section9-latest-gate.json"
cp "$AGGREGATE" "$LATEST.tmp"
mv "$LATEST.tmp" "$LATEST"
echo "SECTION 9 phase=complete" >&2
