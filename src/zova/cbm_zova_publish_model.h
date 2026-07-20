#ifndef CBM_ZOVA_PUBLISH_MODEL_H
#define CBM_ZOVA_PUBLISH_MODEL_H

#include "cbm_zova.h"

typedef struct cbm_zova_publish_model cbm_zova_publish_model_t;
/* Full-publication view over finalized dense dump arrays. The concrete
 * storage is shared with the row consumer, but unlike a publication model it
 * does not materialize logical edge IDs or delta-planning edge rows. */
typedef struct cbm_zova_publish_model cbm_zova_prepared_view_t;

typedef struct {
    const CBMDumpNode *source;
    const char *stable_id;
    uint64_t ordinal;
    uint64_t source_ordinal;
} cbm_zova_publish_node_t;

typedef struct {
    const CBMDumpEdge *source;
    const char *edge_id;
    const char *source_stable_id;
    const char *target_stable_id;
    uint64_t source_ordinal;
    uint64_t target_ordinal;
    uint64_t topology_ordinal;
    uint64_t ordinal;
    const uint8_t *payload;
    size_t payload_len;
    uint64_t logical_edge_count;
} cbm_zova_publish_edge_t;

typedef struct {
    const CBMDumpVector *source;
    const char *stable_id;
    uint64_t node_ordinal;
    uint64_t ordinal;
} cbm_zova_publish_node_vector_t;

typedef struct {
    const CBMDumpTokenVec *source;
    const char *token_id;
    uint64_t ordinal;
} cbm_zova_publish_token_vector_t;

typedef struct {
    uint64_t stable_node_id_computations;
    uint64_t edge_id_computations;
    uint64_t token_id_computations;
    uint64_t camel_split_computations;
    uint64_t endpoint_lookups;
    uint64_t global_sorts;
    uint64_t digest_row_revisit_count;
    double normalization_ms;
    double nodes_ms;
    double edges_ms;
    double hashes_ms;
    double vectors_ms;
    double digests_ms;
} cbm_zova_publish_model_metrics_t;

int cbm_zova_publish_model_build(
    const char *workspace_id,
    const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_publish_model_t **out_model);
int cbm_zova_prepared_view_build(
    const char *workspace_id,
    const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_prepared_view_t **out_view);
int cbm_zova_prepared_view_build_base(
    const char *workspace_id,
    const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_prepared_view_t **out_view);
int cbm_zova_prepared_view_complete(
    cbm_zova_prepared_view_t *view,
    const cbm_zova_file_hash_input_t *file_hashes,
    int file_hash_count,
    const cbm_zova_project_summary_input_t *project_summary);
void cbm_zova_prepared_view_free(cbm_zova_prepared_view_t *view);
void cbm_zova_publish_model_free(cbm_zova_publish_model_t *model);

const char *cbm_zova_publish_model_workspace_id(const cbm_zova_publish_model_t *model);
const cbm_zova_workspace_generation_input_t *cbm_zova_publish_model_input(
    const cbm_zova_publish_model_t *model);
int cbm_zova_publish_model_node_count(const cbm_zova_publish_model_t *model);
const cbm_zova_publish_node_t *cbm_zova_publish_model_node_at(
    const cbm_zova_publish_model_t *model, int index);
int cbm_zova_publish_model_set_node_source_ordinal(
    cbm_zova_publish_model_t *model, int index, uint64_t source_ordinal);
const char *cbm_zova_publish_model_fts_name_at(
    const cbm_zova_publish_model_t *model, int index);
const char *cbm_zova_publish_model_stable_id_for_dump_id(
    const cbm_zova_publish_model_t *model, int64_t dump_id);
int cbm_zova_publish_model_edge_count(const cbm_zova_publish_model_t *model);
const cbm_zova_publish_edge_t *cbm_zova_publish_model_edge_at(
    const cbm_zova_publish_model_t *model, int index);
int cbm_zova_publish_model_topology_count(const cbm_zova_publish_model_t *model);
const cbm_zova_publish_edge_t *cbm_zova_publish_model_topology_at(
    const cbm_zova_publish_model_t *model, int index);
int cbm_zova_publish_model_file_hash_count(const cbm_zova_publish_model_t *model);
const cbm_zova_file_hash_input_t *cbm_zova_publish_model_file_hash_at(
    const cbm_zova_publish_model_t *model, int index);
int cbm_zova_publish_model_node_vector_count(const cbm_zova_publish_model_t *model);
const cbm_zova_publish_node_vector_t *cbm_zova_publish_model_node_vector_at(
    const cbm_zova_publish_model_t *model, int index);
int cbm_zova_publish_model_token_vector_count(const cbm_zova_publish_model_t *model);
const cbm_zova_publish_token_vector_t *cbm_zova_publish_model_token_vector_at(
    const cbm_zova_publish_model_t *model, int index);
const cbm_zova_workspace_generation_result_t *cbm_zova_publish_model_digests(
    const cbm_zova_publish_model_t *model);
void cbm_zova_publish_model_metrics(const cbm_zova_publish_model_t *model,
                                    cbm_zova_publish_model_metrics_t *out_metrics);
/* Carry forward authoritative vector digests when an incremental input gate
 * proves that the corresponding derived corpus is unchanged. The current
 * model must contain no rows for every reused component. */
int cbm_zova_publish_model_reuse_vector_digests(
    cbm_zova_publish_model_t *model,
    const cbm_zova_workspace_generation_result_t *previous,
    uint32_t components);

/* Test-only deterministic allocation injector. A non-negative index fails
 * exactly that allocation in the next model build; -1 disables injection. */
void cbm_zova_publish_model_test_fail_allocation_at(int64_t index);
int64_t cbm_zova_publish_model_test_allocation_count(void);
void cbm_zova_publish_model_test_reset_build_count(void);
uint64_t cbm_zova_publish_model_test_build_count(void);

#endif
