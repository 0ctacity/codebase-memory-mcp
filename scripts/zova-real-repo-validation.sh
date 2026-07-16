#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
REPO=${CBM_ZOVA_VALIDATION_REPO:-$ROOT}
if [[ ! -d "$REPO" ]]; then
  echo "error: repository directory does not exist: $REPO" >&2
  exit 1
fi
REPO=$(cd "$REPO" && pwd -P)
if [[ ! -e "$REPO/.git" ]]; then
  echo "error: repository is not a Git checkout: $REPO" >&2
  exit 1
fi

cd "$ROOT"

# CBM_ZOVA_VALIDATION_RUN_DIR is the stable validation root retained for
# compatibility. Every invocation receives a fresh child below runs/; latest is
# updated only after the whole validation succeeds.
RUN_ROOT=${CBM_ZOVA_VALIDATION_RUN_DIR:-"$ROOT/build/zova-real-repo"}
RUN_ROOT=$(mkdir -p "$RUN_ROOT" && cd "$RUN_ROOT" && pwd -P)
RUNS_DIR="$RUN_ROOT/runs"
LATEST_LINK="$RUN_ROOT/latest"
# One validation lock spans every repository. A multi-repo run is deliberately
# sequential, and an independently started run must not overlap it.
LOCK_DIR="${CBM_ZOVA_VALIDATION_LOCK_DIR:-$ROOT/build/zova-real-repo/.validation.lock}"
RUN_DIR=""
HOME_DIR=""
CACHE_DIR=""
REPORT=""
GRAPH_REPORT=""
GRAPH_MCP_REPORT=""
GRAPH_REGISTRY=""
INDEX_JSON=""
INDEX_STDERR=""
INDEX_PID=""
INDEX_WORKER_PIDS=""
LOCK_HELD=0
UNREAPED_INDEX=0

BINARY="${CBM_ZOVA_REAL_BINARY:-$ROOT/build/c/codebase-memory-mcp}"
TEST_RUNNER="${CBM_ZOVA_REAL_TEST_RUNNER:-$ROOT/build/c/test-runner}"
QUIET_TIMEOUT_S="${CBM_ZOVA_VALIDATION_QUIET_TIMEOUT_S:-${CBM_INDEX_WORKER_TIMEOUT_S:-900}}"
TERMINATE_GRACE_S=10

case "$QUIET_TIMEOUT_S" in
  ''|*[!0-9]*)
    echo "error: CBM_ZOVA_VALIDATION_QUIET_TIMEOUT_S must be a positive integer" >&2
    exit 1
    ;;
esac
if (( QUIET_TIMEOUT_S <= 0 )); then
  echo "error: CBM_ZOVA_VALIDATION_QUIET_TIMEOUT_S must be a positive integer" >&2
  exit 1
fi

# Fail before allocating an index/cache when the machine is already close to
# full. Successful runs compact themselves; failed runs retain diagnostics.
bash "$ROOT/scripts/zova-disk-guard.sh" "$RUN_ROOT"

now_s() {
  date +%s
}

file_mtime_s() {
  local path="$1"
  if stat -f %m "$path" >/dev/null 2>&1; then
    stat -f %m "$path"
  else
    stat -c %Y "$path"
  fi
}

lock_owner_pid() {
  local key="$1"
  [[ -f "$LOCK_DIR/owner" ]] || return 1
  sed -n "s/^${key}=//p" "$LOCK_DIR/owner" | head -n 1
}

lock_live_pid() {
  local pid
  for key in guard_pid runner_pid; do
    pid=$(lock_owner_pid "$key" || true)
    if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
      printf '%s\n' "$pid"
      return 0
    fi
  done
  return 1
}

write_lock_owner() {
  [[ "$LOCK_HELD" -eq 1 ]] || return 0
  {
    printf 'runner_pid=%s\n' "$$"
    printf 'guard_pid=%s\n' "${1:-}"
    printf 'index_pid=%s\n' "$INDEX_PID"
    printf 'worker_pids=%s\n' "${INDEX_WORKER_PIDS// /,}"
    printf 'repo=%s\n' "$REPO"
    printf 'run_dir=%s\n' "$RUN_DIR"
    printf 'started_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "$LOCK_DIR/owner"
}

refresh_index_workers() {
  [[ -n "$INDEX_PID" ]] || return 0
  local pid workers known
  workers=$(ps -ax -o pid= -o ppid= | awk -v parent="$INDEX_PID" '$2 == parent { print $1 }')
  for pid in $workers; do
    known=0
    local existing
    for existing in $INDEX_WORKER_PIDS; do
      [[ "$existing" == "$pid" ]] && known=1 && break
    done
    [[ "$known" -eq 1 ]] || INDEX_WORKER_PIDS="${INDEX_WORKER_PIDS:+$INDEX_WORKER_PIDS }$pid"
  done
}

live_tracked_index_pid() {
  local pid
  if [[ -n "$INDEX_PID" ]] && kill -0 "$INDEX_PID" 2>/dev/null; then
    printf '%s\n' "$INDEX_PID"
    return 0
  fi
  for pid in $INDEX_WORKER_PIDS; do
    if kill -0 "$pid" 2>/dev/null; then
      printf '%s\n' "$pid"
      return 0
    fi
  done
  return 1
}

acquire_lock() {
  mkdir -p "$(dirname "$LOCK_DIR")"
  while ! mkdir "$LOCK_DIR" 2>/dev/null; do
    local live_pid
    live_pid=$(lock_live_pid || true)
    if [[ -n "$live_pid" ]]; then
      echo "error: validation run already active (pid $live_pid)" >&2
      [[ -f "$LOCK_DIR/owner" ]] && cat "$LOCK_DIR/owner" >&2
      return 1
    fi

    # The owner is gone. Archive its metadata rather than deleting it before a
    # replacement lock exists; this leaves a useful post-mortem trail.
    local stale="$(dirname "$LOCK_DIR")/$(basename "$LOCK_DIR").stale.$$.${RANDOM}"
    if mv "$LOCK_DIR" "$stale" 2>/dev/null; then
      echo "warning: reclaimed stale validation lock: $stale" >&2
    fi
  done
  LOCK_HELD=1
  write_lock_owner
}

release_lock() {
  if [[ "$LOCK_HELD" -eq 1 && "$UNREAPED_INDEX" -eq 0 ]]; then
    rm -rf "$LOCK_DIR"
    LOCK_HELD=0
  fi
}

capture_stall_diagnostics() {
  [[ -n "$RUN_DIR" ]] || return 0
  {
    echo "reason=no_progress_or_unreaped_index"
    echo "runner_pid=$$"
    echo "index_pid=$INDEX_PID"
    echo "repo=$REPO"
    date -u +%Y-%m-%dT%H:%M:%SZ
    if [[ -n "$INDEX_PID" ]]; then
      ps -p "$INDEX_PID" -o pid,ppid,stat,etime,command || true
    fi
    [[ -n "$CACHE_DIR" ]] && ls -la "$CACHE_DIR/logs" 2>/dev/null || true
  } > "$RUN_DIR/runner-stall.txt"
}

stop_tracked_index() {
  refresh_index_workers
  local live_pid
  live_pid=$(live_tracked_index_pid || true)
  [[ -n "$live_pid" ]] || return 0

  echo "warning: stopping validation index process tree rooted at ${INDEX_PID:-$live_pid}" >&2
  local pid
  for pid in $INDEX_WORKER_PIDS "$INDEX_PID"; do
    [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null || true
  done
  local deadline=$(( $(now_s) + TERMINATE_GRACE_S ))
  while :; do
    live_pid=$(live_tracked_index_pid || true)
    [[ -n "$live_pid" ]] && (( $(now_s) < deadline )) || break
    sleep 1
  done

  live_pid=$(live_tracked_index_pid || true)
  if [[ -n "$live_pid" ]]; then
    for pid in $INDEX_WORKER_PIDS "$INDEX_PID"; do
      [[ -n "$pid" ]] && kill -KILL "$pid" 2>/dev/null || true
    done
    deadline=$(( $(now_s) + TERMINATE_GRACE_S ))
    while :; do
      live_pid=$(live_tracked_index_pid || true)
      [[ -n "$live_pid" ]] && (( $(now_s) < deadline )) || break
      sleep 1
    done
  fi

  live_pid=$(live_tracked_index_pid || true)
  if [[ -n "$live_pid" ]]; then
    # A macOS U-state process can survive SIGKILL. Its PID becomes the lock
    # guard, deliberately blocking a new benchmark until it actually exits.
    UNREAPED_INDEX=1
    capture_stall_diagnostics
    write_lock_owner "$live_pid"
    echo "error: index process $live_pid did not exit; lock retained at $LOCK_DIR" >&2
    return 1
  fi

  [[ -n "$INDEX_PID" ]] && wait "$INDEX_PID" 2>/dev/null || true
  INDEX_PID=""
  INDEX_WORKER_PIDS=""
  write_lock_owner
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if live_tracked_index_pid >/dev/null; then
    stop_tracked_index || true
  fi
  release_lock
  exit "$rc"
}

on_signal() {
  local signal="$1"
  echo "warning: received $signal; cancelling validation" >&2
  stop_tracked_index || true
  exit 130
}

trap cleanup EXIT
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM

preflight_existing_indexers() {
  local matches
  matches=$(ps -ax -o pid= -o command= | awk -v repo="$REPO" '
    index($0, repo) &&
    ($0 ~ /cli[[:space:]]+index_repository/ ||
     $0 ~ /cli[[:space:]]+--index-worker[[:space:]]+index_repository/) { print }
  ')
  if [[ -n "$matches" ]]; then
    echo "error: existing index process for this repository; refusing to overlap:" >&2
    printf '%s\n' "$matches" >&2
    return 1
  fi
}

monitor_index() {
  local worker_log="$CACHE_DIR/logs/.worker-$INDEX_PID.log"
  local last_activity now observed_mtime=""
  last_activity=$(now_s)

  while kill -0 "$INDEX_PID" 2>/dev/null; do
    refresh_index_workers
    if [[ -f "$worker_log" ]]; then
      local current_mtime
      current_mtime=$(file_mtime_s "$worker_log" || true)
      if [[ -n "$current_mtime" && "$current_mtime" != "$observed_mtime" ]]; then
        observed_mtime="$current_mtime"
        last_activity=$(now_s)
      fi
    fi

    now=$(now_s)
    if (( now - last_activity >= QUIET_TIMEOUT_S )); then
      echo "error: indexer made no worker-log progress for ${QUIET_TIMEOUT_S}s" >&2
      capture_stall_diagnostics
      stop_tracked_index || true
      return 124
    fi
    sleep 2
  done

  local rc=0
  wait "$INDEX_PID" || rc=$?
  refresh_index_workers
  if live_tracked_index_pid >/dev/null; then
    stop_tracked_index || return 125
  fi
  INDEX_PID=""
  INDEX_WORKER_PIDS=""
  write_lock_owner
  return "$rc"
}

publish_latest() {
  local tmp_link="$RUN_ROOT/.latest.$$.tmp"
  rm -f "$tmp_link"
  ln -s "$RUN_DIR" "$tmp_link"

  # A pre-safety run used latest as a real directory. Preserve it once, then
  # switch to a symlink so future publication is a single rename.
  if [[ -d "$LATEST_LINK" && ! -L "$LATEST_LINK" ]]; then
    local legacy="$RUN_ROOT/latest.legacy.$(basename "$RUN_DIR")"
    mv "$LATEST_LINK" "$legacy"
    echo "ARCHIVED LEGACY LATEST: $legacy"
  fi
  python3 - "$tmp_link" "$LATEST_LINK" <<'PY'
import os
import sys
os.replace(sys.argv[1], sys.argv[2])
PY
}

compact_successful_run() {
  [[ "${CBM_ZOVA_VALIDATION_COMPACT:-1}" == "1" ]] || return 0
  # Reports are self-contained JSON. Keep them, but discard large logs, MCP
  # samples (unless profiling was explicitly requested), and disposable
  # databases after a successful validation. Failure paths intentionally keep
  # all diagnostics untouched.
  rm -f "$INDEX_JSON" "$INDEX_STDERR"
  if [[ "${CBM_ZOVA_REAL_GRAPH_PROFILE:-0}" != "1" ]]; then
    rm -f "$GRAPH_MCP_SAMPLES"
  fi
  if [[ "${CBM_ZOVA_VALIDATION_PRESERVE_CACHE:-0}" != "1" ]]; then
    rm -rf "$CACHE_DIR" "$HOME_DIR"
  fi
}

acquire_lock
preflight_existing_indexers

mkdir -p "$RUNS_DIR"
RUN_DIR=$(mktemp -d "$RUNS_DIR/run.XXXXXX")
write_lock_owner
HOME_DIR="$RUN_DIR/home"
CACHE_DIR=${CBM_ZOVA_VALIDATION_CACHE_DIR:-"$RUN_DIR/cache"}
REPORT="$RUN_DIR/report.json"
GRAPH_REPORT="$RUN_DIR/graph-report.json"
GRAPH_MCP_REPORT="$RUN_DIR/graph-mcp-report.json"
GRAPH_MCP_SAMPLES="$RUN_DIR/graph-mcp-samples.jsonl"
GRAPH_REGISTRY="$RUN_DIR/cbm-workspace-registry.zova"
INDEX_JSON="$RUN_DIR/index.json"
INDEX_STDERR="$RUN_DIR/index.stderr.log"
mkdir -p "$HOME_DIR" "$CACHE_DIR"
CACHE_DIR=$(cd "$CACHE_DIR" && pwd -P)

if [[ "${CBM_ZOVA_VALIDATION_SKIP_BUILD:-0}" != "1" ]]; then
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
fi

echo "INDEX: $(basename "$REPO") ($REPO)"
START_MS=$(python3 -c 'import time; print(int(time.time() * 1000))')
CBM_INDEX_WORKER_TIMEOUT_S="$QUIET_TIMEOUT_S" \
HOME="$HOME_DIR" \
CBM_CACHE_DIR="$CACHE_DIR" \
CBM_ZOVA_MODE=graph_read \
  "$BINARY" cli index_repository --repo-path "$REPO" --mode full > "$INDEX_JSON" 2> "$INDEX_STDERR" &
INDEX_PID=$!
write_lock_owner

if monitor_index; then
  :
else
  INDEX_RC=$?
  echo "error: index validation failed (exit $INDEX_RC); diagnostics kept in $RUN_DIR" >&2
  exit "$INDEX_RC"
fi
END_MS=$(python3 -c 'import time; print(int(time.time() * 1000))')
INDEX_MS=$((END_MS - START_MS))

DBS=()
while IFS= read -r db_path; do
  DBS+=("$db_path")
done < <(find "$CACHE_DIR" -maxdepth 1 -type f -name '*.db' | sort)
if [[ "${#DBS[@]}" -ne 1 ]]; then
  echo "error: expected exactly one fresh project DB in $CACHE_DIR, found ${#DBS[@]}" >&2
  printf '%s\n' "${DBS[@]}" >&2
  exit 1
fi

DB="${DBS[0]}"
ZOVA="${DB%.db}.zova"
if [[ ! -f "$ZOVA" ]]; then
  echo "error: expected Zova sidecar next to DB: $ZOVA" >&2
  exit 1
fi

PROJECT=$(python3 - "$INDEX_JSON" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as f:
    d = json.load(f)
if "content" in d:
    d = json.loads(d["content"][0]["text"])
print(d.get("project") or "")
PY
)

if [[ -z "$PROJECT" ]]; then
  echo "error: index response did not include a project name" >&2
  cat "$INDEX_JSON" >&2
  exit 1
fi

CBM_ZOVA_REAL_DB="$DB" \
CBM_ZOVA_REAL_ZOVA="$ZOVA" \
CBM_ZOVA_REAL_PROJECT="$PROJECT" \
CBM_ZOVA_REAL_REPO="$REPO" \
CBM_ZOVA_REAL_REPORT="$REPORT" \
CBM_ZOVA_REAL_INDEX_MS="$INDEX_MS" \
CBM_CACHE_DIR="$CACHE_DIR" \
CBM_ZOVA_MODE=i8_vectors \
  "$TEST_RUNNER" zova_real_repo

CBM_ZOVA_REAL_DB="$DB" \
CBM_ZOVA_REAL_ZOVA="$ZOVA" \
CBM_ZOVA_REAL_PROJECT="$PROJECT" \
CBM_ZOVA_REAL_REPO="$REPO" \
CBM_ZOVA_REAL_GRAPH_REGISTRY="$GRAPH_REGISTRY" \
CBM_ZOVA_REAL_GRAPH_REPORT="$GRAPH_REPORT" \
CBM_ZOVA_REAL_GRAPH_MCP_REPORT="$GRAPH_MCP_REPORT" \
CBM_ZOVA_REAL_GRAPH_MCP_SAMPLES="$GRAPH_MCP_SAMPLES" \
CBM_CACHE_DIR="$CACHE_DIR" \
CBM_ZOVA_GRAPH_PROFILE="${CBM_ZOVA_REAL_GRAPH_PROFILE:-0}" \
CBM_ZOVA_MODE=graph_read \
  "$TEST_RUNNER" zova_graph_real_repo

if [[ ! -f "$REPORT" ]]; then
  echo "error: expected validation report was not written: $REPORT" >&2
  exit 1
fi
if [[ ! -f "$GRAPH_REPORT" ]]; then
  echo "error: expected graph validation report was not written: $GRAPH_REPORT" >&2
  exit 1
fi
if [[ ! -f "$GRAPH_MCP_REPORT" ]]; then
  echo "error: expected MCP graph validation report was not written: $GRAPH_MCP_REPORT" >&2
  exit 1
fi

if [[ "${CBM_ZOVA_REAL_GRAPH_PROFILE:-0}" == "1" && ! -s "$GRAPH_MCP_SAMPLES" ]]; then
  echo "error: expected detailed graph MCP samples were not written: $GRAPH_MCP_SAMPLES" >&2
  exit 1
fi

compact_successful_run
publish_latest
echo "RUN: $RUN_DIR"
echo "REPORT: $LATEST_LINK/report.json"
echo "GRAPH REPORT: $LATEST_LINK/graph-report.json"
echo "GRAPH MCP REPORT: $LATEST_LINK/graph-mcp-report.json"
if [[ "${CBM_ZOVA_REAL_GRAPH_PROFILE:-0}" == "1" ]]; then
  echo "GRAPH MCP SAMPLES: $LATEST_LINK/graph-mcp-samples.jsonl"
fi
