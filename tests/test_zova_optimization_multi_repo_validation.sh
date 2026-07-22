#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-optimization-multi-repo-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-opt-multi.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$TMP/bin" "$TMP/baseline"
for repo in tops deno motive rvault CBM; do
  mkdir -p "$TMP/$repo"
  git -C "$TMP/$repo" init -q
  printf '%s\n' "$repo" >"$TMP/$repo/file"
  git -C "$TMP/$repo" add file
  git -C "$TMP/$repo" -c user.name=Codex -c user.email=codex@example.invalid commit -qm init
done
printf build >"$TMP/bin/build"
printf runner >"$TMP/bin/test-runner"
printf 'source_commit=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n' >"$TMP/manifest.txt"
chmod +x "$TMP/bin/build" "$TMP/bin/test-runner"

cat >"$TMP/bin/build-once" <<'SH'
#!/usr/bin/env bash
echo build >>"$CBM_FAKE_LOG"
SH
cat >"$TMP/bin/focused" <<'SH'
#!/usr/bin/env bash
echo focused >>"$CBM_FAKE_LOG"
SH
cat >"$TMP/bin/repository" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
stage="$CBM_ZOVA_OPTIMIZATION_REPOSITORY:$CBM_ZOVA_OPTIMIZATION_ATTEMPT"
scope=${1:-all}
echo "$stage:$scope" >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != "$stage" ]] || exit 9
mkdir -p "$CBM_ZOVA_OPTIMIZATION_RUN_ROOT"
if [[ $scope == index-only ]]; then
  attempt=$CBM_ZOVA_OPTIMIZATION_ATTEMPT
  pure=$((attempt * 10))
  single=$((attempt * 20))
  cat >"$CBM_ZOVA_OPTIMIZATION_RUN_ROOT/optimization-report.json" <<JSON
{"schema_version":1,"benchmark_scope":"index-only","repository":"$CBM_ZOVA_OPTIMIZATION_REPOSITORY","passed":true,"states":[{"route":"pure","timing_ms":{"pipeline":$pure,"publish":0},"storage":{"database_bytes":100}},{"route":"single","timing_ms":{"pipeline":$single,"publish":5},"storage":{"database_bytes":80}}]}
JSON
  exit 0
fi
printf '{"schema_version":1,"repository":"%s","source_commit":"%040d","build_sha256":"%064d","run_id":"%s","passed":true,"states":[]}\n' \
  "$CBM_ZOVA_OPTIMIZATION_REPOSITORY" 0 0 "$stage" \
  >"$CBM_ZOVA_OPTIMIZATION_RUN_ROOT/optimization-report.json"
SH
cat >"$TMP/bin/gate" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
mode=gate output= repository=
[[ ${1:-} != --aggregate-baseline ]] || mode=median
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output) output=$2; shift 2;;
    --report) repository=$(sed -n 's/.*"repository":"\([^"]*\)".*/\1/p' "$2"); shift 2;;
    *) shift;;
  esac
done
[[ -n $output ]]
if [[ $mode == gate ]]; then echo "gate:$repository" >>"$CBM_FAKE_LOG"; fi
printf '{"schema_version":1,"repository":"%s","passed":true,"states":[]}\n' \
  "${repository:-tops}" >"$output"
SH
cat >"$TMP/bin/baseline" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
output=
while [[ $# -gt 0 ]]; do
  case "$1" in --output) output=$2; shift 2;; *) shift;; esac
done
echo baseline >>"$CBM_FAKE_LOG"
mkdir -p "$output"
for repo in tops motive rvault CBM; do
  printf '{"schema_version":1,"baseline_kind":"documented_pre_v6","repository":"%s"}\n' "$repo" >"$output/$repo.json"
done
SH
cat >"$TMP/bin/aggregate" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
root=
while [[ $# -gt 0 ]]; do
  case "$1" in --root) root=$2; shift 2;; *) shift;; esac
done
[[ -n $root ]]
printf '{"schema_version":1,"passed":true}\n' >"$root/optimization.json"
printf '# Zova optimization results\n' >"$root/optimization.md"
SH
chmod +x "$TMP/bin/"*

invoke() {
  local output=$1
  CBM_FAKE_LOG="$TMP/log" CBM_ZOVA_OPTIMIZATION_FINAL_ROOT="$output" \
  CBM_ZOVA_OPTIMIZATION_TOPS_REPO="$TMP/tops" \
  CBM_ZOVA_OPTIMIZATION_DENO_REPO="$TMP/deno" \
  CBM_ZOVA_OPTIMIZATION_MOTIVE_REPO="$TMP/motive" \
  CBM_ZOVA_OPTIMIZATION_RVAULT_REPO="$TMP/rvault" \
  CBM_ZOVA_OPTIMIZATION_CBM_REPO="$TMP/CBM" \
  CBM_ZOVA_OPTIMIZATION_BASELINE_ROOT="$TMP/baseline" \
  CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_OPTIMIZATION_BUILD_ONCE="$TMP/bin/build-once" \
  CBM_ZOVA_OPTIMIZATION_FOCUSED_RUNNER="$TMP/bin/focused" \
  CBM_ZOVA_OPTIMIZATION_REPOSITORY_RUNNER="$TMP/bin/repository" \
  CBM_ZOVA_OPTIMIZATION_GATE="$TMP/bin/gate" \
  CBM_ZOVA_OPTIMIZATION_BASELINE_EXTRACTOR="$TMP/bin/baseline" \
  CBM_ZOVA_OPTIMIZATION_AGGREGATOR="$TMP/bin/aggregate" \
  CBM_ZOVA_OPTIMIZATION_MANIFEST="$TMP/manifest.txt" "$SCRIPT"
}

invoke "$TMP/output"
expected='baseline
build
focused
tops:1:all
tops:2:all
tops:3:all
gate:tops
motive:1:all
rvault:1:all
CBM:1:all
gate:motive
gate:rvault
gate:CBM'
[[ $(cat "$TMP/log") == "$expected" ]] || { cat "$TMP/log" >&2; fail "stage order wrong"; }
[[ -s "$TMP/output/optimization.json" && -s "$TMP/output/optimization.md" ]] ||
  fail "aggregate outputs missing"

: >"$TMP/index-log"
CBM_FAKE_LOG="$TMP/index-log" CBM_ZOVA_OPTIMIZATION_FINAL_ROOT="$TMP/index-output" \
CBM_ZOVA_OPTIMIZATION_TOPS_REPO="$TMP/tops" \
CBM_ZOVA_OPTIMIZATION_DENO_REPO="$TMP/deno" \
CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$TMP/bin/build" \
CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TMP/bin/test-runner" \
CBM_ZOVA_OPTIMIZATION_BUILD_ONCE="$TMP/bin/build-once" \
CBM_ZOVA_OPTIMIZATION_REPOSITORY_RUNNER="$TMP/bin/repository" \
CBM_WORKERS=4 "$SCRIPT" index-only >/dev/null
expected_index='build
tops:1:index-only
tops:2:index-only
deno:1:index-only
deno:2:index-only'
[[ $(cat "$TMP/index-log") == "$expected_index" ]] ||
  { cat "$TMP/index-log" >&2; fail "index-only stage order wrong"; }
python3 - "$TMP/index-output/index-only-average.json" <<'PY'
import json, pathlib, sys
report = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert report["benchmark_scope"] == "index-only"
assert report["workers"] == "4"
assert [item["repository"] for item in report["repositories"]] == ["tops", "deno"]
for item in report["repositories"]:
    assert item["sample_count"] == 2
    assert item["average"]["pure_sqlite_full_ms"] == 15
    assert item["average"]["zova_native_full_ms"] == 30
PY

: >"$TMP/fail-log"
set +e
CBM_FAKE_FAIL_STAGE=tops:2 CBM_FAKE_LOG="$TMP/fail-log" \
CBM_ZOVA_OPTIMIZATION_FINAL_ROOT="$TMP/fail-output" \
CBM_ZOVA_OPTIMIZATION_TOPS_REPO="$TMP/tops" \
CBM_ZOVA_OPTIMIZATION_DENO_REPO="$TMP/deno" \
CBM_ZOVA_OPTIMIZATION_MOTIVE_REPO="$TMP/motive" \
CBM_ZOVA_OPTIMIZATION_RVAULT_REPO="$TMP/rvault" \
CBM_ZOVA_OPTIMIZATION_CBM_REPO="$TMP/CBM" \
CBM_ZOVA_OPTIMIZATION_BASELINE_ROOT="$TMP/baseline" \
CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$TMP/bin/build" \
CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TMP/bin/test-runner" \
CBM_ZOVA_OPTIMIZATION_BUILD_ONCE="$TMP/bin/build-once" \
CBM_ZOVA_OPTIMIZATION_FOCUSED_RUNNER="$TMP/bin/focused" \
CBM_ZOVA_OPTIMIZATION_REPOSITORY_RUNNER="$TMP/bin/repository" \
CBM_ZOVA_OPTIMIZATION_GATE="$TMP/bin/gate" \
CBM_ZOVA_OPTIMIZATION_BASELINE_EXTRACTOR="$TMP/bin/baseline" \
CBM_ZOVA_OPTIMIZATION_AGGREGATOR="$TMP/bin/aggregate" \
CBM_ZOVA_OPTIMIZATION_MANIFEST="$TMP/manifest.txt" "$SCRIPT" >/dev/null 2>&1
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "failed TOPS attempt unexpectedly passed"
! rg -q '^(motive|rvault|CBM):' "$TMP/fail-log" ||
  fail "larger repository ran after TOPS failure"

echo "PASS: Zova optimization multi-repository orchestration"
