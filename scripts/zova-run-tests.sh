#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" != "1" ]]; then
  bash "$ROOT/scripts/zova-build-once.sh" >/dev/null
fi

RUNNER=${CBM_ZOVA_REAL_TEST_RUNNER:-"$ROOT/build/c/test-runner"}
[[ -x "$RUNNER" ]] || { echo "error: test runner is missing: $RUNNER" >&2; exit 1; }

TEST_ENV_OWNED=0
if [[ -n "${CBM_ZOVA_TEST_CACHE_DIR:-}" ]]; then
  TEST_ENV_ROOT=$CBM_ZOVA_TEST_CACHE_DIR
else
  TEST_ENV_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-tests.XXXXXX")
  TEST_ENV_OWNED=1
fi
TEST_HOME="$TEST_ENV_ROOT/home"
TEST_CACHE="$TEST_ENV_ROOT/cache"
mkdir -p "$TEST_HOME" "$TEST_CACHE"
cleanup_test_environment() {
  if [[ "$TEST_ENV_OWNED" -eq 1 && "${CBM_ZOVA_TEST_PRESERVE:-0}" != "1" ]]; then
    rm -rf "$TEST_ENV_ROOT"
  fi
}
trap cleanup_test_environment EXIT INT TERM

if [[ "$#" -eq 0 ]]; then
  HOME="$TEST_HOME" CBM_CACHE_DIR="$TEST_CACHE" "$RUNNER"
  exit $?
fi

# Build once above, then run every requested suite directly. This is the key
# distinction from invoking `zig build test-*` repeatedly.
for suite in "$@"; do
  echo "TEST SUITE: $suite" >&2
  HOME="$TEST_HOME" CBM_CACHE_DIR="$TEST_CACHE" "$RUNNER" "$suite"
done
