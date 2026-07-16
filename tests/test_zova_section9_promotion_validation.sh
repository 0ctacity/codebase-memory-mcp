#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-section9-promotion-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-section9-repo.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
expect_fail() { if "$@" >/dev/null 2>&1; then fail "unexpected success: $*"; fi; }

mkdir -p "$TMP/source" "$TMP/bin"
git -C "$TMP/source" init -q
printf 'package source\nfunc Existing() int { return 1 }\n' >"$TMP/source/source.go"
git -C "$TMP/source" add source.go
git -C "$TMP/source" -c user.name=Codex -c user.email=codex@example.invalid commit -qm fixture
printf 'build\n' >"$TMP/bin/build"
printf 'runner\n' >"$TMP/bin/test-runner"
chmod +x "$TMP/bin/build" "$TMP/bin/test-runner"

cat >"$TMP/bin/disk-guard" <<'SH'
#!/usr/bin/env bash
echo "$1" >>"$CBM_FAKE_RECORD.disk"
exit "${CBM_FAKE_DISK_EXIT:-0}"
SH
cat >"$TMP/bin/run-tests" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
state=$CBM_ZOVA_PROMOTION_STATE
probe=$(sed -n 's/.*return \([0-9][0-9]*\).*/\1/p' \
  "$CBM_ZOVA_VALIDATION_REPO/section9_promotion_probe/probe.go")
printf '%s|%s|%s\n' "$state" "$probe" "$CBM_ZOVA_TEST_CACHE_DIR" >>"$CBM_FAKE_RECORD.states"
if [[ "$state" == full ]]; then
  python3 - "$CBM_FAKE_SOURCE/source.go" "$CBM_ZOVA_VALIDATION_REPO/source.go" \
    "$CBM_ZOVA_VALIDATION_REPO/.git/objects/info/alternates" "$CBM_FAKE_RECORD.clone" <<'PY'
import os, pathlib, sys
source, clone, alternates, output = map(pathlib.Path, sys.argv[1:])
pathlib.Path(output).write_text(
    f"{os.stat(source).st_ino != os.stat(clone).st_ino}|{alternates.is_file()}\n"
)
PY
fi
[[ "$1" == zova_single_file_promotion_real_repo ]]
cat >"$CBM_ZOVA_PROMOTION_REPORT" <<JSON
{"name":"$state","passed":true,"probe":$probe}
JSON
echo "fake state $state"
SH
cat >"$TMP/bin/gate" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >"$CBM_FAKE_RECORD.gate"
output=
while [[ $# -gt 0 ]]; do
  if [[ "$1" == --output ]]; then output=$2; shift 2; else shift; fi
done
[[ -n "$output" ]]
printf '{"passed":true}\n' >"$output"
exit "${CBM_FAKE_GATE_EXIT:-0}"
SH
chmod +x "$TMP/bin/disk-guard" "$TMP/bin/run-tests" "$TMP/bin/gate"

cat >"$TMP/calibration.json" <<JSON
{"build_sha256":"$(shasum -a 256 "$TMP/bin/build" | awk '{print $1}')","digest":"$(printf 'c%.0s' {1..64})"}
JSON
cat >"$TMP/focused.json" <<JSON
{"digest":"$(printf 'f%.0s' {1..64})"}
JSON

invoke() {
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=tops CBM_ZOVA_SECTION9_ATTEMPT=1 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$1" \
  CBM_ZOVA_SECTION9_CALIBRATION="$TMP/calibration.json" \
  CBM_ZOVA_SECTION9_FOCUSED_EVIDENCE="$TMP/focused.json" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" \
  CBM_ZOVA_SECTION9_DISK_GUARD="$TMP/bin/disk-guard" \
  CBM_FAKE_RECORD="$TMP/record" CBM_FAKE_SOURCE="$TMP/source" "$SCRIPT"
}

mkdir -p "$TMP/not-git"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/not-git" \
  CBM_ZOVA_SECTION9_REPOSITORY=tops CBM_ZOVA_SECTION9_ATTEMPT=1 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/not-git-run" "$SCRIPT"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=wrong CBM_ZOVA_SECTION9_ATTEMPT=1 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/wrong-name" "$SCRIPT"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=tops CBM_ZOVA_SECTION9_ATTEMPT=0 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/wrong-attempt" "$SCRIPT"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=tops CBM_ZOVA_SECTION9_ATTEMPT=1 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/missing-evidence" "$SCRIPT"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=motive CBM_ZOVA_SECTION9_ATTEMPT=0 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/bad-calibration" \
  CBM_ZOVA_SECTION9_CALIBRATION_MODE=1 "$SCRIPT"

expect_fail env CBM_FAKE_DISK_EXIT=1 \
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=tops CBM_ZOVA_SECTION9_ATTEMPT=1 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/disk-fail" \
  CBM_ZOVA_SECTION9_CALIBRATION="$TMP/calibration.json" \
  CBM_ZOVA_SECTION9_FOCUSED_EVIDENCE="$TMP/focused.json" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" \
  CBM_ZOVA_SECTION9_DISK_GUARD="$TMP/bin/disk-guard" CBM_FAKE_RECORD="$TMP/record" "$SCRIPT"

invoke "$TMP/success"
[[ -s "$TMP/success/repository-report.json" ]] || fail "repository report missing"
[[ -s "$TMP/success/repository-gate.json" ]] || fail "repository gate missing"
[[ ! -e "$TMP/success/clone" && ! -e "$TMP/success/state-cache" ]] ||
  fail "successful clone/cache retained"
[[ $(wc -l <"$TMP/record.states" | tr -d ' ') == 2 ]] || fail "wrong state invocation count"
state_full=$(sed -n '1p' "$TMP/record.states")
state_incremental=$(sed -n '2p' "$TMP/record.states")
[[ $state_full == full\|7\|* ]] || fail "full did not precede mutation"
[[ $state_incremental == incremental\|99\|* ]] || fail "incremental mutation missing"
[[ ${state_full#*|*|} == "${state_incremental#*|*|}" ]] || fail "state cache changed"
rg -q -- '--repository-only' "$TMP/record.gate" || fail "repository-only gate missing"
[[ $(cat "$TMP/record.clone") == "True|True" ]] ||
  fail "clone did not use shared/no-hardlinks isolation"
python3 - "$TMP/success/repository-report.json" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
assert d["schema_version"] == 1 and d["repository"] == "tops" and d["attempt"] == 1
assert d["flagged_full_authority"] is True and d["passed"] is True
assert [s["name"] for s in d["states"]] == ["full", "incremental"]
assert [s["probe"] for s in d["states"]] == [7, 99]
PY

expect_fail env CBM_FAKE_GATE_EXIT=9 \
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=tops CBM_ZOVA_SECTION9_ATTEMPT=1 \
  CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/gate-fail" \
  CBM_ZOVA_SECTION9_CALIBRATION="$TMP/calibration.json" \
  CBM_ZOVA_SECTION9_FOCUSED_EVIDENCE="$TMP/focused.json" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" \
  CBM_ZOVA_SECTION9_DISK_GUARD="$TMP/bin/disk-guard" CBM_FAKE_RECORD="$TMP/record2" \
  CBM_FAKE_SOURCE="$TMP/source" "$SCRIPT"
[[ -d "$TMP/gate-fail/clone" && -d "$TMP/gate-fail/state-cache" ]] ||
  fail "failure diagnostics were removed"

CBM_ZOVA_VALIDATION_REPO="$TMP/source" CBM_ZOVA_SECTION9_REPOSITORY=tops \
CBM_ZOVA_SECTION9_ATTEMPT=0 CBM_ZOVA_SECTION9_RUN_ROOT="$TMP/calibration-run" \
CBM_ZOVA_SECTION9_CALIBRATION_MODE=1 \
CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
CBM_ZOVA_SECTION9_RUN_TESTS="$TMP/bin/run-tests" \
CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" \
CBM_ZOVA_SECTION9_DISK_GUARD="$TMP/bin/disk-guard" \
CBM_FAKE_RECORD="$TMP/cal-record" CBM_FAKE_SOURCE="$TMP/source" "$SCRIPT"
python3 - "$TMP/calibration-run/repository-report.json" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
assert d["calibration_mode"] is True and d["attempt"] == 0
assert d["calibration_sha256"] is None and d["focused_evidence_sha256"] is None
PY

echo "PASS: Zova Section 9 repository promotion validation"
