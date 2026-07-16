#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-}
NAME=${CBM_ZOVA_SECTION9_REPOSITORY:-}
ATTEMPT=${CBM_ZOVA_SECTION9_ATTEMPT:-}
RUN_ROOT=${CBM_ZOVA_SECTION9_RUN_ROOT:-}
CALIBRATION=${CBM_ZOVA_SECTION9_CALIBRATION:-}
FOCUSED=${CBM_ZOVA_SECTION9_FOCUSED_EVIDENCE:-}
CALIBRATION_MODE=${CBM_ZOVA_SECTION9_CALIBRATION_MODE:-0}
BUILD=${CBM_ZOVA_SECTION9_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_SECTION9_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
RUN_TESTS=${CBM_ZOVA_SECTION9_RUN_TESTS:-"$ROOT/scripts/zova-run-tests.sh"}
GATE=${CBM_ZOVA_SECTION9_GATE:-"$ROOT/scripts/zova-full-authority-promotion-gate.py"}
DISK_GUARD=${CBM_ZOVA_SECTION9_DISK_GUARD:-"$ROOT/scripts/zova-disk-guard.sh"}

fail() { echo "error: $*" >&2; exit 1; }
[[ -n "$REPO" && -n "$NAME" && -n "$ATTEMPT" && -n "$RUN_ROOT" ]] ||
  fail "repository path, name, attempt, and run root are required"
git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
  fail "repository is missing .git: $REPO"
REPO=$(cd "$REPO" && pwd -P)
case "$NAME" in tops|motive|rvault|CBM) ;; *) fail "invalid repository name: $NAME";; esac
[[ "$CALIBRATION_MODE" == 0 || "$CALIBRATION_MODE" == 1 ]] ||
  fail "calibration mode must be 0 or 1"
if [[ "$CALIBRATION_MODE" == 1 ]]; then
  [[ "$NAME" == tops && "$ATTEMPT" == 0 ]] ||
    fail "calibration mode requires repository tops and attempt 0"
else
  [[ "$ATTEMPT" =~ ^[1-3]$ ]] || fail "official attempt must be 1, 2, or 3"
  [[ -f "$CALIBRATION" ]] || fail "missing calibration: $CALIBRATION"
  [[ -f "$FOCUSED" ]] || fail "missing focused evidence: $FOCUSED"
fi
[[ -x "$BUILD" ]] || fail "missing built binary: $BUILD"
[[ -x "$TEST_RUNNER" ]] || fail "missing test runner: $TEST_RUNNER"
[[ -x "$RUN_TESTS" ]] || fail "missing test wrapper: $RUN_TESTS"
[[ -x "$DISK_GUARD" ]] || fail "missing disk guard: $DISK_GUARD"
[[ -f "$GATE" && ( -x "$GATE" || "$GATE" == *.py ) ]] || fail "missing gate: $GATE"

mkdir -p "$RUN_ROOT"
RUN_ROOT=$(cd "$RUN_ROOT" && pwd -P)
LOCK="$RUN_ROOT/.section9.lock"
CLONE="$RUN_ROOT/clone"
STATE_CACHE="$RUN_ROOT/state-cache"
FULL_REPORT="$RUN_ROOT/full-state.json"
INCREMENTAL_REPORT="$RUN_ROOT/incremental-state.json"
REPORT="$RUN_ROOT/repository-report.json"
DECISION="$RUN_ROOT/repository-gate.json"
RUN_ID="${NAME}-a${ATTEMPT}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
[[ ! -e "$REPORT" && ! -e "$CLONE" && ! -e "$STATE_CACHE" ]] ||
  fail "run root is not fresh: $RUN_ROOT"
if ! mkdir "$LOCK" 2>/dev/null; then fail "repository validation is already running: $LOCK"; fi
printf 'pid=%s\nrepository=%s\nattempt=%s\n' "$$" "$NAME" "$ATTEMPT" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

"$DISK_GUARD" "$RUN_ROOT"
echo "SECTION 9 repo=$NAME phase=clone" >&2
git clone -q --shared --no-hardlinks "$REPO" "$CLONE"
SOURCE_COMMIT=$(git -C "$CLONE" rev-parse HEAD)
mkdir -p "$CLONE/section9_promotion_probe" "$STATE_CACHE"
cat >"$CLONE/section9_promotion_probe/probe.go" <<'GO'
package section9_promotion_probe

func PromotionProbe() int { return 7 }
GO

read_inputs() {
  python3 - "$BUILD" "$CALIBRATION_MODE" "$CALIBRATION" "$FOCUSED" <<'PY'
import hashlib, json, pathlib, sys
build, mode, calibration, focused = sys.argv[1:]
digest = hashlib.sha256(pathlib.Path(build).read_bytes()).hexdigest()
if mode == "1":
    print(digest); print(""); print("")
else:
    c = json.loads(pathlib.Path(calibration).read_text())
    f = json.loads(pathlib.Path(focused).read_text())
    if c.get("build_sha256") != digest:
        raise SystemExit("calibration build hash does not match binary")
    print(digest); print(c.get("digest", "")); print(f.get("digest", ""))
PY
}
INPUTS=$(read_inputs) || fail "invalid calibration or focused evidence"
BUILD_SHA=$(printf '%s\n' "$INPUTS" | sed -n '1p')
CALIBRATION_SHA=$(printf '%s\n' "$INPUTS" | sed -n '2p')
FOCUSED_SHA=$(printf '%s\n' "$INPUTS" | sed -n '3p')

run_state() {
  local state=$1 report=$2 log="$RUN_ROOT/$1-state.log"
  echo "SECTION 9 repo=$NAME phase=$state" >&2
  CBM_ZOVA_TEST_CACHE_DIR="$STATE_CACHE" CBM_ZOVA_BUILD_SKIP=1 \
  CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
  CBM_ZOVA_VALIDATION_REPO="$CLONE" CBM_ZOVA_PROMOTION_STATE="$state" \
  CBM_ZOVA_PROMOTION_REPORT="$report" CBM_ZOVA_PROMOTION_REPOSITORY="$NAME" \
  CBM_ZOVA_PROMOTION_RUN_ID="$RUN_ID" CBM_ZOVA_PROMOTION_ATTEMPT="$ATTEMPT" \
  CBM_ZOVA_PROMOTION_BUILD_SHA256="$BUILD_SHA" \
  CBM_ZOVA_PROMOTION_CALIBRATION_SHA256="$CALIBRATION_SHA" \
  CBM_ZOVA_PROMOTION_FOCUSED_SHA256="$FOCUSED_SHA" \
  CBM_ZOVA_SECTION9_CALIBRATION_MODE="$CALIBRATION_MODE" \
  "$RUN_TESTS" zova_single_file_promotion_real_repo >"$log" 2>&1 || {
    tail -80 "$log" >&2 || true
    fail "$state promotion state failed; diagnostics retained in $RUN_ROOT"
  }
  [[ -s "$report" ]] || fail "$state state report is missing"
}

run_state full "$FULL_REPORT"
python3 - "$CLONE/section9_promotion_probe/probe.go" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
if text.count("return 7") != 1:
    raise SystemExit("promotion probe does not contain exactly one return 7")
path.write_text(text.replace("return 7", "return 99"))
PY
run_state incremental "$INCREMENTAL_REPORT"

python3 - "$FULL_REPORT" "$INCREMENTAL_REPORT" "$REPORT" "$NAME" "$ATTEMPT" \
  "$RUN_ID" "$SOURCE_COMMIT" "$BUILD_SHA" "$CALIBRATION_SHA" "$FOCUSED_SHA" \
  "$CALIBRATION_MODE" <<'PY'
import json, os, pathlib, sys
full_path, incremental_path, output = map(pathlib.Path, sys.argv[1:4])
name, attempt, run_id, commit, build, calibration, focused, mode = sys.argv[4:]
full = json.loads(full_path.read_text()); incremental = json.loads(incremental_path.read_text())
if full.get("name") != "full" or incremental.get("name") != "incremental":
    raise SystemExit("state report order/name mismatch")
report = {
    "schema_version": 1,
    "repository": name,
    "source_commit": commit,
    "run_id": run_id,
    "attempt": int(attempt),
    "calibration_mode": mode == "1",
    "build_sha256": build,
    "calibration_sha256": calibration or None,
    "focused_evidence_sha256": focused or None,
    "flagged_full_authority": True,
    "passed": full.get("passed") is True and incremental.get("passed") is True,
    "states": [full, incremental],
}
temporary = output.with_suffix(output.suffix + ".tmp")
temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
os.replace(temporary, output)
PY

if [[ "$CALIBRATION_MODE" == 0 ]]; then
  echo "SECTION 9 repo=$NAME phase=repository-gate" >&2
  if [[ "$GATE" == *.py ]]; then GATE_COMMAND=(python3 "$GATE"); else GATE_COMMAND=("$GATE"); fi
  "${GATE_COMMAND[@]}" --repository-only --calibration "$CALIBRATION" \
    --focused-evidence "$FOCUSED" --attempt "$ATTEMPT" --output "$DECISION" \
    --report "$NAME=$REPORT" || fail "repository gate failed; diagnostics retained in $RUN_ROOT"
else
  python3 - "$REPORT" "$DECISION" <<'PY'
import json, pathlib, sys
report = json.loads(pathlib.Path(sys.argv[1]).read_text())
pathlib.Path(sys.argv[2]).write_text(json.dumps({"passed": report.get("passed") is True}) + "\n")
if report.get("passed") is not True:
    raise SystemExit(1)
PY
fi

rm -rf "$CLONE" "$STATE_CACHE"
rm -f "$FULL_REPORT" "$INCREMENTAL_REPORT" \
  "$RUN_ROOT/full-state.log" "$RUN_ROOT/incremental-state.log"
