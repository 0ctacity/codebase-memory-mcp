#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-pure-sqlite-repository-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-pure-repo.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

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
exit 0
SH
cat >"$TMP/bin/run-tests" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == zova_single_file_promotion_real_repo ]]
[[ "$CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE" == 1 ]]
state=$CBM_ZOVA_PROMOTION_STATE
probe=$(sed -n 's/.*return \([0-9][0-9]*\).*/\1/p' \
  "$CBM_ZOVA_VALIDATION_REPO/section9_promotion_probe/probe.go")
printf '%s|%s|%s\n' "$state" "$probe" "$CBM_ZOVA_TEST_CACHE_DIR" >>"$CBM_FAKE_RECORD"
workload=full_pipeline
[[ "$state" != incremental ]] || workload=changed_state_full_pipeline
cat >"$CBM_ZOVA_PROMOTION_REPORT" <<JSON
{"name":"$state","baseline_route":"pure_sqlite","state_workload":"$workload","passed":true,"storage":{"compat_zova_bytes":0}}
JSON
SH
chmod +x "$TMP/bin/disk-guard" "$TMP/bin/run-tests"

CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
CBM_ZOVA_PURE_REPOSITORY=tops CBM_ZOVA_PURE_ATTEMPT=1 \
CBM_ZOVA_PURE_RUN_ROOT="$TMP/run" \
CBM_ZOVA_PURE_BUILD_BINARY="$TMP/bin/build" \
CBM_ZOVA_PURE_TEST_RUNNER="$TMP/bin/test-runner" \
CBM_ZOVA_PURE_RUN_TESTS="$TMP/bin/run-tests" \
CBM_ZOVA_PURE_DISK_GUARD="$TMP/bin/disk-guard" \
CBM_FAKE_RECORD="$TMP/record" "$SCRIPT"

[[ -s "$TMP/run/repository-report.json" ]] || fail "repository report missing"
[[ ! -e "$TMP/run/clone" && ! -e "$TMP/run/state-cache" ]] || fail "success state retained"
[[ $(cat "$TMP/record") == $'full|7|'* ]] || fail "full state missing"
[[ $(tail -1 "$TMP/record") == incremental\|99\|* ]] || fail "changed state missing"
python3 - "$TMP/run/repository-report.json" <<'PY'
import json, pathlib, sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d["schema_version"] == 1
assert d["baseline_route"] == "pure_sqlite"
assert d["repository"] == "tops" and d["attempt"] == 1 and d["passed"] is True
assert [s["state_workload"] for s in d["states"]] == [
    "full_pipeline", "changed_state_full_pipeline"]
assert all(s["storage"]["compat_zova_bytes"] == 0 for s in d["states"])
PY

echo "PASS: pure SQLite repository validation"
