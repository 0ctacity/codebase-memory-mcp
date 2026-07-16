#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-section9-focused-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-section9-focused.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
expect_fail() {
  if "$@" >/dev/null 2>&1; then fail "command unexpectedly succeeded: $*"; fi
}

mkdir -p "$TMP/bin"
printf 'build\n' >"$TMP/bin/build"
printf 'runner\n' >"$TMP/bin/test-runner"
chmod +x "$TMP/bin/build" "$TMP/bin/test-runner"

cat >"$TMP/bin/run-tests" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >"$CBM_FAKE_RECORD.args"
printf '%s\n%s\n' "$HOME" "$CBM_ZOVA_TEST_CACHE_DIR" >"$CBM_FAKE_RECORD.env"
if [[ "${CBM_FAKE_QUIET:-0}" != "1" ]]; then echo "fake focused suites passed"; fi
exit "${CBM_FAKE_EXIT:-0}"
SH
chmod +x "$TMP/bin/run-tests"

invoke() {
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_FAKE_RECORD="$TMP/record" "$SCRIPT" "$@"
}

expect_fail invoke 0 "$TMP/invalid-zero"
expect_fail invoke 4 "$TMP/invalid-four"
expect_fail invoke nope "$TMP/invalid-text"

expect_fail env \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/missing" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_FAKE_RECORD="$TMP/record" "$SCRIPT" 1 "$TMP/missing-build"

expect_fail env \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_FAKE_RECORD="$TMP/record" CBM_FAKE_EXIT=7 \
  "$SCRIPT" 1 "$TMP/nonzero"
[[ -s "$TMP/nonzero/focused.log" ]] || fail "failed suite log was not retained"
[[ ! -e "$TMP/nonzero/focused-evidence.json" ]] || fail "failed suite published evidence"

expect_fail env \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_FAKE_RECORD="$TMP/record" CBM_FAKE_QUIET=1 \
  "$SCRIPT" 1 "$TMP/empty-log"

mkdir -p "$TMP/locked/.focused.lock"
expect_fail invoke 1 "$TMP/locked"

invoke 2 "$TMP/success"
[[ -s "$TMP/success/focused-evidence.json" ]] || fail "focused evidence missing"
[[ ! -e "$TMP/success/focused-cache" ]] || fail "successful focused cache retained"
[[ ! -e "$TMP/success/focused.log" ]] || fail "successful focused log retained"
[[ ! -e "$TMP/success/.focused.lock" ]] || fail "focused lock retained"
[[ $(cat "$TMP/record.args") == \
  "zova_operations zova_migration zova pipeline mcp cypher cli" ]] ||
  fail "focused suite order changed"

python3 - "$TMP/success/focused-evidence.json" "$TMP/bin/build" \
  "$TMP/bin/test-runner" "$TMP/record.env" "$TMP/success" <<'PY'
import hashlib, json, pathlib, sys
report_path, build_path, runner_path, env_path, root = map(pathlib.Path, sys.argv[1:])
data = json.loads(report_path.read_text())
assert data["schema_version"] == 1
assert data["attempt"] == 2
assert data["passed"] is True
assert set(data["proofs"]) == {
    "cancellation", "publication_crash_recovery", "migration",
    "backup_restore", "workspace_deletion", "corruption_recovery",
}
assert all(data["proofs"].values())
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
assert data["build_sha256"] == digest(build_path)
assert data["test_runner_sha256"] == digest(runner_path)
payload = dict(data); actual = payload.pop("digest")
canonical = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
assert actual == hashlib.sha256(canonical).hexdigest()
home, cache = env_path.read_text().splitlines()
assert pathlib.Path(home).resolve().is_relative_to(root.resolve())
assert pathlib.Path(cache).resolve().is_relative_to(root.resolve())
PY

echo "PASS: Zova Section 9 focused validation"
