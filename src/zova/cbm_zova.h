#ifndef CBM_ZOVA_H
#define CBM_ZOVA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../internal/cbm/sqlite_writer.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#define CBM_ZOVA_DATABASE_SCHEMA_VERSION 7

#if CBM_WITH_ZOVA
typedef struct zova_database zova_database;
#endif

typedef enum {
    CBM_ZOVA_MODE_OFF = 0,
    CBM_ZOVA_MODE_CONTAINER = 1,
    CBM_ZOVA_MODE_I8_VECTORS = 2,
    CBM_ZOVA_MODE_GRAPH_MIRROR = 3,
    CBM_ZOVA_MODE_GRAPH_READ = 4,
    CBM_ZOVA_MODE_AUTHORITY = 5,
} cbm_zova_mode_t;

typedef struct {
    int64_t node_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    int8_t *vector;
    int vector_len;
    double first_score;
} cbm_zova_node_candidate_t;

typedef struct {
    char *vector_id;
    double score;
} cbm_zova_vector_hit_t;

typedef struct cbm_zova_vector_session cbm_zova_vector_session_t;
typedef struct cbm_zova_graph_session cbm_zova_graph_session_t;
typedef struct cbm_zova_publish_model cbm_zova_publish_model_t;
typedef struct cbm_zova_publish_model cbm_zova_prepared_view_t;
typedef struct cbm_zova_workspace_delta cbm_zova_workspace_delta_t;

typedef struct {
    char *node_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    /* Rich node properties projected into the workspace-scoped sidecar. */
    char *properties;
    int start_line;
    int end_line;
    int hop;
} cbm_zova_graph_visit_t;

typedef struct {
    char *source_node_id;
    char *target_node_id;
    char *edge_type;
    char *properties;
} cbm_zova_graph_adjacency_t;

typedef struct {
    int walk_count;
    double walk_ms;
    double hydrate_prepare_ms;
    double hydrate_step_ms;
    double result_build_ms;
    bool native_profiled;
    double native_mutex_wait_ms;
    double native_root_lookup_ms;
    double native_adjacency_prepare_ms;
    double native_adjacency_execute_ms;
    double native_bfs_bookkeeping_allocation_ms;
    double native_c_abi_result_export_ms;
    double native_total_profiled_ms;
    uint64_t frontier_expansions;
    uint64_t adjacency_query_binds;
    uint64_t adjacency_rows_stepped;
    uint64_t native_result_count;
    /* True when a present sidecar was rejected because its generation did
     * not match the committed SQLite/registry generation.  A missing or
     * unavailable sidecar is reported only as a normal fallback. */
    bool generation_mismatch;
    bool fallback;
} cbm_zova_graph_metrics_t;

typedef struct {
    double direct_graph_write_ms;
    double sqlite_row_mirror_ms;
    uint64_t direct_node_count;
    uint64_t direct_edge_count;
    uint64_t mirror_node_count;
    uint64_t mirror_edge_count;
} cbm_zova_graph_ingestion_metrics_t;

typedef struct {
    bool used_full_search;
    double open_ms;
    double candidate_count_ms;
    double candidate_id_collection_ms;
    double vector_search_ms;
    double hydration_ms;
} cbm_zova_vector_prefetch_metrics_t;

/* Non-destructive capability report for the SQL surface CBM needs before
 * moving metadata/FTS into a user-local cbm.zova file. The probe never
 * creates or mutates persistent tables; transaction testing uses a temporary
 * table. */
typedef struct {
    bool canonical_workspace_metadata;
    bool fts5;
    bool bm25;
    bool json;
    bool recursive_cte;
    bool transactions;
    bool cbm_scalar_functions;
} cbm_zova_sql_capabilities_t;

#define CBM_ZOVA_NODE_COLLECTION "cbm_node_vectors_i8"
#define CBM_ZOVA_TOKEN_COLLECTION "cbm_token_vectors_i8"
#define CBM_ZOVA_CODE_GRAPH "cbm_code_graph"
#define CBM_ZOVA_WORKSPACE_ID_MAX 80
#define CBM_ZOVA_MODEL_FINGERPRINT "nomic_embed_code_v1"
#define CBM_ZOVA_DIGEST_HEX_SIZE 65

typedef enum {
    CBM_ZOVA_DATABASE_COMPATIBLE = 0,
    CBM_ZOVA_DATABASE_REPACK_REQUIRED = 1,
    CBM_ZOVA_DATABASE_INCOMPATIBLE = -1,
} cbm_zova_database_format_status_t;

typedef enum {
    CBM_ZOVA_REPACK_SOURCE_SNAPSHOT = 0,
    CBM_ZOVA_REPACK_TEMP_CREATION,
    CBM_ZOVA_REPACK_WORKSPACE_PUBLISH,
    CBM_ZOVA_REPACK_VERIFICATION,
    CBM_ZOVA_REPACK_FSYNC,
    CBM_ZOVA_REPACK_LIVE_TO_RECOVERY_RENAME,
    CBM_ZOVA_REPACK_TEMP_TO_LIVE_RENAME,
    CBM_ZOVA_REPACK_FINAL_REOPEN,
    CBM_ZOVA_REPACK_PHASE_COUNT,
} cbm_zova_repack_phase_t;

bool cbm_zova_build_enabled(void);
cbm_zova_mode_t cbm_zova_mode_from_env(void);
const char *cbm_zova_mode_name(cbm_zova_mode_t mode);
/* Graph reads default to Zova when CBM_ZOVA_MODE is unset. Explicit modes
 * retain their existing behavior; in particular, CBM_ZOVA_MODE=off disables
 * every Zova route. */
bool cbm_zova_graph_read_is_enabled(void);
/* Vector reads/writes default to Zova when the mode is unset. Explicit
 * `off` remains the complete SQLite rollback switch. */
bool cbm_zova_vector_read_is_enabled(void);
/* The shared single-file authority is the default. Explicit `authority`
 * selects the same route; diagnostic modes retain their compatibility
 * behavior and `off` selects pure SQLite. */
bool cbm_zova_single_file_enabled(void);
/* Raw, read-only compatibility check used before calling the pinned Zova SDK.
 * A v5/v7 file is valid source material but must be atomically repacked. */
cbm_zova_database_format_status_t cbm_zova_database_format_status(const char *path);
const char *cbm_zova_repack_phase_name(cbm_zova_repack_phase_t phase);

/* Validate a durable workspace identifier before any shared SQL or native
 * object is opened. */
int cbm_zova_workspace_id_validate(const char *workspace_id);
int cbm_zova_workspace_id_for_root(const char *root_path, char *out_workspace_id,
                                   size_t out_workspace_id_size);
int cbm_zova_workspace_token_id_v1(const char *workspace_id, const char *token,
                                   char *out, size_t out_size);

int cbm_zova_sidecar_path(const char *db_path, char *out, size_t out_sz);
int cbm_zova_workspace_registry_path(char *out, size_t out_size);
int cbm_zova_workspace_graph_name(const char *workspace_id, char *out, size_t out_size);
int cbm_zova_workspace_node_vector_collection_name(const char *workspace_id,
                                                    const char *model_fingerprint, int dimensions,
                                                    char *out, size_t out_size);
int cbm_zova_workspace_token_vector_collection_name(const char *workspace_id,
                                                     const char *model_fingerprint, int dimensions,
                                                     char *out, size_t out_size);
int cbm_zova_workspace_node_id_v1(const char *workspace_id, const char *node_kind,
                                  const char *relative_path, const char *qualified_name,
                                  const char *semantic_discriminator, char *out, size_t out_size);
int cbm_zova_workspace_node_id_v2(const char *workspace_id, const char *node_kind,
                                  const char *relative_path, const char *qualified_name,
                                  const char *semantic_discriminator, char *out, size_t out_size);
int cbm_zova_workspace_get_or_create_at(const char *registry_path, const char *root_path,
                                        char *out_workspace_id, size_t out_workspace_id_size);
/* Read-only workspace lookup for query paths. Never creates a registry or row. */
int cbm_zova_workspace_lookup_at(const char *registry_path, const char *root_path,
                                 char *out_workspace_id, size_t out_workspace_id_size);
int cbm_zova_workspace_generation_begin_at(const char *registry_path, const char *workspace_id,
                                            int64_t generation);
int cbm_zova_workspace_generation_finish_at(const char *registry_path, const char *workspace_id,
                                             int64_t generation, bool ready);
int cbm_zova_workspace_active_generation_at(const char *registry_path, const char *workspace_id,
                                             int64_t *out_generation);
int cbm_zova_workspace_next_generation_at(const char *registry_path, const char *workspace_id,
                                           int64_t *out_generation);
/* Reads the source generation copied into a published sidecar. A missing
 * generation table means the sidecar predates crash-safe publication. */
int cbm_zova_sidecar_generation_get(const char *zova_path, int64_t *out_generation);
int cbm_zova_after_sqlite_dump(const char *db_path);
int cbm_zova_after_sqlite_dump_with_i8_vectors(const char *db_path,
                                                const CBMDumpVector *node_vectors,
                                                int node_vector_count,
                                                const CBMDumpTokenVec *token_vectors,
                                                int token_vector_count, int vector_dim);
int cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
    const char *db_path, const char *root_path, const char *project,
    const CBMDumpVector *node_vectors, int node_vector_count,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim);
int cbm_zova_after_sqlite_dump_workspace_direct(
    const char *db_path, const char *root_path, const char *project,
    const CBMDumpNode *nodes, int node_count, const CBMDumpEdge *edges, int edge_count,
    const CBMDumpVector *node_vectors, int node_vector_count,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim);
int cbm_zova_validate_container(const char *zova_path);
int cbm_zova_probe_sql_capabilities(const char *zova_path,
                                    cbm_zova_sql_capabilities_t *out_capabilities);
/* Bootstrap the versioned SQL schema for the eventual user-local cbm.zova
 * database. This is additive and idempotent: it creates empty workspace-
 * partitioned metadata/FTS tables but does not import or delete any project
 * artifacts. */
int cbm_zova_user_database_init(const char *zova_path);
#if CBM_WITH_ZOVA
int cbm_zova_user_database_verify_workspace_schema(zova_database *db);
#endif
int cbm_zova_user_database_generation_begin(const char *zova_path, const char *root_path,
                                             int64_t generation, char *out_workspace_id,
                                             size_t out_workspace_id_size);
int cbm_zova_user_database_generation_finish(const char *zova_path, const char *workspace_id,
                                              int64_t generation, bool ready);
typedef struct {
    const char *file_path;
    const char *content_hash;
    int64_t mtime_ns;
    int64_t size_bytes;
} cbm_zova_file_hash_input_t;

typedef struct {
    bool present;
    const char *summary;
    const char *source_hash;
    const char *created_at;
    const char *updated_at;
} cbm_zova_project_summary_input_t;

typedef struct {
    const char *root_path;
    const char *project;
    const char *indexed_at;
    const char *model_fingerprint;
    int vector_dimensions;
    const CBMDumpNode *nodes;
    int node_count;
    const CBMDumpEdge *edges;
    int edge_count;
    const CBMDumpVector *node_vectors;
    int node_vector_count;
    const CBMDumpTokenVec *token_vectors;
    int token_vector_count;
    const cbm_zova_file_hash_input_t *file_hashes;
    int file_hash_count;
    cbm_zova_project_summary_input_t project_summary;
} cbm_zova_workspace_generation_input_t;

typedef enum {
    CBM_ZOVA_PUBLICATION_MODE_UNKNOWN = 0,
    CBM_ZOVA_PUBLICATION_MODE_FULL = 1,
    CBM_ZOVA_PUBLICATION_MODE_DELTA = 2,
} cbm_zova_publication_mode_t;

typedef struct {
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    int64_t generation;
    uint64_t graph_nodes;
    uint64_t graph_edges;
    uint64_t metadata_nodes;
    uint64_t metadata_edges;
    uint64_t metadata_topology_edges;
    uint64_t fts_rows;
    uint64_t node_vector_rows;
    uint64_t token_vector_rows;
    uint64_t node_vectors;
    uint64_t token_vectors;
    char metadata_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char fts_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char topology_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char node_vector_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char token_vector_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    double publish_ms;
    uint64_t database_bytes;
    uint64_t wal_bytes_before;
    uint64_t wal_bytes_after;
    cbm_zova_publication_mode_t publication_mode;
    uint64_t inserted_count;
    uint64_t updated_count;
    uint64_t deleted_count;
    double normalization_ms;
    double diff_ms;
    double model_nodes_ms;
    double model_edges_ms;
    double model_edge_endpoint_ms;
    double model_edge_sort_ms;
    double model_edge_group_ms;
    double model_edge_payload_ms;
    double model_edge_digest_ms;
    uint64_t model_edge_default_payloads;
    uint64_t model_edge_payload_scratch_edges;
    double model_hashes_ms;
    double model_vectors_ms;
    double model_digests_ms;
    double writer_guard_ms;
    double database_init_ms;
    double database_open_ms;
    double transaction_begin_ms;
    double transaction_body_ms;
    double transaction_commit_ms;
    double database_close_ms;
    double clear_ms;
    double finalize_ms;
    double canonical_files_ms;
    double canonical_nodes_ms;
    double canonical_edges_ms;
    double canonical_hashes_ms;
    double fts_ms;
    double token_metadata_ms;
    double native_graph_ms;
    double native_graph_materialize_ms;
    double native_graph_reset_ms;
    double native_graph_nodes_ms;
    double native_graph_edges_ms;
    double native_graph_validate_ms;
    double native_graph_key_generation_ms;
    double native_graph_cleanup_ms;
    double native_vectors_ms;
    double fresh_validation_ms;
    double fresh_index_ms;
    double fresh_commit_ms;
    double fresh_build_ms;
    double readback_ms;
    uint64_t full_clear_count;
    uint64_t unchanged_rewrite_count;
    uint64_t nodes_inserted;
    uint64_t nodes_updated;
    uint64_t nodes_deleted;
    uint64_t edges_inserted;
    uint64_t edges_deleted;
    uint64_t node_vectors_upserted;
    uint64_t node_vectors_deleted;
    uint64_t token_vectors_upserted;
    uint64_t token_vectors_deleted;
} cbm_zova_workspace_generation_result_t;

typedef struct {
    uint64_t rows;
    uint64_t bind_i64_calls;
    uint64_t bind_text_calls;
    uint64_t bind_double_calls;
    uint64_t step_calls;
    uint64_t reset_calls;
    uint64_t clear_bindings_calls;
} cbm_zova_statement_phase_metrics_t;

typedef struct {
    uint64_t database_open_count;
    uint64_t database_close_count;
    uint64_t database_handle_open_count;
    uint64_t fresh_page_size_vacuum_count;
    uint64_t transaction_count;
    uint64_t full_clear_count;
    uint64_t canonical_node_fts_passes;
    uint64_t canonical_edge_passes;
    uint64_t native_graph_fresh_calls;
    uint64_t native_graph_prepared_calls;
    uint64_t native_graph_node_calls;
    uint64_t native_graph_edge_calls;
    uint64_t native_node_vector_calls;
    uint64_t native_token_vector_calls;
    uint64_t integrity_writes;
    uint64_t delta_authoritative_rows_touched;
    uint64_t delta_clear_violation_count;
    uint64_t delta_file_key_resolutions;
    uint64_t delta_endpoint_key_lookups;
    cbm_zova_statement_phase_metrics_t canonical_files_sql;
    cbm_zova_statement_phase_metrics_t canonical_nodes_sql;
    cbm_zova_statement_phase_metrics_t canonical_edges_sql;
    cbm_zova_statement_phase_metrics_t canonical_hashes_sql;
    cbm_zova_statement_phase_metrics_t canonical_token_metadata_sql;
    uint64_t full_fts_bulk_statements;
    uint64_t full_fts_trigger_rows_avoided;
    uint64_t full_node_guard_validation_statements;
    uint64_t full_edge_guard_validation_statements;
    uint64_t readback_count_scan_count;
} cbm_zova_publish_test_metrics_t;

void cbm_zova_publish_test_metrics_reset(void);
void cbm_zova_publish_test_metrics_get(cbm_zova_publish_test_metrics_t *out_metrics);

#if CBM_WITH_ZOVA
/* Caller-owned transaction primitive. It never begins, commits, or rolls
 * back; the caller remains responsible for transaction lifetime. */
int cbm_zova_user_database_schema_is_current(zova_database *db);
int cbm_zova_user_database_publish_workspace_tx(
    zova_database *db, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result);
int cbm_zova_user_database_publish_model_tx(
    zova_database *db, const cbm_zova_publish_model_t *model,
    int64_t requested_generation,
    cbm_zova_workspace_generation_result_t *out_result);
#endif
int cbm_zova_user_database_publish_model(
    const char *zova_path, const cbm_zova_publish_model_t *model,
    cbm_zova_workspace_generation_result_t *out_result);
int cbm_zova_user_database_publish_prepared_view(
    const char *zova_path, const cbm_zova_prepared_view_t *view,
    cbm_zova_workspace_generation_result_t *out_result);
int cbm_zova_user_database_publish_delta(
    const char *zova_path, const cbm_zova_publish_model_t *after,
    const cbm_zova_workspace_delta_t *delta,
    cbm_zova_workspace_generation_result_t *out_result);
int cbm_zova_workspace_generation_digest_input(
    const char *workspace_id, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result);
/* Opens the user-local database, publishes one complete workspace generation
 * in one transaction, and commits only after all validation succeeds. */
int cbm_zova_user_database_publish_workspace(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result);
/* Controlled operations path: publishes exactly requested_generation in the
 * normal single transaction. The requested generation must be positive and
 * strictly greater than the workspace's current maximum generation. */
int cbm_zova_user_database_publish_workspace_at_generation(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    int64_t requested_generation, cbm_zova_workspace_generation_result_t *out_result);
int cbm_zova_user_database_delete_workspace(const char *zova_path,
                                            const char *workspace_id);
/* Staging-only importer for one finalized workspace. It writes rich node/edge
 * metadata and canonical FTS rows into the user-local file;
 * normal indexing remains on the project .db plus sidecar until parity gates
 * promote this boundary. */
int cbm_zova_user_database_import_workspace(
    const char *zova_path, const char *root_path, const char *project, int64_t generation,
    const CBMDumpNode *nodes, int node_count, const CBMDumpEdge *edges, int edge_count);
int cbm_zova_mirror_i8_vectors(const char *zova_path, int vector_dim);
int cbm_zova_write_i8_vectors_direct(const char *zova_path,
                                     const CBMDumpVector *node_vectors, int node_vector_count,
                                     const CBMDumpTokenVec *token_vectors, int token_vector_count,
                                     int vector_dim);
/* Future shared-file path: one raw-i8 node collection per workspace/model/dimension. */
int cbm_zova_write_workspace_node_i8_vectors_direct(
    const char *zova_path, const char *workspace_id, const char *model_fingerprint,
    const CBMDumpVector *node_vectors, int node_vector_count, int vector_dim);
int cbm_zova_delete_workspace_node_i8_vectors(const char *zova_path, const char *workspace_id,
                                               const char *model_fingerprint, int vector_dim);
int cbm_zova_write_workspace_token_i8_vectors_direct(
    const char *zova_path, const char *workspace_id, const char *model_fingerprint,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim);
int cbm_zova_delete_workspace_token_i8_vectors(const char *zova_path, const char *workspace_id,
                                                const char *model_fingerprint, int vector_dim);
int cbm_zova_mirror_graph(const char *zova_path);
/* Experimental workspace-scoped topology mirror. The existing SQLite project
 * rows remain authoritative; this only writes a scoped Zova graph for parity. */
int cbm_zova_mirror_workspace_graph(const char *zova_path, const char *workspace_id,
                                    const char *project);
/* Write one workspace's native topology from CBM's finalized dump arrays.
 * This path never scans SQLite nodes or edges. */
int cbm_zova_write_workspace_graph_direct(const char *zova_path, const char *workspace_id,
                                          const char *project, const CBMDumpNode *nodes,
                                          int node_count, const CBMDumpEdge *edges,
                                          int edge_count);
/* Remove one workspace's scoped native topology only. */
int cbm_zova_delete_workspace_graph(const char *zova_path, const char *workspace_id);
/* Benchmark only workspace graph materialization. Each destination is first
 * converted from db_path outside the timers; caller owns cleanup of both. */
int cbm_zova_benchmark_workspace_graph_ingestion(
    const char *db_path, const char *direct_zova_path, const char *mirror_zova_path,
    const char *workspace_id, const char *project, const CBMDumpNode *nodes, int node_count,
    const CBMDumpEdge *edges, int edge_count, cbm_zova_graph_ingestion_metrics_t *out_metrics);
bool cbm_zova_should_use_full_vector_search(int candidate_count, int collection_count);

#if CBM_WITH_ZOVA
int cbm_zova_register_sql_functions(zova_database *db);
#endif

int cbm_zova_vector_prefetch_nodes(const char *zova_path, const char *project,
                                   const int8_t *query, int vector_dim, int limit,
                                   cbm_zova_node_candidate_t **out, int *out_count);
int cbm_zova_vector_prefetch_nodes_ex(const char *zova_path, const char *project,
                                      const int8_t *query, int vector_dim, int limit,
                                      cbm_zova_node_candidate_t **out, int *out_count,
                                      cbm_zova_vector_prefetch_metrics_t *metrics);
cbm_zova_vector_session_t *cbm_zova_vector_session_open(const char *zova_path);
void cbm_zova_vector_session_close(cbm_zova_vector_session_t *session);
int cbm_zova_vector_session_generation(const cbm_zova_vector_session_t *session,
                                       int64_t *out_generation);
int cbm_zova_vector_session_has_workspace(const cbm_zova_vector_session_t *session,
                                          const char *workspace_id, bool *out_present);
int cbm_zova_vector_session_prefetch_nodes_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query, int vector_dim,
    int limit, bool include_vector, cbm_zova_node_candidate_t **out, int *out_count,
    cbm_zova_vector_prefetch_metrics_t *metrics);
int cbm_zova_vector_session_prefetch_multi_i8_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_node_candidate_t **out, int *out_count, cbm_zova_vector_prefetch_metrics_t *metrics);
int cbm_zova_vector_session_search_collection_i8(
    cbm_zova_vector_session_t *session, const char *collection, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_vector_hit_t **out, int *out_count);
int cbm_zova_vector_session_get_workspace_token_i8(
    cbm_zova_vector_session_t *session, const char *workspace_id,
    const char *model_fingerprint, int vector_dim, const char *token,
    int8_t *out_values, size_t out_len, bool *out_found);
void cbm_zova_vector_hits_free(cbm_zova_vector_hit_t *items, int count);
void cbm_zova_node_candidates_free(cbm_zova_node_candidate_t *items, int count);

cbm_zova_graph_session_t *cbm_zova_graph_session_open(const char *zova_path);
void cbm_zova_graph_session_close(cbm_zova_graph_session_t *session);
int cbm_zova_graph_session_generation(const cbm_zova_graph_session_t *session,
                                      int64_t *out_generation);
int cbm_zova_graph_session_walk_calls(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *start_node_id, const char *direction, int max_depth, int max_results,
    cbm_zova_graph_visit_t **out_visits, int *out_count, cbm_zova_graph_metrics_t *out_metrics);
int cbm_zova_graph_session_adjacency(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *node_id, const char *direction, const char *const *edge_types,
    int edge_type_count, int max_results, cbm_zova_graph_adjacency_t **out, int *out_count);
int cbm_zova_graph_session_degree(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *node_id, const char *direction, const char *const *edge_types,
    int edge_type_count, int *out_degree);
int cbm_zova_graph_session_adjacency_prepare_count(
    const cbm_zova_graph_session_t *session);
void cbm_zova_graph_adjacency_free(cbm_zova_graph_adjacency_t *items, int count);
void cbm_zova_graph_visits_free(cbm_zova_graph_visit_t *visits, int count);

int cbm_zova_graph_neighbor_count(const char *zova_path, const char *node_id, int *out_count);

#endif /* CBM_ZOVA_H */
