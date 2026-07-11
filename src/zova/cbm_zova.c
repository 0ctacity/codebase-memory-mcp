#include "zova/cbm_zova.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_regex.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

enum {
    ZV_PATH_MAX = 1024,
    ZV_BATCH = 128,
    ZV_ID_MAX = 32,
    ZV_SQL_MAX = 512,
};

#define CBM_ZOVA_VECTOR_META_TABLE "_cbm_zova_vector_meta"

bool cbm_zova_build_enabled(void) {
    return CBM_WITH_ZOVA != 0;
}

cbm_zova_mode_t cbm_zova_mode_from_env(void) {
    const char *mode = getenv("CBM_ZOVA_MODE");
    if (!mode || mode[0] == '\0' || strcmp(mode, "off") == 0) {
        return CBM_ZOVA_MODE_OFF;
    }
    if (strcmp(mode, "container") == 0) {
        return CBM_ZOVA_MODE_CONTAINER;
    }
    if (strcmp(mode, "i8_vectors") == 0) {
        return CBM_ZOVA_MODE_I8_VECTORS;
    }
    if (strcmp(mode, "graph_mirror") == 0) {
        return CBM_ZOVA_MODE_GRAPH_MIRROR;
    }
    if (strcmp(mode, "graph_read") == 0) {
        return CBM_ZOVA_MODE_GRAPH_READ;
    }
    return CBM_ZOVA_MODE_OFF;
}

const char *cbm_zova_mode_name(cbm_zova_mode_t mode) {
    switch (mode) {
    case CBM_ZOVA_MODE_OFF:
        return "off";
    case CBM_ZOVA_MODE_CONTAINER:
        return "container";
    case CBM_ZOVA_MODE_I8_VECTORS:
        return "i8_vectors";
    case CBM_ZOVA_MODE_GRAPH_MIRROR:
        return "graph_mirror";
    case CBM_ZOVA_MODE_GRAPH_READ:
        return "graph_read";
    }
    return "off";
}

bool cbm_zova_graph_read_is_enabled(void) {
    const char *mode = getenv("CBM_ZOVA_MODE");
    return !mode || mode[0] == '\0' || strcmp(mode, "graph_read") == 0;
}

bool cbm_zova_should_use_full_vector_search(int candidate_count, int collection_count) {
    return candidate_count > 0 && collection_count > 0 && candidate_count == collection_count;
}

int cbm_zova_sidecar_path(const char *db_path, char *out, size_t out_sz) {
    if (!db_path || !out || out_sz == 0) {
        return -1;
    }
    size_t len = strlen(db_path);
    if (len > 3 && strcmp(db_path + len - 3, ".db") == 0) {
        if (len - 3 + strlen(".zova") + 1 > out_sz) {
            return -1;
        }
        memcpy(out, db_path, len - 3);
        memcpy(out + len - 3, ".zova", strlen(".zova") + 1);
        return 0;
    }
    if (snprintf(out, out_sz, "%s.zova", db_path) >= (int)out_sz) {
        return -1;
    }
    return 0;
}

int cbm_zova_workspace_registry_path(char *out, size_t out_size) {
    const char *cache_dir = cbm_resolve_cache_dir();
    if (!cache_dir || !out || out_size == 0) {
        return -1;
    }
    return snprintf(out, out_size, "%s/cbm.zova", cache_dir) < (int)out_size ? 0 : -1;
}

static bool workspace_name_component_valid(const char *value) {
    if (!value || !value[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

int cbm_zova_workspace_graph_name(const char *workspace_id, char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) || !out || out_size == 0) {
        return -1;
    }
    return snprintf(out, out_size, "cbm_graph_%s", workspace_id) < (int)out_size ? 0 : -1;
}

int cbm_zova_workspace_node_vector_collection_name(const char *workspace_id,
                                                    const char *model_fingerprint, int dimensions,
                                                    char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) ||
        !workspace_name_component_valid(model_fingerprint) || dimensions <= 0 || !out ||
        out_size == 0) {
        return -1;
    }
    return snprintf(out, out_size, "cbm_nodes_i8_%s_%s_d%d", workspace_id, model_fingerprint,
                    dimensions) < (int)out_size
               ? 0
               : -1;
}

int cbm_zova_workspace_token_vector_collection_name(const char *workspace_id,
                                                     const char *model_fingerprint, int dimensions,
                                                     char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) ||
        !workspace_name_component_valid(model_fingerprint) || dimensions <= 0 || !out ||
        out_size == 0) {
        return -1;
    }
    return snprintf(out, out_size, "cbm_tokens_i8_%s_%s_d%d", workspace_id, model_fingerprint,
                    dimensions) < (int)out_size
               ? 0
               : -1;
}

int cbm_zova_workspace_node_id_v1(const char *workspace_id, const char *node_kind,
                                  const char *relative_path, const char *qualified_name,
                                  const char *semantic_discriminator, char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) || !node_kind || !relative_path ||
        !qualified_name || !semantic_discriminator || !out || out_size == 0) {
        return -1;
    }
    const char *parts[] = {node_kind, relative_path, qualified_name, semantic_discriminator};
    cbm_sha256_ctx hash;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    char hex[33];
    static const char digits[] = "0123456789abcdef";
    cbm_sha256_init(&hash);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        cbm_sha256_update(&hash, parts[i], strlen(parts[i]));
        cbm_sha256_update(&hash, "\0", 1);
    }
    cbm_sha256_final(&hash, digest);
    for (size_t i = 0; i < 16; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[32] = '\0';
    return snprintf(out, out_size, "n:v1:%s:%s", workspace_id, hex) < (int)out_size ? 0 : -1;
}

void cbm_zova_node_candidates_free(cbm_zova_node_candidate_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].qualified_name);
        free(items[i].file_path);
        free(items[i].label);
        free(items[i].vector);
    }
    free(items);
}

void cbm_zova_graph_visits_free(cbm_zova_graph_visit_t *visits, int count) {
    if (!visits) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(visits[i].node_id);
        free(visits[i].name);
        free(visits[i].qualified_name);
        free(visits[i].file_path);
        free(visits[i].label);
    }
    free(visits);
}

#if !CBM_WITH_ZOVA

static int zova_disabled_if_requested(void) {
    cbm_zova_mode_t mode = cbm_zova_mode_from_env();
    if (mode == CBM_ZOVA_MODE_OFF) {
        return 0;
    }
    cbm_log_error("zova.disabled", "mode", cbm_zova_mode_name(mode), "reason",
                  "build_without_CBM_WITH_ZOVA");
    return -1;
}

static int cbm_zova_after_sqlite_dump_impl(const char *db_path,
                                           const CBMDumpVector *node_vectors,
                                           int node_vector_count,
                                           const CBMDumpTokenVec *token_vectors,
                                           int token_vector_count, int vector_dim,
                                           bool direct_vectors, const char *root_path,
                                           const char *project, const CBMDumpNode *nodes,
                                           int node_count, const CBMDumpEdge *edges,
                                           int edge_count) {
    (void)db_path;
    (void)root_path;
    (void)project;
    return zova_disabled_if_requested();
}

int cbm_zova_after_sqlite_dump(const char *db_path) {
    return cbm_zova_after_sqlite_dump_impl(db_path, NULL, 0, NULL, 0, 0, false, NULL, NULL,
                                           NULL, 0, NULL, 0);
}

int cbm_zova_after_sqlite_dump_with_i8_vectors(const char *db_path,
                                                const CBMDumpVector *node_vectors,
                                                int node_vector_count,
                                                const CBMDumpTokenVec *token_vectors,
                                                int token_vector_count, int vector_dim) {
    (void)db_path;
    (void)node_vectors;
    (void)node_vector_count;
    (void)token_vectors;
    (void)token_vector_count;
    (void)vector_dim;
    return zova_disabled_if_requested();
}

int cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
    const char *db_path, const char *root_path, const char *project,
    const CBMDumpVector *node_vectors, int node_vector_count,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim) {
    (void)db_path;
    (void)root_path;
    (void)project;
    (void)node_vectors;
    (void)node_vector_count;
    (void)token_vectors;
    (void)token_vector_count;
    (void)vector_dim;
    return zova_disabled_if_requested();
}

int cbm_zova_after_sqlite_dump_workspace_direct(
    const char *db_path, const char *root_path, const char *project, const CBMDumpNode *nodes,
    int node_count, const CBMDumpEdge *edges, int edge_count, const CBMDumpVector *node_vectors,
    int node_vector_count, const CBMDumpTokenVec *token_vectors, int token_vector_count,
    int vector_dim) {
    (void)db_path;
    (void)root_path;
    (void)project;
    (void)nodes;
    (void)node_count;
    (void)edges;
    (void)edge_count;
    (void)node_vectors;
    (void)node_vector_count;
    (void)token_vectors;
    (void)token_vector_count;
    (void)vector_dim;
    return zova_disabled_if_requested();
}

int cbm_zova_validate_container(const char *zova_path) {
    (void)zova_path;
    return -1;
}

int cbm_zova_mirror_i8_vectors(const char *zova_path, int vector_dim) {
    (void)zova_path;
    (void)vector_dim;
    return -1;
}

int cbm_zova_write_i8_vectors_direct(const char *zova_path,
                                     const CBMDumpVector *node_vectors, int node_vector_count,
                                     const CBMDumpTokenVec *token_vectors, int token_vector_count,
                                     int vector_dim) {
    (void)zova_path;
    (void)node_vectors;
    (void)node_vector_count;
    (void)token_vectors;
    (void)token_vector_count;
    (void)vector_dim;
    return -1;
}

int cbm_zova_write_workspace_node_i8_vectors_direct(
    const char *zova_path, const char *workspace_id, const char *model_fingerprint,
    const CBMDumpVector *node_vectors, int node_vector_count, int vector_dim) {
    (void)zova_path;
    (void)workspace_id;
    (void)model_fingerprint;
    (void)node_vectors;
    (void)node_vector_count;
    (void)vector_dim;
    return -1;
}

int cbm_zova_delete_workspace_node_i8_vectors(const char *zova_path, const char *workspace_id,
                                               const char *model_fingerprint, int vector_dim) {
    (void)zova_path;
    (void)workspace_id;
    (void)model_fingerprint;
    (void)vector_dim;
    return -1;
}

int cbm_zova_write_workspace_token_i8_vectors_direct(
    const char *zova_path, const char *workspace_id, const char *model_fingerprint,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim) {
    (void)zova_path;
    (void)workspace_id;
    (void)model_fingerprint;
    (void)token_vectors;
    (void)token_vector_count;
    (void)vector_dim;
    return -1;
}

int cbm_zova_delete_workspace_token_i8_vectors(const char *zova_path, const char *workspace_id,
                                                const char *model_fingerprint, int vector_dim) {
    (void)zova_path;
    (void)workspace_id;
    (void)model_fingerprint;
    (void)vector_dim;
    return -1;
}

int cbm_zova_mirror_graph(const char *zova_path) {
    (void)zova_path;
    return -1;
}

int cbm_zova_mirror_workspace_graph(const char *zova_path, const char *workspace_id,
                                    const char *project) {
    (void)zova_path;
    (void)workspace_id;
    (void)project;
    return -1;
}

int cbm_zova_delete_workspace_graph(const char *zova_path, const char *workspace_id) {
    (void)zova_path;
    (void)workspace_id;
    return -1;
}

int cbm_zova_benchmark_workspace_graph_ingestion(
    const char *db_path, const char *direct_zova_path, const char *mirror_zova_path,
    const char *workspace_id, const char *project, const CBMDumpNode *nodes, int node_count,
    const CBMDumpEdge *edges, int edge_count, cbm_zova_graph_ingestion_metrics_t *out_metrics) {
    (void)db_path;
    (void)direct_zova_path;
    (void)mirror_zova_path;
    (void)workspace_id;
    (void)project;
    (void)nodes;
    (void)node_count;
    (void)edges;
    (void)edge_count;
    if (out_metrics) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }
    return -1;
}

int cbm_zova_workspace_get_or_create_at(const char *registry_path, const char *root_path,
                                        char *out_workspace_id, size_t out_workspace_id_size) {
    (void)registry_path;
    (void)root_path;
    (void)out_workspace_id;
    (void)out_workspace_id_size;
    return -1;
}

int cbm_zova_workspace_lookup_at(const char *registry_path, const char *root_path,
                                 char *out_workspace_id, size_t out_workspace_id_size) {
    (void)registry_path;
    (void)root_path;
    if (out_workspace_id && out_workspace_id_size > 0) {
        out_workspace_id[0] = '\0';
    }
    return -1;
}

int cbm_zova_workspace_generation_begin_at(const char *registry_path, const char *workspace_id,
                                            int64_t generation) {
    (void)registry_path;
    (void)workspace_id;
    (void)generation;
    return -1;
}

int cbm_zova_workspace_generation_finish_at(const char *registry_path, const char *workspace_id,
                                             int64_t generation, bool ready) {
    (void)registry_path;
    (void)workspace_id;
    (void)generation;
    (void)ready;
    return -1;
}

int cbm_zova_workspace_active_generation_at(const char *registry_path, const char *workspace_id,
                                             int64_t *out_generation) {
    (void)registry_path;
    (void)workspace_id;
    (void)out_generation;
    return -1;
}

int cbm_zova_workspace_next_generation_at(const char *registry_path, const char *workspace_id,
                                           int64_t *out_generation) {
    (void)registry_path;
    (void)workspace_id;
    if (out_generation) {
        *out_generation = 0;
    }
    return -1;
}

int cbm_zova_sidecar_generation_get(const char *zova_path, int64_t *out_generation) {
    (void)zova_path;
    if (out_generation) {
        *out_generation = 0;
    }
    return -1;
}

int cbm_zova_vector_prefetch_nodes(const char *zova_path, const char *project,
                                   const int8_t *query, int vector_dim, int limit,
                                   cbm_zova_node_candidate_t **out, int *out_count) {
    (void)zova_path;
    (void)project;
    (void)query;
    (void)vector_dim;
    (void)limit;
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    return -1;
}

cbm_zova_vector_session_t *cbm_zova_vector_session_open(const char *zova_path) {
    (void)zova_path;
    return NULL;
}

cbm_zova_graph_session_t *cbm_zova_graph_session_open(const char *zova_path) {
    (void)zova_path;
    return NULL;
}

int cbm_zova_graph_session_generation(const cbm_zova_graph_session_t *session,
                                      int64_t *out_generation) {
    (void)session;
    if (out_generation) {
        *out_generation = 0;
    }
    return -1;
}

void cbm_zova_graph_session_close(cbm_zova_graph_session_t *session) {
    (void)session;
}

int cbm_zova_graph_session_walk_calls(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *start_node_id, const char *direction, int max_depth, int max_results,
    cbm_zova_graph_visit_t **out_visits, int *out_count, cbm_zova_graph_metrics_t *out_metrics) {
    (void)session;
    (void)workspace_id;
    (void)graph_name;
    (void)start_node_id;
    (void)direction;
    (void)max_depth;
    (void)max_results;
    if (out_visits) {
        *out_visits = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (out_metrics) {
        memset(out_metrics, 0, sizeof(*out_metrics));
        out_metrics->fallback = true;
    }
    return -1;
}

void cbm_zova_vector_session_close(cbm_zova_vector_session_t *session) {
    (void)session;
}

int cbm_zova_vector_session_generation(const cbm_zova_vector_session_t *session,
                                       int64_t *out_generation) {
    (void)session;
    if (out_generation) {
        *out_generation = 0;
    }
    return -1;
}

int cbm_zova_vector_session_has_workspace(const cbm_zova_vector_session_t *session,
                                          const char *workspace_id, bool *out_present) {
    (void)session;
    (void)workspace_id;
    if (out_present) {
        *out_present = false;
    }
    return -1;
}

int cbm_zova_vector_session_prefetch_nodes_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query, int vector_dim,
    int limit, bool include_vector, cbm_zova_node_candidate_t **out, int *out_count,
    cbm_zova_vector_prefetch_metrics_t *metrics) {
    (void)session;
    (void)project;
    (void)query;
    (void)vector_dim;
    (void)limit;
    (void)include_vector;
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
    return -1;
}

int cbm_zova_vector_session_prefetch_multi_i8_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_node_candidate_t **out, int *out_count, cbm_zova_vector_prefetch_metrics_t *metrics) {
    (void)session;
    (void)project;
    (void)query_values;
    (void)query_count;
    (void)vector_dim;
    (void)prefilter_limit;
    (void)limit;
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
    return -1;
}

int cbm_zova_graph_neighbor_count(const char *zova_path, const char *node_id, int *out_count) {
    (void)zova_path;
    (void)node_id;
    if (out_count) {
        *out_count = 0;
    }
    return -1;
}

#else

#include "zova.h"

#include <errno.h>
#include <sqlite3.h>

typedef struct {
    zova_database *db;
} cbm_zova_db_t;

struct cbm_zova_vector_session {
    cbm_zova_db_t zdb;
    int64_t generation;
    bool generation_valid;
};

struct cbm_zova_graph_session {
    cbm_zova_db_t zdb;
    int64_t generation;
    bool generation_valid;
};

enum {
    ZV_CAMEL_SPLIT_BUF = 2048,
    ZV_CAMEL_BUF_GUARD = 2,
    ZV_GRAPH_HYDRATE_BATCH = 128,
};

#define ZV_DENOM_EPS_D 1e-10
#define CBM_ZOVA_SIDECAR_GENERATION_TABLE "cbm_zova_sidecar_generation_v1"

static char *zv_strndup(const char *data, size_t len) {
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    return copy;
}

static bool zv_camel_should_split(const char *input, size_t len, size_t i) {
    if (i == 0) {
        return false;
    }
    char curr = input[i];
    char prev = input[i - 1];
    char next = i + 1 < len ? input[i + 1] : '\0';
    if (curr >= 'A' && curr <= 'Z' && prev >= 'a' && prev <= 'z') {
        return true;
    }
    if (curr >= 'A' && curr <= 'Z' && prev >= 'A' && prev <= 'Z' && next >= 'a' &&
        next <= 'z') {
        return true;
    }
    return false;
}

static void sql_result_int(zova_sql_result *out, int64_t value) {
    memset(out, 0, sizeof(*out));
    out->result_type = ZOVA_SQL_RESULT_INTEGER;
    out->int64_value = value;
}

static void sql_result_double(zova_sql_result *out, double value) {
    memset(out, 0, sizeof(*out));
    out->result_type = ZOVA_SQL_RESULT_FLOAT;
    out->double_value = value;
}

static void sql_result_text(zova_sql_result *out, const char *data, size_t len) {
    memset(out, 0, sizeof(*out));
    out->result_type = ZOVA_SQL_RESULT_TEXT;
    out->data = data;
    out->data_len = len;
}

static void sql_result_error(zova_sql_result *out, const char *message) {
    memset(out, 0, sizeof(*out));
    out->result_type = ZOVA_SQL_RESULT_ERROR;
    out->error_message = message ? message : "zova sql callback error";
    out->error_message_len = strlen(out->error_message);
}

static const zova_sql_value *sql_arg(const zova_sql_function_call *call, size_t index) {
    if (!call || !call->argv || index >= call->argc) {
        return NULL;
    }
    return &call->argv[index];
}

static char *sql_text_copy(const zova_sql_value *value) {
    if (!value || value->value_type != ZOVA_SQL_VALUE_TEXT) {
        return NULL;
    }
    if (!value->data && value->data_len != 0) {
        return NULL;
    }
    return zv_strndup((const char *)(value->data ? value->data : ""), value->data_len);
}

static void zova_sql_cosine_i8(void *user_data, const zova_sql_function_call *call,
                               zova_sql_result *out) {
    (void)user_data;
    const zova_sql_value *a_value = sql_arg(call, 0);
    const zova_sql_value *b_value = sql_arg(call, 1);
    if (!a_value || !b_value || a_value->value_type != ZOVA_SQL_VALUE_BLOB ||
        b_value->value_type != ZOVA_SQL_VALUE_BLOB || a_value->data_len != b_value->data_len ||
        a_value->data_len == 0 || !a_value->data || !b_value->data) {
        sql_result_double(out, 0.0);
        return;
    }

    const int8_t *a = (const int8_t *)a_value->data;
    const int8_t *b = (const int8_t *)b_value->data;
    int64_t dot = 0;
    int64_t mag_a = 0;
    int64_t mag_b = 0;
    for (size_t i = 0; i < a_value->data_len; i++) {
        dot += (int64_t)a[i] * (int64_t)b[i];
        mag_a += (int64_t)a[i] * (int64_t)a[i];
        mag_b += (int64_t)b[i] * (int64_t)b[i];
    }

    double denom = sqrt((double)mag_a) * sqrt((double)mag_b);
    sql_result_double(out, denom > ZV_DENOM_EPS_D ? (double)dot / denom : 0.0);
}

static void zova_sql_camel_split(void *user_data, const zova_sql_function_call *call,
                                 zova_sql_result *out) {
    (void)user_data;
    const zova_sql_value *value = sql_arg(call, 0);
    if (!value || value->value_type != ZOVA_SQL_VALUE_TEXT) {
        sql_result_text(out, "", 0);
        return;
    }
    const char *input = (const char *)(value->data ? value->data : "");
    size_t input_len = value->data_len;
    if (input_len == 0) {
        sql_result_text(out, "", 0);
        return;
    }
    char buf[ZV_CAMEL_SPLIT_BUF];
    if (input_len + 1 >= sizeof(buf)) {
        sql_result_text(out, input, input_len);
        return;
    }
    memcpy(buf, input, input_len);
    size_t len = input_len;
    buf[len++] = ' ';
    for (size_t i = 0; i < input_len && len < sizeof(buf) - ZV_CAMEL_BUF_GUARD; i++) {
        if (zv_camel_should_split(input, input_len, i)) {
            buf[len++] = ' ';
        }
        buf[len++] = input[i];
    }
    buf[len] = '\0';
    sql_result_text(out, buf, len);
}

static void zova_sql_regexp_impl(const zova_sql_function_call *call, zova_sql_result *out,
                                 int flags) {
    char *pattern = sql_text_copy(sql_arg(call, 0));
    char *text = sql_text_copy(sql_arg(call, 1));
    if (!pattern || !text) {
        free(pattern);
        free(text);
        sql_result_int(out, 0);
        return;
    }

    cbm_regex_t re;
    if (cbm_regcomp(&re, pattern, flags) != 0) {
        free(pattern);
        free(text);
        sql_result_error(out, "invalid regex");
        return;
    }
    int matched = cbm_regexec(&re, text, 0, NULL, 0) == 0 ? 1 : 0;
    cbm_regfree(&re);
    free(pattern);
    free(text);
    sql_result_int(out, matched);
}

static void zova_sql_regexp(void *user_data, const zova_sql_function_call *call,
                            zova_sql_result *out) {
    (void)user_data;
    zova_sql_regexp_impl(call, out, CBM_REG_EXTENDED | CBM_REG_NOSUB);
}

static void zova_sql_iregexp(void *user_data, const zova_sql_function_call *call,
                             zova_sql_result *out) {
    (void)user_data;
    zova_sql_regexp_impl(call, out, CBM_REG_EXTENDED | CBM_REG_NOSUB | CBM_REG_ICASE);
}

static int status_ok(zova_status status, zova_database *db, const char *phase) {
    if (status == ZOVA_OK) {
        return 0;
    }
    const char *msg = db ? zova_database_last_error_message(db) : NULL;
    cbm_log_error("zova.err", "phase", phase ? phase : "unknown", "status",
                  zova_status_name(status), "msg", msg ? msg : "");
    return -1;
}

static void close_zova(cbm_zova_db_t *zdb);

static int register_zova_sql_function(zova_database *db, const char *name, int arity,
                                      zova_sql_scalar_callback callback) {
    zova_sql_function_register_request req = {
        .db = db,
        .name = name,
        .arity = arity,
        .flags = ZOVA_SQL_FUNCTION_DETERMINISTIC,
        .user_data = NULL,
        .callback = callback,
        .destroy = NULL,
    };
    return status_ok(zova_database_register_function(&req), db, name);
}

int cbm_zova_register_sql_functions(zova_database *db) {
    if (!db) {
        return -1;
    }
    if (zova_abi_version_major() != 0 || zova_abi_version_minor() != 22) {
        cbm_log_error("zova.abi", "expected", "0.22.x", "actual",
                      zova_abi_version_string());
        return -1;
    }
    if (register_zova_sql_function(db, "cbm_cosine_i8", 2, zova_sql_cosine_i8) != 0) {
        return -1;
    }
    if (register_zova_sql_function(db, "cbm_camel_split", 1, zova_sql_camel_split) != 0) {
        return -1;
    }
    if (register_zova_sql_function(db, "regexp", 2, zova_sql_regexp) != 0) {
        return -1;
    }
    if (register_zova_sql_function(db, "iregexp", 2, zova_sql_iregexp) != 0) {
        return -1;
    }
    return 0;
}

static int open_zova(const char *path, bool read_only, cbm_zova_db_t *out) {
    if (!path || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    zova_message err = {0};
    zova_status st;
    if (read_only) {
        zova_database_open_options_request req = {
            .path = path,
            .flags = ZOVA_OPEN_READ_ONLY,
            .busy_timeout_ms = 5000,
            .out_db = &out->db,
            .out_error_message = &err,
        };
        st = zova_database_open_with_options(&req);
    } else {
        zova_database_open_request req = {
            .path = path,
            .out_db = &out->db,
            .out_error_message = &err,
        };
        st = zova_database_open(&req);
    }
    if (st != ZOVA_OK) {
        cbm_log_error("zova.open", "path", path, "status", zova_status_name(st), "msg",
                      err.data ? err.data : "");
        zova_message_free(&err);
        return -1;
    }
    zova_message_free(&err);
    if (cbm_zova_register_sql_functions(out->db) != 0) {
        close_zova(out);
        return -1;
    }
    return 0;
}

static void close_zova(cbm_zova_db_t *zdb) {
    if (zdb && zdb->db) {
        (void)zova_database_close(zdb->db);
        zdb->db = NULL;
    }
}

static int prepare_zova(zova_database *db, const char *sql, zova_statement **out,
                        const char *phase) {
    zova_database_prepare_request req = {.db = db, .sql = sql, .out_statement = out};
    return status_ok(zova_database_prepare(&req), db, phase);
}

static int open_or_create_zova(const char *path, cbm_zova_db_t *out) {
    if (cbm_file_exists(path)) {
        return open_zova(path, false, out);
    }
    memset(out, 0, sizeof(*out));
    zova_message err = {0};
    zova_database_open_request req = {
        .path = path,
        .out_db = &out->db,
        .out_error_message = &err,
    };
    zova_status status = zova_database_create(&req);
    if (status != ZOVA_OK) {
        cbm_log_error("zova.registry_create", "path", path, "status", zova_status_name(status),
                      "msg", err.data ? err.data : "");
        zova_message_free(&err);
        return -1;
    }
    zova_message_free(&err);
    if (cbm_zova_register_sql_functions(out->db) != 0) {
        close_zova(out);
        return -1;
    }
    return 0;
}

static int workspace_registry_init(zova_database *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS cbm_workspace_registry("
        "workspace_id TEXT PRIMARY KEY, canonical_root TEXT NOT NULL UNIQUE, "
        "id_format_version INTEGER NOT NULL, active_generation INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS cbm_workspace_generations("
        "workspace_id TEXT NOT NULL, generation INTEGER NOT NULL, state TEXT NOT NULL "
        "CHECK(state IN ('building','ready','failed','retired')), "
        "PRIMARY KEY(workspace_id,generation), "
        "FOREIGN KEY(workspace_id) REFERENCES cbm_workspace_registry(workspace_id));";
    zova_database_exec_request req = {.db = db, .sql = sql};
    return status_ok(zova_database_exec(&req), db, "workspace.registry_init");
}

static int workspace_registry_begin(zova_database *db) {
    zova_database_simple_request req = {.db = db};
    return status_ok(zova_database_begin_immediate(&req), db, "workspace.begin");
}

static int workspace_registry_commit(zova_database *db) {
    zova_database_simple_request req = {.db = db};
    return status_ok(zova_database_commit(&req), db, "workspace.commit");
}

static void workspace_registry_rollback(zova_database *db) {
    zova_database_simple_request req = {.db = db};
    (void)zova_database_rollback(&req);
}

static int workspace_normalize_root(const char *root_path, char out[ZV_PATH_MAX]) {
    if (!root_path || !root_path[0] ||
        snprintf(out, ZV_PATH_MAX, "%s", root_path) >= ZV_PATH_MAX) {
        return -1;
    }
    cbm_normalize_path_sep(out);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return 0;
}

static int workspace_id_from_root(const char *root, char *out, size_t out_size) {
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(root, strlen(root), digest);
    return snprintf(out, out_size, "w1_%.32s", digest) < (int)out_size ? 0 : -1;
}

static int workspace_bind_text(zova_database *db, zova_statement *stmt, int index,
                               const char *value, const char *phase) {
    zova_statement_bind_text_request req = {
        .statement = stmt,
        .index = index,
        .data = (const uint8_t *)value,
        .len = strlen(value),
    };
    return status_ok(zova_statement_bind_text(&req), db, phase);
}

static int workspace_bind_i64(zova_database *db, zova_statement *stmt, int index, int64_t value,
                              const char *phase) {
    zova_statement_bind_int64_request req = {.statement = stmt, .index = index, .value = value};
    return status_ok(zova_statement_bind_int64(&req), db, phase);
}

static int workspace_step_done(zova_database *db, zova_statement *stmt, const char *phase) {
    zova_step_result result = ZOVA_STEP_DONE;
    zova_statement_step_request req = {.statement = stmt, .out_result = &result};
    return status_ok(zova_statement_step(&req), db, phase) == 0 && result == ZOVA_STEP_DONE ? 0
                                                                                               : -1;
}

static int workspace_query_state(zova_database *db, const char *workspace_id, int64_t generation,
                                 char *out_state, size_t out_state_size) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "SELECT state FROM cbm_workspace_generations "
                          "WHERE workspace_id = ?1 AND generation = ?2",
                          &stmt, "workspace.state_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 1, workspace_id, "workspace.state_bind_id");
    }
    if (rc == 0) {
        rc = workspace_bind_i64(db, stmt, 2, generation, "workspace.state_bind_generation");
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), db, "workspace.state_step");
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_text text = {0};
        zova_statement_column_text_request req = {.statement = stmt, .index = 0, .out_text = &text};
        rc = status_ok(zova_statement_column_text(&req), db, "workspace.state_column");
        if (rc == 0 && (!text.data || text.len + 1 > out_state_size)) {
            rc = -1;
        }
        if (rc == 0) {
            memcpy(out_state, text.data, text.len);
            out_state[text.len] = '\0';
        }
        zova_text_free(&text);
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
        stmt = NULL;
    }
    return rc;
}

int cbm_zova_workspace_get_or_create_at(const char *registry_path, const char *root_path,
                                        char *out_workspace_id, size_t out_workspace_id_size) {
    char root[ZV_PATH_MAX];
    if (!registry_path || !registry_path[0] || !out_workspace_id ||
        out_workspace_id_size < CBM_ZOVA_WORKSPACE_ID_MAX ||
        workspace_normalize_root(root_path, root) != 0) {
        return -1;
    }
    out_workspace_id[0] = '\0';
    cbm_zova_db_t zdb;
    if (open_or_create_zova(registry_path, &zdb) != 0 || workspace_registry_init(zdb.db) != 0 ||
        workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "SELECT workspace_id FROM cbm_workspace_registry WHERE canonical_root = ?1",
                          &stmt, "workspace.lookup_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(zdb.db, stmt, 1, root, "workspace.lookup_bind_root");
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), zdb.db, "workspace.lookup_step");
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_text text = {0};
        zova_statement_column_text_request req = {.statement = stmt, .index = 0, .out_text = &text};
        rc = status_ok(zova_statement_column_text(&req), zdb.db, "workspace.lookup_column");
        if (rc == 0 && text.data && text.len + 1 <= out_workspace_id_size) {
            memcpy(out_workspace_id, text.data, text.len);
            out_workspace_id[text.len] = '\0';
        } else if (rc == 0) {
            rc = -1;
        }
        zova_text_free(&text);
    } else if (rc == 0 && step != ZOVA_STEP_DONE) {
        rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
        stmt = NULL;
    }
    if (rc == 0 && out_workspace_id[0] == '\0') {
        char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
        if (workspace_id_from_root(root, workspace_id, sizeof(workspace_id)) != 0 ||
            prepare_zova(zdb.db,
                         "INSERT INTO cbm_workspace_registry"
                         "(workspace_id,canonical_root,id_format_version,active_generation) "
                         "VALUES(?1,?2,1,0)",
                         &stmt, "workspace.insert_prepare") != 0 ||
            workspace_bind_text(zdb.db, stmt, 1, workspace_id, "workspace.insert_bind_id") != 0 ||
            workspace_bind_text(zdb.db, stmt, 2, root, "workspace.insert_bind_root") != 0 ||
            workspace_step_done(zdb.db, stmt, "workspace.insert_step") != 0) {
            rc = -1;
        } else {
            snprintf(out_workspace_id, out_workspace_id_size, "%s", workspace_id);
        }
        if (stmt) {
            (void)zova_statement_finalize(stmt);
        }
    }
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    }
    if (rc != 0) {
        workspace_registry_rollback(zdb.db);
        out_workspace_id[0] = '\0';
    }
    close_zova(&zdb);
    return rc;
}

int cbm_zova_workspace_next_generation_at(const char *registry_path, const char *workspace_id,
                                           int64_t *out_generation) {
    if (!registry_path || !workspace_id || !workspace_id[0] || !out_generation) {
        return -1;
    }
    *out_generation = 0;
    cbm_zova_db_t zdb;
    if (open_or_create_zova(registry_path, &zdb) != 0 || workspace_registry_init(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "SELECT COALESCE(MAX(generation), 0) + 1 "
                          "FROM cbm_workspace_generations WHERE workspace_id = ?1",
                          &stmt, "workspace.next_generation_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(zdb.db, stmt, 1, workspace_id,
                                 "workspace.next_generation_bind_id");
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), zdb.db, "workspace.next_generation_step");
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_statement_column_int64_request req = {
            .statement = stmt, .index = 0, .out_value = out_generation};
        rc = status_ok(zova_statement_column_int64(&req), zdb.db,
                       "workspace.next_generation_column");
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    close_zova(&zdb);
    return rc == 0 && *out_generation > 0 ? 0 : -1;
}

static int source_sidecar_generation_assign(const char *db_path, int64_t minimum_generation,
                                            int64_t *out_generation) {
    if (!db_path || !db_path[0] || !out_generation) {
        return -1;
    }
    *out_generation = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool in_transaction = false;
    int rc = -1;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) !=
        SQLITE_OK) {
        goto done;
    }
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        goto done;
    }
    in_transaction = true;
    if (sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS " CBM_ZOVA_SIDECAR_GENERATION_TABLE
                     "(id INTEGER PRIMARY KEY CHECK(id = 1), generation INTEGER NOT NULL)",
                     NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_exec(db, "INSERT OR IGNORE INTO " CBM_ZOVA_SIDECAR_GENERATION_TABLE
                         "(id,generation) VALUES(1,0)",
                     NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT generation FROM " CBM_ZOVA_SIDECAR_GENERATION_TABLE
                               " WHERE id = 1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        goto done;
    }
    int64_t current_generation = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;
    int64_t next_generation = current_generation + 1;
    if (next_generation < minimum_generation) {
        next_generation = minimum_generation;
    }
    if (next_generation <= 0 ||
        sqlite3_prepare_v2(db, "UPDATE " CBM_ZOVA_SIDECAR_GENERATION_TABLE
                               " SET generation = ?1 WHERE id = 1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, next_generation) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) {
        goto done;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        goto done;
    }
    in_transaction = false;
    *out_generation = next_generation;
    rc = 0;
done:
    if (stmt) {
        sqlite3_finalize(stmt);
    }
    if (in_transaction) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    if (db) {
        sqlite3_close(db);
    }
    return rc;
}

static int sidecar_generation_read_db(zova_database *db, int64_t *out_generation) {
    if (!db || !out_generation) {
        return -1;
    }
    *out_generation = 0;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, "SELECT generation FROM " CBM_ZOVA_SIDECAR_GENERATION_TABLE
                              " WHERE id = 1",
                          &stmt, "sidecar_generation.prepare");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), db, "sidecar_generation.step");
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_statement_column_int64_request req = {
            .statement = stmt, .index = 0, .out_value = out_generation};
        rc = status_ok(zova_statement_column_int64(&req), db, "sidecar_generation.column");
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc == 0 && *out_generation > 0 ? 0 : -1;
}

int cbm_zova_sidecar_generation_get(const char *zova_path, int64_t *out_generation) {
    if (!zova_path || !zova_path[0] || !out_generation) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, true, &zdb) != 0) {
        return -1;
    }
    int rc = sidecar_generation_read_db(zdb.db, out_generation);
    close_zova(&zdb);
    return rc;
}

/* Test-only fault points model cancellation or process loss after a durable
 * source-db generation has been allocated. Unset in normal operation. */
static bool sidecar_test_fault(const char *phase) {
    const char *requested = getenv("CBM_ZOVA_TEST_FAIL_PHASE");
    return requested && phase && strcmp(requested, phase) == 0;
}

int cbm_zova_workspace_lookup_at(const char *registry_path, const char *root_path,
                                 char *out_workspace_id, size_t out_workspace_id_size) {
    char root[ZV_PATH_MAX];
    if (!registry_path || !registry_path[0] || !out_workspace_id ||
        out_workspace_id_size < CBM_ZOVA_WORKSPACE_ID_MAX ||
        workspace_normalize_root(root_path, root) != 0 || !cbm_file_exists(registry_path)) {
        return -1;
    }
    out_workspace_id[0] = '\0';
    cbm_zova_db_t zdb;
    if (open_zova(registry_path, true, &zdb) != 0) {
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "SELECT workspace_id FROM cbm_workspace_registry "
                          "WHERE canonical_root = ?1",
                          &stmt, "workspace.lookup_read_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(zdb.db, stmt, 1, root, "workspace.lookup_read_bind_root");
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), zdb.db, "workspace.lookup_read_step");
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_text text = {0};
        zova_statement_column_text_request req = {.statement = stmt, .index = 0, .out_text = &text};
        rc = status_ok(zova_statement_column_text(&req), zdb.db, "workspace.lookup_read_column");
        if (rc == 0 && text.data && text.len + 1 <= out_workspace_id_size) {
            memcpy(out_workspace_id, text.data, text.len);
            out_workspace_id[text.len] = '\0';
        } else if (rc == 0) {
            rc = -1;
        }
        zova_text_free(&text);
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    close_zova(&zdb);
    return rc;
}

int cbm_zova_workspace_generation_begin_at(const char *registry_path, const char *workspace_id,
                                            int64_t generation) {
    if (!registry_path || !workspace_id || !workspace_id[0] || generation <= 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_or_create_zova(registry_path, &zdb) != 0 || workspace_registry_init(zdb.db) != 0 ||
        workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "INSERT INTO cbm_workspace_generations(workspace_id,generation,state) "
                          "VALUES(?1,?2,'building')",
                          &stmt, "workspace.begin_prepare");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 1, workspace_id, "workspace.begin_bind_id");
    if (rc == 0)
        rc = workspace_bind_i64(zdb.db, stmt, 2, generation, "workspace.begin_bind_generation");
    if (rc == 0)
        rc = workspace_step_done(zdb.db, stmt, "workspace.begin_step");
    if (stmt)
        (void)zova_statement_finalize(stmt);
    if (rc == 0)
        rc = workspace_registry_commit(zdb.db);
    if (rc != 0)
        workspace_registry_rollback(zdb.db);
    close_zova(&zdb);
    return rc;
}

int cbm_zova_workspace_generation_finish_at(const char *registry_path, const char *workspace_id,
                                             int64_t generation, bool ready) {
    if (!registry_path || !workspace_id || !workspace_id[0] || generation <= 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_or_create_zova(registry_path, &zdb) != 0 || workspace_registry_init(zdb.db) != 0 ||
        workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    char state[16] = {0};
    int rc = workspace_query_state(zdb.db, workspace_id, generation, state, sizeof(state));
    zova_statement *stmt = NULL;
    if (rc == 0 && strcmp(state, "building") != 0) {
        rc = -1;
    }
    if (rc == 0) {
        rc = prepare_zova(zdb.db,
                          "UPDATE cbm_workspace_generations SET state = ?1 "
                          "WHERE workspace_id = ?2 AND generation = ?3",
                          &stmt, "workspace.finish_prepare");
    }
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 1, ready ? "ready" : "failed",
                                 "workspace.finish_bind_state");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 2, workspace_id, "workspace.finish_bind_id");
    if (rc == 0)
        rc = workspace_bind_i64(zdb.db, stmt, 3, generation, "workspace.finish_bind_generation");
    if (rc == 0)
        rc = workspace_step_done(zdb.db, stmt, "workspace.finish_step");
    if (stmt)
        (void)zova_statement_finalize(stmt);
    stmt = NULL;
    if (rc == 0 && ready) {
        rc = prepare_zova(zdb.db,
                          "UPDATE cbm_workspace_registry SET active_generation = ?1 "
                          "WHERE workspace_id = ?2",
                          &stmt, "workspace.active_prepare");
    }
    if (rc == 0 && ready)
        rc = workspace_bind_i64(zdb.db, stmt, 1, generation, "workspace.active_bind_generation");
    if (rc == 0 && ready)
        rc = workspace_bind_text(zdb.db, stmt, 2, workspace_id, "workspace.active_bind_id");
    if (rc == 0 && ready)
        rc = workspace_step_done(zdb.db, stmt, "workspace.active_step");
    if (stmt)
        (void)zova_statement_finalize(stmt);
    if (rc == 0)
        rc = workspace_registry_commit(zdb.db);
    if (rc != 0)
        workspace_registry_rollback(zdb.db);
    close_zova(&zdb);
    return rc;
}

int cbm_zova_workspace_active_generation_at(const char *registry_path, const char *workspace_id,
                                             int64_t *out_generation) {
    if (!registry_path || !workspace_id || !workspace_id[0] || !out_generation) {
        return -1;
    }
    *out_generation = 0;
    cbm_zova_db_t zdb;
    if (open_or_create_zova(registry_path, &zdb) != 0 || workspace_registry_init(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id = ?1",
                          &stmt, "workspace.active_get_prepare");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 1, workspace_id, "workspace.active_get_bind_id");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), zdb.db, "workspace.active_get_step");
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_statement_column_int64_request req = {
            .statement = stmt, .index = 0, .out_value = out_generation};
        rc = status_ok(zova_statement_column_int64(&req), zdb.db, "workspace.active_get_column");
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt)
        (void)zova_statement_finalize(stmt);
    close_zova(&zdb);
    return rc;
}

static bool i8_nonzero(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (((const int8_t *)data)[i] != 0) {
            return true;
        }
    }
    return false;
}

int cbm_zova_validate_container(const char *zova_path) {
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, true, &zdb) != 0) {
        return -1;
    }

    int rc = 0;
    const char *queries[] = {
        "SELECT count(*) FROM projects;",
        "SELECT url_path_gen, local_name_gen FROM edges LIMIT 0;",
        "SELECT name FROM sqlite_master WHERE name = 'nodes_fts';",
        "SELECT bm25(nodes_fts) FROM nodes_fts LIMIT 1;",
        NULL,
    };
    for (int i = 0; queries[i]; i++) {
        zova_statement *stmt = NULL;
        if (prepare_zova(zdb.db, queries[i], &stmt, "validate.prepare") != 0) {
            rc = -1;
            break;
        }
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, zdb.db, "validate.step");
        }
        (void)zova_statement_finalize(stmt);
        if (rc != 0) {
            break;
        }
    }

    close_zova(&zdb);
    return rc;
}

static int put_direct_vector_batch(zova_database *db, const char *collection,
                                   zova_vector_input *inputs, int *count) {
    if (*count == 0) {
        return 0;
    }
    zova_vector_put_many_request req = {
        .db = db,
        .collection_name = collection,
        .vectors = inputs,
        .vectors_len = (size_t)*count,
    };
    int rc = status_ok(zova_vector_put_many(&req), db, "vector.put_many_direct_i8");
    *count = 0;
    return rc;
}

static int collection_create_i8(zova_database *db, const char *name, int vector_dim);
static int vector_meta_refresh(zova_database *db, int vector_dim);

static int put_direct_node_vectors(zova_database *db, const char *collection,
                                   const CBMDumpVector *vectors, int count, int vector_dim) {
    zova_vector_input inputs[ZV_BATCH] = {0};
    char ids[ZV_BATCH][ZV_ID_MAX] = {{0}};
    int batch = 0;
    for (int i = 0; i < count; i++) {
        if (vectors[i].vector_len != vector_dim ||
            !i8_nonzero(vectors[i].vector, (size_t)vectors[i].vector_len)) {
            continue;
        }
        snprintf(ids[batch], sizeof(ids[batch]), "%lld", (long long)vectors[i].node_id);
        inputs[batch] = (zova_vector_input){
            .id = ids[batch],
            .values =
                {
                    .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = (const int8_t *)vectors[i].vector,
                    .values_len = (size_t)vectors[i].vector_len,
                },
        };
        batch++;
        if (batch == ZV_BATCH) {
            int rc = put_direct_vector_batch(db, collection, inputs, &batch);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return put_direct_vector_batch(db, collection, inputs, &batch);
}

static int put_direct_token_vectors(zova_database *db, const char *collection,
                                    const CBMDumpTokenVec *vectors, int count, int vector_dim) {
    zova_vector_input inputs[ZV_BATCH] = {0};
    char ids[ZV_BATCH][ZV_ID_MAX] = {{0}};
    int batch = 0;
    for (int i = 0; i < count; i++) {
        if (vectors[i].vector_len != vector_dim ||
            !i8_nonzero(vectors[i].vector, (size_t)vectors[i].vector_len)) {
            continue;
        }
        snprintf(ids[batch], sizeof(ids[batch]), "%lld", (long long)vectors[i].id);
        inputs[batch] = (zova_vector_input){
            .id = ids[batch],
            .values =
                {
                    .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = (const int8_t *)vectors[i].vector,
                    .values_len = (size_t)vectors[i].vector_len,
                },
        };
        batch++;
        if (batch == ZV_BATCH) {
            int rc = put_direct_vector_batch(db, collection, inputs, &batch);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return put_direct_vector_batch(db, collection, inputs, &batch);
}

int cbm_zova_write_i8_vectors_direct(const char *zova_path,
                                     const CBMDumpVector *node_vectors, int node_vector_count,
                                     const CBMDumpTokenVec *token_vectors, int token_vector_count,
                                     int vector_dim) {
    if (!zova_path || vector_dim <= 0 || node_vector_count < 0 || token_vector_count < 0 ||
        (node_vector_count > 0 && !node_vectors) ||
        (token_vector_count > 0 && !token_vectors)) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0) {
        return -1;
    }
    int rc = collection_create_i8(zdb.db, CBM_ZOVA_NODE_COLLECTION, vector_dim);
    if (rc == 0) {
        rc = collection_create_i8(zdb.db, CBM_ZOVA_TOKEN_COLLECTION, vector_dim);
    }
    if (rc == 0) {
        rc = put_direct_node_vectors(zdb.db, CBM_ZOVA_NODE_COLLECTION, node_vectors,
                                     node_vector_count, vector_dim);
    }
    if (rc == 0) {
        rc = put_direct_token_vectors(zdb.db, CBM_ZOVA_TOKEN_COLLECTION, token_vectors,
                                      token_vector_count, vector_dim);
    }
    if (rc == 0) {
        rc = vector_meta_refresh(zdb.db, vector_dim);
    }
    close_zova(&zdb);
    return rc;
}

static int collection_delete_if_exists(zova_database *db, const char *name) {
    uint8_t exists = 0;
    zova_vector_collection_exists_request ereq = {
        .db = db,
        .name = name,
        .out_exists = &exists,
    };
    if (status_ok(zova_vector_collection_exists(&ereq), db, "vector.exists") != 0) {
        return -1;
    }
    if (!exists) {
        return 0;
    }
    zova_vector_collection_delete_request dreq = {.db = db, .name = name};
    return status_ok(zova_vector_collection_delete(&dreq), db, "vector.delete_collection");
}

static int collection_create_i8(zova_database *db, const char *name, int vector_dim) {
    if (collection_delete_if_exists(db, name) != 0) {
        return -1;
    }
    zova_vector_collection_create_request req = {
        .db = db,
        .name = name,
        .options =
            {
                .dimensions = (uint32_t)vector_dim,
                .metric = ZOVA_VECTOR_METRIC_COSINE,
                .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
            },
    };
    return status_ok(zova_vector_collection_create(&req), db, "vector.create_collection");
}

static int workspace_node_vectors_valid(const CBMDumpVector *vectors, int count, int vector_dim) {
    if (count < 0 || (count > 0 && !vectors) || vector_dim <= 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (vectors[i].node_id <= 0 || !vectors[i].vector || vectors[i].vector_len != vector_dim) {
            return -1;
        }
    }
    return 0;
}

int cbm_zova_write_workspace_node_i8_vectors_direct(
    const char *zova_path, const char *workspace_id, const char *model_fingerprint,
    const CBMDumpVector *node_vectors, int node_vector_count, int vector_dim) {
    char collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (!zova_path ||
        cbm_zova_workspace_node_vector_collection_name(workspace_id, model_fingerprint, vector_dim,
                                                        collection, sizeof(collection)) != 0 ||
        workspace_node_vectors_valid(node_vectors, node_vector_count, vector_dim) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    int rc = collection_create_i8(zdb.db, collection, vector_dim);
    if (rc == 0) {
        rc = put_direct_node_vectors(zdb.db, collection, node_vectors, node_vector_count,
                                     vector_dim);
    }
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    } else {
        workspace_registry_rollback(zdb.db);
    }
    close_zova(&zdb);
    return rc;
}

int cbm_zova_delete_workspace_node_i8_vectors(const char *zova_path, const char *workspace_id,
                                               const char *model_fingerprint, int vector_dim) {
    char collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (!zova_path ||
        cbm_zova_workspace_node_vector_collection_name(workspace_id, model_fingerprint, vector_dim,
                                                        collection, sizeof(collection)) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    int rc = collection_delete_if_exists(zdb.db, collection);
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    } else {
        workspace_registry_rollback(zdb.db);
    }
    close_zova(&zdb);
    return rc;
}

static int workspace_token_vectors_valid(const CBMDumpTokenVec *vectors, int count,
                                         int vector_dim) {
    if (count < 0 || (count > 0 && !vectors) || vector_dim <= 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (vectors[i].id <= 0 || !vectors[i].token || !vectors[i].vector ||
            vectors[i].vector_len != vector_dim) {
            return -1;
        }
    }
    return 0;
}

int cbm_zova_write_workspace_token_i8_vectors_direct(
    const char *zova_path, const char *workspace_id, const char *model_fingerprint,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim) {
    char collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (!zova_path ||
        cbm_zova_workspace_token_vector_collection_name(workspace_id, model_fingerprint,
                                                         vector_dim, collection,
                                                         sizeof(collection)) != 0 ||
        workspace_token_vectors_valid(token_vectors, token_vector_count, vector_dim) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    int rc = collection_create_i8(zdb.db, collection, vector_dim);
    if (rc == 0) {
        rc = put_direct_token_vectors(zdb.db, collection, token_vectors, token_vector_count,
                                      vector_dim);
    }
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    } else {
        workspace_registry_rollback(zdb.db);
    }
    close_zova(&zdb);
    return rc;
}

int cbm_zova_delete_workspace_token_i8_vectors(const char *zova_path, const char *workspace_id,
                                                const char *model_fingerprint, int vector_dim) {
    char collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (!zova_path ||
        cbm_zova_workspace_token_vector_collection_name(workspace_id, model_fingerprint,
                                                         vector_dim, collection,
                                                         sizeof(collection)) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    int rc = collection_delete_if_exists(zdb.db, collection);
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    } else {
        workspace_registry_rollback(zdb.db);
    }
    close_zova(&zdb);
    return rc;
}

static int flush_vector_batch(zova_database *db, const char *collection,
                              zova_vector_input *inputs, zova_buffer *buffers, int *count) {
    if (*count == 0) {
        return 0;
    }
    zova_vector_put_many_request req = {
        .db = db,
        .collection_name = collection,
        .vectors = inputs,
        .vectors_len = (size_t)*count,
    };
    int rc = status_ok(zova_vector_put_many(&req), db, "vector.put_many_i8");
    for (int i = 0; i < *count; i++) {
        zova_buffer_free(&buffers[i]);
    }
    *count = 0;
    return rc;
}

static int mirror_vector_table(zova_database *db, const char *collection, const char *sql,
                               int vector_dim) {
    zova_statement *stmt = NULL;
    if (prepare_zova(db, sql, &stmt, "vector.scan_prepare") != 0) {
        return -1;
    }
    zova_statement_bind_int64_request breq = {
        .statement = stmt,
        .index = 1,
        .value = vector_dim,
    };
    if (status_ok(zova_statement_bind_int64(&breq), db, "vector.scan_bind") != 0) {
        (void)zova_statement_finalize(stmt);
        return -1;
    }

    zova_vector_input inputs[ZV_BATCH];
    zova_buffer buffers[ZV_BATCH];
    char ids[ZV_BATCH][ZV_ID_MAX];
    memset(inputs, 0, sizeof(inputs));
    memset(buffers, 0, sizeof(buffers));
    int batch = 0;
    int rc = 0;

    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, db, "vector.scan_step");
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }

        int64_t id = 0;
        zova_statement_column_int64_request ireq = {
            .statement = stmt,
            .index = 0,
            .out_value = &id,
        };
        zova_buffer blob = {0};
        zova_statement_column_blob_request vreq = {
            .statement = stmt,
            .index = 1,
            .out_buffer = &blob,
        };
        if (status_ok(zova_statement_column_int64(&ireq), db, "vector.id") != 0 ||
            status_ok(zova_statement_column_blob(&vreq), db, "vector.blob") != 0) {
            zova_buffer_free(&blob);
            rc = -1;
            break;
        }
        if (blob.len != (size_t)vector_dim || !i8_nonzero(blob.data, blob.len)) {
            zova_buffer_free(&blob);
            continue;
        }

        snprintf(ids[batch], sizeof(ids[batch]), "%lld", (long long)id);
        buffers[batch] = blob;
        inputs[batch].id = ids[batch];
        inputs[batch].values.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8;
        inputs[batch].values.f32_values = NULL;
        inputs[batch].values.f16_values = NULL;
        inputs[batch].values.i8_values = (const int8_t *)blob.data;
        inputs[batch].values.values_len = blob.len;
        batch++;
        if (batch == ZV_BATCH) {
            rc = flush_vector_batch(db, collection, inputs, buffers, &batch);
            if (rc != 0) {
                break;
            }
        }
    }

    if (rc == 0) {
        rc = flush_vector_batch(db, collection, inputs, buffers, &batch);
    } else {
        for (int i = 0; i < batch; i++) {
            zova_buffer_free(&buffers[i]);
        }
    }
    (void)zova_statement_finalize(stmt);
    return rc;
}

static int vector_meta_refresh(zova_database *db, int vector_dim);
static int vector_meta_try_get(zova_database *db, const char *project, int vector_dim,
                               bool *out_found, bool *out_full_scan_safe);

int cbm_zova_mirror_i8_vectors(const char *zova_path, int vector_dim) {
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0) {
        return -1;
    }
    int rc = collection_create_i8(zdb.db, CBM_ZOVA_NODE_COLLECTION, vector_dim);
    if (rc == 0) {
        rc = collection_create_i8(zdb.db, CBM_ZOVA_TOKEN_COLLECTION, vector_dim);
    }
    if (rc == 0) {
        rc = mirror_vector_table(
            zdb.db, CBM_ZOVA_NODE_COLLECTION,
            "SELECT node_id, vector FROM node_vectors WHERE length(vector) = ?1", vector_dim);
    }
    if (rc == 0) {
        rc = vector_meta_refresh(zdb.db, vector_dim);
    }
    if (rc == 0) {
        rc = mirror_vector_table(
            zdb.db, CBM_ZOVA_TOKEN_COLLECTION,
            "SELECT id, vector FROM token_vectors WHERE length(vector) = ?1", vector_dim);
    }
    close_zova(&zdb);
    return rc;
}

static int graph_delete_if_exists(zova_database *db) {
    uint8_t exists = 0;
    zova_graph_exists_request ereq = {
        .db = db,
        .name = CBM_ZOVA_CODE_GRAPH,
        .out_exists = &exists,
    };
    if (status_ok(zova_graph_exists(&ereq), db, "graph.exists") != 0) {
        return -1;
    }
    if (!exists) {
        return 0;
    }
    zova_graph_delete_request dreq = {.db = db, .name = CBM_ZOVA_CODE_GRAPH};
    return status_ok(zova_graph_delete(&dreq), db, "graph.delete");
}

int cbm_zova_mirror_graph(const char *zova_path) {
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0) {
        return -1;
    }
    int rc = graph_delete_if_exists(zdb.db);
    if (rc == 0) {
        zova_graph_create_request creq = {.db = zdb.db, .name = CBM_ZOVA_CODE_GRAPH};
        rc = status_ok(zova_graph_create(&creq), zdb.db, "graph.create");
    }

    zova_statement *stmt = NULL;
    if (rc == 0) {
        rc = prepare_zova(zdb.db, "SELECT id, label FROM nodes", &stmt, "graph.nodes_prepare");
    }
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, zdb.db, "graph.nodes_step");
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        int64_t id = 0;
        zova_statement_column_int64_request ireq = {.statement = stmt, .index = 0, .out_value = &id};
        zova_text label = {0};
        zova_statement_column_text_request lreq = {.statement = stmt, .index = 1, .out_text = &label};
        if (status_ok(zova_statement_column_int64(&ireq), zdb.db, "graph.node_id") != 0 ||
            status_ok(zova_statement_column_text(&lreq), zdb.db, "graph.node_label") != 0) {
            zova_text_free(&label);
            rc = -1;
            break;
        }
        char id_buf[ZV_ID_MAX];
        snprintf(id_buf, sizeof(id_buf), "%lld", (long long)id);
        char *kind = zv_strndup(label.data ? label.data : "", label.data ? label.len : 0);
        if (!kind) {
            zova_text_free(&label);
            rc = -1;
            break;
        }
        zova_graph_node_put_request nreq = {
            .db = zdb.db,
            .graph_name = CBM_ZOVA_CODE_GRAPH,
            .node_id = id_buf,
            .kind = kind,
            .target_type = ZOVA_GRAPH_TARGET_RECORD,
            .target_namespace = "nodes",
            .target_ref = id_buf,
        };
        rc = status_ok(zova_graph_node_put(&nreq), zdb.db, "graph.node_put");
        free(kind);
        zova_text_free(&label);
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
        stmt = NULL;
    }

    if (rc == 0) {
        rc = prepare_zova(zdb.db, "SELECT source_id, type, target_id FROM edges", &stmt,
                          "graph.edges_prepare");
    }
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, zdb.db, "graph.edges_step");
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        int64_t source = 0;
        int64_t target = 0;
        zova_text type = {0};
        zova_statement_column_int64_request s_id = {.statement = stmt, .index = 0, .out_value = &source};
        zova_statement_column_text_request t_req = {.statement = stmt, .index = 1, .out_text = &type};
        zova_statement_column_int64_request t_id = {.statement = stmt, .index = 2, .out_value = &target};
        if (status_ok(zova_statement_column_int64(&s_id), zdb.db, "graph.edge_source") != 0 ||
            status_ok(zova_statement_column_text(&t_req), zdb.db, "graph.edge_type") != 0 ||
            status_ok(zova_statement_column_int64(&t_id), zdb.db, "graph.edge_target") != 0) {
            zova_text_free(&type);
            rc = -1;
            break;
        }
        char source_buf[ZV_ID_MAX];
        char target_buf[ZV_ID_MAX];
        snprintf(source_buf, sizeof(source_buf), "%lld", (long long)source);
        snprintf(target_buf, sizeof(target_buf), "%lld", (long long)target);
        char *edge_type = zv_strndup(type.data ? type.data : "", type.data ? type.len : 0);
        if (!edge_type) {
            zova_text_free(&type);
            rc = -1;
            break;
        }
        zova_graph_edge_put_request ereq = {
            .db = zdb.db,
            .graph_name = CBM_ZOVA_CODE_GRAPH,
            .from_node_id = source_buf,
            .edge_type = edge_type,
            .to_node_id = target_buf,
        };
        rc = status_ok(zova_graph_edge_put(&ereq), zdb.db, "graph.edge_put");
        free(edge_type);
        zova_text_free(&type);
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    close_zova(&zdb);
    return rc;
}

typedef struct {
    int64_t sqlite_id;
    char *stable_id;
} workspace_graph_node_map_t;

typedef struct {
    zova_graph_node_input inputs[ZV_BATCH];
    char *kinds[ZV_BATCH];
    char *target_refs[ZV_BATCH];
    int count;
} workspace_graph_node_batch_t;

typedef struct {
    zova_graph_edge_input inputs[ZV_BATCH];
    char *types[ZV_BATCH];
    int count;
} workspace_graph_edge_batch_t;

static int graph_delete_named_if_exists(zova_database *db, const char *graph_name) {
    uint8_t exists = 0;
    zova_graph_exists_request ereq = {
        .db = db,
        .name = graph_name,
        .out_exists = &exists,
    };
    if (status_ok(zova_graph_exists(&ereq), db, "workspace_graph.exists") != 0) {
        return -1;
    }
    if (!exists) {
        return 0;
    }
    zova_graph_delete_request dreq = {.db = db, .name = graph_name};
    return status_ok(zova_graph_delete(&dreq), db, "workspace_graph.delete");
}

static void workspace_graph_node_map_free(workspace_graph_node_map_t *items, size_t count) {
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(items[i].stable_id);
    }
    free(items);
}

static const char *workspace_graph_node_map_find(const workspace_graph_node_map_t *items,
                                                  size_t count, int64_t sqlite_id) {
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (items[mid].sqlite_id == sqlite_id) {
            return items[mid].stable_id;
        }
        if (items[mid].sqlite_id < sqlite_id) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return NULL;
}

static int workspace_graph_column_text_copy(zova_database *db, zova_statement *stmt, int index,
                                            char **out, const char *phase) {
    *out = NULL;
    zova_text text = {0};
    zova_statement_column_text_request req = {
        .statement = stmt,
        .index = index,
        .out_text = &text,
    };
    int rc = status_ok(zova_statement_column_text(&req), db, phase);
    if (rc == 0) {
        *out = zv_strndup(text.data ? text.data : "", text.data ? text.len : 0);
        if (!*out) {
            rc = -1;
        }
    }
    zova_text_free(&text);
    return rc;
}

static void workspace_graph_node_batch_clear(workspace_graph_node_batch_t *batch) {
    for (int i = 0; i < batch->count; i++) {
        free(batch->kinds[i]);
        free(batch->target_refs[i]);
    }
    batch->count = 0;
}

static int workspace_graph_flush_node_batch(zova_database *db,
                                            workspace_graph_node_batch_t *batch) {
    if (batch->count == 0) {
        return 0;
    }
    zova_graph_node_put_many_request req = {
        .db = db,
        .nodes = batch->inputs,
        .nodes_len = (size_t)batch->count,
    };
    int rc = status_ok(zova_graph_node_put_many(&req), db, "workspace_graph.node_put_many");
    workspace_graph_node_batch_clear(batch);
    return rc;
}

static void workspace_graph_edge_batch_clear(workspace_graph_edge_batch_t *batch) {
    for (int i = 0; i < batch->count; i++) {
        free(batch->types[i]);
    }
    batch->count = 0;
}

static int workspace_graph_flush_edge_batch(zova_database *db,
                                            workspace_graph_edge_batch_t *batch) {
    if (batch->count == 0) {
        return 0;
    }
    zova_graph_edge_put_many_request req = {
        .db = db,
        .edges = batch->inputs,
        .edges_len = (size_t)batch->count,
    };
    int rc = status_ok(zova_graph_edge_put_many(&req), db, "workspace_graph.edge_put_many");
    workspace_graph_edge_batch_clear(batch);
    return rc;
}

static int workspace_trace_projection_init(zova_database *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS cbm_zova_trace_nodes_v1("
        "workspace_id TEXT NOT NULL, node_id TEXT NOT NULL, project TEXT NOT NULL, "
        "name TEXT NOT NULL, qualified_name TEXT NOT NULL, file_path TEXT NOT NULL, "
        "label TEXT NOT NULL, start_line INTEGER NOT NULL, end_line INTEGER NOT NULL, "
        "PRIMARY KEY(workspace_id,node_id)) WITHOUT ROWID;";
    zova_database_exec_request req = {.db = db, .sql = sql};
    return status_ok(zova_database_exec(&req), db, "workspace_trace.init");
}

static int workspace_trace_projection_clear(zova_database *db, const char *workspace_id) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, "DELETE FROM cbm_zova_trace_nodes_v1 WHERE workspace_id = ?1", &stmt,
                          "workspace_trace.clear_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 1, workspace_id, "workspace_trace.clear_bind");
    }
    if (rc == 0) {
        rc = workspace_step_done(db, stmt, "workspace_trace.clear_step");
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int workspace_trace_projection_write(zova_database *db, zova_statement *stmt,
                                            const char *workspace_id, const char *node_id,
                                            const char *project, const char *name,
                                            const char *qualified_name, const char *file_path,
                                            const char *label, int64_t start_line, int64_t end_line) {
    int rc = workspace_bind_text(db, stmt, 1, workspace_id, "workspace_trace.bind_workspace");
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 2, node_id, "workspace_trace.bind_node");
    }
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 3, project, "workspace_trace.bind_project");
    }
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 4, name, "workspace_trace.bind_name");
    }
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 5, qualified_name, "workspace_trace.bind_qn");
    }
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 6, file_path, "workspace_trace.bind_file");
    }
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 7, label, "workspace_trace.bind_label");
    }
    if (rc == 0) {
        rc = workspace_bind_i64(db, stmt, 8, start_line, "workspace_trace.bind_start");
    }
    if (rc == 0) {
        rc = workspace_bind_i64(db, stmt, 9, end_line, "workspace_trace.bind_end");
    }
    if (rc == 0) {
        rc = workspace_step_done(db, stmt, "workspace_trace.insert_step");
    }
    if (zova_statement_reset(stmt) != ZOVA_OK || zova_statement_clear_bindings(stmt) != ZOVA_OK) {
        rc = -1;
    }
    return rc;
}

static int workspace_graph_mirror_nodes(zova_database *db, const char *graph_name,
                                        const char *workspace_id, const char *project,
                                        workspace_graph_node_map_t **out_items,
                                        size_t *out_count) {
    *out_items = NULL;
    *out_count = 0;
    zova_statement *stmt = NULL;
    workspace_graph_node_map_t *items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    workspace_graph_node_batch_t batch = {0};
    zova_statement *projection_stmt = NULL;
    int rc = prepare_zova(db,
                          "SELECT id,label,name,file_path,qualified_name,start_line,end_line "
                          "FROM nodes WHERE project = ?1 ORDER BY id",
                          &stmt, "workspace_graph.nodes_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 1, project, "workspace_graph.nodes_bind_project");
    }
    if (rc == 0) {
        rc = prepare_zova(
            db,
            "INSERT INTO cbm_zova_trace_nodes_v1("
            "workspace_id,node_id,project,name,qualified_name,file_path,label,start_line,end_line) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)",
            &projection_stmt, "workspace_trace.insert_prepare");
    }
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        if (status_ok(zova_statement_step(&step_req), db, "workspace_graph.nodes_step") != 0) {
            rc = -1;
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        int64_t sqlite_id = 0;
        int64_t start_line = 0;
        int64_t end_line = 0;
        char *kind = NULL;
        char *name = NULL;
        char *file_path = NULL;
        char *qualified_name = NULL;
        zova_statement_column_int64_request id_req = {
            .statement = stmt, .index = 0, .out_value = &sqlite_id};
        zova_statement_column_int64_request start_req = {
            .statement = stmt, .index = 5, .out_value = &start_line};
        zova_statement_column_int64_request end_req = {
            .statement = stmt, .index = 6, .out_value = &end_line};
        if (status_ok(zova_statement_column_int64(&id_req), db, "workspace_graph.node_id") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 1, &kind, "workspace_graph.node_kind") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 2, &name, "workspace_graph.node_name") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 3, &file_path,
                                             "workspace_graph.node_file") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 4, &qualified_name,
                                             "workspace_graph.node_qualified_name") != 0 ||
            status_ok(zova_statement_column_int64(&start_req), db,
                      "workspace_graph.node_start") != 0 ||
            status_ok(zova_statement_column_int64(&end_req), db, "workspace_graph.node_end") != 0) {
            free(kind);
            free(name);
            free(file_path);
            free(qualified_name);
            rc = -1;
            break;
        }
        char discriminator[64];
        if (qualified_name[0]) {
            snprintf(discriminator, sizeof(discriminator), "named");
        } else if (snprintf(discriminator, sizeof(discriminator), "anon:%lld:%lld",
                            (long long)start_line, (long long)end_line) >=
                   (int)sizeof(discriminator)) {
            free(kind);
            free(name);
            free(file_path);
            free(qualified_name);
            rc = -1;
            break;
        }
        char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (cbm_zova_workspace_node_id_v1(workspace_id, kind, file_path, qualified_name,
                                          discriminator, stable_id, sizeof(stable_id)) != 0) {
            free(kind);
            free(name);
            free(file_path);
            free(qualified_name);
            rc = -1;
            break;
        }
        char *stable_id_copy = zv_strndup(stable_id, strlen(stable_id));
        /* Keep the topology target reference stable too: SQLite ids are only
         * an in-memory translation aid during this mirror, never persisted in
         * the scoped graph. */
        char *target_ref = zv_strndup(stable_id, strlen(stable_id));
        if (!stable_id_copy || !target_ref) {
            free(stable_id_copy);
            free(target_ref);
            free(kind);
            free(name);
            free(file_path);
            free(qualified_name);
            rc = -1;
            break;
        }
        rc = workspace_trace_projection_write(db, projection_stmt, workspace_id, stable_id_copy,
                                              project, name, qualified_name, file_path, kind,
                                              start_line, end_line);
        free(name);
        free(file_path);
        free(qualified_name);
        if (rc != 0) {
            free(stable_id_copy);
            free(target_ref);
            free(kind);
            break;
        }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 128;
            workspace_graph_node_map_t *grown = realloc(items, next * sizeof(*items));
            if (!grown) {
                free(stable_id_copy);
                free(target_ref);
                free(kind);
                rc = -1;
                break;
            }
            items = grown;
            capacity = next;
        }
        items[count++] = (workspace_graph_node_map_t){.sqlite_id = sqlite_id,
                                                       .stable_id = stable_id_copy};
        batch.kinds[batch.count] = kind;
        batch.target_refs[batch.count] = target_ref;
        batch.inputs[batch.count] = (zova_graph_node_input){
            .graph_name = graph_name,
            .node_id = stable_id_copy,
            .kind = kind,
            .target_type = ZOVA_GRAPH_TARGET_RECORD,
            .target_namespace = "cbm_nodes",
            .target_ref = target_ref,
        };
        batch.count++;
        if (batch.count == ZV_BATCH) {
            rc = workspace_graph_flush_node_batch(db, &batch);
        }
    }
    if (rc == 0) {
        rc = workspace_graph_flush_node_batch(db, &batch);
    }
    workspace_graph_node_batch_clear(&batch);
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    if (projection_stmt) {
        (void)zova_statement_finalize(projection_stmt);
    }
    if (rc != 0) {
        workspace_graph_node_map_free(items, count);
        return -1;
    }
    *out_items = items;
    *out_count = count;
    return 0;
}

static int workspace_graph_mirror_edges(zova_database *db, const char *graph_name,
                                        const char *project,
                                        const workspace_graph_node_map_t *nodes,
                                        size_t node_count) {
    zova_statement *stmt = NULL;
    workspace_graph_edge_batch_t batch = {0};
    int rc = prepare_zova(db,
                          "SELECT source_id,type,target_id FROM edges WHERE project = ?1 ORDER BY id",
                          &stmt, "workspace_graph.edges_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 1, project, "workspace_graph.edges_bind_project");
    }
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        if (status_ok(zova_statement_step(&step_req), db, "workspace_graph.edges_step") != 0) {
            rc = -1;
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        int64_t source_id = 0;
        int64_t target_id = 0;
        char *edge_type = NULL;
        zova_statement_column_int64_request source_req = {
            .statement = stmt, .index = 0, .out_value = &source_id};
        zova_statement_column_int64_request target_req = {
            .statement = stmt, .index = 2, .out_value = &target_id};
        if (status_ok(zova_statement_column_int64(&source_req), db,
                      "workspace_graph.edge_source") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 1, &edge_type,
                                             "workspace_graph.edge_type") != 0 ||
            status_ok(zova_statement_column_int64(&target_req), db,
                      "workspace_graph.edge_target") != 0) {
            free(edge_type);
            rc = -1;
            break;
        }
        const char *source = workspace_graph_node_map_find(nodes, node_count, source_id);
        const char *target = workspace_graph_node_map_find(nodes, node_count, target_id);
        if (!source || !target) {
            free(edge_type);
            rc = -1;
            break;
        }
        batch.types[batch.count] = edge_type;
        batch.inputs[batch.count] = (zova_graph_edge_input){
            .graph_name = graph_name,
            .from_node_id = source,
            .edge_type = edge_type,
            .to_node_id = target,
        };
        batch.count++;
        if (batch.count == ZV_BATCH) {
            rc = workspace_graph_flush_edge_batch(db, &batch);
        }
    }
    if (rc == 0) {
        rc = workspace_graph_flush_edge_batch(db, &batch);
    }
    workspace_graph_edge_batch_clear(&batch);
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

int cbm_zova_mirror_workspace_graph(const char *zova_path, const char *workspace_id,
                                    const char *project) {
    if (!zova_path || !workspace_name_component_valid(workspace_id) || !project || !project[0]) {
        return -1;
    }
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    workspace_graph_node_map_t *nodes = NULL;
    size_t node_count = 0;
    int rc = workspace_trace_projection_init(zdb.db);
    if (rc == 0) {
        rc = workspace_trace_projection_clear(zdb.db, workspace_id);
    }
    if (rc == 0) {
        rc = graph_delete_named_if_exists(zdb.db, graph_name);
    }
    if (rc == 0) {
        zova_graph_create_request create_req = {.db = zdb.db, .name = graph_name};
        rc = status_ok(zova_graph_create(&create_req), zdb.db, "workspace_graph.create");
    }
    if (rc == 0) {
        rc = workspace_graph_mirror_nodes(zdb.db, graph_name, workspace_id, project, &nodes,
                                          &node_count);
    }
    if (rc == 0) {
        rc = workspace_graph_mirror_edges(zdb.db, graph_name, project, nodes, node_count);
    }
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    }
    if (rc != 0) {
        workspace_registry_rollback(zdb.db);
    }
    workspace_graph_node_map_free(nodes, node_count);
    close_zova(&zdb);
    return rc;
}

static int workspace_graph_write_direct_nodes(zova_database *db, const char *graph_name,
                                              const char *workspace_id, const char *project,
                                              const CBMDumpNode *dump_nodes, int node_count,
                                              workspace_graph_node_map_t **out_items,
                                              size_t *out_count) {
    *out_items = NULL;
    *out_count = 0;
    if (node_count < 0 || (node_count > 0 && !dump_nodes)) {
        return -1;
    }
    workspace_graph_node_map_t *items =
        node_count > 0 ? calloc((size_t)node_count, sizeof(*items)) : NULL;
    if (node_count > 0 && !items) {
        return -1;
    }
    zova_statement *projection_stmt = NULL;
    workspace_graph_node_batch_t batch = {0};
    int rc = prepare_zova(
        db,
        "INSERT INTO cbm_zova_trace_nodes_v1("
        "workspace_id,node_id,project,name,qualified_name,file_path,label,start_line,end_line) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)",
        &projection_stmt, "workspace_graph.direct_projection_prepare");
    size_t count = 0;
    for (int i = 0; rc == 0 && i < node_count; i++) {
        const CBMDumpNode *node = &dump_nodes[i];
        if (node->id <= 0 || !node->label || !node->name || !node->qualified_name ||
            !node->file_path || (count > 0 && items[count - 1].sqlite_id >= node->id)) {
            rc = -1;
            break;
        }
        char discriminator[64];
        if (node->qualified_name[0]) {
            snprintf(discriminator, sizeof(discriminator), "named");
        } else if (snprintf(discriminator, sizeof(discriminator), "anon:%d:%d", node->start_line,
                            node->end_line) >= (int)sizeof(discriminator)) {
            rc = -1;
            break;
        }
        char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (cbm_zova_workspace_node_id_v1(workspace_id, node->label, node->file_path,
                                          node->qualified_name, discriminator, stable_id,
                                          sizeof(stable_id)) != 0) {
            rc = -1;
            break;
        }
        char *stable_id_copy = zv_strndup(stable_id, strlen(stable_id));
        if (!stable_id_copy) {
            rc = -1;
            break;
        }
        rc = workspace_trace_projection_write(db, projection_stmt, workspace_id, stable_id_copy,
                                              project, node->name, node->qualified_name,
                                              node->file_path, node->label, node->start_line,
                                              node->end_line);
        if (rc != 0) {
            free(stable_id_copy);
            break;
        }
        items[count++] = (workspace_graph_node_map_t){.sqlite_id = node->id,
                                                       .stable_id = stable_id_copy};
        batch.inputs[batch.count] = (zova_graph_node_input){
            .graph_name = graph_name,
            .node_id = stable_id_copy,
            .kind = node->label,
            .target_type = ZOVA_GRAPH_TARGET_RECORD,
            .target_namespace = "cbm_nodes",
            .target_ref = stable_id_copy,
        };
        batch.count++;
        if (batch.count == ZV_BATCH) {
            zova_graph_node_put_many_request req = {
                .db = db, .nodes = batch.inputs, .nodes_len = (size_t)batch.count};
            rc = status_ok(zova_graph_node_put_many(&req), db,
                           "workspace_graph.direct_node_put_many");
            batch.count = 0;
        }
    }
    if (rc == 0 && batch.count > 0) {
        zova_graph_node_put_many_request req = {
            .db = db, .nodes = batch.inputs, .nodes_len = (size_t)batch.count};
        rc = status_ok(zova_graph_node_put_many(&req), db,
                       "workspace_graph.direct_node_put_many");
    }
    if (rc == 0 && sidecar_test_fault("after_graph_nodes")) {
        rc = -1;
    }
    if (projection_stmt) {
        (void)zova_statement_finalize(projection_stmt);
    }
    if (rc != 0) {
        workspace_graph_node_map_free(items, count);
        return -1;
    }
    *out_items = items;
    *out_count = count;
    return 0;
}

static int workspace_graph_write_direct_edges(zova_database *db, const char *graph_name,
                                              const CBMDumpEdge *edges, int edge_count,
                                              const workspace_graph_node_map_t *nodes,
                                              size_t node_count) {
    if (edge_count < 0 || (edge_count > 0 && !edges)) {
        return -1;
    }
    zova_graph_edge_input inputs[ZV_BATCH] = {0};
    int batch = 0;
    for (int i = 0; i < edge_count; i++) {
        const CBMDumpEdge *edge = &edges[i];
        const char *source = workspace_graph_node_map_find(nodes, node_count, edge->source_id);
        const char *target = workspace_graph_node_map_find(nodes, node_count, edge->target_id);
        if (!source || !target || !edge->type || !edge->type[0]) {
            return -1;
        }
        inputs[batch++] = (zova_graph_edge_input){
            .graph_name = graph_name,
            .from_node_id = source,
            .edge_type = edge->type,
            .to_node_id = target,
        };
        if (batch == ZV_BATCH) {
            zova_graph_edge_put_many_request req = {
                .db = db, .edges = inputs, .edges_len = (size_t)batch};
            if (status_ok(zova_graph_edge_put_many(&req), db,
                          "workspace_graph.direct_edge_put_many") != 0) {
                return -1;
            }
            batch = 0;
        }
    }
    if (batch > 0) {
        zova_graph_edge_put_many_request req = {
            .db = db, .edges = inputs, .edges_len = (size_t)batch};
        if (status_ok(zova_graph_edge_put_many(&req), db,
                      "workspace_graph.direct_edge_put_many") != 0) {
            return -1;
        }
    }
    if (sidecar_test_fault("after_graph_edges")) {
        return -1;
    }
    return 0;
}

int cbm_zova_write_workspace_graph_direct(const char *zova_path, const char *workspace_id,
                                          const char *project, const CBMDumpNode *nodes,
                                          int node_count, const CBMDumpEdge *edges,
                                          int edge_count) {
    if (!zova_path || !workspace_name_component_valid(workspace_id) || !project || !project[0] ||
        node_count < 0 || edge_count < 0 || (node_count > 0 && !nodes) ||
        (edge_count > 0 && !edges)) {
        return -1;
    }
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    workspace_graph_node_map_t *node_map = NULL;
    size_t node_count_written = 0;
    int rc = workspace_trace_projection_init(zdb.db);
    if (rc == 0) {
        rc = workspace_trace_projection_clear(zdb.db, workspace_id);
    }
    if (rc == 0) {
        rc = graph_delete_named_if_exists(zdb.db, graph_name);
    }
    if (rc == 0) {
        zova_graph_create_request req = {.db = zdb.db, .name = graph_name};
        rc = status_ok(zova_graph_create(&req), zdb.db, "workspace_graph.direct_create");
    }
    if (rc == 0) {
        rc = workspace_graph_write_direct_nodes(zdb.db, graph_name, workspace_id, project, nodes,
                                                node_count, &node_map, &node_count_written);
    }
    if (rc == 0) {
        rc = workspace_graph_write_direct_edges(zdb.db, graph_name, edges, edge_count, node_map,
                                                node_count_written);
    }
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    } else {
        workspace_registry_rollback(zdb.db);
    }
    workspace_graph_node_map_free(node_map, node_count_written);
    close_zova(&zdb);
    return rc;
}

int cbm_zova_delete_workspace_graph(const char *zova_path, const char *workspace_id) {
    if (!zova_path || !workspace_name_component_valid(workspace_id)) {
        return -1;
    }
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    int rc = workspace_trace_projection_init(zdb.db);
    if (rc == 0) {
        rc = workspace_trace_projection_clear(zdb.db, workspace_id);
    }
    if (rc == 0) {
        rc = graph_delete_named_if_exists(zdb.db, graph_name);
    }
    if (rc == 0) {
        rc = workspace_registry_commit(zdb.db);
    } else {
        workspace_registry_rollback(zdb.db);
    }
    close_zova(&zdb);
    return rc;
}

static double graph_ingestion_elapsed_ms(const struct timespec *start,
                                         const struct timespec *end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static int graph_ingestion_convert_container(const char *db_path, const char *dest_path) {
    zova_message error = {0};
    zova_convert_sqlite_to_zova_request request = {
        .source_path = db_path,
        .dest_path = dest_path,
        .out_error_message = &error,
    };
    zova_status status = zova_convert_sqlite_to_zova(&request);
    zova_message_free(&error);
    return status == ZOVA_OK ? 0 : -1;
}

static int graph_ingestion_graph_counts(const char *zova_path, const char *graph_name,
                                        uint64_t *out_nodes, uint64_t *out_edges) {
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0) {
        return -1;
    }
    zova_graph_info info = {0};
    zova_graph_info_get_request request = {
        .db = zdb.db,
        .name = graph_name,
        .out_info = &info,
    };
    int rc = status_ok(zova_graph_info_get(&request), zdb.db, "graph_ingestion.graph_info");
    if (rc == 0) {
        *out_nodes = info.node_count;
        *out_edges = info.edge_count;
    }
    zova_graph_info_free(&info);
    close_zova(&zdb);
    return rc;
}

int cbm_zova_benchmark_workspace_graph_ingestion(
    const char *db_path, const char *direct_zova_path, const char *mirror_zova_path,
    const char *workspace_id, const char *project, const CBMDumpNode *nodes, int node_count,
    const CBMDumpEdge *edges, int edge_count, cbm_zova_graph_ingestion_metrics_t *out_metrics) {
    if (!db_path || !cbm_file_exists(db_path) || !direct_zova_path || !mirror_zova_path ||
        strcmp(direct_zova_path, mirror_zova_path) == 0 || cbm_file_exists(direct_zova_path) ||
        cbm_file_exists(mirror_zova_path) || !workspace_name_component_valid(workspace_id) ||
        !project || !project[0] || node_count < 0 || edge_count < 0 ||
        (node_count > 0 && !nodes) || (edge_count > 0 && !edges) || !out_metrics) {
        return -1;
    }
    memset(out_metrics, 0, sizeof(*out_metrics));
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0 ||
        graph_ingestion_convert_container(db_path, direct_zova_path) != 0 ||
        graph_ingestion_convert_container(db_path, mirror_zova_path) != 0) {
        return -1;
    }

    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = cbm_zova_write_workspace_graph_direct(direct_zova_path, workspace_id, project, nodes,
                                                   node_count, edges, edge_count);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    out_metrics->direct_graph_write_ms = graph_ingestion_elapsed_ms(&start, &end);
    if (rc != 0) {
        return -1;
    }

    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    rc = cbm_zova_mirror_workspace_graph(mirror_zova_path, workspace_id, project);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    out_metrics->sqlite_row_mirror_ms = graph_ingestion_elapsed_ms(&start, &end);
    if (rc != 0 ||
        graph_ingestion_graph_counts(direct_zova_path, graph_name, &out_metrics->direct_node_count,
                                     &out_metrics->direct_edge_count) != 0 ||
        graph_ingestion_graph_counts(mirror_zova_path, graph_name, &out_metrics->mirror_node_count,
                                     &out_metrics->mirror_edge_count) != 0 ||
        out_metrics->direct_node_count != out_metrics->mirror_node_count ||
        out_metrics->direct_edge_count != out_metrics->mirror_edge_count) {
        return -1;
    }
    return 0;
}

static int cbm_zova_after_sqlite_dump_impl(const char *db_path,
                                           const CBMDumpVector *node_vectors,
                                           int node_vector_count,
                                           const CBMDumpTokenVec *token_vectors,
                                           int token_vector_count, int vector_dim,
                                           bool direct_vectors, const char *root_path,
                                           const char *project, const CBMDumpNode *nodes,
                                           int node_count, const CBMDumpEdge *edges,
                                           int edge_count) {
    cbm_zova_mode_t mode = cbm_zova_mode_from_env();
    bool graph_read_enabled = cbm_zova_graph_read_is_enabled();
    if (mode == CBM_ZOVA_MODE_OFF && !graph_read_enabled) {
        return 0;
    }
    if (!db_path || !cbm_file_exists(db_path)) {
        cbm_log_error("zova.convert", "reason", "missing_db_path");
        return -1;
    }

    char zova_path[ZV_PATH_MAX];
    char tmp_path[ZV_PATH_MAX];
    char registry_path[ZV_PATH_MAX] = {0};
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    int64_t requested_generation = 1;
    int64_t generation = 0;
    bool workspace_generation_started = false;
    bool workspace_scoped = root_path && root_path[0] && project && project[0];
    if (cbm_zova_sidecar_path(db_path, zova_path, sizeof(zova_path)) != 0 ||
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.zova", zova_path) >= (int)sizeof(tmp_path)) {
        cbm_log_error("zova.convert", "reason", "path_too_long", "db_path", db_path);
        return -1;
    }
    if (workspace_scoped &&
        (cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)) != 0 ||
         cbm_zova_workspace_get_or_create_at(registry_path, root_path, workspace_id,
                                              sizeof(workspace_id)) != 0 ||
         cbm_zova_workspace_next_generation_at(registry_path, workspace_id,
                                                &requested_generation) != 0)) {
        cbm_log_error("zova.convert", "reason", "workspace_generation_setup");
        return -1;
    }
    if (source_sidecar_generation_assign(db_path, requested_generation, &generation) != 0) {
        cbm_log_error("zova.convert", "reason", "source_generation_assign", "path", db_path);
        return -1;
    }
    if (workspace_scoped) {
        if (cbm_zova_workspace_generation_begin_at(registry_path, workspace_id, generation) != 0) {
            cbm_log_error("zova.convert", "reason", "workspace_generation_begin");
            return -1;
        }
        workspace_generation_started = true;
    }

    cbm_unlink(tmp_path);
    zova_message err = {0};
    zova_convert_sqlite_to_zova_request req = {
        .source_path = db_path,
        .dest_path = tmp_path,
        .out_error_message = &err,
    };
    zova_status st = zova_convert_sqlite_to_zova(&req);
    if (st != ZOVA_OK) {
        cbm_log_error("zova.convert", "status", zova_status_name(st), "msg",
                      err.data ? err.data : "");
        zova_message_free(&err);
        goto failed;
    }
    zova_message_free(&err);

    int rc = cbm_zova_validate_container(tmp_path);
    if (rc == 0 && mode >= CBM_ZOVA_MODE_I8_VECTORS) {
        rc = direct_vectors
                 ? cbm_zova_write_i8_vectors_direct(tmp_path, node_vectors, node_vector_count,
                                                     token_vectors, token_vector_count, vector_dim)
                 : cbm_zova_mirror_i8_vectors(tmp_path, 768);
    }
    if (rc == 0 && sidecar_test_fault("after_vectors")) {
        rc = -1;
    }
    if (rc == 0 && (mode >= CBM_ZOVA_MODE_GRAPH_MIRROR || graph_read_enabled)) {
        if (workspace_scoped) {
            rc = nodes ? cbm_zova_write_workspace_graph_direct(tmp_path, workspace_id, project,
                                                                nodes, node_count, edges, edge_count)
                       : cbm_zova_mirror_workspace_graph(tmp_path, workspace_id, project);
        } else {
            rc = cbm_zova_mirror_graph(tmp_path);
        }
    }
    int64_t temporary_generation = 0;
    if (rc == 0 && cbm_zova_sidecar_generation_get(tmp_path, &temporary_generation) != 0) {
        rc = -1;
    }
    if (rc == 0 && temporary_generation != generation) {
        rc = -1;
    }
    if (rc != 0) {
        goto failed;
    }

    if (rename(tmp_path, zova_path) != 0) {
        cbm_log_error("zova.convert", "phase", "rename", "dest", zova_path);
        goto failed;
    }
    if (sidecar_test_fault("after_publish")) {
        cbm_log_error("zova.convert", "reason", "injected_after_publish");
        goto failed_published;
    }
    if (workspace_generation_started &&
        cbm_zova_workspace_generation_finish_at(registry_path, workspace_id, generation, true) != 0) {
        cbm_log_error("zova.convert", "reason", "workspace_generation_ready");
        goto failed_published;
    }
    cbm_log_info("zova.convert", "mode",
                 mode == CBM_ZOVA_MODE_OFF ? "graph_read_default" : cbm_zova_mode_name(mode),
                 "path", zova_path);
    return 0;

failed:
    cbm_unlink(tmp_path);
failed_published:
    if (workspace_generation_started) {
        (void)cbm_zova_workspace_generation_finish_at(registry_path, workspace_id, generation,
                                                       false);
    }
    return -1;
}

int cbm_zova_after_sqlite_dump(const char *db_path) {
    return cbm_zova_after_sqlite_dump_impl(db_path, NULL, 0, NULL, 0, 0, false, NULL, NULL, NULL,
                                           0, NULL, 0);
}

int cbm_zova_after_sqlite_dump_with_i8_vectors(const char *db_path,
                                                const CBMDumpVector *node_vectors,
                                                int node_vector_count,
                                                const CBMDumpTokenVec *token_vectors,
                                                int token_vector_count, int vector_dim) {
    return cbm_zova_after_sqlite_dump_impl(db_path, node_vectors, node_vector_count, token_vectors,
                                           token_vector_count, vector_dim, true, NULL, NULL, NULL, 0,
                                           NULL, 0);
}

int cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
    const char *db_path, const char *root_path, const char *project,
    const CBMDumpVector *node_vectors, int node_vector_count,
    const CBMDumpTokenVec *token_vectors, int token_vector_count, int vector_dim) {
    return cbm_zova_after_sqlite_dump_impl(db_path, node_vectors, node_vector_count, token_vectors,
                                           token_vector_count, vector_dim, true, root_path, project,
                                           NULL, 0, NULL, 0);
}

int cbm_zova_after_sqlite_dump_workspace_direct(
    const char *db_path, const char *root_path, const char *project, const CBMDumpNode *nodes,
    int node_count, const CBMDumpEdge *edges, int edge_count, const CBMDumpVector *node_vectors,
    int node_vector_count, const CBMDumpTokenVec *token_vectors, int token_vector_count,
    int vector_dim) {
    return cbm_zova_after_sqlite_dump_impl(
        db_path, node_vectors, node_vector_count, token_vectors, token_vector_count, vector_dim, true,
        root_path, project, nodes, node_count, edges, edge_count);
}

static char *column_text_owned(zova_database *db, zova_statement *stmt, int index) {
    zova_text text = {0};
    zova_statement_column_text_request req = {
        .statement = stmt,
        .index = index,
        .out_text = &text,
    };
    if (status_ok(zova_statement_column_text(&req), db, "column.text") != 0) {
        return NULL;
    }
    char *copy = zv_strndup(text.data ? text.data : "", text.data ? text.len : 0);
    zova_text_free(&text);
    return copy;
}

static void string_list_free(char **items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static int append_string(char ***items, int *count, int *cap, char *owned) {
    if (*count == *cap) {
        int next = *cap == 0 ? 64 : *cap * 2;
        char **grown = (char **)realloc(*items, (size_t)next * sizeof(**items));
        if (!grown) {
            return -1;
        }
        *items = grown;
        *cap = next;
    }
    (*items)[*count] = owned;
    (*count)++;
    return 0;
}

static int count_prefetch_candidates(zova_database *db, const char *project, int vector_dim,
                                     int *out_count) {
    (void)vector_dim;
    *out_count = 0;
    const char *sql =
        "SELECT COUNT(*) "
        "FROM nodes n "
        "INNER JOIN _zova_vectors zv "
        "        ON zv.collection_name = 'cbm_node_vectors_i8' "
        "       AND zv.vector_id = CAST(n.id AS TEXT) "
        "WHERE n.project = ?1 "
        "  AND n.label IN ('Function','Method','Class') "
        ";";

    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, "vector.candidates_count_prepare");
    if (rc == 0) {
        zova_statement_bind_text_request preq = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)project,
            .len = strlen(project),
        };
        if (status_ok(zova_statement_bind_text(&preq), db,
                      "vector.candidates_count_bind_project") != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, db, "vector.candidates_count_step");
        } else if (step == ZOVA_STEP_DONE) {
            rc = -1;
        }
    }
    if (rc == 0) {
        int64_t value = 0;
        zova_statement_column_int64_request creq = {
            .statement = stmt,
            .index = 0,
            .out_value = &value,
        };
        if (status_ok(zova_statement_column_int64(&creq), db,
                      "vector.candidates_count_column") != 0 ||
            value < 0 || value > INT32_MAX) {
            rc = -1;
        } else {
            *out_count = (int)value;
        }
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int count_node_collection_vectors(zova_database *db, int *out_count) {
    *out_count = 0;
    const char *sql = "SELECT COUNT(*) FROM _zova_vectors WHERE collection_name = ?1";

    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, "vector.collection_count_prepare");
    if (rc == 0) {
        zova_statement_bind_text_request req = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)CBM_ZOVA_NODE_COLLECTION,
            .len = strlen(CBM_ZOVA_NODE_COLLECTION),
        };
        if (status_ok(zova_statement_bind_text(&req), db, "vector.collection_count_bind") != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, db, "vector.collection_count_step");
        } else if (step == ZOVA_STEP_DONE) {
            rc = -1;
        }
    }
    if (rc == 0) {
        int64_t value = 0;
        zova_statement_column_int64_request creq = {
            .statement = stmt,
            .index = 0,
            .out_value = &value,
        };
        if (status_ok(zova_statement_column_int64(&creq), db,
                      "vector.collection_count_column") != 0 ||
            value < 0 || value > INT32_MAX) {
            rc = -1;
        } else {
            *out_count = (int)value;
        }
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int vector_meta_exec(zova_database *db, const char *sql, const char *phase) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, phase);
    if (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), db, phase);
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int vector_meta_store(zova_database *db, const char *project, int vector_dim,
                             bool full_scan_safe) {
    const char *sql =
        "INSERT OR REPLACE INTO " CBM_ZOVA_VECTOR_META_TABLE
        "(project, collection_name, vector_dim, full_scan_safe) VALUES(?1, ?2, ?3, ?4)";
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, "vector.meta_store_prepare");
    if (rc == 0) {
        zova_statement_bind_text_request project_req = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)project,
            .len = strlen(project),
        };
        zova_statement_bind_text_request collection_req = {
            .statement = stmt,
            .index = 2,
            .data = (const uint8_t *)CBM_ZOVA_NODE_COLLECTION,
            .len = strlen(CBM_ZOVA_NODE_COLLECTION),
        };
        zova_statement_bind_int64_request dim_req = {.statement = stmt, .index = 3, .value = vector_dim};
        zova_statement_bind_int64_request safe_req = {
            .statement = stmt,
            .index = 4,
            .value = full_scan_safe ? 1 : 0,
        };
        if (status_ok(zova_statement_bind_text(&project_req), db, "vector.meta_store_project") != 0 ||
            status_ok(zova_statement_bind_text(&collection_req), db,
                      "vector.meta_store_collection") != 0 ||
            status_ok(zova_statement_bind_int64(&dim_req), db, "vector.meta_store_dim") != 0 ||
            status_ok(zova_statement_bind_int64(&safe_req), db, "vector.meta_store_safe") != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), db, "vector.meta_store_step");
        if (rc == 0 && step != ZOVA_STEP_DONE) {
            rc = -1;
        }
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int vector_meta_refresh(zova_database *db, int vector_dim) {
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS " CBM_ZOVA_VECTOR_META_TABLE "("
        "project TEXT NOT NULL, collection_name TEXT NOT NULL, vector_dim INTEGER NOT NULL, "
        "full_scan_safe INTEGER NOT NULL, PRIMARY KEY(project, collection_name, vector_dim))";
    if (vector_meta_exec(db, create_sql, "vector.meta_create") != 0) {
        return -1;
    }

    int collection_count = 0;
    if (count_node_collection_vectors(db, &collection_count) != 0) {
        return -1;
    }

    zova_statement *stmt = NULL;
    if (prepare_zova(db, "SELECT name FROM projects", &stmt, "vector.meta_projects_prepare") != 0) {
        return -1;
    }

    int rc = 0;
    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&step_req);
        if (st != ZOVA_OK) {
            rc = status_ok(st, db, "vector.meta_projects_step");
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        char *project = column_text_owned(db, stmt, 0);
        if (!project) {
            rc = -1;
            break;
        }
        int candidate_count = 0;
        rc = count_prefetch_candidates(db, project, vector_dim, &candidate_count);
        if (rc == 0) {
            rc = vector_meta_store(db, project, vector_dim,
                                   candidate_count > 0 && candidate_count == collection_count);
        }
        free(project);
        if (rc != 0) {
            break;
        }
    }
    (void)zova_statement_finalize(stmt);
    return rc;
}

static int vector_meta_try_get(zova_database *db, const char *project, int vector_dim,
                               bool *out_found, bool *out_full_scan_safe) {
    *out_found = false;
    *out_full_scan_safe = false;
    const char *sql = "SELECT full_scan_safe FROM " CBM_ZOVA_VECTOR_META_TABLE
                      " WHERE project = ?1 AND collection_name = ?2 AND vector_dim = ?3";
    zova_statement *stmt = NULL;
    zova_database_prepare_request prepare_req = {.db = db, .sql = sql, .out_statement = &stmt};
    if (zova_database_prepare(&prepare_req) != ZOVA_OK || !stmt) {
        return 0;
    }

    int rc = 0;
    zova_statement_bind_text_request project_req = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)project,
        .len = strlen(project),
    };
    zova_statement_bind_text_request collection_req = {
        .statement = stmt,
        .index = 2,
        .data = (const uint8_t *)CBM_ZOVA_NODE_COLLECTION,
        .len = strlen(CBM_ZOVA_NODE_COLLECTION),
    };
    zova_statement_bind_int64_request dim_req = {.statement = stmt, .index = 3, .value = vector_dim};
    if (zova_statement_bind_text(&project_req) != ZOVA_OK ||
        zova_statement_bind_text(&collection_req) != ZOVA_OK ||
        zova_statement_bind_int64(&dim_req) != ZOVA_OK) {
        rc = -1;
    }
    if (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        if (zova_statement_step(&step_req) != ZOVA_OK) {
            rc = -1;
        } else if (step == ZOVA_STEP_ROW) {
            int64_t value = 0;
            zova_statement_column_int64_request col_req = {
                .statement = stmt,
                .index = 0,
                .out_value = &value,
            };
            if (zova_statement_column_int64(&col_req) != ZOVA_OK) {
                rc = -1;
            } else {
                *out_found = true;
                *out_full_scan_safe = value != 0;
            }
        }
    }
    (void)zova_statement_finalize(stmt);
    return rc;
}

static int collect_prefetch_candidate_ids(zova_database *db, const char *project, int vector_dim,
                                          char ***out_ids, int *out_count) {
    (void)vector_dim;
    *out_ids = NULL;
    *out_count = 0;
    const char *sql =
        "SELECT CAST(n.id AS TEXT) "
        "FROM nodes n "
        "INNER JOIN _zova_vectors zv "
        "        ON zv.collection_name = 'cbm_node_vectors_i8' "
        "       AND zv.vector_id = CAST(n.id AS TEXT) "
        "WHERE n.project = ?1 "
        "  AND n.label IN ('Function','Method','Class') "
        ";";

    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, "vector.candidates_prepare");
    if (rc == 0) {
        zova_statement_bind_text_request preq = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)project,
            .len = strlen(project),
        };
        if (status_ok(zova_statement_bind_text(&preq), db, "vector.candidates_bind_project") != 0) {
            rc = -1;
        }
    }

    char **ids = NULL;
    int count = 0;
    int cap = 0;
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, db, "vector.candidates_step");
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        char *id = column_text_owned(db, stmt, 0);
        if (!id || append_string(&ids, &count, &cap, id) != 0) {
            free(id);
            rc = -1;
            break;
        }
    }

    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    if (rc != 0) {
        string_list_free(ids, count);
        return -1;
    }
    *out_ids = ids;
    *out_count = count;
    return 0;
}

static int parse_i64_slice(const char *data, size_t len, int64_t *out) {
    if (!data || len == 0 || len >= ZV_ID_MAX || !out) {
        return -1;
    }
    char buf[ZV_ID_MAX];
    memcpy(buf, data, len);
    buf[len] = '\0';
    errno = 0;
    char *end = NULL;
    long long value = strtoll(buf, &end, 10);
    if (errno != 0 || end != buf + len) {
        return -1;
    }
    *out = (int64_t)value;
    return 0;
}

static int hydrate_prefetch_nodes(zova_database *db, const char *project, int vector_dim,
                                  const zova_vector_search_results *results, bool include_vector,
                                  cbm_zova_node_candidate_t **out, int *out_count) {
    *out = NULL;
    *out_count = 0;
    if (results->len == 0) {
        return 0;
    }
    if (results->len > (size_t)INT32_MAX) {
        return -1;
    }

    cbm_zova_node_candidate_t *items = calloc(results->len, sizeof(*items));
    int64_t *node_ids = calloc(results->len, sizeof(*node_ids));
    bool *seen = calloc(results->len, sizeof(*seen));
    if (!items || !node_ids || !seen) {
        free(items);
        free(node_ids);
        free(seen);
        return -1;
    }
    for (size_t i = 0; i < results->len; i++) {
        if (parse_i64_slice(results->items[i].id, results->items[i].id_len, &node_ids[i]) != 0) {
            free(items);
            free(node_ids);
            free(seen);
            return -1;
        }
        items[i].node_id = node_ids[i];
        items[i].first_score = 1.0 - results->items[i].distance;
    }

    const char *prefix = include_vector
                             ? "SELECT n.id, n.name, n.qualified_name, n.file_path, n.label, v.vector "
                             : "SELECT n.id, n.name, n.qualified_name, n.file_path, n.label ";
    const char *from = include_vector ? "FROM node_vectors v INNER JOIN nodes n ON n.id = v.node_id "
                                      : "FROM nodes n ";
    size_t sql_cap = strlen(prefix) + strlen(from) + results->len * 16 + 256;
    char *sql = malloc(sql_cap);
    if (!sql) {
        free(items);
        free(node_ids);
        free(seen);
        return -1;
    }
    int written = snprintf(sql, sql_cap, "%s%sWHERE n.id IN(", prefix, from);
    int rc = written < 0 || (size_t)written >= sql_cap ? -1 : 0;
    size_t used = rc == 0 ? (size_t)written : 0;
    for (size_t i = 0; rc == 0 && i < results->len; i++) {
        written = snprintf(sql + used, sql_cap - used, "%s?%zu", i == 0 ? "" : ",", i + 1);
        if (written < 0 || (size_t)written >= sql_cap - used) {
            rc = -1;
        } else {
            used += (size_t)written;
        }
    }
    if (rc == 0) {
        written = include_vector
                      ? snprintf(sql + used, sql_cap - used,
                                 ") AND n.project = ?%zu AND v.project = ?%zu AND length(v.vector) = ?%zu",
                                 results->len + 1, results->len + 1, results->len + 2)
                      : snprintf(sql + used, sql_cap - used, ") AND n.project = ?%zu",
                                 results->len + 1);
        if (written < 0 || (size_t)written >= sql_cap - used) {
            rc = -1;
        }
    }

    zova_statement *stmt = NULL;
    if (rc == 0) {
        rc = prepare_zova(db, sql, &stmt, "vector.hydrate_batch_prepare");
    }
    for (size_t i = 0; rc == 0 && i < results->len; i++) {
        zova_statement_bind_int64_request req = {
            .statement = stmt, .index = i + 1, .value = node_ids[i]};
        if (status_ok(zova_statement_bind_int64(&req), db, "vector.hydrate_batch_bind_id") != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        zova_statement_bind_text_request project_req = {
            .statement = stmt,
            .index = results->len + 1,
            .data = (const uint8_t *)project,
            .len = strlen(project),
        };
        if (status_ok(zova_statement_bind_text(&project_req), db,
                      "vector.hydrate_batch_bind_project") != 0) {
            rc = -1;
        }
        if (rc == 0 && include_vector) {
            zova_statement_bind_int64_request dim_req = {
                .statement = stmt, .index = results->len + 2, .value = vector_dim};
            if (status_ok(zova_statement_bind_int64(&dim_req), db,
                          "vector.hydrate_batch_bind_dim") != 0) {
                rc = -1;
            }
        }
    }
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        zova_status status = zova_statement_step(&step_req);
        if (status != ZOVA_OK) {
            rc = status_ok(status, db, "vector.hydrate_batch_step");
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        int64_t node_id = 0;
        zova_statement_column_int64_request id_req = {
            .statement = stmt, .index = 0, .out_value = &node_id};
        if (status_ok(zova_statement_column_int64(&id_req), db,
                      "vector.hydrate_batch_column_id") != 0) {
            rc = -1;
            break;
        }
        size_t index = results->len;
        for (size_t i = 0; i < results->len; i++) {
            if (node_ids[i] == node_id) {
                index = i;
                break;
            }
        }
        if (index == results->len || seen[index]) {
            rc = -1;
            break;
        }
        cbm_zova_node_candidate_t *row = &items[index];
        row->name = column_text_owned(db, stmt, 1);
        row->qualified_name = column_text_owned(db, stmt, 2);
        row->file_path = column_text_owned(db, stmt, 3);
        row->label = column_text_owned(db, stmt, 4);
        if (!row->name || !row->qualified_name || !row->file_path || !row->label) {
            rc = -1;
            break;
        }
        if (include_vector) {
            zova_buffer blob = {0};
            zova_statement_column_blob_request blob_req = {
                .statement = stmt, .index = 5, .out_buffer = &blob};
            if (status_ok(zova_statement_column_blob(&blob_req), db,
                          "vector.hydrate_batch_column_vector") != 0 ||
                blob.len > (size_t)INT32_MAX) {
                zova_buffer_free(&blob);
                rc = -1;
                break;
            }
            row->vector = malloc(blob.len);
            if (!row->vector) {
                zova_buffer_free(&blob);
                rc = -1;
                break;
            }
            memcpy(row->vector, blob.data, blob.len);
            row->vector_len = (int)blob.len;
            zova_buffer_free(&blob);
        }
        seen[index] = true;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    for (size_t i = 0; rc == 0 && i < results->len; i++) {
        if (!seen[i]) {
            rc = -1;
        }
    }
    free(sql);
    free(node_ids);
    free(seen);
    if (rc != 0) {
        cbm_zova_node_candidates_free(items, (int)results->len);
        return -1;
    }
    *out = items;
    *out_count = (int)results->len;
    return 0;
}

static double prefetch_elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static void prefetch_record_elapsed(const struct timespec *start, double *out_ms) {
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = prefetch_elapsed_ms(start, &end);
}

static int vector_session_prefetch_nodes_ex(
    zova_database *db, const char *project, const int8_t *query, int vector_dim, int limit,
    bool include_vector, cbm_zova_node_candidate_t **out, int *out_count,
    cbm_zova_vector_prefetch_metrics_t *metrics) {
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!db || !project || !query || vector_dim <= 0 || limit <= 0 || !out || !out_count) {
        return -1;
    }
    if (!i8_nonzero((const uint8_t *)query, (size_t)vector_dim)) {
        return -1;
    }

    struct timespec stage_start;

    char **candidate_ids = NULL;
    int candidate_count = 0;
    int collection_count = 0;
    bool meta_found = false;
    bool meta_full_scan_safe = false;
    int rc = vector_meta_try_get(db, project, vector_dim, &meta_found, &meta_full_scan_safe);
    bool use_full_search = rc == 0 && meta_found && meta_full_scan_safe;
    if (!use_full_search) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
        rc = count_prefetch_candidates(db, project, vector_dim, &candidate_count);
        if (rc == 0 && candidate_count > 0) {
            rc = count_node_collection_vectors(db, &collection_count);
        }
        if (metrics) {
            prefetch_record_elapsed(&stage_start, &metrics->candidate_count_ms);
        }
        if (rc == 0) {
            use_full_search = cbm_zova_should_use_full_vector_search(candidate_count, collection_count);
        }
    }
    cbm_zova_node_candidate_t *items = NULL;
    int count = 0;

    zova_vector_search_results results = {0};
    if (rc == 0 && (use_full_search || candidate_count > 0)) {
        zova_vector_values query_values = {
            .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
            .f32_values = NULL,
            .f16_values = NULL,
            .i8_values = query,
            .values_len = (size_t)vector_dim,
        };
        if (use_full_search) {
            if (metrics) {
                metrics->used_full_search = true;
            }
            zova_vector_search_request req = {
                .db = db,
                .collection_name = CBM_ZOVA_NODE_COLLECTION,
                .query = query_values,
                .limit = (size_t)limit,
                .out_results = &results,
            };
            cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
            rc = status_ok(zova_vector_search(&req), db, "vector.search_i8");
            if (metrics) {
                prefetch_record_elapsed(&stage_start, &metrics->vector_search_ms);
            }
        } else {
            cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
            rc = collect_prefetch_candidate_ids(db, project, vector_dim, &candidate_ids,
                                                &candidate_count);
            if (metrics) {
                prefetch_record_elapsed(&stage_start, &metrics->candidate_id_collection_ms);
            }
            if (rc == 0 && candidate_count > 0) {
                zova_vector_search_in_request req = {
                    .db = db,
                    .collection_name = CBM_ZOVA_NODE_COLLECTION,
                    .query = query_values,
                    .candidate_ids = (const char *const *)candidate_ids,
                    .candidate_count = (size_t)candidate_count,
                    .limit = (size_t)limit,
                    .out_results = &results,
                };
                cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
                rc = status_ok(zova_vector_search_in(&req), db, "vector.search_in_i8");
                if (metrics) {
                    prefetch_record_elapsed(&stage_start, &metrics->vector_search_ms);
                }
            }
        }
    }

    if (rc == 0) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
        rc = hydrate_prefetch_nodes(db, project, vector_dim, &results, include_vector, &items,
                                    &count);
        if (metrics) {
            prefetch_record_elapsed(&stage_start, &metrics->hydration_ms);
        }
    }
    zova_vector_search_results_free(&results);
    string_list_free(candidate_ids, candidate_count);
    if (rc != 0) {
        cbm_zova_node_candidates_free(items, count);
        return -1;
    }
    *out = items;
    *out_count = count;
    return 0;
}

cbm_zova_vector_session_t *cbm_zova_vector_session_open(const char *zova_path) {
    if (!zova_path) {
        return NULL;
    }
    cbm_zova_vector_session_t *session = calloc(1, sizeof(*session));
    if (!session || open_zova(zova_path, true, &session->zdb) != 0) {
        free(session);
        return NULL;
    }
    session->generation_valid = sidecar_generation_read_db(session->zdb.db, &session->generation) == 0;
    return session;
}

void cbm_zova_vector_session_close(cbm_zova_vector_session_t *session) {
    if (!session) {
        return;
    }
    close_zova(&session->zdb);
    free(session);
}

int cbm_zova_vector_session_generation(const cbm_zova_vector_session_t *session,
                                       int64_t *out_generation) {
    if (!session || !out_generation || !session->generation_valid) {
        return -1;
    }
    *out_generation = session->generation;
    return 0;
}

int cbm_zova_vector_session_has_workspace(const cbm_zova_vector_session_t *session,
                                          const char *workspace_id, bool *out_present) {
    if (!session || !workspace_name_component_valid(workspace_id) || !out_present) {
        return -1;
    }
    *out_present = false;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(session->zdb.db,
                          "SELECT 1 FROM sqlite_master WHERE type = 'table' "
                          "AND name = 'cbm_zova_trace_nodes_v1' LIMIT 1",
                          &stmt, "vector_session.trace_table_prepare");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), session->zdb.db,
                       "vector_session.trace_table_step");
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
        stmt = NULL;
    }
    if (rc != 0 || step == ZOVA_STEP_DONE) {
        return rc;
    }
    if (step != ZOVA_STEP_ROW ||
        prepare_zova(session->zdb.db,
                     "SELECT 1 FROM cbm_zova_trace_nodes_v1 WHERE workspace_id = ?1 LIMIT 1",
                     &stmt, "vector_session.workspace_prepare") != 0 ||
        workspace_bind_text(session->zdb.db, stmt, 1, workspace_id,
                            "vector_session.workspace_bind") != 0) {
        if (stmt) {
            (void)zova_statement_finalize(stmt);
            stmt = NULL;
        }
        return -1;
    }
    step = ZOVA_STEP_DONE;
    zova_statement_step_request req = {.statement = stmt, .out_result = &step};
    rc = status_ok(zova_statement_step(&req), session->zdb.db, "vector_session.workspace_step");
    if (stmt) {
        (void)zova_statement_finalize(stmt);
        stmt = NULL;
    }
    if (rc != 0 || (step != ZOVA_STEP_ROW && step != ZOVA_STEP_DONE)) {
        return -1;
    }
    *out_present = step == ZOVA_STEP_ROW;
    return 0;
}

int cbm_zova_vector_session_prefetch_nodes_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query, int vector_dim,
    int limit, bool include_vector, cbm_zova_node_candidate_t **out, int *out_count,
    cbm_zova_vector_prefetch_metrics_t *metrics) {
    if (!session) {
        return -1;
    }
    return vector_session_prefetch_nodes_ex(session->zdb.db, project, query, vector_dim, limit,
                                            include_vector, out, out_count, metrics);
}

int cbm_zova_vector_session_prefetch_multi_i8_ex(
    cbm_zova_vector_session_t *session, const char *project, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_node_candidate_t **out, int *out_count, cbm_zova_vector_prefetch_metrics_t *metrics) {
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!session || !project || !query_values || query_count < 2 || vector_dim <= 0 ||
        prefilter_limit <= 0 || limit <= 0 || !out || !out_count ||
        (size_t)query_count > SIZE_MAX / (size_t)vector_dim) {
        return -1;
    }
    size_t query_values_len = (size_t)query_count * (size_t)vector_dim;
    for (int query_index = 0; query_index < query_count; query_index++) {
        if (!i8_nonzero((const uint8_t *)(query_values + (size_t)query_index * vector_dim),
                        (size_t)vector_dim)) {
            return -1;
        }
    }

    zova_database *db = session->zdb.db;
    char **candidate_ids = NULL;
    int candidate_count = 0;
    int collection_count = 0;
    bool meta_found = false;
    bool meta_full_scan_safe = false;
    int rc = vector_meta_try_get(db, project, vector_dim, &meta_found, &meta_full_scan_safe);
    bool use_full_search = rc == 0 && meta_found && meta_full_scan_safe;
    struct timespec stage_start;
    if (!use_full_search) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
        rc = count_prefetch_candidates(db, project, vector_dim, &candidate_count);
        if (rc == 0 && candidate_count > 0) {
            rc = count_node_collection_vectors(db, &collection_count);
        }
        if (metrics) {
            prefetch_record_elapsed(&stage_start, &metrics->candidate_count_ms);
        }
        if (rc == 0) {
            use_full_search = cbm_zova_should_use_full_vector_search(candidate_count, collection_count);
        }
    }

    zova_vector_search_results results = {0};
    if (rc == 0 && (use_full_search || candidate_count > 0)) {
        if (use_full_search && metrics) {
            metrics->used_full_search = true;
        }
        if (!use_full_search) {
            cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
            rc = collect_prefetch_candidate_ids(db, project, vector_dim, &candidate_ids,
                                                &candidate_count);
            if (metrics) {
                prefetch_record_elapsed(&stage_start, &metrics->candidate_id_collection_ms);
            }
        }
        if (rc == 0 && (use_full_search || candidate_count > 0)) {
            zova_vector_search_multi_i8_request req = {
                .db = db,
                .collection_name = CBM_ZOVA_NODE_COLLECTION,
                .query_values = query_values,
                .query_values_len = query_values_len,
                .query_count = (size_t)query_count,
                .dimensions = (size_t)vector_dim,
                .candidate_ids = use_full_search ? NULL : (const char *const *)candidate_ids,
                .candidate_count = use_full_search ? 0 : (size_t)candidate_count,
                .mode = ZOVA_VECTOR_MULTI_I8_SEARCH_CBM_PREFILTER_MIN_COSINE,
                .aggregation = ZOVA_VECTOR_MULTI_I8_AGGREGATION_MIN_COSINE,
                .prefilter_query_index = 0,
                .prefilter_limit = (size_t)prefilter_limit,
                .limit = (size_t)limit,
                .out_results = &results,
            };
            cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
            rc = status_ok(zova_vector_search_multi_i8(&req), db, "vector.search_multi_i8");
            if (metrics) {
                prefetch_record_elapsed(&stage_start, &metrics->vector_search_ms);
            }
        }
    }

    cbm_zova_node_candidate_t *items = NULL;
    int count = 0;
    if (rc == 0) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
        rc = hydrate_prefetch_nodes(db, project, vector_dim, &results, false, &items, &count);
        if (metrics) {
            prefetch_record_elapsed(&stage_start, &metrics->hydration_ms);
        }
    }
    zova_vector_search_results_free(&results);
    string_list_free(candidate_ids, candidate_count);
    if (rc != 0) {
        cbm_zova_node_candidates_free(items, count);
        return -1;
    }
    *out = items;
    *out_count = count;
    return 0;
}

int cbm_zova_vector_prefetch_nodes_ex(const char *zova_path, const char *project,
                                      const int8_t *query, int vector_dim, int limit,
                                      cbm_zova_node_candidate_t **out, int *out_count,
                                      cbm_zova_vector_prefetch_metrics_t *metrics) {
    struct timespec stage_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
    cbm_zova_vector_session_t *session = cbm_zova_vector_session_open(zova_path);
    if (!session) {
        return -1;
    }
    double open_ms = 0.0;
    if (metrics) {
        prefetch_record_elapsed(&stage_start, &open_ms);
    }
    int rc = cbm_zova_vector_session_prefetch_nodes_ex(session, project, query, vector_dim, limit,
                                                        true, out, out_count, metrics);
    if (metrics) {
        metrics->open_ms = open_ms;
    }
    cbm_zova_vector_session_close(session);
    return rc;
}

int cbm_zova_vector_prefetch_nodes(const char *zova_path, const char *project,
                                   const int8_t *query, int vector_dim, int limit,
                                   cbm_zova_node_candidate_t **out, int *out_count) {
    return cbm_zova_vector_prefetch_nodes_ex(zova_path, project, query, vector_dim, limit, out,
                                              out_count, NULL);
}

typedef struct {
    const char *node_id;
    int index;
} graph_visit_index_t;

static int graph_visit_index_compare(const void *left, const void *right) {
    const graph_visit_index_t *a = left;
    const graph_visit_index_t *b = right;
    return strcmp(a->node_id, b->node_id);
}

static int graph_visit_find(const graph_visit_index_t *items, int count, const char *node_id) {
    graph_visit_index_t key = {.node_id = node_id, .index = 0};
    const graph_visit_index_t *found = bsearch(&key, items, (size_t)count, sizeof(*items),
                                               graph_visit_index_compare);
    if (!found) {
        return -1;
    }
    if ((found > items && strcmp((found - 1)->node_id, node_id) == 0) ||
        (found + 1 < items + count && strcmp((found + 1)->node_id, node_id) == 0)) {
        return -1;
    }
    return found->index;
}

static void graph_record_elapsed(const struct timespec *start, double *out_ms) {
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms += ((double)(end.tv_sec - start->tv_sec) * 1000.0) +
               ((double)(end.tv_nsec - start->tv_nsec) / 1000000.0);
}

static int graph_hydrate_visits(zova_database *db, const char *workspace_id,
                                 cbm_zova_graph_visit_t *visits, const graph_visit_index_t *index,
                                 int count, cbm_zova_graph_metrics_t *metrics) {
    bool *filled = calloc((size_t)count, sizeof(*filled));
    if (!filled) {
        return -1;
    }
    int rc = 0;
    for (int offset = 0; rc == 0 && offset < count; offset += ZV_GRAPH_HYDRATE_BATCH) {
        int batch_count = count - offset;
        if (batch_count > ZV_GRAPH_HYDRATE_BATCH) {
            batch_count = ZV_GRAPH_HYDRATE_BATCH;
        }
        char sql[ZV_SQL_MAX * 8];
        int length = snprintf(sql, sizeof(sql),
                              "SELECT node_id,name,qualified_name,file_path,label,start_line,end_line "
                              "FROM cbm_zova_trace_nodes_v1 WHERE workspace_id=?1 AND node_id IN (");
        if (length < 0 || (size_t)length >= sizeof(sql)) {
            rc = -1;
            break;
        }
        for (int i = 0; i < batch_count; i++) {
            int written = snprintf(sql + length, sizeof(sql) - (size_t)length, "%s?%d",
                                   i == 0 ? "" : ",", i + 2);
            if (written < 0 || (size_t)written >= sizeof(sql) - (size_t)length) {
                rc = -1;
                break;
            }
            length += written;
        }
        if (rc != 0 || snprintf(sql + length, sizeof(sql) - (size_t)length, ")") >=
                           (int)(sizeof(sql) - (size_t)length)) {
            rc = -1;
            break;
        }
        zova_statement *stmt = NULL;
        struct timespec stage_start;
        cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
        rc = prepare_zova(db, sql, &stmt, "graph.hydrate_prepare");
        if (metrics) {
            graph_record_elapsed(&stage_start, &metrics->hydrate_prepare_ms);
        }
        if (rc == 0) {
            rc = workspace_bind_text(db, stmt, 1, workspace_id, "graph.hydrate_bind_workspace");
        }
        for (int i = 0; rc == 0 && i < batch_count; i++) {
            rc = workspace_bind_text(db, stmt, i + 2, visits[offset + i].node_id,
                                     "graph.hydrate_bind_node");
        }
        if (rc == 0) {
            cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
        }
        while (rc == 0) {
            zova_step_result step = ZOVA_STEP_DONE;
            zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
            if (status_ok(zova_statement_step(&step_req), db, "graph.hydrate_step") != 0) {
                rc = -1;
                break;
            }
            if (step == ZOVA_STEP_DONE) {
                break;
            }
            char *node_id = column_text_owned(db, stmt, 0);
            int visit_index = node_id ? graph_visit_find(index, count, node_id) : -1;
            free(node_id);
            if (visit_index < offset || visit_index >= offset + batch_count || filled[visit_index]) {
                rc = -1;
                break;
            }
            cbm_zova_graph_visit_t *visit = &visits[visit_index];
            visit->name = column_text_owned(db, stmt, 1);
            visit->qualified_name = column_text_owned(db, stmt, 2);
            visit->file_path = column_text_owned(db, stmt, 3);
            visit->label = column_text_owned(db, stmt, 4);
            int64_t start_line = 0;
            int64_t end_line = 0;
            zova_statement_column_int64_request start_req = {
                .statement = stmt, .index = 5, .out_value = &start_line};
            zova_statement_column_int64_request end_req = {
                .statement = stmt, .index = 6, .out_value = &end_line};
            if (!visit->name || !visit->qualified_name || !visit->file_path || !visit->label ||
                status_ok(zova_statement_column_int64(&start_req), db, "graph.hydrate_start") != 0 ||
                status_ok(zova_statement_column_int64(&end_req), db, "graph.hydrate_end") != 0 ||
                start_line < INT_MIN || start_line > INT_MAX || end_line < INT_MIN ||
                end_line > INT_MAX) {
                rc = -1;
                break;
            }
            visit->start_line = (int)start_line;
            visit->end_line = (int)end_line;
            filled[visit_index] = true;
        }
        if (metrics && rc == 0) {
            graph_record_elapsed(&stage_start, &metrics->hydrate_step_ms);
        }
        if (stmt) {
            (void)zova_statement_finalize(stmt);
        }
    }
    for (int i = 0; rc == 0 && i < count; i++) {
        if (!filled[i]) {
            rc = -1;
        }
    }
    free(filled);
    return rc;
}

static bool graph_native_profile_enabled(void) {
    const char *value = getenv("CBM_ZOVA_GRAPH_PROFILE");
    return value && strcmp(value, "1") == 0;
}

static void graph_metrics_copy_native_profile(cbm_zova_graph_metrics_t *metrics,
                                              const zova_graph_walk_profile *profile) {
    if (!metrics || !profile) {
        return;
    }
    metrics->native_profiled = true;
    metrics->native_mutex_wait_ms = profile->mutex_wait_ms;
    metrics->native_root_lookup_ms = profile->root_lookup_ms;
    metrics->native_adjacency_prepare_ms = profile->adjacency_prepare_ms;
    metrics->native_adjacency_execute_ms = profile->adjacency_execute_ms;
    metrics->native_bfs_bookkeeping_allocation_ms = profile->bfs_bookkeeping_allocation_ms;
    metrics->native_c_abi_result_export_ms = profile->c_abi_result_export_ms;
    metrics->native_total_profiled_ms = profile->total_profiled_ms;
    metrics->frontier_expansions = profile->frontier_expansions;
    metrics->adjacency_query_binds = profile->adjacency_query_binds;
    metrics->adjacency_rows_stepped = profile->adjacency_rows_stepped;
    metrics->native_result_count = profile->result_count;
}

cbm_zova_graph_session_t *cbm_zova_graph_session_open(const char *zova_path) {
    if (!zova_path) {
        return NULL;
    }
    cbm_zova_graph_session_t *session = calloc(1, sizeof(*session));
    if (!session || open_zova(zova_path, true, &session->zdb) != 0) {
        free(session);
        return NULL;
    }
    session->generation_valid = sidecar_generation_read_db(session->zdb.db, &session->generation) == 0;
    return session;
}

void cbm_zova_graph_session_close(cbm_zova_graph_session_t *session) {
    if (!session) {
        return;
    }
    close_zova(&session->zdb);
    free(session);
}

int cbm_zova_graph_session_generation(const cbm_zova_graph_session_t *session,
                                      int64_t *out_generation) {
    if (!session || !out_generation || !session->generation_valid) {
        return -1;
    }
    *out_generation = session->generation;
    return 0;
}

int cbm_zova_graph_session_walk_calls(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *start_node_id, const char *direction, int max_depth, int max_results,
    cbm_zova_graph_visit_t **out_visits, int *out_count, cbm_zova_graph_metrics_t *out_metrics) {
    if (out_visits) {
        *out_visits = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (out_metrics) {
        memset(out_metrics, 0, sizeof(*out_metrics));
        out_metrics->fallback = true;
    }
    if (!session || !workspace_name_component_valid(workspace_id) || !graph_name || !graph_name[0] ||
        !start_node_id || !start_node_id[0] || !direction || max_depth < 0 || max_results <= 0 ||
        !out_visits || !out_count || max_results == INT_MAX) {
        return -1;
    }
    int zova_direction = strcmp(direction, "inbound") == 0
                             ? ZOVA_GRAPH_NEIGHBOR_INCOMING
                             : strcmp(direction, "outbound") == 0 ? ZOVA_GRAPH_NEIGHBOR_OUTGOING : -1;
    if (zova_direction < 0) {
        return -1;
    }
    zova_graph_walk_results results = {0};
    bool profiled = graph_native_profile_enabled();
    zova_graph_walk_profile profile = {0};
    zova_graph_walk_direction_request request = {
        .db = session->zdb.db,
        .graph_name = graph_name,
        .start_node_id = start_node_id,
        .direction = zova_direction,
        .edge_type = "CALLS",
        .max_depth = (uint32_t)max_depth,
        .limit = (size_t)max_results + 1,
        .out_results = &results,
    };
    struct timespec stage_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
    int rc;
    if (profiled) {
        zova_graph_walk_direction_profiled_request profiled_request = {
            .db = request.db,
            .graph_name = request.graph_name,
            .start_node_id = request.start_node_id,
            .direction = request.direction,
            .edge_type = request.edge_type,
            .max_depth = request.max_depth,
            .limit = request.limit,
            .out_results = request.out_results,
            .out_profile = &profile,
        };
        rc = status_ok(zova_graph_walk_direction_profiled(&profiled_request), session->zdb.db,
                       "graph.walk_calls_profiled");
        if (out_metrics) {
            graph_metrics_copy_native_profile(out_metrics, &profile);
        }
    } else {
        rc = status_ok(zova_graph_walk_direction(&request), session->zdb.db, "graph.walk_calls");
    }
    if (out_metrics) {
        graph_record_elapsed(&stage_start, &out_metrics->walk_ms);
    }
    if (rc != 0 || results.len == 0 || !results.items ||
        strcmp(results.items[0].node_id, start_node_id) != 0 || results.items[0].depth != 0 ||
        results.len > (size_t)max_results + 1 || results.len - 1 > (size_t)INT_MAX) {
        zova_graph_walk_results_free(&results);
        return -1;
    }
    int count = (int)(results.len - 1);
    cbm_zova_graph_visit_t *visits = calloc((size_t)count, sizeof(*visits));
    graph_visit_index_t *index = calloc((size_t)count, sizeof(*index));
    cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
    if ((count > 0 && (!visits || !index))) {
        rc = -1;
    }
    for (int i = 0; rc == 0 && i < count; i++) {
        const zova_graph_walk_result *item = &results.items[i + 1];
        if (!item->node_id || !item->node_id[0] || item->depth == 0 || item->depth > (uint32_t)INT_MAX) {
            rc = -1;
            break;
        }
        visits[i].node_id = zv_strndup(item->node_id, strlen(item->node_id));
        visits[i].hop = (int)item->depth;
        index[i] = (graph_visit_index_t){.node_id = visits[i].node_id, .index = i};
        if (!visits[i].node_id) {
            rc = -1;
        }
    }
    if (rc == 0 && count > 1) {
        qsort(index, (size_t)count, sizeof(*index), graph_visit_index_compare);
        for (int i = 1; i < count; i++) {
            if (strcmp(index[i - 1].node_id, index[i].node_id) == 0) {
                rc = -1;
                break;
            }
        }
    }
    if (out_metrics) {
        graph_record_elapsed(&stage_start, &out_metrics->result_build_ms);
    }
    if (rc == 0 && count > 0) {
        rc = graph_hydrate_visits(session->zdb.db, workspace_id, visits, index, count, out_metrics);
    }
    zova_graph_walk_results_free(&results);
    free(index);
    if (rc != 0) {
        cbm_zova_graph_visits_free(visits, count);
        return -1;
    }
    *out_visits = visits;
    *out_count = count;
    if (out_metrics) {
        out_metrics->walk_count = 1;
        out_metrics->fallback = false;
    }
    return 0;
}

int cbm_zova_graph_neighbor_count(const char *zova_path, const char *node_id, int *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    if (!zova_path || !node_id || !out_count) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, true, &zdb) != 0) {
        return -1;
    }
    zova_graph_neighbor_results results = {0};
    zova_graph_neighbors_request req = {
        .db = zdb.db,
        .graph_name = CBM_ZOVA_CODE_GRAPH,
        .node_id = node_id,
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = NULL,
        .limit = 100,
        .out_results = &results,
    };
    int rc = status_ok(zova_graph_neighbors(&req), zdb.db, "graph.neighbors");
    if (rc == 0) {
        *out_count = (int)results.len;
    }
    zova_graph_neighbor_results_free(&results);
    close_zova(&zdb);
    return rc;
}

#endif
