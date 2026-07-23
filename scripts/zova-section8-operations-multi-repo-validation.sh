#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
PARENT=$(cd "$ROOT/.." && pwd -P)
RUN_ROOT=${CBM_ZOVA_SECTION8_MULTI_RUN_ROOT:-"$ROOT/build/zova-single-file-multi"}
FOCUSED_ROOT="$RUN_ROOT/section8-focused-environment"
FOCUSED_LOG="$RUN_ROOT/section8-focused.log"
AGGREGATE="$RUN_ROOT/section8-latest-gate.json"
mkdir -p "$RUN_ROOT"

repos=("$PARENT/tops" "$PARENT/motive" "$PARENT/rvault" "$ROOT")
names=("tops" "motive" "rvault" "CBM")
for repo in "${repos[@]}"; do
  [[ -d "$repo/.git" ]] || { echo "error: missing repository: $repo" >&2; exit 1; }
done

if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" != "1" ]]; then
  echo "SECTION 8 BUILD: once" >&2
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
  bash "$ROOT/scripts/zova-build-test-runner.sh" full >/dev/null
fi

echo "SECTION 8 FOCUSED: synthetic suites" >&2
rm -rf "$FOCUSED_ROOT"
CBM_ZOVA_TEST_CACHE_DIR="$FOCUSED_ROOT" CBM_ZOVA_BUILD_SKIP=1 \
  "$ROOT/scripts/zova-run-tests.sh" zova_operations zova_migration zova pipeline \
  mcp cypher cli >"$FOCUSED_LOG" 2>&1 || {
    tail -120 "$FOCUSED_LOG" >&2 || true
    echo "error: focused Section 8 suites failed; no real repository was started" >&2
    exit 1
  }
python3 "$ROOT/tests/test_zova_single_file_gate.py"
CBM_ZOVA_OPERATIONS_BINARY="$ROOT/build/c/codebase-memory-mcp" \
  bash "$ROOT/tests/test_zova_operations.sh"
bash -n "$ROOT/scripts/zova-operations.sh" \
  "$ROOT/scripts/zova-section8-operations-validation.sh" \
  "$ROOT/scripts/zova-section8-operations-multi-repo-validation.sh"
git -C "$ROOT" diff --check
rm -rf "$FOCUSED_ROOT"

report_args=()
for index in 0 1 2 3; do
  repo=${repos[$index]}
  name=${names[$index]}
  confirmation_only=0
  [[ "$name" == "CBM" ]] && confirmation_only=1
  echo "SECTION 8 STAGE $((index + 1))/4: $name" >&2
  CBM_ZOVA_VALIDATION_REPO="$repo" CBM_ZOVA_SECTION8_REPO_NAME="$name" \
  CBM_ZOVA_SECTION8_RUN_ROOT="$RUN_ROOT/$name" CBM_ZOVA_SECTION8_FOCUSED_VERIFIED=1 \
  CBM_ZOVA_SECTION8_CONFIRMATION_ONLY="$confirmation_only" \
  CBM_ZOVA_SECTION8_BINARY="$ROOT/build/c/codebase-memory-mcp" \
    bash "$ROOT/scripts/zova-section8-operations-validation.sh"
  report="$RUN_ROOT/$name/latest-report.json"
  gate="$RUN_ROOT/$name/latest-gate.json"
  python3 - "$name" "$report" "$gate" <<'PY'
import json, sys
name, report_path, gate_path = sys.argv[1:]
with open(report_path, encoding="utf-8") as source:
    report = json.load(source)
with open(gate_path, encoding="utf-8") as source:
    gate = json.load(source)
if gate.get("passed") is not True:
    raise SystemExit(f"{name} gate failed: {gate.get('reasons')}")
if report.get("operations_health_state") != "healthy":
    raise SystemExit(f"{name} report is not healthy")
print(
    f"INSPECTED: {name} schema={report.get('target_schema_version')} "
    f"archive={report.get('operations_archive_version')} "
    f"health={report.get('operations_health_state')}"
)
PY
  report_args+=(--report "$name=$report")
done

python3 "$ROOT/scripts/zova-single-file-gate.py" --require-section8-order \
  --output "$AGGREGATE" "${report_args[@]}"
rm -f "$FOCUSED_LOG"
echo "PASS: Section 8 staged order=tops,motive,rvault,CBM gate=$AGGREGATE"
