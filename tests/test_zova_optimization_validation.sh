#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-optimization-validation.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-optimization.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
expect_fail() { if "$@" >/dev/null 2>&1; then fail "unexpected success: $*"; fi; }

mkdir -p "$TMP/source" "$TMP/bin"
git -C "$TMP/source" init -q
mkdir -p "$TMP/source/pkg"
printf 'package pkg\nfunc Existing() int { return 7 }\n' >"$TMP/source/pkg/probe.go"
git -C "$TMP/source" add pkg/probe.go
git -C "$TMP/source" -c user.name=Codex -c user.email=codex@example.invalid \
  commit -qm fixture
printf 'build\n' >"$TMP/bin/build"
printf 'runner\n' >"$TMP/bin/test-runner"
chmod +x "$TMP/bin/build" "$TMP/bin/test-runner"

cat >"$TMP/bin/disk-guard" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$1" >>"$CBM_FAKE_RECORD.disk"
SH

cat >"$TMP/bin/run-tests" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == zova_single_file_promotion_real_repo ]]
[[ "$CBM_ZOVA_PROMOTION_ATTEMPT" == 1 ]] || exit 80
route=$CBM_ZOVA_OPTIMIZATION_ROUTE
workload=$CBM_ZOVA_OPTIMIZATION_WORKLOAD
mode=$CBM_ZOVA_OPTIMIZATION_PIPELINE_MODE
mutation=$CBM_ZOVA_OPTIMIZATION_MUTATION
probe=$(sed -n 's/.*return \([0-9][0-9]*\).*/\1/p' \
  "$CBM_ZOVA_OPTIMIZATION_MUTATION_FILE")
printf '%s|%s|%s|%s|%s|%s\n' "$route" "$workload" "$mode" "$probe" "$mutation" \
  "$CBM_ZOVA_TEST_CACHE_DIR" >>"$CBM_FAKE_RECORD.states"
if [[ ${CBM_FAKE_FAIL_STATE:-} == "$route:$workload" ]]; then exit 1; fi
cat >"$CBM_ZOVA_OPTIMIZATION_REPORT" <<JSON
{"route":"$route","workload":"$workload","pipeline_mode":"$mode","incremental_fallback_reason":null,"full_fallback_count":0,"full_clear_count":0,"unchanged_rewrite_count":0,"rows":{"nodes_total":100,"nodes_inserted":1,"nodes_updated":0,"nodes_deleted":0,"edges_total":200,"edges_inserted":1,"edges_deleted":0,"node_vectors_total":80,"node_vectors_upserted":1,"node_vectors_deleted":0,"token_vectors_total":20,"token_vectors_upserted":1,"token_vectors_deleted":0},"timing_ms":{"normalize":1.0,"canonical_files":1.0,"canonical_nodes":1.0,"canonical_edges":1.0,"canonical_hashes":1.0,"fts":1.0,"token_metadata":1.0,"native_graph":1.0,"native_graph_materialize":0.1,"native_graph_reset":0.2,"native_graph_nodes":0.2,"native_graph_edges":0.3,"native_graph_validate":0.1,"native_graph_cleanup":0.1,"native_vectors":1.0,"readback":1.0,"digests":1.0,"verify":1.0,"publish":10.0,"pipeline":20.0},"statement_metrics":{"canonical_files":{"rows":2,"bind_i64_calls":4,"bind_text_calls":2,"bind_double_calls":0,"step_calls":2,"reset_calls":2,"clear_bindings_calls":2},"canonical_nodes":{"rows":2,"bind_i64_calls":12,"bind_text_calls":10,"bind_double_calls":0,"step_calls":2,"reset_calls":2,"clear_bindings_calls":2},"canonical_edges":{"rows":1,"bind_i64_calls":3,"bind_text_calls":5,"bind_double_calls":0,"step_calls":1,"reset_calls":1,"clear_bindings_calls":1},"canonical_hashes":{"rows":2,"bind_i64_calls":6,"bind_text_calls":2,"bind_double_calls":0,"step_calls":2,"reset_calls":2,"clear_bindings_calls":2},"canonical_token_metadata":{"rows":2,"bind_i64_calls":2,"bind_text_calls":4,"bind_double_calls":2,"step_calls":2,"reset_calls":2,"clear_bindings_calls":2},"full_fts_bulk_statements":0,"full_fts_trigger_rows_avoided":0,"full_node_guard_validation_statements":0,"full_edge_guard_validation_statements":0},"snapshot":{"completed":$([[ $route == single && $workload == incremental ]] && echo true || echo false),"generation":$([[ $route == single && $workload == incremental ]] && echo 1 || echo 0),"base_ms":0.0,"optional_ms":0.0,"hydrated_components":0,"topology_rows":0,"node_vector_rows":0,"token_vector_rows":0},"storage":{"database_bytes":1000,"wal_bytes":0,"freelist_bytes":0,"canonical_bytes":200,"fts_bytes":100,"native_graph_bytes":300,"native_vector_bytes":300,"other_bytes":100,"graph_bytes_per_edge":1.5,"vector_bytes_per_row":3.0},"performance":{"vector_p95_ms":1.0},"forbidden_table_count":0,"parity_mismatch_count":0,"unexpected_fallback_count":0,"passed":true}
JSON
SH

cat >"$TMP/bin/gate" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >"$CBM_FAKE_RECORD.gate"
output=
while [[ $# -gt 0 ]]; do
  if [[ $1 == --output ]]; then output=$2; shift 2; else shift; fi
done
[[ -n $output ]]
printf '{"passed":true,"failures":[]}\n' >"$output"
SH
chmod +x "$TMP/bin/disk-guard" "$TMP/bin/run-tests" "$TMP/bin/gate"

cat >"$TMP/baseline.json" <<'JSON'
{"schema_version":1,"repository":"tops","source_commit":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","build_sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","run_id":"baseline","passed":true,"states":[]}
JSON

invoke() {
  local run_root=$1
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_OPTIMIZATION_REPOSITORY=tops \
  CBM_ZOVA_OPTIMIZATION_ATTEMPT=1 \
  CBM_ZOVA_OPTIMIZATION_MUTATION=source-change \
  CBM_ZOVA_OPTIMIZATION_RUN_ROOT="$run_root" \
  CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_OPTIMIZATION_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_ZOVA_OPTIMIZATION_DISK_GUARD="$TMP/bin/disk-guard" \
  CBM_ZOVA_OPTIMIZATION_GATE="$TMP/bin/gate" \
  CBM_ZOVA_OPTIMIZATION_BASELINE="$TMP/baseline.json" \
  CBM_FAKE_RECORD="$TMP/record" "$SCRIPT"
}

# RED contract: the script must reject missing/invalid inputs before mutation.
mkdir -p "$TMP/not-git"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/not-git" \
  CBM_ZOVA_OPTIMIZATION_REPOSITORY=tops \
  CBM_ZOVA_OPTIMIZATION_ATTEMPT=1 \
  CBM_ZOVA_OPTIMIZATION_RUN_ROOT="$TMP/not-git-run" "$SCRIPT"
expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_OPTIMIZATION_REPOSITORY=large \
  CBM_ZOVA_OPTIMIZATION_RUN_ROOT="$TMP/bad-name" "$SCRIPT"

invoke "$TMP/success"
[[ -s "$TMP/success/optimization-report.json" ]] || fail "aggregate report missing"
[[ -s "$TMP/success/optimization-gate.json" ]] || fail "gate report missing"
[[ ! -e "$TMP/success/clone" ]] || fail "successful clone retained"
[[ $(wc -l <"$TMP/record.states" | tr -d ' ') == 4 ]] || fail "wrong state count"
state1=$(sed -n '1p' "$TMP/record.states")
state2=$(sed -n '2p' "$TMP/record.states")
state3=$(sed -n '3p' "$TMP/record.states")
state4=$(sed -n '4p' "$TMP/record.states")
[[ $state1 == pure\|full\|CBM_MODE_FULL\|7\|source-change\|* ]] || fail "pure full not first"
[[ $state2 == single\|full\|CBM_MODE_FULL\|7\|source-change\|* ]] || fail "single full not second"
[[ $state3 == pure\|incremental\|CBM_MODE_INCREMENTAL\|99\|source-change\|* ]] ||
  fail "pure incremental did not reuse mutated repository"
[[ $state4 == single\|incremental\|CBM_MODE_INCREMENTAL\|99\|source-change\|* ]] ||
  fail "single incremental did not run last"
pure_full_cache=${state1##*|}
single_full_cache=${state2##*|}
[[ ${state3##*|} == "$pure_full_cache" ]] || fail "pure cache was not reused"
[[ ${state4##*|} == "$single_full_cache" ]] || fail "single cache was not reused"
grep -q -- '--baseline' "$TMP/record.gate" || fail "baseline not passed to gate"
grep -q -- '--report' "$TMP/record.gate" || fail "report not passed to gate"

rm -f "$TMP/record.states"
expect_fail env CBM_FAKE_FAIL_STATE=single:full \
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_OPTIMIZATION_REPOSITORY=tops \
  CBM_ZOVA_OPTIMIZATION_ATTEMPT=1 \
  CBM_ZOVA_OPTIMIZATION_MUTATION=source-change \
  CBM_ZOVA_OPTIMIZATION_RUN_ROOT="$TMP/full-fail" \
  CBM_ZOVA_OPTIMIZATION_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_OPTIMIZATION_TEST_RUNNER="$TMP/bin/test-runner" \
  CBM_ZOVA_OPTIMIZATION_RUN_TESTS="$TMP/bin/run-tests" \
  CBM_ZOVA_OPTIMIZATION_DISK_GUARD="$TMP/bin/disk-guard" \
  CBM_ZOVA_OPTIMIZATION_GATE="$TMP/bin/gate" \
  CBM_ZOVA_OPTIMIZATION_BASELINE="$TMP/baseline.json" \
  CBM_FAKE_RECORD="$TMP/record" "$SCRIPT"
[[ $(wc -l <"$TMP/record.states" | tr -d ' ') == 2 ]] ||
  fail "incremental ran after full-state failure"

echo "PASS: Zova optimization validation orchestration"
