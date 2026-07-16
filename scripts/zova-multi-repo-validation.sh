#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPOS=("$ROOT" "$ROOT/../motive" "$ROOT/../rvault" "$ROOT/../tops")
BINARY=${CBM_ZOVA_REAL_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_REAL_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
GATE_ROOT=${CBM_ZOVA_GRAPH_GATE_ROOT:-"$ROOT/build/zova-real-repo/graph-gate"}
GATE_RUNS=${CBM_ZOVA_GRAPH_GATE_RUNS:-3}
GATE_CHECKER="$ROOT/scripts/zova-graph-promotion-gate.py"

case "$GATE_RUNS" in
  ''|*[!0-9]*)
    echo "error: CBM_ZOVA_GRAPH_GATE_RUNS must be a positive integer" >&2
    exit 1
    ;;
esac
if (( GATE_RUNS <= 0 )); then
  echo "error: CBM_ZOVA_GRAPH_GATE_RUNS must be a positive integer" >&2
  exit 1
fi

if [[ "${CBM_ZOVA_VALIDATION_SKIP_BUILD:-0}" != "1" ]]; then
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
fi

mkdir -p "$GATE_ROOT"
failed=0

for ((attempt = 1; attempt <= GATE_RUNS; attempt++)); do
  attempt_root="$GATE_ROOT/run-$attempt"
  report_args=()
  topology_report_args=()
  vector_report_args=()
  echo "=== ZOVA GRAPH PROMOTION GATE: run $attempt/$GATE_RUNS ==="

  for repo in "${REPOS[@]}"; do
    if [[ ! -d "$repo" ]]; then
      echo "error: approved validation repository is missing: $repo" >&2
      exit 1
    fi
    name=$(basename "$repo")
    validation_root="$attempt_root/$name"
    echo "VALIDATE: run=$attempt repo=$name"
    CBM_ZOVA_VALIDATION_REPO="$repo" \
    CBM_ZOVA_VALIDATION_RUN_DIR="$validation_root" \
    CBM_ZOVA_VALIDATION_SKIP_BUILD=1 \
    CBM_ZOVA_REAL_BINARY="$BINARY" \
    CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" \
      bash "$ROOT/scripts/zova-real-repo-validation.sh"
    report_args+=(--report "$name=$validation_root/latest/graph-mcp-report.json")
    topology_report_args+=(--topology-report "$name=$validation_root/latest/graph-report.json")
    vector_report_args+=(--vector-report "$name=$validation_root/latest/report.json")
  done

  decision="$attempt_root/promotion-gate.json"
  if python3 "$GATE_CHECKER" --output "$decision" "${report_args[@]}" \
      "${topology_report_args[@]}" "${vector_report_args[@]}"; then
    echo "PROMOTION GATE PASS: $decision"
  else
    echo "PROMOTION GATE FAIL: $decision" >&2
    failed=1
  fi
done

if (( failed != 0 )); then
  echo "error: Zova graph-read promotion gate did not pass every fresh run" >&2
  exit 1
fi

echo "PROMOTION GATE PASS: $GATE_RUNS fresh four-repository runs"
