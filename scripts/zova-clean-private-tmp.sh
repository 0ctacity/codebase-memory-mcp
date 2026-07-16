#!/usr/bin/env bash
set -euo pipefail

CACHE_DIR=${CBM_CACHE_DIR:-"$HOME/.cache/codebase-memory-mcp"}

if [[ ! -d "$CACHE_DIR" ]]; then
  exit 0
fi

find "$CACHE_DIR" -maxdepth 1 -type f -name 'private-tmp*.zova*' \
  -print -delete
