#ifndef CBM_ZOVA_H
#define CBM_ZOVA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../internal/cbm/sqlite_writer.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
typedef struct zova_database zova_database;
#endif

typedef enum {
    CBM_ZOVA_MODE_OFF = 0,
    CBM_ZOVA_MODE_CONTAINER = 1,
    CBM_ZOVA_MODE_I8_VECTORS = 2,
    CBM_ZOVA_MODE_GRAPH_MIRROR = 3,
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

typedef struct cbm_zova_vector_session cbm_zova_vector_session_t;

typedef struct {
    bool used_full_search;
    double open_ms;
    double candidate_count_ms;
    double candidate_id_collection_ms;
    double vector_search_ms;
    double hydration_ms;
} cbm_zova_vector_prefetch_metrics_t;

#define CBM_ZOVA_NODE_COLLECTION "cbm_node_vectors_i8"
#define CBM_ZOVA_TOKEN_COLLECTION "cbm_token_vectors_i8"
#define CBM_ZOVA_CODE_GRAPH "cbm_code_graph"
#define CBM_ZOVA_WORKSPACE_ID_MAX 80

bool cbm_zova_build_enabled(void);
cbm_zova_mode_t cbm_zova_mode_from_env(void);
const char *cbm_zova_mode_name(cbm_zova_mode_t mode);

int cbm_zova_sidecar_path(const char *db_path, char *out, size_t out_sz);
int cbm_zova_workspace_registry_path(char *out, size_t out_size);
int cbm_zova_workspace_graph_name(const char *workspace_id, char *out, size_t out_size);
int cbm_zova_workspace_node_vector_collection_name(const char *workspace_id,
                                                    const char *model_fingerprint, int dimensions,
                                                    char *out, size_t out_size);
int cbm_zova_workspace_node_id_v1(const char *workspace_id, const char *node_kind,
                                  const char *relative_path, const char *qualified_name,
                                  const char *semantic_discriminator, char *out, size_t out_size);
int cbm_zova_workspace_get_or_create_at(const char *registry_path, const char *root_path,
                                        char *out_workspace_id, size_t out_workspace_id_size);
int cbm_zova_workspace_generation_begin_at(const char *registry_path, const char *workspace_id,
                                            int64_t generation);
int cbm_zova_workspace_generation_finish_at(const char *registry_path, const char *workspace_id,
                                             int64_t generation, bool ready);
int cbm_zova_workspace_active_generation_at(const char *registry_path, const char *workspace_id,
                                             int64_t *out_generation);
int cbm_zova_after_sqlite_dump(const char *db_path);
int cbm_zova_after_sqlite_dump_with_i8_vectors(const char *db_path,
                                                const CBMDumpVector *node_vectors,
                                                int node_vector_count,
                                                const CBMDumpTokenVec *token_vectors,
                                                int token_vector_count, int vector_dim);
int cbm_zova_validate_container(const char *zova_path);
int cbm_zova_mirror_i8_vectors(const char *zova_path, int vector_dim);
int cbm_zova_write_i8_vectors_direct(const char *zova_path,
                                     const CBMDumpVector *node_vectors, int node_vector_count,
                                     const CBMDumpTokenVec *token_vectors, int token_vector_count,
                                     int vector_dim);
int cbm_zova_mirror_graph(const char *zova_path);
/* Experimental workspace-scoped topology mirror. The existing SQLite project
 * rows remain authoritative; this only writes a scoped Zova graph for parity. */
int cbm_zova_mirror_workspace_graph(const char *zova_path, const char *workspace_id,
                                    const char *project);
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
int cbm_zova_vector_session_prefetch_nodes_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query, int vector_dim,
    int limit, bool include_vector, cbm_zova_node_candidate_t **out, int *out_count,
    cbm_zova_vector_prefetch_metrics_t *metrics);
int cbm_zova_vector_session_prefetch_multi_i8_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_node_candidate_t **out, int *out_count, cbm_zova_vector_prefetch_metrics_t *metrics);
void cbm_zova_node_candidates_free(cbm_zova_node_candidate_t *items, int count);

int cbm_zova_graph_neighbor_count(const char *zova_path, const char *node_id, int *out_count);

#endif /* CBM_ZOVA_H */
