#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-clean-private-tmp.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

CACHE="$TMP/cache"
mkdir -p "$CACHE/private-tmp-directory.zova"
touch "$CACHE/private-tmp-one.zova"
touch "$CACHE/private-tmp-one.zova-wal"
touch "$CACHE/private-tmp-one.zova-shm"
touch "$CACHE/private-tmp-not-a-database.txt"
touch "$CACHE/keep-private-tmp.zova"
touch "$CACHE/private-tmp-directory.zova/keep.zova"

CBM_CACHE_DIR="$CACHE" bash "$ROOT/scripts/zova-clean-private-tmp.sh" \
  > "$TMP/output.log"

[[ ! -e "$CACHE/private-tmp-one.zova" ]]
[[ ! -e "$CACHE/private-tmp-one.zova-wal" ]]
[[ ! -e "$CACHE/private-tmp-one.zova-shm" ]]
[[ -e "$CACHE/private-tmp-not-a-database.txt" ]]
[[ -e "$CACHE/keep-private-tmp.zova" ]]
[[ -e "$CACHE/private-tmp-directory.zova/keep.zova" ]]
[[ "$(wc -l < "$TMP/output.log" | tr -d ' ')" == "3" ]]

echo "PASS: private-tmp Zova databases and companions are removed safely"
