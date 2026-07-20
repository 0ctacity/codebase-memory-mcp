#ifndef CBM_ZOVA_DELTA_H
#define CBM_ZOVA_DELTA_H

#include "cbm_zova_publish_model.h"
#include "cbm_zova_repository.h"

typedef struct cbm_zova_workspace_delta cbm_zova_workspace_delta_t;

typedef struct {
    const char *source_stable_id;
    const char *edge_type;
    const char *target_stable_id;
} cbm_zova_delta_topology_edge_t;

typedef struct {
    const char *edge_id;
    const char *source_stable_id;
    const char *edge_type;
    const char *target_stable_id;
    const char *local_name;
} cbm_zova_delta_edge_delete_t;

typedef struct {
    uint64_t node_inserts;
    uint64_t node_updates;
    uint64_t node_deletes;
    uint64_t edge_inserts;
    uint64_t edge_deletes;
    uint64_t topology_inserts;
    uint64_t topology_deletes;
    uint64_t node_vector_upserts;
    uint64_t node_vector_deletes;
    uint64_t token_vector_upserts;
    uint64_t token_vector_deletes;
    uint64_t file_hash_upserts;
    uint64_t file_hash_deletes;
    uint64_t summary_replacements;
    double diff_ms;
} cbm_zova_workspace_delta_metrics_t;

int cbm_zova_workspace_delta_build(
    const cbm_zova_workspace_snapshot_t *before,
    cbm_zova_publish_model_t *after,
    cbm_zova_workspace_delta_t **out_delta);
int cbm_zova_workspace_delta_required_components(
    const cbm_zova_workspace_snapshot_t *before,
    const cbm_zova_publish_model_t *after,
    cbm_zova_snapshot_components_t *out_components);
int64_t cbm_zova_workspace_delta_expected_generation(
    const cbm_zova_workspace_delta_t *delta);
void cbm_zova_workspace_delta_free(cbm_zova_workspace_delta_t *delta);

void cbm_zova_workspace_delta_metrics(
    const cbm_zova_workspace_delta_t *delta,
    cbm_zova_workspace_delta_metrics_t *out_metrics);

const cbm_zova_publish_node_t *cbm_zova_workspace_delta_node_insert_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_publish_node_t *cbm_zova_workspace_delta_node_update_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const char *cbm_zova_workspace_delta_node_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_publish_edge_t *cbm_zova_workspace_delta_edge_insert_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_delta_edge_delete_t *cbm_zova_workspace_delta_edge_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_delta_topology_edge_t *cbm_zova_workspace_delta_topology_insert_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_delta_topology_edge_t *cbm_zova_workspace_delta_topology_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_publish_node_vector_t *cbm_zova_workspace_delta_node_vector_upsert_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const char *cbm_zova_workspace_delta_node_vector_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_publish_token_vector_t *cbm_zova_workspace_delta_token_vector_upsert_at(
    const cbm_zova_workspace_delta_t *delta, int index);
/* Internal physical delete identity: the canonical token string, not its public digest ID. */
const char *cbm_zova_workspace_delta_token_vector_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const cbm_zova_file_hash_input_t *cbm_zova_workspace_delta_file_hash_upsert_at(
    const cbm_zova_workspace_delta_t *delta, int index);
const char *cbm_zova_workspace_delta_file_hash_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index);
bool cbm_zova_workspace_delta_replaces_summary(const cbm_zova_workspace_delta_t *delta);
const cbm_zova_project_summary_input_t *cbm_zova_workspace_delta_summary(
    const cbm_zova_workspace_delta_t *delta);

#endif
