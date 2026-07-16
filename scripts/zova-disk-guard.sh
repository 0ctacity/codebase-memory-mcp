#!/usr/bin/env bash
set -euo pipefail

TARGET=${1:-.}
MIN_FREE_GB=${CBM_ZOVA_MIN_FREE_GB:-8}
case "$MIN_FREE_GB" in
  ''|*[!0-9]*)
    echo "error: CBM_ZOVA_MIN_FREE_GB must be a positive integer" >&2
    exit 2
    ;;
esac
if (( MIN_FREE_GB <= 0 )); then
  echo "error: CBM_ZOVA_MIN_FREE_GB must be a positive integer" >&2
  exit 2
fi

if [[ -n "${CBM_ZOVA_AVAILABLE_KB_OVERRIDE:-}" ]]; then
  AVAILABLE_KB=$CBM_ZOVA_AVAILABLE_KB_OVERRIDE
else
  AVAILABLE_KB=$(df -Pk "$TARGET" | awk 'NR == 2 { print $4 }')
fi
case "$AVAILABLE_KB" in
  ''|*[!0-9]*)
    echo "error: could not determine available disk space for $TARGET" >&2
    exit 2
    ;;
esac

REQUIRED_KB=$((MIN_FREE_GB * 1024 * 1024))
if (( AVAILABLE_KB < REQUIRED_KB )); then
  available_gb=$(awk -v kb="$AVAILABLE_KB" 'BEGIN { printf "%.2f", kb / 1024 / 1024 }')
  echo "error: refusing to start a Zova validation with only ${available_gb} GiB free" >&2
  echo "Required free space: ${MIN_FREE_GB} GiB (override with CBM_ZOVA_MIN_FREE_GB)." >&2
  exit 1
fi

