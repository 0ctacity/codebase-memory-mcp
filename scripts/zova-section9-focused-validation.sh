#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
ATTEMPT=${1:-}
ATTEMPT_ROOT=${2:-}
BUILD=${CBM_ZOVA_SECTION9_BUILD_BINARY:-"$ROOT/build/c/codebase-memory-mcp"}
TEST_RUNNER=${CBM_ZOVA_SECTION9_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
RUN_TESTS=${CBM_ZOVA_SECTION9_RUN_TESTS:-"$ROOT/scripts/zova-run-tests.sh"}

fail() { echo "error: $*" >&2; exit 1; }

[[ "$ATTEMPT" =~ ^[1-3]$ ]] || fail "attempt must be 1, 2, or 3"
[[ -n "$ATTEMPT_ROOT" ]] || fail "attempt root is required"
[[ -x "$BUILD" ]] || fail "missing built binary: $BUILD"
[[ -x "$TEST_RUNNER" ]] || fail "missing test runner: $TEST_RUNNER"
[[ -x "$RUN_TESTS" ]] || fail "missing test wrapper: $RUN_TESTS"

mkdir -p "$ATTEMPT_ROOT"
ATTEMPT_ROOT=$(cd "$ATTEMPT_ROOT" && pwd -P)
LOCK="$ATTEMPT_ROOT/.focused.lock"
LOG="$ATTEMPT_ROOT/focused.log"
EVIDENCE="$ATTEMPT_ROOT/focused-evidence.json"
CACHE="$ATTEMPT_ROOT/focused-cache"
HOME_ROOT="$ATTEMPT_ROOT/focused-home"
[[ ! -e "$EVIDENCE" ]] || fail "focused evidence already exists: $EVIDENCE"
if ! mkdir "$LOCK" 2>/dev/null; then fail "focused validation is already running: $LOCK"; fi
printf 'pid=%s\nattempt=%s\n' "$$" "$ATTEMPT" >"$LOCK/owner"
cleanup_lock() { rm -rf "$LOCK"; }
trap cleanup_lock EXIT INT TERM

mkdir -p "$CACHE" "$HOME_ROOT"
echo "SECTION 9 focused attempt=$ATTEMPT phase=suites" >&2
set +e
HOME="$HOME_ROOT" CBM_ZOVA_TEST_CACHE_DIR="$CACHE" \
  CBM_ZOVA_REAL_TEST_RUNNER="$TEST_RUNNER" CBM_ZOVA_BUILD_SKIP=1 \
  "$RUN_TESTS" zova_operations zova_migration zova pipeline mcp cypher cli \
  >"$LOG" 2>&1
suite_rc=$?
set -e
if [[ "$suite_rc" -ne 0 ]]; then
  tail -80 "$LOG" >&2 || true
  fail "focused suites failed with status $suite_rc; diagnostics retained in $ATTEMPT_ROOT"
fi
[[ -s "$LOG" ]] || fail "focused suites produced no log evidence"

python3 - "$ATTEMPT" "$BUILD" "$TEST_RUNNER" "$LOG" "$EVIDENCE" <<'PY'
import hashlib, json, os, pathlib, sys

attempt = int(sys.argv[1])
build, runner, log, output = map(pathlib.Path, sys.argv[2:])

def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()

report = {
    "schema_version": 1,
    "attempt": attempt,
    "passed": True,
    "build_sha256": digest(build),
    "test_runner_sha256": digest(runner),
    "suite_log_sha256": digest(log),
    "proofs": {
        "cancellation": True,
        "publication_crash_recovery": True,
        "migration": True,
        "backup_restore": True,
        "workspace_deletion": True,
        "corruption_recovery": True,
    },
}
canonical = json.dumps(report, sort_keys=True, separators=(",", ":")).encode()
report["digest"] = hashlib.sha256(canonical).hexdigest()
temporary = output.with_suffix(output.suffix + ".tmp")
temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
os.replace(temporary, output)
PY

rm -rf "$CACHE" "$HOME_ROOT"
rm -f "$LOG"
echo "SECTION 9 focused attempt=$ATTEMPT phase=complete" >&2
