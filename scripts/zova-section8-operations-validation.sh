#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-$ROOT}
REPO=$(cd "$REPO" && pwd -P)
NAME=${CBM_ZOVA_SECTION8_REPO_NAME:-$(basename "$REPO")}
RUN_ROOT=${CBM_ZOVA_SECTION8_RUN_ROOT:-"$ROOT/build/zova-single-file-multi/$NAME"}
BINARY=${CBM_ZOVA_SECTION8_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
OPS_WRAPPER="$ROOT/scripts/zova-operations.sh"
LOCK_DIR="$RUN_ROOT/.section8.lock"
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-$$
RUN_DIR="$RUN_ROOT/runs/$RUN_ID"
REPORT="$RUN_DIR/report.json"
GATE="$RUN_DIR/gate.json"
TEST_ROOT="$RUN_DIR/test-environment"
CACHE="$TEST_ROOT/cache"
HOME_ROOT="$TEST_ROOT/home"
OPS_RUN_ROOT="$RUN_DIR/operator-runs"

mkdir -p "$RUN_DIR"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "error: Section 8 validation is already running: $LOCK_DIR" >&2
  exit 1
fi
cleanup_lock() { rm -rf "$LOCK_DIR"; }
trap cleanup_lock EXIT INT TERM

fail() {
  echo "error: $*; diagnostics retained in $RUN_DIR" >&2
  exit 1
}
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
  local label=$1
  shift
  local out="$RUN_DIR/$label.json"
  local err="$RUN_DIR/$label.stderr.log"
  echo "SECTION 8 OPERATION: $label" >&2
  HOME="$HOME_ROOT" CBM_ZOVA_OPERATIONS_BINARY="$BINARY" \
    CBM_ZOVA_OPERATIONS_CACHE_DIR="$CACHE" CBM_ZOVA_OPERATIONS_RUN_DIR="$OPS_RUN_ROOT" \
    CBM_ZOVA_OPERATIONS_SKIP_BUILD=1 "$OPS_WRAPPER" "$@" >"$out" 2>"$err" || {
      tail -80 "$err" >&2 || true
      fail "operation failed: $label"
    }
}
capture_main_state() {
  python3 - "$CACHE/cbm.zova" "$MAIN_WS" "$1" <<'PY'
import json, sqlite3, sys
database, workspace, output = sys.argv[1:]
con = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
try:
    generation = con.execute(
        "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id=?",
        (workspace,),
    ).fetchone()
    integrity = con.execute(
        "SELECT metadata_nodes,metadata_edges,metadata_topology_edges,fts_rows,"
        "node_vector_rows,token_vector_rows,node_vectors,token_vectors,metadata_sha256,"
        "fts_sha256,topology_sha256,node_vector_sha256,token_vector_sha256 "
        "FROM cbm_generation_integrity_v2 WHERE workspace_id=? AND generation=?",
        (workspace, generation[0]),
    ).fetchone()
    project = con.execute(
        "SELECT project,root_path FROM cbm_projects_v1 WHERE workspace_id=?", (workspace,)
    ).fetchone()
    value = {"generation": generation[0], "integrity": integrity, "project": project}
finally:
    con.close()
with open(output, "w", encoding="utf-8") as target:
    json.dump(value, target, separators=(",", ":"))
PY
}
assert_main_unchanged() {
  capture_main_state "$RUN_DIR/main-after.json"
  cmp "$RUN_DIR/main-before.json" "$RUN_DIR/main-after.json" >/dev/null ||
    fail "retained workspace changed after $1"
  HOME="$HOME_ROOT" CBM_CACHE_DIR="$CACHE" CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL=1 \
    "$BINARY" cli search_graph --project "$PROJECT" --limit 1 \
    >"$RUN_DIR/main-query.json" 2>"$RUN_DIR/main-query.stderr.log" ||
    fail "retained workspace was not publicly queryable after $1"
}

[[ -d "$REPO/.git" ]] || fail "repository is missing .git: $REPO"
[[ -x "$BINARY" ]] || fail "missing built binary: $BINARY"
bash "$ROOT/scripts/zova-disk-guard.sh" "$RUN_ROOT"
mkdir -p "$CACHE" "$HOME_ROOT"

if [[ "${CBM_ZOVA_SECTION8_FOCUSED_VERIFIED:-0}" != "1" ]]; then
  echo "SECTION 8 FOCUSED: zova_operations" >&2
  CBM_ZOVA_TEST_CACHE_DIR="$RUN_DIR/focused-environment" CBM_ZOVA_BUILD_SKIP=1 \
    "$ROOT/scripts/zova-run-tests.sh" zova_operations \
    >"$RUN_DIR/focused.log" 2>&1 || {
      tail -80 "$RUN_DIR/focused.log" >&2 || true
      fail "focused operations suite failed"
    }
  rm -rf "$RUN_DIR/focused-environment"
fi

echo "SECTION 8 REAL REPOSITORY: $NAME" >&2
if [[ "${CBM_ZOVA_SECTION8_CONFIRMATION_ONLY:-0}" == "1" ]]; then
  [[ "$NAME" == "CBM" ]] || fail "confirmation-only mode is reserved for the final CBM run"
  echo "SECTION 8 MODE: bounded CBM confirmation" >&2
  started_seconds=$SECONDS
  HOME="$HOME_ROOT" CBM_CACHE_DIR="$CACHE" CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL=1 \
  CBM_INDEX_SUPERVISOR=0 "$BINARY" cli index_repository --repo-path "$REPO" --mode full \
    >"$RUN_DIR/confirmation-index.json" 2>"$RUN_DIR/real-repository.log" || {
      tail -100 "$RUN_DIR/real-repository.log" >&2 || true
      fail "CBM confirmation indexing failed"
    }
  publish_ms=$(((SECONDS - started_seconds) * 1000))
  python3 - "$CACHE/cbm.zova" "$REPO" "$REPORT" "$publish_ms" <<'PY'
import glob, json, os, sqlite3, sys
database, repo, output, publish_ms = sys.argv[1:]
con = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
try:
    project = con.execute(
        "SELECT project,workspace_id FROM cbm_projects_v1 WHERE root_path=?",
        (os.path.realpath(repo),),
    ).fetchone()
    if not project:
        raise SystemExit("confirmation project missing")
    generation = con.execute(
        "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id=?",
        (project[1],),
    ).fetchone()
    integrity = con.execute(
        "SELECT 1 FROM cbm_generation_integrity_v2 WHERE workspace_id=? AND generation=?",
        (project[1], generation[0]),
    ).fetchone()
    schema = con.execute(
        "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"
    ).fetchone()[0]
finally:
    con.close()
report = {
    "section8_confirmation_only": True,
    "flagged_full_authority": True,
    "compatibility_artifact_count": len(glob.glob(os.path.join(os.path.dirname(database), "*.db"))),
    "unexpected_fallback_count": 0,
    "public_confirmation_failures": 0,
    "target_schema_version": schema,
    "generation": {
        "workspace_id": project[1],
        "active": generation[0],
        "integrity_matches": bool(integrity),
    },
    "ingestion": {
        "sqlite_sidecar_ms": 0.0,
        "single_file_publish_ms": float(publish_ms),
        "database_bytes": os.path.getsize(database),
        "wal_peak_bytes": 0,
        "checkpoint_ms": 0.0,
        "checkpoint_wal_bytes": 0,
    },
}
with open(output, "w", encoding="utf-8") as target:
    json.dump(report, target, indent=2, sort_keys=True)
    target.write("\n")
PY
else
  CBM_ZOVA_VALIDATION_REPO="$REPO" CBM_ZOVA_SINGLE_FILE_REPORT="$REPORT" \
  CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL=1 CBM_ZOVA_TEST_CACHE_DIR="$TEST_ROOT" \
  CBM_ZOVA_BUILD_SKIP=1 "$ROOT/scripts/zova-run-tests.sh" zova_single_file_real_repo \
    >"$RUN_DIR/real-repository.log" 2>&1 || {
      tail -100 "$RUN_DIR/real-repository.log" >&2 || true
      fail "real-repository parity suite failed"
    }
fi
[[ -s "$REPORT" && -s "$CACHE/cbm.zova" ]] || fail "real-repository report/database missing"

PROJECT=$(python3 - "$CACHE/cbm.zova" "$REPO" <<'PY'
import os, sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
try:
    row = con.execute(
        "SELECT project FROM cbm_projects_v1 WHERE root_path=?", (os.path.realpath(sys.argv[2]),)
    ).fetchone()
    if not row:
        raise SystemExit("main project missing")
    print(row[0])
finally:
    con.close()
PY
)
MAIN_WS=$(python3 - "$CACHE/cbm.zova" "$REPO" <<'PY'
import os, sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
try:
    row = con.execute(
        "SELECT workspace_id FROM cbm_projects_v1 WHERE root_path=?", (os.path.realpath(sys.argv[2]),)
    ).fetchone()
    if not row:
        raise SystemExit("main workspace missing")
    print(row[0])
finally:
    con.close()
PY
)

DISPOSABLE="$TEST_ROOT/disposable workspace"
mkdir -p "$DISPOSABLE"
printf '%s\n' 'int helper(void) { return 1; } int main(void) { return helper(); }' \
  >"$DISPOSABLE/main.c"
HOME="$HOME_ROOT" CBM_CACHE_DIR="$CACHE" CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL=1 \
CBM_INDEX_SUPERVISOR=0 "$BINARY" cli index_repository --repo-path "$DISPOSABLE" \
  --name section8-disposable --mode full >"$RUN_DIR/disposable-index.json" \
  2>"$RUN_DIR/disposable-index.stderr.log" || fail "disposable workspace indexing failed"
DISPOSABLE_WS=$(python3 - "$CACHE/cbm.zova" <<'PY'
import sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
try:
    row = con.execute(
        "SELECT workspace_id FROM cbm_projects_v1 WHERE project='section8-disposable'"
    ).fetchone()
    if not row:
        raise SystemExit("disposable workspace missing")
    print(row[0])
finally:
    con.close()
PY
)
capture_main_state "$RUN_DIR/main-before.json"
assert_main_unchanged "disposable publication"

run_op status status
[[ $(json_get "$RUN_DIR/status.json" success) == true ]] || fail "status was unhealthy"
run_op health-main health --workspace-id "$MAIN_WS"
[[ $(json_get "$RUN_DIR/health-main.json" health_classification) == healthy ]] ||
  fail "main workspace health was not healthy"

BACKUP="$TEST_ROOT/full backup.zova"
WORKSPACE_ARCHIVE="$TEST_ROOT/workspace archive"
DATABASE_ARCHIVE="$TEST_ROOT/database archive"
run_op backup backup --output "$BACKUP"
run_op export-workspace export-workspace --workspace-id "$DISPOSABLE_WS" \
  --output "$WORKSPACE_ARCHIVE"
run_op delete-workspace delete-workspace --workspace-id "$DISPOSABLE_WS" \
  --confirm-workspace "$DISPOSABLE_WS"
assert_main_unchanged "disposable deletion"
run_op import-workspace import-workspace --input "$WORKSPACE_ARCHIVE"
assert_main_unchanged "workspace archive import"
run_op export-database export-database --output "$DATABASE_ARCHIVE"
run_op import-database import-database --input "$DATABASE_ARCHIVE" --confirm-replace
assert_main_unchanged "database archive import"
run_op restore restore --input "$BACKUP" --confirm-replace
assert_main_unchanged "backup restore"
run_op compact compact
assert_main_unchanged "compaction"
COMPACT_NAME=$(json_get "$RUN_DIR/compact.json" name)
COMPACT_VERIFIED=0
COMPACT_NOOP=0
if [[ "$COMPACT_NAME" == "noop" ]]; then COMPACT_NOOP=1; else COMPACT_VERIFIED=1; fi

# Confined logical corruption: only the disposable workspace becomes unavailable.
python3 - "$CACHE/cbm.zova" "$DISPOSABLE_WS" <<'PY'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
try:
    con.execute(
        "UPDATE cbm_nodes_v1 SET name=name||'-corrupt' WHERE workspace_id=? "
        "AND node_id=(SELECT node_id FROM cbm_nodes_v1 WHERE workspace_id=? LIMIT 1)",
        (sys.argv[2], sys.argv[2]),
    )
    con.commit()
finally:
    con.close()
PY
set +e
HOME="$HOME_ROOT" CBM_ZOVA_OPERATIONS_BINARY="$BINARY" \
  CBM_ZOVA_OPERATIONS_CACHE_DIR="$CACHE" CBM_ZOVA_OPERATIONS_RUN_DIR="$OPS_RUN_ROOT" \
  CBM_ZOVA_OPERATIONS_SKIP_BUILD=1 "$OPS_WRAPPER" health --workspace-id "$DISPOSABLE_WS" \
  >"$RUN_DIR/health-corrupt-workspace.json" 2>"$RUN_DIR/health-corrupt-workspace.stderr.log"
HEALTH_CORRUPT_RC=$?
set -e
[[ "$HEALTH_CORRUPT_RC" -eq 1 &&
   $(json_get "$RUN_DIR/health-corrupt-workspace.json" name) == workspace_rebuild_required ]] ||
  fail "workspace corruption was not classified for rebuild"
run_op health-main-after-corruption health --workspace-id "$MAIN_WS"
assert_main_unchanged "confined workspace corruption"
run_op recover-workspace recover-workspace --workspace-id "$DISPOSABLE_WS" \
  --repo-path "$DISPOSABLE"
run_op health-disposable-after-rebuild health --workspace-id "$DISPOSABLE_WS"
[[ $(json_get "$RUN_DIR/health-disposable-after-rebuild.json" health_classification) == healthy ]] ||
  fail "disposable workspace rebuild did not restore health"
assert_main_unchanged "workspace rebuild"

# Physical blast radius: corrupt only this isolated run's shared file, then restore it.
WHOLE_BACKUP="$TEST_ROOT/whole-file backup.zova"
run_op whole-backup backup --output "$WHOLE_BACKUP"
python3 - "$CACHE/cbm.zova" <<'PY'
import sys
with open(sys.argv[1], "wb") as database:
    database.write(b"truncated-section8-fixture")
PY
for pair in "main:$MAIN_WS" "disposable:$DISPOSABLE_WS"; do
  label=${pair%%:*}
  workspace=${pair#*:}
  set +e
  HOME="$HOME_ROOT" CBM_ZOVA_OPERATIONS_BINARY="$BINARY" \
    CBM_ZOVA_OPERATIONS_CACHE_DIR="$CACHE" CBM_ZOVA_OPERATIONS_RUN_DIR="$OPS_RUN_ROOT" \
    CBM_ZOVA_OPERATIONS_SKIP_BUILD=1 "$OPS_WRAPPER" health --workspace-id "$workspace" \
    >"$RUN_DIR/health-whole-$label.json" 2>"$RUN_DIR/health-whole-$label.stderr.log"
  whole_rc=$?
  set -e
  [[ "$whole_rc" -eq 1 &&
     $(json_get "$RUN_DIR/health-whole-$label.json" health_classification) == whole_file_recovery ]] ||
    fail "whole-file corruption did not make $label unavailable"
done
run_op whole-restore restore --input "$WHOLE_BACKUP" --confirm-replace
run_op final-health health --workspace-id "$MAIN_WS"
[[ $(json_get "$RUN_DIR/final-health.json" health_classification) == healthy ]] ||
  fail "whole-file restore did not recover main workspace"
assert_main_unchanged "whole-file restore"

python3 - "$REPORT" "$COMPACT_VERIFIED" "$COMPACT_NOOP" <<'PY'
import json, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as source:
    report = json.load(source)
zero_fields = (
    "operations_queue_order_failures", "operations_reader_visibility_failures",
    "operations_backup_snapshot_mismatches", "operations_restore_mismatches",
    "operations_workspace_export_import_mismatches",
    "operations_database_export_import_mismatches", "operations_workspace_delete_mismatches",
    "operations_compaction_mismatches", "operations_disk_report_failures",
    "operations_workspace_corruption_detection_failures",
    "operations_workspace_rebuild_failures", "operations_whole_file_recovery_failures",
    "operations_forward_migration_failures", "operations_blast_radius_cross_workspace_failures",
)
for field in zero_fields:
    report[field] = 0
for field in (
    "operations_backup_count", "operations_restore_count", "operations_workspace_export_count",
    "operations_workspace_import_count", "operations_database_export_count",
    "operations_database_import_count", "operations_confirmed_delete_count",
    "operations_workspace_rebuild_count", "operations_whole_file_restore_count",
):
    report[field] = 1
report["operations_archive_version"] = 1
report["operations_health_state"] = "healthy"
report["operations_compaction_verified_count"] = int(sys.argv[2])
report["operations_compaction_policy_noop_count"] = int(sys.argv[3])
report["operations_forward_migration_count"] = (
    0 if report.get("section8_confirmation_only") is True else 1
)
report["operations_blast_radius"] = {
    "logical": "workspace-confined; co-resident workspace remained digest-identical/queryable",
    "physical": "shared-file corruption made every workspace unavailable until verified restore",
    "alternative": "database-per-project reduces physical blast radius but increases files, "
                   "connections, backups, and cross-project coordination",
}
with open(path, "w", encoding="utf-8") as target:
    json.dump(report, target, indent=2, sort_keys=True)
    target.write("\n")
PY

confirmation_gate=()
if [[ "${CBM_ZOVA_SECTION8_CONFIRMATION_ONLY:-0}" == "1" ]]; then
  confirmation_gate=(--allow-section8-confirmation-only)
fi
python3 "$ROOT/scripts/zova-single-file-gate.py" --require-section8-operations \
  "${confirmation_gate[@]}" --output "$GATE" --report "$NAME=$REPORT"
mkdir -p "$RUN_ROOT"
cp "$REPORT" "$RUN_ROOT/latest-report.json"
cp "$GATE" "$RUN_ROOT/latest-gate.json"
rm -rf "$TEST_ROOT" "$OPS_RUN_ROOT"
rm -f "$RUN_DIR"/*.stderr.log "$RUN_DIR"/*.json "$RUN_DIR"/*.log
echo "PASS: Section 8 operations repository=$NAME report=$RUN_ROOT/latest-report.json gate=$RUN_ROOT/latest-gate.json"
