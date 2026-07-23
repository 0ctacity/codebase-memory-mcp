#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
WORKFLOWS="$ROOT/.github/workflows"

expected=$(printf '%s\n' _build.yml ci.yml release.yml)
actual=$(find "$WORKFLOWS" -maxdepth 1 -type f -name '*.yml' -exec basename {} \; | sort)
if [[ "$actual" != "$expected" ]]; then
  echo "error: unexpected GitHub workflow set" >&2
  diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") || true
  exit 1
fi

for path in \
  "$ROOT/.github/actions/setup-zova/action.yml" \
  "$WORKFLOWS/ci.yml" \
  "$WORKFLOWS/_build.yml" \
  "$WORKFLOWS/release.yml"; do
  [[ -f "$path" ]] || { echo "error: missing $path" >&2; exit 1; }
done

grep -q 'repository: ata-sesli/zova' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'ref: e492609d480a60781c106012f11b17b2a1c9e330' \
  "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'version: 0.16.0' "$ROOT/.github/actions/setup-zova/action.yml"
[[ $(grep -c 'zig build c-abi' "$ROOT/.github/actions/setup-zova/action.yml") -eq 2 ]] || {
  echo "error: native and cross-target Zova setup must build the C ABI artifact" >&2
  exit 1
}

grep -q 'pull_request:' "$WORKFLOWS/ci.yml"
grep -q 'push:' "$WORKFLOWS/ci.yml"
grep -q 'scripts/zova-run-tests.sh' "$WORKFLOWS/ci.yml"
grep -q 'tests/test_zova_build_workflow.sh' "$WORKFLOWS/ci.yml"

grep -q 'CBM_WITH_ZOVA=1' "$WORKFLOWS/_build.yml"
grep -q 'ZOVA_ROOT=' "$WORKFLOWS/_build.yml"
grep -q 'zova_fresh_build_begin' "$WORKFLOWS/_build.yml"
grep -q 'cbm_zova_publish_model.c' "$ROOT/Makefile.cbm"
grep -q 'cbm-with-ui:.*CBM_ZOVA_BRIDGE_LIB' "$ROOT/Makefile.cbm"
grep -q 'elif.*WITH_ZOVA' "$ROOT/scripts/build.sh"
grep -q 'codebase-memory-mcp-linux-.*-portable.tar.gz' "$WORKFLOWS/_build.yml"
grep -q 'os: darwin' "$WORKFLOWS/_build.yml"
grep -q 'codebase-memory-mcp-windows-' "$WORKFLOWS/_build.yml"

grep -q 'workflow_dispatch:' "$WORKFLOWS/release.yml"
grep -q 'contents: write' "$WORKFLOWS/release.yml"
grep -q 'softprops/action-gh-release@' "$WORKFLOWS/release.yml"
grep -q 'checksums.txt' "$WORKFLOWS/release.yml"

if rg -n -i \
    'npm publish|twine upload|mcp-publisher|pypi|registry\.npmjs|virus.?total|deusdata|brew tap|deploy-pages|stale@|issue-labeler|label-actions|scorecard' \
    "$WORKFLOWS" "$ROOT/.github/actions"; then
  echo "error: publishing or inherited repository automation remains" >&2
  exit 1
fi

echo "PASS: GitHub Actions are limited to Zova-CBM CI and GitHub release artifacts"
