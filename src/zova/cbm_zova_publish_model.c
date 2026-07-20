#include "cbm_zova_publish_model.h"

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

struct cbm_zova_publish_model {
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    cbm_zova_workspace_generation_input_t input;
    model_node_t *nodes;
    int node_count;
    const char **dense_ids;
    model_id_map_t *id_map;
    model_edge_t *edges;
    int edge_count;
    model_edge_t *topology;
    int topology_count;
    model_hash_t *hashes;
    int hash_count;
    model_node_vector_t *node_vectors;
    int node_vector_count;
    model_token_vector_t *token_vectors;
    int token_vector_count;
    cbm_zova_workspace_generation_result_t digests;
    cbm_zova_publish_model_metrics_t metrics;
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

static char *model_dup(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    char *copy = model_malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
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

static char *model_camel_name(const char *input) {
    if (!input) return NULL;
    size_t length = strlen(input);
    size_t boundaries = 0;
    for (size_t i = 0; i < length; i++)
        if (model_camel_boundary(input, length, i)) boundaries++;
    if (length > (SIZE_MAX - 2) / 2 ||
        boundaries > SIZE_MAX - (length * 2) - 2) return NULL;
    char *value = model_malloc(length * 2 + boundaries + 2);
    if (!value) return NULL;
    memcpy(value, input, length);
    size_t output = length;
    value[output++] = ' ';
    for (size_t i = 0; i < length; i++) {
        if (model_camel_boundary(input, length, i)) value[output++] = ' ';
        value[output++] = input[i];
    }
    value[output] = '\0';
    return value;
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
    int comparison = strcmp(left->value.source_stable_id, right->value.source_stable_id);
    if (comparison != 0) return comparison;
    comparison = strcmp(left->value.source->type, right->value.source->type);
    if (comparison != 0) return comparison;
    return strcmp(left->value.target_stable_id, right->value.target_stable_id);
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

static int model_build_nodes(cbm_zova_publish_model_t *model) {
    int count = model->input.node_count;
    if (count == 0) return 0;
    model->nodes = model_calloc((size_t)count, sizeof(*model->nodes));
    model->dense_ids = model_calloc((size_t)count, sizeof(*model->dense_ids));
    if (!model->nodes || !model->dense_ids) return -1;
    model->node_count = count;
    int dense_ids = 1;
    for (int i = 0; i < count; i++) {
        const CBMDumpNode *node = &model->input.nodes[i];
        char stable_id[MODEL_ID_CAP];
        if (node->id <= 0 || !node->project || strcmp(node->project, model->input.project) != 0 ||
            !node->label || !node->name || !node->qualified_name || !node->file_path ||
            model_node_id(model->workspace_id, node, stable_id) != 0) return -1;
        char *owned_id = model_dup(stable_id);
        char *fts_name = model_camel_name(node->name);
        if (!owned_id || !fts_name) {
            free(owned_id);
            free(fts_name);
            return -1;
        }
        model->nodes[i].value = (cbm_zova_publish_node_t){
            .source = node, .stable_id = owned_id, .ordinal = 0,
            .source_ordinal = (uint64_t)i};
        model->nodes[i].fts_name = fts_name;
        if (node->id > count || model->dense_ids[node->id - 1]) {
            dense_ids = 0;
        } else {
            model->dense_ids[node->id - 1] = owned_id;
        }
        model->metrics.stable_node_id_computations++;
        model->metrics.camel_split_computations++;
    }
    if (!dense_ids) {
        free(model->dense_ids);
        model->dense_ids = NULL;
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
    }
    return 0;
}

static int model_build_edges(cbm_zova_publish_model_t *model) {
    int count = model->input.edge_count;
    if (count == 0) return 0;
    model->edges = model_calloc((size_t)count, sizeof(*model->edges));
    if (!model->edges) return -1;
    model->edge_count = count;
    for (int i = 0; i < count; i++) {
        const CBMDumpEdge *edge = &model->input.edges[i];
        const char *source = model_lookup(model, edge->source_id);
        const char *target = model_lookup(model, edge->target_id);
        model->metrics.endpoint_lookups += 2;
        if (!edge->project || strcmp(edge->project, model->input.project) != 0 ||
            !edge->type || !edge->type[0] || !source || !target) return -1;
        char edge_id[MODEL_ID_CAP];
        if (model_edge_id(model->workspace_id, source, edge->type, target,
                          edge->local_name ? edge->local_name : "", edge_id) != 0) return -1;
        char *owned_id = model_dup(edge_id);
        if (!owned_id) return -1;
        model->edges[i].value = (cbm_zova_publish_edge_t){
            .source = edge, .edge_id = owned_id, .source_stable_id = source,
            .target_stable_id = target};
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
            free((char *)model->edges[unique_edges - 1].value.edge_id);
            model->edges[unique_edges - 1] = model->edges[i];
        } else {
            if (unique_edges != i) model->edges[unique_edges] = model->edges[i];
            unique_edges++;
        }
    }
    model->edge_count = unique_edges;
    for (int i = 0; i < unique_edges; i++) model->edges[i].value.ordinal = (uint64_t)i;

    model->topology = model_calloc((size_t)unique_edges, sizeof(*model->topology));
    if (!model->topology) return -1;
    for (int i = 0; i < unique_edges; i++) model->topology[i] = model->edges[i];
    qsort(model->topology, (size_t)unique_edges, sizeof(*model->topology),
          model_topology_compare);
    model->metrics.global_sorts++;
    int unique_topology = 0;
    for (int i = 0; i < unique_edges; i++) {
        if (unique_topology == 0 ||
            model_topology_compare(&model->topology[unique_topology - 1], &model->topology[i]) != 0) {
            if (unique_topology != i) model->topology[unique_topology] = model->topology[i];
            model->topology[unique_topology].value.ordinal = (uint64_t)unique_topology;
            unique_topology++;
        }
    }
    model->topology_count = unique_topology;
    return 0;
}

static int model_build_hashes(cbm_zova_publish_model_t *model) {
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
    for (int i = 1; i < count; i++)
        if (strcmp(model->hashes[i - 1].source->file_path,
                   model->hashes[i].source->file_path) == 0) return -1;
    return 0;
}

static int model_build_vectors(cbm_zova_publish_model_t *model) {
    int node_capacity = model->input.node_vector_count;
    int token_capacity = model->input.token_vector_count;
    if (node_capacity > 0) {
        model->node_vectors = model_calloc((size_t)node_capacity, sizeof(*model->node_vectors));
        if (!model->node_vectors) return -1;
    }
    for (int i = 0; i < node_capacity; i++) {
        const CBMDumpVector *vector = &model->input.node_vectors[i];
        const char *stable_id = model_lookup(model, vector->node_id);
        if (!vector->project || strcmp(vector->project, model->input.project) != 0 ||
            !stable_id || !vector->vector || vector->vector_len != model->input.vector_dimensions)
            return -1;
        if (!model_nonzero(vector->vector, vector->vector_len)) continue;
        model->node_vectors[model->node_vector_count++].value =
            (cbm_zova_publish_node_vector_t){.source = vector, .stable_id = stable_id};
    }
    if (model->node_vector_count > 0) {
        qsort(model->node_vectors, (size_t)model->node_vector_count,
              sizeof(*model->node_vectors), model_node_vector_compare);
        model->metrics.global_sorts++;
        for (int i = 0; i < model->node_vector_count; i++) {
            if (i > 0 && strcmp(model->node_vectors[i - 1].value.stable_id,
                                model->node_vectors[i].value.stable_id) == 0) return -1;
            model->node_vectors[i].value.ordinal = (uint64_t)i;
        }
    }
    if (token_capacity > 0) {
        model->token_vectors = model_calloc((size_t)token_capacity,
                                            sizeof(*model->token_vectors));
        if (!model->token_vectors) return -1;
    }
    for (int i = 0; i < token_capacity; i++) {
        const CBMDumpTokenVec *vector = &model->input.token_vectors[i];
        if (vector->id <= 0 || !vector->project ||
            strcmp(vector->project, model->input.project) != 0 || !vector->token ||
            !vector->vector || vector->vector_len != model->input.vector_dimensions ||
            !isfinite(vector->idf)) return -1;
        char token_id[MODEL_ID_CAP];
        if (model_token_id(model->workspace_id, vector->token, token_id) != 0) return -1;
        model->metrics.token_id_computations++;
        if (!model_nonzero(vector->vector, vector->vector_len)) continue;
        char *owned_id = model_dup(token_id);
        if (!owned_id) return -1;
        model->token_vectors[model->token_vector_count++].value =
            (cbm_zova_publish_token_vector_t){.source = vector, .token_id = owned_id};
    }
    if (model->token_vector_count > 0) {
        qsort(model->token_vectors, (size_t)model->token_vector_count,
              sizeof(*model->token_vectors), model_token_vector_compare);
        model->metrics.global_sorts++;
        for (int i = 0; i < model->token_vector_count; i++) {
            if (i > 0 && strcmp(model->token_vectors[i - 1].value.token_id,
                                model->token_vectors[i].value.token_id) == 0) return -1;
            model->token_vectors[i].value.ordinal = (uint64_t)i;
        }
    }
    return 0;
}

static void model_build_digests(cbm_zova_publish_model_t *model) {
    cbm_sha256_backend_ctx metadata, fts, topology, nodes, tokens;
    cbm_sha256_backend_init(&metadata);
    cbm_sha256_backend_init(&fts);
    cbm_sha256_backend_init(&topology);
    cbm_sha256_backend_init(&nodes);
    cbm_sha256_backend_init(&tokens);
    for (int i = 0; i < model->node_count; i++) {
        const model_node_t *row = &model->nodes[i];
        const CBMDumpNode *node = row->value.source;
        model_digest_text(&metadata, row->value.stable_id);
        model_digest_text(&metadata, node->project);
        model_digest_text(&metadata, node->label);
        model_digest_text(&metadata, node->name);
        model_digest_text(&metadata, node->qualified_name);
        model_digest_text(&metadata, node->file_path);
        model_digest_i64(&metadata, node->start_line);
        model_digest_i64(&metadata, node->end_line);
        model_digest_text(&metadata, node->properties);
        model_digest_text(&fts, row->value.stable_id);
        model_digest_text(&fts, row->fts_name);
        model_digest_text(&fts, node->qualified_name);
        model_digest_text(&fts, node->label);
        model_digest_text(&fts, node->file_path);
    }
    for (int i = 0; i < model->edge_count; i++) {
        const cbm_zova_publish_edge_t *row = &model->edges[i].value;
        model_digest_text(&metadata, row->edge_id);
        model_digest_text(&metadata, row->source_stable_id);
        model_digest_text(&metadata, row->target_stable_id);
        model_digest_text(&metadata, row->source->type);
        model_digest_text(&metadata, row->source->properties);
        model_digest_text(&metadata, row->source->url_path);
        model_digest_text(&metadata, row->source->local_name);
    }
    for (int i = 0; i < model->topology_count; i++) {
        const cbm_zova_publish_edge_t *row = &model->topology[i].value;
        model_digest_text(&topology, row->source_stable_id);
        model_digest_text(&topology, row->source->type);
        model_digest_text(&topology, row->target_stable_id);
    }
    for (int i = 0; i < model->hash_count; i++) {
        const cbm_zova_file_hash_input_t *hash = model->hashes[i].source;
        model_digest_text(&metadata, hash->file_path);
        model_digest_text(&metadata, hash->content_hash);
        model_digest_i64(&metadata, hash->mtime_ns);
        model_digest_i64(&metadata, hash->size_bytes);
    }
    model_digest_text(&metadata, model->input.project_summary.present ? "1" : "0");
    if (model->input.project_summary.present) {
        model_digest_text(&metadata, model->input.project_summary.summary);
        model_digest_text(&metadata, model->input.project_summary.source_hash);
        model_digest_text(&metadata, model->input.project_summary.created_at);
        model_digest_text(&metadata, model->input.project_summary.updated_at);
    }
    for (int i = 0; i < model->node_vector_count; i++) {
        const cbm_zova_publish_node_vector_t *row = &model->node_vectors[i].value;
        model_digest_text(&nodes, row->stable_id);
        cbm_sha256_backend_update(&nodes, row->source->vector,
                                  (size_t)row->source->vector_len);
    }
    for (int i = 0; i < model->token_vector_count; i++) {
        const cbm_zova_publish_token_vector_t *row = &model->token_vectors[i].value;
        model_digest_text(&tokens, row->token_id);
        model_digest_text(&tokens, row->source->token);
        cbm_sha256_backend_update(&tokens, &row->source->idf, sizeof(row->source->idf));
        cbm_sha256_backend_update(&tokens, row->source->vector,
                                  (size_t)row->source->vector_len);
    }
    model_digest_finish(&metadata, model->digests.metadata_sha256);
    model_digest_finish(&fts, model->digests.fts_sha256);
    model_digest_finish(&topology, model->digests.topology_sha256);
    model_digest_finish(&nodes, model->digests.node_vector_sha256);
    model_digest_finish(&tokens, model->digests.token_vector_sha256);
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
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    if (model_build_nodes(model) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.nodes_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_edges(model) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.edges_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_hashes(model) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.hashes_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    if (model_build_vectors(model) != 0) {
        cbm_zova_publish_model_free(model);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.vectors_ms = model_elapsed_ms(&phase_started, &finished);
    phase_started = finished;
    model_build_digests(model);
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    model->metrics.digests_ms = model_elapsed_ms(&phase_started, &finished);
    model->metrics.normalization_ms = model_elapsed_ms(&started, &finished);
    model_build_count++;
    *out_model = model;
    return 0;
}

void cbm_zova_publish_model_free(cbm_zova_publish_model_t *model) {
    if (!model) return;
    for (int i = 0; i < model->node_count; i++) {
        free((char *)model->nodes[i].value.stable_id);
        free(model->nodes[i].fts_name);
    }
    for (int i = 0; i < model->edge_count; i++) free((char *)model->edges[i].value.edge_id);
    for (int i = 0; i < model->token_vector_count; i++)
        free((char *)model->token_vectors[i].value.token_id);
    free(model->nodes);
    free(model->dense_ids);
    free(model->id_map);
    free(model->edges);
    free(model->topology);
    free(model->hashes);
    free(model->node_vectors);
    free(model->token_vectors);
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
    const cbm_zova_publish_model_t *model) { return model ? &model->digests : NULL; }
void cbm_zova_publish_model_metrics(const cbm_zova_publish_model_t *model,
                                    cbm_zova_publish_model_metrics_t *out_metrics) {
    if (!out_metrics) return;
    if (model) *out_metrics = model->metrics;
    else memset(out_metrics, 0, sizeof(*out_metrics));
}
