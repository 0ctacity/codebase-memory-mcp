#ifndef CBM_ZOVA_REPOSITORY_H
#define CBM_ZOVA_REPOSITORY_H

#include "store/store.h"
#include "zova/cbm_zova.h"

typedef struct cbm_zova_repository cbm_zova_repository_t;

typedef uint32_t cbm_zova_snapshot_components_t;

enum {
    CBM_ZOVA_SNAPSHOT_COMPONENT_NONE = 0,
    CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY = 1u << 0,
    CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS = 1u << 1,
    CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS = 1u << 2,
    CBM_ZOVA_SNAPSHOT_COMPONENT_ALL =
        CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY |
        CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
        CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS,
};

typedef enum {
    CBM_ZOVA_SNAPSHOT_OK = 0,
    CBM_ZOVA_SNAPSHOT_ERROR = -1,
    CBM_ZOVA_SNAPSHOT_STALE = -2,
} cbm_zova_snapshot_status_t;

typedef struct {
    double base_ms;
    double optional_ms;
    cbm_zova_snapshot_components_t hydrated_components;
    uint64_t topology_rows;
    uint64_t node_vector_rows;
    uint64_t token_vector_rows;
} cbm_zova_snapshot_metrics_t;

typedef struct {
    char *source_stable_id;
    char *edge_type;
    char *target_stable_id;
} cbm_zova_snapshot_topology_edge_t;

typedef struct cbm_zova_workspace_snapshot {
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char *root_path;
    char *project;
    char *indexed_at;
    char *model_fingerprint;
    int vector_dimensions;
    int64_t generation;
    CBMDumpNode *nodes;
    char **node_stable_ids;
    uint64_t *node_source_ordinals;
    int node_count;
    CBMDumpEdge *edges;
    char **edge_ids;
    int edge_count;
    cbm_zova_snapshot_topology_edge_t *topology_edges;
    int topology_edge_count;
    CBMDumpVector *node_vectors;
    char **node_vector_ids;
    int node_vector_count;
    CBMDumpTokenVec *token_vectors;
    char **token_vector_ids;
    int token_vector_count;
    cbm_zova_file_hash_input_t *file_hashes;
    int file_hash_count;
    cbm_zova_project_summary_input_t project_summary;
    cbm_zova_workspace_generation_result_t integrity;
    cbm_zova_snapshot_components_t hydrated_components;
    cbm_zova_snapshot_metrics_t metrics;
} cbm_zova_workspace_snapshot_t;

cbm_zova_repository_t *cbm_zova_repository_open(const char *path, const char *project);
void cbm_zova_repository_close(cbm_zova_repository_t *repo);
const char *cbm_zova_repository_workspace_id(const cbm_zova_repository_t *repo);
int cbm_zova_repository_get_project(cbm_zova_repository_t *repo, const char *workspace_id,
                                    cbm_project_t *out);
int cbm_zova_repository_find_node_by_qn(cbm_zova_repository_t *repo, const char *workspace_id,
                                         const char *qualified_name, cbm_node_t *out);
int cbm_zova_repository_find_node_by_stable_id(cbm_zova_repository_t *repo,
                                                const char *workspace_id,
                                                const char *stable_id, cbm_node_t *out);
int cbm_zova_repository_find_edges(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *stable_id,
                                   const char *direction, cbm_edge_t **out, int *out_count);
int cbm_zova_repository_search(cbm_zova_repository_t *repo, const char *workspace_id,
                               const cbm_search_params_t *params, cbm_search_output_t *out);
int cbm_zova_repository_search_fts(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *query,
                                   const char *file_pattern, int limit, int offset,
                                   cbm_search_output_t *out);
int cbm_zova_repository_index_status(cbm_zova_repository_t *repo, const char *workspace_id,
                                      int64_t *generation,
                                      char **indexed_at);
int cbm_zova_repository_counts(cbm_zova_repository_t *repo, const char *workspace_id,
                               int *nodes, int *edges);
int cbm_zova_repository_project_summary(cbm_zova_repository_t *repo,
                                        const char *workspace_id,
                                        cbm_project_summary_export_t *out);
int cbm_zova_repository_export_snapshot(const char *zova_path, const char *workspace_id,
                                        cbm_zova_workspace_snapshot_t *out);
/* Export the complete committed baseline needed to calculate an exact
 * incremental delta, including the authoritative native vector payloads. */
int cbm_zova_repository_export_incremental_snapshot(
    const char *zova_path, const char *workspace_id, cbm_zova_workspace_snapshot_t *out);
int cbm_zova_repository_hydrate_incremental_components(
    const char *zova_path, const char *workspace_id, int64_t expected_generation,
    cbm_zova_snapshot_components_t requested, cbm_zova_workspace_snapshot_t *snapshot);
void cbm_zova_workspace_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot);

#endif
