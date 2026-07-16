#!/usr/bin/env bash
set -euo pipefail
ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-section9-promotion-multi-repo-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-section9-multi.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
expect_fail() { if "$@" >/dev/null 2>&1; then fail "unexpected success: $*"; fi; }

mkdir -p "$TMP/bin"
for name in tops motive rvault CBM; do
  mkdir -p "$TMP/$name"; git -C "$TMP/$name" init -q
  printf '%s\n' "$name" >"$TMP/$name/file"
  git -C "$TMP/$name" add file
  git -C "$TMP/$name" -c user.name=Codex -c user.email=codex@example.invalid commit -qm init
done
printf build >"$TMP/bin/build"; printf runner >"$TMP/bin/test-runner"
chmod +x "$TMP/bin/build" "$TMP/bin/test-runner"

cat >"$TMP/bin/build-once" <<'SH'
#!/usr/bin/env bash
echo build >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != build ]] || exit 9
SH
cat >"$TMP/bin/preflight" <<'SH'
#!/usr/bin/env bash
echo preflight >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != preflight ]] || exit 9
SH
cat >"$TMP/bin/calibrate" <<'SH'
#!/usr/bin/env bash
echo calibration >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != calibration ]] || exit 9
mkdir -p "$CBM_ZOVA_SECTION9_CALIBRATION_ROOT"
printf '{"build_sha256":"%064d","digest":"%064d"}\n' 0 1 >"$CBM_ZOVA_SECTION9_CALIBRATION_ROOT/calibration.json"
SH
cat >"$TMP/bin/focused" <<'SH'
#!/usr/bin/env bash
echo "focused:$1" >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != "focused:$1" ]] || exit 9
mkdir -p "$2"
printf '{"digest":"%064d"}\n' "$1" >"$2/focused-evidence.json"
SH
cat >"$TMP/bin/repository" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
stage="repo:$CBM_ZOVA_SECTION9_ATTEMPT:$CBM_ZOVA_SECTION9_REPOSITORY"
echo "$stage:$CBM_ZOVA_MIN_FREE_GB" >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != "$stage" ]] || exit 9
mkdir -p "$CBM_ZOVA_SECTION9_RUN_ROOT"
run_id="${CBM_ZOVA_SECTION9_REPOSITORY}-${CBM_ZOVA_SECTION9_ATTEMPT}"
[[ "${CBM_FAKE_DUPLICATE_RUN_ID:-0}" != 1 ]] || run_id=duplicate
printf '{"run_id":"%s","passed":true}\n' "$run_id" >"$CBM_ZOVA_SECTION9_RUN_ROOT/repository-report.json"
printf '{"passed":true}\n' >"$CBM_ZOVA_SECTION9_RUN_ROOT/repository-gate.json"
SH
cat >"$TMP/bin/gate" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
mode=attempt; output=; reports=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --aggregate) mode=aggregate; shift;;
    --output) output=$2; shift 2;;
    --report|--attempt-report) reports+=("$2"); shift 2;;
    *) shift;;
  esac
done
attempt=${CBM_ZOVA_SECTION9_ATTEMPT:-0}
echo "$mode-gate:$attempt" >>"$CBM_FAKE_LOG"
[[ "${CBM_FAKE_FAIL_STAGE:-}" != "$mode-gate:$attempt" ]] || exit 9
if [[ "$mode" == attempt ]]; then
  ids=$(python3 - "${reports[@]}" <<'PY'
import json,sys
print("\n".join(json.load(open(x.split("=",1)[1]))["run_id"] for x in sys.argv[1:]))
PY
)
  [[ $(printf '%s\n' "$ids" | sort -u | wc -l | tr -d ' ') == 4 ]] || exit 9
fi
mkdir -p "$(dirname "$output")"; printf '{"passed":true}\n' >"$output"
SH
chmod +x "$TMP/bin/"*

invoke() {
  CBM_FAKE_LOG="$TMP/log" CBM_ZOVA_SECTION9_MULTI_ROOT="$1" \
  CBM_ZOVA_SECTION9_TOPS_REPO="$TMP/tops" CBM_ZOVA_SECTION9_MOTIVE_REPO="$TMP/motive" \
  CBM_ZOVA_SECTION9_RVAULT_REPO="$TMP/rvault" CBM_ZOVA_SECTION9_CBM_REPO="$TMP/CBM" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_BUILD_ONCE="$TMP/bin/build-once" \
  CBM_ZOVA_SECTION9_PREFLIGHT_COMMAND="$TMP/bin/preflight" \
  CBM_ZOVA_SECTION9_CALIBRATOR="$TMP/bin/calibrate" \
  CBM_ZOVA_SECTION9_FOCUSED_RUNNER="$TMP/bin/focused" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository" \
  CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" "$SCRIPT"
}

expect_fail env CBM_ZOVA_SECTION9_ATTEMPTS=2 "$SCRIPT"
mv "$TMP/rvault" "$TMP/rvault-missing"
expect_fail invoke "$TMP/missing"
mv "$TMP/rvault-missing" "$TMP/rvault"
mkdir -p "$TMP/locked/.section9-multi.lock"
expect_fail invoke "$TMP/locked"

: >"$TMP/log"; invoke "$TMP/success"
expected='build
preflight
calibration
focused:1
repo:1:tops:8
repo:1:motive:8
repo:1:rvault:8
repo:1:CBM:8
attempt-gate:1
focused:2
repo:2:tops:8
repo:2:motive:8
repo:2:rvault:8
repo:2:CBM:8
attempt-gate:2
focused:3
repo:3:tops:8
repo:3:motive:8
repo:3:rvault:8
repo:3:CBM:8
attempt-gate:3
aggregate-gate:0'
[[ $(cat "$TMP/log") == "$expected" ]] || { cat "$TMP/log" >&2; fail "stage order changed"; }
[[ -s "$TMP/success/section9-latest-gate.json" ]] || fail "latest aggregate missing"

: >"$TMP/log"
expect_fail env CBM_FAKE_FAIL_STAGE=repo:1:rvault CBM_FAKE_LOG="$TMP/log" \
  CBM_ZOVA_SECTION9_MULTI_ROOT="$TMP/fail-fast" \
  CBM_ZOVA_SECTION9_TOPS_REPO="$TMP/tops" CBM_ZOVA_SECTION9_MOTIVE_REPO="$TMP/motive" \
  CBM_ZOVA_SECTION9_RVAULT_REPO="$TMP/rvault" CBM_ZOVA_SECTION9_CBM_REPO="$TMP/CBM" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_BUILD_ONCE="$TMP/bin/build-once" CBM_ZOVA_SECTION9_PREFLIGHT_COMMAND="$TMP/bin/preflight" \
  CBM_ZOVA_SECTION9_CALIBRATOR="$TMP/bin/calibrate" CBM_ZOVA_SECTION9_FOCUSED_RUNNER="$TMP/bin/focused" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository" CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" "$SCRIPT"
! rg -q 'repo:1:CBM' "$TMP/log" || fail "CBM ran after rvault failure"

: >"$TMP/log"
expect_fail env CBM_FAKE_DUPLICATE_RUN_ID=1 CBM_FAKE_LOG="$TMP/log" \
  CBM_ZOVA_SECTION9_MULTI_ROOT="$TMP/duplicate" \
  CBM_ZOVA_SECTION9_TOPS_REPO="$TMP/tops" CBM_ZOVA_SECTION9_MOTIVE_REPO="$TMP/motive" \
  CBM_ZOVA_SECTION9_RVAULT_REPO="$TMP/rvault" CBM_ZOVA_SECTION9_CBM_REPO="$TMP/CBM" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" CBM_ZOVA_SECTION9_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_SECTION9_BUILD_ONCE="$TMP/bin/build-once" CBM_ZOVA_SECTION9_PREFLIGHT_COMMAND="$TMP/bin/preflight" \
  CBM_ZOVA_SECTION9_CALIBRATOR="$TMP/bin/calibrate" CBM_ZOVA_SECTION9_FOCUSED_RUNNER="$TMP/bin/focused" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository" CBM_ZOVA_SECTION9_GATE="$TMP/bin/gate" "$SCRIPT"

echo "PASS: Zova Section 9 multi-repository promotion validation"
