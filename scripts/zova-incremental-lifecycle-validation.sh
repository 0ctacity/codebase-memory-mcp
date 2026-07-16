#!/usr/bin/env bash
set -euo pipefail

# Runs one isolated repository through the direct-Zova incremental lifecycle
# gate. The source checkout is never modified: each attempt operates on a
# disposable local clone and compares its normal incremental index with a
# fresh-full control at every lifecycle state.

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-$ROOT}
REPO=$(cd "$REPO" && pwd -P)
RUN_ROOT=${CBM_ZOVA_INCREMENTAL_LIFECYCLE_RUN_DIR:-"$ROOT/build/zova-incremental-lifecycle"}
RUN_ROOT=$(mkdir -p "$RUN_ROOT" && cd "$RUN_ROOT" && pwd -P)
VALIDATOR=${CBM_ZOVA_INCREMENTAL_LIFECYCLE_VALIDATOR:-"$ROOT/scripts/zova-real-repo-validation.sh"}
CHECKER="$ROOT/scripts/zova-incremental-lifecycle-gate.py"
BINARY=${CBM_ZOVA_REAL_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_REAL_TEST_RUNNER:-"$ROOT/build/c/test-runner"}

if [[ ! -d "$REPO/.git" ]]; then
  echo "error: repository is not a Git checkout: $REPO" >&2
  exit 1
fi
if [[ ! -x "$VALIDATOR" && ! -f "$VALIDATOR" ]]; then
  echo "error: validation harness is unavailable: $VALIDATOR" >&2
  exit 1
fi

bash "$ROOT/scripts/zova-disk-guard.sh" "$RUN_ROOT"

RUNS_DIR="$RUN_ROOT/runs"
mkdir -p "$RUNS_DIR"
RUN_DIR=$(mktemp -d "$RUNS_DIR/run.XXXXXX")
WORK_REPO="$RUN_DIR/repo"
INCREMENTAL_CACHE="$RUN_DIR/incremental-cache"
STATE_REPORTS=()

publish_latest() {
  local temporary="$RUN_ROOT/.latest.$$.tmp"
  rm -f "$temporary"
  ln -s "$RUN_DIR" "$temporary"
  python3 - "$temporary" "$RUN_ROOT/latest" <<'PY'
import os
import sys
os.replace(sys.argv[1], sys.argv[2])
PY
}

run_validation() {
  local route="$1"
  local state="$2"
  local cache="$3"
  local validation_root="$RUN_DIR/$route-$state"
  echo "INDEX: route=$route state=$state repo=$(basename "$REPO")" >&2
  CBM_ZOVA_VALIDATION_REPO="$WORK_REPO" \
  CBM_ZOVA_VALIDATION_RUN_DIR="$validation_root" \
  CBM_ZOVA_VALIDATION_CACHE_DIR="$cache" \
  CBM_ZOVA_VALIDATION_SKIP_BUILD=1 \
  CBM_ZOVA_REAL_BINARY="$BINARY" \
  CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
    bash "$VALIDATOR" >&2
  printf '%s/latest/report.json\n' "$validation_root"
}

write_probe_caller() {
  mkdir -p "$WORK_REPO/zova_lifecycle_probe"
  printf '%s\n' \
    'package zovalifecycle' \
    '' \
    'func CbmZovaLifecycleCaller() int {' \
    '    return cbmZovaLifecycleHelper()' \
    '}' > "$WORK_REPO/zova_lifecycle_probe/caller.go"
}

write_probe_helper() {
  local value="$1"
  local path="$2"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' \
    'package zovalifecycle' \
    '' \
    'func cbmZovaLifecycleHelper() int {' \
    "    return $value" \
    '}' > "$path"
}

run_state() {
  local state="$1"
  local incremental_raw control_raw state_report
  incremental_raw=$(run_validation incremental "$state" "$INCREMENTAL_CACHE")
  control_raw=$(run_validation control "$state" "$RUN_DIR/control-$state-cache")
  state_report="$RUN_DIR/$state-report.json"
  if ! python3 "$CHECKER" --output "$state_report" \
      --state "$state=$incremental_raw=$control_raw"; then
    echo "STATE FAIL: $state" >&2
  fi
  STATE_REPORTS+=("$state_report")
  # Comparison is complete; discard both state harness directories and the
  # fresh control cache immediately. The incremental cache remains for the
  # next mutation only.
  rm -rf "$(dirname "$(dirname "$incremental_raw")")" \
         "$(dirname "$(dirname "$control_raw")")" \
         "$RUN_DIR/control-$state-cache"
}

echo "PREPARE: isolated clone for $(basename "$REPO")"
git clone --quiet --shared --no-hardlinks "$REPO" "$WORK_REPO"

# G0 establishes the normal cache/database. Every following incremental pass
# reuses that cache; every control pass gets a brand-new one.
run_validation incremental baseline "$INCREMENTAL_CACHE" >/dev/null
rm -rf "$RUN_DIR/incremental-baseline"

write_probe_caller
write_probe_helper 7 "$WORK_REPO/zova_lifecycle_probe/helper.go"
run_state add

write_probe_helper 99 "$WORK_REPO/zova_lifecycle_probe/helper.go"
run_state edit

mkdir -p "$WORK_REPO/zova_lifecycle_probe/nested"
mv "$WORK_REPO/zova_lifecycle_probe/helper.go" "$WORK_REPO/zova_lifecycle_probe/nested/helper.go"
run_state rename

rm "$WORK_REPO/zova_lifecycle_probe/nested/helper.go"
run_state delete

REPORT="$RUN_DIR/lifecycle-report.json"
python3 - "$REPORT" "${STATE_REPORTS[@]}" <<'PY'
import json
import sys

output, *paths = sys.argv[1:]
states = []
for path in paths:
    with open(path, encoding="utf-8") as source:
        states.append(json.load(source)["states"][0])
report = {"schema_version": 1, "passed": bool(states) and all(s["passed"] for s in states),
          "states": states}
with open(output, "w", encoding="utf-8") as destination:
    json.dump(report, destination, indent=2, sort_keys=True)
    destination.write("\n")
PY
rm -rf "$WORK_REPO" "$INCREMENTAL_CACHE"
if python3 - "$REPORT" <<'PY'
import json
import sys
raise SystemExit(0 if json.load(open(sys.argv[1], encoding="utf-8"))["passed"] else 1)
PY
then
  publish_latest
  echo "LIFECYCLE GATE PASS: $RUN_ROOT/latest/lifecycle-report.json"
else
  echo "LIFECYCLE GATE FAIL: $REPORT" >&2
  exit 1
fi
