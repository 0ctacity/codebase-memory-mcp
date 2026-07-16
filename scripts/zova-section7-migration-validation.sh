#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-$ROOT}
RUN_ROOT=${CBM_ZOVA_SECTION7_RUN_ROOT:-$ROOT/build/zova-single-file-multi/$(basename "$REPO")}

echo "SECTION 7: repository=$(basename "$REPO")"
CBM_ZOVA_VALIDATION_REPO="$REPO" \
CBM_ZOVA_SINGLE_FILE_RUN_ROOT="$RUN_ROOT" \
  bash "$ROOT/scripts/zova-single-file-validation.sh"

