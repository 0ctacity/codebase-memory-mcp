#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
PARENT=$(cd "$ROOT/.." && pwd -P)
RUN_ROOT=${CBM_ZOVA_SINGLE_FILE_MULTI_RUN_ROOT:-$ROOT/build/zova-single-file-multi}
mkdir -p "$RUN_ROOT"

repos=(
  "$ROOT"
  "$PARENT/motive"
  "$PARENT/rvault"
  "$PARENT/tops"
)

for repo in "${repos[@]}"; do
  [[ -d "$repo" ]] || { echo "error: missing repository: $repo" >&2; exit 1; }
  name=$(basename "$repo")
  echo "RUN single-file repo=$name"
  CBM_ZOVA_VALIDATION_REPO="$repo" \
  CBM_ZOVA_SINGLE_FILE_RUN_ROOT="$RUN_ROOT/$name" \
    bash "$ROOT/scripts/zova-single-file-validation.sh"
done

echo "PASS: single-file multi-repository slice reports are under $RUN_ROOT"
