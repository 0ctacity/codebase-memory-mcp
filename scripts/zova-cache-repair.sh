#!/usr/bin/env bash
set -euo pipefail

for cache_root in "$@"; do
  manifest_dir="$cache_root/h"
  if [[ -d "$manifest_dir" ]]; then
    find "$manifest_dir" -type f -size 0 -delete
  fi
done
