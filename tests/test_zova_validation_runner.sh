#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
SCRIPT="$ROOT/scripts/zova-real-repo-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-runner.XXXXXX")
FIRST_PID=""
SECOND_PID=""

cleanup() {
  local pid_file pid
  for pid_file in "$TMP"/fake.*.pid; do
    [[ -f "$pid_file" ]] || continue
    pid=$(cat "$pid_file")
    kill -KILL "$pid" 2>/dev/null || true
  done
  [[ -n "$SECOND_PID" ]] && kill -KILL "$SECOND_PID" 2>/dev/null || true
  [[ -n "$FIRST_PID" ]] && kill -KILL "$FIRST_PID" 2>/dev/null || true
  [[ -n "$SECOND_PID" ]] && wait "$SECOND_PID" 2>/dev/null || true
  [[ -n "$FIRST_PID" ]] && wait "$FIRST_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/repo/.git"

cat > "$TMP/fake-cbm" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
marker=${FAKE_MARKER:?}
printf '%s\n' "$$" > "$marker/fake.$$.pid"
if [[ -e "$marker/first-entered" ]]; then
  touch "$marker/second-entered"
else
  touch "$marker/first-entered"
fi
while :; do sleep 1; done
EOF
chmod +x "$TMP/fake-cbm"

run_validation() {
  CBM_ZOVA_VALIDATION_REPO="$TMP/repo" \
  CBM_ZOVA_VALIDATION_RUN_DIR="$TMP/validation" \
  CBM_ZOVA_VALIDATION_LOCK_DIR="$TMP/validation.lock" \
  CBM_ZOVA_VALIDATION_SKIP_BUILD=1 \
  CBM_ZOVA_REAL_BINARY="$TMP/fake-cbm" \
  CBM_ZOVA_REAL_TEST_RUNNER="$TMP/fake-test-runner" \
  FAKE_MARKER="$TMP" \
    bash "$SCRIPT"
}

run_validation > "$TMP/first.out" 2>&1 &
FIRST_PID=$!

for _ in $(seq 1 50); do
  [[ -e "$TMP/first-entered" ]] && break
  sleep 0.1
done
[[ -e "$TMP/first-entered" ]] || {
  cat "$TMP/first.out" >&2
  echo "FAIL: first validation did not reach the fake indexer" >&2
  exit 1
}

set +e
run_validation > "$TMP/second.out" 2>&1 &
SECOND_PID=$!
for _ in $(seq 1 20); do
  kill -0 "$SECOND_PID" 2>/dev/null || break
  [[ -e "$TMP/second-entered" ]] && break
  sleep 0.1
done
if kill -0 "$SECOND_PID" 2>/dev/null; then
  SECOND_RC=124
else
  wait "$SECOND_PID"
  SECOND_RC=$?
fi
set -e

[[ "$SECOND_RC" -ne 0 ]] || {
  cat "$TMP/second.out" >&2
  echo "FAIL: overlapping validation unexpectedly succeeded" >&2
  exit 1
}
[[ ! -e "$TMP/second-entered" ]] || {
  cat "$TMP/second.out" >&2
  echo "FAIL: overlapping validation reached the indexer" >&2
  exit 1
}
grep -q 'validation run already active' "$TMP/second.out" || {
  cat "$TMP/second.out" >&2
  echo "FAIL: overlapping validation did not report its active owner" >&2
  exit 1
}

if CBM_ZOVA_AVAILABLE_KB_OVERRIDE=8388607 \
    bash "$ROOT/scripts/zova-disk-guard.sh" "$TMP" 2> "$TMP/disk-guard.err"; then
  echo "FAIL: default disk guard accepted less than 8 GiB" >&2
  exit 1
fi
grep -q 'Required free space: 8 GiB' "$TMP/disk-guard.err" || {
  cat "$TMP/disk-guard.err" >&2
  echo "FAIL: default disk guard no longer reports the 8 GiB requirement" >&2
  exit 1
}
CBM_ZOVA_AVAILABLE_KB_OVERRIDE=8388608 \
  bash "$ROOT/scripts/zova-disk-guard.sh" "$TMP"

echo "PASS: zova validation runner rejects an overlapping run"
echo "PASS: zova validation runner retains the default 8 GiB disk guard"
