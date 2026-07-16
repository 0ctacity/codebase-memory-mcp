#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-}
NAME=${CBM_ZOVA_OPTIMIZATION_REPOSITORY:-}
RUN_ROOT=${CBM_ZOVA_OPTIMIZATION_RUN_ROOT:-}
BASELINE=${CBM_ZOVA_OPTIMIZATION_BASELINE:-}
BASELINE_CAPTURE=${CBM_ZOVA_OPTIMIZATION_BASELINE_CAPTURE:-0}
ATTEMPT=${CBM_ZOVA_OPTIMIZATION_ATTEMPT:-0}
MUTATION=${CBM_ZOVA_OPTIMIZATION_MUTATION:-}
BUILD=${CBM_ZOVA_OPTIMIZATION_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_OPTIMIZATION_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
RUN_TESTS=${CBM_ZOVA_OPTIMIZATION_RUN_TESTS:-"$ROOT/scripts/zova-run-tests.sh"}
GATE=${CBM_ZOVA_OPTIMIZATION_GATE:-"$ROOT/scripts/zova-optimization-gate.py"}
DISK_GUARD=${CBM_ZOVA_OPTIMIZATION_DISK_GUARD:-"$ROOT/scripts/zova-disk-guard.sh"}

fail() { echo "error: $*" >&2; exit 1; }
[[ -n "$REPO" && -n "$NAME" && -n "$RUN_ROOT" ]] ||
  fail "repository path, name, and run root are required"
case "$NAME" in tops|motive|rvault|CBM) ;; *) fail "invalid repository name: $NAME";; esac
case "$MUTATION" in digest-stable|source-change) ;;
  *) fail "mutation must be digest-stable or source-change";;
esac
[[ "$BASELINE_CAPTURE" == 0 || "$BASELINE_CAPTURE" == 1 ]] ||
  fail "baseline capture must be 0 or 1"
[[ "$ATTEMPT" =~ ^[0-9]+$ ]] || fail "attempt must be a nonnegative integer"
git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
  fail "repository is missing .git: $REPO"
for path in "$BUILD" "$TEST_RUNNER" "$RUN_TESTS" "$DISK_GUARD"; do
  [[ -x "$path" ]] || fail "required executable is missing: $path"
done
[[ -f "$GATE" ]] || fail "optimization gate is missing: $GATE"
if [[ "$BASELINE_CAPTURE" == 0 ]]; then
  [[ -f "$BASELINE" ]] || fail "optimization baseline is missing: $BASELINE"
fi

REPO=$(cd "$REPO" && pwd -P)
[[ ! -e "$RUN_ROOT" ]] || fail "run root must be fresh: $RUN_ROOT"
mkdir -p "$RUN_ROOT"
RUN_ROOT=$(cd "$RUN_ROOT" && pwd -P)
LOCK="$RUN_ROOT/.optimization.lock"
CLONE="$RUN_ROOT/clone"
PURE_CACHE="$RUN_ROOT/pure-cache"
SINGLE_CACHE="$RUN_ROOT/single-cache"
REPORT="$RUN_ROOT/optimization-report.json"
DECISION="$RUN_ROOT/optimization-gate.json"
RUN_ID="$NAME-$MUTATION-optimization-attempt-$ATTEMPT-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir "$LOCK"
printf 'pid=%s\nrepository=%s\n' "$$" "$NAME" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

"$DISK_GUARD" "$RUN_ROOT"
echo "ZOVA OPTIMIZATION repo=$NAME phase=clone" >&2
git clone -q --shared --no-hardlinks "$REPO" "$CLONE"
SOURCE_COMMIT=$(git -C "$CLONE" rev-parse HEAD)
BUILD_SHA=$(shasum -a 256 "$BUILD" | awk '{print $1}')
mkdir -p "$PURE_CACHE" "$SINGLE_CACHE" "$CLONE/zova_optimization_probe"
MUTATION_FILE="$CLONE/zova_optimization_probe/probe.go"
cat >"$MUTATION_FILE" <<'GO'
package zova_optimization_probe

func OptimizationProbe() int { return 7 }
GO

run_state() {
  local route=$1 workload=$2 cache=$3 report=$4 log=$5 mode
  if [[ "$workload" == incremental ]]; then mode=CBM_MODE_INCREMENTAL; else mode=CBM_MODE_FULL; fi
  echo "ZOVA OPTIMIZATION repo=$NAME route=$route workload=$workload" >&2
  CBM_ZOVA_TEST_CACHE_DIR="$cache" CBM_ZOVA_BUILD_SKIP=1 \
  CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
  CBM_ZOVA_VALIDATION_REPO="$CLONE" \
  CBM_ZOVA_OPTIMIZATION_REPORT="$report" \
  CBM_ZOVA_OPTIMIZATION_ROUTE="$route" \
  CBM_ZOVA_OPTIMIZATION_WORKLOAD="$workload" \
  CBM_ZOVA_OPTIMIZATION_PIPELINE_MODE="$mode" \
  CBM_ZOVA_OPTIMIZATION_MUTATION="$MUTATION" \
  CBM_ZOVA_OPTIMIZATION_MUTATION_FILE="$MUTATION_FILE" \
  CBM_ZOVA_PROMOTION_STATE="$workload" \
  CBM_ZOVA_PROMOTION_REPORT="$report" \
  CBM_ZOVA_PROMOTION_REPOSITORY="$NAME" \
  CBM_ZOVA_PROMOTION_RUN_ID="$RUN_ID" \
  CBM_ZOVA_PROMOTION_ATTEMPT="$ATTEMPT" \
  CBM_ZOVA_PROMOTION_BUILD_SHA256="$BUILD_SHA" \
  CBM_ZOVA_SECTION9_CALIBRATION_MODE=0 \
  "$RUN_TESTS" zova_single_file_promotion_real_repo >"$log" 2>&1 || {
    tail -80 "$log" >&2 || true
    fail "$route $workload state failed; diagnostics retained in $RUN_ROOT"
  }
  [[ -s "$report" ]] || fail "$route $workload report is missing"
}

PURE_FULL="$RUN_ROOT/pure-full.json"
SINGLE_FULL="$RUN_ROOT/single-full.json"
PURE_INCREMENTAL="$RUN_ROOT/pure-incremental.json"
SINGLE_INCREMENTAL="$RUN_ROOT/single-incremental.json"
run_state pure full "$PURE_CACHE" "$PURE_FULL" "$RUN_ROOT/pure-full.log"
run_state single full "$SINGLE_CACHE" "$SINGLE_FULL" "$RUN_ROOT/single-full.log"

python3 - "$MUTATION_FILE" "$MUTATION" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
mutation = sys.argv[2]
text = path.read_text()
if text.count("return 7") != 1:
    raise SystemExit("optimization probe does not contain exactly one return 7")
if mutation == "source-change":
    path.write_text(text.replace("return 7", "return 99"))
elif mutation == "digest-stable":
    path.touch()
else:
    raise SystemExit(f"unsupported mutation: {mutation}")
PY

run_state pure incremental "$PURE_CACHE" "$PURE_INCREMENTAL" \
  "$RUN_ROOT/pure-incremental.log"
run_state single incremental "$SINGLE_CACHE" "$SINGLE_INCREMENTAL" \
  "$RUN_ROOT/single-incremental.log"

python3 - "$PURE_FULL" "$SINGLE_FULL" "$PURE_INCREMENTAL" "$SINGLE_INCREMENTAL" \
  "$REPORT" "$NAME" "$SOURCE_COMMIT" "$BUILD_SHA" "$RUN_ID" "$MUTATION" <<'PY'
import json, os, pathlib, sys
state_paths = [pathlib.Path(value) for value in sys.argv[1:5]]
output = pathlib.Path(sys.argv[5])
name, source_commit, build_sha256, run_id, mutation = sys.argv[6:]
states = [json.loads(path.read_text()) for path in state_paths]
expected = [
    ("pure", "full", "CBM_MODE_FULL"),
    ("single", "full", "CBM_MODE_FULL"),
    ("pure", "incremental", "CBM_MODE_INCREMENTAL"),
    ("single", "incremental", "CBM_MODE_INCREMENTAL"),
]
for index, (route, workload, mode) in enumerate(expected):
    state = states[index]
    if (state.get("route"), state.get("workload"), state.get("pipeline_mode")) != (
        route, workload, mode
    ):
        raise SystemExit(f"state {index} route/workload/mode mismatch")
report = {
    "schema_version": 1,
    "benchmark_kind": "lazy-hydration",
    "repository": name,
    "source_commit": source_commit,
    "build_sha256": build_sha256,
    "run_id": run_id,
    "mutation": mutation,
    "passed": all(state.get("passed") is True for state in states),
    "states": states,
}
temporary = output.with_suffix(output.suffix + ".tmp")
temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
os.replace(temporary, output)
PY

if [[ "$BASELINE_CAPTURE" == 1 ]]; then
  printf '{"schema_version":1,"repository":"%s","passed":true,"baseline_capture":true}\n' \
    "$NAME" >"$DECISION"
else
  echo "ZOVA OPTIMIZATION repo=$NAME phase=gate" >&2
  if [[ "$GATE" == *.py ]]; then GATE_COMMAND=(python3 "$GATE"); else GATE_COMMAND=("$GATE"); fi
  "${GATE_COMMAND[@]}" --baseline "$BASELINE" --report "$REPORT" --output "$DECISION" ||
    fail "optimization gate failed; diagnostics retained in $RUN_ROOT"
fi

rm -rf "$CLONE" "$PURE_CACHE" "$SINGLE_CACHE"
echo "ZOVA OPTIMIZATION repo=$NAME phase=complete" >&2
