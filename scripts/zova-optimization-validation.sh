#!/usr/bin/env bash
set -euo pipefail

SCOPE=${1:-all}
[[ $# -le 1 ]] || { echo "usage: $0 [all|full|index-only]" >&2; exit 2; }
case "$SCOPE" in all|full|index-only) ;;
  *) echo "usage: $0 [all|full|index-only]" >&2; exit 2;;
esac
INDEX_ONLY=0
[[ "$SCOPE" == index-only ]] && INDEX_ONLY=1

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-}
NAME=${CBM_ZOVA_OPTIMIZATION_REPOSITORY:-}
RUN_ROOT=${CBM_ZOVA_OPTIMIZATION_RUN_ROOT:-}
BASELINE=${CBM_ZOVA_OPTIMIZATION_BASELINE:-}
BASELINE_CAPTURE=${CBM_ZOVA_OPTIMIZATION_BASELINE_CAPTURE:-0}
ATTEMPT=${CBM_ZOVA_OPTIMIZATION_ATTEMPT:-0}
MUTATION=${CBM_ZOVA_OPTIMIZATION_MUTATION:-}
BENCHMARK_KIND=${CBM_ZOVA_OPTIMIZATION_BENCHMARK_KIND:-lazy-hydration}
BUILD=${CBM_ZOVA_OPTIMIZATION_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_OPTIMIZATION_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
RUN_TESTS=${CBM_ZOVA_OPTIMIZATION_RUN_TESTS:-"$ROOT/scripts/zova-run-tests.sh"}
GATE=${CBM_ZOVA_OPTIMIZATION_GATE:-"$ROOT/scripts/zova-optimization-gate.py"}
DISK_GUARD=${CBM_ZOVA_OPTIMIZATION_DISK_GUARD:-"$ROOT/scripts/zova-disk-guard.sh"}

fail() { echo "error: $*" >&2; exit 1; }
[[ -n "$REPO" && -n "$NAME" && -n "$RUN_ROOT" ]] ||
  fail "repository path, name, and run root are required"
case "$NAME" in tops|motive|rvault|rchat|deno|CBM) ;; *) fail "invalid repository name: $NAME";; esac
case "$MUTATION" in digest-stable|source-change) ;;
  *) fail "mutation must be digest-stable or source-change";;
esac
case "$BENCHMARK_KIND" in storage-ingestion|lazy-hydration) ;;
  *) fail "benchmark kind must be storage-ingestion or lazy-hydration";;
esac
[[ "$BASELINE_CAPTURE" == 0 || "$BASELINE_CAPTURE" == 1 ]] ||
  fail "baseline capture must be 0 or 1"
[[ "$ATTEMPT" =~ ^[0-9]+$ ]] || fail "attempt must be a nonnegative integer"
git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
  fail "repository is missing .git: $REPO"
for path in "$BUILD" "$TEST_RUNNER" "$RUN_TESTS" "$DISK_GUARD"; do
  [[ -x "$path" ]] || fail "required executable is missing: $path"
done
if [[ "$SCOPE" == all && "$BASELINE_CAPTURE" == 0 ]]; then
  [[ -f "$GATE" ]] || fail "optimization gate is missing: $GATE"
  [[ -f "$BASELINE" ]] || fail "optimization baseline is missing: $BASELINE"
fi

REPO=$(cd "$REPO" && pwd -P)
[[ ! -e "$RUN_ROOT" ]] || fail "run root must be fresh: $RUN_ROOT"
mkdir -p "$RUN_ROOT"
RUN_ROOT=$(cd "$RUN_ROOT" && pwd -P)
LOCK="$RUN_ROOT/.optimization.lock"
CLONE="$RUN_ROOT/clone"
BENCHMARK_CACHE="$RUN_ROOT/benchmark-cache"
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
mkdir -p "$BENCHMARK_CACHE" "$CLONE/zova_optimization_probe"
MUTATION_FILE="$CLONE/zova_optimization_probe/probe.go"
cat >"$MUTATION_FILE" <<'GO'
package zova_optimization_probe

func OptimizationProbe() int { return 7 }
GO

run_workload() {
  local workload=$1 pure_report=$2 single_report=$3 log=$4 mode
  if [[ "$workload" == incremental ]]; then mode=CBM_MODE_INCREMENTAL; else mode=CBM_MODE_FULL; fi
  echo "ZOVA OPTIMIZATION repo=$NAME workload=$workload routes=pure,single" >&2
  CBM_ZOVA_TEST_CACHE_DIR="$BENCHMARK_CACHE" CBM_ZOVA_BUILD_SKIP=1 \
  CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
  CBM_ZOVA_VALIDATION_REPO="$CLONE" \
  CBM_ZOVA_OPTIMIZATION_PURE_REPORT="$pure_report" \
  CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT="$single_report" \
  CBM_ZOVA_OPTIMIZATION_WORKLOAD="$workload" \
  CBM_ZOVA_OPTIMIZATION_INDEX_ONLY="$INDEX_ONLY" \
  CBM_ZOVA_OPTIMIZATION_PIPELINE_MODE="$mode" \
  CBM_ZOVA_OPTIMIZATION_MUTATION="$MUTATION" \
  CBM_ZOVA_OPTIMIZATION_MUTATION_FILE="$MUTATION_FILE" \
  CBM_ZOVA_PROMOTION_STATE="$workload" \
  CBM_ZOVA_PROMOTION_REPORT="$single_report" \
  CBM_ZOVA_PROMOTION_REPOSITORY="$NAME" \
  CBM_ZOVA_PROMOTION_RUN_ID="$RUN_ID" \
  CBM_ZOVA_PROMOTION_ATTEMPT="$ATTEMPT" \
  CBM_ZOVA_PROMOTION_BUILD_SHA256="$BUILD_SHA" \
  CBM_ZOVA_SECTION9_CALIBRATION_MODE=0 \
  "$RUN_TESTS" zova_single_file_promotion_real_repo >"$log" 2>&1 || {
    tail -80 "$log" >&2 || true
    fail "$workload workload failed; diagnostics retained in $RUN_ROOT"
  }
  [[ -s "$pure_report" ]] || fail "pure $workload report is missing"
  [[ -s "$single_report" ]] || fail "single $workload report is missing"
}

PURE_FULL="$RUN_ROOT/pure-full.json"
SINGLE_FULL="$RUN_ROOT/single-full.json"
PURE_INCREMENTAL="$RUN_ROOT/pure-incremental.json"
SINGLE_INCREMENTAL="$RUN_ROOT/single-incremental.json"
run_workload full "$PURE_FULL" "$SINGLE_FULL" "$RUN_ROOT/full.log"

if [[ "$SCOPE" == all ]]; then
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

  run_workload incremental "$PURE_INCREMENTAL" "$SINGLE_INCREMENTAL" \
    "$RUN_ROOT/incremental.log"
fi

python3 - "$PURE_FULL" "$SINGLE_FULL" "$PURE_INCREMENTAL" "$SINGLE_INCREMENTAL" \
  "$REPORT" "$NAME" "$SOURCE_COMMIT" "$BUILD_SHA" "$RUN_ID" "$MUTATION" \
  "$BENCHMARK_KIND" "$SCOPE" <<'PY'
import json, os, pathlib, sys
all_state_paths = [pathlib.Path(value) for value in sys.argv[1:5]]
output = pathlib.Path(sys.argv[5])
name, source_commit, build_sha256, run_id, mutation, benchmark_kind, scope = sys.argv[6:]
state_count = 4 if scope == "all" else 2
state_paths = all_state_paths[:state_count]
states = [json.loads(path.read_text()) for path in state_paths]
expected = [
    ("pure", "full", "CBM_MODE_FULL"),
    ("single", "full", "CBM_MODE_FULL"),
    ("pure", "incremental", "CBM_MODE_INCREMENTAL"),
    ("single", "incremental", "CBM_MODE_INCREMENTAL"),
][:state_count]
for index, (route, workload, mode) in enumerate(expected):
    state = states[index]
    if (state.get("route"), state.get("workload"), state.get("pipeline_mode")) != (
        route, workload, mode
    ):
        raise SystemExit(f"state {index} route/workload/mode mismatch")
report = {
    "schema_version": 1,
    "benchmark_scope": scope,
    "benchmark_kind": benchmark_kind,
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
elif [[ "$SCOPE" != all ]]; then
  printf '{"schema_version":1,"repository":"%s","passed":true,"benchmark_scope":"%s","gate_skipped":true}\n' \
    "$NAME" "$SCOPE" >"$DECISION"
else
  echo "ZOVA OPTIMIZATION repo=$NAME phase=gate" >&2
  if [[ "$GATE" == *.py ]]; then GATE_COMMAND=(python3 "$GATE"); else GATE_COMMAND=("$GATE"); fi
  "${GATE_COMMAND[@]}" --baseline "$BASELINE" --report "$REPORT" --output "$DECISION" ||
    fail "optimization gate failed; diagnostics retained in $RUN_ROOT"
fi

rm -rf "$CLONE" "$BENCHMARK_CACHE"
echo "ZOVA OPTIMIZATION repo=$NAME phase=complete" >&2
