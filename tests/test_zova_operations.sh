#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
WRAPPER="$ROOT/scripts/zova-operations.sh"
BINARY=${CBM_ZOVA_OPERATIONS_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-operations-shell.XXXXXX")
TMP=$(cd "$TMP" && pwd -P)
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
json_get() {
  python3 - "$1" "$2" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as report:
    value = json.load(report)
for part in sys.argv[2].split("."):
    value = value[part]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("null")
else:
    print(value)
PY
}

run_op() {
  local out=$1 err=$2
  shift 2
  HOME="$HOME_ROOT" CBM_ZOVA_OPERATIONS_BINARY="$BINARY" \
    CBM_ZOVA_OPERATIONS_CACHE_DIR="$CACHE" CBM_ZOVA_OPERATIONS_RUN_DIR="$RUN_ROOT" \
    CBM_ZOVA_OPERATIONS_SKIP_BUILD=1 CBM_ZOVA_MIN_FREE_GB=1 \
    "$WRAPPER" "$@" >"$out" 2>"$err"
}

[[ -x "$BINARY" ]] || fail "missing built binary: $BINARY"
REPO="$TMP/repository with spaces"
CACHE="$TMP/cache with spaces"
RUN_ROOT="$TMP/run root with spaces"
HOME_ROOT="$TMP/home"
mkdir -p "$REPO" "$CACHE" "$RUN_ROOT" "$HOME_ROOT"
printf '%s\n' 'int helper(void) { return 1; } int main(void) { return helper(); }' >"$REPO/main.c"

HOME="$HOME_ROOT" CBM_CACHE_DIR="$CACHE" CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL=1 \
  CBM_INDEX_SUPERVISOR=0 "$BINARY" cli index_repository --repo-path "$REPO" --mode full \
  >"$TMP/index.json" 2>"$TMP/index.err" || {
    cat "$TMP/index.err" >&2
    fail "tiny flagged fixture indexing failed"
  }
[[ -s "$CACHE/cbm.zova" ]] || fail "flagged indexing did not create cbm.zova"
WORKSPACE=$(python3 - "$CACHE/cbm.zova" <<'PY'
import sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
try:
    print(con.execute("SELECT workspace_id FROM cbm_workspace_registry").fetchone()[0])
finally:
    con.close()
PY
)

run_op "$TMP/status.json" "$TMP/status.err" status
[[ $(json_get "$TMP/status.json" success) == true ]] || fail "status failed"
[[ $(json_get "$TMP/status.json" schema_version) == 6 ]] || fail "wrong schema version"
python3 - "$TMP/status.json" <<'PY' || fail "deterministic JSON contract changed"
import json, sys
with open(sys.argv[1], encoding="utf-8") as report:
    value = json.load(report)
expected = [
    "action", "success", "code", "name", "reason", "schema_version",
    "archive_version", "database_path", "input_path", "output_path", "repo_path",
    "workspace_id", "generation", "database_bytes", "wal_bytes", "free_bytes",
    "reclaimable_bytes", "page_size", "page_count", "freelist_count", "elapsed_ms",
    "health_classification",
]
assert list(value) == expected, list(value)
for key in ("code", "schema_version", "archive_version", "generation", "database_bytes",
            "wal_bytes", "free_bytes", "reclaimable_bytes", "page_size", "page_count",
            "freelist_count"):
    assert isinstance(value[key], int) and not isinstance(value[key], bool), (key, value[key])
assert isinstance(value["elapsed_ms"], (int, float)) and not isinstance(value["elapsed_ms"], bool)
PY
run_op "$TMP/health.json" "$TMP/health.err" health --workspace-id "$WORKSPACE"
[[ $(json_get "$TMP/health.json" health_classification) == healthy ]] || fail "health failed"

BACKUP="$TMP/backup with spaces.zova"
DATABASE_ARCHIVE="$TMP/database archive with spaces"
WORKSPACE_ARCHIVE="$TMP/workspace archive with spaces"
run_op "$TMP/backup.json" "$TMP/backup.err" backup --output "$BACKUP"
[[ -s "$BACKUP" ]] || fail "backup missing"
run_op "$TMP/export-db.json" "$TMP/export-db.err" export-database --output "$DATABASE_ARCHIVE"
[[ -s "$DATABASE_ARCHIVE/manifest.json" ]] || fail "database archive missing"
run_op "$TMP/export-ws.json" "$TMP/export-ws.err" export-workspace \
  --workspace-id "$WORKSPACE" --output "$WORKSPACE_ARCHIVE"
[[ -s "$WORKSPACE_ARCHIVE/manifest.json" ]] || fail "workspace archive missing"
run_op "$TMP/compact.json" "$TMP/compact.err" compact
[[ $(json_get "$TMP/compact.json" success) == true ]] || fail "compact failed"
run_op "$TMP/recover-noop.json" "$TMP/recover-noop.err" recover-workspace \
  --workspace-id "$WORKSPACE" --repo-path "$REPO" || {
    cat "$TMP/recover-noop.err" >&2
    cat "$TMP/recover-noop.json" >&2
    fail "healthy recovery command failed"
  }
[[ $(json_get "$TMP/recover-noop.json" name) == noop ]] || fail "healthy recovery was not noop"

# Destructive actions refuse absent or mismatched confirmation.
if run_op "$TMP/restore-refused.json" "$TMP/restore-refused.err" restore --input "$BACKUP"; then
  fail "restore without confirmation succeeded"
fi
[[ $(json_get "$TMP/restore-refused.json" code) == -1 ]] || fail "restore parser refusal changed"
if run_op "$TMP/import-refused.json" "$TMP/import-refused.err" import-database \
     --input "$DATABASE_ARCHIVE"; then
  fail "database import without confirmation succeeded"
fi
if run_op "$TMP/delete-refused.json" "$TMP/delete-refused.err" delete-workspace \
     --workspace-id "$WORKSPACE" \
     --confirm-workspace w1_00000000000000000000000000000000; then
  fail "workspace deletion with mismatched confirmation succeeded"
fi
[[ $(json_get "$TMP/delete-refused.json" name) == confirmation_required ]] ||
  fail "delete mismatch did not map to confirmation_required"
BACKUP_ALIAS="$TMP/backup symlink.zova"
ln -s "$BACKUP" "$BACKUP_ALIAS"
if run_op "$TMP/restore-symlink.json" "$TMP/restore-symlink.err" restore \
     --input "$BACKUP_ALIAS" --confirm-replace; then
  fail "restore accepted a symlinked backup"
fi
[[ $(json_get "$TMP/restore-symlink.json" name) == invalid ]] ||
  fail "symlinked backup refusal changed"

# Replacement interruption retains a valid live database and retry converges.
if CBM_ZOVA_TEST_FAIL_PHASE=restore_after_temp_verification \
     run_op "$TMP/restore-fault.json" "$TMP/restore-fault.err" restore \
       --input "$BACKUP" --confirm-replace; then
  fail "faulted restore unexpectedly succeeded"
fi
run_op "$TMP/status-after-fault.json" "$TMP/status-after-fault.err" status
[[ $(json_get "$TMP/status-after-fault.json" success) == true ]] ||
  fail "restore fault damaged live database"
run_op "$TMP/restore.json" "$TMP/restore.err" restore --input "$BACKUP" --confirm-replace
[[ $(json_get "$TMP/restore.json" success) == true ]] || fail "restore retry failed"
run_op "$TMP/import-db.json" "$TMP/import-db.err" import-database \
  --input "$DATABASE_ARCHIVE" --confirm-replace
[[ $(json_get "$TMP/import-db.json" success) == true ]] || fail "database import failed"

run_op "$TMP/delete.json" "$TMP/delete.err" delete-workspace --workspace-id "$WORKSPACE" \
  --confirm-workspace "$WORKSPACE"
[[ $(json_get "$TMP/delete.json" success) == true ]] || fail "workspace deletion failed"
run_op "$TMP/import-ws.json" "$TMP/import-ws.err" import-workspace --input "$WORKSPACE_ARCHIVE"
[[ $(json_get "$TMP/import-ws.json" success) == true ]] || fail "workspace import failed"

# Wrapper lock contention refuses before running the command or changing reports.
mkdir "$RUN_ROOT/.operations.lock"
if run_op "$TMP/locked.json" "$TMP/locked.err" status; then
  fail "lock contention unexpectedly succeeded"
fi
grep -F "another Zova operation" "$TMP/locked.err" >/dev/null ||
  fail "lock contention reason missing"
rmdir "$RUN_ROOT/.operations.lock"

find "$RUN_ROOT/runs" -name operation.json -type f | grep . >/dev/null ||
  fail "retained compact JSON reports missing"
grep -F "ACTION: status" "$TMP/status.err" >/dev/null || fail "stderr progress missing"
echo "PASS: zova operations wrapper"
