#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
RUNNER_DIR=${CBM_ZOVA_TEST_RUNNER_DIR:-"$ROOT/build/c"}

runner_group_for_suite() {
  case "$1" in
    zova|zova_normalization_benchmark|zova_operations|zova_migration|\
    zova_c_sql_functions|zova_bridge|zova_real_repo|zova_graph_real_repo|\
    zova_single_file_real_repo|zova_single_file_promotion_real_repo|\
    zova_incremental_native)
      printf 'zova\n'
      ;;
    mcp)
      printf 'mcp\n'
      ;;
    graph_buffer|registry|pipeline|index_resilience|fqn|route_canon|path_alias|\
    configlink|infrascan|worker_pool|parallel|incremental)
      printf 'pipeline\n'
      ;;
    *)
      printf 'full\n'
      ;;
  esac
}

runner_for_group() {
  if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" == "1" ]]; then
    printf '%s\n' "${CBM_ZOVA_REAL_TEST_RUNNER:-"$RUNNER_DIR/test-runner"}"
  elif [[ "$1" == "full" ]]; then
    printf '%s\n' "$RUNNER_DIR/test-runner"
  else
    printf '%s\n' "$RUNNER_DIR/test-runner-$1"
  fi
}

built_groups=" "
ensure_runner() {
  local group=$1
  RUNNER=$(runner_for_group "$group")
  if [[ "${CBM_ZOVA_BUILD_SKIP:-0}" != "1" && "$built_groups" != *" $group "* ]]; then
    bash "$ROOT/scripts/zova-build-test-runner.sh" "$group" >/dev/null
    built_groups+="$group "
  fi
  [[ -x "$RUNNER" ]] || { echo "error: test runner is missing: $RUNNER" >&2; exit 1; }
}

TEST_ENV_OWNED=0
if [[ -n "${CBM_ZOVA_TEST_CACHE_DIR:-}" ]]; then
  TEST_ENV_ROOT=$CBM_ZOVA_TEST_CACHE_DIR
else
  TEST_ENV_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-tests.XXXXXX")
  TEST_ENV_OWNED=1
fi
TEST_HOME="$TEST_ENV_ROOT/home"
TEST_CACHE="$TEST_HOME/.cache/codebase-memory-mcp"
mkdir -p "$TEST_HOME" "$TEST_CACHE"
cleanup_test_environment() {
  if [[ "$TEST_ENV_OWNED" -eq 1 && "${CBM_ZOVA_TEST_PRESERVE:-0}" != "1" ]]; then
    rm -rf "$TEST_ENV_ROOT"
  fi
}
trap cleanup_test_environment EXIT INT TERM

if [[ "$#" -eq 0 ]]; then
  ensure_runner full
  HOME="$TEST_HOME" CBM_CACHE_DIR="$TEST_CACHE" "$RUNNER"
  exit $?
fi

for suite in "$@"; do
  group=$(runner_group_for_suite "$suite")
  ensure_runner "$group"
  echo "TEST SUITE: $suite" >&2
  if [[ -n "${CBM_ZOVA_MODE+x}" || "$group" == "zova" ]]; then
    HOME="$TEST_HOME" CBM_CACHE_DIR="$TEST_CACHE" "$RUNNER" "$suite"
  else
    HOME="$TEST_HOME" CBM_CACHE_DIR="$TEST_CACHE" CBM_ZOVA_MODE=off "$RUNNER" "$suite"
  fi
done
