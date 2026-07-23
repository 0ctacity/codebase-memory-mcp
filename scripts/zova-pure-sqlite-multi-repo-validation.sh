#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
OUTPUT=${CBM_ZOVA_PURE_ROOT:-"$ROOT/build/zova-pure-sqlite-comparison"}
TOPS=${CBM_ZOVA_PURE_TOPS_REPO:-"$ROOT/../tops"}
MOTIVE=${CBM_ZOVA_PURE_MOTIVE_REPO:-"$ROOT/../motive"}
RVAULT=${CBM_ZOVA_PURE_RVAULT_REPO:-"$ROOT/../rvault"}
CBM=${CBM_ZOVA_PURE_CBM_REPO:-"$ROOT"}
BUILD=${CBM_ZOVA_PURE_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_PURE_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
BUILD_ONCE=${CBM_ZOVA_PURE_BUILD_ONCE:-"$ROOT/scripts/zova-build-once.sh"}
REPOSITORY_RUNNER=${CBM_ZOVA_PURE_REPOSITORY_RUNNER:-"$ROOT/scripts/zova-pure-sqlite-repository-validation.sh"}
COMPARE=${CBM_ZOVA_PURE_COMPARE:-"$ROOT/scripts/zova-pure-sqlite-compare.py"}
SECTION9=${CBM_ZOVA_SECTION9_AGGREGATE:-"$ROOT/build/zova-section9-promotion/section9-latest-gate.json"}

fail() { echo "error: $*" >&2; exit 1; }
for repo in "$TOPS" "$MOTIVE" "$RVAULT" "$CBM"; do
  git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "repository is missing .git: $repo"
done
for path in "$BUILD" "$TEST_RUNNER" "$BUILD_ONCE" "$REPOSITORY_RUNNER" "$COMPARE"; do
  [[ -x "$path" ]] || fail "required executable is missing: $path"
done
[[ -f "$SECTION9" ]] || fail "retained Section 9 aggregate is missing: $SECTION9"
[[ ! -e "$OUTPUT" ]] || fail "comparison root must be fresh: $OUTPUT"
mkdir -p "$OUTPUT"
OUTPUT=$(cd "$OUTPUT" && pwd -P)
LOCK="$OUTPUT/.pure-sqlite-multi.lock"
mkdir "$LOCK"
printf 'pid=%s\n' "$$" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" != 1 ]]; then
  echo "PURE SQLITE phase=build" >&2
  "$BUILD_ONCE" >/dev/null
  "$ROOT/scripts/zova-build-test-runner.sh" full >/dev/null
fi

validate_repository() {
  local name=$1
  python3 - "$name" "$OUTPUT/$name" <<'PY'
import json, pathlib, sys
name, root = sys.argv[1], pathlib.Path(sys.argv[2])
for attempt in range(1, 4):
    path = root / f"attempt-{attempt}" / "repository-report.json"
    data = json.loads(path.read_text())
    if data.get("baseline_route") != "pure_sqlite" or data.get("repository") != name:
        raise SystemExit(f"invalid pure report identity: {path}")
    if data.get("attempt") != attempt or data.get("passed") is not True:
        raise SystemExit(f"failed pure report: {path}")
    states = data.get("states", [])
    if [s.get("state_workload") for s in states] != [
        "full_pipeline", "changed_state_full_pipeline"
    ]:
        raise SystemExit(f"invalid workload labels: {path}")
    if any(s.get("storage", {}).get("compat_zova_bytes") != 0 for s in states):
        raise SystemExit(f"pure report contains sidecar bytes: {path}")
PY
}

run_repository() {
  local name=$1 repo=$2
  echo "PURE SQLITE phase=$name" >&2
  for attempt in 1 2 3; do
    CBM_ZOVA_VALIDATION_REPO="$repo" CBM_ZOVA_PURE_REPOSITORY="$name" \
    CBM_ZOVA_PURE_ATTEMPT="$attempt" \
    CBM_ZOVA_PURE_RUN_ROOT="$OUTPUT/$name/attempt-$attempt" \
    CBM_ZOVA_PURE_BUILD_BINARY="$BUILD" CBM_ZOVA_PURE_TEST_RUNNER="$TEST_RUNNER" \
    "$REPOSITORY_RUNNER"
  done
  validate_repository "$name"
  echo "PURE SQLITE phase=$name-validated" >&2
}

# Deliberately finish and validate all TOPS attempts before starting any larger repository.
run_repository tops "$TOPS"
run_repository motive "$MOTIVE"
run_repository rvault "$RVAULT"
run_repository CBM "$CBM"

compare_args=()
for name in tops motive rvault CBM; do
  for attempt in 1 2 3; do
    compare_args+=(--pure-report "$name=$OUTPUT/$name/attempt-$attempt/repository-report.json")
  done
done
echo "PURE SQLITE phase=compare" >&2
"$COMPARE" --section9 "$SECTION9" --output "$OUTPUT/comparison.json" \
  --markdown "$OUTPUT/comparison.md" "${compare_args[@]}"
echo "PURE SQLITE phase=complete" >&2
