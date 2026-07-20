#include "cbm_zova_publish_model.h"
#include "cbm_zova_edge_payload.h"
#include "cbm_zova_repository.h"

#include "foundation/sha256.h"
#include "foundation/sha256_backend.h"
#include "foundation/compat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MODEL_ID_CAP = CBM_ZOVA_WORKSPACE_ID_MAX + 80,
    MODEL_PATH_CAP = 4096,
    MODEL_DISCRIMINATOR_CAP = 512,
};

typedef struct {
    cbm_zova_publish_node_t value;
    char *fts_name;
} model_node_t;

typedef struct {
    cbm_zova_publish_edge_t value;
} model_edge_t;

typedef struct {
    int64_t dump_id;
    const char *stable_id;
    uint64_t ordinal;
} model_id_map_t;

typedef struct {
    const cbm_zova_file_hash_input_t *source;
} model_hash_t;

typedef struct {
    cbm_zova_publish_node_vector_t value;
} model_node_vector_t;

typedef struct {
    cbm_zova_publish_token_vector_t value;
} model_token_vector_t;

typedef struct {
    cbm_sha256_backend_ctx metadata;
    cbm_sha256_backend_ctx fts;
    cbm_sha256_backend_ctx topology;
    cbm_sha256_backend_ctx nodes;
    cbm_sha256_backend_ctx tokens;
} model_digest_state_t;

struct cbm_zova_publish_model {
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    cbm_zova_workspace_generation_input_t input;
    model_node_t *nodes;
    char *node_id_storage;
    char *node_fts_storage;
    int node_count;
    const char **dense_ids;
    uint64_t *dense_ordinals;
    model_id_map_t *id_map;
    model_edge_t *edges;
    char *edge_id_storage;
    int edge_count;
    model_edge_t *topology;
    int topology_count;
    uint8_t *edge_payload_storage;
    model_hash_t *hashes;
    int hash_count;
    model_node_vector_t *node_vectors;
    int node_vector_count;
    model_token_vector_t *token_vectors;
    char *token_id_storage;
    int token_vector_count;
    cbm_zova_workspace_generation_result_t digests;
    cbm_zova_publish_model_metrics_t metrics;
    model_digest_state_t pending_digests;
    bool prepared_pending;
};

static int64_t model_fail_allocation_at = -1;
static int64_t model_allocation_count = 0;
static uint64_t model_build_count = 0;

static void *model_malloc(size_t size) {
    int64_t index = model_allocation_count++;
    return index == model_fail_allocation_at ? NULL : malloc(size);
}

static void *model_calloc(size_t count, size_t size) {
    int64_t index = model_allocation_count++;
    return index == model_fail_allocation_at ? NULL : calloc(count, size);
}

void cbm_zova_publish_model_test_fail_allocation_at(int64_t index) {
    model_fail_allocation_at = index;
    model_allocation_count = 0;
}

int64_t cbm_zova_publish_model_test_allocation_count(void) {
    return model_allocation_count;
}

void cbm_zova_publish_model_test_reset_build_count(void) {
    model_build_count = 0;
}

uint64_t cbm_zova_publish_model_test_build_count(void) {
    return model_build_count;
}

static double model_elapsed_ms(const struct timespec *started,
                               const struct timespec *finished) {
    return (double)(finished->tv_sec - started->tv_sec) * 1000.0 +
           (double)(finished->tv_nsec - started->tv_nsec) / 1000000.0;
}

static int model_nonzero(const uint8_t *values, int length) {
    if (!values || length < 0) return 0;
    for (int i = 0; i < length; i++)
        if (values[i] != 0) return 1;
    return 0;
}

static int model_node_id(const char *workspace_id, const CBMDumpNode *node,
                         char out[MODEL_ID_CAP]) {
    if (!node || !node->label || !node->name || !node->qualified_name || !node->file_path) {
        return -1;
    }
    char path[MODEL_PATH_CAP];
    if (snprintf(path, sizeof(path), "%s", node->file_path) >= (int)sizeof(path)) return -1;
    for (char *cursor = path; *cursor; cursor++)
        if (*cursor == '\\') *cursor = '/';
    char discriminator[MODEL_DISCRIMINATOR_CAP];
    if (node->qualified_name[0]) {
        if (snprintf(discriminator, sizeof(discriminator), "named:%s", node->qualified_name) >=
            (int)sizeof(discriminator)) return -1;
    } else if (!node->name[0] || node->start_line < 0 || node->end_line < node->start_line ||
               snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", node->name,
                        node->start_line, node->end_line) >= (int)sizeof(discriminator)) {
        return -1;
    }
    return cbm_zova_workspace_node_id_v2(workspace_id, node->label, path,
                                         node->qualified_name, discriminator,
                                         out, MODEL_ID_CAP);
}

static int model_hash_id(const char *prefix, const char *workspace_id,
                         const char *const *parts, size_t part_count,
                         char out[MODEL_ID_CAP]) {
    cbm_sha256_ctx context;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char digits[] = "0123456789abcdef";
    char hex[33];
    cbm_sha256_init(&context);
    for (size_t i = 0; i < part_count; i++) {
        if (!parts[i]) return -1;
        cbm_sha256_update(&context, parts[i], strlen(parts[i]));
        cbm_sha256_update(&context, "\0", 1);
    }
    cbm_sha256_final(&context, digest);
    for (size_t i = 0; i < 16; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[32] = '\0';
    return snprintf(out, MODEL_ID_CAP, "%s:%s:%s", prefix, workspace_id, hex) < MODEL_ID_CAP
        ? 0 : -1;
}

static int model_edge_id(const char *workspace_id, const char *source, const char *type,
                         const char *target, const char *local_name,
                         char out[MODEL_ID_CAP]) {
    const char *parts[] = {source, type, target, local_name};
    return model_hash_id("e:v1", workspace_id, parts, 4, out);
}

static int model_token_id(const char *workspace_id, const char *token,
                          char out[MODEL_ID_CAP]) {
    const char *parts[] = {token};
    return model_hash_id("t:v1", workspace_id, parts, 1, out);
}

static int model_camel_boundary(const char *input, size_t length, size_t index) {
    if (index == 0) return 0;
    char current = input[index];
    char previous = input[index - 1];
    char next = index + 1 < length ? input[index + 1] : '\0';
    if (current >= 'A' && current <= 'Z' && previous >= 'a' && previous <= 'z') return 1;
    return current >= 'A' && current <= 'Z' && previous >= 'A' && previous <= 'Z' &&
           next >= 'a' && next <= 'z';
}

static int model_camel_storage_size(const char *input, size_t *out_size) {
    if (!input || !out_size) return -1;
    size_t length = strlen(input);
    size_t boundaries = 0;
    for (size_t i = 0; i < length; i++)
        if (model_camel_boundary(input, length, i)) boundaries++;
    if (length > (SIZE_MAX - 2) / 2 ||
        boundaries > SIZE_MAX - (length * 2) - 2) return -1;
    *out_size = length * 2 + boundaries + 2;
    return 0;
}

static int model_camel_name_into(const char *input, char *value, size_t capacity) {
    size_t required = 0;
    if (model_camel_storage_size(input, &required) != 0 || !value || capacity < required)
        return -1;
    size_t length = strlen(input);
    memcpy(value, input, length);
    size_t output = length;
    value[output++] = ' ';
    for (size_t i = 0; i < length; i++) {
        if (model_camel_boundary(input, length, i)) value[output++] = ' ';
        value[output++] = input[i];
    }
    value[output] = '\0';
    return 0;
}

static int model_id_storage_layout(const char *workspace_id, int count,
                                   size_t *out_stride, size_t *out_bytes) {
    if (!workspace_id || count < 0 || !out_stride || !out_bytes) return -1;
    size_t workspace_length = strlen(workspace_id);
    /* n:v2:<workspace>:<32 hex>, e:v1:..., and t:v1:... are equal-width. */
    if (workspace_length > SIZE_MAX - 39) return -1;
    size_t stride = workspace_length + 39;
    if ((size_t)count > SIZE_MAX / stride) return -1;
    *out_stride = stride;
    *out_bytes = (size_t)count * stride;
    return stride <= MODEL_ID_CAP ? 0 : -1;
}

static int model_id_compare(const void *left_ptr, const void *right_ptr) {
    const model_id_map_t *left = left_ptr;
    const model_id_map_t *right = right_ptr;
    return left->dump_id < right->dump_id ? -1 : left->dump_id > right->dump_id ? 1 : 0;
}

static int model_node_compare(const void *left_ptr, const void *right_ptr) {
    const model_node_t *left = left_ptr;
    const model_node_t *right = right_ptr;
    return strcmp(left->value.stable_id, right->value.stable_id);
}

static int model_nullable_compare(const char *left, const char *right) {
    return strcmp(left ? left : "", right ? right : "");
}

static void model_digest_text(cbm_sha256_backend_ctx *context, const char *value);

static int model_edge_compare(const void *left_ptr, const void *right_ptr) {
    const model_edge_t *left = left_ptr;
    const model_edge_t *right = right_ptr;
    int comparison = strcmp(left->value.edge_id, right->value.edge_id);
    if (comparison != 0) return comparison;
    comparison = strcmp(left->value.source_stable_id, right->value.source_stable_id);
    if (comparison != 0) return comparison;
    comparison = model_nullable_compare(left->value.source->type, right->value.source->type);
    if (comparison != 0) return comparison;
    comparison = strcmp(left->value.target_stable_id, right->value.target_stable_id);
    if (comparison != 0) return comparison;
    comparison = model_nullable_compare(left->value.source->properties,
                                        right->value.source->properties);
    if (comparison != 0) return comparison;
    return model_nullable_compare(left->value.source->url_path, right->value.source->url_path);
}

static int model_topology_compare(const void *left_ptr, const void *right_ptr) {
    const model_edge_t *left = left_ptr;
    const model_edge_t *right = right_ptr;
    int comparison = left->value.source_ordinal < right->value.source_ordinal
                         ? -1
                         : left->value.source_ordinal > right->value.source_ordinal ? 1 : 0;
    if (comparison != 0) return comparison;
    comparison = strcmp(left->value.source->type, right->value.source->type);
    if (comparison != 0) return comparison;
    return left->value.target_ordinal < right->value.target_ordinal
               ? -1
               : left->value.target_ordinal > right->value.target_ordinal ? 1 : 0;
}

typedef struct {
    const CBMDumpEdge *source;
    char edge_id[MODEL_ID_CAP];
} prepared_group_edge_t;

static int prepared_group_edge_compare(const void *left_ptr, const void *right_ptr) {
    const prepared_group_edge_t *left = left_ptr;
    const prepared_group_edge_t *right = right_ptr;
    return strcmp(left->edge_id, right->edge_id);
}

static void model_digest_topology_payload(model_digest_state_t *digests,
                                          const cbm_zova_publish_edge_t *row) {
    model_digest_text(&digests->metadata, row->source_stable_id);
    model_digest_text(&digests->metadata, row->source->type);
    model_digest_text(&digests->metadata, row->target_stable_id);
    cbm_sha256_backend_update(&digests->metadata, row->payload, row->payload_len);
}

static int model_hash_compare(const void *left_ptr, const void *right_ptr) {
    const model_hash_t *left = left_ptr;
    const model_hash_t *right = right_ptr;
    return strcmp(left->source->file_path, right->source->file_path);
}

static int model_node_vector_compare(const void *left_ptr, const void *right_ptr) {
    const model_node_vector_t *left = left_ptr;
    const model_node_vector_t *right = right_ptr;
    return strcmp(left->value.stable_id, right->value.stable_id);
}

static int model_token_vector_compare(const void *left_ptr, const void *right_ptr) {
    const model_token_vector_t *left = left_ptr;
    const model_token_vector_t *right = right_ptr;
    return strcmp(left->value.token_id, right->value.token_id);
}

static const char *model_lookup(const cbm_zova_publish_model_t *model, int64_t dump_id) {
    if (model->dense_ids) {
        if (dump_id <= 0 || dump_id > model->node_count) return NULL;
        return model->dense_ids[dump_id - 1];
    }
    int low = 0;
    int high = model->node_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (model->id_map[middle].dump_id == dump_id) return model->id_map[middle].stable_id;
        if (model->id_map[middle].dump_id < dump_id) low = middle + 1;
        else high = middle;
    }
    return NULL;
}

static model_id_map_t *model_sparse_lookup(cbm_zova_publish_model_t *model,
                                           int64_t dump_id) {
    int low = 0;
    int high = model->node_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (model->id_map[middle].dump_id == dump_id) return &model->id_map[middle];
        if (model->id_map[middle].dump_id < dump_id) low = middle + 1;
        else high = middle;
    }
    return NULL;
}

static int model_lookup_ordinal(cbm_zova_publish_model_t *model, int64_t dump_id,
                                uint64_t *out_ordinal) {
    if (!model || !out_ordinal) return -1;
    if (model->dense_ids) {
        if (dump_id <= 0 || dump_id > model->node_count ||
            !model->dense_ids[dump_id - 1] || !model->dense_ordinals)
            return -1;
        *out_ordinal = model->dense_ordinals[dump_id - 1];
        return 0;
    }
    model_id_map_t *entry = model_sparse_lookup(model, dump_id);
    if (!entry) return -1;
    *out_ordinal = entry->ordinal;
    return 0;
}

static void model_digest_text(cbm_sha256_backend_ctx *context, const char *value) {
    const char *text = value ? value : "";
    cbm_sha256_backend_update(context, text, strlen(text));
    cbm_sha256_backend_update(context, "\0", 1);
}

static void model_digest_i64(cbm_sha256_backend_ctx *context, int64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)((uint64_t)value >> (i * 8));
    cbm_sha256_backend_update(context, bytes, sizeof(bytes));
}

static void model_digest_finish(cbm_sha256_backend_ctx *context,
                                char output[CBM_ZOVA_DIGEST_HEX_SIZE]) {
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char digits[] = "0123456789abcdef";
    cbm_sha256_backend_final(context, digest);
    for (size_t i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        output[i * 2] = digits[digest[i] >> 4];
        output[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    output[CBM_SHA256_HEX_LEN] = '\0';
}

static int model_validate_input(const char *workspace_id,
                                const cbm_zova_workspace_generation_input_t *input) {
    if (!workspace_id || cbm_zova_workspace_id_validate(workspace_id) != 0 || !input ||
        !input->root_path || !input->root_path[0] || !input->project || !input->project[0] ||
        !input->indexed_at || !input->indexed_at[0] || !input->model_fingerprint ||
        !input->model_fingerprint[0] || input->vector_dimensions <= 0 ||
        input->node_count < 0 || input->edge_count < 0 || input->node_vector_count < 0 ||
        input->token_vector_count < 0 || input->file_hash_count < 0 ||
        (input->node_count && !input->nodes) || (input->edge_count && !input->edges) ||
        (input->node_vector_count && !input->node_vectors) ||
        (input->token_vector_count && !input->token_vectors) ||
        (input->file_hash_count && !input->file_hashes)) return -1;
    if (input->project_summary.present &&
        (!input->project_summary.summary || !input->project_summary.source_hash ||
         !input->project_summary.created_at || !input->project_summary.updated_at)) return -1;
    return 0;
}

static int model_build_nodes(cbm_zova_publish_model_t *model,
                             model_digest_state_t *digests) {
    int count = model->input.node_count;
    if (count == 0) return 0;
    model->nodes = model_calloc((size_t)count, sizeof(*model->nodes));
    model->dense_ids = model_calloc((size_t)count, sizeof(*model->dense_ids));
    model->dense_ordinals = model_calloc((size_t)count, sizeof(*model->dense_ordinals));
    size_t id_stride = 0;
    size_t id_bytes = 0;
    size_t fts_bytes = 0;
    if (model_id_storage_layout(model->workspace_id, count, &id_stride, &id_bytes) != 0)
        return -1;
    for (int i = 0; i < count; i++) {
        size_t row_bytes = 0;
        if (!model->input.nodes[i].name ||
            model_camel_storage_size(model->input.nodes[i].name, &row_bytes) != 0 ||
            row_bytes > SIZE_MAX - fts_bytes)
            return -1;
        fts_bytes += row_bytes;
    }
    model->node_id_storage = model_malloc(id_bytes);
    model->node_fts_storage = model_malloc(fts_bytes);
    if (!model->nodes || !model->dense_ids || !model->dense_ordinals ||
        !model->node_id_storage || !model->node_fts_storage)
        return -1;
    model->node_count = count;
    int dense_ids = 1;
    char *fts_cursor = model->node_fts_storage;
    for (int i = 0; i < count; i++) {
        const CBMDumpNode *node = &model->input.nodes[i];
        char *stable_id = model->node_id_storage + (size_t)i * id_stride;
        size_t fts_capacity = 0;
        if (node->id <= 0 || !node->project || strcmp(node->project, model->input.project) != 0 ||
            !node->label || !node->name || !node->qualified_name || !node->file_path ||
            model_node_id(model->workspace_id, node, stable_id) != 0 ||
            model_camel_storage_size(node->name, &fts_capacity) != 0 ||
            model_camel_name_into(node->name, fts_cursor, fts_capacity) != 0) {
            return -1;
        }
        model->nodes[i].value = (cbm_zova_publish_node_t){
            .source = node, .stable_id = stable_id, .ordinal = 0,
            .source_ordinal = (uint64_t)i};
        model->nodes[i].fts_name = fts_cursor;
        fts_cursor += fts_capacity;
        if (node->id > count || model->dense_ids[node->id - 1]) {
            dense_ids = 0;
        } else {
            model->dense_ids[node->id - 1] = stable_id;
        }
        model->metrics.stable_node_id_computations++;
        model->metrics.camel_split_computations++;
    }
    if (!dense_ids) {
        free(model->dense_ids);
        model->dense_ids = NULL;
        free(model->dense_ordinals);
        model->dense_ordinals = NULL;
        model->id_map = model_calloc((size_t)count, sizeof(*model->id_map));
        if (!model->id_map) return -1;
        for (int i = 0; i < count; i++) {
            model->id_map[i] = (model_id_map_t){
                .dump_id = model->nodes[i].value.source->id,
                .stable_id = model->nodes[i].value.stable_id};
        }
        qsort(model->id_map, (size_t)count, sizeof(*model->id_map), model_id_compare);
        model->metrics.global_sorts++;
        for (int i = 1; i < count; i++)
            if (model->id_map[i - 1].dump_id == model->id_map[i].dump_id) return -1;
    }
    qsort(model->nodes, (size_t)count, sizeof(*model->nodes), model_node_compare);
    model->metrics.global_sorts++;
    for (int i = 0; i < count; i++) {
        if (i > 0 && strcmp(model->nodes[i - 1].value.stable_id,
                            model->nodes[i].value.stable_id) == 0) return -1;
        model->nodes[i].value.ordinal = (uint64_t)i;
        int64_t dump_id = model->nodes[i].value.source->id;
        if (model->dense_ids) {
            model->dense_ordinals[dump_id - 1] = (uint64_t)i;
        } else {
            model_id_map_t *entry = model_sparse_lookup(model, dump_id);
            if (!entry) return -1;
            entry->ordinal = (uint64_t)i;
        }
        const model_node_t *row = &model->nodes[i];
        const CBMDumpNode *node = row->value.source;
        model_digest_text(&digests->metadata, row->value.stable_id);
        model_digest_text(&digests->metadata, node->project);
        model_digest_text(&digests->metadata, node->label);
        model_digest_text(&digests->metadata, node->name);
        model_digest_text(&digests->metadata, node->qualified_name);
        model_digest_text(&digests->metadata, node->file_path);
        model_digest_i64(&digests->metadata, node->start_line);
        model_digest_i64(&digests->metadata, node->end_line);
        model_digest_text(&digests->metadata, node->properties);
        model_digest_text(&digests->fts, row->value.stable_id);
        model_digest_text(&digests->fts, row->fts_name);
        model_digest_text(&digests->fts, node->qualified_name);
        model_digest_text(&digests->fts, node->label);
        model_digest_text(&digests->fts, node->file_path);
    }
    return 0;
}

static int model_build_edges(cbm_zova_publish_model_t *model,
                             model_digest_state_t *digests) {
    int count = model->input.edge_count;
    if (count == 0) return 0;
    model->edges = model_calloc((size_t)count, sizeof(*model->edges));
    size_t id_stride = 0;
    size_t id_bytes = 0;
    if (model_id_storage_layout(model->workspace_id, count, &id_stride, &id_bytes) != 0)
        return -1;
    model->edge_id_storage = model_malloc(id_bytes);
    if (!model->edges || !model->edge_id_storage) return -1;
    model->edge_count = count;
    for (int i = 0; i < count; i++) {
        const CBMDumpEdge *edge = &model->input.edges[i];
        const char *source = model_lookup(model, edge->source_id);
        const char *target = model_lookup(model, edge->target_id);
        uint64_t source_ordinal = 0;
        uint64_t target_ordinal = 0;
        model->metrics.endpoint_lookups += 2;
        if (!edge->project || strcmp(edge->project, model->input.project) != 0 ||
            !edge->type || !edge->type[0] || !source || !target) return -1;
        if (model_lookup_ordinal(model, edge->source_id, &source_ordinal) != 0 ||
            model_lookup_ordinal(model, edge->target_id, &target_ordinal) != 0)
            return -1;
        char *edge_id = model->edge_id_storage + (size_t)i * id_stride;
        if (model_edge_id(model->workspace_id, source, edge->type, target,
                          edge->local_name ? edge->local_name : "", edge_id) != 0) return -1;
        model->edges[i].value = (cbm_zova_publish_edge_t){
            .source = edge, .edge_id = edge_id, .source_stable_id = source,
            .target_stable_id = target, .source_ordinal = source_ordinal,
            .target_ordinal = target_ordinal};
        model->metrics.edge_id_computations++;
    }
    qsort(model->edges, (size_t)count, sizeof(*model->edges), model_edge_compare);
    model->metrics.global_sorts++;
    int unique_edges = 0;
    for (int i = 0; i < count; i++) {
        if (unique_edges > 0 && strcmp(model->edges[unique_edges - 1].value.edge_id,
                                       model->edges[i].value.edge_id) == 0) {
            const cbm_zova_publish_edge_t *previous = &model->edges[unique_edges - 1].value;
            const cbm_zova_publish_edge_t *current = &model->edges[i].value;
            if (strcmp(previous->source_stable_id, current->source_stable_id) != 0 ||
                strcmp(previous->source->type, current->source->type) != 0 ||
                strcmp(previous->target_stable_id, current->target_stable_id) != 0 ||
                model_nullable_compare(previous->source->local_name,
                                       current->source->local_name) != 0) return -1;
            model->edges[unique_edges - 1] = model->edges[i];
        } else {
            if (unique_edges != i) model->edges[unique_edges] = model->edges[i];
            unique_edges++;
        }
    }
    model->edge_count = unique_edges;
    for (int i = 0; i < unique_edges; i++) {
        cbm_zova_publish_edge_t *row = &model->edges[i].value;
        row->ordinal = (uint64_t)i;
    }

    model->topology = model_calloc((size_t)unique_edges, sizeof(*model->topology));
    if (!model->topology) return -1;
    for (int i = 0; i < unique_edges; i++) model->topology[i] = model->edges[i];
    qsort(model->topology, (size_t)unique_edges, sizeof(*model->topology),
          model_topology_compare);
    model->metrics.global_sorts++;
    int unique_topology = 0;
    for (int i = 0; i < unique_edges; i++) {
        model_edge_t current = model->topology[i];
        if (unique_topology == 0 ||
            model_topology_compare(&model->topology[unique_topology - 1], &current) != 0) {
            if (unique_topology != i) model->topology[unique_topology] = current;
            model->topology[unique_topology].value.ordinal = (uint64_t)unique_topology;
            const cbm_zova_publish_edge_t *row = &model->topology[unique_topology].value;
            model_digest_text(&digests->topology, row->source_stable_id);
            model_digest_text(&digests->topology, row->source->type);
            model_digest_text(&digests->topology, row->target_stable_id);
            unique_topology++;
        }
        if (current.value.ordinal >= (uint64_t)unique_edges || unique_topology <= 0) return -1;
        model->edges[current.value.ordinal].value.topology_ordinal =
            (uint64_t)(unique_topology - 1);
    }
    model->topology_count = unique_topology;

    size_t *counts = model_calloc((size_t)unique_topology * 3, sizeof(*counts));
    const CBMDumpEdge **grouped = model_malloc((size_t)unique_edges * sizeof(*grouped));
    if (!counts || !grouped) {
        free(counts);
        free(grouped);
        return -1;
    }
    size_t *offsets = counts + unique_topology;
    size_t *cursors = offsets + unique_topology;
    for (int i = 0; i < unique_edges; i++) {
        uint64_t ordinal = model->edges[i].value.topology_ordinal;
        if (ordinal >= (uint64_t)unique_topology) {
            free(counts);
            free(grouped);
            return -1;
        }
        counts[ordinal]++;
    }
    size_t grouped_count = 0;
    for (int i = 0; i < unique_topology; i++) {
        offsets[i] = grouped_count;
        cursors[i] = grouped_count;
        if (counts[i] > SIZE_MAX - grouped_count) {
            free(counts);
            free(grouped);
            return -1;
        }
        grouped_count += counts[i];
    }
    if (grouped_count != (size_t)unique_edges) {
        free(counts);
        free(grouped);
        return -1;
    }
    for (int i = 0; i < unique_edges; i++) {
        size_t ordinal = (size_t)model->edges[i].value.topology_ordinal;
        grouped[cursors[ordinal]++] = model->edges[i].value.source;
    }

    size_t payload_bytes = 0;
    for (int i = 0; i < unique_topology; i++) {
        size_t payload_size = 0;
        if (cbm_zova_edge_payload_encoded_size(grouped + offsets[i], counts[i],
                                               &payload_size) != 0 ||
            payload_size > SIZE_MAX - payload_bytes) {
            free(counts);
            free(grouped);
            return -1;
        }
        cursors[i] = payload_size;
        payload_bytes += payload_size;
    }
    if (payload_bytes > 0) {
        model->edge_payload_storage = model_malloc(payload_bytes);
        if (!model->edge_payload_storage) {
            free(counts);
            free(grouped);
            return -1;
        }
    }
    uint8_t *payload_cursor = model->edge_payload_storage;
    for (int i = 0; i < unique_topology; i++) {
        cbm_zova_publish_edge_t *topology = &model->topology[i].value;
        topology->logical_edge_count = counts[i];
        topology->payload = cursors[i] > 0 ? payload_cursor : NULL;
        topology->payload_len = cursors[i];
        size_t encoded = 0;
        if (cbm_zova_edge_payload_encode(grouped + offsets[i], counts[i],
                                         payload_cursor, cursors[i], &encoded) != 0 ||
            encoded != cursors[i]) {
            free(counts);
            free(grouped);
            return -1;
        }
        if (encoded > 0) payload_cursor += encoded;
        model_digest_topology_payload(digests, topology);
    }
    free(counts);
    free(grouped);
    return 0;
}

/* Build only the topology rows required by authoritative full publication.
 * Finalized graph-buffer input already has dense node IDs and deduplicated
 * logical edges, so delta-only logical edge IDs and edge ordering are omitted. */
static int model_build_prepared_topology(cbm_zova_publish_model_t *model,
                                         model_digest_state_t *digests) {
    int count = model->input.edge_count;
    model->edge_count = count;
    if (count == 0) return 0;
    if (!model->dense_ids || !model->dense_ordinals) return -1;
    model->topology = model_calloc((size_t)count, sizeof(*model->topology));
    const CBMDumpEdge **grouped = model_malloc((size_t)count * sizeof(*grouped));
    size_t *counts = model_calloc((size_t)count * 3, sizeof(*counts));
    if (!model->topology || !grouped || !counts) {
        free(grouped);
        free(counts);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        const CBMDumpEdge *edge = &model->input.edges[i];
        const char *source = model_lookup(model, edge->source_id);
        const char *target = model_lookup(model, edge->target_id);
        uint64_t source_ordinal = 0;
        uint64_t target_ordinal = 0;
        model->metrics.endpoint_lookups += 2;
        if (!edge->project || strcmp(edge->project, model->input.project) != 0 ||
            !edge->type || !edge->type[0] || !source || !target ||
            model_lookup_ordinal(model, edge->source_id, &source_ordinal) != 0 ||
            model_lookup_ordinal(model, edge->target_id, &target_ordinal) != 0) {
            free(grouped);
            free(counts);
            return -1;
        }
        model->topology[i].value = (cbm_zova_publish_edge_t){
            .source = edge,
            .source_stable_id = source,
            .target_stable_id = target,
            .source_ordinal = source_ordinal,
            .target_ordinal = target_ordinal,
        };
    }
    qsort(model->topology, (size_t)count, sizeof(*model->topology),
          model_topology_compare);
    model->metrics.global_sorts++;

    size_t *offsets = counts + count;
    size_t *payload_sizes = offsets + count;
    int unique_topology = 0;
    size_t grouped_count = 0;
    for (int i = 0; i < count;) {
        int end = i + 1;
        while (end < count &&
               model_topology_compare(&model->topology[i], &model->topology[end]) == 0)
            end++;
        cbm_zova_publish_edge_t row = model->topology[i].value;
        row.ordinal = (uint64_t)unique_topology;
        row.logical_edge_count = (uint64_t)(end - i);
        model->topology[unique_topology].value = row;
        counts[unique_topology] = (size_t)(end - i);
        offsets[unique_topology] = grouped_count;
        for (int j = i; j < end; j++) grouped[grouped_count++] = model->topology[j].value.source;
        model_digest_text(&digests->topology, row.source_stable_id);
        model_digest_text(&digests->topology, row.source->type);
        model_digest_text(&digests->topology, row.target_stable_id);
        unique_topology++;
        i = end;
    }
    model->topology_count = unique_topology;

    size_t payload_bytes = 0;
    for (int i = 0; i < unique_topology; i++) {
        const size_t offset = offsets[i];
        if (counts[i] > 1) {
            prepared_group_edge_t *ordered =
                model_malloc(counts[i] * sizeof(*ordered));
            if (!ordered) {
                free(grouped);
                free(counts);
                return -1;
            }
            const cbm_zova_publish_edge_t *topology = &model->topology[i].value;
            for (size_t j = 0; j < counts[i]; j++) {
                ordered[j].source = grouped[offset + j];
                if (model_edge_id(model->workspace_id, topology->source_stable_id,
                                  topology->source->type,
                                  topology->target_stable_id,
                                  ordered[j].source->local_name
                                      ? ordered[j].source->local_name
                                      : "",
                                  ordered[j].edge_id) != 0) {
                    free(ordered);
                    free(grouped);
                    free(counts);
                    return -1;
                }
                model->metrics.edge_id_computations++;
            }
            qsort(ordered, counts[i], sizeof(*ordered), prepared_group_edge_compare);
            for (size_t j = 0; j < counts[i]; j++) {
                if (j > 0 && strcmp(ordered[j - 1].edge_id, ordered[j].edge_id) == 0) {
                    free(ordered);
                    free(grouped);
                    free(counts);
                    return -1;
                }
                grouped[offset + j] = ordered[j].source;
            }
            free(ordered);
        }
        if (cbm_zova_edge_payload_encoded_size(grouped + offset, counts[i],
                                               &payload_sizes[i]) != 0 ||
            payload_sizes[i] > SIZE_MAX - payload_bytes) {
            free(grouped);
            free(counts);
            return -1;
        }
        payload_bytes += payload_sizes[i];
    }
    if (payload_bytes > 0) {
        model->edge_payload_storage = model_malloc(payload_bytes);
        if (!model->edge_payload_storage) {
            free(grouped);
            free(counts);
            return -1;
        }
    }
    uint8_t *payload_cursor = model->edge_payload_storage;
    for (int i = 0; i < unique_topology; i++) {
        cbm_zova_publish_edge_t *row = &model->topology[i].value;
        row->payload = payload_sizes[i] > 0 ? payload_cursor : NULL;
        row->payload_len = payload_sizes[i];
        size_t encoded = 0;
        if (cbm_zova_edge_payload_encode(grouped + offsets[i], counts[i],
                                         payload_cursor, payload_sizes[i], &encoded) != 0 ||
            encoded != payload_sizes[i]) {
            free(grouped);
            free(counts);
            return -1;
        }
        if (encoded > 0) payload_cursor += encoded;
        model_digest_topology_payload(digests, row);
    }
    free(grouped);
    free(counts);
    return 0;
}

static int model_build_hashes(cbm_zova_publish_model_t *model,
                              model_digest_state_t *digests) {
    int count = model->input.file_hash_count;
    if (count == 0) return 0;
    model->hashes = model_calloc((size_t)count, sizeof(*model->hashes));
    if (!model->hashes) return -1;
    model->hash_count = count;
    for (int i = 0; i < count; i++) {
        const cbm_zova_file_hash_input_t *hash = &model->input.file_hashes[i];
        if (!hash->file_path || !hash->content_hash) return -1;
        model->hashes[i].source = hash;
    }
    qsort(model->hashes, (size_t)count, sizeof(*model->hashes), model_hash_compare);
    model->metrics.global_sorts++;
    for (int i = 0; i < count; i++) {
        if (i > 0 && strcmp(model->hashes[i - 1].source->file_path,
                            model->hashes[i].source->file_path) == 0) return -1;
        const cbm_zova_file_hash_input_t *hash = model->hashes[i].source;
        model_digest_text(&digests->metadata, hash->file_path);
        model_digest_text(&digests->metadata, hash->content_hash);
        model_digest_i64(&digests->metadata, hash->mtime_ns);
        model_digest_i64(&digests->metadata, hash->size_bytes);
    }
    return 0;
}

static int model_build_vectors(cbm_zova_publish_model_t *model,
                               model_digest_state_t *digests) {
    int node_capacity = model->input.node_vector_count;
    int token_capacity = model->input.token_vector_count;
    size_t token_id_stride = 0;
    size_t token_id_bytes = 0;
    if (node_capacity > 0) {
        model->node_vectors = model_calloc((size_t)node_capacity, sizeof(*model->node_vectors));
        if (!model->node_vectors) return -1;
    }
    for (int i = 0; i < node_capacity; i++) {
        const CBMDumpVector *vector = &model->input.node_vectors[i];
        const char *stable_id = model_lookup(model, vector->node_id);
        uint64_t node_ordinal = 0;
        if (!vector->project || strcmp(vector->project, model->input.project) != 0 ||
            !stable_id || model_lookup_ordinal(model, vector->node_id, &node_ordinal) != 0 ||
            !vector->vector || vector->vector_len != model->input.vector_dimensions)
            return -1;
        if (!model_nonzero(vector->vector, vector->vector_len)) continue;
        model->node_vectors[model->node_vector_count++].value =
            (cbm_zova_publish_node_vector_t){
                .source = vector, .stable_id = stable_id, .node_ordinal = node_ordinal};
    }
    if (model->node_vector_count > 0) {
        qsort(model->node_vectors, (size_t)model->node_vector_count,
              sizeof(*model->node_vectors), model_node_vector_compare);
        model->metrics.global_sorts++;
        for (int i = 0; i < model->node_vector_count; i++) {
            if (i > 0 && strcmp(model->node_vectors[i - 1].value.stable_id,
                                model->node_vectors[i].value.stable_id) == 0) return -1;
            model->node_vectors[i].value.ordinal = (uint64_t)i;
            const cbm_zova_publish_node_vector_t *row = &model->node_vectors[i].value;
            model_digest_text(&digests->nodes, row->stable_id);
            cbm_sha256_backend_update(&digests->nodes, row->source->vector,
                                      (size_t)row->source->vector_len);
        }
    }
    if (token_capacity > 0) {
        model->token_vectors = model_calloc((size_t)token_capacity,
                                            sizeof(*model->token_vectors));
        if (model_id_storage_layout(model->workspace_id, token_capacity,
                                    &token_id_stride, &token_id_bytes) != 0)
            return -1;
        model->token_id_storage = model_malloc(token_id_bytes);
        if (!model->token_vectors || !model->token_id_storage) return -1;
    }
    for (int i = 0; i < token_capacity; i++) {
        const CBMDumpTokenVec *vector = &model->input.token_vectors[i];
        if (vector->id <= 0 || !vector->project ||
            strcmp(vector->project, model->input.project) != 0 || !vector->token ||
            !vector->vector || vector->vector_len != model->input.vector_dimensions ||
            !isfinite(vector->idf)) return -1;
        char *token_id = model->token_id_storage + (size_t)i * token_id_stride;
        if (model_token_id(model->workspace_id, vector->token, token_id) != 0) return -1;
        model->metrics.token_id_computations++;
        if (!model_nonzero(vector->vector, vector->vector_len)) continue;
        model->token_vectors[model->token_vector_count++].value =
            (cbm_zova_publish_token_vector_t){.source = vector, .token_id = token_id};
    }
    if (model->token_vector_count > 0) {
        qsort(model->token_vectors, (size_t)model->token_vector_count,
              sizeof(*model->token_vectors), model_token_vector_compare);
        model->metrics.global_sorts++;
        for (int i = 0; i < model->token_vector_count; i++) {
            if (i > 0 && strcmp(model->token_vectors[i - 1].value.token_id,
                                model->token_vectors[i].value.token_id) == 0) return -1;
            model->token_vectors[i].value.ordinal = (uint64_t)i;
            const cbm_zova_publish_token_vector_t *row = &model->token_vectors[i].value;
            model_digest_text(&digests->tokens, row->token_id);
            model_digest_text(&digests->tokens, row->source->token);
            cbm_sha256_backend_update(&digests->tokens, &row->source->idf,
                                      sizeof(row->source->idf));
            cbm_sha256_backend_update(&digests->tokens, row->source->vector,
                                      (size_t)row->source->vector_len);
        }
    }
    return 0;
}

static void model_digest_summary(cbm_zova_publish_model_t *model,
                                 model_digest_state_t *digests) {
    model_digest_text(&digests->metadata,
                      model->input.project_summary.present ? "1" : "0");
    if (model->input.project_summary.present) {
        model_digest_text(&digests->metadata, model->input.project_summary.summary);
        model_digest_text(&digests->metadata, model->input.project_summary.source_hash);
        model_digest_text(&digests->metadata, model->input.project_summary.created_at);
        model_digest_text(&digests->metadata, model->input.project_summary.updated_at);
    }
}

static void model_digest_finish_all(cbm_zova_publish_model_t *model,
                                    model_digest_state_t *digests) {
    model_digest_finish(&digests->metadata, model->digests.metadata_sha256);
    model_digest_finish(&digests->fts, model->digests.fts_sha256);
    model_digest_finish(&digests->topology, model->digests.topology_sha256);
    model_digest_finish(&digests->nodes, model->digests.node_vector_sha256);
    model_digest_finish(&digests->tokens, model->digests.token_vector_sha256);
    model->digests.graph_nodes = (uint64_t)model->node_count;
    model->digests.graph_edges = (uint64_t)model->topology_count;
    model->digests.metadata_nodes = (uint64_t)model->node_count;
    model->digests.metadata_edges = (uint64_t)model->edge_count;
    model->digests.metadata_topology_edges = (uint64_t)model->topology_count;
    model->digests.fts_rows = (uint64_t)model->node_count;
    model->digests.node_vectors = (uint64_t)model->node_vector_count;
    model->digests.node_vector_rows = (uint64_t)model->node_vector_count;
    model->digests.token_vectors = (uint64_t)model->token_vector_count;
    model->digests.token_vector_rows = (uint64_t)model->token_vector_count;
}

int cbm_zova_publish_model_build(
    const char *workspace_id, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_publish_model_t **out_model) {
    struct timespec started = {0}, phase_started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    if (!out_model) return -1;
    *out_model = NULL;
    if (model_validate_input(workspace_id, input) != 0) return -1;
    model_allocation_count = 0;
    cbm_zova_publish_model_t *model = model_calloc(1, sizeof(*model));
    if (!model) return -1;
    snprintf(model->workspace_id, sizeof(model->workspace_id), "%s", workspace_id);
    model->input = *input;
    model_digest_state_t digests;
    cbm_sha256_backend_init(&digests.metadata);
    cbm_sha256_backend_init(&digests.fts);
    cbm_sha256_backend_init(&digests.topology);
    cbm_sha256_backend_init(&digests.nodes);
    cbm_sha256_backend_init(&digests.tokens);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    if (model_build_nodes(model, &digests) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.nodes_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_edges(model, &digests) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.edges_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_hashes(model, &digests) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.hashes_ms = model_elapsed_ms(&phase_started, &finished);
    model_digest_summary(model, &digests);
    phase_started = finished;
    if (model_build_vectors(model, &digests) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.vectors_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    model_digest_finish_all(model, &digests);
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.digests_ms = model_elapsed_ms(&phase_started, &finished);
    model->metrics.normalization_ms = model_elapsed_ms(&started, &finished);
    model_build_count++;
    *out_model = model;
    return 0;
}

int cbm_zova_prepared_view_build_base(
    const char *workspace_id, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_prepared_view_t **out_view) {
    struct timespec started = {0}, phase_started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    if (!out_view) return -1;
    *out_view = NULL;
    if (model_validate_input(workspace_id, input) != 0 ||
        input->file_hash_count != 0 || input->file_hashes ||
        input->project_summary.present) return -1;
    model_allocation_count = 0;
    cbm_zova_publish_model_t *view = model_calloc(1, sizeof(*view));
    if (!view) return -1;
    snprintf(view->workspace_id, sizeof(view->workspace_id), "%s", workspace_id);
    view->input = *input;
    model_digest_state_t *digests = &view->pending_digests;
    cbm_sha256_backend_init(&digests->metadata);
    cbm_sha256_backend_init(&digests->fts);
    cbm_sha256_backend_init(&digests->topology);
    cbm_sha256_backend_init(&digests->nodes);
    cbm_sha256_backend_init(&digests->tokens);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    if (model_build_nodes(view, digests) != 0 ||
        (view->node_count > 0 && (!view->dense_ids || !view->dense_ordinals))) {
        cbm_zova_publish_model_free(view);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    view->metrics.nodes_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_prepared_topology(view, digests) != 0) {
        cbm_zova_publish_model_free(view);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    view->metrics.edges_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_vectors(view, digests) != 0) {
        cbm_zova_publish_model_free(view);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    view->metrics.vectors_ms = model_elapsed_ms(&phase_started, &finished);
    view->metrics.normalization_ms = model_elapsed_ms(&started, &finished);
    view->prepared_pending = true;
    *out_view = view;
    return 0;
}

int cbm_zova_prepared_view_complete(
    cbm_zova_prepared_view_t *view,
    const cbm_zova_file_hash_input_t *file_hashes, int file_hash_count,
    const cbm_zova_project_summary_input_t *project_summary) {
    if (!view || !view->prepared_pending || file_hash_count < 0 ||
        (file_hash_count > 0 && !file_hashes)) return -1;
    cbm_zova_project_summary_input_t summary = project_summary
        ? *project_summary : (cbm_zova_project_summary_input_t){0};
    if (summary.present &&
        (!summary.summary || !summary.source_hash ||
         !summary.created_at || !summary.updated_at)) return -1;
    view->input.file_hashes = file_hashes;
    view->input.file_hash_count = file_hash_count;
    view->input.project_summary = summary;
    struct timespec started = {0}, phase = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    phase = started;
    if (model_build_hashes(view, &view->pending_digests) != 0) return -1;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    view->metrics.hashes_ms = model_elapsed_ms(&phase, &finished);
    model_digest_summary(view, &view->pending_digests);
    phase = finished;
    model_digest_finish_all(view, &view->pending_digests);
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    view->metrics.digests_ms = model_elapsed_ms(&phase, &finished);
    view->metrics.normalization_ms += model_elapsed_ms(&started, &finished);
    view->prepared_pending = false;
    return 0;
}

int cbm_zova_prepared_view_build(
    const char *workspace_id, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_prepared_view_t **out_view) {
    if (!input || !out_view) return -1;
    *out_view = NULL;
    cbm_zova_workspace_generation_input_t base = *input;
    base.file_hashes = NULL;
    base.file_hash_count = 0;
    base.project_summary = (cbm_zova_project_summary_input_t){0};
    cbm_zova_prepared_view_t *view = NULL;
    if (cbm_zova_prepared_view_build_base(workspace_id, &base, &view) != 0)
        return -1;
    if (cbm_zova_prepared_view_complete(
            view, input->file_hashes, input->file_hash_count,
            &input->project_summary) != 0) {
        cbm_zova_prepared_view_free(view);
        return -1;
    }
    *out_view = view;
    return 0;
}

void cbm_zova_prepared_view_free(cbm_zova_prepared_view_t *view) {
    cbm_zova_publish_model_free(view);
}

void cbm_zova_publish_model_free(cbm_zova_publish_model_t *model) {
    if (!model) return;
    free(model->nodes);
    free(model->node_id_storage);
    free(model->node_fts_storage);
    free(model->dense_ids);
    free(model->dense_ordinals);
    free(model->id_map);
    free(model->edges);
    free(model->edge_id_storage);
    free(model->topology);
    free(model->edge_payload_storage);
    free(model->hashes);
    free(model->node_vectors);
    free(model->token_vectors);
    free(model->token_id_storage);
    free(model);
}

const char *cbm_zova_publish_model_workspace_id(const cbm_zova_publish_model_t *model) {
    return model ? model->workspace_id : NULL;
}

const cbm_zova_workspace_generation_input_t *cbm_zova_publish_model_input(
    const cbm_zova_publish_model_t *model) { return model ? &model->input : NULL; }
int cbm_zova_publish_model_node_count(const cbm_zova_publish_model_t *model) {
    return model ? model->node_count : 0;
}
const cbm_zova_publish_node_t *cbm_zova_publish_model_node_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->node_count ? &model->nodes[index].value : NULL;
}
int cbm_zova_publish_model_set_node_source_ordinal(
    cbm_zova_publish_model_t *model, int index, uint64_t source_ordinal) {
    if (!model || index < 0 || index >= model->node_count || source_ordinal > INT64_MAX) return -1;
    model->nodes[index].value.source_ordinal = source_ordinal;
    return 0;
}
const char *cbm_zova_publish_model_fts_name_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->node_count ? model->nodes[index].fts_name : NULL;
}
const char *cbm_zova_publish_model_stable_id_for_dump_id(
    const cbm_zova_publish_model_t *model, int64_t dump_id) {
    return model ? model_lookup(model, dump_id) : NULL;
}
int cbm_zova_publish_model_edge_count(const cbm_zova_publish_model_t *model) {
    return model ? model->edge_count : 0;
}
const cbm_zova_publish_edge_t *cbm_zova_publish_model_edge_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->edge_count ? &model->edges[index].value : NULL;
}
int cbm_zova_publish_model_topology_count(const cbm_zova_publish_model_t *model) {
    return model ? model->topology_count : 0;
}
const cbm_zova_publish_edge_t *cbm_zova_publish_model_topology_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->topology_count
        ? &model->topology[index].value : NULL;
}
int cbm_zova_publish_model_file_hash_count(const cbm_zova_publish_model_t *model) {
    return model ? model->hash_count : 0;
}
const cbm_zova_file_hash_input_t *cbm_zova_publish_model_file_hash_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->hash_count ? model->hashes[index].source : NULL;
}
int cbm_zova_publish_model_node_vector_count(const cbm_zova_publish_model_t *model) {
    return model ? model->node_vector_count : 0;
}
const cbm_zova_publish_node_vector_t *cbm_zova_publish_model_node_vector_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->node_vector_count
        ? &model->node_vectors[index].value : NULL;
}
int cbm_zova_publish_model_token_vector_count(const cbm_zova_publish_model_t *model) {
    return model ? model->token_vector_count : 0;
}
const cbm_zova_publish_token_vector_t *cbm_zova_publish_model_token_vector_at(
    const cbm_zova_publish_model_t *model, int index) {
    return model && index >= 0 && index < model->token_vector_count
        ? &model->token_vectors[index].value : NULL;
}
const cbm_zova_workspace_generation_result_t *cbm_zova_publish_model_digests(
    const cbm_zova_publish_model_t *model) {
    return model && !model->prepared_pending ? &model->digests : NULL;
}
void cbm_zova_publish_model_metrics(const cbm_zova_publish_model_t *model,
                                    cbm_zova_publish_model_metrics_t *out_metrics) {
    if (!out_metrics) return;
    if (model) *out_metrics = model->metrics;
    else memset(out_metrics, 0, sizeof(*out_metrics));
}

int cbm_zova_publish_model_reuse_vector_digests(
    cbm_zova_publish_model_t *model,
    const cbm_zova_workspace_generation_result_t *previous,
    uint32_t components) {
    const uint32_t vector_components = CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
                                       CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS;
    if (!model || !previous || components == 0 || (components & ~vector_components) != 0)
        return -1;
    if ((components & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) != 0) {
        if (model->node_vector_count != 0 || previous->node_vectors == 0 ||
            strlen(previous->node_vector_sha256) != CBM_ZOVA_DIGEST_HEX_SIZE - 1)
            return -1;
        memcpy(model->digests.node_vector_sha256, previous->node_vector_sha256,
               CBM_ZOVA_DIGEST_HEX_SIZE);
        model->digests.node_vectors = previous->node_vectors;
        model->digests.node_vector_rows = previous->node_vector_rows;
    }
    if ((components & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) != 0) {
        if (model->token_vector_count != 0 || previous->token_vectors == 0 ||
            strlen(previous->token_vector_sha256) != CBM_ZOVA_DIGEST_HEX_SIZE - 1)
            return -1;
        memcpy(model->digests.token_vector_sha256, previous->token_vector_sha256,
               CBM_ZOVA_DIGEST_HEX_SIZE);
        model->digests.token_vectors = previous->token_vectors;
        model->digests.token_vector_rows = previous->token_vector_rows;
    }
    return 0;
}
