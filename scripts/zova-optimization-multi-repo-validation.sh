#!/usr/bin/env bash
set -euo pipefail

MODE=${1:-all}
[[ $# -le 1 ]] || { echo "usage: $0 [all|index-only]" >&2; exit 2; }
case "$MODE" in all|index-only) ;;
  *) echo "usage: $0 [all|index-only]" >&2; exit 2;;
esac

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
OUTPUT=${CBM_ZOVA_OPTIMIZATION_FINAL_ROOT:-"$ROOT/build/zova-optimization/final"}
TOPS=${CBM_ZOVA_OPTIMIZATION_TOPS_REPO:-"$ROOT/../tops"}
DENO=${CBM_ZOVA_OPTIMIZATION_DENO_REPO:-"$ROOT/../deno"}
MOTIVE=${CBM_ZOVA_OPTIMIZATION_MOTIVE_REPO:-"$ROOT/../motive"}
RVAULT=${CBM_ZOVA_OPTIMIZATION_RVAULT_REPO:-"$ROOT/../rvault"}
CBM=${CBM_ZOVA_OPTIMIZATION_CBM_REPO:-"$ROOT"}
BASELINES=${CBM_ZOVA_OPTIMIZATION_BASELINE_ROOT:-"$ROOT/build/zova-optimization/baseline"}
BUILD=${CBM_ZOVA_OPTIMIZATION_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_OPTIMIZATION_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
BUILD_ONCE=${CBM_ZOVA_OPTIMIZATION_BUILD_ONCE:-"$ROOT/scripts/zova-build-once.sh"}
FOCUSED=${CBM_ZOVA_OPTIMIZATION_FOCUSED_RUNNER:-"$ROOT/scripts/zova-run-tests.sh"}
REPOSITORY_RUNNER=${CBM_ZOVA_OPTIMIZATION_REPOSITORY_RUNNER:-"$ROOT/scripts/zova-optimization-validation.sh"}
GATE=${CBM_ZOVA_OPTIMIZATION_GATE:-"$ROOT/scripts/zova-optimization-gate.py"}
BASELINE_EXTRACTOR=${CBM_ZOVA_OPTIMIZATION_BASELINE_EXTRACTOR:-"$ROOT/scripts/zova-optimization-baseline.py"}
AGGREGATOR=${CBM_ZOVA_OPTIMIZATION_AGGREGATOR:-"$ROOT/scripts/zova-optimization-aggregate.py"}
MANIFEST=${CBM_ZOVA_OPTIMIZATION_MANIFEST:-"$ROOT/.zova-sdk/manifest.txt"}

fail() { echo "error: $*" >&2; exit 1; }

if [[ "$MODE" == index-only ]]; then
  for repo in "$TOPS" "$DENO"; do
    git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
      fail "repository is missing .git: $repo"
  done
  for path in "$BUILD_ONCE" "$REPOSITORY_RUNNER"; do
    [[ -x "$path" ]] || fail "required executable is missing: $path"
  done
  [[ ! -e "$OUTPUT" ]] || fail "index-only output root must be fresh: $OUTPUT"
  mkdir -p "$OUTPUT"
  OUTPUT=$(cd "$OUTPUT" && pwd -P)
  if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" != 1 ]]; then
    echo "ZOVA INDEX ONLY phase=build" >&2
    "$BUILD_ONCE" >/dev/null
    "$ROOT/scripts/zova-build-test-runner.sh" full >/dev/null
  fi
  [[ -x "$BUILD" && -x "$TEST_RUNNER" ]] || fail "build products are missing"

  run_index_only_repository() {
    local name=$1 repo=$2 attempt=$3
    local run_root="$OUTPUT/$name/attempt-$attempt"
    echo "ZOVA INDEX ONLY repository=$name attempt=$attempt workers=${CBM_WORKERS:-auto}" >&2
    CBM_ZOVA_BUILD_SKIP=1 \
    CBM_ZOVA_VALIDATION_REPO="$repo" \
    CBM_ZOVA_OPTIMIZATION_REPOSITORY="$name" \
    CBM_ZOVA_OPTIMIZATION_ATTEMPT="$attempt" \
    CBM_ZOVA_OPTIMIZATION_MUTATION=source-change \
    CBM_ZOVA_OPTIMIZATION_BENCHMARK_KIND=storage-ingestion \
    CBM_ZOVA_OPTIMIZATION_RUN_ROOT="$run_root" \
    CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$BUILD" \
    CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TEST_RUNNER" \
    CBM_ZOVA_OPTIMIZATION_BASELINE_CAPTURE=1 \
    "$REPOSITORY_RUNNER" index-only
  }

  for name in tops deno; do
    repo=$TOPS
    [[ "$name" == deno ]] && repo=$DENO
    run_index_only_repository "$name" "$repo" 1
    run_index_only_repository "$name" "$repo" 2
  done

  python3 - "$OUTPUT" "${CBM_WORKERS:-auto}" <<'PY'
import json, os, pathlib, statistics, sys
root = pathlib.Path(sys.argv[1])
workers = sys.argv[2]
repositories = []
for name in ("tops", "deno"):
    samples = []
    states_by_route = {"pure": [], "single": []}
    for attempt in (1, 2):
        path = root / name / f"attempt-{attempt}" / "optimization-report.json"
        report = json.loads(path.read_text())
        if report.get("benchmark_scope") != "index-only":
            raise SystemExit(f"{path} is not an index-only report")
        states = {state["route"]: state for state in report["states"]}
        if set(states) != {"pure", "single"}:
            raise SystemExit(f"{path} does not contain pure and single states")
        for route in states_by_route:
            states_by_route[route].append(states[route])
        samples.append({
            "attempt": attempt,
            "pure_sqlite_full_ms": states["pure"]["timing_ms"]["pipeline"],
            "zova_native_full_ms": states["single"]["timing_ms"]["pipeline"],
            "zova_publish_ms": states["single"]["timing_ms"]["publish"],
            "pure_sqlite_bytes": states["pure"]["storage"]["database_bytes"],
            "zova_native_bytes": states["single"]["storage"]["database_bytes"],
        })
    average = {
        key: statistics.fmean(sample[key] for sample in samples)
        for key in samples[0] if key != "attempt"
    }
    timing_fields = states_by_route["single"][0]["timing_ms"]
    zova_phase_average_ms = {
        field: statistics.fmean(state["timing_ms"][field]
                                for state in states_by_route["single"])
        for field in timing_fields
    }
    repositories.append({
        "repository": name,
        "sample_count": 2,
        "samples": samples,
        "average": average,
        "zova_phase_average_ms": zova_phase_average_ms,
    })
output = root / "index-only-average.json"
temporary = output.with_suffix(".json.tmp")
temporary.write_text(json.dumps({
    "schema_version": 1,
    "benchmark_scope": "index-only",
    "workers": workers,
    "repositories": repositories,
}, indent=2, sort_keys=True) + "\n")
os.replace(temporary, output)
print(output)
PY
  echo "ZOVA INDEX ONLY phase=complete" >&2
  exit 0
fi

for repo in "$TOPS" "$MOTIVE" "$RVAULT" "$CBM"; do
  git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "repository is missing .git: $repo"
done
for path in "$BUILD_ONCE" "$FOCUSED" "$REPOSITORY_RUNNER"; do
  [[ -x "$path" ]] || fail "required executable is missing: $path"
done
[[ -f "$GATE" ]] || fail "optimization gate is missing: $GATE"
[[ -f "$BASELINE_EXTRACTOR" ]] || fail "documented baseline extractor is missing: $BASELINE_EXTRACTOR"
[[ -f "$AGGREGATOR" ]] || fail "optimization aggregator is missing: $AGGREGATOR"
[[ -f "$MANIFEST" ]] || fail "pinned Zova manifest is missing: $MANIFEST"
echo "ZOVA OPTIMIZATION phase=documented-baseline" >&2
if [[ "$BASELINE_EXTRACTOR" == *.py ]]; then
  python3 "$BASELINE_EXTRACTOR" --root "$ROOT" --output "$BASELINES"
else
  "$BASELINE_EXTRACTOR" --root "$ROOT" --output "$BASELINES"
fi
for name in tops motive rvault CBM; do
  [[ -f "$BASELINES/$name.json" ]] ||
    fail "immutable optimization baseline is missing: $BASELINES/$name.json"
done
[[ ! -e "$OUTPUT" ]] || fail "final optimization root must be fresh: $OUTPUT"
mkdir -p "$OUTPUT"
OUTPUT=$(cd "$OUTPUT" && pwd -P)
LOCK="$OUTPUT/.optimization-multi.lock"
mkdir "$LOCK"
printf 'pid=%s\n' "$$" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

run_gate() {
  if [[ "$GATE" == *.py ]]; then python3 "$GATE" "$@"; else "$GATE" "$@"; fi
}

if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" != 1 ]]; then
  echo "ZOVA OPTIMIZATION phase=build" >&2
  "$BUILD_ONCE" >/dev/null
  "$ROOT/scripts/zova-build-test-runner.sh" full >/dev/null
fi
[[ -x "$BUILD" && -x "$TEST_RUNNER" ]] || fail "build products are missing"

echo "ZOVA OPTIMIZATION phase=focused-small-fixtures" >&2
CBM_ZOVA_BUILD_SKIP=1 CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
  "$FOCUSED" zova zova_incremental_native zova_migration zova_operations mcp cypher

python3 - "$ROOT" "$BUILD" "$TEST_RUNNER" "$MANIFEST" "$BASELINES" \
  "$OUTPUT/provenance.json" <<'PY'
import hashlib, json, os, pathlib, platform, subprocess, sys
root, build, runner, manifest, baselines, output = map(pathlib.Path, sys.argv[1:])
def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()
def command(*args):
    return subprocess.check_output(args, cwd=root, text=True).strip()
diff = subprocess.check_output(["git", "diff", "--binary"], cwd=root)
manifest_text = manifest.read_text()
source_commit = ""
for line in manifest_text.splitlines():
    if line.startswith("source_commit="):
        source_commit = line.split("=", 1)[1]
report = {
    "schema_version": 1,
    "cbm_commit": command("git", "rev-parse", "HEAD"),
    "cbm_diff_sha256": hashlib.sha256(diff).hexdigest(),
    "zova_source_commit": source_commit,
    "zova_manifest_sha256": digest(manifest),
    "binary_sha256": digest(build),
    "test_runner_sha256": digest(runner),
    "cbm_schema_version": 6,
    "zova_format_version": 9,
    "machine": {"platform": platform.platform(), "machine": platform.machine()},
    "baseline_sha256": {
        name: digest(baselines / f"{name}.json")
        for name in ("tops", "motive", "rvault", "CBM")
    },
}
temporary = output.with_suffix(".tmp")
temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
os.replace(temporary, output)
PY

run_repository() {
  local name=$1 repo=$2 attempt=$3
  local run_root="$OUTPUT/$name/attempt-$attempt"
  echo "ZOVA OPTIMIZATION phase=$name attempt=$attempt" >&2
  CBM_ZOVA_VALIDATION_REPO="$repo" \
  CBM_ZOVA_OPTIMIZATION_REPOSITORY="$name" \
  CBM_ZOVA_OPTIMIZATION_ATTEMPT="$attempt" \
  CBM_ZOVA_OPTIMIZATION_MUTATION=source-change \
  CBM_ZOVA_OPTIMIZATION_BENCHMARK_KIND=storage-ingestion \
  CBM_ZOVA_OPTIMIZATION_RUN_ROOT="$run_root" \
  CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$BUILD" \
  CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TEST_RUNNER" \
  CBM_ZOVA_OPTIMIZATION_BASELINE="$BASELINES/$name.json" \
  CBM_ZOVA_OPTIMIZATION_BASELINE_CAPTURE=1 \
  "$REPOSITORY_RUNNER"
}

for attempt in 1 2 3; do run_repository tops "$TOPS" "$attempt"; done
run_gate --aggregate-baseline \
  --sample "$OUTPUT/tops/attempt-1/optimization-report.json" \
  --sample "$OUTPUT/tops/attempt-2/optimization-report.json" \
  --sample "$OUTPUT/tops/attempt-3/optimization-report.json" \
  --output "$OUTPUT/tops/current-median.json"
echo "ZOVA OPTIMIZATION phase=tops-gate" >&2
run_gate --baseline "$BASELINES/tops.json" \
  --report "$OUTPUT/tops/current-median.json" \
  --output "$OUTPUT/tops/decision.json"

# No larger repository is reachable until the three TOPS attempts and strict
# median gate above have passed.
run_repository motive "$MOTIVE" 1
run_repository rvault "$RVAULT" 1
run_repository CBM "$CBM" 1

echo "ZOVA OPTIMIZATION phase=aggregate-gate" >&2
for name in motive rvault CBM; do
  run_gate --baseline "$BASELINES/$name.json" \
    --report "$OUTPUT/$name/attempt-1/optimization-report.json" \
    --output "$OUTPUT/$name/decision.json"
done

if [[ "$AGGREGATOR" == *.py ]]; then
  python3 "$AGGREGATOR" --root "$OUTPUT" --baseline-root "$BASELINES"
else
  "$AGGREGATOR" --root "$OUTPUT" --baseline-root "$BASELINES"
fi
echo "ZOVA OPTIMIZATION phase=complete" >&2
