#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)

usage() {
  cat >&2 <<'EOF'
usage: scripts/zova-operations.sh ACTION [ACTION FLAGS]

Runs one shared cbm.zova operation and appends --json automatically.

Environment:
  CBM_ZOVA_OPERATIONS_BINARY       executable (default: build/c/codebase-memory-mcp)
  CBM_ZOVA_OPERATIONS_CACHE_DIR    isolated database cache root
  CBM_ZOVA_OPERATIONS_RUN_DIR      retained report/run root
  CBM_ZOVA_OPERATIONS_SKIP_BUILD=1 reuse the existing executable
EOF
  exit 2
}

[[ $# -ge 1 ]] || usage
ACTION=$1
shift
case "$ACTION" in
  status|backup|restore|export-database|import-database|export-workspace|import-workspace|delete-workspace|compact|health|recover-workspace) ;;
  *) usage ;;
esac

CACHE_DIR=${CBM_ZOVA_OPERATIONS_CACHE_DIR:-${CBM_CACHE_DIR:-${HOME:?}/.cache/codebase-memory-mcp}}
RUN_ROOT=${CBM_ZOVA_OPERATIONS_RUN_DIR:-"$ROOT/build/zova-operations"}
BINARY=${CBM_ZOVA_OPERATIONS_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
mkdir -p "$CACHE_DIR" "$RUN_ROOT/runs"
CACHE_DIR=$(cd "$CACHE_DIR" && pwd -P)
RUN_ROOT=$(cd "$RUN_ROOT" && pwd -P)
LOCK_DIR="$RUN_ROOT/.operations.lock"
LOCK_HELD=0

cleanup() {
  if [[ "$LOCK_HELD" -eq 1 ]]; then
    rmdir "$LOCK_DIR" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "error: another Zova operation is already running under $RUN_ROOT" >&2
  exit 1
fi
LOCK_HELD=1

bash "$ROOT/scripts/zova-disk-guard.sh" "$RUN_ROOT"
if [[ "${CBM_ZOVA_OPERATIONS_SKIP_BUILD:-0}" != "1" ]]; then
  echo "BUILD: pinned Zova executable" >&2
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
fi
[[ -x "$BINARY" ]] || { echo "error: CBM executable not found: $BINARY" >&2; exit 1; }

RUN_DIR=$(mktemp -d "$RUN_ROOT/runs/run.XXXXXX")
REPORT_TMP=$(mktemp "$RUN_DIR/.operation.json.XXXXXX")
REPORT="$RUN_DIR/operation.json"
COMMAND_STDERR="$RUN_DIR/command.stderr.log"

echo "ACTION: $ACTION" >&2
echo "DATABASE: $CACHE_DIR/cbm.zova" >&2
set +e
HOME="${HOME:?}" CBM_CACHE_DIR="$CACHE_DIR" \
  "$BINARY" zova-ops "$ACTION" "$@" --json >"$REPORT_TMP" 2>"$COMMAND_STDERR"
COMMAND_RC=$?
set -e
mv "$REPORT_TMP" "$REPORT"
cat "$COMMAND_STDERR" >&2
echo "REPORT: $REPORT" >&2
cat "$REPORT"
exit "$COMMAND_RC"
