#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)

authority=(
  zova_operations zova zova_c_sql_functions zova_bridge zova_plugin zova_incremental_native
)
mcp_query=(
  mcp cypher cli integration security
)
pipeline=(
  graph_buffer registry pipeline spawns importance index_resilience call_reference fqn route_canon
  path_alias watcher
  configlink infrascan worker_pool parallel incremental discover language userconfig
  gitignore git_context
)
languages=(
  extraction parse_coverage extraction_inheritance extraction_imports grammar_regression grammar_labels
  grammar_imports scope type_rep go_lsp c_lsp php_lsp cs_lsp cs_lsp_bench py_lsp
  kotlin_lsp rust_lsp zig_lsp py_lsp_bench py_lsp_stress py_lsp_scale ts_lsp java_lsp
  java_lsp_coverage lang_contract edge_imports edge_structural lsp_resolution_probe
  node_creation_probe edge_types_probe convergence_probe matrix_known_classes
  matrix_new_constructs grammar_probe_a grammar_probe_b grammar_probe_c grammar_probe_d
  grammar_probe_e grammar_probe_f grammar_probe_g
)
core=(
  arena hash_table dyn_array str_intern log str_util platform subprocess dump_verify ac
  store_nodes store_edges store_search
  lz4 zstd system_info slab_alloc mem yaml semantic
  ast_profile simhash stack_overflow store_arch
)

all_sharded_suites() {
  printf '%s\n' \
    "${authority[@]}" "${mcp_query[@]}" "${pipeline[@]}" "${languages[@]}" "${core[@]}"
}

default_suites() {
  awk '
    /^#if defined\(CBM_TEST_RUNNER_ZOVA\)/ { focused = 1 }
    focused && /^#else$/ { full = 1; next }
    full && /^#endif$/ { exit }
    full { print }
  ' "$ROOT/tests/test_main.c" |
    sed -n 's/.*RUN_SELECTED_SUITE(\([^)]*\)).*/\1/p'
}

if [[ "${1:-}" == "--verify" ]]; then
  duplicates=$(all_sharded_suites | sort | uniq -d)
  if [[ -n "$duplicates" ]]; then
    echo "error: CI suites assigned to multiple shards:" >&2
    printf '%s\n' "$duplicates" >&2
    exit 1
  fi
  diff -u <(default_suites | sort) <(all_sharded_suites | sort)
  echo "PASS: CI shards cover every default C suite exactly once"
  exit 0
fi

SHARD=${1:-}
case "$SHARD" in
  authority) suites=("${authority[@]}") ;;
  mcp-query) suites=("${mcp_query[@]}") ;;
  pipeline) suites=("${pipeline[@]}") ;;
  languages) suites=("${languages[@]}") ;;
  core) suites=("${core[@]}") ;;
  *)
    echo "error: unknown CI test shard: $SHARD" >&2
    exit 1
    ;;
esac

echo "CI TEST SHARD: $SHARD (${#suites[@]} suites)" >&2
"$ROOT/scripts/zova-run-tests.sh" "${suites[@]}"

# The pipeline shard owns git_context: run it again with an inherited git
# repository environment (#56).
if [[ "$SHARD" == "pipeline" ]]; then
  bash "$ROOT/tests/test_git_env_isolation_contract.sh" \
    "${CBM_ZOVA_REAL_TEST_RUNNER:-$ROOT/build/c/test-runner}"
fi
