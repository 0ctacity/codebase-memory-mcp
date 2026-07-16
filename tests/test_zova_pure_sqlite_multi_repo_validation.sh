#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-pure-sqlite-multi-repo-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-pure-multi.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$TMP/bin"
for repo in tops motive rvault CBM; do
  mkdir -p "$TMP/$repo"
  git -C "$TMP/$repo" init -q
  printf '%s\n' "$repo" >"$TMP/$repo/file"
  git -C "$TMP/$repo" add file
  git -C "$TMP/$repo" -c user.name=Codex -c user.email=codex@example.invalid commit -qm init
done
printf build >"$TMP/bin/build"
printf runner >"$TMP/bin/test-runner"
chmod +x "$TMP/bin/build" "$TMP/bin/test-runner"

cat >"$TMP/bin/build-once" <<'SH'
#!/usr/bin/env bash
echo build >>"$CBM_FAKE_LOG"
SH
cat >"$TMP/bin/repository" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
stage="$CBM_ZOVA_PURE_REPOSITORY:$CBM_ZOVA_PURE_ATTEMPT"
echo "$stage" >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != "$stage" ]] || exit 9
mkdir -p "$CBM_ZOVA_PURE_RUN_ROOT"
cat >"$CBM_ZOVA_PURE_RUN_ROOT/repository-report.json" <<JSON
{"schema_version":1,"baseline_route":"pure_sqlite","repository":"$CBM_ZOVA_PURE_REPOSITORY","attempt":$CBM_ZOVA_PURE_ATTEMPT,"passed":true,"states":[{"name":"full","baseline_route":"pure_sqlite","state_workload":"full_pipeline","passed":true,"storage":{"compat_zova_bytes":0}},{"name":"incremental","baseline_route":"pure_sqlite","state_workload":"changed_state_full_pipeline","passed":true,"storage":{"compat_zova_bytes":0}}]}
JSON
SH
cat >"$TMP/bin/compare" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
echo compare >>"$CBM_FAKE_LOG"
output= markdown=
while [[ $# -gt 0 ]]; do
  case "$1" in --output) output=$2; shift 2;; --markdown) markdown=$2; shift 2;; *) shift;; esac
done
printf '{"passed":true}\n' >"$output"
printf '# comparison\n' >"$markdown"
SH
chmod +x "$TMP/bin/"*
printf '{"passed":true}\n' >"$TMP/section9.json"

CBM_FAKE_LOG="$TMP/log" CBM_ZOVA_PURE_ROOT="$TMP/output" \
CBM_ZOVA_PURE_TOPS_REPO="$TMP/tops" CBM_ZOVA_PURE_MOTIVE_REPO="$TMP/motive" \
CBM_ZOVA_PURE_RVAULT_REPO="$TMP/rvault" CBM_ZOVA_PURE_CBM_REPO="$TMP/CBM" \
CBM_ZOVA_PURE_BUILD_BINARY="$TMP/bin/build" \
CBM_ZOVA_PURE_TEST_RUNNER="$TMP/bin/test-runner" \
CBM_ZOVA_PURE_BUILD_ONCE="$TMP/bin/build-once" \
CBM_ZOVA_PURE_REPOSITORY_RUNNER="$TMP/bin/repository" \
CBM_ZOVA_PURE_COMPARE="$TMP/bin/compare" \
CBM_ZOVA_SECTION9_AGGREGATE="$TMP/section9.json" "$SCRIPT"

expected='build
tops:1
tops:2
tops:3
motive:1
motive:2
motive:3
rvault:1
rvault:2
rvault:3
CBM:1
CBM:2
CBM:3
compare'
[[ $(cat "$TMP/log") == "$expected" ]] || { cat "$TMP/log" >&2; fail "stage order wrong"; }
[[ -s "$TMP/output/comparison.json" && -s "$TMP/output/comparison.md" ]] ||
  fail "comparison outputs missing"

: >"$TMP/fail-log"
set +e
CBM_FAKE_FAIL_STAGE=tops:2 CBM_FAKE_LOG="$TMP/fail-log" \
CBM_ZOVA_PURE_ROOT="$TMP/fail-output" \
CBM_ZOVA_PURE_TOPS_REPO="$TMP/tops" CBM_ZOVA_PURE_MOTIVE_REPO="$TMP/motive" \
CBM_ZOVA_PURE_RVAULT_REPO="$TMP/rvault" CBM_ZOVA_PURE_CBM_REPO="$TMP/CBM" \
CBM_ZOVA_PURE_BUILD_BINARY="$TMP/bin/build" CBM_ZOVA_PURE_TEST_RUNNER="$TMP/bin/test-runner" \
CBM_ZOVA_PURE_BUILD_ONCE="$TMP/bin/build-once" \
CBM_ZOVA_PURE_REPOSITORY_RUNNER="$TMP/bin/repository" \
CBM_ZOVA_PURE_COMPARE="$TMP/bin/compare" \
CBM_ZOVA_SECTION9_AGGREGATE="$TMP/section9.json" "$SCRIPT" >/dev/null 2>&1
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "failed TOPS attempt unexpectedly passed"
! rg -q '^motive:' "$TMP/fail-log" || fail "larger repository ran after TOPS failure"

echo "PASS: pure SQLite multi-repository validation"
