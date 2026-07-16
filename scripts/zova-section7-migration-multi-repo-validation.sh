#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
PARENT=$(cd "$ROOT/.." && pwd -P)
RUN_ROOT=${CBM_ZOVA_SECTION7_MULTI_RUN_ROOT:-$ROOT/build/zova-single-file-multi}
GATE="$RUN_ROOT/section7-latest-gate.json"

# Keep the first real-repository gate small, then confirm sequentially on the
# remaining repositories. A failure stops the sequence immediately.
repos=(
  "$PARENT/tops"
  "$PARENT/motive"
  "$PARENT/rvault"
  "$ROOT"
)

mkdir -p "$RUN_ROOT"
report_args=()
for repo in "${repos[@]}"; do
  [[ -d "$repo/.git" ]] || { echo "error: missing repository: $repo" >&2; exit 1; }
  name=$(basename "$repo")
  echo "SECTION 7 RUN: repo=$name"
  CBM_ZOVA_VALIDATION_REPO="$repo" \
  CBM_ZOVA_SECTION7_RUN_ROOT="$RUN_ROOT/$name" \
    bash "$ROOT/scripts/zova-section7-migration-validation.sh"
  report_args+=(--report "$name=$RUN_ROOT/$name/latest-report.json")
done

python3 "$ROOT/scripts/zova-single-file-gate.py" --output "$GATE" "${report_args[@]}"
echo "PASS: Section 7 four-repository migration gate=$GATE"
