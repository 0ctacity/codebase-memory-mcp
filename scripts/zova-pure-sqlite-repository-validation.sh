#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-}
NAME=${CBM_ZOVA_PURE_REPOSITORY:-}
ATTEMPT=${CBM_ZOVA_PURE_ATTEMPT:-}
RUN_ROOT=${CBM_ZOVA_PURE_RUN_ROOT:-}
BUILD=${CBM_ZOVA_PURE_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_PURE_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
RUN_TESTS=${CBM_ZOVA_PURE_RUN_TESTS:-"$ROOT/scripts/zova-run-tests.sh"}
DISK_GUARD=${CBM_ZOVA_PURE_DISK_GUARD:-"$ROOT/scripts/zova-disk-guard.sh"}

fail() { echo "error: $*" >&2; exit 1; }
[[ -n "$REPO" && -n "$NAME" && -n "$ATTEMPT" && -n "$RUN_ROOT" ]] ||
  fail "repository path, name, attempt, and run root are required"
case "$NAME" in tops|motive|rvault|CBM) ;; *) fail "invalid repository name: $NAME";; esac
[[ "$ATTEMPT" =~ ^[1-3]$ ]] || fail "attempt must be 1, 2, or 3"
git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
  fail "repository is missing .git: $REPO"
for path in "$BUILD" "$TEST_RUNNER" "$RUN_TESTS" "$DISK_GUARD"; do
  [[ -x "$path" ]] || fail "required executable is missing: $path"
done

REPO=$(cd "$REPO" && pwd -P)
[[ ! -e "$RUN_ROOT" ]] || fail "run root must be fresh: $RUN_ROOT"
mkdir -p "$RUN_ROOT"
RUN_ROOT=$(cd "$RUN_ROOT" && pwd -P)
LOCK="$RUN_ROOT/.pure-sqlite.lock"
CLONE="$RUN_ROOT/clone"
STATE_CACHE="$RUN_ROOT/state-cache"
FULL_REPORT="$RUN_ROOT/full-state.json"
CHANGED_REPORT="$RUN_ROOT/changed-state.json"
REPORT="$RUN_ROOT/repository-report.json"
mkdir "$LOCK"
printf 'pid=%s\nrepository=%s\nattempt=%s\n' "$$" "$NAME" "$ATTEMPT" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

"$DISK_GUARD" "$RUN_ROOT"
echo "PURE SQLITE repo=$NAME attempt=$ATTEMPT phase=clone" >&2
git clone -q --shared --no-hardlinks "$REPO" "$CLONE"
SOURCE_COMMIT=$(git -C "$CLONE" rev-parse HEAD)
mkdir -p "$CLONE/section9_promotion_probe" "$STATE_CACHE"
printf 'package section9_promotion_probe\n\nfunc PromotionProbe() int { return 7 }\n' \
  >"$CLONE/section9_promotion_probe/probe.go"

run_state() {
  local state=$1 report=$2 log="$RUN_ROOT/$1-state.log"
  echo "PURE SQLITE repo=$NAME attempt=$ATTEMPT phase=$state" >&2
  CBM_ZOVA_TEST_CACHE_DIR="$STATE_CACHE" CBM_ZOVA_BUILD_SKIP=1 \
  CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
  CBM_ZOVA_VALIDATION_REPO="$CLONE" CBM_ZOVA_PROMOTION_STATE="$state" \
  CBM_ZOVA_PROMOTION_REPORT="$report" CBM_ZOVA_PROMOTION_REPOSITORY="$NAME" \
  CBM_ZOVA_PROMOTION_RUN_ID="pure-$NAME-a$ATTEMPT" \
  CBM_ZOVA_PROMOTION_ATTEMPT="$ATTEMPT" \
  CBM_ZOVA_PROMOTION_BUILD_SHA256="$(shasum -a 256 "$BUILD" | awk '{print $1}')" \
  CBM_ZOVA_SECTION9_CALIBRATION_MODE=0 \
  CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE=1 \
  "$RUN_TESTS" zova_single_file_promotion_real_repo >"$log" 2>&1 || {
    tail -80 "$log" >&2 || true
    fail "$state pure-SQLite state failed; diagnostics retained in $RUN_ROOT"
  }
  [[ -s "$report" ]] || fail "$state report is missing"
}

run_state full "$FULL_REPORT"
python3 - "$CLONE/section9_promotion_probe/probe.go" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
if text.count("return 7") != 1:
    raise SystemExit("probe does not contain exactly one return 7")
path.write_text(text.replace("return 7", "return 99"))
PY
run_state incremental "$CHANGED_REPORT"

python3 - "$FULL_REPORT" "$CHANGED_REPORT" "$REPORT" "$NAME" "$ATTEMPT" \
  "$SOURCE_COMMIT" "$BUILD" <<'PY'
import hashlib, json, os, pathlib, sys
full_path, changed_path, output = map(pathlib.Path, sys.argv[1:4])
name, attempt, commit = sys.argv[4:7]
build = pathlib.Path(sys.argv[7])
full = json.loads(full_path.read_text())
changed = json.loads(changed_path.read_text())
states = [full, changed]
if [s.get("name") for s in states] != ["full", "incremental"]:
    raise SystemExit("state order/name mismatch")
if any(s.get("baseline_route") != "pure_sqlite" for s in states):
    raise SystemExit("state did not use pure SQLite")
if any(s.get("storage", {}).get("compat_zova_bytes") != 0 for s in states):
    raise SystemExit("pure SQLite state produced sidecar bytes")
report = {
    "schema_version": 1,
    "baseline_route": "pure_sqlite",
    "repository": name,
    "attempt": int(attempt),
    "source_commit": commit,
    "build_sha256": hashlib.sha256(build.read_bytes()).hexdigest(),
    "passed": all(s.get("passed") is True for s in states),
    "states": states,
}
temporary = output.with_suffix(output.suffix + ".tmp")
temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
os.replace(temporary, output)
if not report["passed"]:
    raise SystemExit("one or more pure SQLite states failed parity")
PY

rm -rf "$CLONE" "$STATE_CACHE"
rm -f "$FULL_REPORT" "$CHANGED_REPORT" "$RUN_ROOT/full-state.log" \
  "$RUN_ROOT/incremental-state.log"
echo "PURE SQLITE repo=$NAME attempt=$ATTEMPT phase=complete" >&2
