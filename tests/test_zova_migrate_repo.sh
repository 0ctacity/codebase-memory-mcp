#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
WRAPPER="$ROOT/scripts/zova-migrate-repo.sh"
BINARY=${CBM_ZOVA_MIGRATE_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-migrate-shell.XXXXXX")
TMP=$(cd "$TMP" && pwd -P)

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
assert_file() { [[ -s "$1" ]] || fail "expected non-empty file: $1"; }
assert_absent() { [[ ! -e "$1" ]] || fail "expected absent: $1"; }
assert_contains() { grep -F "$2" "$1" >/dev/null || fail "expected '$2' in $1"; }
json_get() {
  python3 - "$1" "$2" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    value = json.load(f)
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

make_repo() {
  local repo=$1
  mkdir -p "$repo/src"
  printf '%s\n' 'int beta(void) { return 2; }' >"$repo/src/beta.c"
  printf '%s\n' 'int beta(void); int alpha(void) { return beta(); }' >"$repo/src/alpha.c"
  git -C "$repo" init -q
  git -C "$repo" add src
  git -C "$repo" -c user.name=test -c user.email=test@example.invalid commit -qm fixture
}

index_legacy() {
  local repo=$1 cache=$2 home=$3
  mkdir -p "$cache" "$home"
  local output="$TMP/index-$(basename "$cache").json"
  if ! HOME="$home" CBM_CACHE_DIR="$cache" CBM_ZOVA_MODE=graph_read \
       CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL=0 \
       CBM_INDEX_SUPERVISOR=0 "$BINARY" cli index_repository \
         --repo-path "$repo" --mode full >"$output" 2>"$output.stderr"; then
    cat "$output.stderr" >&2
    cat "$output" >&2
    fail "legacy fixture indexing failed"
  fi
}

source_db_for_repo() {
  python3 - "$1" "$2" <<'PY'
import os, sqlite3, sys
cache, repo = sys.argv[1:]
repo = os.path.realpath(repo)
matches = []
for name in os.listdir(cache):
    if not name.endswith(".db"):
        continue
    path = os.path.join(cache, name)
    try:
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        try:
            if con.execute("SELECT count(*) FROM projects WHERE root_path=?", (repo,)).fetchone()[0]:
                matches.append(path)
        finally:
            con.close()
    except sqlite3.Error:
        pass
if len(matches) != 1:
    raise SystemExit(f"expected one legacy DB, found {matches}")
print(matches[0])
PY
}

run_action() {
  local action=$1 repo=$2 cache=$3 run_root=$4 out=$5 err=$6
  shift 6
  HOME="$TMP/home" CBM_CACHE_DIR="$cache" CBM_ZOVA_MIGRATE_BINARY="$BINARY" \
    CBM_ZOVA_MIGRATE_SKIP_BUILD=1 CBM_ZOVA_MIGRATE_RUN_DIR="$run_root" \
    "$WRAPPER" "$action" "$repo" "$@" >"$out" 2>"$err"
}

[[ -x "$BINARY" ]] || fail "missing built binary: $BINARY"

REPO="$TMP/repository with spaces"
CACHE="$TMP/cache"
RUN_ROOT="$TMP/run root with spaces"
make_repo "$REPO"
index_legacy "$REPO" "$CACHE" "$TMP/index-home"
SOURCE_DB=$(source_db_for_repo "$CACHE" "$REPO")
SOURCE_ZOVA=${SOURCE_DB%.db}.zova
assert_file "$SOURCE_DB"
assert_file "$SOURCE_ZOVA"
SOURCE_DB_HASH=$(shasum -a 256 "$SOURCE_DB" | awk '{print $1}')
SOURCE_ZOVA_HASH=$(shasum -a 256 "$SOURCE_ZOVA" | awk '{print $1}')

# A real process interruption must leave a report and converge on rerun.
if CBM_ZOVA_TEST_FAIL_PHASE=migration_after_target_publish \
   run_action migrate "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/interrupted.json" \
     "$TMP/interrupted.err"; then
  fail "faulted migrate unexpectedly succeeded"
fi
assert_file "$TMP/interrupted.json"
run_action migrate "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/migrate.json" "$TMP/migrate.err"
[[ $(json_get "$TMP/migrate.json" state) == active ]] || fail "migrate did not activate"
WORKSPACE=$(json_get "$TMP/migrate.json" workspace_id)
GENERATION=$(json_get "$TMP/migrate.json" target_generation)
assert_contains "$TMP/migrate.err" "ACTION: migrate"
assert_contains "$TMP/migrate.err" "REPOSITORY: $REPO"
assert_contains "$TMP/migrate.err" "STATE: active"
assert_contains "$TMP/migrate.err" "REPORT:"
find "$RUN_ROOT" -name migration.json -type f | grep . >/dev/null || fail "missing retained report"

# Restart/no-op and status are non-destructive.
run_action migrate "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/noop.json" "$TMP/noop.err"
[[ $(json_get "$TMP/noop.json" no_op) == true ]] || fail "second migrate was not a no-op"
run_action status "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/status.json" "$TMP/status.err"
[[ $(json_get "$TMP/status.json" state) == active ]] || fail "status not active"
[[ $(shasum -a 256 "$SOURCE_DB" | awk '{print $1}') == "$SOURCE_DB_HASH" ]] ||
  fail "status changed source DB"
[[ $(shasum -a 256 "$SOURCE_ZOVA" | awk '{print $1}') == "$SOURCE_ZOVA_HASH" ]] ||
  fail "status changed source Zova"

# Rollback and reactivation preserve the published generation.
run_action rollback "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/rollback.json" "$TMP/rollback.err"
[[ $(json_get "$TMP/rollback.json" state) == rolled_back ]] || fail "rollback failed"
run_action migrate "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/reactivate.json" "$TMP/reactivate.err"
[[ $(json_get "$TMP/reactivate.json" state) == active ]] || fail "reactivation failed"
[[ $(json_get "$TMP/reactivate.json" target_generation) == "$GENERATION" ]] ||
  fail "reactivation published a new generation"

# Cleanup requires the exact workspace and is the only destructive action.
if run_action cleanup "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/wrong-cleanup.json" \
     "$TMP/wrong-cleanup.err" --confirm-workspace w1_00000000000000000000000000000000; then
  fail "wrong cleanup confirmation succeeded"
fi
assert_file "$SOURCE_DB"
assert_file "$SOURCE_ZOVA"
run_action cleanup "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/cleanup.json" "$TMP/cleanup.err" \
  --confirm-workspace "$WORKSPACE"
[[ $(json_get "$TMP/cleanup.json" state) == retired ]] || fail "cleanup did not retire"
assert_absent "$SOURCE_DB"
assert_absent "$SOURCE_ZOVA"
assert_file "$CACHE/cbm.zova"

# The historical REPOSITORY [RUN_ROOT] form remains a migrate alias.
ALIAS_REPO="$TMP/legacy alias repository"
ALIAS_CACHE="$TMP/legacy-alias-cache"
ALIAS_RUN="$TMP/legacy alias run"
make_repo "$ALIAS_REPO"
index_legacy "$ALIAS_REPO" "$ALIAS_CACHE" "$TMP/alias-home"
HOME="$TMP/home" CBM_CACHE_DIR="$ALIAS_CACHE" CBM_ZOVA_MIGRATE_BINARY="$BINARY" \
  CBM_ZOVA_MIGRATE_SKIP_BUILD=1 "$WRAPPER" "$ALIAS_REPO" "$ALIAS_RUN" \
  >"$TMP/alias.json" 2>"$TMP/alias.err"
[[ $(json_get "$TMP/alias.json" state) == active ]] || fail "legacy alias did not migrate"

# Existing run-root lock refuses concurrent actions without touching reports.
mkdir -p "$RUN_ROOT/.migration.lock"
if run_action status "$REPO" "$CACHE" "$RUN_ROOT" "$TMP/locked.json" "$TMP/locked.err"; then
  fail "lock contention unexpectedly succeeded"
fi
assert_contains "$TMP/locked.err" "another Zova migration"
rmdir "$RUN_ROOT/.migration.lock"

echo "PASS: zova migrate repo wrapper"
