#include "cbm_zova_delta.h"

#include "foundation/compat.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct cbm_zova_workspace_delta {
    const cbm_zova_publish_node_t **node_inserts;
    const cbm_zova_publish_node_t **node_updates;
    char **node_deletes;
    const cbm_zova_publish_edge_t **edge_inserts;
    char **edge_deletes;
    cbm_zova_delta_topology_edge_t *topology_inserts;
    cbm_zova_delta_topology_edge_t *topology_deletes;
    const cbm_zova_publish_node_vector_t **node_vector_upserts;
    char **node_vector_deletes;
    const cbm_zova_publish_token_vector_t **token_vector_upserts;
    char **token_vector_deletes;
    const cbm_zova_file_hash_input_t **file_hash_upserts;
    char **file_hash_deletes;
    bool replace_summary;
    const cbm_zova_project_summary_input_t *summary;
    int64_t expected_generation;
    cbm_zova_workspace_delta_metrics_t metrics;
};

static int text_equal(const char *left, const char *right) {
    return strcmp(left ? left : "", right ? right : "") == 0;
}

static char *delta_dup(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, value, length + 1);
    return copy;
}

static double delta_elapsed_ms(struct timespec started) {
    struct timespec finished;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    return (double)(finished.tv_sec - started.tv_sec) * 1000.0 +
           (double)(finished.tv_nsec - started.tv_nsec) / 1000000.0;
}

static int sorted_unique(char *const *ids, int count) {
    if (count < 0 || (count > 0 && !ids)) return 0;
    for (int i = 0; i < count; i++) {
        if (!ids[i] || (i > 0 && strcmp(ids[i - 1], ids[i]) >= 0)) return 0;
    }
    return 1;
}

static int digest_valid(const char digest[CBM_ZOVA_DIGEST_HEX_SIZE]) {
    if (!digest || strlen(digest) != CBM_ZOVA_DIGEST_HEX_SIZE - 1) return 0;
    for (size_t i = 0; i < CBM_ZOVA_DIGEST_HEX_SIZE - 1; i++) {
        unsigned char c = (unsigned char)digest[i];
        if (!isdigit(c) && !(c >= 'a' && c <= 'f')) return 0;
    }
    return 1;
}

int cbm_zova_workspace_delta_required_components(
    const cbm_zova_workspace_snapshot_t *before,
    const cbm_zova_publish_model_t *after,
    cbm_zova_snapshot_components_t *out_components) {
    if (!before || !after || !out_components) return -1;
    const cbm_zova_workspace_generation_result_t *digests =
        cbm_zova_publish_model_digests(after);
    if (!digests || !digest_valid(before->integrity.topology_sha256) ||
        !digest_valid(before->integrity.node_vector_sha256) ||
        !digest_valid(before->integrity.token_vector_sha256) ||
        !digest_valid(digests->topology_sha256) ||
        !digest_valid(digests->node_vector_sha256) ||
        !digest_valid(digests->token_vector_sha256)) return -1;
    cbm_zova_snapshot_components_t components = CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    if (strcmp(before->integrity.topology_sha256, digests->topology_sha256) != 0)
        components |= CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY;
    if (strcmp(before->integrity.node_vector_sha256, digests->node_vector_sha256) != 0)
        components |= CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS;
    if (strcmp(before->integrity.token_vector_sha256, digests->token_vector_sha256) != 0)
        components |= CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS;
    *out_components = components;
    return 0;
}

static int node_equal(const cbm_zova_workspace_snapshot_t *before, int before_index,
                      const cbm_zova_publish_node_t *after) {
    const CBMDumpNode *left = &before->nodes[before_index];
    const CBMDumpNode *right = after->source;
    return text_equal(left->project, right->project) && text_equal(left->label, right->label) &&
           text_equal(left->name, right->name) &&
           text_equal(left->qualified_name, right->qualified_name) &&
           text_equal(left->file_path, right->file_path) &&
           left->start_line == right->start_line && left->end_line == right->end_line &&
           text_equal(left->properties, right->properties);
}

static const char *snapshot_edge_endpoint(const cbm_zova_workspace_snapshot_t *snapshot,
                                          int64_t dump_id) {
    return dump_id > 0 && dump_id <= snapshot->node_count
               ? snapshot->node_stable_ids[dump_id - 1]
               : NULL;
}

static int edge_equal(const cbm_zova_workspace_snapshot_t *before, int before_index,
                      const cbm_zova_publish_edge_t *after) {
    const CBMDumpEdge *left = &before->edges[before_index];
    const CBMDumpEdge *right = after->source;
    return text_equal(snapshot_edge_endpoint(before, left->source_id),
                      after->source_stable_id) &&
           text_equal(snapshot_edge_endpoint(before, left->target_id),
                      after->target_stable_id) &&
           text_equal(left->project, right->project) && text_equal(left->type, right->type) &&
           text_equal(left->properties, right->properties) &&
           text_equal(left->url_path, right->url_path) &&
           text_equal(left->local_name, right->local_name);
}

static int topology_compare(const char *left_source, const char *left_type,
                            const char *left_target, const char *right_source,
                            const char *right_type, const char *right_target) {
    int order = strcmp(left_source, right_source);
    if (order == 0) order = strcmp(left_type, right_type);
    if (order == 0) order = strcmp(left_target, right_target);
    return order;
}

static int vector_equal(const CBMDumpVector *left, const CBMDumpVector *right) {
    return text_equal(left->project, right->project) && left->vector_len == right->vector_len &&
           left->vector_len >= 0 &&
           (left->vector_len == 0 ||
            (left->vector && right->vector &&
             memcmp(left->vector, right->vector, (size_t)left->vector_len) == 0));
}

static int token_vector_equal(const CBMDumpTokenVec *left, const CBMDumpTokenVec *right) {
    return text_equal(left->project, right->project) && text_equal(left->token, right->token) &&
           left->idf == right->idf && left->vector_len == right->vector_len &&
           left->vector_len >= 0 &&
           (left->vector_len == 0 ||
            (left->vector && right->vector &&
             memcmp(left->vector, right->vector, (size_t)left->vector_len) == 0));
}

static int hash_equal(const cbm_zova_file_hash_input_t *left,
                      const cbm_zova_file_hash_input_t *right) {
    return text_equal(left->file_path, right->file_path) &&
           text_equal(left->content_hash, right->content_hash) &&
           left->mtime_ns == right->mtime_ns && left->size_bytes == right->size_bytes;
}

static int summary_equal(const cbm_zova_project_summary_input_t *left,
                         const cbm_zova_project_summary_input_t *right) {
    return left->present == right->present &&
           (!left->present ||
            (text_equal(left->summary, right->summary) &&
             text_equal(left->source_hash, right->source_hash) &&
             text_equal(left->created_at, right->created_at) &&
             text_equal(left->updated_at, right->updated_at)));
}

static int allocate_arrays(cbm_zova_workspace_delta_t *delta,
                           const cbm_zova_workspace_snapshot_t *before,
                           const cbm_zova_publish_model_t *after,
                           cbm_zova_snapshot_components_t required) {
#define DELTA_ALLOC(field, count, type) \
    do { \
        delta->field = (count) > 0 ? calloc((size_t)(count), sizeof(type)) : NULL; \
        if ((count) > 0 && !delta->field) return -1; \
    } while (0)
    DELTA_ALLOC(node_inserts, cbm_zova_publish_model_node_count(after), *delta->node_inserts);
    DELTA_ALLOC(node_updates, cbm_zova_publish_model_node_count(after), *delta->node_updates);
    DELTA_ALLOC(node_deletes, before->node_count, *delta->node_deletes);
    DELTA_ALLOC(edge_inserts, cbm_zova_publish_model_edge_count(after), *delta->edge_inserts);
    DELTA_ALLOC(edge_deletes, before->edge_count + cbm_zova_publish_model_edge_count(after),
                *delta->edge_deletes);
    DELTA_ALLOC(topology_inserts,
                (required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY) ?
                    cbm_zova_publish_model_topology_count(after) : 0,
                *delta->topology_inserts);
    DELTA_ALLOC(topology_deletes,
                (required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY) ?
                    before->topology_edge_count : 0,
                *delta->topology_deletes);
    DELTA_ALLOC(node_vector_upserts,
                (required & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) ?
                    cbm_zova_publish_model_node_vector_count(after) : 0,
                *delta->node_vector_upserts);
    DELTA_ALLOC(node_vector_deletes,
                (required & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) ?
                    before->node_vector_count : 0,
                *delta->node_vector_deletes);
    DELTA_ALLOC(token_vector_upserts,
                (required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) ?
                    cbm_zova_publish_model_token_vector_count(after) : 0,
                *delta->token_vector_upserts);
    DELTA_ALLOC(token_vector_deletes,
                (required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) ?
                    before->token_vector_count : 0,
                *delta->token_vector_deletes);
    DELTA_ALLOC(file_hash_upserts, cbm_zova_publish_model_file_hash_count(after),
                *delta->file_hash_upserts);
    DELTA_ALLOC(file_hash_deletes, before->file_hash_count, *delta->file_hash_deletes);
#undef DELTA_ALLOC
    return 0;
}

static int append_delete(char **values, uint64_t *count, const char *id) {
    char *copy = delta_dup(id);
    if (!copy) return -1;
    values[(*count)++] = copy;
    return 0;
}

static int build_nodes(cbm_zova_workspace_delta_t *delta,
                       const cbm_zova_workspace_snapshot_t *before,
                       cbm_zova_publish_model_t *after) {
    uint64_t next_source_ordinal = 0;
    for (int i = 0; i < before->node_count; i++) {
        if (before->node_source_ordinals[i] == UINT64_MAX) return -1;
        if (before->node_source_ordinals[i] >= next_source_ordinal)
            next_source_ordinal = before->node_source_ordinals[i] + 1;
    }
    int left = 0, right = 0, right_count = cbm_zova_publish_model_node_count(after);
    while (left < before->node_count || right < right_count) {
        const cbm_zova_publish_node_t *node =
            right < right_count ? cbm_zova_publish_model_node_at(after, right) : NULL;
        int order = left >= before->node_count ? 1 : right >= right_count ? -1
                    : strcmp(before->node_stable_ids[left], node->stable_id);
        if (order < 0) {
            if (append_delete(delta->node_deletes, &delta->metrics.node_deletes,
                              before->node_stable_ids[left++]) != 0) return -1;
        } else if (order > 0) {
            if (next_source_ordinal == UINT64_MAX ||
                cbm_zova_publish_model_set_node_source_ordinal(
                    after, right, next_source_ordinal++) != 0) return -1;
            node = cbm_zova_publish_model_node_at(after, right);
            delta->node_inserts[delta->metrics.node_inserts++] = node;
            right++;
        } else {
            bool equal = node_equal(before, left, node);
            uint64_t old_ordinal = before->node_source_ordinals[left];
            uint64_t desired_ordinal = old_ordinal;
            if (!equal && node->source_ordinal != old_ordinal) {
                if (next_source_ordinal == UINT64_MAX) return -1;
                desired_ordinal = next_source_ordinal++;
            }
            if (cbm_zova_publish_model_set_node_source_ordinal(
                    after, right, desired_ordinal) != 0) return -1;
            node = cbm_zova_publish_model_node_at(after, right);
            if (!equal)
                delta->node_updates[delta->metrics.node_updates++] = node;
            left++; right++;
        }
    }
    return 0;
}

static int build_edges(cbm_zova_workspace_delta_t *delta,
                       const cbm_zova_workspace_snapshot_t *before,
                       const cbm_zova_publish_model_t *after) {
    int left = 0, right = 0, right_count = cbm_zova_publish_model_edge_count(after);
    while (left < before->edge_count || right < right_count) {
        const cbm_zova_publish_edge_t *edge =
            right < right_count ? cbm_zova_publish_model_edge_at(after, right) : NULL;
        int order = left >= before->edge_count ? 1 : right >= right_count ? -1
                    : strcmp(before->edge_ids[left], edge->edge_id);
        if (order < 0) {
            if (append_delete(delta->edge_deletes, &delta->metrics.edge_deletes,
                              before->edge_ids[left++]) != 0) return -1;
        } else if (order > 0) {
            delta->edge_inserts[delta->metrics.edge_inserts++] = edge;
            right++;
        } else {
            if (!edge_equal(before, left, edge)) {
                if (append_delete(delta->edge_deletes, &delta->metrics.edge_deletes,
                                  before->edge_ids[left]) != 0) return -1;
                delta->edge_inserts[delta->metrics.edge_inserts++] = edge;
            }
            left++; right++;
        }
    }
    return 0;
}

static int build_topology(cbm_zova_workspace_delta_t *delta,
                          const cbm_zova_workspace_snapshot_t *before,
                          const cbm_zova_publish_model_t *after) {
    int left = 0, right = 0, right_count = cbm_zova_publish_model_topology_count(after);
    while (left < before->topology_edge_count || right < right_count) {
        const cbm_zova_publish_edge_t *edge = right < right_count
            ? cbm_zova_publish_model_topology_at(after, right) : NULL;
        const cbm_zova_snapshot_topology_edge_t *old = left < before->topology_edge_count
            ? &before->topology_edges[left] : NULL;
        int order = left >= before->topology_edge_count ? 1 : right >= right_count ? -1
            : topology_compare(old->source_stable_id, old->edge_type, old->target_stable_id,
                               edge->source_stable_id, edge->source->type,
                               edge->target_stable_id);
        if (order < 0) {
            cbm_zova_delta_topology_edge_t *row =
                &delta->topology_deletes[delta->metrics.topology_deletes++];
            row->source_stable_id = delta_dup(old->source_stable_id);
            row->edge_type = delta_dup(old->edge_type);
            row->target_stable_id = delta_dup(old->target_stable_id);
            if (!row->source_stable_id || !row->edge_type || !row->target_stable_id) return -1;
            left++;
        } else if (order > 0) {
            delta->topology_inserts[delta->metrics.topology_inserts++] =
                (cbm_zova_delta_topology_edge_t){
                    .source_stable_id=edge->source_stable_id,
                    .edge_type=edge->source->type,
                    .target_stable_id=edge->target_stable_id};
            right++;
        } else {
            left++; right++;
        }
    }
    return 0;
}

static int build_node_vectors(cbm_zova_workspace_delta_t *delta,
                              const cbm_zova_workspace_snapshot_t *before,
                              const cbm_zova_publish_model_t *after) {
    int left = 0, right = 0, right_count = cbm_zova_publish_model_node_vector_count(after);
    while (left < before->node_vector_count || right < right_count) {
        const cbm_zova_publish_node_vector_t *vector = right < right_count
            ? cbm_zova_publish_model_node_vector_at(after, right) : NULL;
        int order = left >= before->node_vector_count ? 1 : right >= right_count ? -1
                    : strcmp(before->node_vector_ids[left], vector->stable_id);
        if (order < 0) {
            if (append_delete(delta->node_vector_deletes,
                              &delta->metrics.node_vector_deletes,
                              before->node_vector_ids[left++]) != 0) return -1;
        } else if (order > 0) {
            delta->node_vector_upserts[delta->metrics.node_vector_upserts++] = vector;
            right++;
        } else {
            if (!vector_equal(&before->node_vectors[left], vector->source))
                delta->node_vector_upserts[delta->metrics.node_vector_upserts++] = vector;
            left++; right++;
        }
    }
    return 0;
}

static int build_token_vectors(cbm_zova_workspace_delta_t *delta,
                               const cbm_zova_workspace_snapshot_t *before,
                               const cbm_zova_publish_model_t *after) {
    int left = 0, right = 0, right_count = cbm_zova_publish_model_token_vector_count(after);
    while (left < before->token_vector_count || right < right_count) {
        const cbm_zova_publish_token_vector_t *vector = right < right_count
            ? cbm_zova_publish_model_token_vector_at(after, right) : NULL;
        int order = left >= before->token_vector_count ? 1 : right >= right_count ? -1
                    : strcmp(before->token_vector_ids[left], vector->token_id);
        if (order < 0) {
            if (append_delete(delta->token_vector_deletes,
                              &delta->metrics.token_vector_deletes,
                              before->token_vector_ids[left++]) != 0) return -1;
        } else if (order > 0) {
            delta->token_vector_upserts[delta->metrics.token_vector_upserts++] = vector;
            right++;
        } else {
            if (!token_vector_equal(&before->token_vectors[left], vector->source))
                delta->token_vector_upserts[delta->metrics.token_vector_upserts++] = vector;
            left++; right++;
        }
    }
    return 0;
}

static int build_hashes(cbm_zova_workspace_delta_t *delta,
                        const cbm_zova_workspace_snapshot_t *before,
                        const cbm_zova_publish_model_t *after) {
    int left = 0, right = 0, right_count = cbm_zova_publish_model_file_hash_count(after);
    while (left < before->file_hash_count || right < right_count) {
        const cbm_zova_file_hash_input_t *hash = right < right_count
            ? cbm_zova_publish_model_file_hash_at(after, right) : NULL;
        int order = left >= before->file_hash_count ? 1 : right >= right_count ? -1
                    : strcmp(before->file_hashes[left].file_path, hash->file_path);
        if (order < 0) {
            if (append_delete(delta->file_hash_deletes, &delta->metrics.file_hash_deletes,
                              before->file_hashes[left++].file_path) != 0) return -1;
        } else if (order > 0) {
            delta->file_hash_upserts[delta->metrics.file_hash_upserts++] = hash;
            right++;
        } else {
            if (!hash_equal(&before->file_hashes[left], hash))
                delta->file_hash_upserts[delta->metrics.file_hash_upserts++] = hash;
            left++; right++;
        }
    }
    return 0;
}

int cbm_zova_workspace_delta_build(const cbm_zova_workspace_snapshot_t *before,
                                   cbm_zova_publish_model_t *after,
                                   cbm_zova_workspace_delta_t **out_delta) {
    if (!before || !after || !out_delta) return -1;
    *out_delta = NULL;
    cbm_zova_snapshot_components_t required = CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    if (before->generation <= 0 ||
        cbm_zova_workspace_delta_required_components(before, after, &required) != 0 ||
        (before->hydrated_components & required) != required ||
        (before->node_count > 0 && !before->node_source_ordinals) ||
        !sorted_unique(before->node_stable_ids, before->node_count) ||
        !sorted_unique(before->edge_ids, before->edge_count) ||
        ((required & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) &&
         !sorted_unique(before->node_vector_ids, before->node_vector_count)) ||
        ((required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) &&
         !sorted_unique(before->token_vector_ids, before->token_vector_count))) return -1;
    struct timespec started;
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_workspace_delta_t *delta = calloc(1, sizeof(*delta));
    if (!delta || allocate_arrays(delta, before, after, required) != 0 ||
        build_nodes(delta, before, after) != 0 || build_edges(delta, before, after) != 0 ||
        ((required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY) &&
         build_topology(delta, before, after) != 0) ||
        ((required & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) &&
         build_node_vectors(delta, before, after) != 0) ||
        ((required & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) &&
         build_token_vectors(delta, before, after) != 0) ||
        build_hashes(delta, before, after) != 0) {
        cbm_zova_workspace_delta_free(delta);
        return -1;
    }
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_publish_model_input(after);
    delta->summary = &input->project_summary;
    delta->expected_generation = before->generation;
    delta->replace_summary = !summary_equal(&before->project_summary, delta->summary);
    delta->metrics.summary_replacements = delta->replace_summary ? 1 : 0;
    delta->metrics.diff_ms = delta_elapsed_ms(started);
    *out_delta = delta;
    return 0;
}

int64_t cbm_zova_workspace_delta_expected_generation(
    const cbm_zova_workspace_delta_t *delta) {
    return delta ? delta->expected_generation : 0;
}

void cbm_zova_workspace_delta_free(cbm_zova_workspace_delta_t *delta) {
    if (!delta) return;
    for (uint64_t i = 0; i < delta->metrics.node_deletes; i++) free(delta->node_deletes[i]);
    for (uint64_t i = 0; i < delta->metrics.edge_deletes; i++) free(delta->edge_deletes[i]);
    for (uint64_t i = 0; i < delta->metrics.topology_deletes; i++) {
        free((char *)delta->topology_deletes[i].source_stable_id);
        free((char *)delta->topology_deletes[i].edge_type);
        free((char *)delta->topology_deletes[i].target_stable_id);
    }
    for (uint64_t i = 0; i < delta->metrics.node_vector_deletes; i++) free(delta->node_vector_deletes[i]);
    for (uint64_t i = 0; i < delta->metrics.token_vector_deletes; i++) free(delta->token_vector_deletes[i]);
    for (uint64_t i = 0; i < delta->metrics.file_hash_deletes; i++) free(delta->file_hash_deletes[i]);
    free(delta->node_inserts); free(delta->node_updates); free(delta->node_deletes);
    free(delta->edge_inserts); free(delta->edge_deletes);
    free(delta->topology_inserts); free(delta->topology_deletes);
    free(delta->node_vector_upserts); free(delta->node_vector_deletes);
    free(delta->token_vector_upserts); free(delta->token_vector_deletes);
    free(delta->file_hash_upserts); free(delta->file_hash_deletes);
    free(delta);
}

void cbm_zova_workspace_delta_metrics(const cbm_zova_workspace_delta_t *delta,
                                      cbm_zova_workspace_delta_metrics_t *out) {
    if (out) *out = delta ? delta->metrics : (cbm_zova_workspace_delta_metrics_t){0};
}

#define DELTA_GETTER(name, type, field, count_field) \
    type cbm_zova_workspace_delta_##name##_at(const cbm_zova_workspace_delta_t *delta, int index) { \
        return delta && index >= 0 && (uint64_t)index < delta->metrics.count_field \
            ? delta->field[index] : NULL; \
    }
DELTA_GETTER(node_insert, const cbm_zova_publish_node_t *, node_inserts, node_inserts)
DELTA_GETTER(node_update, const cbm_zova_publish_node_t *, node_updates, node_updates)
DELTA_GETTER(node_delete, const char *, node_deletes, node_deletes)
DELTA_GETTER(edge_insert, const cbm_zova_publish_edge_t *, edge_inserts, edge_inserts)
DELTA_GETTER(edge_delete, const char *, edge_deletes, edge_deletes)
DELTA_GETTER(node_vector_upsert, const cbm_zova_publish_node_vector_t *, node_vector_upserts, node_vector_upserts)
DELTA_GETTER(node_vector_delete, const char *, node_vector_deletes, node_vector_deletes)
DELTA_GETTER(token_vector_upsert, const cbm_zova_publish_token_vector_t *, token_vector_upserts, token_vector_upserts)
DELTA_GETTER(token_vector_delete, const char *, token_vector_deletes, token_vector_deletes)
DELTA_GETTER(file_hash_upsert, const cbm_zova_file_hash_input_t *, file_hash_upserts, file_hash_upserts)
DELTA_GETTER(file_hash_delete, const char *, file_hash_deletes, file_hash_deletes)
#undef DELTA_GETTER

const cbm_zova_delta_topology_edge_t *cbm_zova_workspace_delta_topology_insert_at(
    const cbm_zova_workspace_delta_t *delta, int index) {
    return delta && index >= 0 && (uint64_t)index < delta->metrics.topology_inserts
        ? &delta->topology_inserts[index] : NULL;
}

const cbm_zova_delta_topology_edge_t *cbm_zova_workspace_delta_topology_delete_at(
    const cbm_zova_workspace_delta_t *delta, int index) {
    return delta && index >= 0 && (uint64_t)index < delta->metrics.topology_deletes
        ? &delta->topology_deletes[index] : NULL;
}

bool cbm_zova_workspace_delta_replaces_summary(const cbm_zova_workspace_delta_t *delta) {
    return delta && delta->replace_summary;
}

const cbm_zova_project_summary_input_t *cbm_zova_workspace_delta_summary(
    const cbm_zova_workspace_delta_t *delta) {
    return delta ? delta->summary : NULL;
}
