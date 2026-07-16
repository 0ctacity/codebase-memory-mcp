#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)

usage() {
  cat >&2 <<'EOF'
usage: scripts/zova-migrate-repo.sh ACTION REPOSITORY [--confirm-workspace WORKSPACE_ID]
       scripts/zova-migrate-repo.sh REPOSITORY [RUN_ROOT]

Actions:
  migrate
  status
  rollback
  cleanup --confirm-workspace WORKSPACE_ID

Environment:
  CBM_ZOVA_MIGRATE_BINARY    CBM executable (default: build/c/codebase-memory-mcp)
  CBM_ZOVA_MIGRATE_RUN_DIR   run root (overrides the legacy positional RUN_ROOT)
  CBM_ZOVA_MIGRATE_SKIP_BUILD=1  reuse the existing executable
  ZOVA_ROOT                   local Zova checkout passed to Zig

The historical REPOSITORY [RUN_ROOT] form is an alias for migrate.
EOF
  exit 2
}

[[ $# -ge 1 ]] || usage

ACTION=$1
POSITIONAL_RUN_ROOT=""
CONFIRM_ARGS=()
case "$ACTION" in
  migrate|status|rollback)
    [[ $# -eq 2 ]] || usage
    REPO=$2
    ;;
  cleanup)
    [[ $# -eq 4 && "$3" == "--confirm-workspace" ]] || usage
    REPO=$2
    CONFIRM_ARGS=(--confirm-workspace "$4")
    ;;
  *)
    ACTION=migrate
    [[ $# -le 2 ]] || usage
    REPO=$1
    POSITIONAL_RUN_ROOT=${2:-}
    ;;
esac

RUN_ROOT=${CBM_ZOVA_MIGRATE_RUN_DIR:-${POSITIONAL_RUN_ROOT:-"$ROOT/build/zova-migrate"}}
mkdir -p "$RUN_ROOT"
RUN_ROOT=$(cd "$RUN_ROOT" && pwd -P)
RUNS_DIR="$RUN_ROOT/runs"
LOCK_DIR="$RUN_ROOT/.migration.lock"
BINARY=${CBM_ZOVA_MIGRATE_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
LOCK_HELD=0

cleanup_lock() {
  if [[ "$LOCK_HELD" -eq 1 ]]; then
    rmdir "$LOCK_DIR" 2>/dev/null || true
  fi
}
trap cleanup_lock EXIT INT TERM

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "error: another Zova migration is already running under $RUN_ROOT" >&2
  exit 1
fi
LOCK_HELD=1

mkdir -p "$RUNS_DIR"
RUN_DIR=$(mktemp -d "$RUNS_DIR/run.XXXXXX")
REPORT="$RUN_DIR/migration.json"
REPORT_TMP=$(mktemp "$RUN_DIR/.migration.json.XXXXXX")
COMMAND_STDERR="$RUN_DIR/command.stderr.log"

if [[ "${CBM_ZOVA_MIGRATE_SKIP_BUILD:-0}" != "1" ]]; then
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
fi
if [[ ! -x "$BINARY" ]]; then
  echo "error: CBM executable not found: $BINARY" >&2
  exit 1
fi

echo "ACTION: $ACTION" >&2
echo "REPOSITORY: $REPO" >&2

COMMAND=("$BINARY" zova-migrate "$ACTION" --repo-path "$REPO")
if [[ "$ACTION" == "cleanup" ]]; then
  COMMAND+=("${CONFIRM_ARGS[@]}")
fi
COMMAND+=(--json)

set +e
"${COMMAND[@]}" >"$REPORT_TMP" 2>"$COMMAND_STDERR"
COMMAND_RC=$?
set -e

mv "$REPORT_TMP" "$REPORT"
cat "$COMMAND_STDERR" >&2

STATE=$(python3 - "$REPORT" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as report:
        value = json.load(report).get("state")
except (OSError, AttributeError, json.JSONDecodeError):
    value = None
print(value if value is not None else "unknown")
PY
)
echo "STATE: $STATE" >&2
echo "REPORT: $REPORT" >&2
cat "$REPORT"
exit "$COMMAND_RC"
