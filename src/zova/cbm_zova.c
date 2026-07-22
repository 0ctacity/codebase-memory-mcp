#include "zova/cbm_zova.h"
#include "zova/cbm_zova_legacy_snapshot.h"
#include "zova/cbm_zova_migration.h"
#include "zova/cbm_zova_edge_payload.h"
#include "zova/cbm_zova_publish_model.h"
#include "zova/cbm_zova_delta.h"
#include "zova/cbm_zova_writer_gate.h"

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
#include <sys/stat.h>

#define CBM_NODE_FTS_INSERT_TRIGGER_SQL \
    "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_ai AFTER INSERT ON cbm_nodes_v1 BEGIN " \
    "INSERT INTO cbm_nodes_fts_v1(rowid,name,qualified_name,file_path,label) " \
    "SELECT NEW.zova_node_key,cbm_camel_split(NEW.name),NEW.qualified_name,f.file_path,NEW.label " \
    "FROM cbm_files_v1 f WHERE f.file_key=NEW.file_key; END;"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

enum {
    ZV_PATH_MAX = 1024,
    ZV_BATCH = 512,
    ZV_ID_MAX = 32,
    ZV_SQL_MAX = 512,
};

static cbm_zova_publish_test_metrics_t user_publish_test_metrics = {0};
#if CBM_WITH_ZOVA
static _Thread_local bool user_publish_delta_active = false;
#endif

static void user_publish_metric_row(cbm_zova_statement_phase_metrics_t *metrics,
                                    uint64_t bind_i64_calls,
                                    uint64_t bind_text_calls,
                                    uint64_t bind_double_calls,
                                    bool cleared_bindings) {
    if (!metrics) return;
    metrics->rows++;
    metrics->bind_i64_calls += bind_i64_calls;
    metrics->bind_text_calls += bind_text_calls;
    metrics->bind_double_calls += bind_double_calls;
    metrics->step_calls++;
    metrics->reset_calls++;
    if (cleared_bindings) metrics->clear_bindings_calls++;
}

void cbm_zova_publish_test_metrics_reset(void) {
    memset(&user_publish_test_metrics, 0, sizeof(user_publish_test_metrics));
}

void cbm_zova_publish_test_metrics_get(cbm_zova_publish_test_metrics_t *out_metrics) {
    if (out_metrics) *out_metrics = user_publish_test_metrics;
}

#define CBM_ZOVA_VECTOR_META_TABLE "_cbm_zova_vector_meta"

bool cbm_zova_build_enabled(void) {
    return CBM_WITH_ZOVA != 0;
}

static int workspace_normalize_root(const char *root_path, char out[ZV_PATH_MAX]) {
    if (!root_path || !root_path[0] ||
        snprintf(out, ZV_PATH_MAX, "%s", root_path) >= ZV_PATH_MAX) {
        return -1;
    }
    cbm_normalize_path_sep(out);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    return 0;
}

static int workspace_id_from_root(const char *root, char *out, size_t out_size) {
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(root, strlen(root), digest);
    return snprintf(out, out_size, "w1_%.32s", digest) < (int)out_size ? 0 : -1;
}

int cbm_zova_workspace_id_for_root(const char *root_path, char *out_workspace_id,
                                   size_t out_workspace_id_size) {
    char root[ZV_PATH_MAX];
    if (!out_workspace_id || out_workspace_id_size == 0 ||
        workspace_normalize_root(root_path, root) != 0)
        return -1;
    return workspace_id_from_root(root, out_workspace_id, out_workspace_id_size);
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
    if (strcmp(mode, "authority") == 0) {
        return CBM_ZOVA_MODE_AUTHORITY;
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
    case CBM_ZOVA_MODE_AUTHORITY:
        return "authority";
    }
    return "off";
}

bool cbm_zova_graph_read_is_enabled(void) {
    const char *mode = getenv("CBM_ZOVA_MODE");
    return !mode || mode[0] == '\0' || strcmp(mode, "graph_read") == 0 ||
           strcmp(mode, "authority") == 0;
}

bool cbm_zova_vector_read_is_enabled(void) {
    const char *mode = getenv("CBM_ZOVA_MODE");
    if (!mode || mode[0] == '\0') {
        return true;
    }
    return strcmp(mode, "i8_vectors") == 0 || strcmp(mode, "graph_mirror") == 0 ||
           strcmp(mode, "graph_read") == 0 || strcmp(mode, "authority") == 0;
}

bool cbm_zova_single_file_enabled(void) {
    const char *mode = getenv("CBM_ZOVA_MODE");
    return !mode || mode[0] == '\0' || strcmp(mode, "authority") == 0;
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

int cbm_zova_workspace_id_validate(const char *workspace_id) {
    return workspace_name_component_valid(workspace_id) ? 0 : -1;
}

int cbm_zova_workspace_token_id_v1(const char *workspace_id, const char *token,
                                   char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) || !token || !out || out_size == 0) {
        return -1;
    }
    cbm_sha256_ctx hash;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char digits[] = "0123456789abcdef";
    char hex[33];
    cbm_sha256_init(&hash);
    cbm_sha256_update(&hash, token, strlen(token));
    cbm_sha256_update(&hash, "\0", 1);
    cbm_sha256_final(&hash, digest);
    for (size_t i = 0; i < 16; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[32] = '\0';
    return snprintf(out, out_size, "t:v1:%s:%s", workspace_id, hex) < (int)out_size ? 0 : -1;
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

int cbm_zova_workspace_node_id_v2(const char *workspace_id, const char *node_kind,
                                  const char *relative_path, const char *qualified_name,
                                  const char *semantic_discriminator, char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) || !node_kind || !node_kind[0] ||
        !relative_path || !qualified_name || !semantic_discriminator ||
        !semantic_discriminator[0] || !out || out_size == 0) {
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
    return snprintf(out, out_size, "n:v2:%s:%s", workspace_id, hex) < (int)out_size ? 0 : -1;
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

void cbm_zova_vector_hits_free(cbm_zova_vector_hit_t *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) free(items[i].vector_id);
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
        free(visits[i].properties);
    }
    free(visits);
}

#if !CBM_WITH_ZOVA

int cbm_zova_workspace_generation_digest_input(
    const char *workspace_id, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result) {
    (void)workspace_id;
    (void)input;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    return -1;
}

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

int cbm_zova_probe_sql_capabilities(const char *zova_path,
                                    cbm_zova_sql_capabilities_t *out_capabilities) {
    (void)zova_path;
    if (out_capabilities) {
        memset(out_capabilities, 0, sizeof(*out_capabilities));
    }
    return -1;
}

int cbm_zova_user_database_init(const char *zova_path) {
    (void)zova_path;
    return -1;
}

int cbm_zova_user_database_generation_begin(const char *zova_path, const char *root_path,
                                             int64_t generation, char *out_workspace_id,
                                             size_t out_workspace_id_size) {
    (void)zova_path;
    (void)root_path;
    (void)generation;
    (void)out_workspace_id;
    (void)out_workspace_id_size;
    return -1;
}

int cbm_zova_user_database_generation_finish(const char *zova_path, const char *workspace_id,
                                              int64_t generation, bool ready) {
    (void)zova_path;
    (void)workspace_id;
    (void)generation;
    (void)ready;
    return -1;
}

int cbm_zova_user_database_import_workspace(
    const char *zova_path, const char *root_path, const char *project, int64_t generation,
    const CBMDumpNode *nodes, int node_count, const CBMDumpEdge *edges, int edge_count) {
    (void)zova_path;
    (void)root_path;
    (void)project;
    (void)generation;
    (void)nodes;
    (void)node_count;
    (void)edges;
    (void)edge_count;
    return -1;
}

int cbm_zova_user_database_publish_workspace(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result) {
    (void)zova_path;
    (void)input;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    return -1;
}

int cbm_zova_user_database_publish_model(
    const char *zova_path, const cbm_zova_publish_model_t *model,
    cbm_zova_workspace_generation_result_t *out_result) {
    (void)zova_path;
    (void)model;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    return -1;
}

int cbm_zova_user_database_publish_delta(
    const char *zova_path, const cbm_zova_publish_model_t *after,
    const cbm_zova_workspace_delta_t *delta,
    cbm_zova_workspace_generation_result_t *out_result) {
    (void)zova_path;
    (void)after;
    (void)delta;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    return -1;
}

int cbm_zova_user_database_publish_workspace_at_generation(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    int64_t requested_generation, cbm_zova_workspace_generation_result_t *out_result) {
    (void)zova_path;
    (void)input;
    (void)requested_generation;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    return -1;
}

int cbm_zova_user_database_delete_workspace(const char *zova_path,
                                            const char *workspace_id) {
    (void)zova_path;
    (void)workspace_id;
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

int cbm_zova_vector_session_search_collection_i8(
    cbm_zova_vector_session_t *session, const char *collection, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_vector_hit_t **out, int *out_count) {
    (void)session; (void)collection; (void)query_values; (void)query_count;
    (void)vector_dim; (void)prefilter_limit; (void)limit;
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    return -1;
}

int cbm_zova_vector_session_get_workspace_token_i8(
    cbm_zova_vector_session_t *session, const char *workspace_id,
    const char *model_fingerprint, int vector_dim, const char *token,
    int8_t *out_values, size_t out_len, bool *out_found) {
    (void)session; (void)workspace_id; (void)model_fingerprint; (void)vector_dim;
    (void)token; (void)out_values; (void)out_len;
    if (out_found) *out_found = false;
    return -1;
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

void cbm_zova_graph_adjacency_free(cbm_zova_graph_adjacency_t *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; ++i) {
        free(items[i].source_node_id);
        free(items[i].target_node_id);
        free(items[i].edge_type);
        free(items[i].properties);
    }
    free(items);
}
int cbm_zova_graph_session_adjacency(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *node_id, const char *direction, const char *const *edge_types,
    int edge_type_count, int max_results, cbm_zova_graph_adjacency_t **out, int *out_count) {
    (void)session;(void)workspace_id;(void)graph_name;(void)node_id;(void)direction;
    (void)edge_types;(void)edge_type_count;(void)max_results;
    if(out)*out=NULL;if(out_count)*out_count=0;return -1;
}

int cbm_zova_graph_session_degree(
    cbm_zova_graph_session_t *session, const char *workspace_id, const char *graph_name,
    const char *node_id, const char *direction, const char *const *edge_types,
    int edge_type_count, int *out_degree) {
    (void)session;(void)workspace_id;(void)graph_name;(void)node_id;(void)direction;
    (void)edge_types;(void)edge_type_count;if(out_degree)*out_degree=0;return -1;
}
int cbm_zova_graph_session_adjacency_prepare_count(
    const cbm_zova_graph_session_t *session) {(void)session;return 0;}

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
        .flags = ZOVA_SQL_FUNCTION_DETERMINISTIC | ZOVA_SQL_FUNCTION_INNOCUOUS,
        .user_data = NULL,
        .callback = callback,
        .destroy = NULL,
    };
    zova_status status = zova_database_register_function(&req);
    /* Pinned Zova builds report duplicate registration as INVALID_ARGUMENT.
     * Treat that one case as success so callers can safely establish the
     * callback set on handles opened by another CBM helper. */
    if (status == ZOVA_INVALID_ARGUMENT) {
        return 0;
    }
    return status_ok(status, db, name);
}

int cbm_zova_register_sql_functions(zova_database *db) {
    if (!db) {
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

static int prepare_zova(zova_database *db, const char *sql, zova_statement **out,
                        const char *phase);
static int workspace_bind_text(zova_database *db, zova_statement *stmt, int index,
                               const char *value, const char *phase);
static int workspace_bind_i64(zova_database *db, zova_statement *stmt, int index,
                              int64_t value, const char *phase);
static char *column_text_owned(zova_database *db, zova_statement *stmt, int column);

void cbm_zova_graph_adjacency_free(cbm_zova_graph_adjacency_t *items, int count) {
    if (!items) return;
    for (int i=0;i<count;i++){free(items[i].source_node_id);free(items[i].target_node_id);
        free(items[i].edge_type);free(items[i].properties);} free(items);
}

static bool workspace_metadata_table_exists(zova_database *db, const char *table) {
    zova_statement *stmt = NULL;
    if (prepare_zova(db,
                     "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                     "name=?1 LIMIT 1",
                     &stmt, "workspace_metadata.probe") != 0)
        return false;
    if (workspace_bind_text(db, stmt, 1, table, "workspace_metadata.probe_bind") != 0) {
        (void)zova_statement_finalize(stmt);
        return false;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int rc = status_ok(zova_statement_step(&(zova_statement_step_request){
                           .statement = stmt, .out_result = &step}),
                       db, "workspace_metadata.probe_step");
    (void)zova_statement_finalize(stmt);
    return rc == 0 && step == ZOVA_STEP_ROW;
}

typedef struct {
    cbm_zova_graph_adjacency_t **items;
    int *count;
    int *capacity;
    int max_results;
    const char *source;
    const char *target;
    const char *edge_type;
    size_t edge_type_len;
} graph_adjacency_payload_context_t;

static int graph_adjacency_append_payload(
    const cbm_zova_edge_payload_record_t *record, void *context_ptr) {
    graph_adjacency_payload_context_t *context = context_ptr;
    if (!record || !context || !context->items || !context->count ||
        !context->capacity) return -1;
    if (*context->count >= context->max_results) return 0;
    if (*context->count == *context->capacity) {
        if (*context->capacity > INT_MAX / 2) return -1;
        int grown_capacity = *context->capacity * 2;
        void *grown = realloc(*context->items,
                              (size_t)grown_capacity * sizeof(**context->items));
        if (!grown) return -1;
        *context->items = grown;
        memset(*context->items + *context->capacity, 0,
               (size_t)(grown_capacity - *context->capacity) * sizeof(**context->items));
        *context->capacity = grown_capacity;
    }
    cbm_zova_graph_adjacency_t *item = &(*context->items)[*context->count];
    item->source_node_id = zv_strndup(context->source, strlen(context->source));
    item->target_node_id = zv_strndup(context->target, strlen(context->target));
    item->edge_type = zv_strndup(context->edge_type, context->edge_type_len);
    item->properties = zv_strndup((const char *)record->properties.data,
                                  record->properties.len);
    if (!item->source_node_id || !item->target_node_id ||
        !item->edge_type || !item->properties) {
        free(item->source_node_id);
        free(item->target_node_id);
        free(item->edge_type);
        free(item->properties);
        memset(item, 0, sizeof(*item));
        return -1;
    }
    (*context->count)++;
    return 0;
}

int cbm_zova_graph_session_adjacency(cbm_zova_graph_session_t *session,const char *workspace_id,
    const char *graph_name,const char *node_id,const char *direction,
    const char *const *edge_types,int edge_type_count,int max_results,
    cbm_zova_graph_adjacency_t **out,int *out_count){
    if(out)*out=NULL;if(out_count)*out_count=0;
    char expected[CBM_ZOVA_WORKSPACE_ID_MAX+32];
    if(!session||!workspace_name_component_valid(workspace_id)||
       cbm_zova_workspace_graph_name(workspace_id,expected,sizeof(expected))!=0||
       !graph_name||strcmp(graph_name,expected)!=0||!node_id||!node_id[0]||!direction||
       edge_type_count<0||(edge_type_count&& !edge_types)||max_results<=0||!out||!out_count)return -1;
    int dirs[2],nd=0;if(strcmp(direction,"outbound")==0||strcmp(direction,"both")==0)dirs[nd++]=ZOVA_GRAPH_NEIGHBOR_OUTGOING;
    if(strcmp(direction,"inbound")==0||strcmp(direction,"both")==0)dirs[nd++]=ZOVA_GRAPH_NEIGHBOR_INCOMING;
    if(!nd)return -1;int loops=edge_type_count?edge_type_count:1,cap=8,count=0,rc=0;
    cbm_zova_graph_adjacency_t *items=calloc((size_t)cap,sizeof(*items));if(!items)return -1;
    for(int d=0;rc==0&&d<nd;d++)for(int t=0;rc==0&&t<loops;t++){
        zova_graph_keyed_neighbor_results results={0};
        zova_graph_neighbors_keyed_request req={.db=session->zdb.db,.graph_name=graph_name,.node_id=node_id,
            .direction=dirs[d],.edge_type=edge_type_count?edge_types[t]:NULL,
            .limit=(size_t)(max_results-count),.out_results=&results};
        if(status_ok(zova_graph_neighbors_keyed(&req),session->zdb.db,"graph.adjacency")!=0)rc=-1;
        int64_t *edge_keys=results.len?calloc(results.len,sizeof(*edge_keys)):NULL;
        zova_graph_edge_payload_results payloads={0};
        if(rc==0&&results.len&&!edge_keys)rc=-1;
        for(size_t i=0;rc==0&&i<results.len;i++)edge_keys[i]=results.items[i].edge_key;
        if(rc==0&&results.len)rc=status_ok(zova_graph_edge_payload_get_many(
            &(zova_graph_edge_payload_get_many_request){.db=session->zdb.db,
                .graph_name=graph_name,.edge_keys=edge_keys,.key_count=results.len,
                .out_results=&payloads}),session->zdb.db,"graph.adjacency.payloads");
        if(rc==0&&payloads.len!=results.len)rc=-1;
        for(size_t i=0;rc==0&&i<results.len&&count<max_results;i++){
            const char *src=dirs[d]==ZOVA_GRAPH_NEIGHBOR_OUTGOING?node_id:results.items[i].node_id;
            const char *dst=dirs[d]==ZOVA_GRAPH_NEIGHBOR_OUTGOING?results.items[i].node_id:node_id;
            if(!payloads.items[i].found||payloads.items[i].edge_key!=results.items[i].edge_key){rc=-1;break;}
            graph_adjacency_payload_context_t context={.items=&items,.count=&count,
                .capacity=&cap,.max_results=max_results,.source=src,.target=dst,
                .edge_type=results.items[i].edge_type,
                .edge_type_len=results.items[i].edge_type_len};
            if(cbm_zova_edge_payload_visit(payloads.items[i].payload,
                    payloads.items[i].payload_len,graph_adjacency_append_payload,
                    &context,NULL)!=0)rc=-1;
        }
        free(edge_keys);zova_graph_edge_payload_results_free(&payloads);
        zova_graph_keyed_neighbor_results_free(&results);
    }
    if(rc){cbm_zova_graph_adjacency_free(items,count+1);return -1;}*out=items;*out_count=count;return 0;
}

int cbm_zova_graph_session_degree(cbm_zova_graph_session_t *session,const char *workspace_id,
    const char *graph_name,const char *node_id,const char *direction,
    const char *const *edge_types,int edge_type_count,int *out_degree){
    if(!out_degree)return -1;*out_degree=0;char expected[CBM_ZOVA_WORKSPACE_ID_MAX+32];
    if(!session||cbm_zova_workspace_graph_name(workspace_id,expected,sizeof(expected))!=0||
       !graph_name||strcmp(graph_name,expected)!=0||!node_id||!direction||edge_type_count<0||
       (edge_type_count&&!edge_types))return -1;
    int dirs[2],nd=0;if(strcmp(direction,"outbound")==0||strcmp(direction,"both")==0)dirs[nd++]=ZOVA_GRAPH_NEIGHBOR_OUTGOING;
    if(strcmp(direction,"inbound")==0||strcmp(direction,"both")==0)dirs[nd++]=ZOVA_GRAPH_NEIGHBOR_INCOMING;if(!nd)return -1;
    int loops=edge_type_count?edge_type_count:1;uint64_t total=0;
    for(int d=0;d<nd;d++)for(int t=0;t<loops;t++){uint64_t degree=0;
        zova_graph_degree_request req={.db=session->zdb.db,.graph_name=graph_name,.node_id=node_id,
            .direction=dirs[d],.edge_type=edge_type_count?edge_types[t]:NULL,.out_degree=&degree};
        if(status_ok(zova_graph_degree(&req),session->zdb.db,"graph.degree")!=0||degree>(uint64_t)INT_MAX-total)return -1;total+=degree;}
    *out_degree=(int)total;return 0;
}

int cbm_zova_graph_session_adjacency_prepare_count(
    const cbm_zova_graph_session_t *session) {
    (void)session;
    return 0;
}

cbm_zova_database_format_status_t cbm_zova_database_format_status(const char *path) {
    if (!path || !path[0]) return CBM_ZOVA_DATABASE_INCOMPATIBLE;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    cbm_zova_database_format_status_t result = CBM_ZOVA_DATABASE_INCOMPATIBLE;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
            SQLITE_OK ||
        sqlite3_exec(db, "PRAGMA query_only=ON;BEGIN", NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
                           "SELECT (SELECT value FROM _zova_meta WHERE key='format_version'),"
                           "(SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1)",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_type(stmt, 0) != SQLITE_TEXT ||
        sqlite3_column_type(stmt, 1) != SQLITE_INTEGER) {
        goto done;
    }
    const unsigned char *format = sqlite3_column_text(stmt, 0);
    int64_t schema = sqlite3_column_int64(stmt, 1);
    if (format && strcmp((const char *)format, "9") == 0 &&
        schema == CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
        result = CBM_ZOVA_DATABASE_COMPATIBLE;
    } else if (format && strcmp((const char *)format, "7") == 0 && schema == 5) {
        result = CBM_ZOVA_DATABASE_REPACK_REQUIRED;
    }
done:
    sqlite3_finalize(stmt);
    if (db) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
    }
    return result;
}

const char *cbm_zova_repack_phase_name(cbm_zova_repack_phase_t phase) {
    static const char *names[] = {
        "source_snapshot", "temp_creation", "workspace_publish", "verification", "fsync",
        "live_to_recovery_rename", "temp_to_live_rename", "final_reopen",
    };
    return phase >= 0 && phase < CBM_ZOVA_REPACK_PHASE_COUNT ? names[phase] : NULL;
}

static int open_zova(const char *path, bool read_only, cbm_zova_db_t *out) {
    if (!path || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (cbm_zova_database_format_status(path) == CBM_ZOVA_DATABASE_REPACK_REQUIRED) {
        cbm_log_error("zova.open", "path", path, "status", "repack_required");
        return -1;
    }
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
    user_publish_test_metrics.database_handle_open_count++;
    zova_message_free(&err);
    if (cbm_zova_register_sql_functions(out->db) != 0 ||
        zova_database_exec(&(zova_database_exec_request){
            .db = out->db, .sql = "PRAGMA foreign_keys=ON"}) != ZOVA_OK) {
        close_zova(out);
        return -1;
    }
    return 0;
}

/* Execute one capability probe without logging expected feature failures.
 * A successful statement with no rows is still a successful capability (for
 * example BM25 on an empty FTS table). */
static int zova_probe_sql(zova_database *db, const char *sql, bool *out_row) {
    if (out_row) {
        *out_row = false;
    }
    if (!db || !sql) {
        return -1;
    }
    zova_statement *stmt = NULL;
    zova_database_prepare_request prepare_req = {.db = db, .sql = sql, .out_statement = &stmt};
    if (zova_database_prepare(&prepare_req) != ZOVA_OK || !stmt) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
    int rc = zova_statement_step(&step_req) == ZOVA_OK ? 0 : -1;
    if (rc == 0 && out_row) {
        *out_row = step == ZOVA_STEP_ROW;
    }
    if (zova_statement_finalize(stmt) != ZOVA_OK) {
        rc = -1;
    }
    return rc;
}

int cbm_zova_probe_sql_capabilities(const char *zova_path,
                                    cbm_zova_sql_capabilities_t *out_capabilities) {
    if (!zova_path || !zova_path[0] || !out_capabilities) {
        return -1;
    }
    memset(out_capabilities, 0, sizeof(*out_capabilities));
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0) {
        return -1;
    }

    bool row = false;
    out_capabilities->canonical_workspace_metadata =
        zova_probe_sql(zdb.db,
                       "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                       "name='cbm_nodes_v1' LIMIT 1",
                       &row) == 0 &&
        row;

    /* Exercise the complete FTS surface on a disposable table instead of
     * relying on a converted sidecar containing a particular application
     * table.  This covers creation, insert, MATCH, update and delete. */
    const char *fts_setup =
        "DROP TABLE IF EXISTS cbm_zova_capability_fts_v1;"
        "CREATE VIRTUAL TABLE cbm_zova_capability_fts_v1 USING fts5("
        "workspace_id UNINDEXED,node_id UNINDEXED,name,qualified_name,file_path,label,"
        "tokenize='unicode61 remove_diacritics 2');"
        "INSERT INTO cbm_zova_capability_fts_v1"
        "(workspace_id,node_id,name,qualified_name,file_path,label) "
        "VALUES('w1','n1','AlphaBeta','pkg::AlphaBeta','src/a.c','Function');";
    zova_database_exec_request fts_req = {.db = zdb.db, .sql = fts_setup};
    zova_status fts_status = zova_database_exec(&fts_req);
    bool fts_surface = fts_status == ZOVA_OK;
    if (!fts_surface) {
        cbm_log_error("zova.capability", "feature", "fts5", "phase", "create_insert",
                      "status", zova_status_name(fts_status), "msg",
                      zova_database_last_error_message(zdb.db));
    }
    if (fts_surface) {
        fts_surface = zova_probe_sql(
                          zdb.db,
                          "SELECT rowid FROM cbm_zova_capability_fts_v1 "
                          "WHERE cbm_zova_capability_fts_v1 MATCH 'AlphaBeta' LIMIT 1",
                          &row) == 0 &&
                      row;
    }
    if (fts_surface) {
        const char *fts_update =
            "UPDATE cbm_zova_capability_fts_v1 SET name='BetaGamma' WHERE rowid=1;"
            "DELETE FROM cbm_zova_capability_fts_v1 WHERE rowid=1;";
        zova_database_exec_request update_req = {.db = zdb.db, .sql = fts_update};
        zova_status update_status = zova_database_exec(&update_req);
        fts_surface = update_status == ZOVA_OK &&
                      zova_probe_sql(zdb.db,
                                     "SELECT count(*) FROM cbm_zova_capability_fts_v1",
                                     &row) == 0 &&
                      row;
        if (!fts_surface && update_status != ZOVA_OK) {
            cbm_log_error("zova.capability", "feature", "fts5", "phase", "update_delete",
                          "status", zova_status_name(update_status), "msg",
                          zova_database_last_error_message(zdb.db));
        }
    }
    out_capabilities->fts5 = fts_surface;
    if (fts_surface) {
        zova_database_exec_request bm25_insert = {
            .db = zdb.db,
            .sql = "INSERT INTO cbm_zova_capability_fts_v1"
                   "(workspace_id,node_id,name,qualified_name,file_path,label) "
                   "VALUES('w1','n2','Alpha','pkg::Alpha','src/b.c','Function');"};
        fts_surface = zova_database_exec(&bm25_insert) == ZOVA_OK;
    }
    out_capabilities->bm25 = fts_surface &&
                             zova_probe_sql(
                                 zdb.db,
                                 "SELECT bm25(cbm_zova_capability_fts_v1) "
                                 "FROM cbm_zova_capability_fts_v1 "
                                 "WHERE cbm_zova_capability_fts_v1 MATCH 'Alpha' LIMIT 1",
                                 &row) == 0 &&
                             row;
    (void)zova_database_exec(&(zova_database_exec_request){
        .db = zdb.db, .sql = "DROP TABLE IF EXISTS cbm_zova_capability_fts_v1;"});

    out_capabilities->json =
        zova_probe_sql(zdb.db,
                       "SELECT json_extract('{\"workspace_id\":\"w1\",\"value\":1}', "
                       "'$.value') WHERE json_extract('{\"workspace_id\":\"w1\",\"value\":1}', "
                       "'$.workspace_id') = 'w1'",
                       &row) == 0 &&
        row;
    out_capabilities->recursive_cte =
        zova_probe_sql(zdb.db,
                       "WITH RECURSIVE n(workspace_id,x) AS (VALUES('w1',1) UNION ALL "
                       "SELECT workspace_id,x+1 FROM n WHERE workspace_id='w1' AND x<2) "
                       "SELECT count(*) FROM n WHERE workspace_id='w1'",
                       &row) == 0 &&
        row;

    const char *function_probes[] = {
        "SELECT cbm_camel_split('AlphaBeta')",
        "SELECT regexp('alpha', 'Alpha')",
        "SELECT iregexp('alpha', 'Alpha')",
        "SELECT cbm_cosine_i8(x'01', x'01')",
    };
    out_capabilities->cbm_scalar_functions = true;
    for (size_t i = 0; i < sizeof(function_probes) / sizeof(function_probes[0]); i++) {
        if (zova_probe_sql(zdb.db, function_probes[i], &row) != 0 || !row) {
            out_capabilities->cbm_scalar_functions = false;
            break;
        }
    }

    zova_database_exec_request transaction_req = {
        .db = zdb.db,
        .sql = "BEGIN; CREATE TEMP TABLE cbm_zova_capability_probe(x INTEGER); "
               "INSERT INTO cbm_zova_capability_probe VALUES(1); ROLLBACK;",
    };
    out_capabilities->transactions = zova_database_exec(&transaction_req) == ZOVA_OK;
    close_zova(&zdb);
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

static int configure_new_zova_database(zova_database *db) {
    zova_statement *stmt = NULL;
    int64_t page_size = 0;
    int rc = status_ok(zova_database_exec(&(zova_database_exec_request){
                           .db = db,
                           .sql = "PRAGMA journal_mode=WAL"}),
                       db, "zova.journal_mode_configure");
    if (rc == 0)
        rc = prepare_zova(db, "PRAGMA page_size", &stmt, "zova.page_size_prepare");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0)
        rc = status_ok(zova_statement_step(&(zova_statement_step_request){
                           .statement = stmt, .out_result = &step}),
                       db, "zova.page_size_step");
    if (rc == 0 && step == ZOVA_STEP_ROW)
        rc = status_ok(zova_statement_column_int64(
                           &(zova_statement_column_int64_request){
                               .statement = stmt, .index = 0, .out_value = &page_size}),
                       db, "zova.page_size_column");
    else if (rc == 0)
        rc = -1;
    if (stmt && zova_statement_finalize(stmt) != ZOVA_OK) rc = -1;
    return rc == 0 && page_size == 65536 ? 0 : -1;
}

static int open_or_create_zova(const char *path, cbm_zova_db_t *out) {
    bool exists = cbm_file_exists(path);
    if (exists) {
        return open_zova(path, false, out);
    }
    memset(out, 0, sizeof(*out));
    zova_message err = {0};
    zova_database_create_options_request req = {
        .path = path,
        .page_size = 65536,
        .out_db = &out->db,
        .out_error_message = &err,
    };
    zova_status status = zova_database_create_with_options(&req);
    if (status != ZOVA_OK) {
        cbm_log_error("zova.registry_create", "path", path, "status", zova_status_name(status),
                      "msg", err.data ? err.data : "");
        zova_message_free(&err);
        return -1;
    }
    user_publish_test_metrics.database_handle_open_count++;
    zova_message_free(&err);
    /* Zova selected 64 KiB pages before creating its private catalog. Keep the
     * new handle, enable WAL, and verify the effective page size in place. */
    if (configure_new_zova_database(out->db) != 0) {
        close_zova(out);
        cbm_unlink(path);
        return -1;
    }
    if (cbm_zova_register_sql_functions(out->db) != 0 ||
        zova_database_exec(&(zova_database_exec_request){
            .db = out->db, .sql = "PRAGMA foreign_keys=ON"}) != ZOVA_OK) {
        close_zova(out);
        cbm_unlink(path);
        return -1;
    }
    return 0;
}

static int workspace_registry_init(zova_database *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS cbm_workspace_registry("
        "workspace_key INTEGER PRIMARY KEY, workspace_id TEXT NOT NULL UNIQUE, "
        "canonical_root TEXT NOT NULL UNIQUE, "
        "id_format_version INTEGER NOT NULL, active_generation INTEGER NOT NULL DEFAULT 0);";
    zova_database_exec_request req = {.db = db, .sql = sql};
    return status_ok(zova_database_exec(&req), db, "workspace.registry_init");
}

static int workspace_legacy_generation_init(zova_database *db) {
    /* The normal single-file schema uses cbm_database_generation_v1. Keep this
     * table out of fresh databases unless an old sidecar coordination API is
     * explicitly invoked. */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS cbm_workspace_generations("
        "workspace_key INTEGER NOT NULL, generation INTEGER NOT NULL, state TEXT NOT NULL "
        "CHECK(state IN ('building','ready','failed','retired')), "
        "PRIMARY KEY(workspace_key,generation), "
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE) WITHOUT ROWID;";
    zova_database_exec_request req = {.db = db, .sql = sql};
    return status_ok(zova_database_exec(&req), db, "workspace.legacy_generation_init");
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

/* These bind/step helpers are defined below the workspace registry code; the
 * user-database generation API is kept beside the schema bootstrap for the
 * migration boundary, so declare them before using them here. */
static int workspace_bind_text(zova_database *db, zova_statement *stmt, int index,
                               const char *value, const char *phase);
static int workspace_bind_i64(zova_database *db, zova_statement *stmt, int index, int64_t value,
                              const char *phase);
static int workspace_step_done(zova_database *db, zova_statement *stmt, const char *phase);

static int user_schema_scalar(zova_database *db, const char *sql, int64_t *out_value) {
    zova_statement *stmt = NULL;
    if (!db || !sql || !out_value ||
        prepare_zova(db, sql, &stmt, "user_database.schema_probe") != 0) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int rc = zova_statement_step(&(zova_statement_step_request){
                 .statement = stmt, .out_result = &step}) == ZOVA_OK &&
                     step == ZOVA_STEP_ROW &&
                     zova_statement_column_int64(&(zova_statement_column_int64_request){
                         .statement = stmt, .index = 0, .out_value = out_value}) == ZOVA_OK
                 ? 0
                 : -1;
    (void)zova_statement_finalize(stmt);
    return rc;
}

static int user_schema_v3_is_well_formed(zova_database *db) {
    int64_t table_count = 0;
    const char *sql =
        "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') AND name IN ("
        "'cbm_workspace_registry','cbm_database_generation_v1',"
        "'cbm_projects_v1','cbm_files_v1','cbm_nodes_v1','cbm_file_hashes_v1',"
        "'cbm_token_vector_metadata_v1','cbm_nodes_fts_v1',"
        "'cbm_workspace_index_state_v1','cbm_project_summaries_v2',"
        "'cbm_generation_integrity_v2')";
    return user_schema_scalar(db, sql, &table_count) == 0 && table_count == 11 ? 0 : -1;
}

static int user_schema_v4_is_well_formed(zova_database *db) {
    int64_t columns = 0;
    return user_schema_v3_is_well_formed(db) == 0 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM pragma_table_info('cbm_workspace_migrations_v1')",
                       &columns) == 0 &&
                   columns == 17
               ? 0
               : -1;
}

int cbm_zova_user_database_schema_is_current(zova_database *db) {
    int64_t columns = 0;
    int64_t exact_columns = 0;
    int64_t foreign_keys = 0;
    int64_t state_constraint = 0;
    int64_t token_columns = 0;
    int64_t forbidden_objects = 0;
    int64_t compact_foreign_keys = 0;
    int64_t compact_child_indexes = 0;
    int64_t guard_triggers = 0;
    int64_t topology_columns = 0;
    return user_schema_v4_is_well_formed(db) == 0 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM pragma_table_info('cbm_workspace_health_v1')",
                       &columns) == 0 &&
                   columns == 5 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM pragma_table_info('cbm_workspace_health_v1') "
                       "WHERE (cid=0 AND name='workspace_key' AND type='INTEGER' AND pk=1 AND "
                       "dflt_value IS NULL) OR "
                       "(cid=1 AND name='state' AND type='TEXT' AND \"notnull\"=1 AND pk=0 AND "
                       "dflt_value IS NULL) OR "
                       "(cid=2 AND name='reason' AND type='TEXT' AND \"notnull\"=1 AND pk=0 AND "
                       "dflt_value IS NULL) OR "
                       "(cid=3 AND name='checked_generation' AND type='INTEGER' AND \"notnull\"=1 "
                       "AND pk=0 AND dflt_value IS NULL) OR "
                       "(cid=4 AND name='checked_at' AND type='TEXT' AND \"notnull\"=1 AND pk=0 AND "
                       "dflt_value IS NULL)",
                       &exact_columns) == 0 &&
                   exact_columns == 5 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM pragma_foreign_key_list("
                       "'cbm_workspace_health_v1') WHERE \"table\"='cbm_workspace_registry' "
                       "AND \"from\"='workspace_key' AND \"to\"='workspace_key'",
                       &foreign_keys) == 0 &&
                   foreign_keys == 1 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                       "name='cbm_workspace_health_v1' AND "
                       "instr(sql,\"state IN ('healthy','rebuild_required')\") > 0",
                       &state_constraint) == 0 &&
                   state_constraint == 1 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM pragma_table_info("
                       "'cbm_token_vector_metadata_v1') WHERE "
                       "(cid=0 AND name='token_key' AND type='INTEGER' AND pk=1) OR "
                       "(cid=1 AND name='workspace_key' AND type='INTEGER' AND \"notnull\"=1) OR "
                       "(cid=2 AND name='token' AND type='TEXT' AND \"notnull\"=1) OR "
                       "(cid=3 AND name='idf' AND type='REAL' AND \"notnull\"=1)",
                       &token_columns) == 0 &&
                   token_columns == 4 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM ("
                       "SELECT 1 FROM pragma_table_info('cbm_nodes_v1') WHERE "
                       "(cid=0 AND name='zova_node_key' AND type='INTEGER' AND pk=1) OR "
                       "(cid=1 AND name='workspace_key' AND type='INTEGER' AND \"notnull\"=1) OR "
                       "(cid=2 AND name='label' AND type='TEXT' AND \"notnull\"=1) OR "
                       "(cid=3 AND name='name' AND type='TEXT' AND \"notnull\"=1) OR "
                       "(cid=4 AND name='qualified_name' AND type='TEXT' AND \"notnull\"=1) OR "
                       "(cid=5 AND name='file_key' AND type='INTEGER' AND \"notnull\"=1) OR "
                       "(cid=6 AND name='start_line' AND type='INTEGER' AND \"notnull\"=1) OR "
                       "(cid=7 AND name='end_line' AND type='INTEGER' AND \"notnull\"=1) OR "
                       "(cid=8 AND name='properties' AND type='TEXT' AND \"notnull\"=1) OR "
                       "(cid=9 AND name='source_ordinal' AND type='INTEGER' AND \"notnull\"=1))",
                       &topology_columns) == 0 &&
                   topology_columns == 10 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM ("
                       "SELECT 1 FROM pragma_foreign_key_list('cbm_nodes_v1') "
                       "WHERE \"table\"='cbm_files_v1' AND ((seq=0 AND \"from\"='workspace_key' "
                       "AND \"to\"='workspace_key') OR (seq=1 AND \"from\"='file_key' "
                       "AND \"to\"='file_key')))",
                       &compact_foreign_keys) == 0 &&
                   compact_foreign_keys == 2 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM sqlite_master WHERE type='index' AND "
                       "name='cbm_nodes_v1_workspace_file'",
                       &compact_child_indexes) == 0 &&
                   compact_child_indexes == 1 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
                       "'cbm_nodes_v1_file_workspace_bi','cbm_nodes_v1_file_workspace_bu',"
                       "'cbm_edges_v1_endpoint_workspace_bi',"
                       "'cbm_edges_v1_endpoint_workspace_bu')",
                       &guard_triggers) == 0 &&
                   guard_triggers == 0 &&
                   user_schema_scalar(
                       db,
                       "SELECT count(*) FROM sqlite_master WHERE "
                       "name IN ('cbm_edges_v1','cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1',"
                       "'cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
                       "'cbm_token_vectors_compat_v1','_zova_vector_norms') "
                       "OR name GLOB 'cbm_fts_w1_*'",
                       &forbidden_objects) == 0 &&
                   forbidden_objects == 0
               ? 0
               : -1;
}

static int user_database_ensure_schema(zova_database *db) {
    if (!db) return -1;
    int64_t schema_table_count = 0;
    int64_t schema_version = 0;
    if (user_schema_scalar(
            db,
            "SELECT count(*) FROM sqlite_master WHERE type='table' "
            "AND name='cbm_database_schema_v1'",
            &schema_table_count) != 0 ||
        (schema_table_count != 0 && schema_table_count != 1)) {
        return -1;
    }
    if (schema_table_count == 1 &&
        (user_schema_scalar(db,
                            "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
                            &schema_version) != 0 ||
         schema_version < 1 || schema_version > CBM_ZOVA_DATABASE_SCHEMA_VERSION)) {
        return -1;
    }
    if (schema_version == CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
        return cbm_zova_user_database_schema_is_current(db);
    }
    /* Every pre-v7 file is immutable repack input. Never run partial in-place
     * DDL against a live v5/Zova-v7 database. */
    if (schema_table_count == 1) {
        return -1;
    }
    if (workspace_registry_begin(db) != 0 || workspace_registry_init(db) != 0) {
        workspace_registry_rollback(db);
        return -1;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS cbm_database_schema_v1("
        "id INTEGER PRIMARY KEY CHECK(id = 1),"
        "schema_version INTEGER NOT NULL,"
        "metadata_projection_version INTEGER NOT NULL,"
        "edge_metadata_projection_version INTEGER NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO cbm_database_schema_v1"
        "(id,schema_version,metadata_projection_version,edge_metadata_projection_version)"
        "VALUES(1,7,0,0);"
        "CREATE TABLE IF NOT EXISTS cbm_database_generation_v1("
        "workspace_key INTEGER NOT NULL, generation INTEGER NOT NULL,"
        "state TEXT NOT NULL CHECK(state IN ('building','ready','failed','retired')),"
        "PRIMARY KEY(workspace_key,generation),"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS cbm_projects_v1("
        "workspace_key INTEGER PRIMARY KEY, project TEXT NOT NULL, root_path TEXT NOT NULL,"
        "indexed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS cbm_files_v1("
        "file_key INTEGER PRIMARY KEY, workspace_key INTEGER NOT NULL, file_path TEXT NOT NULL,"
        "UNIQUE(workspace_key,file_path),UNIQUE(workspace_key,file_key),"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS cbm_nodes_v1("
        "zova_node_key INTEGER PRIMARY KEY, workspace_key INTEGER NOT NULL,"
        "label TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL,"
        "file_key INTEGER NOT NULL, start_line INTEGER NOT NULL, end_line INTEGER NOT NULL,"
        "properties TEXT NOT NULL DEFAULT '{}',source_ordinal INTEGER NOT NULL DEFAULT 0 "
        "CHECK(source_ordinal>=0),"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE,"
        "FOREIGN KEY(workspace_key,file_key) "
        "REFERENCES cbm_files_v1(workspace_key,file_key));"
        "CREATE INDEX IF NOT EXISTS cbm_nodes_v1_workspace_file "
        "ON cbm_nodes_v1(workspace_key,file_key);"
        "CREATE TABLE IF NOT EXISTS cbm_file_hashes_v1("
        "file_key INTEGER PRIMARY KEY, content_hash TEXT NOT NULL,"
        "mtime_ns INTEGER NOT NULL DEFAULT 0, size_bytes INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(file_key) REFERENCES cbm_files_v1(file_key) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS cbm_token_vector_metadata_v1("
        "token_key INTEGER PRIMARY KEY, workspace_key INTEGER NOT NULL,"
        "token TEXT NOT NULL, idf REAL NOT NULL, UNIQUE(workspace_key,token),"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE);"
        "CREATE VIEW IF NOT EXISTS cbm_nodes_fts_content_v1 AS "
        "SELECT n.zova_node_key,cbm_camel_split(n.name) AS name,n.qualified_name,f.file_path,n.label "
        "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key;"
        "CREATE VIRTUAL TABLE IF NOT EXISTS cbm_nodes_fts_v1 USING fts5("
        "name,qualified_name,file_path,label,content='cbm_nodes_fts_content_v1',"
        "content_rowid='zova_node_key');"
        CBM_NODE_FTS_INSERT_TRIGGER_SQL
        "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_bd BEFORE DELETE ON cbm_nodes_v1 BEGIN "
        "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1,rowid,name,qualified_name,file_path,label) "
        "SELECT 'delete',OLD.zova_node_key,cbm_camel_split(OLD.name),OLD.qualified_name,f.file_path,OLD.label "
        "FROM cbm_files_v1 f WHERE f.file_key=OLD.file_key; END;"
        "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_bu "
        "BEFORE UPDATE OF name,qualified_name,file_key,label ON cbm_nodes_v1 BEGIN "
        "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1,rowid,name,qualified_name,file_path,label) "
        "SELECT 'delete',OLD.zova_node_key,cbm_camel_split(OLD.name),OLD.qualified_name,f.file_path,OLD.label "
        "FROM cbm_files_v1 f WHERE f.file_key=OLD.file_key; END;"
        "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_au "
        "AFTER UPDATE OF name,qualified_name,file_key,label ON cbm_nodes_v1 BEGIN "
        "INSERT INTO cbm_nodes_fts_v1(rowid,name,qualified_name,file_path,label) "
        "SELECT NEW.zova_node_key,cbm_camel_split(NEW.name),NEW.qualified_name,f.file_path,NEW.label "
        "FROM cbm_files_v1 f WHERE f.file_key=NEW.file_key; END;"
        "CREATE TABLE IF NOT EXISTS cbm_workspace_index_state_v1("
        "workspace_key INTEGER PRIMARY KEY, generation INTEGER NOT NULL,"
        "model_fingerprint TEXT NOT NULL, vector_dimensions INTEGER NOT NULL,"
        "indexed_at TEXT NOT NULL,"
        "FOREIGN KEY(workspace_key,generation) REFERENCES cbm_database_generation_v1(workspace_key,generation) "
        "ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS cbm_project_summaries_v2("
        "workspace_key INTEGER PRIMARY KEY, summary TEXT NOT NULL, source_hash TEXT NOT NULL,"
        "created_at TEXT NOT NULL, updated_at TEXT NOT NULL,"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS cbm_generation_integrity_v2("
        "workspace_key INTEGER NOT NULL, generation INTEGER NOT NULL,"
        "graph_nodes INTEGER NOT NULL, graph_edges INTEGER NOT NULL,"
        "metadata_nodes INTEGER NOT NULL, metadata_edges INTEGER NOT NULL,"
        "metadata_topology_edges INTEGER NOT NULL, fts_rows INTEGER NOT NULL,"
        "node_vector_rows INTEGER NOT NULL, token_vector_rows INTEGER NOT NULL,"
        "node_vectors INTEGER NOT NULL, token_vectors INTEGER NOT NULL,"
        "metadata_sha256 TEXT NOT NULL, fts_sha256 TEXT NOT NULL,"
        "topology_sha256 TEXT NOT NULL, node_vector_sha256 TEXT NOT NULL,"
        "token_vector_sha256 TEXT NOT NULL,"
        "PRIMARY KEY(workspace_key,generation),"
        "FOREIGN KEY(workspace_key,generation) REFERENCES cbm_database_generation_v1(workspace_key,generation) "
        "ON DELETE CASCADE) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS cbm_workspace_migrations_v1("
        "workspace_key INTEGER PRIMARY KEY,"
        "migration_version INTEGER NOT NULL CHECK(migration_version=1),"
        "project TEXT NOT NULL,root_path TEXT NOT NULL,"
        "source_db_path TEXT NOT NULL,source_zova_path TEXT NOT NULL,"
        "source_generation INTEGER NOT NULL,target_generation INTEGER NOT NULL DEFAULT 0,"
        "state TEXT NOT NULL CHECK(state IN ('prepared','copying','active','failed',"
        "'rolled_back','cleanup_pending','retired')),"
        "metadata_sha256 TEXT NOT NULL,fts_sha256 TEXT NOT NULL,"
        "topology_sha256 TEXT NOT NULL,node_vector_sha256 TEXT NOT NULL,"
        "token_vector_sha256 TEXT NOT NULL,prepared_at TEXT NOT NULL,"
        "activated_at TEXT,retired_at TEXT,"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS cbm_workspace_health_v1("
        "workspace_key INTEGER PRIMARY KEY,"
        "state TEXT NOT NULL CHECK(state IN ('healthy','rebuild_required')),"
        "reason TEXT NOT NULL,checked_generation INTEGER NOT NULL,checked_at TEXT NOT NULL,"
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) "
        "ON DELETE CASCADE);";

    zova_database_exec_request req = {.db = db, .sql = sql};
    int rc = status_ok(zova_database_exec(&req), db, "user_database.schema_init");
    const char *fault = getenv("CBM_ZOVA_TEST_FAIL_PHASE");
    if (rc == 0 && fault && strcmp(fault, "schema_v6_before_commit") == 0) {
        rc = -1;
    }
    if (rc == 0) {
        rc = workspace_registry_commit(db);
    }
    if (rc != 0) {
        workspace_registry_rollback(db);
    }
    return rc;
}

/* Bootstrap the SQL portion of the user-local cbm.zova database. Publication
 * callers reuse user_database_ensure_schema() on their existing handle. */
int cbm_zova_user_database_init(const char *zova_path) {
    if (!zova_path || !zova_path[0]) return -1;
    cbm_zova_db_t zdb;
    if (open_or_create_zova(zova_path, &zdb) != 0) return -1;
    user_publish_test_metrics.database_open_count++;
    int rc = user_database_ensure_schema(zdb.db);
    close_zova(&zdb);
    user_publish_test_metrics.database_close_count++;
    return rc;
}

int cbm_zova_user_database_verify_workspace_schema(zova_database *db) {
    if (!db) return -1;
    const struct { const char *sql; int64_t expected; } checks[] = {
        {"PRAGMA foreign_keys", 1},
        {"SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
         CBM_ZOVA_DATABASE_SCHEMA_VERSION},
        {"SELECT count(*) FROM pragma_foreign_key_list('cbm_nodes_v1') "
         "WHERE \"table\"='cbm_files_v1'", 2},
    };
    for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
        zova_statement *stmt = NULL;
        if (prepare_zova(db, checks[i].sql, &stmt, "workspace_schema.verify_prepare") != 0)
            return -1;
        zova_step_result step = ZOVA_STEP_DONE;
        int64_t value = 0;
        int rc = zova_statement_step(&(zova_statement_step_request){
                     .statement=stmt,.out_result=&step}) == ZOVA_OK && step == ZOVA_STEP_ROW &&
                 zova_statement_column_int64(&(zova_statement_column_int64_request){
                     .statement=stmt,.index=0,.out_value=&value}) == ZOVA_OK &&
                 value == checks[i].expected ? 0 : -1;
        (void)zova_statement_finalize(stmt);
        if (rc != 0) return -1;
    }
    bool violation = false;
    return zova_probe_sql(db, "PRAGMA foreign_key_check", &violation) == 0 && !violation ? 0 : -1;
}

int cbm_zova_user_database_generation_begin(const char *zova_path, const char *root_path,
                                             int64_t generation, char *out_workspace_id,
                                             size_t out_workspace_id_size) {
    if (!zova_path || !zova_path[0] || !root_path || !root_path[0] || generation <= 0 ||
        !out_workspace_id || out_workspace_id_size < CBM_ZOVA_WORKSPACE_ID_MAX) {
        return -1;
    }
    out_workspace_id[0] = '\0';
    if (cbm_zova_user_database_init(zova_path) != 0 ||
        cbm_zova_workspace_get_or_create_at(zova_path, root_path, out_workspace_id,
                                             out_workspace_id_size) != 0) {
        return -1;
    }

    cbm_zova_db_t zdb;
    if (open_or_create_zova(zova_path, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        zdb.db,
        "INSERT INTO cbm_database_generation_v1(workspace_key,generation,state) "
        "SELECT workspace_key,?2,'building' FROM cbm_workspace_registry WHERE workspace_id=?1 "
        "ON CONFLICT(workspace_key,generation) DO UPDATE SET state='building'",
        &stmt, "user_generation.begin_prepare");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 1, out_workspace_id,
                                  "user_generation.begin_bind_workspace");
    if (rc == 0)
        rc = workspace_bind_i64(zdb.db, stmt, 2, generation,
                                "user_generation.begin_bind_generation");
    if (rc == 0)
        rc = workspace_step_done(zdb.db, stmt, "user_generation.begin_step");
    if (stmt)
        (void)zova_statement_finalize(stmt);
    if (rc == 0)
        rc = workspace_registry_commit(zdb.db);
    if (rc != 0)
        workspace_registry_rollback(zdb.db);
    close_zova(&zdb);
    if (rc != 0)
        out_workspace_id[0] = '\0';
    return rc;
}

int cbm_zova_user_database_generation_finish(const char *zova_path, const char *workspace_id,
                                              int64_t generation, bool ready) {
    if (!zova_path || !zova_path[0] || !workspace_name_component_valid(workspace_id) ||
        generation <= 0) {
        return -1;
    }
    cbm_zova_db_t zdb;
    if (open_or_create_zova(zova_path, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }

    int rc = -1;
    zova_statement *stmt = NULL;
    rc = prepare_zova(zdb.db,
                      "SELECT state FROM cbm_database_generation_v1 "
                      "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                      "WHERE workspace_id=?1) AND generation=?2",
                      &stmt, "user_generation.finish_lookup_prepare");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 1, workspace_id,
                                  "user_generation.finish_lookup_bind_workspace");
    if (rc == 0)
        rc = workspace_bind_i64(zdb.db, stmt, 2, generation,
                                "user_generation.finish_lookup_bind_generation");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), zdb.db, "user_generation.finish_lookup_step");
    }
    char state[16] = {0};
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_text text = {0};
        zova_statement_column_text_request req = {
            .statement = stmt, .index = 0, .out_text = &text};
        rc = status_ok(zova_statement_column_text(&req), zdb.db,
                       "user_generation.finish_lookup_column");
        if (rc == 0 && text.data && text.len < sizeof(state)) {
            memcpy(state, text.data, text.len);
            state[text.len] = '\0';
        } else if (rc == 0) {
            rc = -1;
        }
        zova_text_free(&text);
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt)
        (void)zova_statement_finalize(stmt);
    stmt = NULL;
    if (rc == 0 && strcmp(state, "building") != 0)
        rc = -1;

    const char *next_state = ready ? "ready" : "failed";
    if (rc == 0)
        rc = prepare_zova(zdb.db,
                          "UPDATE cbm_database_generation_v1 SET state=?1 "
                          "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                          "WHERE workspace_id=?2) AND generation=?3",
                          &stmt, "user_generation.finish_update_prepare");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 1, next_state,
                                  "user_generation.finish_update_bind_state");
    if (rc == 0)
        rc = workspace_bind_text(zdb.db, stmt, 2, workspace_id,
                                  "user_generation.finish_update_bind_workspace");
    if (rc == 0)
        rc = workspace_bind_i64(zdb.db, stmt, 3, generation,
                                "user_generation.finish_update_bind_generation");
    if (rc == 0)
        rc = workspace_step_done(zdb.db, stmt, "user_generation.finish_update_step");
    if (stmt)
        (void)zova_statement_finalize(stmt);
    stmt = NULL;
    if (rc == 0 && ready)
        rc = prepare_zova(zdb.db,
                          "UPDATE cbm_workspace_registry SET active_generation=?1 "
                          "WHERE workspace_id=?2",
                          &stmt, "user_generation.finish_active_prepare");
    if (rc == 0 && ready)
        rc = workspace_bind_i64(zdb.db, stmt, 1, generation,
                                "user_generation.finish_active_bind_generation");
    if (rc == 0 && ready)
        rc = workspace_bind_text(zdb.db, stmt, 2, workspace_id,
                                 "user_generation.finish_active_bind_workspace");
    if (rc == 0 && ready)
        rc = workspace_step_done(zdb.db, stmt, "user_generation.finish_active_step");
    if (stmt)
        (void)zova_statement_finalize(stmt);
    if (rc == 0)
        rc = workspace_registry_commit(zdb.db);
    if (rc != 0)
        workspace_registry_rollback(zdb.db);
    close_zova(&zdb);
    return rc;
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

static int workspace_key_for_id(zova_database *db, const char *workspace_id,
                                int64_t *out_workspace_key) {
    if (!db || !workspace_id || !workspace_id[0] || !out_workspace_key) return -1;
    *out_workspace_key = 0;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1",
                          &stmt, "workspace.key_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id, "workspace.key_bind");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0)
        rc = status_ok(zova_statement_step(&(zova_statement_step_request){
                           .statement = stmt, .out_result = &step}),
                       db, "workspace.key_step");
    if (rc == 0 && step == ZOVA_STEP_ROW)
        rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                           .statement = stmt, .index = 0, .out_value = out_workspace_key}),
                       db, "workspace.key_column");
    else if (rc == 0)
        rc = -1;
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc == 0 && *out_workspace_key > 0 ? 0 : -1;
}

static int next_private_key(zova_database *db, const char *sql, int64_t *out_key,
                            const char *phase) {
    if (!db || !sql || !out_key) return -1;
    zova_statement *stmt = NULL;
    *out_key = 0;
    int rc = prepare_zova(db, sql, &stmt, phase);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0)
        rc = status_ok(zova_statement_step(&(zova_statement_step_request){
                           .statement = stmt, .out_result = &step}),
                       db, phase);
    if (rc == 0 && step == ZOVA_STEP_ROW)
        rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                           .statement = stmt, .index = 0, .out_value = out_key}),
                       db, phase);
    else if (rc == 0)
        rc = -1;
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc == 0 && *out_key > 0 ? 0 : -1;
}

static int file_key_get_or_create(zova_database *db, int64_t workspace_key,
                                  const char *file_path, int64_t *out_file_key) {
    if (!db || workspace_key <= 0 || !file_path || !out_file_key) return -1;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "INSERT INTO cbm_files_v1(workspace_key,file_path) VALUES(?1,?2) "
                          "ON CONFLICT(workspace_key,file_path) DO NOTHING",
                          &stmt, "files.insert_prepare");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 1, workspace_key, "files.insert_workspace");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 2, file_path, "files.insert_path");
    if (rc == 0) rc = workspace_step_done(db, stmt, "files.insert_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    stmt = NULL;
    if (rc == 0)
        rc = prepare_zova(db,
                          "SELECT file_key FROM cbm_files_v1 WHERE workspace_key=?1 AND file_path=?2",
                          &stmt, "files.lookup_prepare");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 1, workspace_key, "files.lookup_workspace");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 2, file_path, "files.lookup_path");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0)
        rc = status_ok(zova_statement_step(&(zova_statement_step_request){
                           .statement = stmt, .out_result = &step}),
                       db, "files.lookup_step");
    if (rc == 0 && step == ZOVA_STEP_ROW)
        rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                           .statement = stmt, .index = 0, .out_value = out_file_key}),
                       db, "files.lookup_column");
    else if (rc == 0)
        rc = -1;
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc == 0 && *out_file_key > 0 ? 0 : -1;
}

static int workspace_query_state(zova_database *db, const char *workspace_id, int64_t generation,
                                 char *out_state, size_t out_state_size) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "SELECT state FROM cbm_workspace_generations "
                          "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                          "WHERE workspace_id=?1) AND generation=?2",
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

static int node_key_for_id(zova_database *db, int64_t workspace_key,
                           const char *node_id, int64_t *out_node_key) {
    if (!db || workspace_key <= 0 || !node_id || !node_id[0] || !out_node_key) return -1;
    *out_node_key = 0;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, "SELECT workspace_id FROM cbm_workspace_registry "
                              "WHERE workspace_key=?1", &stmt, "nodes.workspace_prepare");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 1, workspace_key,
                                          "nodes.workspace_bind");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && (status_ok(zova_statement_step(&(zova_statement_step_request){
            .statement = stmt, .out_result = &step}), db, "nodes.workspace_step") != 0 ||
                    step != ZOVA_STEP_ROW)) rc = -1;
    char *workspace_id = rc == 0 ? column_text_owned(db, stmt, 0) : NULL;
    if (stmt) (void)zova_statement_finalize(stmt);
    if (!workspace_id) return -1;
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) rc = -1;
    free(workspace_id);
    zova_graph_scan_cursor cursor = {0};
    while (rc == 0) {
        zova_graph_scan_results page = {0};
        if (status_ok(zova_graph_scan(&(zova_graph_scan_request){
                .db = db, .graph_name = graph_name, .node_after = cursor,
                .node_limit = 1024, .edge_limit = 0, .out_results = &page}),
                db, "nodes.scan") != 0) return -1;
        for (size_t i = 0; i < page.nodes_len; i++) {
            if (page.nodes[i].node_id && strcmp(page.nodes[i].node_id, node_id) == 0) {
                *out_node_key = page.nodes[i].node_key;
                zova_graph_scan_results_free(&page);
                return *out_node_key > 0 ? 0 : -1;
            }
        }
        if (page.nodes_len) {
            zova_graph_scan_node *last = &page.nodes[page.nodes_len - 1];
            cursor = (zova_graph_scan_cursor){.created_order = last->created_order,
                                              .key = last->node_key};
        }
        uint8_t more = page.has_more_nodes;
        zova_graph_scan_results_free(&page);
        if (!more) break;
    }
    return -1;
}

static int edge_key_for_identity(zova_database *db, const char *workspace_id,
                                 const char *source_id, const char *edge_type,
                                 const char *target_id, int64_t *out_edge_key) {
    if (!db || !workspace_id || !source_id || !edge_type || !target_id || !out_edge_key)
        return -1;
    *out_edge_key = 0;
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0)
        return -1;
    zova_graph_keyed_neighbor_results neighbors = {0};
    int rc = status_ok(zova_graph_neighbors_keyed(&(zova_graph_neighbors_keyed_request){
            .db = db, .graph_name = graph_name, .node_id = source_id,
            .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING, .edge_type = edge_type,
            .limit = (size_t)INT64_MAX, .out_results = &neighbors}), db,
            "edges.keyed_neighbors");
    for (size_t i = 0; rc == 0 && i < neighbors.len; i++) {
        if (neighbors.items[i].node_id && strcmp(neighbors.items[i].node_id, target_id) == 0) {
            *out_edge_key = neighbors.items[i].edge_key;
            break;
        }
    }
    zova_graph_keyed_neighbor_results_free(&neighbors);
    return rc == 0 && *out_edge_key > 0 ? 0 : -1;
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
    if (open_or_create_zova(registry_path, &zdb) != 0 || workspace_registry_init(zdb.db) != 0 ||
        workspace_legacy_generation_init(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "SELECT COALESCE(MAX(generation), 0) + 1 "
                          "FROM cbm_workspace_generations WHERE workspace_key=(SELECT workspace_key "
                          "FROM cbm_workspace_registry WHERE workspace_id=?1)",
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

static int zova_query_i64(zova_database *db, const char *sql, const char *bind_value,
                          int64_t *out_value, const char *phase);

static int sidecar_generation_read_db(zova_database *db, int64_t *out_generation) {
    if (!db || !out_generation) {
        return -1;
    }
    *out_generation = 0;
    zova_statement *stmt = NULL;
    int64_t has_sidecar_table = 0;
    int rc = zova_query_i64(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='"
        CBM_ZOVA_SIDECAR_GENERATION_TABLE "'", NULL, &has_sidecar_table,
        "generation.table_probe");
    const char *generation_sql = has_sidecar_table > 0
        ? "SELECT generation FROM " CBM_ZOVA_SIDECAR_GENERATION_TABLE " WHERE id = 1"
        : "SELECT MAX(generation) FROM cbm_database_generation_v1 WHERE state='ready'";
    if (rc == 0) {
        rc = prepare_zova(db, generation_sql, &stmt, "sidecar_generation.prepare");
    }
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

static int zova_query_i64(zova_database *db, const char *sql, const char *bind_value,
                          int64_t *out_value, const char *phase) {
    if (!db || !sql || !out_value) {
        return -1;
    }
    *out_value = 0;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, phase);
    if (rc == 0 && bind_value) {
        rc = workspace_bind_text(db, stmt, 1, bind_value, phase);
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request req = {.statement = stmt, .out_result = &step};
        rc = status_ok(zova_statement_step(&req), db, phase);
    }
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        zova_statement_column_int64_request req = {
            .statement = stmt, .index = 0, .out_value = out_value};
        rc = status_ok(zova_statement_column_int64(&req), db, phase);
    } else if (rc == 0) {
        rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int sidecar_schema_and_integrity_record(const char *zova_path, int64_t generation,
                                                const char *workspace_id, const char *project,
                                                bool workspace_scoped) {
    cbm_zova_db_t zdb;
    if (open_zova(zova_path, false, &zdb) != 0) {
        return -1;
    }
    const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS cbm_zova_schema_v1("
        "id INTEGER PRIMARY KEY CHECK(id=1), schema_version INTEGER NOT NULL, "
        "metadata_projection_version INTEGER NOT NULL, edge_metadata_projection_version INTEGER NOT NULL);"
        "INSERT OR REPLACE INTO cbm_zova_schema_v1"
        "(id,schema_version,metadata_projection_version,edge_metadata_projection_version)"
        "VALUES(1,1,1,1);"
        "CREATE TABLE IF NOT EXISTS cbm_zova_generation_integrity_v1("
        "generation INTEGER PRIMARY KEY, workspace_id TEXT NOT NULL, project TEXT NOT NULL, "
        "graph_nodes INTEGER NOT NULL, graph_edges INTEGER NOT NULL, metadata_nodes INTEGER NOT NULL, "
        "metadata_edges INTEGER NOT NULL, metadata_topology_edges INTEGER NOT NULL DEFAULT 0, "
        "fts_rows INTEGER NOT NULL, node_vectors INTEGER NOT NULL, "
        "token_vectors INTEGER NOT NULL);";
    zova_database_exec_request schema_req = {.db = zdb.db, .sql = schema_sql};
    int rc = status_ok(zova_database_exec(&schema_req), zdb.db, "integrity.schema_init");
    int64_t has_topology_column = 0;
    if (rc == 0) {
        rc = zova_query_i64(
            zdb.db,
            "SELECT count(*) FROM pragma_table_info('cbm_zova_generation_integrity_v1') "
            "WHERE name='metadata_topology_edges'",
            NULL, &has_topology_column, "integrity.topology_column_probe");
    }
    if (rc == 0 && !has_topology_column) {
        schema_req.sql =
            "ALTER TABLE cbm_zova_generation_integrity_v1 "
            "ADD COLUMN metadata_topology_edges INTEGER NOT NULL DEFAULT 0";
        rc = status_ok(zova_database_exec(&schema_req), zdb.db,
                        "integrity.topology_column_add");
    }
    int64_t graph_nodes = 0;
    int64_t graph_edges = 0;
    int64_t metadata_nodes = 0;
    int64_t metadata_edges = 0;
    int64_t metadata_topology_edges = 0;
    int64_t fts_rows = 0;
    int64_t node_vectors = 0;
    int64_t token_vectors = 0;
    if (rc == 0) {
        rc = zova_query_i64(zdb.db, "SELECT count(*) FROM nodes WHERE project=?1", project,
                            &metadata_nodes, "integrity.nodes_count");
    }
    if (rc == 0) {
        rc = zova_query_i64(zdb.db, "SELECT count(*) FROM edges WHERE project=?1", project,
                            &metadata_edges, "integrity.edges_count");
    }
    if (rc == 0) {
        rc = zova_query_i64(zdb.db, "SELECT count(*) FROM nodes_fts", NULL, &fts_rows,
                            "integrity.fts_count");
    }
    if (rc == 0) {
        zova_vector_collection_info info = {0};
        zova_status status = zova_vector_collection_info_get(
            &(zova_vector_collection_info_get_request){
                .db = zdb.db, .name = CBM_ZOVA_NODE_COLLECTION, .out_info = &info});
        if (status == ZOVA_OK) {
            node_vectors = (int64_t)info.vector_count;
            zova_vector_collection_info_free(&info);
        } else if (status != ZOVA_VECTOR_COLLECTION_NOT_FOUND) rc = -1;
    }
    if (rc == 0) {
        zova_vector_collection_info info = {0};
        zova_status status = zova_vector_collection_info_get(
            &(zova_vector_collection_info_get_request){
                .db = zdb.db, .name = CBM_ZOVA_TOKEN_COLLECTION, .out_info = &info});
        if (status == ZOVA_OK) {
            token_vectors = (int64_t)info.vector_count;
            zova_vector_collection_info_free(&info);
        } else if (status != ZOVA_VECTOR_COLLECTION_NOT_FOUND) rc = -1;
    }
    if (rc == 0 && workspace_scoped) {
        char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
        if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
            rc = -1;
        } else {
            zova_graph_info info = {0};
            zova_graph_info_get_request req = {.db = zdb.db, .name = graph_name, .out_info = &info};
            rc = status_ok(zova_graph_info_get(&req), zdb.db, "integrity.graph_info");
            if (rc == 0) {
                graph_nodes = (int64_t)info.node_count;
                graph_edges = (int64_t)info.edge_count;
            }
            zova_graph_info_free(&info);
        }
        if (rc == 0) {
            rc = zova_query_i64(
                zdb.db,
                "SELECT count(*) FROM (SELECT source_id,target_id,type "
                "FROM edges WHERE project=?1 GROUP BY source_id,target_id,type)",
                project, &metadata_topology_edges,
                "integrity.edge_topology_count");
        }
        /* One native topology edge can represent several rich canonical rows.
         * Compare the graph against distinct source/type/target keys. */
    }
    if (rc == 0) {
        char insert_sql[] =
            "INSERT OR REPLACE INTO cbm_zova_generation_integrity_v1"
            "(generation,workspace_id,project,graph_nodes,graph_edges,metadata_nodes,metadata_edges,"
            "metadata_topology_edges,fts_rows,node_vectors,token_vectors) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)";
        zova_statement *stmt = NULL;
        rc = prepare_zova(zdb.db, insert_sql, &stmt, "integrity.record_prepare");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 1, generation, "integrity.bind_generation");
        if (rc == 0) rc = workspace_bind_text(zdb.db, stmt, 2, workspace_id ? workspace_id : "", "integrity.bind_workspace");
        if (rc == 0) rc = workspace_bind_text(zdb.db, stmt, 3, project ? project : "", "integrity.bind_project");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 4, graph_nodes, "integrity.bind_graph_nodes");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 5, graph_edges, "integrity.bind_graph_edges");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 6, metadata_nodes, "integrity.bind_metadata_nodes");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 7, metadata_edges, "integrity.bind_metadata_edges");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 8, metadata_topology_edges,
                                             "integrity.bind_metadata_topology_edges");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 9, fts_rows, "integrity.bind_fts_rows");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 10, node_vectors, "integrity.bind_node_vectors");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 11, token_vectors, "integrity.bind_token_vectors");
        if (rc == 0) rc = workspace_step_done(zdb.db, stmt, "integrity.record_step");
        if (stmt) (void)zova_statement_finalize(stmt);
    }
    close_zova(&zdb);
    return rc;
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
        workspace_legacy_generation_init(zdb.db) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        return -1;
    }
    zova_statement *stmt = NULL;
    int rc = prepare_zova(zdb.db,
                          "INSERT INTO cbm_workspace_generations(workspace_key,generation,state) "
                          "SELECT workspace_key,?2,'building' FROM cbm_workspace_registry "
                          "WHERE workspace_id=?1",
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
        workspace_legacy_generation_init(zdb.db) != 0 || workspace_registry_begin(zdb.db) != 0) {
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
                          "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                          "WHERE workspace_id=?2) AND generation=?3",
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

typedef struct workspace_graph_node_map workspace_graph_node_map_t;
static const char *workspace_graph_node_map_find(const workspace_graph_node_map_t *items,
                                                  size_t count, int64_t sqlite_id);

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
            .target_type = ZOVA_GRAPH_TARGET_NONE,
            .target_namespace = NULL,
            .target_ref = NULL,
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

typedef struct workspace_graph_node_map {
    int64_t sqlite_id;
    int64_t node_key;
    char *stable_id;
} workspace_graph_node_map_t;

typedef struct {
    zova_graph_node_input inputs[ZV_BATCH];
    char *kinds[ZV_BATCH];
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

static int workspace_dump_node_id_v2(const char *workspace_id, const CBMDumpNode *node,
                                     char *out, size_t out_size) {
    if (!node || !node->label || !node->name || !node->qualified_name || !node->file_path ||
        !workspace_name_component_valid(workspace_id) || !out || out_size == 0) {
        return -1;
    }
    char relative_path[ZV_PATH_MAX];
    if (snprintf(relative_path, sizeof(relative_path), "%s", node->file_path) >=
        (int)sizeof(relative_path)) {
        return -1;
    }
    cbm_normalize_path_sep(relative_path);
    char discriminator[512];
    if (node->qualified_name[0]) {
        if (snprintf(discriminator, sizeof(discriminator), "named:%s", node->qualified_name) >=
            (int)sizeof(discriminator)) return -1;
    } else if (!node->name[0] || node->start_line < 0 || node->end_line < node->start_line ||
               snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", node->name,
                        node->start_line, node->end_line) >= (int)sizeof(discriminator)) {
        return -1;
    }
    return cbm_zova_workspace_node_id_v2(workspace_id, node->label, relative_path,
                                         node->qualified_name, discriminator, out, out_size);
}

static int workspace_graph_node_map_compare(const void *a, const void *b) {
    const workspace_graph_node_map_t *left = a;
    const workspace_graph_node_map_t *right = b;
    if (left->sqlite_id < right->sqlite_id) return -1;
    if (left->sqlite_id > right->sqlite_id) return 1;
    return strcmp(left->stable_id, right->stable_id);
}

static int workspace_generation_map_build(const char *workspace_id, const CBMDumpNode *nodes,
                                          int node_count, workspace_graph_node_map_t **out_map,
                                          size_t *out_count) {
    if (!out_map || !out_count || node_count < 0 || (node_count > 0 && !nodes)) {
        return -1;
    }
    *out_map = NULL;
    *out_count = 0;
    workspace_graph_node_map_t *map =
        node_count > 0 ? calloc((size_t)node_count, sizeof(*map)) : NULL;
    if (node_count > 0 && !map) {
        return -1;
    }
    for (int i = 0; i < node_count; i++) {
        char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (nodes[i].id <= 0 || workspace_dump_node_id_v2(workspace_id, &nodes[i], stable_id,
                                                          sizeof(stable_id)) != 0) {
            workspace_graph_node_map_free(map, (size_t)i);
            return -1;
        }
        map[i].sqlite_id = nodes[i].id;
        map[i].stable_id = zv_strndup(stable_id, strlen(stable_id));
        if (!map[i].stable_id) {
            workspace_graph_node_map_free(map, (size_t)i);
            return -1;
        }
    }
    qsort(map, (size_t)node_count, sizeof(*map), workspace_graph_node_map_compare);
    for (int i = 1; i < node_count; i++) {
        if (map[i - 1].sqlite_id == map[i].sqlite_id ||
            strcmp(map[i - 1].stable_id, map[i].stable_id) == 0) {
            workspace_graph_node_map_free(map, (size_t)node_count);
            return -1;
        }
    }
    *out_map = map;
    *out_count = (size_t)node_count;
    return 0;
}

static int workspace_edge_id_v1(const char *workspace_id, const char *source_node_id,
                                const char *edge_type, const char *target_node_id,
                                const char *local_name, char *out, size_t out_size) {
    if (!workspace_name_component_valid(workspace_id) || !source_node_id || !edge_type ||
        !target_node_id || !local_name || !out || out_size == 0) {
        return -1;
    }
    const char *parts[] = {source_node_id, edge_type, target_node_id, local_name};
    cbm_sha256_ctx hash;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char digits[] = "0123456789abcdef";
    char hex[33];
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
    return snprintf(out, out_size, "e:v1:%s:%s", workspace_id, hex) < (int)out_size ? 0 : -1;
}

static int user_database_reset_statement(zova_database *db, zova_statement *stmt,
                                         const char *phase) {
    if (!stmt || zova_statement_reset(stmt) != ZOVA_OK ||
        zova_statement_clear_bindings(stmt) != ZOVA_OK) {
        cbm_log_error("zova.user_import", "phase", phase ? phase : "reset");
        return -1;
    }
    (void)db;
    return 0;
}

static int user_database_reset_rebound_statement(zova_database *db,
                                                 zova_statement *stmt,
                                                 const char *phase) {
    if (!stmt || zova_statement_reset(stmt) != ZOVA_OK) {
        cbm_log_error("zova.user_import", "phase", phase ? phase : "reset");
        return -1;
    }
    (void)db;
    return 0;
}

static int user_database_clear_workspace(zova_database *db, const char *workspace_id) {
    if (user_publish_delta_active) {
        user_publish_test_metrics.delta_clear_violation_count++;
        return -1;
    }
    int64_t workspace_key = 0;
    if (workspace_key_for_id(db, workspace_id, &workspace_key) != 0) return -1;
    const char *statements[] = {
        "DELETE FROM cbm_nodes_v1 WHERE workspace_key=?1",
        ("DELETE FROM cbm_file_hashes_v1 WHERE file_key IN "
         "(SELECT file_key FROM cbm_files_v1 WHERE workspace_key=?1)"),
        "DELETE FROM cbm_files_v1 WHERE workspace_key=?1",
        "DELETE FROM cbm_token_vector_metadata_v1 WHERE workspace_key=?1",
        "DELETE FROM cbm_project_summaries_v2 WHERE workspace_key=?1",
        "DELETE FROM cbm_projects_v1 WHERE workspace_key=?1",
    };
    for (size_t i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) {
        zova_statement *stmt = NULL;
        int rc = prepare_zova(db, statements[i], &stmt, "user_import.clear_prepare");
        if (rc == 0)
            rc = workspace_bind_i64(db, stmt, 1, workspace_key, "user_import.clear_bind");
        if (rc == 0) {
            rc = workspace_step_done(db, stmt, "user_import.clear_step");
        }
        if (stmt) {
            (void)zova_statement_finalize(stmt);
        }
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

static int user_database_write_nodes_with_map(
    zova_database *db, const char *workspace_id, const char *project,
    const CBMDumpNode *nodes, int node_count, workspace_graph_node_map_t *map,
    size_t map_count) {
    if (node_count < 0 || (node_count > 0 && (!nodes || !map)) ||
        map_count != (size_t)node_count) {
        return -1;
    }
    zova_statement *node_stmt = NULL;
    int64_t workspace_key = 0;
    if (workspace_key_for_id(db, workspace_id, &workspace_key) != 0) return -1;
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    zova_graph_node_input *native = node_count ? calloc((size_t)node_count, sizeof(*native)) : NULL;
    int64_t *native_keys = node_count ? calloc((size_t)node_count, sizeof(*native_keys)) : NULL;
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0 ||
        (node_count && (!native || !native_keys))) { free(native); free(native_keys); return -1; }
    for (int i = 0; i < node_count; i++) native[i] = (zova_graph_node_input){
        .graph_name=graph_name,.node_id=map[i].stable_id,.kind=nodes[i].label,
        .target_type=ZOVA_GRAPH_TARGET_NONE,.target_namespace=NULL,
        .target_ref=NULL};
    int rc = node_count ? status_ok(zova_graph_node_put_many_keyed(
        &(zova_graph_node_put_many_keyed_request){.db=db,.nodes=native,
        .nodes_len=(size_t)node_count,.out_node_keys=native_keys,
        .out_node_keys_capacity=(size_t)node_count}),db,"user_import.native_nodes") : 0;
    free(native);
    if (rc != 0) { free(native_keys); return -1; }
    rc = prepare_zova(
        db,
        "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,"
        "file_key,start_line,end_line,properties,source_ordinal) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
        "ON CONFLICT(zova_node_key) DO UPDATE SET "
        "label=excluded.label,name=excluded.name,qualified_name=excluded.qualified_name,"
        "file_key=excluded.file_key,start_line=excluded.start_line,end_line=excluded.end_line,"
        "properties=excluded.properties,source_ordinal=excluded.source_ordinal",
        &node_stmt, "user_import.nodes_prepare");
    for (int i = 0; rc == 0 && i < node_count; i++) {
        const CBMDumpNode *node = &nodes[i];
        if (node->id <= 0 || !node->project || !node->label || !node->name ||
            !node->qualified_name || !node->file_path ||
            (i > 0 && nodes[i - 1].id >= node->id)) {
            rc = -1;
            break;
        }
        const char *stable_id = map[i].stable_id;
        if (map[i].sqlite_id != node->id || !stable_id ||
            !workspace_name_component_valid(workspace_id)) {
            rc = -1;
            break;
        }

        int64_t file_key = 0;
        map[i].node_key = native_keys[i];
        if (file_key_get_or_create(db, workspace_key, node->file_path, &file_key) != 0) {
            rc = -1;
            break;
        }
        rc = workspace_bind_i64(db, node_stmt, 1, map[i].node_key, "user_import.node_key");
        if (rc == 0) rc = workspace_bind_i64(db, node_stmt, 2, workspace_key,
                                              "user_import.node_workspace");
        if (rc == 0) rc = workspace_bind_text(db, node_stmt, 3, node->label, "user_import.node_label");
        if (rc == 0) rc = workspace_bind_text(db, node_stmt, 4, node->name, "user_import.node_name");
        if (rc == 0) rc = workspace_bind_text(db, node_stmt, 5, node->qualified_name,
                                               "user_import.node_qn");
        if (rc == 0) rc = workspace_bind_i64(db, node_stmt, 6, file_key, "user_import.node_file");
        if (rc == 0) rc = workspace_bind_i64(db, node_stmt, 7, node->start_line, "user_import.node_start");
        if (rc == 0) rc = workspace_bind_i64(db, node_stmt, 8, node->end_line, "user_import.node_end");
        if (rc == 0) rc = workspace_bind_text(db, node_stmt, 9, node->properties ? node->properties : "{}",
                                               "user_import.node_properties");
        if (rc == 0) rc = workspace_bind_i64(db, node_stmt, 10, i,
                                              "user_import.node_source_ordinal");
        if (rc == 0) rc = workspace_step_done(db, node_stmt, "user_import.node_step");
        if (rc == 0) rc = user_database_reset_statement(db, node_stmt, "user_import.node_reset");
    }
    if (node_stmt) (void)zova_statement_finalize(node_stmt);
    free(native_keys);
    (void)project;
    return rc;
}

static int user_database_write_nodes(zova_database *db, const char *workspace_id,
                                     const char *project, const CBMDumpNode *nodes, int node_count,
                                     workspace_graph_node_map_t **out_map, size_t *out_map_count) {
    *out_map = NULL;
    *out_map_count = 0;
    workspace_graph_node_map_t *map = NULL;
    size_t map_count = 0;
    if (workspace_generation_map_build(workspace_id, nodes, node_count, &map, &map_count) != 0)
        return -1;
    if (user_database_write_nodes_with_map(db, workspace_id, project, nodes, node_count,
                                           map, map_count) != 0) {
        workspace_graph_node_map_free(map, map_count);
        return -1;
    }
    *out_map = map;
    *out_map_count = map_count;
    return 0;
}

static int user_database_write_edges(zova_database *db, const char *workspace_id,
                                     const CBMDumpEdge *edges, int edge_count,
                                     const workspace_graph_node_map_t *map, size_t map_count) {
    if (edge_count < 0 || (edge_count > 0 && !edges)) {
        return -1;
    }
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    zova_graph_edge_input *native = edge_count ? calloc((size_t)edge_count, sizeof(*native)) : NULL;
    int64_t *native_keys = edge_count ? calloc((size_t)edge_count, sizeof(*native_keys)) : NULL;
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0 ||
        (edge_count && (!native || !native_keys))) { free(native); free(native_keys); return -1; }
    for (int i = 0; i < edge_count; i++) native[i] = (zova_graph_edge_input){
        .graph_name=graph_name,.from_node_id=workspace_graph_node_map_find(map,map_count,edges[i].source_id),
        .edge_type=edges[i].type,.to_node_id=workspace_graph_node_map_find(map,map_count,edges[i].target_id)};
    int rc = edge_count ? status_ok(zova_graph_edge_put_many_keyed(
        &(zova_graph_edge_put_many_keyed_request){.db=db,.edges=native,
        .edges_len=(size_t)edge_count,.out_edge_keys=native_keys,
        .out_edge_keys_capacity=(size_t)edge_count}),db,"user_import.native_edges") : 0;
    free(native);
    if (rc != 0) { free(native_keys); return -1; }
    bool *handled = edge_count ? calloc((size_t)edge_count, sizeof(*handled)) : NULL;
    const CBMDumpEdge **group = edge_count
        ? calloc((size_t)edge_count, sizeof(*group)) : NULL;
    zova_graph_edge_payload_replacement *replacements = edge_count
        ? calloc((size_t)edge_count, sizeof(*replacements)) : NULL;
    uint8_t **owned_payloads = edge_count
        ? calloc((size_t)edge_count, sizeof(*owned_payloads)) : NULL;
    if (edge_count && (!handled || !group || !replacements || !owned_payloads)) rc = -1;
    size_t replacement_count = 0;
    for (int i = 0; rc == 0 && i < edge_count; i++) {
        if (handled[i]) continue;
        if (native_keys[i] <= 0) { rc = -1; break; }
        size_t group_count = 0;
        for (int j = i; j < edge_count; j++) {
            if (!handled[j] && native_keys[j] == native_keys[i]) {
                handled[j] = true;
                group[group_count++] = &edges[j];
            }
        }
        size_t payload_len = 0;
        if (cbm_zova_edge_payload_encoded_size(group, group_count, &payload_len) != 0) {
            rc = -1;
            break;
        }
        if (payload_len > 0) {
            owned_payloads[replacement_count] = malloc(payload_len);
            if (!owned_payloads[replacement_count]) { rc = -1; break; }
        }
        size_t encoded = 0;
        if (cbm_zova_edge_payload_encode(
                group, group_count, owned_payloads[replacement_count], payload_len,
                &encoded) != 0 || encoded != payload_len) {
            rc = -1;
            break;
        }
        replacements[replacement_count] = (zova_graph_edge_payload_replacement){
            .edge_key = native_keys[i],
            .payload = owned_payloads[replacement_count],
            .payload_len = payload_len,
        };
        replacement_count++;
    }
    if (rc == 0 && replacement_count > 0)
        rc = status_ok(zova_graph_edge_payload_replace_many(
                           &(zova_graph_edge_payload_replace_many_request){
                               .db = db, .graph_name = graph_name,
                               .replacements = replacements,
                               .replacement_count = replacement_count}),
                       db, "user_import.edge_payloads");
    for (size_t i = 0; owned_payloads && i < replacement_count; i++)
        free(owned_payloads[i]);
    free(owned_payloads);
    free(replacements);
    free(group);
    free(handled);
    free(native_keys);
    return rc;
}

static int user_database_write_summary(zova_database *db, const char *workspace_id,
                                       const cbm_zova_project_summary_input_t *summary) {
    if (!summary) return 0;
    if (!summary->present) {
        zova_statement *stmt = NULL;
        int rc = prepare_zova(db,
                              "DELETE FROM cbm_project_summaries_v2 WHERE workspace_key=(SELECT "
                              "workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1)",
                              &stmt, "user_import.summary_delete_prepare");
        if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id, "user_import.summary_delete_bind");
        if (rc == 0) rc = workspace_step_done(db, stmt, "user_import.summary_delete_step");
        if (stmt) (void)zova_statement_finalize(stmt);
        return rc;
    }
    if (!summary->summary || !summary->source_hash || !summary->created_at || !summary->updated_at) return -1;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "INSERT INTO cbm_project_summaries_v2(workspace_key,summary,source_hash,created_at,updated_at) "
                          "SELECT workspace_key,?2,?3,?4,?5 FROM cbm_workspace_registry WHERE workspace_id=?1 "
                          "ON CONFLICT(workspace_key) DO UPDATE SET summary=excluded.summary,"
                          "source_hash=excluded.source_hash,created_at=excluded.created_at,updated_at=excluded.updated_at",
                          &stmt, "user_import.summary_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id, "user_import.summary_workspace");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 2, summary->summary, "user_import.summary_value");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 3, summary->source_hash, "user_import.summary_hash");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 4, summary->created_at, "user_import.summary_created");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 5, summary->updated_at, "user_import.summary_updated");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_import.summary_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

int cbm_zova_user_database_import_workspace(
    const char *zova_path, const char *root_path, const char *project, int64_t generation,
    const CBMDumpNode *nodes, int node_count, const CBMDumpEdge *edges, int edge_count) {
    if (!zova_path || !zova_path[0] || !root_path || !root_path[0] || !project || !project[0] ||
        generation <= 0 || node_count < 0 || edge_count < 0 ||
        (node_count > 0 && !nodes) || (edge_count > 0 && !edges)) {
        return -1;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    if (cbm_zova_user_database_generation_begin(zova_path, root_path, generation, workspace_id,
                                                sizeof(workspace_id)) != 0) {
        return -1;
    }

    cbm_zova_db_t zdb;
    if (open_or_create_zova(zova_path, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        (void)cbm_zova_user_database_generation_finish(zova_path, workspace_id, generation, false);
        return -1;
    }
    workspace_graph_node_map_t *map = NULL;
    size_t map_count = 0;
    int rc = user_database_clear_workspace(zdb.db, workspace_id);
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (rc == 0 && cbm_zova_workspace_graph_name(workspace_id, graph_name,
                                                  sizeof(graph_name)) != 0) rc = -1;
    if (rc == 0) rc = graph_delete_named_if_exists(zdb.db, graph_name);
    if (rc == 0) rc = status_ok(zova_graph_create(&(zova_graph_create_request){
        .db=zdb.db,.name=graph_name}),zdb.db,"user_import.graph_create");
    zova_statement *project_stmt = NULL;
    if (rc == 0) {
        rc = prepare_zova(zdb.db,
                          "INSERT INTO cbm_projects_v1(workspace_key,project,root_path) "
                          "SELECT workspace_key,?2,?3 FROM cbm_workspace_registry WHERE workspace_id=?1 "
                          "ON CONFLICT(workspace_key) DO UPDATE SET "
                          "project=excluded.project,root_path=excluded.root_path,indexed_at=CURRENT_TIMESTAMP",
                          &project_stmt, "user_import.project_prepare");
    }
    if (rc == 0) rc = workspace_bind_text(zdb.db, project_stmt, 1, workspace_id, "user_import.project_workspace");
    if (rc == 0) rc = workspace_bind_text(zdb.db, project_stmt, 2, project, "user_import.project_name");
    if (rc == 0) rc = workspace_bind_text(zdb.db, project_stmt, 3, root_path, "user_import.project_root");
    if (rc == 0) rc = workspace_step_done(zdb.db, project_stmt, "user_import.project_step");
    if (project_stmt) (void)zova_statement_finalize(project_stmt);
    if (rc == 0) {
        rc = user_database_write_nodes(zdb.db, workspace_id, project, nodes, node_count, &map,
                                       &map_count);
    }
    if (rc == 0 && sidecar_test_fault("user_after_nodes")) rc = -1;
    if (rc == 0) rc = user_database_write_edges(zdb.db, workspace_id, edges, edge_count, map, map_count);
    if (rc == 0 && sidecar_test_fault("user_after_edges")) rc = -1;
    if (rc == 0 && sidecar_test_fault("user_before_commit")) rc = -1;
    if (rc == 0) rc = workspace_registry_commit(zdb.db);
    if (rc != 0) workspace_registry_rollback(zdb.db);
    workspace_graph_node_map_free(map, map_count);
    close_zova(&zdb);
    if (rc != 0) {
        (void)cbm_zova_user_database_generation_finish(zova_path, workspace_id, generation, false);
        return -1;
    }
    if (cbm_zova_user_database_generation_finish(zova_path, workspace_id, generation, true) != 0) {
        (void)cbm_zova_user_database_generation_finish(zova_path, workspace_id, generation, false);
        return -1;
    }
    return 0;
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
    int rc = prepare_zova(db,
                          "SELECT id,label,name,file_path,qualified_name,start_line,end_line,properties "
                          "FROM nodes WHERE project = ?1 ORDER BY id",
                          &stmt, "workspace_graph.nodes_prepare");
    if (rc == 0) {
        rc = workspace_bind_text(db, stmt, 1, project, "workspace_graph.nodes_bind_project");
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
        char *properties = NULL;
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
            workspace_graph_column_text_copy(db, stmt, 7, &properties,
                                             "workspace_graph.node_properties") != 0 ||
            status_ok(zova_statement_column_int64(&start_req), db,
                      "workspace_graph.node_start") != 0 ||
            status_ok(zova_statement_column_int64(&end_req), db, "workspace_graph.node_end") != 0) {
            free(kind);
            free(name);
            free(file_path);
            free(qualified_name);
            free(properties);
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
            free(properties);
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
            free(properties);
            rc = -1;
            break;
        }
        char *stable_id_copy = zv_strndup(stable_id, strlen(stable_id));
        if (!stable_id_copy) {
            free(stable_id_copy);
            free(kind);
            free(name);
            free(file_path);
            free(qualified_name);
            rc = -1;
            break;
        }
        free(name);
        free(file_path);
        free(qualified_name);
        free(properties);
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 128;
            workspace_graph_node_map_t *grown = realloc(items, next * sizeof(*items));
            if (!grown) {
                free(stable_id_copy);
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
        batch.inputs[batch.count] = (zova_graph_node_input){
            .graph_name = graph_name,
            .node_id = stable_id_copy,
            .kind = kind,
            .target_type = ZOVA_GRAPH_TARGET_NONE,
            .target_namespace = NULL,
            .target_ref = NULL,
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
                          "SELECT source_id,type,target_id,properties FROM edges "
                          "WHERE project = ?1 ORDER BY id",
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
        char *properties = NULL;
        zova_statement_column_int64_request source_req = {
            .statement = stmt, .index = 0, .out_value = &source_id};
        zova_statement_column_int64_request target_req = {
            .statement = stmt, .index = 2, .out_value = &target_id};
        if (status_ok(zova_statement_column_int64(&source_req), db,
                      "workspace_graph.edge_source") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 1, &edge_type,
                                             "workspace_graph.edge_type") != 0 ||
            status_ok(zova_statement_column_int64(&target_req), db,
                      "workspace_graph.edge_target") != 0 ||
            workspace_graph_column_text_copy(db, stmt, 3, &properties,
                                             "workspace_graph.edge_properties") != 0) {
            free(edge_type);
            free(properties);
            rc = -1;
            break;
        }
        const char *source = workspace_graph_node_map_find(nodes, node_count, source_id);
        const char *target = workspace_graph_node_map_find(nodes, node_count, target_id);
        if (!source || !target) {
            free(edge_type);
            free(properties);
            rc = -1;
            break;
        }
        free(properties);
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
    int rc = graph_delete_named_if_exists(zdb.db, graph_name);
    if (rc == 0) {
        zova_graph_create_request create_req = {.db = zdb.db, .name = graph_name};
        rc = status_ok(zova_graph_create(&create_req), zdb.db, "workspace_graph.create");
    }
    if (rc == 0) {
        rc = workspace_graph_mirror_nodes(zdb.db, graph_name, workspace_id, project, &nodes,
                                          &node_count);
    }
    if (rc == 0) {
        rc = workspace_graph_mirror_edges(zdb.db, graph_name, project, nodes,
                                          node_count);
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
                                              const char *workspace_id,
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
    workspace_graph_node_batch_t batch = {0};
    int rc = 0;
    size_t count = 0;
    for (int i = 0; rc == 0 && i < node_count; i++) {
        const CBMDumpNode *node = &dump_nodes[i];
        if (node->id <= 0 || !node->label || !node->name || !node->qualified_name ||
            !node->file_path || (count > 0 && items[count - 1].sqlite_id >= node->id)) {
            rc = -1;
            break;
        }
        char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        char discriminator[64];
        if (node->qualified_name[0]) snprintf(discriminator, sizeof(discriminator), "named");
        else snprintf(discriminator, sizeof(discriminator), "anon:%d:%d",
                      node->start_line, node->end_line);
        if (cbm_zova_workspace_node_id_v1(
                workspace_id, node->label, node->file_path, node->qualified_name,
                discriminator, stable_id, sizeof(stable_id)) != 0) {
            rc = -1;
            break;
        }
        char *stable_id_copy = zv_strndup(stable_id, strlen(stable_id));
        if (!stable_id_copy) {
            rc = -1;
            break;
        }
        items[count++] = (workspace_graph_node_map_t){.sqlite_id = node->id,
                                                       .stable_id = stable_id_copy};
        batch.inputs[batch.count] = (zova_graph_node_input){
            .graph_name = graph_name,
            .node_id = stable_id_copy,
            .kind = node->label,
            .target_type = ZOVA_GRAPH_TARGET_NONE,
            .target_namespace = NULL,
            .target_ref = NULL,
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
    if (rc != 0) {
        workspace_graph_node_map_free(items, count);
        return -1;
    }
    *out_items = items;
    *out_count = count;
    return 0;
}

static int workspace_graph_write_direct_edges(zova_database *db, const char *graph_name,
                                              const CBMDumpEdge *edges,
                                              int edge_count,
                                              const workspace_graph_node_map_t *nodes,
                                              size_t node_count) {
    if (edge_count < 0 || (edge_count > 0 && !edges)) {
        return -1;
    }
    zova_graph_edge_input inputs[ZV_BATCH] = {0};
    int rc = 0;
    int batch = 0;
    for (int i = 0; i < edge_count; i++) {
        const CBMDumpEdge *edge = &edges[i];
        const char *source = workspace_graph_node_map_find(nodes, node_count, edge->source_id);
        const char *target = workspace_graph_node_map_find(nodes, node_count, edge->target_id);
        if (!source || !target || !edge->type || !edge->type[0]) {
            rc = -1;
            break;
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
                rc = -1;
                break;
            }
            batch = 0;
        }
    }
    if (batch > 0) {
        zova_graph_edge_put_many_request req = {
            .db = db, .edges = inputs, .edges_len = (size_t)batch};
        if (status_ok(zova_graph_edge_put_many(&req), db,
                      "workspace_graph.direct_edge_put_many") != 0) {
            rc = -1;
        }
    }
    if (rc == 0 && sidecar_test_fault("after_graph_edges")) {
        rc = -1;
    }
    return rc;
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
    int rc = graph_delete_named_if_exists(zdb.db, graph_name);
    if (rc == 0) {
        zova_graph_create_request req = {.db = zdb.db, .name = graph_name};
        rc = status_ok(zova_graph_create(&req), zdb.db, "workspace_graph.direct_create");
    }
    if (rc == 0) {
        rc = workspace_graph_write_direct_nodes(zdb.db, graph_name, workspace_id, nodes,
                                                node_count, &node_map,
                                                &node_count_written);
    }
    if (rc == 0) {
        rc = workspace_graph_write_direct_edges(zdb.db, graph_name, edges,
                                                edge_count, node_map, node_count_written);
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

static int workspace_vector_nonzero(const uint8_t *values, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (values[i] != 0) return 1;
    return 0;
}

int cbm_zova_workspace_generation_digest_input(
    const char *workspace_id, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result) {
    if (!workspace_id || !input || !out_result) return -1;
    cbm_zova_publish_model_t *model = NULL;
    if (cbm_zova_publish_model_build(workspace_id, input, &model) != 0) return -1;
    const cbm_zova_workspace_generation_result_t *digests =
        cbm_zova_publish_model_digests(model);
    if (digests) *out_result = *digests;
    cbm_zova_publish_model_free(model);
    return digests ? 0 : -1;
}

static int user_workspace_id_tx(zova_database *db, const char *root_path, char *out_id, size_t out_size) {
    char root[ZV_PATH_MAX];
    if (workspace_normalize_root(root_path, root) != 0 || workspace_id_from_root(root, out_id, out_size) != 0) return -1;
    if (workspace_registry_init(db) != 0) return -1;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "INSERT INTO cbm_workspace_registry(workspace_id,canonical_root,id_format_version,active_generation) "
                          "VALUES(?1,?2,1,0) ON CONFLICT(canonical_root) DO NOTHING",
                          &stmt, "user_publish.workspace_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, out_id, "user_publish.workspace_id");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 2, root, "user_publish.workspace_root");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.workspace_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int user_publish_generation_state_tx(zova_database *db, const char *workspace_id,
                                            int64_t generation, bool ready) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db,
                          "UPDATE cbm_database_generation_v1 SET state=?1 WHERE workspace_key="
                          "(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id=?2) "
                          "AND generation=?3",
                          &stmt, "user_publish.generation_update_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, ready ? "ready" : "failed", "user_publish.generation_state");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 2, workspace_id, "user_publish.generation_workspace");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 3, generation, "user_publish.generation_id");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.generation_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    if (rc == 0 && ready) {
        rc = prepare_zova(db, "UPDATE cbm_workspace_registry SET active_generation=?1 WHERE workspace_id=?2",
                          &stmt, "user_publish.active_prepare");
        if (rc == 0) rc = workspace_bind_i64(db, stmt, 1, generation, "user_publish.active_generation");
        if (rc == 0) rc = workspace_bind_text(db, stmt, 2, workspace_id, "user_publish.active_workspace");
        if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.active_step");
        if (stmt) (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int user_publish_clear_health_tx(zova_database *db, const char *workspace_id) {
    zova_statement *statement = NULL;
    int rc = prepare_zova(db,
                          "DELETE FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT "
                          "workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1)",
                          &statement, "user_publish.health_prepare");
    if (rc == 0)
        rc = workspace_bind_text(db, statement, 1, workspace_id,
                                 "user_publish.health_workspace");
    if (rc == 0)
        rc = workspace_step_done(db, statement, "user_publish.health_step");
    if (statement) (void)zova_statement_finalize(statement);
    return rc;
}

static int user_publish_insert_generation_tx(zova_database *db, const char *workspace_id,
                                             int64_t generation) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "INSERT INTO cbm_database_generation_v1(workspace_key,generation,state) "
        "SELECT workspace_key,?2,'building' FROM cbm_workspace_registry WHERE workspace_id=?1",
        &stmt, "user_publish.generation_insert_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id,
                                          "user_publish.generation_insert_workspace");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 2, generation,
                                         "user_publish.generation_insert_id");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.generation_insert_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int user_publish_project_tx(zova_database *db, const char *workspace_id,
                                   const char *root_path, const char *project,
                                   const char *indexed_at) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "INSERT INTO cbm_projects_v1(workspace_key,project,root_path,indexed_at) "
        "SELECT workspace_key,?2,?3,?4 FROM cbm_workspace_registry WHERE workspace_id=?1 "
        "ON CONFLICT(workspace_key) DO UPDATE SET "
        "project=excluded.project,root_path=excluded.root_path,indexed_at=excluded.indexed_at",
        &stmt, "user_publish.project_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id,
                                          "user_publish.project_workspace");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 2, project,
                                          "user_publish.project_name");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 3, root_path,
                                          "user_publish.project_root");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 4, indexed_at,
                                          "user_publish.project_indexed_at");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.project_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int user_publish_write_index_state_tx(zova_database *db, const char *workspace_id,
                                             int64_t generation, const char *model,
                                             int dimensions, const char *indexed_at) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "INSERT INTO cbm_workspace_index_state_v1"
        "(workspace_key,generation,model_fingerprint,vector_dimensions,indexed_at) "
        "SELECT workspace_key,?2,?3,?4,?5 FROM cbm_workspace_registry WHERE workspace_id=?1 "
        "ON CONFLICT(workspace_key) DO UPDATE SET "
        "generation=excluded.generation,model_fingerprint=excluded.model_fingerprint,"
        "vector_dimensions=excluded.vector_dimensions,indexed_at=excluded.indexed_at",
        &stmt, "user_publish.index_state_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id,
                                          "user_publish.index_state_workspace");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 2, generation,
                                         "user_publish.index_state_generation");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 3, model,
                                          "user_publish.index_state_model");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 4, dimensions,
                                         "user_publish.index_state_dimensions");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 5, indexed_at,
                                          "user_publish.index_state_indexed_at");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.index_state_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int user_publish_native_readback_counts_tx(
    zova_database *db, const cbm_zova_workspace_generation_result_t *result,
    const char *node_collection, const char *token_collection) {
    if (!db || !result || !node_collection || !token_collection) {
        return -1;
    }
    zova_vector_collection_info node_info = {0};
    zova_vector_collection_info_get_request node_request = {
        .db = db, .name = node_collection, .out_info = &node_info};
    if (status_ok(zova_vector_collection_info_get(&node_request), db,
                  "user_publish.readback.node_collection") != 0 ||
        node_info.vector_count != result->node_vectors) {
        zova_vector_collection_info_free(&node_info);
        return -1;
    }
    zova_vector_collection_info_free(&node_info);
    zova_vector_collection_info token_info = {0};
    zova_vector_collection_info_get_request token_request = {
        .db = db, .name = token_collection, .out_info = &token_info};
    if (status_ok(zova_vector_collection_info_get(&token_request), db,
                  "user_publish.readback.token_collection") != 0 ||
        token_info.vector_count != result->token_vectors) {
        zova_vector_collection_info_free(&token_info);
        return -1;
    }
    zova_vector_collection_info_free(&token_info);
    return 0;
}

static int user_publish_integrity_tx(zova_database *db, const char *workspace_id,
                                     int64_t generation,
                                     const cbm_zova_workspace_generation_result_t *result) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "INSERT INTO cbm_generation_integrity_v2"
        "(workspace_key,generation,graph_nodes,graph_edges,metadata_nodes,metadata_edges,"
        "metadata_topology_edges,fts_rows,node_vector_rows,token_vector_rows,node_vectors,"
        "token_vectors,metadata_sha256,fts_sha256,topology_sha256,node_vector_sha256,"
        "token_vector_sha256) SELECT workspace_key,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
        "?13,?14,?15,?16,?17 FROM cbm_workspace_registry WHERE workspace_id=?1",
        &stmt, "user_publish.integrity_prepare");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 1, workspace_id,
                                          "user_publish.integrity_workspace");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 2, generation,
                                         "user_publish.integrity_generation");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 3, (int64_t)result->graph_nodes,
                                         "user_publish.integrity_graph_nodes");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 4, (int64_t)result->graph_edges,
                                         "user_publish.integrity_graph_edges");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 5, (int64_t)result->metadata_nodes,
                                         "user_publish.integrity_metadata_nodes");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 6, (int64_t)result->metadata_edges,
                                         "user_publish.integrity_metadata_edges");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 7,
                                         (int64_t)result->metadata_topology_edges,
                                         "user_publish.integrity_topology_edges");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 8, (int64_t)result->fts_rows,
                                         "user_publish.integrity_fts_rows");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 9, (int64_t)result->node_vector_rows,
                                         "user_publish.integrity_node_vector_rows");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 10, (int64_t)result->token_vector_rows,
                                         "user_publish.integrity_token_vector_rows");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 11, (int64_t)result->node_vectors,
                                         "user_publish.integrity_node_vectors");
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 12, (int64_t)result->token_vectors,
                                         "user_publish.integrity_token_vectors");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 13, result->metadata_sha256,
                                          "user_publish.integrity_metadata_digest");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 14, result->fts_sha256,
                                          "user_publish.integrity_fts_digest");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 15, result->topology_sha256,
                                          "user_publish.integrity_topology_digest");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 16, result->node_vector_sha256,
                                          "user_publish.integrity_node_vector_digest");
    if (rc == 0) rc = workspace_bind_text(db, stmt, 17, result->token_vector_sha256,
                                          "user_publish.integrity_token_vector_digest");
    if (rc == 0) rc = workspace_step_done(db, stmt, "user_publish.integrity_step");
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static double user_publish_elapsed_ms(const struct timespec *start,
                                      const struct timespec *end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static double user_publish_profile_phase(const char *phase, struct timespec *mark) {
    if (!mark) return 0.0;
    struct timespec now = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_ms = user_publish_elapsed_ms(mark, &now);
    const char *enabled = getenv("CBM_ZOVA_PUBLISH_PROFILE");
    if (enabled && strcmp(enabled, "1") == 0) {
        char elapsed[64];
        snprintf(elapsed, sizeof(elapsed), "%.3f", elapsed_ms);
        cbm_log_info("zova.single_file_publish.phase", "phase", phase, "elapsed_ms", elapsed);
    }
    *mark = now;
    return elapsed_ms;
}

typedef struct {
    const char *path;
    int64_t file_key;
} user_file_key_t;

typedef struct {
    const char *stable_id;
    int64_t node_key;
} user_node_key_t;

typedef struct {
    const char *token;
    int64_t token_key;
} user_token_key_t;

static int user_node_key_compare(const void *left_ptr, const void *right_ptr) {
    const user_node_key_t *left = left_ptr;
    const user_node_key_t *right = right_ptr;
    return strcmp(left->stable_id, right->stable_id);
}

static int user_token_key_compare(const void *left_ptr, const void *right_ptr) {
    const user_token_key_t *left = left_ptr;
    const user_token_key_t *right = right_ptr;
    return strcmp(left->token, right->token);
}

typedef struct {
    const char *source_stable_id;
    const char *edge_type;
    const char *target_stable_id;
    int64_t edge_key;
} user_topology_key_t;

static int user_string_pointer_compare(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static int64_t user_file_key_find(const user_file_key_t *items, int count, const char *path) {
    int low = 0, high = count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        int order = strcmp(items[mid].path, path);
        if (order == 0) return items[mid].file_key;
        if (order < 0) low = mid + 1;
        else high = mid;
    }
    return 0;
}

static int64_t user_node_key_find(const user_node_key_t *items, int count,
                                  const char *stable_id) {
    int low = 0, high = count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        int order = strcmp(items[mid].stable_id, stable_id);
        if (order == 0) return items[mid].node_key;
        if (order < 0) low = mid + 1;
        else high = mid;
    }
    return 0;
}

static int64_t user_token_key_find(const user_token_key_t *items, int count,
                                   const char *token) {
    int low = 0, high = count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        int order = strcmp(items[mid].token, token);
        if (order == 0) return items[mid].token_key;
        if (order < 0) low = mid + 1;
        else high = mid;
    }
    return 0;
}

static int user_topology_identity_compare(
    const cbm_zova_publish_edge_t *row, const char *source,
    const char *edge_type, const char *target) {
    int order = strcmp(row->source_stable_id, source);
    if (order == 0) order = strcmp(row->source->type, edge_type);
    if (order == 0) order = strcmp(row->target_stable_id, target);
    return order;
}

static const cbm_zova_publish_edge_t *user_publish_model_topology_find(
    const cbm_zova_publish_model_t *model, const char *source,
    const char *edge_type, const char *target) {
    int low = 0;
    int high = cbm_zova_publish_model_topology_count(model);
    while (low < high) {
        int middle = low + (high - low) / 2;
        const cbm_zova_publish_edge_t *row =
            cbm_zova_publish_model_topology_at(model, middle);
        if (!row) return NULL;
        int order = user_topology_identity_compare(row, source, edge_type, target);
        if (order < 0) low = middle + 1;
        else if (order > 0) high = middle;
        else return row;
    }
    return NULL;
}

static int user_private_key_id(int64_t key, char out[32]) {
    return key > 0 && snprintf(out, 32, "%lld", (long long)key) < 32 ? 0 : -1;
}

static int user_delta_file_keys_tx(
    zova_database *db, int64_t workspace_key,
    const cbm_zova_workspace_delta_t *delta,
    const cbm_zova_workspace_delta_metrics_t *metrics,
    user_file_key_t **out_items, int *out_count) {
    uint64_t capacity64 = metrics->node_inserts + metrics->node_updates +
                          metrics->file_hash_upserts;
    if (capacity64 > INT_MAX) return -1;
    int capacity = (int)capacity64;
    const char **paths = capacity ? calloc((size_t)capacity, sizeof(*paths)) : NULL;
    if (capacity && !paths) return -1;
    int path_count = 0;
    for (uint64_t i = 0; i < metrics->node_inserts; i++) {
        const cbm_zova_publish_node_t *row =
            cbm_zova_workspace_delta_node_insert_at(delta, (int)i);
        if (!row || !row->source) { free(paths); return -1; }
        paths[path_count++] = row->source->file_path;
    }
    for (uint64_t i = 0; i < metrics->node_updates; i++) {
        const cbm_zova_publish_node_t *row =
            cbm_zova_workspace_delta_node_update_at(delta, (int)i);
        if (!row || !row->source) { free(paths); return -1; }
        paths[path_count++] = row->source->file_path;
    }
    for (uint64_t i = 0; i < metrics->file_hash_upserts; i++) {
        const cbm_zova_file_hash_input_t *row =
            cbm_zova_workspace_delta_file_hash_upsert_at(delta, (int)i);
        if (!row) { free(paths); return -1; }
        paths[path_count++] = row->file_path;
    }
    qsort(paths, (size_t)path_count, sizeof(*paths), user_string_pointer_compare);
    int unique_count = 0;
    for (int i = 0; i < path_count; i++)
        if (i == 0 || strcmp(paths[i - 1], paths[i]) != 0)
            paths[unique_count++] = paths[i];
    user_file_key_t *items = unique_count ? calloc((size_t)unique_count, sizeof(*items)) : NULL;
    if (unique_count && !items) { free(paths); return -1; }
    int rc = 0;
    for (int i = 0; rc == 0 && i < unique_count; i++) {
        items[i].path = paths[i];
        rc = file_key_get_or_create(db, workspace_key, paths[i], &items[i].file_key);
        if (rc == 0) user_publish_test_metrics.delta_file_key_resolutions++;
    }
    free(paths);
    if (rc != 0) { free(items); return -1; }
    *out_items = items;
    *out_count = unique_count;
    return 0;
}

static int user_publish_file_keys_tx(zova_database *db,
                                     const cbm_zova_publish_model_t *model,
                                     int64_t workspace_key, user_file_key_t **out_items,
                                     int *out_count) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_publish_model_input(model);
    int hash_count = cbm_zova_publish_model_file_hash_count(model);
    int capacity = input->node_count + hash_count;
    const char **paths = capacity ? calloc((size_t)capacity, sizeof(*paths)) : NULL;
    if (capacity && !paths) return -1;
    int path_count = 0;
    for (int i = 0; i < input->node_count; i++) paths[path_count++] = input->nodes[i].file_path;
    for (int i = 0; i < hash_count; i++)
        paths[path_count++] = cbm_zova_publish_model_file_hash_at(model, i)->file_path;
    qsort(paths, (size_t)path_count, sizeof(*paths), user_string_pointer_compare);
    int unique_count = 0;
    for (int i = 0; i < path_count; i++)
        if (i == 0 || strcmp(paths[i - 1], paths[i]) != 0) paths[unique_count++] = paths[i];
    user_file_key_t *items = unique_count ? calloc((size_t)unique_count, sizeof(*items)) : NULL;
    if (unique_count && !items) { free(paths); return -1; }
    int64_t next_key = 0;
    int rc = unique_count == 0 ? 0 : next_private_key(
        db, "SELECT COALESCE(MAX(file_key),0)+1 FROM cbm_files_v1", &next_key,
        "user_publish.next_file_key");
    zova_statement *statement = NULL;
    if (rc == 0 && unique_count)
        rc = prepare_zova(db,
                          "INSERT INTO cbm_files_v1(file_key,workspace_key,file_path) "
                          "VALUES(?1,?2,?3)",
                          &statement, "user_publish.files_prepare");
    for (int i = 0; rc == 0 && i < unique_count; i++) {
        items[i] = (user_file_key_t){.path = paths[i], .file_key = next_key + i};
        rc = workspace_bind_i64(db, statement, 1, items[i].file_key,
                                "user_publish.file_key");
        if (rc == 0) rc = workspace_bind_i64(db, statement, 2, workspace_key,
                                              "user_publish.file_workspace");
        if (rc == 0) rc = workspace_bind_text(db, statement, 3, items[i].path,
                                               "user_publish.file_path");
        if (rc == 0) rc = workspace_step_done(db, statement, "user_publish.file_step");
        if (rc == 0) rc = user_database_reset_rebound_statement(
            db, statement, "user_publish.file_reset");
        if (rc == 0)
            user_publish_metric_row(&user_publish_test_metrics.canonical_files_sql,
                                    2, 1, 0, false);
    }
    if (statement) (void)zova_statement_finalize(statement);
    free(paths);
    if (rc != 0) { free(items); return -1; }
    *out_items = items;
    *out_count = unique_count;
    return 0;
}

static int user_publish_model_nodes_tx(zova_database *db,
                                       const cbm_zova_publish_model_t *model,
                                       int64_t workspace_key,
                                       const user_file_key_t *file_keys, int file_key_count,
                                       const user_node_key_t *node_keys) {
    user_publish_test_metrics.canonical_node_fts_passes++;
    zova_statement *node = NULL;
    int count = cbm_zova_publish_model_node_count(model);
    int rc = 0;
    if (rc == 0) rc = prepare_zova(
        db,
        "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,"
        "file_key,start_line,end_line,properties,source_ordinal) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
        &node, "user_publish.model_nodes_prepare");
    for (int i = 0; rc == 0 && i < count; i++) {
        const cbm_zova_publish_node_t *row = cbm_zova_publish_model_node_at(model, i);
        const CBMDumpNode *source = row ? row->source : NULL;
        int64_t file_key = source ? user_file_key_find(file_keys, file_key_count,
                                                       source->file_path) : 0;
        if (!row || !source || file_key <= 0 || row->ordinal != (uint64_t)i) {
            rc = -1;
            break;
        }
        if (!node_keys || strcmp(node_keys[i].stable_id, row->stable_id) != 0 ||
            node_keys[i].node_key <= 0) {
            rc = -1;
            break;
        }
        rc = workspace_bind_i64(db, node, 1, node_keys[i].node_key,
                                "user_publish.model_node_key");
        if (rc == 0) rc = workspace_bind_i64(db, node, 2, workspace_key,
                                              "user_publish.model_node_ws");
        if (rc == 0) rc = workspace_bind_text(db, node, 3, source->label,
                                               "user_publish.model_node_label");
        if (rc == 0) rc = workspace_bind_text(db, node, 4, source->name,
                                               "user_publish.model_node_name");
        if (rc == 0) rc = workspace_bind_text(db, node, 5, source->qualified_name,
                                               "user_publish.model_node_qn");
        if (rc == 0) rc = workspace_bind_i64(db, node, 6, file_key,
                                               "user_publish.model_node_file");
        if (rc == 0) rc = workspace_bind_i64(db, node, 7, source->start_line,
                                              "user_publish.model_node_start");
        if (rc == 0) rc = workspace_bind_i64(db, node, 8, source->end_line,
                                              "user_publish.model_node_end");
        if (rc == 0) rc = workspace_bind_text(db, node, 9,
                                               source->properties ? source->properties : "{}",
                                               "user_publish.model_node_properties");
        if (rc == 0) rc = workspace_bind_i64(db, node, 10, (int64_t)row->source_ordinal,
                                              "user_publish.model_node_source_ordinal");
        if (rc == 0) rc = workspace_step_done(db, node, "user_publish.model_node_step");
        if (rc == 0) rc = user_database_reset_rebound_statement(
            db, node, "user_publish.model_node_reset");
        if (rc == 0)
            user_publish_metric_row(&user_publish_test_metrics.canonical_nodes_sql,
                                    6, 4, 0, false);
    }
    if (node) (void)zova_statement_finalize(node);
    return rc;
}

static int user_publish_model_hashes_tx(zova_database *db,
                                        const cbm_zova_publish_model_t *model,
                                        const user_file_key_t *file_keys, int file_key_count) {
    zova_statement *statement = NULL;
    int rc = prepare_zova(
        db,
        "INSERT INTO cbm_file_hashes_v1(file_key,content_hash,mtime_ns,size_bytes) "
        "VALUES(?1,?2,?3,?4)",
        &statement, "user_publish.model_hash_prepare");
    int count = cbm_zova_publish_model_file_hash_count(model);
    for (int i = 0; rc == 0 && i < count; i++) {
        const cbm_zova_file_hash_input_t *row = cbm_zova_publish_model_file_hash_at(model, i);
        if (!row) {
            rc = -1;
            break;
        }
        int64_t file_key = user_file_key_find(file_keys, file_key_count, row->file_path);
        if (file_key <= 0) { rc = -1; break; }
        rc = workspace_bind_i64(db, statement, 1, file_key,
                                "user_publish.model_hash_file");
        if (rc == 0) rc = workspace_bind_text(db, statement, 2, row->content_hash,
                                               "user_publish.model_hash_value");
        if (rc == 0) rc = workspace_bind_i64(db, statement, 3, row->mtime_ns,
                                              "user_publish.model_hash_mtime");
        if (rc == 0) rc = workspace_bind_i64(db, statement, 4, row->size_bytes,
                                              "user_publish.model_hash_size");
        if (rc == 0) rc = workspace_step_done(db, statement,
                                               "user_publish.model_hash_step");
        if (rc == 0) rc = user_database_reset_rebound_statement(
            db, statement, "user_publish.model_hash_reset");
        if (rc == 0)
            user_publish_metric_row(&user_publish_test_metrics.canonical_hashes_sql,
                                    3, 1, 0, false);
    }
    if (statement) (void)zova_statement_finalize(statement);
    return rc;
}

static int user_publish_model_token_metadata_tx(
    zova_database *db, const cbm_zova_publish_model_t *model, int64_t workspace_key,
    user_token_key_t **out_token_keys, int *out_token_key_count) {
    if (!out_token_keys || !out_token_key_count) return -1;
    *out_token_keys = NULL;
    *out_token_key_count = 0;
    int count = cbm_zova_publish_model_token_vector_count(model);
    user_token_key_t *token_keys = count ? calloc((size_t)count, sizeof(*token_keys)) : NULL;
    if (count && !token_keys) return -1;
    int64_t next_key = 0;
    int rc = count ? next_private_key(
        db, "SELECT COALESCE(MAX(token_key),0)+1 FROM cbm_token_vector_metadata_v1",
        &next_key, "user_publish.next_token_key") : 0;
    zova_statement *statement = NULL;
    if (rc == 0 && count) rc = prepare_zova(
        db,
        "INSERT INTO cbm_token_vector_metadata_v1(token_key,workspace_key,token,idf) "
        "VALUES(?1,?2,?3,?4)",
        &statement, "user_publish.model_token_prepare");
    for (int i = 0; rc == 0 && i < count; i++) {
        const cbm_zova_publish_token_vector_t *row =
            cbm_zova_publish_model_token_vector_at(model, i);
        if (!row || !row->source || row->ordinal != (uint64_t)i) {
            rc = -1;
            break;
        }
        token_keys[i] = (user_token_key_t){.token = row->source->token,
                                           .token_key = next_key + i};
        rc = workspace_bind_i64(db, statement, 1, token_keys[i].token_key,
                                "user_publish.model_token_key");
        if (rc == 0) rc = workspace_bind_i64(db, statement, 2, workspace_key,
                                              "user_publish.model_token_ws");
        if (rc == 0) rc = workspace_bind_text(db, statement, 3, row->source->token,
                                               "user_publish.model_token_value");
        if (rc == 0) {
            zova_statement_bind_double_request bind = {
                .statement = statement, .index = 4, .value = row->source->idf};
            rc = status_ok(zova_statement_bind_double(&bind), db,
                           "user_publish.model_token_idf");
        }
        if (rc == 0) rc = workspace_step_done(db, statement,
                                               "user_publish.model_token_step");
        if (rc == 0) rc = user_database_reset_rebound_statement(
            db, statement, "user_publish.model_token_reset");
        if (rc == 0)
            user_publish_metric_row(
                &user_publish_test_metrics.canonical_token_metadata_sql,
                2, 1, 1, false);
    }
    if (statement) (void)zova_statement_finalize(statement);
    if (rc != 0) { free(token_keys); return rc; }
    *out_token_keys = token_keys;
    *out_token_key_count = count;
    return 0;
}

typedef struct {
    double materialize_ms;
    double reset_ms;
    double nodes_ms;
    double edges_ms;
    double validate_ms;
    double key_generation_ms;
    double cleanup_ms;
} user_native_graph_profile_t;

static int user_publish_model_graph_tx(zova_database *db,
                                       const cbm_zova_publish_model_t *model,
                                       user_node_key_t **out_node_keys,
                                       user_topology_key_t **out_topology_keys,
                                       uint64_t *out_nodes, uint64_t *out_edges,
                                       user_native_graph_profile_t *profile) {
    if (!profile || !out_node_keys || !out_topology_keys) return -1;
    *out_node_keys = NULL;
    *out_topology_keys = NULL;
    memset(profile, 0, sizeof(*profile));
    struct timespec profile_mark = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &profile_mark);
    const char *workspace_id = cbm_zova_publish_model_workspace_id(model);
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
        profile->materialize_ms =
            user_publish_profile_phase("native_graph.materialize", &profile_mark);
        return -1;
    }
    int node_count = cbm_zova_publish_model_node_count(model);
    int edge_count = cbm_zova_publish_model_topology_count(model);
    zova_graph_fresh_node_input *nodes =
        node_count ? calloc((size_t)node_count, sizeof(*nodes)) : NULL;
    zova_graph_fresh_edge_payload_input *edges =
        edge_count ? calloc((size_t)edge_count, sizeof(*edges)) : NULL;
    int64_t *node_key_values = node_count ? calloc((size_t)node_count, sizeof(*node_key_values)) : NULL;
    int64_t *edge_key_values = edge_count ? calloc((size_t)edge_count, sizeof(*edge_key_values)) : NULL;
    user_node_key_t *node_keys = node_count ? calloc((size_t)node_count, sizeof(*node_keys)) : NULL;
    user_topology_key_t *topology_keys = edge_count
        ? calloc((size_t)edge_count, sizeof(*topology_keys)) : NULL;
    if ((node_count && (!nodes || !node_key_values || !node_keys)) ||
        (edge_count && (!edges || !edge_key_values || !topology_keys))) {
        profile->materialize_ms =
            user_publish_profile_phase("native_graph.materialize", &profile_mark);
        free(nodes);
        free(edges);
        free(node_key_values);
        free(edge_key_values);
        free(node_keys);
        free(topology_keys);
        profile->cleanup_ms =
            user_publish_profile_phase("native_graph.cleanup", &profile_mark);
        return -1;
    }
    for (int i = 0; i < node_count; i++) {
        const cbm_zova_publish_node_t *row = cbm_zova_publish_model_node_at(model, i);
        nodes[i] = (zova_graph_fresh_node_input){
            .node_id = row->stable_id, .kind = row->source->label,
            .target_type = ZOVA_GRAPH_TARGET_NONE, .target_namespace = NULL,
            .target_ref = NULL};
        node_keys[i].stable_id = row->stable_id;
    }
    for (int i = 0; i < edge_count; i++) {
        const cbm_zova_publish_edge_t *row = cbm_zova_publish_model_topology_at(model, i);
        edges[i] = (zova_graph_fresh_edge_payload_input){
            .from_node_ordinal = (size_t)row->source_ordinal,
            .edge_type = row->source->type,
            .to_node_ordinal = (size_t)row->target_ordinal,
            .payload = row->payload,
            .payload_len = row->payload_len};
        topology_keys[i] = (user_topology_key_t){
            .source_stable_id = row->source_stable_id,
            .edge_type = row->source->type,
            .target_stable_id = row->target_stable_id};
    }
    profile->materialize_ms =
        user_publish_profile_phase("native_graph.materialize", &profile_mark);
    int rc = graph_delete_named_if_exists(db, graph_name);
    bool use_fresh_build = false;
    zova_graph_list graph_list = {0};
    if (rc == 0) {
        rc = status_ok(zova_graphs_list(&(zova_graph_list_request){
                           .db = db, .out_list = &graph_list}),
                       db, "user_publish.model_graph_list");
        if (rc == 0) use_fresh_build = graph_list.len == 0;
    }
    zova_graph_list_free(&graph_list);
    if (rc == 0 && !use_fresh_build)
        rc = status_ok(zova_graph_create(&(zova_graph_create_request){
                           .db = db, .name = graph_name}),
                       db, "user_publish.model_graph_create");
    profile->reset_ms = user_publish_profile_phase("native_graph.reset", &profile_mark);
    if (rc == 0 && use_fresh_build) {
        user_publish_test_metrics.native_graph_fresh_calls++;
        user_publish_test_metrics.native_graph_prepared_calls++;
        rc = status_ok(zova_graph_build_fresh_prepared_keyed_with_payloads(
                           &(zova_graph_build_fresh_prepared_keyed_with_payloads_request){
                               .db = db,
                               .graph_name = graph_name,
                               .nodes = nodes,
                               .nodes_len = (size_t)node_count,
                               .edges = edges,
                               .edges_len = (size_t)edge_count,
                               .out_node_keys = node_key_values,
                               .out_node_keys_capacity = (size_t)node_count,
                               .out_edge_keys = edge_key_values,
                               .out_edge_keys_capacity = (size_t)edge_count}),
                       db, "user_publish.model_graph_fresh_prepared");
        if (rc == 0 && sidecar_test_fault("user_after_graph_nodes")) rc = -1;
        if (rc == 0 && sidecar_test_fault("user_after_graph_edges")) rc = -1;
        /* Fresh construction is one atomic Zova call, so it has no truthful
         * node/edge timing boundary. Keep the node phase empty and report the
         * complete bulk-build call in the edge phase used by existing reports. */
        profile->nodes_ms = 0.0;
        profile->edges_ms = user_publish_profile_phase("native_graph.edges", &profile_mark);
    } else if (rc == 0) {
        zova_graph_node_input *legacy_nodes =
            node_count ? calloc((size_t)node_count, sizeof(*legacy_nodes)) : NULL;
        zova_graph_edge_input *legacy_edges =
            edge_count ? calloc((size_t)edge_count, sizeof(*legacy_edges)) : NULL;
        if ((node_count && !legacy_nodes) || (edge_count && !legacy_edges)) rc = -1;
        for (int i = 0; rc == 0 && i < node_count; i++) {
            const cbm_zova_publish_node_t *row = cbm_zova_publish_model_node_at(model, i);
            legacy_nodes[i] = (zova_graph_node_input){
                .graph_name = graph_name,
                .node_id = row->stable_id,
                .kind = row->source->label,
                .target_type = ZOVA_GRAPH_TARGET_NONE,
                .target_namespace = NULL,
                .target_ref = NULL};
        }
        for (int i = 0; rc == 0 && i < edge_count; i++) {
            const cbm_zova_publish_edge_t *row = cbm_zova_publish_model_topology_at(model, i);
            legacy_edges[i] = (zova_graph_edge_input){
                .graph_name = graph_name,
                .from_node_id = row->source_stable_id,
                .edge_type = row->source->type,
                .to_node_id = row->target_stable_id};
        }
        if (rc == 0 && node_count) {
            user_publish_test_metrics.native_graph_node_calls++;
            rc = status_ok(zova_graph_node_put_many_keyed(
                               &(zova_graph_node_put_many_keyed_request){
                                   .db = db,
                                   .nodes = legacy_nodes,
                                   .nodes_len = (size_t)node_count,
                                   .out_node_keys = node_key_values,
                                   .out_node_keys_capacity = (size_t)node_count}),
                           db, "user_publish.model_graph_nodes");
        }
        if (rc == 0 && sidecar_test_fault("user_after_graph_nodes")) rc = -1;
        profile->nodes_ms = user_publish_profile_phase("native_graph.nodes", &profile_mark);
        if (rc == 0 && edge_count) {
            user_publish_test_metrics.native_graph_edge_calls++;
            rc = status_ok(zova_graph_edge_put_many_keyed(
                               &(zova_graph_edge_put_many_keyed_request){
                                   .db = db,
                                   .edges = legacy_edges,
                                   .edges_len = (size_t)edge_count,
                                   .out_edge_keys = edge_key_values,
                                   .out_edge_keys_capacity = (size_t)edge_count}),
                           db, "user_publish.model_graph_edges");
        }
        if (rc == 0 && edge_count) {
            zova_graph_edge_payload_replacement *replacements =
                calloc((size_t)edge_count, sizeof(*replacements));
            if (!replacements) rc = -1;
            for (int i = 0; rc == 0 && i < edge_count; i++) {
                const cbm_zova_publish_edge_t *row =
                    cbm_zova_publish_model_topology_at(model, i);
                replacements[i] = (zova_graph_edge_payload_replacement){
                    .edge_key = edge_key_values[i],
                    .payload = row->payload,
                    .payload_len = row->payload_len,
                };
            }
            if (rc == 0)
                rc = status_ok(zova_graph_edge_payload_replace_many(
                                   &(zova_graph_edge_payload_replace_many_request){
                                       .db = db,
                                       .graph_name = graph_name,
                                       .replacements = replacements,
                                       .replacement_count = (size_t)edge_count}),
                               db, "user_publish.model_graph_payloads");
            free(replacements);
        }
        if (rc == 0 && sidecar_test_fault("user_after_graph_edges")) rc = -1;
        profile->edges_ms = user_publish_profile_phase("native_graph.edges", &profile_mark);
        free(legacy_nodes);
        free(legacy_edges);
    }
    for (int i = 0; rc == 0 && i < node_count; i++) {
        if (node_key_values[i] <= 0) rc = -1;
        node_keys[i].node_key = node_key_values[i];
    }
    for (int i = 0; rc == 0 && i < edge_count; i++) {
        if (edge_key_values[i] <= 0) rc = -1;
        topology_keys[i].edge_key = edge_key_values[i];
    }
    zova_graph_info info = {0};
    if (rc == 0) {
        rc = status_ok(zova_graph_info_get(&(zova_graph_info_get_request){
                           .db = db, .name = graph_name, .out_info = &info}),
                       db, "user_publish.model_graph_info");
        if (rc == 0) {
            if (out_nodes) *out_nodes = info.node_count;
            if (out_edges) *out_edges = info.edge_count;
        }
        profile->validate_ms =
            user_publish_profile_phase("native_graph.validate", &profile_mark);
    }
    zova_graph_info_free(&info);
    free(nodes);
    free(edges);
    free(node_key_values);
    free(edge_key_values);
    if (rc == 0) {
        *out_node_keys = node_keys;
        *out_topology_keys = topology_keys;
    } else {
        free(node_keys);
        free(topology_keys);
    }
    profile->cleanup_ms = user_publish_profile_phase("native_graph.cleanup", &profile_mark);
    return rc;
}

static int user_publish_model_vectors_tx(zova_database *db,
                                         const cbm_zova_publish_model_t *model,
                                         const char *node_collection,
                                         const char *token_collection,
                                         const user_node_key_t *node_keys, int node_key_count,
                                         const user_token_key_t *token_keys,
                                         int token_key_count) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_publish_model_input(model);
    int node_count = cbm_zova_publish_model_node_vector_count(model);
    int token_count = cbm_zova_publish_model_token_vector_count(model);
    zova_vector_input *nodes = node_count ? calloc((size_t)node_count, sizeof(*nodes)) : NULL;
    zova_vector_input *tokens = token_count ? calloc((size_t)token_count, sizeof(*tokens)) : NULL;
    char (*node_ids)[32] = node_count ? calloc((size_t)node_count, sizeof(*node_ids)) : NULL;
    char (*token_ids)[32] = token_count ? calloc((size_t)token_count, sizeof(*token_ids)) : NULL;
    if ((node_count && (!nodes || !node_ids)) || (token_count && (!tokens || !token_ids))) {
        free(nodes);
        free(tokens);
        free(node_ids);
        free(token_ids);
        return -1;
    }
    for (int i = 0; i < node_count; i++) {
        const cbm_zova_publish_node_vector_t *row =
            cbm_zova_publish_model_node_vector_at(model, i);
        int64_t key = row && row->node_ordinal < (uint64_t)node_key_count
                          ? node_keys[row->node_ordinal].node_key
                          : 0;
        if (!row || row->ordinal != (uint64_t)i || key <= 0 ||
            strcmp(node_keys[row->node_ordinal].stable_id, row->stable_id) != 0 ||
            user_private_key_id(key, node_ids[i]) != 0) {
            free(nodes); free(tokens); free(node_ids); free(token_ids); return -1;
        }
        nodes[i] = (zova_vector_input){
            .id = node_ids[i],
            .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                       .i8_values = (const int8_t *)row->source->vector,
                       .values_len = (size_t)input->vector_dimensions}};
    }
    for (int i = 0; i < token_count; i++) {
        const cbm_zova_publish_token_vector_t *row =
            cbm_zova_publish_model_token_vector_at(model, i);
        int64_t key = row && i < token_key_count ? token_keys[i].token_key : 0;
        if (!row || !row->source || row->ordinal != (uint64_t)i || key <= 0 ||
            strcmp(token_keys[i].token, row->source->token) != 0 ||
            user_private_key_id(key, token_ids[i]) != 0) {
            free(nodes); free(tokens); free(node_ids); free(token_ids); return -1;
        }
        tokens[i] = (zova_vector_input){
            .id = token_ids[i],
            .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                       .i8_values = (const int8_t *)row->source->vector,
                       .values_len = (size_t)input->vector_dimensions}};
    }
    int rc = collection_create_i8(db, node_collection, input->vector_dimensions);
    if (rc == 0) rc = collection_create_i8(db, token_collection, input->vector_dimensions);
    if (rc == 0 && node_count)
        user_publish_test_metrics.native_node_vector_calls++;
    if (rc == 0 && node_count)
        rc = status_ok(zova_vector_put_many(&(zova_vector_put_many_request){
                           .db = db, .collection_name = node_collection,
                           .vectors = nodes, .vectors_len = (size_t)node_count}),
                       db, "user_publish.model_node_vectors");
    if (rc == 0 && sidecar_test_fault("user_after_node_vectors")) rc = -1;
    if (rc == 0 && token_count)
        user_publish_test_metrics.native_token_vector_calls++;
    if (rc == 0 && token_count)
        rc = status_ok(zova_vector_put_many(&(zova_vector_put_many_request){
                           .db = db, .collection_name = token_collection,
                           .vectors = tokens, .vectors_len = (size_t)token_count}),
                       db, "user_publish.model_token_vectors");
    if (rc == 0 && sidecar_test_fault("user_after_token_vectors")) rc = -1;
    free(nodes);
    free(tokens);
    free(node_ids);
    free(token_ids);
    return rc;
}

static int user_delta_delete_ids_tx(zova_database *db, int64_t workspace_key,
                                    const char *sql, const char *phase,
                                    const cbm_zova_workspace_delta_t *delta,
                                    const char *(*item_at)(const cbm_zova_workspace_delta_t *, int),
                                    uint64_t count) {
    zova_statement *statement = NULL;
    int rc = count ? prepare_zova(db, sql, &statement, phase) : 0;
    for (uint64_t i = 0; rc == 0 && i < count; i++) {
        const char *id = item_at(delta, (int)i);
        if (!id) { rc = -1; break; }
        rc = workspace_bind_i64(db, statement, 1, workspace_key, phase);
        if (rc == 0) rc = workspace_bind_text(db, statement, 2, id, phase);
        if (rc == 0) rc = workspace_step_done(db, statement, phase);
        if (rc == 0) rc = user_database_reset_statement(db, statement, phase);
    }
    if (statement) (void)zova_statement_finalize(statement);
    return rc;
}

static int user_delta_delete_native_tx(zova_database *db, const char *workspace_id,
                                       const char *node_collection,
                                       const char *token_collection,
                                       const cbm_zova_workspace_delta_t *delta,
                                       const cbm_zova_workspace_delta_metrics_t *metrics) {
    const char **node_vectors = metrics->node_vector_deletes
        ? calloc((size_t)metrics->node_vector_deletes, sizeof(*node_vectors)) : NULL;
    const char **token_vectors = metrics->token_vector_deletes
        ? calloc((size_t)metrics->token_vector_deletes, sizeof(*token_vectors)) : NULL;
    char (*node_vector_ids)[32] = metrics->node_vector_deletes
        ? calloc((size_t)metrics->node_vector_deletes, sizeof(*node_vector_ids)) : NULL;
    char (*token_vector_ids)[32] = metrics->token_vector_deletes
        ? calloc((size_t)metrics->token_vector_deletes, sizeof(*token_vector_ids)) : NULL;
    if ((metrics->node_vector_deletes && !node_vectors) ||
        (metrics->token_vector_deletes && !token_vectors) ||
        (metrics->node_vector_deletes && !node_vector_ids) ||
        (metrics->token_vector_deletes && !token_vector_ids)) {
        free(node_vectors); free(token_vectors); free(node_vector_ids); free(token_vector_ids);
        return -1;
    }
    int64_t workspace_key = 0;
    int rc = workspace_key_for_id(db, workspace_id, &workspace_key);
    for (uint64_t i = 0; rc == 0 && i < metrics->node_vector_deletes; i++) {
        int64_t node_key = 0;
        const char *stable_id = cbm_zova_workspace_delta_node_vector_delete_at(delta, (int)i);
        if (!stable_id || node_key_for_id(db, workspace_key, stable_id, &node_key) != 0 ||
            user_private_key_id(node_key, node_vector_ids[i]) != 0) rc = -1;
        node_vectors[i] = node_vector_ids[i];
    }
    zova_statement *token_key_statement = NULL;
    if (rc == 0 && metrics->token_vector_deletes)
        rc = prepare_zova(db, "SELECT token_key FROM cbm_token_vector_metadata_v1 "
                              "WHERE workspace_key=?1 AND token=?2", &token_key_statement,
                          "user_delta.token_key_prepare");
    for (uint64_t i = 0; rc == 0 && i < metrics->token_vector_deletes; i++) {
        const char *token = cbm_zova_workspace_delta_token_vector_delete_at(delta, (int)i);
        if (zova_statement_reset(token_key_statement) != ZOVA_OK ||
            zova_statement_clear_bindings(token_key_statement) != ZOVA_OK ||
            workspace_bind_i64(db, token_key_statement, 1, workspace_key,
                               "user_delta.token_key_ws") != 0 ||
            workspace_bind_text(db, token_key_statement, 2, token,
                                "user_delta.token_key_token") != 0) { rc = -1; break; }
        zova_step_result step = ZOVA_STEP_DONE;
        int64_t token_key = 0;
        if (status_ok(zova_statement_step(&(zova_statement_step_request){
                .statement=token_key_statement,.out_result=&step}),db,
                "user_delta.token_key_step") != 0 || step != ZOVA_STEP_ROW ||
            status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                .statement=token_key_statement,.index=0,.out_value=&token_key}),db,
                "user_delta.token_key_value") != 0 ||
            user_private_key_id(token_key, token_vector_ids[i]) != 0) { rc = -1; break; }
        token_vectors[i] = token_vector_ids[i];
    }
    if (token_key_statement) (void)zova_statement_finalize(token_key_statement);
    if (rc == 0 && metrics->node_vector_deletes)
        rc = status_ok(zova_vector_delete_many(&(zova_vector_delete_many_request){
              .db=db,.collection_name=node_collection,.vector_ids=node_vectors,
              .vector_count=(size_t)metrics->node_vector_deletes}),
              db,"user_delta.node_vector_delete");
    if (rc == 0 && metrics->token_vector_deletes)
        rc = status_ok(zova_vector_delete_many(&(zova_vector_delete_many_request){
              .db=db,.collection_name=token_collection,.vector_ids=token_vectors,
              .vector_count=(size_t)metrics->token_vector_deletes}),
              db,"user_delta.token_vector_delete");
    if (rc == 0 && metrics->token_vector_deletes)
        rc = user_delta_delete_ids_tx(db, workspace_key,
            "DELETE FROM cbm_token_vector_metadata_v1 WHERE workspace_key=?1 AND token=?2",
            "user_delta.token_delete", delta,
            cbm_zova_workspace_delta_token_vector_delete_at,
            metrics->token_vector_deletes);
    free(node_vectors); free(token_vectors); free(node_vector_ids); free(token_vector_ids);
    if (rc == 0 && sidecar_test_fault("user_delta_after_vector_deletes")) rc = -1;
    if (rc != 0) return rc;

    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0)
        return -1;
    zova_graph_edge_input *edges = metrics->topology_deletes
        ? calloc((size_t)metrics->topology_deletes, sizeof(*edges)) : NULL;
    const char **nodes = metrics->node_deletes
        ? calloc((size_t)metrics->node_deletes, sizeof(*nodes)) : NULL;
    if ((metrics->topology_deletes && !edges) || (metrics->node_deletes && !nodes)) {
        free(edges); free(nodes); return -1;
    }
    for (uint64_t i = 0; rc == 0 && i < metrics->topology_deletes; i++) {
        const cbm_zova_delta_topology_edge_t *edge =
            cbm_zova_workspace_delta_topology_delete_at(delta, (int)i);
        if (!edge) { rc = -1; break; }
        edges[i] = (zova_graph_edge_input){.graph_name=graph_name,
            .from_node_id=edge->source_stable_id,.edge_type=edge->edge_type,
            .to_node_id=edge->target_stable_id};
    }
    for (uint64_t i = 0; i < metrics->node_deletes; i++)
        nodes[i] = cbm_zova_workspace_delta_node_delete_at(delta, (int)i);
    if (metrics->topology_deletes)
        rc = status_ok(zova_graph_edge_delete_many(&(zova_graph_edge_delete_many_request){
             .db=db,.edges=edges,.edges_len=(size_t)metrics->topology_deletes}),
             db,"user_delta.graph_edge_delete");
    if (rc == 0 && metrics->node_deletes)
        rc = status_ok(zova_graph_node_delete_many(&(zova_graph_node_delete_many_request){
             .db=db,.graph_name=graph_name,.node_ids=nodes,
             .node_count=(size_t)metrics->node_deletes}),
             db,"user_delta.graph_node_delete");
    free(edges); free(nodes);
    if (rc == 0 && sidecar_test_fault("user_delta_after_graph_deletes")) rc = -1;
    return rc;
}

static int user_delta_delete_canonical_tx(
    zova_database *db, int64_t workspace_key,
    const cbm_zova_workspace_delta_t *delta,
    const cbm_zova_workspace_delta_metrics_t *metrics) {
    int rc = 0;
    zova_statement *node_delete = NULL;
    if (rc == 0 && metrics->node_deletes) rc = prepare_zova(
        db, "DELETE FROM cbm_nodes_v1 WHERE workspace_key=?1 AND zova_node_key=?2",
        &node_delete, "user_delta.node_delete_prepare");
    for (uint64_t i = 0; rc == 0 && i < metrics->node_deletes; i++) {
        const char *stable_id = cbm_zova_workspace_delta_node_delete_at(delta, (int)i);
        int64_t node_key = 0;
        if (!stable_id || node_key_for_id(db, workspace_key, stable_id, &node_key) != 0) {
            rc = -1; break;
        }
        rc = workspace_bind_i64(db, node_delete, 1, workspace_key,
                                "user_delta.node_delete_ws");
        if (rc == 0) rc = workspace_bind_i64(db, node_delete, 2, node_key,
                                              "user_delta.node_delete_key");
        if (rc == 0) rc = workspace_step_done(db, node_delete,
                                               "user_delta.node_delete_step");
        if (rc == 0) rc = user_database_reset_statement(db, node_delete,
                                                         "user_delta.node_delete_reset");
    }
    if (node_delete) (void)zova_statement_finalize(node_delete);
    if (rc == 0) rc = user_delta_delete_ids_tx(db, workspace_key,
        "DELETE FROM cbm_file_hashes_v1 WHERE file_key=(SELECT file_key FROM cbm_files_v1 "
        "WHERE workspace_key=?1 AND file_path=?2)",
        "user_delta.hash_delete", delta, cbm_zova_workspace_delta_file_hash_delete_at,
        metrics->file_hash_deletes);
    if (rc == 0 && sidecar_test_fault("user_delta_after_canonical_deletes")) rc = -1;
    return rc;
}

static int user_delta_upsert_nodes_tx(zova_database *db, int64_t workspace_key,
                                      const cbm_zova_publish_model_t *model,
                                      const cbm_zova_workspace_delta_t *delta,
                                      const cbm_zova_workspace_delta_metrics_t *metrics,
                                      const user_file_key_t *file_keys,
                                      int file_key_count,
                                      const user_node_key_t *node_keys,
                                      int node_key_count) {
    (void)model;
    zova_statement *node = NULL;
    int total = (int)(metrics->node_inserts + metrics->node_updates);
    int rc = 0;
    if (rc == 0 && total) rc = prepare_zova(db,
        "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,file_key,"
        "start_line,end_line,properties,source_ordinal) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
        "ON CONFLICT(zova_node_key) DO UPDATE SET "
        "label=excluded.label,name=excluded.name,qualified_name=excluded.qualified_name,"
        "file_key=excluded.file_key,start_line=excluded.start_line,end_line=excluded.end_line,"
        "properties=excluded.properties,source_ordinal=excluded.source_ordinal",
        &node,"user_delta.node_upsert_prepare");
    for (int i = 0; rc == 0 && i < total; i++) {
        const cbm_zova_publish_node_t *row = i < (int)metrics->node_inserts
            ? cbm_zova_workspace_delta_node_insert_at(delta, i)
            : cbm_zova_workspace_delta_node_update_at(delta, i - (int)metrics->node_inserts);
        const CBMDumpNode *source = row->source;
        int64_t node_key = user_node_key_find(node_keys, node_key_count, row->stable_id);
        int64_t file_key = user_file_key_find(file_keys, file_key_count,
                                              source->file_path);
        if (rc == 0 && (node_key <= 0 || file_key <= 0)) rc = -1;
        if (rc==0) rc=workspace_bind_i64(db,node,1,node_key,"user_delta.node_key");
        if (rc==0) rc=workspace_bind_i64(db,node,2,workspace_key,"user_delta.node_ws");
        if (rc==0) rc=workspace_bind_text(db,node,3,source->label,"user_delta.node_label");
        if (rc==0) rc=workspace_bind_text(db,node,4,source->name,"user_delta.node_name");
        if (rc==0) rc=workspace_bind_text(db,node,5,source->qualified_name,"user_delta.node_qn");
        if (rc==0) rc=workspace_bind_i64(db,node,6,file_key,"user_delta.node_file");
        if (rc==0) rc=workspace_bind_i64(db,node,7,source->start_line,"user_delta.node_start");
        if (rc==0) rc=workspace_bind_i64(db,node,8,source->end_line,"user_delta.node_end");
        if (rc==0) rc=workspace_bind_text(db,node,9,source->properties?source->properties:"{}","user_delta.node_props");
        if (rc==0) rc=workspace_bind_i64(db,node,10,(int64_t)row->source_ordinal,
                                         "user_delta.node_source_ordinal");
        if (rc==0) rc=workspace_step_done(db,node,"user_delta.node_step");
        if (rc==0) rc=user_database_reset_statement(db,node,"user_delta.node_reset");
    }
    if (node) (void)zova_statement_finalize(node);
    return rc;
}

static int user_delta_token_keys_tx(zova_database *db, int64_t workspace_key,
                                    const cbm_zova_workspace_delta_t *delta,
                                    const cbm_zova_workspace_delta_metrics_t *metrics,
                                    user_token_key_t **out_keys, int *out_count) {
    if (!out_keys || !out_count) return -1;
    *out_keys = NULL;
    *out_count = 0;
    int count = (int)metrics->token_vector_upserts;
    if (count == 0) return 0;
    user_token_key_t *keys = calloc((size_t)count, sizeof(*keys));
    if (!keys) return -1;
    int64_t next_key = 0;
    int rc = next_private_key(
        db, "SELECT COALESCE(MAX(token_key),0)+1 FROM cbm_token_vector_metadata_v1",
        &next_key, "user_delta.next_token_key");
    zova_statement *statement = NULL;
    if (rc == 0) rc = prepare_zova(db,
        "SELECT token_key FROM cbm_token_vector_metadata_v1 "
        "WHERE workspace_key=?1 AND token=?2", &statement,
        "user_delta.token_lookup_prepare");
    int64_t allocated = 0;
    for (int i = 0; rc == 0 && i < count; i++) {
        const cbm_zova_publish_token_vector_t *row =
            cbm_zova_workspace_delta_token_vector_upsert_at(delta, i);
        if (!row || !row->source || !row->source->token) { rc = -1; break; }
        if (zova_statement_reset(statement) != ZOVA_OK ||
            zova_statement_clear_bindings(statement) != ZOVA_OK ||
            workspace_bind_i64(db, statement, 1, workspace_key,
                               "user_delta.token_lookup_ws") != 0 ||
            workspace_bind_text(db, statement, 2, row->source->token,
                                "user_delta.token_lookup_token") != 0) { rc = -1; break; }
        zova_step_result step = ZOVA_STEP_DONE;
        if (status_ok(zova_statement_step(&(zova_statement_step_request){
                .statement=statement,.out_result=&step}),db,
                "user_delta.token_lookup_step") != 0) { rc = -1; break; }
        int64_t key = 0;
        if (step == ZOVA_STEP_ROW) {
            if (status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                    .statement=statement,.index=0,.out_value=&key}),db,
                    "user_delta.token_lookup_value") != 0) rc = -1;
        } else if (step == ZOVA_STEP_DONE) {
            key = next_key + allocated++;
        } else rc = -1;
        if (key <= 0) rc = -1;
        keys[i] = (user_token_key_t){.token=row->source->token,.token_key=key};
    }
    if (statement) (void)zova_statement_finalize(statement);
    if (rc != 0) { free(keys); return rc; }
    if (count > 1) qsort(keys, (size_t)count, sizeof(*keys), user_token_key_compare);
    *out_keys = keys;
    *out_count = count;
    return 0;
}

static int user_delta_put_native_tx(zova_database *db, const char *workspace_id,
                                    int64_t workspace_key,
                                    const char *node_collection,const char *token_collection,
                                    const cbm_zova_workspace_delta_t *delta,
                                    const cbm_zova_workspace_delta_metrics_t *metrics,
                                    const cbm_zova_publish_model_t *model,
                                    user_node_key_t **out_node_keys,
                                    int *out_node_key_count,
                                    int64_t **out_edge_keys,
                                    const user_token_key_t *token_keys,
                                    int token_key_count) {
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if(cbm_zova_workspace_graph_name(workspace_id,graph_name,sizeof(graph_name))!=0)return -1;
    uint64_t node_count=metrics->node_inserts+metrics->node_updates;
    zova_graph_node_input *nodes=node_count?calloc((size_t)node_count,sizeof(*nodes)):NULL;
    zova_graph_edge_input *edges=metrics->edge_inserts?calloc((size_t)metrics->edge_inserts,sizeof(*edges)):NULL;
    int64_t *node_key_values=node_count?calloc((size_t)node_count,sizeof(*node_key_values)):NULL;
    user_node_key_t *node_keys=node_count?calloc((size_t)node_count,sizeof(*node_keys)):NULL;
    int64_t *edge_keys=metrics->edge_inserts?calloc((size_t)metrics->edge_inserts,sizeof(*edge_keys)):NULL;
    if((node_count&&(!nodes||!node_key_values||!node_keys))||
       (metrics->edge_inserts&&(!edges||!edge_keys))){free(nodes);free(edges);free(node_key_values);free(node_keys);free(edge_keys);return -1;}
    for(uint64_t i=0;i<node_count;i++){
        const cbm_zova_publish_node_t *row=i<metrics->node_inserts?
            cbm_zova_workspace_delta_node_insert_at(delta,(int)i):
            cbm_zova_workspace_delta_node_update_at(delta,(int)(i-metrics->node_inserts));
        nodes[i]=(zova_graph_node_input){.graph_name=graph_name,.node_id=row->stable_id,
            .kind=row->source->label,.target_type=ZOVA_GRAPH_TARGET_NONE,
            .target_namespace=NULL,.target_ref=NULL};
        node_keys[i].stable_id=row->stable_id;
    }
    for(uint64_t i=0;i<metrics->edge_inserts;i++){
        const cbm_zova_publish_edge_t *row=cbm_zova_workspace_delta_edge_insert_at(delta,(int)i);
        edges[i]=(zova_graph_edge_input){.graph_name=graph_name,.from_node_id=row->source_stable_id,
            .edge_type=row->source->type,.to_node_id=row->target_stable_id};
    }
    int rc=node_count?status_ok(zova_graph_node_put_many_keyed(&(zova_graph_node_put_many_keyed_request){
        .db=db,.nodes=nodes,.nodes_len=(size_t)node_count,.out_node_keys=node_key_values,
        .out_node_keys_capacity=(size_t)node_count}),db,"user_delta.graph_nodes"):0;
    for(uint64_t i=0;rc==0&&i<node_count;i++){
        node_keys[i].node_key=node_key_values[i];
        if(node_keys[i].node_key<=0)rc=-1;
    }
    if(rc==0&&node_count>1)qsort(node_keys,(size_t)node_count,sizeof(*node_keys),user_node_key_compare);
    if(rc==0&&metrics->edge_inserts)rc=status_ok(zova_graph_edge_put_many_keyed(
        &(zova_graph_edge_put_many_keyed_request){.db=db,.edges=edges,
        .edges_len=(size_t)metrics->edge_inserts,.out_edge_keys=edge_keys,
        .out_edge_keys_capacity=(size_t)metrics->edge_inserts}),db,"user_delta.graph_edges");
    for(uint64_t i=0;rc==0&&i<metrics->edge_inserts;i++)if(edge_keys[i]<=0)rc=-1;
    int topology_count = cbm_zova_publish_model_topology_count(model);
    bool *affected = topology_count
        ? calloc((size_t)topology_count, sizeof(*affected)) : NULL;
    zova_graph_edge_payload_replacement *replacements = topology_count
        ? calloc((size_t)topology_count, sizeof(*replacements)) : NULL;
    if (topology_count && (!affected || !replacements)) rc = -1;
    for (uint64_t i = 0; rc == 0 && i < metrics->edge_inserts; i++) {
        const cbm_zova_publish_edge_t *row =
            cbm_zova_workspace_delta_edge_insert_at(delta, (int)i);
        if (!row || row->topology_ordinal >= (uint64_t)topology_count) rc = -1;
        else affected[row->topology_ordinal] = true;
    }
    for (uint64_t i = 0; rc == 0 && i < metrics->edge_deletes; i++) {
        const cbm_zova_delta_edge_delete_t *deleted =
            cbm_zova_workspace_delta_edge_delete_at(delta, (int)i);
        if (!deleted) { rc = -1; break; }
        const cbm_zova_publish_edge_t *row = user_publish_model_topology_find(
            model, deleted->source_stable_id, deleted->edge_type,
            deleted->target_stable_id);
        if (row) {
            if (row->ordinal >= (uint64_t)topology_count) rc = -1;
            else affected[row->ordinal] = true;
        }
    }
    size_t replacement_count = 0;
    for (int i = 0; rc == 0 && i < topology_count; i++) {
        if (!affected[i]) continue;
        const cbm_zova_publish_edge_t *row =
            cbm_zova_publish_model_topology_at(model, i);
        int64_t edge_key = 0;
        if (!row || edge_key_for_identity(db, workspace_id,
                row->source_stable_id, row->source->type,
                row->target_stable_id, &edge_key) != 0 || edge_key <= 0) {
            rc = -1;
            break;
        }
        replacements[replacement_count++] = (zova_graph_edge_payload_replacement){
            .edge_key = edge_key,
            .payload = row->payload,
            .payload_len = row->payload_len,
        };
    }
    if (rc == 0 && replacement_count > 0)
        rc = status_ok(zova_graph_edge_payload_replace_many(
                           &(zova_graph_edge_payload_replace_many_request){
                               .db = db, .graph_name = graph_name,
                               .replacements = replacements,
                               .replacement_count = replacement_count}),
                       db, "user_delta.graph_payloads");
    free(affected);
    free(replacements);
    free(nodes);free(edges);free(node_key_values);
    if(rc==0&&sidecar_test_fault("user_delta_after_graph_puts"))rc=-1;
    if(rc!=0){free(node_keys);free(edge_keys);return rc;}

    const cbm_zova_workspace_generation_input_t *input=cbm_zova_publish_model_input(model);
    zova_vector_input *node_vectors=metrics->node_vector_upserts?calloc((size_t)metrics->node_vector_upserts,sizeof(*node_vectors)):NULL;
    zova_vector_input *token_vectors=metrics->token_vector_upserts?calloc((size_t)metrics->token_vector_upserts,sizeof(*token_vectors)):NULL;
    char (*node_vector_ids)[32]=metrics->node_vector_upserts?calloc((size_t)metrics->node_vector_upserts,sizeof(*node_vector_ids)):NULL;
    char (*token_vector_ids)[32]=metrics->token_vector_upserts?calloc((size_t)metrics->token_vector_upserts,sizeof(*token_vector_ids)):NULL;
    if((metrics->node_vector_upserts&&(!node_vectors||!node_vector_ids))||
       (metrics->token_vector_upserts&&(!token_vectors||!token_vector_ids))){
        free(node_vectors);free(token_vectors);free(node_vector_ids);free(token_vector_ids);
        free(node_keys);free(edge_keys);return -1;
    }
    for(uint64_t i=0;i<metrics->node_vector_upserts;i++){
        const cbm_zova_publish_node_vector_t *row=cbm_zova_workspace_delta_node_vector_upsert_at(delta,(int)i);
        int64_t key=user_node_key_find(node_keys,(int)node_count,row->stable_id);
        if(key<=0&&node_key_for_id(db,workspace_key,row->stable_id,&key)!=0)rc=-1;
        if(rc==0&&user_private_key_id(key,node_vector_ids[i])!=0)rc=-1;
        node_vectors[i]=(zova_vector_input){.id=node_vector_ids[i],.values={.element_type=ZOVA_VECTOR_ELEMENT_TYPE_I8,
            .i8_values=(const int8_t*)row->source->vector,.values_len=(size_t)input->vector_dimensions}};
    }
    for(uint64_t i=0;i<metrics->token_vector_upserts;i++){
        const cbm_zova_publish_token_vector_t *row=cbm_zova_workspace_delta_token_vector_upsert_at(delta,(int)i);
        int64_t key=user_token_key_find(token_keys,token_key_count,row->source->token);
        if(user_private_key_id(key,token_vector_ids[i])!=0)rc=-1;
        token_vectors[i]=(zova_vector_input){.id=token_vector_ids[i],.values={.element_type=ZOVA_VECTOR_ELEMENT_TYPE_I8,
            .i8_values=(const int8_t*)row->source->vector,.values_len=(size_t)input->vector_dimensions}};
    }
    if(rc==0&&metrics->node_vector_upserts)rc=status_ok(zova_vector_put_many(&(zova_vector_put_many_request){
        .db=db,.collection_name=node_collection,.vectors=node_vectors,
        .vectors_len=(size_t)metrics->node_vector_upserts}),db,"user_delta.node_vectors");
    if(rc==0&&metrics->token_vector_upserts)rc=status_ok(zova_vector_put_many(&(zova_vector_put_many_request){
        .db=db,.collection_name=token_collection,.vectors=token_vectors,
        .vectors_len=(size_t)metrics->token_vector_upserts}),db,"user_delta.token_vectors");
    free(node_vectors);free(token_vectors);free(node_vector_ids);free(token_vector_ids);
    if(rc==0&&sidecar_test_fault("user_delta_after_vector_puts"))rc=-1;
    if(rc==0){*out_node_keys=node_keys;*out_node_key_count=(int)node_count;*out_edge_keys=edge_keys;}
    else{free(node_keys);free(edge_keys);}
    return rc;
}

static int user_delta_upsert_metadata_tx(
    zova_database *db, const char *workspace_id, int64_t workspace_key,
    const cbm_zova_workspace_delta_t *delta,
    const cbm_zova_workspace_delta_metrics_t *metrics,
    const user_file_key_t *file_keys, int file_key_count,
    const user_token_key_t *token_keys, int token_key_count) {
    zova_statement *statement = NULL;
    int rc = metrics->token_vector_upserts ? prepare_zova(db,
        "INSERT INTO cbm_token_vector_metadata_v1(token_key,workspace_key,token,idf) "
        "VALUES(?1,?2,?3,?4) ON CONFLICT(workspace_key,token) DO UPDATE SET "
        "idf=excluded.idf",&statement,"user_delta.token_upsert_prepare") : 0;
    for(uint64_t i=0;rc==0&&i<metrics->token_vector_upserts;i++){
        const cbm_zova_publish_token_vector_t *row=
            cbm_zova_workspace_delta_token_vector_upsert_at(delta,(int)i);
        int64_t token_key=user_token_key_find(token_keys,token_key_count,row->source->token);
        if(token_key<=0)rc=-1;
        if(rc==0)rc=workspace_bind_i64(db,statement,1,token_key,"user_delta.token_key");
        if(rc==0)rc=workspace_bind_i64(db,statement,2,workspace_key,"user_delta.token_ws");
        if(rc==0)rc=workspace_bind_text(db,statement,3,row->source->token,"user_delta.token_value");
        if(rc==0){zova_statement_bind_double_request bind={.statement=statement,.index=4,.value=row->source->idf};
            rc=status_ok(zova_statement_bind_double(&bind),db,"user_delta.token_idf");}
        if(rc==0)rc=workspace_step_done(db,statement,"user_delta.token_step");
        if(rc==0)rc=user_database_reset_statement(db,statement,"user_delta.token_reset");
    }
    if(statement)(void)zova_statement_finalize(statement);
    statement=NULL;
    if(rc==0&&metrics->file_hash_upserts)rc=prepare_zova(db,
        "INSERT INTO cbm_file_hashes_v1(file_key,content_hash,mtime_ns,size_bytes) "
        "VALUES(?1,?2,?3,?4) ON CONFLICT(file_key) DO UPDATE SET "
        "content_hash=excluded.content_hash,mtime_ns=excluded.mtime_ns,size_bytes=excluded.size_bytes",
        &statement,"user_delta.hash_upsert_prepare");
    for(uint64_t i=0;rc==0&&i<metrics->file_hash_upserts;i++){
        const cbm_zova_file_hash_input_t *row=cbm_zova_workspace_delta_file_hash_upsert_at(delta,(int)i);
        int64_t file_key = user_file_key_find(file_keys, file_key_count, row->file_path);
        if (file_key <= 0) rc=-1;
        if(rc==0)rc=workspace_bind_i64(db,statement,1,file_key,"user_delta.hash_file_key");
        if(rc==0)rc=workspace_bind_text(db,statement,2,row->content_hash,"user_delta.hash_value");
        if(rc==0)rc=workspace_bind_i64(db,statement,3,row->mtime_ns,"user_delta.hash_mtime");
        if(rc==0)rc=workspace_bind_i64(db,statement,4,row->size_bytes,"user_delta.hash_size");
        if(rc==0)rc=workspace_step_done(db,statement,"user_delta.hash_step");
        if(rc==0)rc=user_database_reset_statement(db,statement,"user_delta.hash_reset");
    }
    if(statement)(void)zova_statement_finalize(statement);
    statement=NULL;
    if(rc==0&&cbm_zova_workspace_delta_replaces_summary(delta))
        rc=user_database_write_summary(db,workspace_id,cbm_zova_workspace_delta_summary(delta));
    const bool may_have_orphan_files = metrics->node_deletes || metrics->node_updates ||
                                       metrics->file_hash_deletes;
    if (rc == 0 && may_have_orphan_files) {
        rc = prepare_zova(db,
            "DELETE FROM cbm_files_v1 WHERE workspace_key=?1 "
            "AND NOT EXISTS(SELECT 1 FROM cbm_nodes_v1 n "
            "WHERE n.workspace_key=cbm_files_v1.workspace_key "
            "AND n.file_key=cbm_files_v1.file_key) "
            "AND NOT EXISTS(SELECT 1 FROM cbm_file_hashes_v1 h WHERE h.file_key=cbm_files_v1.file_key)",
            &statement, "user_delta.files_cleanup_prepare");
        if (rc == 0) rc = workspace_bind_i64(db, statement, 1, workspace_key,
                                              "user_delta.files_cleanup_ws");
        if (rc == 0) rc = workspace_step_done(db, statement,
                                               "user_delta.files_cleanup_step");
        if (statement) (void)zova_statement_finalize(statement);
    }
    if(rc==0&&sidecar_test_fault("user_delta_after_metadata_upserts"))rc=-1;
    return rc;
}

static int cbm_zova_user_database_publish_delta_tx(
    zova_database *db, const cbm_zova_publish_model_t *model,
    const cbm_zova_workspace_delta_t *delta,
    cbm_zova_workspace_generation_result_t *out_result) {
    if(!db||!model||!delta||!out_result)return -1;
    const cbm_zova_workspace_generation_input_t *input=cbm_zova_publish_model_input(model);
    const char *model_workspace_id=cbm_zova_publish_model_workspace_id(model);
    const cbm_zova_workspace_generation_result_t *digests=cbm_zova_publish_model_digests(model);
    if(!input||!model_workspace_id||!digests)return -1;
    *out_result=*digests;
    cbm_zova_workspace_delta_metrics_t metrics={0};
    cbm_zova_workspace_delta_metrics(delta,&metrics);
    cbm_zova_publish_model_metrics_t model_metrics={0};
    cbm_zova_publish_model_metrics(model,&model_metrics);
    out_result->publication_mode=CBM_ZOVA_PUBLICATION_MODE_DELTA;
    out_result->inserted_count=metrics.node_inserts+metrics.edge_inserts;
    out_result->updated_count=metrics.node_updates+metrics.node_vector_upserts+
        metrics.token_vector_upserts+metrics.file_hash_upserts+metrics.summary_replacements;
    out_result->deleted_count=metrics.node_deletes+metrics.edge_deletes+
        metrics.node_vector_deletes+metrics.token_vector_deletes+metrics.file_hash_deletes;
    out_result->normalization_ms=model_metrics.normalization_ms;
    out_result->model_nodes_ms=model_metrics.nodes_ms;
    out_result->model_edges_ms=model_metrics.edges_ms;
    out_result->model_edge_endpoint_ms=model_metrics.prepared_endpoint_ms;
    out_result->model_edge_sort_ms=model_metrics.prepared_topology_sort_ms;
    out_result->model_edge_group_ms=model_metrics.prepared_topology_group_ms;
    out_result->model_edge_payload_ms=model_metrics.prepared_payload_ms;
    out_result->model_edge_digest_ms=model_metrics.prepared_topology_digest_ms;
    out_result->model_edge_default_payloads=
        model_metrics.prepared_single_default_payload_count;
    out_result->model_edge_payload_scratch_edges=
        model_metrics.prepared_payload_scratch_edge_count;
    out_result->model_hashes_ms=model_metrics.hashes_ms;
    out_result->model_vectors_ms=model_metrics.vectors_ms;
    out_result->model_digests_ms=model_metrics.digests_ms;
    out_result->diff_ms=metrics.diff_ms;
    out_result->full_clear_count=0;
    out_result->unchanged_rewrite_count=0;
    out_result->nodes_inserted=metrics.node_inserts;
    out_result->nodes_updated=metrics.node_updates;
    out_result->nodes_deleted=metrics.node_deletes;
    out_result->edges_inserted=metrics.edge_inserts;
    out_result->edges_deleted=metrics.edge_deletes;
    out_result->node_vectors_upserted=metrics.node_vector_upserts;
    out_result->node_vectors_deleted=metrics.node_vector_deletes;
    out_result->token_vectors_upserted=metrics.token_vector_upserts;
    out_result->token_vectors_deleted=metrics.token_vector_deletes;
    if(cbm_zova_register_sql_functions(db)!=0)return -1;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX]={0};
    if(user_workspace_id_tx(db,input->root_path,workspace_id,sizeof(workspace_id))!=0||
       strcmp(workspace_id,model_workspace_id)!=0)return -1;
    int64_t workspace_key = 0;
    if (workspace_key_for_id(db, workspace_id, &workspace_key) != 0) return -1;
    int64_t expected_generation=cbm_zova_workspace_delta_expected_generation(delta);
    int64_t active_generation=0;
    if(expected_generation<=0||
       zova_query_i64(db,
        "SELECT r.active_generation FROM cbm_workspace_registry r "
        "JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key "
        "AND g.generation=r.active_generation AND g.state='ready' "
        "WHERE r.workspace_id=?1",workspace_id,&active_generation,
        "user_delta.active_generation")!=0||
       active_generation!=expected_generation)return -1;
    int64_t current_max=0;
    if(zova_query_i64(db,"SELECT COALESCE(MAX(generation),0) FROM cbm_database_generation_v1 "
        "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
        "WHERE workspace_id=?1)",workspace_id,&current_max,"user_delta.current_generation")!=0||
       current_max<=0||current_max==INT64_MAX)return -1;
    int64_t generation=current_max+1;
    if(user_publish_insert_generation_tx(db,workspace_id,generation)!=0)return -1;
    if(sidecar_test_fault("user_delta_after_generation"))return -1;

    char node_collection[ZV_ID_MAX+CBM_ZOVA_WORKSPACE_ID_MAX+96];
    char token_collection[ZV_ID_MAX+CBM_ZOVA_WORKSPACE_ID_MAX+96];
    if(cbm_zova_workspace_node_vector_collection_name(workspace_id,input->model_fingerprint,
        input->vector_dimensions,node_collection,sizeof(node_collection))!=0||
       cbm_zova_workspace_token_vector_collection_name(workspace_id,input->model_fingerprint,
        input->vector_dimensions,token_collection,sizeof(token_collection))!=0)return -1;
    int rc=0;
    user_file_key_t *file_keys = NULL;
    int file_key_count = 0;
    user_node_key_t *delta_node_keys = NULL;
    int delta_node_key_count = 0;
    int64_t *delta_edge_keys = NULL;
    user_token_key_t *delta_token_keys = NULL;
    int delta_token_key_count = 0;
    if(rc==0)rc=user_delta_delete_canonical_tx(db,workspace_key,delta,&metrics);
    if(rc==0)rc=user_delta_delete_native_tx(db,workspace_id,node_collection,token_collection,
                                            delta,&metrics);
    if(rc==0)rc=user_publish_project_tx(db,workspace_id,input->root_path,input->project,input->indexed_at);
    if(rc==0)rc=user_delta_file_keys_tx(db,workspace_key,delta,&metrics,
                                        &file_keys,&file_key_count);
    if(rc==0)rc=user_delta_token_keys_tx(db,workspace_key,delta,&metrics,
                                         &delta_token_keys,&delta_token_key_count);
    if(rc==0)rc=user_delta_put_native_tx(db,workspace_id,workspace_key,
                                         node_collection,token_collection,
                                         delta,&metrics,model,&delta_node_keys,
                                         &delta_node_key_count,&delta_edge_keys,
                                         delta_token_keys,delta_token_key_count);
    if(rc==0)rc=user_delta_upsert_nodes_tx(db,workspace_key,model,delta,&metrics,
                                           file_keys,file_key_count,delta_node_keys,
                                           delta_node_key_count);
    if(rc==0&&sidecar_test_fault("user_delta_after_node_upserts"))rc=-1;
    if(rc==0&&sidecar_test_fault("user_delta_after_edge_inserts"))rc=-1;
    if(rc==0)rc=user_delta_upsert_metadata_tx(db,workspace_id,workspace_key,delta,&metrics,
                                               file_keys,file_key_count,delta_token_keys,
                                               delta_token_key_count);
    out_result->generation=generation;

    char graph_name[ZV_ID_MAX+CBM_ZOVA_WORKSPACE_ID_MAX];
    zova_graph_info graph_info={0};
    if(rc==0&&cbm_zova_workspace_graph_name(workspace_id,graph_name,sizeof(graph_name))!=0)rc=-1;
    if(rc==0)rc=status_ok(zova_graph_info_get(&(zova_graph_info_get_request){
        .db=db,.name=graph_name,.out_info=&graph_info}),db,"user_delta.graph_readback");
    if(rc==0&&(graph_info.node_count!=out_result->graph_nodes||
              graph_info.edge_count!=out_result->graph_edges))rc=-1;
    zova_graph_info_free(&graph_info);
    if(rc==0)rc=user_publish_native_readback_counts_tx(db,out_result,node_collection,
                                                       token_collection);
    if(rc==0){user_publish_test_metrics.integrity_writes++;
              rc=user_publish_integrity_tx(db,workspace_id,generation,out_result);}
    if(rc==0&&sidecar_test_fault("user_delta_after_integrity"))rc=-1;
    if(rc==0)rc=user_publish_write_index_state_tx(db,workspace_id,generation,
        input->model_fingerprint,input->vector_dimensions,input->indexed_at);
    if(rc==0)rc=user_publish_clear_health_tx(db,workspace_id);
    if(rc==0)rc=user_publish_generation_state_tx(db,workspace_id,generation,true);
    if(rc==0&&sidecar_test_fault("user_delta_before_commit"))rc=-1;
    snprintf(out_result->workspace_id,sizeof(out_result->workspace_id),"%s",workspace_id);
    if (rc == 0) {
        user_publish_test_metrics.delta_authoritative_rows_touched +=
            metrics.node_vector_deletes + metrics.token_vector_deletes +
            metrics.topology_deletes + metrics.node_deletes +
            metrics.node_deletes + metrics.node_updates + metrics.edge_deletes +
            metrics.token_vector_deletes + metrics.file_hash_deletes +
            metrics.node_inserts + metrics.node_updates +
            metrics.node_inserts + metrics.node_updates + metrics.edge_inserts +
            metrics.topology_inserts + metrics.node_inserts + metrics.node_updates +
            metrics.node_vector_upserts + metrics.token_vector_upserts +
            metrics.token_vector_upserts + metrics.file_hash_upserts +
            metrics.summary_replacements;
    }
    free(file_keys);
    free(delta_node_keys);
    free(delta_edge_keys);
    free(delta_token_keys);
    return rc;
}

static int user_exec_i64_bound(zova_database *db, const char *sql,
                               int64_t bind_value, const char *phase) {
    if (!db || !sql) return -1;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, phase);
    if (rc == 0) rc = workspace_bind_i64(db, stmt, 1, bind_value, phase);
    if (rc == 0) rc = workspace_step_done(db, stmt, phase);
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int user_publish_full_nodes_begin_tx(zova_database *db) {
    return status_ok(zova_database_exec(&(zova_database_exec_request){
                         .db = db,
                         .sql = "DROP TRIGGER cbm_nodes_v1_fts_ai;"}),
                     db, "user_publish.nodes_bulk_begin");
}

static int user_publish_full_nodes_end_tx(zova_database *db, int64_t workspace_key,
                                          bool populate) {
    int rc = 0;
    if (populate) {
        rc = user_exec_i64_bound(
            db,
            "INSERT INTO cbm_nodes_fts_v1(rowid,name,qualified_name,file_path,label) "
            "SELECT n.zova_node_key,cbm_camel_split(n.name),n.qualified_name,f.file_path,n.label "
            "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key "
            "WHERE n.workspace_key=?1 ORDER BY n.zova_node_key",
            workspace_key, "user_publish.fts_bulk_insert");
        if (rc == 0) user_publish_test_metrics.full_fts_bulk_statements++;
    }
    int restore_rc = status_ok(zova_database_exec(&(zova_database_exec_request){
                                   .db = db,
                                   .sql = CBM_NODE_FTS_INSERT_TRIGGER_SQL}),
                               db, "user_publish.nodes_bulk_restore");
    return rc != 0 ? rc : restore_rc;
}

enum { USER_FRESH_BATCH_ROWS = 512 };
enum { USER_FRESH_UNAVAILABLE = 1 };

static zova_fresh_value user_fresh_i64(int64_t value) {
    return (zova_fresh_value){
        .value_type = ZOVA_FRESH_VALUE_INT64,
        .int64_value = value,
    };
}

static zova_fresh_value user_fresh_text(const char *value) {
    const char *text = value ? value : "";
    return (zova_fresh_value){
        .value_type = ZOVA_FRESH_VALUE_TEXT,
        .bytes = (const uint8_t *)text,
        .bytes_len = strlen(text),
    };
}

static int user_fresh_load_nodes(zova_database *db, zova_fresh_build *build,
                                 const cbm_zova_publish_model_t *model,
                                 int64_t workspace_key,
                                 const user_file_key_t *file_keys, int file_key_count,
                                 const int64_t *node_keys) {
    static const char *columns[] = {
        "zova_node_key", "workspace_key", "label", "name", "qualified_name",
        "file_key", "start_line", "end_line", "properties", "source_ordinal",
    };
    const int count = cbm_zova_publish_model_node_count(model);
    zova_fresh_value *values = count
        ? calloc((size_t)USER_FRESH_BATCH_ROWS * 10, sizeof(*values)) : NULL;
    if (count && !values) return -1;
    int rc = 0;
    user_publish_test_metrics.canonical_node_fts_passes++;
    for (int offset = 0; rc == 0 && offset < count; offset += USER_FRESH_BATCH_ROWS) {
        int rows = count - offset;
        if (rows > USER_FRESH_BATCH_ROWS) rows = USER_FRESH_BATCH_ROWS;
        for (int row_index = 0; row_index < rows; row_index++) {
            const int index = offset + row_index;
            const cbm_zova_publish_node_t *row =
                cbm_zova_publish_model_node_at(model, index);
            const CBMDumpNode *source = row ? row->source : NULL;
            int64_t file_key = source
                ? user_file_key_find(file_keys, file_key_count, source->file_path) : 0;
            if (!row || !source || !node_keys || node_keys[index] <= 0 ||
                file_key <= 0 || row->ordinal != (uint64_t)index) {
                rc = -1;
                break;
            }
            zova_fresh_value *out = values + (size_t)row_index * 10;
            out[0] = user_fresh_i64(node_keys[index]);
            out[1] = user_fresh_i64(workspace_key);
            out[2] = user_fresh_text(source->label);
            out[3] = user_fresh_text(source->name);
            out[4] = user_fresh_text(source->qualified_name);
            out[5] = user_fresh_i64(file_key);
            out[6] = user_fresh_i64(source->start_line);
            out[7] = user_fresh_i64(source->end_line);
            out[8] = user_fresh_text(source->properties ? source->properties : "{}");
            out[9] = user_fresh_i64((int64_t)row->source_ordinal);
        }
        if (rc == 0)
            rc = status_ok(zova_fresh_build_table_rows(
                               &(zova_fresh_build_rows_request){
                                   .build = build,
                                   .table_name = "cbm_nodes_v1",
                                   .column_names = columns,
                                   .column_count = sizeof(columns) / sizeof(columns[0]),
                                   .values = values,
                                   .row_count = (size_t)rows}),
                           db, "user_publish.fresh_nodes");
    }
    free(values);
    return rc;
}

static int user_fresh_load_fts(zova_database *db, zova_fresh_build *build,
                               const cbm_zova_publish_model_t *model,
                               const int64_t *node_keys) {
    static const char *columns[] = {
        "rowid", "name", "qualified_name", "file_path", "label",
    };
    const int count = cbm_zova_publish_model_node_count(model);
    zova_fresh_value *values = count
        ? calloc((size_t)USER_FRESH_BATCH_ROWS * 5, sizeof(*values)) : NULL;
    if (count && !values) return -1;
    int rc = 0;
    for (int offset = 0; rc == 0 && offset < count; offset += USER_FRESH_BATCH_ROWS) {
        int rows = count - offset;
        if (rows > USER_FRESH_BATCH_ROWS) rows = USER_FRESH_BATCH_ROWS;
        for (int row_index = 0; row_index < rows; row_index++) {
            const int index = offset + row_index;
            const cbm_zova_publish_node_t *row =
                cbm_zova_publish_model_node_at(model, index);
            const CBMDumpNode *source = row ? row->source : NULL;
            const char *fts_name = cbm_zova_publish_model_fts_name_at(model, index);
            if (!row || !source || !fts_name || !node_keys || node_keys[index] <= 0) {
                rc = -1;
                break;
            }
            zova_fresh_value *out = values + (size_t)row_index * 5;
            out[0] = user_fresh_i64(node_keys[index]);
            out[1] = user_fresh_text(fts_name);
            out[2] = user_fresh_text(source->qualified_name);
            out[3] = user_fresh_text(source->file_path);
            out[4] = user_fresh_text(source->label);
        }
        if (rc == 0)
            rc = status_ok(zova_fresh_build_fts_rows(
                               &(zova_fresh_build_rows_request){
                                   .build = build,
                                   .table_name = "cbm_nodes_fts_v1",
                                   .column_names = columns,
                                   .column_count = sizeof(columns) / sizeof(columns[0]),
                                   .values = values,
                                   .row_count = (size_t)rows}),
                           db, "user_publish.fresh_fts");
    }
    free(values);
    return rc;
}

static int user_fresh_load_node_vectors(
    zova_database *db, zova_fresh_build *build,
    const cbm_zova_publish_model_t *model, const char *collection,
    const int64_t *node_keys) {
    const cbm_zova_workspace_generation_input_t *input =
        cbm_zova_publish_model_input(model);
    const int count = cbm_zova_publish_model_node_vector_count(model);
    zova_vector_input *vectors = count
        ? calloc(USER_FRESH_BATCH_ROWS, sizeof(*vectors)) : NULL;
    char (*ids)[32] = count ? calloc(USER_FRESH_BATCH_ROWS, sizeof(*ids)) : NULL;
    if (count && (!vectors || !ids)) {
        free(vectors);
        free(ids);
        return -1;
    }
    int rc = 0;
    for (int offset = 0; rc == 0 && offset < count; offset += USER_FRESH_BATCH_ROWS) {
        int rows = count - offset;
        if (rows > USER_FRESH_BATCH_ROWS) rows = USER_FRESH_BATCH_ROWS;
        for (int row_index = 0; row_index < rows; row_index++) {
            const int index = offset + row_index;
            const cbm_zova_publish_node_vector_t *row =
                cbm_zova_publish_model_node_vector_at(model, index);
            int64_t key = row ? node_keys[row->node_ordinal] : 0;
            if (!row || row->ordinal != (uint64_t)index || key <= 0 ||
                user_private_key_id(key, ids[row_index]) != 0) {
                rc = -1;
                break;
            }
            vectors[row_index] = (zova_vector_input){
                .id = ids[row_index],
                .values = {
                    .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = (const int8_t *)row->source->vector,
                    .values_len = (size_t)input->vector_dimensions,
                },
            };
        }
        if (rc == 0)
            rc = status_ok(zova_fresh_build_vectors(
                               &(zova_fresh_build_vectors_request){
                                   .build = build,
                                   .collection_name = collection,
                                   .vectors = vectors,
                                   .vectors_len = (size_t)rows}),
                           db, "user_publish.fresh_node_vectors");
    }
    free(vectors);
    free(ids);
    return rc;
}

static int user_fresh_load_token_vectors(
    zova_database *db, zova_fresh_build *build,
    const cbm_zova_publish_model_t *model, const char *collection,
    const user_token_key_t *token_keys, int token_key_count) {
    const cbm_zova_workspace_generation_input_t *input =
        cbm_zova_publish_model_input(model);
    const int count = cbm_zova_publish_model_token_vector_count(model);
    zova_vector_input *vectors = count
        ? calloc(USER_FRESH_BATCH_ROWS, sizeof(*vectors)) : NULL;
    char (*ids)[32] = count ? calloc(USER_FRESH_BATCH_ROWS, sizeof(*ids)) : NULL;
    if (count && (!vectors || !ids)) {
        free(vectors);
        free(ids);
        return -1;
    }
    int rc = count == token_key_count ? 0 : -1;
    for (int offset = 0; rc == 0 && offset < count; offset += USER_FRESH_BATCH_ROWS) {
        int rows = count - offset;
        if (rows > USER_FRESH_BATCH_ROWS) rows = USER_FRESH_BATCH_ROWS;
        for (int row_index = 0; row_index < rows; row_index++) {
            const int index = offset + row_index;
            const cbm_zova_publish_token_vector_t *row =
                cbm_zova_publish_model_token_vector_at(model, index);
            int64_t key = token_keys[index].token_key;
            if (!row || !row->source || row->ordinal != (uint64_t)index || key <= 0 ||
                strcmp(token_keys[index].token, row->source->token) != 0 ||
                user_private_key_id(key, ids[row_index]) != 0) {
                rc = -1;
                break;
            }
            vectors[row_index] = (zova_vector_input){
                .id = ids[row_index],
                .values = {
                    .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = (const int8_t *)row->source->vector,
                    .values_len = (size_t)input->vector_dimensions,
                },
            };
        }
        if (rc == 0)
            rc = status_ok(zova_fresh_build_vectors(
                               &(zova_fresh_build_vectors_request){
                                   .build = build,
                                   .collection_name = collection,
                                   .vectors = vectors,
                                   .vectors_len = (size_t)rows}),
                           db, "user_publish.fresh_token_vectors");
    }
    free(vectors);
    free(ids);
    return rc;
}

static int user_publish_can_use_fresh_session(zova_database *db, bool *out_fresh) {
    if (!db || !out_fresh) return -1;
    *out_fresh = false;
    zova_graph_list graphs = {0};
    int rc = status_ok(zova_graphs_list(&(zova_graph_list_request){
                           .db = db, .out_list = &graphs}),
                       db, "user_publish.fresh_graph_list");
    if (rc == 0) *out_fresh = graphs.len == 0;
    zova_graph_list_free(&graphs);
    return rc;
}

typedef struct {
    user_native_graph_profile_t graph;
    double validation_ms;
    double table_ms;
    double fts_ms;
    double vector_ms;
    double index_ms;
    double commit_ms;
    double build_ms;
} user_fresh_publish_profile_t;

static int user_publish_model_fresh_session_tx(
    zova_database *db, const cbm_zova_publish_model_t *model,
    int64_t workspace_key, const user_file_key_t *file_keys, int file_key_count,
    const user_token_key_t *token_keys, int token_key_count,
    const char *node_collection, const char *token_collection,
    uint64_t *out_nodes, uint64_t *out_edges,
    user_fresh_publish_profile_t *out_profile) {
    if (!db || !model || !node_collection || !token_collection || !out_profile)
        return -1;
    memset(out_profile, 0, sizeof(*out_profile));
    const cbm_zova_workspace_generation_input_t *input =
        cbm_zova_publish_model_input(model);
    const char *workspace_id = cbm_zova_publish_model_workspace_id(model);
    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (!input || !workspace_id ||
        cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0)
        return -1;
    const int node_count = cbm_zova_publish_model_node_count(model);
    const int edge_count = cbm_zova_publish_model_topology_count(model);
    zova_graph_fresh_node_input *nodes = node_count
        ? calloc((size_t)node_count, sizeof(*nodes)) : NULL;
    zova_graph_fresh_edge_payload_input *edges = edge_count
        ? calloc((size_t)edge_count, sizeof(*edges)) : NULL;
    int64_t *node_keys = node_count
        ? calloc((size_t)node_count, sizeof(*node_keys)) : NULL;
    int64_t *edge_keys = edge_count
        ? calloc((size_t)edge_count, sizeof(*edge_keys)) : NULL;
    if ((node_count && (!nodes || !node_keys)) ||
        (edge_count && (!edges || !edge_keys))) {
        free(nodes); free(edges); free(node_keys); free(edge_keys);
        return -1;
    }
    struct timespec mark = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &mark);
    int rc = 0;
    for (int i = 0; rc == 0 && i < node_count; i++) {
        const cbm_zova_publish_node_t *row = cbm_zova_publish_model_node_at(model, i);
        if (!row || !row->source || row->ordinal != (uint64_t)i) { rc = -1; break; }
        nodes[i] = (zova_graph_fresh_node_input){
            .node_id = row->stable_id,
            .kind = row->source->label,
            .target_type = ZOVA_GRAPH_TARGET_NONE,
        };
    }
    for (int i = 0; rc == 0 && i < edge_count; i++) {
        const cbm_zova_publish_edge_t *row = cbm_zova_publish_model_topology_at(model, i);
        if (!row || !row->source || row->ordinal != (uint64_t)i) { rc = -1; break; }
        edges[i] = (zova_graph_fresh_edge_payload_input){
            .from_node_ordinal = (size_t)row->source_ordinal,
            .edge_type = row->source->type,
            .to_node_ordinal = (size_t)row->target_ordinal,
            .payload = row->payload,
            .payload_len = row->payload_len,
        };
    }
    out_profile->graph.materialize_ms =
        user_publish_profile_phase("fresh.materialize", &mark);
    if (rc == 0) rc = collection_create_i8(db, node_collection, input->vector_dimensions);
    if (rc == 0) rc = collection_create_i8(db, token_collection, input->vector_dimensions);
    if (rc == 0) rc = user_publish_full_nodes_begin_tx(db);

    zova_fresh_build *build = NULL;
    if (rc == 0) {
        zova_status begin_status = zova_fresh_build_begin(
            &(zova_fresh_build_begin_request){.db = db, .out_build = &build});
        if (begin_status == ZOVA_INVALID_ARGUMENT) {
            int restore_rc = user_publish_full_nodes_end_tx(db, workspace_key, false);
            free(nodes);
            free(edges);
            free(node_keys);
            free(edge_keys);
            return restore_rc == 0 ? USER_FRESH_UNAVAILABLE : -1;
        }
        rc = status_ok(begin_status, db, "user_publish.fresh_begin");
    }
    out_profile->graph.reset_ms = user_publish_profile_phase("fresh.begin", &mark);
    struct timespec fresh_started = mark;
    if (rc == 0) {
        user_publish_test_metrics.native_graph_fresh_calls++;
        user_publish_test_metrics.native_graph_prepared_calls++;
        rc = status_ok(zova_fresh_build_graph(&(zova_fresh_build_graph_request){
                           .build = build,
                           .graph_name = graph_name,
                           .nodes = nodes,
                           .nodes_len = (size_t)node_count,
                           .edges = edges,
                           .edges_len = (size_t)edge_count,
                           .out_node_keys = node_keys,
                           .out_node_keys_capacity = (size_t)node_count,
                           .out_edge_keys = edge_keys,
                           .out_edge_keys_capacity = (size_t)edge_count}),
                       db, "user_publish.fresh_graph");
    }
    if (rc == 0 && sidecar_test_fault("user_after_graph_nodes")) rc = -1;
    if (rc == 0 && sidecar_test_fault("user_after_graph_edges")) rc = -1;
    if (rc == 0) rc = user_fresh_load_fts(db, build, model, node_keys);
    if (rc == 0 && sidecar_test_fault("user_after_bulk_fts")) rc = -1;
    if (rc == 0) rc = user_fresh_load_nodes(
        db, build, model, workspace_key, file_keys, file_key_count, node_keys);
    if (rc == 0 && sidecar_test_fault("user_after_nodes_before_bulk_finalize")) rc = -1;
    if (rc == 0 && sidecar_test_fault("user_after_edges_before_bulk_finalize")) rc = -1;
    if (rc == 0) rc = user_fresh_load_node_vectors(
        db, build, model, node_collection, node_keys);
    if (rc == 0 && sidecar_test_fault("user_after_node_vectors")) rc = -1;
    if (rc == 0) rc = user_fresh_load_token_vectors(
        db, build, model, token_collection, token_keys, token_key_count);
    if (rc == 0 && sidecar_test_fault("user_after_token_vectors")) rc = -1;

    zova_fresh_build_profile profile = {0};
    if (rc == 0)
        rc = status_ok(zova_fresh_build_finish(&(zova_fresh_build_finish_request){
                           .build = build,
                           .out_node_keys = node_keys,
                           .out_node_keys_capacity = (size_t)node_count,
                           .out_edge_keys = edge_keys,
                           .out_edge_keys_capacity = (size_t)edge_count,
                           .out_profile = &profile}),
                       db, "user_publish.fresh_finish");
    if (rc != 0 && build) (void)zova_fresh_build_abort(build);
    if (build) zova_fresh_build_destroy(build);
    if (rc == 0) rc = user_publish_full_nodes_end_tx(db, workspace_key, false);
    if (rc == 0) {
        for (int i = 0; i < node_count; i++) if (node_keys[i] <= 0) rc = -1;
        for (int i = 0; i < edge_count; i++) if (edge_keys[i] <= 0) rc = -1;
    }
    struct timespec fresh_completed = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &fresh_completed);
    out_profile->build_ms = user_publish_elapsed_ms(&fresh_started, &fresh_completed);
    out_profile->validation_ms = profile.validation_ms;
    out_profile->graph.nodes_ms = profile.graph_node_load_ms;
    out_profile->graph.edges_ms = profile.graph_edge_load_ms;
    out_profile->graph.validate_ms = profile.graph_validation_ms;
    out_profile->graph.key_generation_ms = profile.graph_key_generation_ms;
    out_profile->table_ms = profile.table_load_ms;
    out_profile->fts_ms = profile.fts_load_ms;
    out_profile->vector_ms = profile.vector_load_ms;
    out_profile->index_ms = profile.index_build_ms;
    out_profile->commit_ms = profile.commit_ms;
    if (rc == 0) {
        if (out_nodes) *out_nodes = (uint64_t)node_count;
        if (out_edges) *out_edges = (uint64_t)edge_count;
        user_publish_test_metrics.full_fts_trigger_rows_avoided += (uint64_t)node_count;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &mark);
    free(nodes);
    free(edges);
    free(node_keys);
    free(edge_keys);
    out_profile->graph.cleanup_ms = user_publish_profile_phase("fresh.cleanup", &mark);
    return rc;
}

int cbm_zova_user_database_publish_model_tx(
    zova_database *db, const cbm_zova_publish_model_t *model,
    int64_t requested_generation, cbm_zova_workspace_generation_result_t *out_result) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_publish_model_input(model);
    if (!db || !input || !out_result || !input->root_path || !input->root_path[0] ||
        !input->project || !input->project[0] || !input->indexed_at || !input->indexed_at[0] ||
        !input->model_fingerprint || !input->model_fingerprint[0] || input->vector_dimensions <= 0 ||
        input->node_count < 0 || input->edge_count < 0 || input->node_vector_count < 0 ||
        input->token_vector_count < 0 || input->file_hash_count < 0 ||
        (input->node_count > 0 && !input->nodes) || (input->edge_count > 0 && !input->edges) ||
        (input->node_vector_count > 0 && !input->node_vectors) ||
        (input->token_vector_count > 0 && !input->token_vectors) ||
        (input->file_hash_count > 0 && !input->file_hashes)) {
        return -1;
    }
    struct timespec profile_mark = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &profile_mark);
    const cbm_zova_workspace_generation_result_t *model_result =
        cbm_zova_publish_model_digests(model);
    const char *model_workspace_id = cbm_zova_publish_model_workspace_id(model);
    if (!model_result || !model_workspace_id) return -1;
    *out_result = *model_result;
    cbm_zova_publish_model_metrics_t model_metrics = {0};
    cbm_zova_publish_model_metrics(model, &model_metrics);
    out_result->publication_mode = CBM_ZOVA_PUBLICATION_MODE_FULL;
    out_result->inserted_count = out_result->metadata_nodes + out_result->metadata_edges +
                                 out_result->node_vectors + out_result->token_vectors +
                                 (uint64_t)input->file_hash_count;
    out_result->normalization_ms = model_metrics.normalization_ms;
    out_result->model_nodes_ms = model_metrics.nodes_ms;
    out_result->model_edges_ms = model_metrics.edges_ms;
    out_result->model_edge_endpoint_ms = model_metrics.prepared_endpoint_ms;
    out_result->model_edge_sort_ms = model_metrics.prepared_topology_sort_ms;
    out_result->model_edge_group_ms = model_metrics.prepared_topology_group_ms;
    out_result->model_edge_payload_ms = model_metrics.prepared_payload_ms;
    out_result->model_edge_digest_ms = model_metrics.prepared_topology_digest_ms;
    out_result->model_edge_default_payloads =
        model_metrics.prepared_single_default_payload_count;
    out_result->model_edge_payload_scratch_edges =
        model_metrics.prepared_payload_scratch_edge_count;
    out_result->model_hashes_ms = model_metrics.hashes_ms;
    out_result->model_vectors_ms = model_metrics.vectors_ms;
    out_result->model_digests_ms = model_metrics.digests_ms;
    out_result->full_clear_count = 1;
    out_result->nodes_inserted = out_result->metadata_nodes;
    out_result->edges_inserted = out_result->metadata_edges;
    out_result->node_vectors_upserted = out_result->node_vectors;
    out_result->token_vectors_upserted = out_result->token_vectors;
    if (cbm_zova_register_sql_functions(db) != 0) return -1;
    if (status_ok(zova_database_exec(&(zova_database_exec_request){
                      .db = db, .sql = "PRAGMA cache_size=-65536"}),
                  db, "user_publish.cache_size") != 0) {
        return -1;
    }
    if (!workspace_name_component_valid(input->model_fingerprint)) return -1;
    (void)user_publish_profile_phase("normalize", &profile_mark);

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    if (user_workspace_id_tx(db, input->root_path, workspace_id, sizeof(workspace_id)) != 0 ||
        strcmp(workspace_id, model_workspace_id) != 0) return -1;
    int64_t workspace_key = 0;
    if (workspace_key_for_id(db, workspace_id, &workspace_key) != 0) return -1;
    int64_t current_max = 0;
    int64_t generation = 0;
    if (zova_query_i64(db,
                       "SELECT COALESCE(MAX(generation),0) FROM cbm_database_generation_v1 "
                       "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                       "WHERE workspace_id=?1)",
                       workspace_id, &current_max, "user_publish.current_generation") != 0 ||
        current_max < 0 ||
        (requested_generation > 0 && requested_generation <= current_max) ||
        (requested_generation == 0 && current_max == INT64_MAX)) {
        return -1;
    }
    generation = requested_generation > 0 ? requested_generation : current_max + 1;
    if (generation <= 0 || user_publish_insert_generation_tx(db, workspace_id, generation) != 0)
        return -1;

    user_publish_test_metrics.full_clear_count++;
    int rc = user_database_clear_workspace(db, workspace_id);
    user_file_key_t *file_keys = NULL;
    int file_key_count = 0;
    user_node_key_t *node_keys = NULL;
    user_token_key_t *token_keys = NULL;
    int token_key_count = 0;
    user_topology_key_t *topology_keys = NULL;
    out_result->clear_ms = user_publish_profile_phase("clear", &profile_mark);
    if (rc == 0) rc = user_publish_project_tx(db, workspace_id, input->root_path, input->project,
                                               input->indexed_at);
    if (rc == 0) rc = user_publish_file_keys_tx(db, model, workspace_key, &file_keys,
                                                 &file_key_count);
    out_result->canonical_files_ms =
        user_publish_profile_phase("canonical_files", &profile_mark);

    char node_collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    char token_collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (rc == 0 && (cbm_zova_workspace_node_vector_collection_name(
                        workspace_id, input->model_fingerprint, input->vector_dimensions,
                        node_collection, sizeof(node_collection)) != 0 ||
                    cbm_zova_workspace_token_vector_collection_name(
                        workspace_id, input->model_fingerprint, input->vector_dimensions,
                        token_collection, sizeof(token_collection)) != 0)) {
        rc = -1;
    }

    uint64_t graph_nodes = 0;
    uint64_t graph_edges = 0;
    bool use_fresh_session = false;
    if (rc == 0) rc = user_publish_can_use_fresh_session(db, &use_fresh_session);
    bool hashes_published = false;
    bool token_metadata_published = false;
    if (rc == 0 && use_fresh_session) {
        if (rc == 0) rc = user_publish_model_hashes_tx(
            db, model, file_keys, file_key_count);
        if (rc == 0) rc = user_database_write_summary(
            db, workspace_id, &input->project_summary);
        hashes_published = rc == 0;
        out_result->canonical_hashes_ms =
            user_publish_profile_phase("canonical_hashes", &profile_mark);
        if (rc == 0) rc = user_publish_model_token_metadata_tx(
            db, model, workspace_key, &token_keys, &token_key_count);
        token_metadata_published = rc == 0;
        out_result->token_metadata_ms =
            user_publish_profile_phase("token_metadata", &profile_mark);
        if (rc == 0 && sidecar_test_fault("user_after_metadata")) rc = -1;

        user_fresh_publish_profile_t fresh_profile = {0};
        if (rc == 0) {
            int fresh_rc = user_publish_model_fresh_session_tx(
                db, model, workspace_key, file_keys, file_key_count,
                token_keys, token_key_count, node_collection, token_collection,
                &graph_nodes, &graph_edges, &fresh_profile);
            if (fresh_rc == USER_FRESH_UNAVAILABLE) {
                use_fresh_session = false;
                cbm_clock_gettime(CLOCK_MONOTONIC, &profile_mark);
            } else {
                rc = fresh_rc;
            }
        }
        if (use_fresh_session) {
            out_result->native_graph_materialize_ms = fresh_profile.graph.materialize_ms;
            out_result->native_graph_reset_ms = fresh_profile.graph.reset_ms;
            out_result->native_graph_nodes_ms = fresh_profile.graph.nodes_ms;
            out_result->native_graph_edges_ms = fresh_profile.graph.edges_ms;
            out_result->native_graph_validate_ms = fresh_profile.graph.validate_ms;
            out_result->native_graph_key_generation_ms =
                fresh_profile.graph.key_generation_ms;
            out_result->native_graph_cleanup_ms = fresh_profile.graph.cleanup_ms;
            out_result->native_graph_ms = fresh_profile.graph.materialize_ms +
                fresh_profile.graph.reset_ms + fresh_profile.graph.nodes_ms +
                fresh_profile.graph.edges_ms + fresh_profile.graph.validate_ms +
                fresh_profile.graph.key_generation_ms + fresh_profile.graph.cleanup_ms;
            out_result->canonical_nodes_ms = fresh_profile.table_ms;
            out_result->fts_ms = fresh_profile.fts_ms;
            out_result->canonical_edges_ms = 0.0;
            out_result->native_vectors_ms = fresh_profile.vector_ms;
            out_result->fresh_validation_ms = fresh_profile.validation_ms;
            out_result->fresh_index_ms = fresh_profile.index_ms;
            out_result->fresh_commit_ms = fresh_profile.commit_ms;
            out_result->fresh_build_ms = fresh_profile.build_ms;
            cbm_clock_gettime(CLOCK_MONOTONIC, &profile_mark);
            if (rc == 0 && sidecar_test_fault("user_after_fts")) rc = -1;
        }
    }
    if (rc == 0 && !use_fresh_session) {
        user_native_graph_profile_t graph_profile = {0};
        rc = user_publish_model_graph_tx(db, model, &node_keys, &topology_keys,
                                         &graph_nodes, &graph_edges, &graph_profile);
        out_result->native_graph_materialize_ms = graph_profile.materialize_ms;
        out_result->native_graph_reset_ms = graph_profile.reset_ms;
        out_result->native_graph_nodes_ms = graph_profile.nodes_ms;
        out_result->native_graph_edges_ms = graph_profile.edges_ms;
        out_result->native_graph_validate_ms = graph_profile.validate_ms;
        out_result->native_graph_key_generation_ms = graph_profile.key_generation_ms;
        out_result->native_graph_cleanup_ms = graph_profile.cleanup_ms;
        out_result->native_graph_ms =
            user_publish_profile_phase("native_graph", &profile_mark);

        bool nodes_bulk_active = false;
        if (rc == 0) {
            rc = user_publish_full_nodes_begin_tx(db);
            nodes_bulk_active = rc == 0;
        }
        if (rc == 0) rc = user_publish_model_nodes_tx(
            db, model, workspace_key, file_keys, file_key_count, node_keys);
        out_result->canonical_nodes_ms =
            user_publish_profile_phase("canonical_nodes", &profile_mark);
        bool populate_fts = rc == 0 &&
            !sidecar_test_fault("user_after_nodes_before_bulk_finalize");
        if (rc == 0 && !populate_fts) rc = -1;
        if (nodes_bulk_active) {
            int end_rc = user_publish_full_nodes_end_tx(db, workspace_key, populate_fts);
            if (rc == 0) rc = end_rc;
            if (populate_fts && end_rc == 0) {
                user_publish_test_metrics.full_fts_trigger_rows_avoided +=
                    (uint64_t)input->node_count;
            }
        }
        out_result->fts_ms = user_publish_profile_phase("fts", &profile_mark);
        if (rc == 0 && sidecar_test_fault("user_after_bulk_fts")) rc = -1;
        if (rc == 0 && sidecar_test_fault("user_after_edges_before_bulk_finalize")) rc = -1;
        out_result->canonical_edges_ms =
            user_publish_profile_phase("canonical_edges", &profile_mark);
        if (!hashes_published) {
            if (rc == 0) rc = user_publish_model_hashes_tx(
                db, model, file_keys, file_key_count);
            if (rc == 0) rc = user_database_write_summary(
                db, workspace_id, &input->project_summary);
            out_result->canonical_hashes_ms =
                user_publish_profile_phase("canonical_hashes", &profile_mark);
        }
        if (!token_metadata_published) {
            if (rc == 0) rc = user_publish_model_token_metadata_tx(
                db, model, workspace_key, &token_keys, &token_key_count);
            out_result->token_metadata_ms =
                user_publish_profile_phase("token_metadata", &profile_mark);
            if (rc == 0 && sidecar_test_fault("user_after_metadata")) rc = -1;
        }
        if (rc == 0 && sidecar_test_fault("user_after_fts")) rc = -1;
        if (rc == 0)
            rc = user_publish_model_vectors_tx(
                db, model, node_collection, token_collection, node_keys,
                cbm_zova_publish_model_node_count(model), token_keys, token_key_count);
        out_result->native_vectors_ms =
            user_publish_profile_phase("native_vectors", &profile_mark);
    }

    out_result->generation = generation;
    if (rc == 0 && (graph_nodes != out_result->graph_nodes ||
                    graph_edges != out_result->graph_edges)) rc = -1;
    (void)user_publish_profile_phase("digests", &profile_mark);
    if (rc == 0) rc = user_publish_native_readback_counts_tx(
        db, out_result, node_collection, token_collection);
    out_result->readback_ms = user_publish_profile_phase("readback", &profile_mark);
    if (rc == 0) {
        user_publish_test_metrics.integrity_writes++;
        rc = user_publish_integrity_tx(db, workspace_id, generation, out_result);
    }
    if (rc == 0 && sidecar_test_fault("user_after_integrity")) rc = -1;
    if (rc == 0) rc = user_publish_write_index_state_tx(
        db, workspace_id, generation, input->model_fingerprint, input->vector_dimensions,
        input->indexed_at);
    if (rc == 0) rc = user_publish_clear_health_tx(db, workspace_id);
    if (rc == 0) rc = user_publish_generation_state_tx(db, workspace_id, generation, true);
    if (rc == 0 && sidecar_test_fault("user_before_commit")) rc = -1;
    out_result->finalize_ms = user_publish_profile_phase("finalize", &profile_mark);
    snprintf(out_result->workspace_id, sizeof(out_result->workspace_id), "%s", workspace_id);
    free(node_keys);
    free(token_keys);
    free(topology_keys);
    free(file_keys);
    return rc;
}

int cbm_zova_user_database_publish_workspace_tx(
    zova_database *db, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result) {
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    cbm_zova_publish_model_t *model = NULL;
    if (!input || cbm_zova_workspace_id_for_root(input->root_path, workspace_id,
                                                  sizeof(workspace_id)) != 0 ||
        cbm_zova_publish_model_build(workspace_id, input, &model) != 0) return -1;
    int rc = cbm_zova_user_database_publish_model_tx(db, model, 0, out_result);
    cbm_zova_publish_model_free(model);
    return rc;
}

static int user_database_publish_model_at_generation(
    const char *zova_path, const cbm_zova_publish_model_t *model,
    int64_t requested_generation, cbm_zova_workspace_generation_result_t *out_result) {
    if (!zova_path || !zova_path[0] || !model || !out_result || requested_generation < 0)
        return -1;
    memset(out_result, 0, sizeof(*out_result));
    struct timespec phase_started = {0}, phase_finished = {0};
    double writer_guard_ms = 0.0, database_init_ms = 0.0, database_open_ms = 0.0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    cbm_zova_writer_guard_t writer_guard = {0};
    if (cbm_zova_writer_guard_acquire(zova_path, &writer_guard) != 0) {
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    writer_guard_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    phase_started = phase_finished;
    cbm_zova_db_t zdb;
    if (open_or_create_zova(zova_path, &zdb) != 0) {
        cbm_zova_writer_guard_release(&writer_guard);
        return -1;
    }
    user_publish_test_metrics.database_open_count++;
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    database_open_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    phase_started = phase_finished;
    if (user_database_ensure_schema(zdb.db) != 0) {
        close_zova(&zdb);
        user_publish_test_metrics.database_close_count++;
        cbm_zova_writer_guard_release(&writer_guard);
        return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    database_init_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    struct timespec started = {0};
    struct timespec finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    struct stat before_db = {0};
    struct stat before_wal = {0};
    char wal_path[ZV_PATH_MAX];
    bool have_before_db = stat(zova_path, &before_db) == 0;
    bool have_wal_path = snprintf(wal_path, sizeof(wal_path), "%s-wal", zova_path) <
                         (int)sizeof(wal_path);
    bool have_before_wal = have_wal_path && stat(wal_path, &before_wal) == 0;
    uint64_t database_bytes_before = have_before_db ? (uint64_t)before_db.st_size : 0;
    uint64_t wal_bytes_before = have_before_wal ? (uint64_t)before_wal.st_size : 0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    int rc = status_ok(zova_database_exec(&(zova_database_exec_request){
                                    .db = zdb.db,
                                    .sql = "PRAGMA journal_mode=WAL;"
                                           "PRAGMA synchronous=NORMAL;"
                                           "PRAGMA cache_size=-65536;"}),
                                zdb.db, "user_publish.wal");
    if (rc == 0) {
        user_publish_test_metrics.transaction_count++;
        rc = workspace_registry_begin(zdb.db);
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    double transaction_begin_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    phase_started = phase_finished;
    if (rc == 0)
        rc = cbm_zova_user_database_publish_model_tx(zdb.db, model,
                                                      requested_generation, out_result);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    double transaction_body_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    phase_started = phase_finished;
    if (rc == 0) rc = workspace_registry_commit(zdb.db);
    else workspace_registry_rollback(zdb.db);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    double transaction_commit_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    phase_started = phase_finished;
    close_zova(&zdb);
    user_publish_test_metrics.database_close_count++;
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_finished);
    double database_close_ms = user_publish_elapsed_ms(&phase_started, &phase_finished);
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    out_result->publish_ms = user_publish_elapsed_ms(&started, &finished);
    out_result->writer_guard_ms = writer_guard_ms;
    out_result->database_init_ms = database_init_ms;
    out_result->database_open_ms = database_open_ms;
    out_result->transaction_begin_ms = transaction_begin_ms;
    out_result->transaction_body_ms = transaction_body_ms;
    out_result->transaction_commit_ms = transaction_commit_ms;
    out_result->database_close_ms = database_close_ms;
    out_result->database_bytes = database_bytes_before;
    out_result->wal_bytes_before = wal_bytes_before;
    struct stat after_db = {0};
    struct stat after_wal = {0};
    if (stat(zova_path, &after_db) == 0) out_result->database_bytes = (uint64_t)after_db.st_size;
    if (have_wal_path && stat(wal_path, &after_wal) == 0) {
        out_result->wal_bytes_after = (uint64_t)after_wal.st_size;
    }
    cbm_zova_writer_guard_release(&writer_guard);
    return rc;
}

static int user_database_publish_workspace_at_generation(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    int64_t requested_generation, cbm_zova_workspace_generation_result_t *out_result) {
    if (!input) return -1;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    cbm_zova_publish_model_t *model = NULL;
    if (cbm_zova_workspace_id_for_root(input->root_path, workspace_id,
                                        sizeof(workspace_id)) != 0 ||
        cbm_zova_publish_model_build(workspace_id, input, &model) != 0) return -1;
    int rc = user_database_publish_model_at_generation(zova_path, model,
                                                        requested_generation, out_result);
    cbm_zova_publish_model_free(model);
    return rc;
}

int cbm_zova_user_database_publish_model(
    const char *zova_path, const cbm_zova_publish_model_t *model,
    cbm_zova_workspace_generation_result_t *out_result) {
    return user_database_publish_model_at_generation(zova_path, model, 0, out_result);
}

int cbm_zova_user_database_publish_prepared_view(
    const char *zova_path, const cbm_zova_prepared_view_t *view,
    cbm_zova_workspace_generation_result_t *out_result) {
    return user_database_publish_model_at_generation(zova_path, view, 0, out_result);
}

int cbm_zova_user_database_publish_delta(
    const char *zova_path, const cbm_zova_publish_model_t *after,
    const cbm_zova_workspace_delta_t *delta,
    cbm_zova_workspace_generation_result_t *out_result) {
    if(!zova_path||!zova_path[0]||!after||!delta||!out_result)return -1;
    memset(out_result,0,sizeof(*out_result));
    struct timespec phase_started={0},phase_finished={0};
    double writer_guard_ms=0.0,database_init_ms=0.0,database_open_ms=0.0;
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_started);
    cbm_zova_writer_guard_t writer_guard={0};
    if(cbm_zova_writer_guard_acquire(zova_path,&writer_guard)!=0)return -1;
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    writer_guard_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    phase_started=phase_finished;
    cbm_zova_db_t zdb={0};
    if(open_or_create_zova(zova_path,&zdb)!=0){
        cbm_zova_writer_guard_release(&writer_guard);return -1;
    }
    user_publish_test_metrics.database_open_count++;
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    database_open_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    phase_started=phase_finished;
    if(user_database_ensure_schema(zdb.db)!=0){
        close_zova(&zdb);user_publish_test_metrics.database_close_count++;
        cbm_zova_writer_guard_release(&writer_guard);return -1;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    database_init_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    struct timespec started={0},finished={0};
    cbm_clock_gettime(CLOCK_MONOTONIC,&started);
    struct stat before_db={0},before_wal={0};
    char wal_path[ZV_PATH_MAX];
    bool have_before_db=stat(zova_path,&before_db)==0;
    bool have_wal_path=snprintf(wal_path,sizeof(wal_path),"%s-wal",zova_path)<(int)sizeof(wal_path);
    bool have_before_wal=have_wal_path&&stat(wal_path,&before_wal)==0;
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_started);
    int rc=status_ok(zova_database_exec(&(zova_database_exec_request){.db=zdb.db,
        .sql="PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA cache_size=-65536;"}),
        zdb.db,"user_delta.wal");
    if(rc==0){user_publish_test_metrics.transaction_count++;rc=workspace_registry_begin(zdb.db);}
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    double transaction_begin_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    phase_started=phase_finished;
    user_publish_delta_active=true;
    if(rc==0)rc=cbm_zova_user_database_publish_delta_tx(zdb.db,after,delta,out_result);
    user_publish_delta_active=false;
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    double transaction_body_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    phase_started=phase_finished;
    if(rc==0)rc=workspace_registry_commit(zdb.db);else workspace_registry_rollback(zdb.db);
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    double transaction_commit_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    phase_started=phase_finished;
    close_zova(&zdb);
    user_publish_test_metrics.database_close_count++;
    cbm_clock_gettime(CLOCK_MONOTONIC,&phase_finished);
    double database_close_ms=user_publish_elapsed_ms(&phase_started,&phase_finished);
    cbm_clock_gettime(CLOCK_MONOTONIC,&finished);
    out_result->publish_ms=user_publish_elapsed_ms(&started,&finished);
    out_result->writer_guard_ms=writer_guard_ms;
    out_result->database_init_ms=database_init_ms;
    out_result->database_open_ms=database_open_ms;
    out_result->transaction_begin_ms=transaction_begin_ms;
    out_result->transaction_body_ms=transaction_body_ms;
    out_result->transaction_commit_ms=transaction_commit_ms;
    out_result->database_close_ms=database_close_ms;
    out_result->database_bytes=have_before_db?(uint64_t)before_db.st_size:0;
    out_result->wal_bytes_before=have_before_wal?(uint64_t)before_wal.st_size:0;
    struct stat after_db={0},after_wal={0};
    if(stat(zova_path,&after_db)==0)out_result->database_bytes=(uint64_t)after_db.st_size;
    if(have_wal_path&&stat(wal_path,&after_wal)==0)out_result->wal_bytes_after=(uint64_t)after_wal.st_size;
    cbm_zova_writer_guard_release(&writer_guard);
    return rc;
}


int cbm_zova_user_database_publish_workspace(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    cbm_zova_workspace_generation_result_t *out_result) {
    return user_database_publish_workspace_at_generation(zova_path, input, 0, out_result);
}

int cbm_zova_user_database_publish_workspace_at_generation(
    const char *zova_path, const cbm_zova_workspace_generation_input_t *input,
    int64_t requested_generation, cbm_zova_workspace_generation_result_t *out_result) {
    if (requested_generation <= 0) return -1;
    return user_database_publish_workspace_at_generation(zova_path, input, requested_generation,
                                                          out_result);
}

int cbm_zova_user_database_delete_workspace(const char *zova_path,
                                            const char *workspace_id) {
    if (!zova_path || !zova_path[0] || cbm_zova_workspace_id_validate(workspace_id) != 0) {
        return -1;
    }
    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_acquire(zova_path, &guard) != 0) return -1;
    cbm_zova_db_t zdb = {0};
    if (open_zova(zova_path, false, &zdb) != 0 || workspace_registry_begin(zdb.db) != 0) {
        close_zova(&zdb);
        cbm_zova_writer_guard_release(&guard);
        return -1;
    }
    int64_t present = 0;
    int rc = zova_query_i64(zdb.db,
                            "SELECT count(*) FROM cbm_workspace_registry WHERE workspace_id=?1",
                            workspace_id, &present, "user_delete.exists");
    if (rc == 0 && present == 0) {
        rc = workspace_registry_commit(zdb.db);
        close_zova(&zdb);
        cbm_zova_writer_guard_release(&guard);
        return rc;
    }
    int64_t workspace_key = 0;
    if (rc == 0 && workspace_key_for_id(zdb.db, workspace_id, &workspace_key) != 0) rc = -1;

    char model[256] = {0};
    int dimensions = 0;
    zova_statement *state = NULL;
    if (rc == 0 && prepare_zova(zdb.db,
            "SELECT model_fingerprint,vector_dimensions FROM cbm_workspace_index_state_v1 "
            "WHERE workspace_key=?1", &state, "user_delete.state_prepare") == 0 &&
        workspace_bind_i64(zdb.db, state, 1, workspace_key, "user_delete.state_bind") == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (zova_statement_step(&(zova_statement_step_request){.statement=state,
                .out_result=&step}) != ZOVA_OK) rc = -1;
        if (rc == 0 && step == ZOVA_STEP_ROW) {
            zova_text text = {0};
            if (zova_statement_column_text(&(zova_statement_column_text_request){
                    .statement=state,.index=0,.out_text=&text}) != ZOVA_OK ||
                !text.data || text.len >= sizeof(model)) rc = -1;
            else { memcpy(model, text.data, text.len); model[text.len] = '\0'; }
            zova_text_free(&text);
            int64_t value = 0;
            if (rc == 0 && zova_statement_column_int64(&(zova_statement_column_int64_request){
                    .statement=state,.index=1,.out_value=&value}) != ZOVA_OK) rc = -1;
            dimensions = (int)value;
        }
    }
    if (state) (void)zova_statement_finalize(state);

    char graph_name[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX];
    if (rc == 0 && cbm_zova_workspace_graph_name(workspace_id, graph_name,
                                                  sizeof(graph_name)) != 0) rc = -1;
    if (rc == 0) rc = graph_delete_named_if_exists(zdb.db, graph_name);
    if (rc == 0 && model[0] && dimensions > 0) {
        char nodes[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
        char tokens[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
        if (cbm_zova_workspace_node_vector_collection_name(workspace_id, model, dimensions,
                                                            nodes, sizeof(nodes)) != 0 ||
            cbm_zova_workspace_token_vector_collection_name(workspace_id, model, dimensions,
                                                              tokens, sizeof(tokens)) != 0 ||
            collection_delete_if_exists(zdb.db, nodes) != 0 ||
            collection_delete_if_exists(zdb.db, tokens) != 0) rc = -1;
    }
    if (rc == 0) rc = user_database_clear_workspace(zdb.db, workspace_id);
    const char *tail_tables[] = {"cbm_workspace_health_v1", "cbm_workspace_index_state_v1",
        "cbm_generation_integrity_v2", "cbm_database_generation_v1"};
    for (size_t i = 0; rc == 0 && i < sizeof(tail_tables)/sizeof(tail_tables[0]); i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE workspace_key=?1", tail_tables[i]);
        zova_statement *stmt = NULL;
        rc = prepare_zova(zdb.db, sql, &stmt, "user_delete.prepare");
        if (rc == 0) rc = workspace_bind_i64(zdb.db, stmt, 1, workspace_key,
                                              "user_delete.bind");
        if (rc == 0) rc = workspace_step_done(zdb.db, stmt, "user_delete.step");
        if (stmt) (void)zova_statement_finalize(stmt);
    }
    if (rc == 0) {
        zova_statement *stmt = NULL;
        rc = prepare_zova(zdb.db,
                          "DELETE FROM cbm_workspace_registry WHERE workspace_id=?1",
                          &stmt, "user_delete.registry_prepare");
        if (rc == 0) rc = workspace_bind_text(zdb.db, stmt, 1, workspace_id,
                                               "user_delete.registry_bind");
        if (rc == 0) rc = workspace_step_done(zdb.db, stmt,
                                               "user_delete.registry_step");
        if (stmt) (void)zova_statement_finalize(stmt);
    }
    if (rc == 0) rc = workspace_registry_commit(zdb.db);
    else workspace_registry_rollback(zdb.db);
    close_zova(&zdb);
    cbm_zova_writer_guard_release(&guard);
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
    int rc = graph_delete_named_if_exists(zdb.db, graph_name);
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
    if (rc == 0 && cbm_zova_vector_read_is_enabled()) {
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
    if (rc == 0) {
        rc = sidecar_schema_and_integrity_record(tmp_path, generation,
                                                 workspace_scoped ? workspace_id : "",
                                                 project ? project : "", workspace_scoped);
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
        "SELECT CAST(n.id AS TEXT) FROM nodes n "
        "WHERE n.project = ?1 "
        "AND n.label IN ('Function','Method','Class')";

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
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (status_ok(zova_statement_step(&(zova_statement_step_request){
                          .statement = stmt, .out_result = &step}),
                      db, "vector.candidates_count_step") != 0) { rc = -1; break; }
        if (step == ZOVA_STEP_DONE) break;
        char *id = column_text_owned(db, stmt, 0);
        zova_vector vector = {0};
        zova_status status = id ? zova_vector_get(&(zova_vector_get_request){
            .db = db, .collection_name = CBM_ZOVA_NODE_COLLECTION,
            .vector_id = id, .out_vector = &vector}) : ZOVA_INVALID_ARGUMENT;
        free(id);
        if (status == ZOVA_OK) { (*out_count)++; zova_vector_free(&vector); }
        else if (status != ZOVA_VECTOR_NOT_FOUND) rc = -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

static int count_node_collection_vectors(zova_database *db, int *out_count) {
    *out_count = 0;
    zova_vector_collection_info info = {0};
    zova_status status = zova_vector_collection_info_get(
        &(zova_vector_collection_info_get_request){
            .db = db, .name = CBM_ZOVA_NODE_COLLECTION, .out_info = &info});
    if (status == ZOVA_VECTOR_COLLECTION_NOT_FOUND) return 0;
    if (status != ZOVA_OK || info.vector_count > INT32_MAX) {
        if (status == ZOVA_OK) zova_vector_collection_info_free(&info);
        return -1;
    }
    *out_count = (int)info.vector_count;
    zova_vector_collection_info_free(&info);
    return 0;
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
        zova_vector vector = {0};
        zova_status vector_status = id ? zova_vector_get(&(zova_vector_get_request){
            .db = db, .collection_name = CBM_ZOVA_NODE_COLLECTION,
            .vector_id = id, .out_vector = &vector}) : ZOVA_INVALID_ARGUMENT;
        if (vector_status == ZOVA_OK) zova_vector_free(&vector);
        if (vector_status == ZOVA_VECTOR_NOT_FOUND) { free(id); continue; }
        if (vector_status != ZOVA_OK || !id || append_string(&ids, &count, &cap, id) != 0) {
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

int cbm_zova_vector_session_search_collection_i8(
    cbm_zova_vector_session_t *session, const char *collection, const int8_t *query_values,
    int query_count, int vector_dim, int prefilter_limit, int limit,
    cbm_zova_vector_hit_t **out, int *out_count) {
    if (!session || !collection || !collection[0] || !query_values || query_count <= 0 ||
        vector_dim <= 0 || prefilter_limit <= 0 || limit <= 0 || !out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    zova_vector_search_results results = {0};
    int rc = 0;
    if (query_count == 1) {
        zova_vector_values query = {
            .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
            .i8_values = query_values,
            .values_len = (size_t)vector_dim,
        };
        rc = status_ok(zova_vector_search(&(zova_vector_search_request){
                           .db = session->zdb.db, .collection_name = collection,
                           .query = query, .limit = (size_t)limit, .out_results = &results}),
                       session->zdb.db, "workspace_vector.search_i8");
    } else {
        rc = status_ok(zova_vector_search_multi_i8(&(zova_vector_search_multi_i8_request){
                           .db = session->zdb.db, .collection_name = collection,
                           .query_values = query_values,
                           .query_values_len = (size_t)query_count * (size_t)vector_dim,
                           .query_count = (size_t)query_count,
                           .dimensions = (size_t)vector_dim,
                           .candidate_ids = NULL, .candidate_count = 0,
                           .mode = ZOVA_VECTOR_MULTI_I8_SEARCH_CBM_PREFILTER_MIN_COSINE,
                           .aggregation = ZOVA_VECTOR_MULTI_I8_AGGREGATION_MIN_COSINE,
                           .prefilter_query_index = 0,
                           .prefilter_limit = (size_t)prefilter_limit,
                           .limit = (size_t)limit, .out_results = &results}),
                       session->zdb.db, "workspace_vector.search_multi_i8");
    }
    cbm_zova_vector_hit_t *items = NULL;
    if (rc == 0 && results.len > 0) {
        items = calloc(results.len, sizeof(*items));
        if (!items) rc = -1;
    }
    int count = 0;
    for (size_t i = 0; rc == 0 && i < results.len; i++) {
        items[i].vector_id = zv_strndup(results.items[i].id, results.items[i].id_len);
        if (!items[i].vector_id) {
            rc = -1;
            break;
        }
        items[i].score = 1.0 - results.items[i].distance;
        count++;
    }
    zova_vector_search_results_free(&results);
    if (rc != 0) {
        cbm_zova_vector_hits_free(items, count);
        return -1;
    }
    *out = items;
    *out_count = count;
    return 0;
}

int cbm_zova_vector_session_get_workspace_token_i8(
    cbm_zova_vector_session_t *session, const char *workspace_id,
    const char *model_fingerprint, int vector_dim, const char *token,
    int8_t *out_values, size_t out_len, bool *out_found) {
    if (!session || !workspace_name_component_valid(workspace_id) ||
        !workspace_name_component_valid(model_fingerprint) || vector_dim <= 0 ||
        !token || !token[0] || !out_values || out_len != (size_t)vector_dim || !out_found) {
        return -1;
    }
    *out_found = false;
    char collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    char vector_id[32];
    if (cbm_zova_workspace_token_vector_collection_name(
            workspace_id, model_fingerprint, vector_dim, collection, sizeof(collection)) != 0) {
        return -1;
    }
    zova_statement *statement = NULL;
    int rc = prepare_zova(session->zdb.db,
        "SELECT m.token_key FROM cbm_token_vector_metadata_v1 m "
        "JOIN cbm_workspace_registry w ON w.workspace_key=m.workspace_key "
        "WHERE w.workspace_id=?1 AND m.token=?2", &statement,
        "token_vector.key_prepare");
    if (rc == 0) rc = workspace_bind_text(session->zdb.db, statement, 1, workspace_id,
                                           "token_vector.key_workspace");
    if (rc == 0) rc = workspace_bind_text(session->zdb.db, statement, 2, token,
                                           "token_vector.key_token");
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && status_ok(zova_statement_step(&(zova_statement_step_request){
            .statement = statement, .out_result = &step}), session->zdb.db,
            "token_vector.key_step") != 0) rc = -1;
    if (rc == 0 && step == ZOVA_STEP_DONE) {
        (void)zova_statement_finalize(statement);
        return 0;
    }
    int64_t token_key = 0;
    if (rc == 0 && (step != ZOVA_STEP_ROW || status_ok(zova_statement_column_int64(
            &(zova_statement_column_int64_request){.statement=statement,.index=0,
                                                    .out_value=&token_key}),
            session->zdb.db, "token_vector.key_value") != 0 ||
            user_private_key_id(token_key, vector_id) != 0)) rc = -1;
    if (statement) (void)zova_statement_finalize(statement);
    if (rc != 0) return -1;
    zova_vector vector = {0};
    zova_status status = zova_vector_get(&(zova_vector_get_request){
        .db = session->zdb.db, .collection_name = collection,
        .vector_id = vector_id, .out_vector = &vector});
    if (status == ZOVA_VECTOR_NOT_FOUND) return 0;
    if (status != ZOVA_OK || vector.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
        vector.values_len != out_len || !vector.i8_values) {
        if (status == ZOVA_OK) zova_vector_free(&vector);
        return -1;
    }
    memcpy(out_values, vector.i8_values, out_len);
    zova_vector_free(&vector);
    *out_found = true;
    return 0;
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
    int rc = 0;
    const char *sql = NULL;
    if (workspace_metadata_table_exists(session->zdb.db, "cbm_nodes_v1")) {
        sql = "SELECT 1 FROM cbm_nodes_v1 n JOIN cbm_workspace_registry r "
              "ON r.workspace_key=n.workspace_key WHERE r.workspace_id=?1 LIMIT 1";
    } else if (workspace_metadata_table_exists(session->zdb.db,
                                                "cbm_zova_generation_integrity_v1")) {
        sql = "SELECT 1 FROM cbm_zova_generation_integrity_v1 "
              "WHERE workspace_id=?1 LIMIT 1";
    } else {
        return 0;
    }
    if (prepare_zova(session->zdb.db, sql, &stmt,
                     "vector_session.workspace_prepare") != 0 ||
        workspace_bind_text(session->zdb.db, stmt, 1, workspace_id,
                            "vector_session.workspace_bind") != 0) {
        if (stmt) {
            (void)zova_statement_finalize(stmt);
            stmt = NULL;
        }
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
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
    int64_t *node_keys = calloc((size_t)count, sizeof(*node_keys));
    if (!filled || !node_keys) {
        free(node_keys);
        free(filled);
        return -1;
    }
    int rc = 0;
    if (!workspace_metadata_table_exists(db, "cbm_nodes_v1")) {
        free(node_keys);
        free(filled);
        return -1;
    }
    int64_t workspace_key = 0;
    if (workspace_key_for_id(db, workspace_id, &workspace_key) != 0) {
        free(node_keys);
        free(filled);
        return -1;
    }
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
        free(node_keys);
        free(filled);
        return -1;
    }
    zova_graph_scan_cursor cursor = {0};
    bool more = true;
    while (rc == 0 && more) {
        zova_graph_scan_results scan = {0};
        zova_status status = zova_graph_scan(&(zova_graph_scan_request){
            .db = db, .graph_name = graph_name, .node_after = cursor,
            .node_limit = 4096, .edge_limit = 0, .out_results = &scan});
        if (status != ZOVA_OK) {
            rc = -1;
        } else {
            for (size_t i = 0; i < scan.nodes_len; i++) {
                int visit_index = graph_visit_find(index, count, scan.nodes[i].node_id);
                if (visit_index >= 0) node_keys[visit_index] = scan.nodes[i].node_key;
            }
            more = scan.has_more_nodes != 0;
            if (scan.nodes_len > 0) {
                const zova_graph_scan_node *last = &scan.nodes[scan.nodes_len - 1];
                cursor.created_order = last->created_order;
                cursor.key = last->node_key;
            } else if (more) {
                rc = -1;
            }
        }
        zova_graph_scan_results_free(&scan);
    }
    for (int offset = 0; rc == 0 && offset < count; offset += ZV_GRAPH_HYDRATE_BATCH) {
        int batch_count = count - offset;
        if (batch_count > ZV_GRAPH_HYDRATE_BATCH) {
            batch_count = ZV_GRAPH_HYDRATE_BATCH;
        }
        for (int i = 0; i < batch_count; i++) {
            if (node_keys[offset + i] <= 0) {
                rc = -1;
                break;
            }
        }
        zova_graph_keyed_node_results keyed = {0};
        if (rc == 0 && zova_graph_nodes_get_many_keyed(
                           &(zova_graph_nodes_get_many_keyed_request){
                               .db = db, .graph_name = graph_name,
                               .node_keys = node_keys + offset,
                               .key_count = (size_t)batch_count,
                               .out_results = &keyed}) != ZOVA_OK) {
            rc = -1;
        }
        if (rc == 0 && keyed.len != (size_t)batch_count) rc = -1;
        for (int i = 0; rc == 0 && i < batch_count; i++) {
            if (!keyed.items[i].found || keyed.items[i].node_key != node_keys[offset + i] ||
                strcmp(keyed.items[i].node_id, visits[offset + i].node_id) != 0) {
                rc = -1;
            }
        }
        zova_graph_keyed_node_results_free(&keyed);
        if (rc != 0) break;

        char sql[ZV_SQL_MAX * 8];
        int length = snprintf(
            sql, sizeof(sql),
            "SELECT n.zova_node_key,n.name,n.qualified_name,f.file_path,n.label,n.start_line,"
            "n.end_line,n.properties FROM cbm_nodes_v1 n "
            "JOIN cbm_files_v1 f ON f.file_key=n.file_key "
            "WHERE n.workspace_key=?1 AND n.zova_node_key IN (");
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
            rc = workspace_bind_i64(db, stmt, 1, workspace_key, "graph.hydrate_bind_workspace");
        }
        for (int i = 0; rc == 0 && i < batch_count; i++) {
            rc = workspace_bind_i64(db, stmt, i + 2, node_keys[offset + i],
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
            int64_t node_key = 0;
            if (status_ok(zova_statement_column_int64(
                              &(zova_statement_column_int64_request){
                                  .statement = stmt, .index = 0, .out_value = &node_key}),
                          db, "graph.hydrate_node_key") != 0) {
                rc = -1;
                break;
            }
            int visit_index = -1;
            for (int i = 0; i < batch_count; i++) {
                if (node_keys[offset + i] == node_key) {
                    visit_index = offset + i;
                    break;
                }
            }
            if (visit_index < offset || visit_index >= offset + batch_count || filled[visit_index]) {
                rc = -1;
                break;
            }
            cbm_zova_graph_visit_t *visit = &visits[visit_index];
            visit->name = column_text_owned(db, stmt, 1);
            visit->qualified_name = column_text_owned(db, stmt, 2);
            visit->file_path = column_text_owned(db, stmt, 3);
            visit->label = column_text_owned(db, stmt, 4);
            visit->properties = column_text_owned(db, stmt, 7);
            int64_t start_line = 0;
            int64_t end_line = 0;
            zova_statement_column_int64_request start_req = {
                .statement = stmt, .index = 5, .out_value = &start_line};
            zova_statement_column_int64_request end_req = {
                .statement = stmt, .index = 6, .out_value = &end_line};
            if (!visit->name || !visit->qualified_name || !visit->file_path || !visit->label ||
                !visit->properties ||
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
    free(node_keys);
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

static int migration_column_text_equals(zova_database *db, zova_statement *stmt, int column,
                                        const char *expected) {
    zova_text text = {0};
    int rc = status_ok(zova_statement_column_text(&(zova_statement_column_text_request){
                                                       .statement = stmt,
                                                       .index = column,
                                                       .out_text = &text,
                                                   }),
                       db, "migration.manifest.text");
    size_t expected_len = strlen(expected ? expected : "");
    if (rc == 0 && (text.len != expected_len ||
                    memcmp(text.data ? text.data : "", expected ? expected : "", expected_len) !=
                        0))
        rc = -1;
    zova_text_free(&text);
    return rc;
}

static int migration_step_row(zova_database *db, zova_statement *stmt) {
    zova_step_result step = ZOVA_STEP_DONE;
    return status_ok(zova_statement_step(&(zova_statement_step_request){
                                             .statement = stmt, .out_result = &step}),
                     db, "migration.manifest.step") == 0 &&
                   step == ZOVA_STEP_ROW
               ? 0
               : -1;
}

static int migration_live_nodes(zova_database *db, const char *workspace_id,
                                const cbm_zova_legacy_snapshot_t *source) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    int64_t count = 0;
    if (zova_query_i64(db, "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_id=?1",
                       workspace_id, &count, "migration.manifest.node_count") != 0 ||
        count != input->node_count)
        return -1;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "SELECT project,label,name,qualified_name,file_path,start_line,end_line,properties "
        "FROM cbm_nodes_v1 WHERE workspace_id=?1 AND node_id=?2",
        &stmt, "migration.manifest.node_prepare");
    for (int i = 0; rc == 0 && i < input->node_count; i++) {
        const CBMDumpNode *node = &input->nodes[i];
        const char *stable_id = cbm_zova_legacy_snapshot_target_id(source, i);
        rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.node_ws");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 2, stable_id, "migration.manifest.node_id");
        if (rc == 0) rc = migration_step_row(db, stmt);
        if (rc == 0) rc = migration_column_text_equals(db, stmt, 0, node->project);
        if (rc == 0) rc = migration_column_text_equals(db, stmt, 1, node->label);
        if (rc == 0) rc = migration_column_text_equals(db, stmt, 2, node->name);
        if (rc == 0) rc = migration_column_text_equals(db, stmt, 3, node->qualified_name);
        if (rc == 0) rc = migration_column_text_equals(db, stmt, 4, node->file_path);
        int64_t start = 0, end = 0;
        if (rc == 0)
            rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                                                            .statement = stmt,
                                                            .index = 5,
                                                            .out_value = &start}),
                           db, "migration.manifest.node_start");
        if (rc == 0)
            rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                                                            .statement = stmt,
                                                            .index = 6,
                                                            .out_value = &end}),
                           db, "migration.manifest.node_end");
        if (rc == 0 && (start != node->start_line || end != node->end_line)) rc = -1;
        if (rc == 0) rc = migration_column_text_equals(db, stmt, 7, node->properties);
        if (rc == 0)
            rc = user_database_reset_statement(db, stmt, "migration.manifest.node_reset");
    }
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int migration_source_node_index(const cbm_zova_workspace_generation_input_t *input,
                                       int64_t source_id) {
    for (int i = 0; i < input->node_count; i++)
        if (input->nodes[i].id == source_id) return i;
    return -1;
}

static int migration_live_edges(zova_database *db, const char *workspace_id,
                                const cbm_zova_legacy_snapshot_t *source) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    cbm_zova_workspace_generation_result_t expected = {0};
    if (cbm_zova_workspace_generation_digest_input(workspace_id, input, &expected) != 0)
        return -1;
    int64_t count = 0;
    if (zova_query_i64(db, "SELECT count(*) FROM cbm_edges_v1 WHERE workspace_id=?1",
                       workspace_id, &count, "migration.manifest.edge_count") != 0 ||
        count != (int64_t)expected.metadata_edges)
        return -1;
    for (int i = 0; i < input->edge_count; i++) {
        const CBMDumpEdge *edge = &input->edges[i];
        int source_index = migration_source_node_index(input, edge->source_id);
        int target_index = migration_source_node_index(input, edge->target_id);
        if (source_index < 0 || target_index < 0) return -1;
        const char *source_id = cbm_zova_legacy_snapshot_target_id(source, source_index);
        const char *target_id = cbm_zova_legacy_snapshot_target_id(source, target_index);
        char edge_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (workspace_edge_id_v1(workspace_id, source_id, edge->type, target_id,
                                 edge->local_name ? edge->local_name : "", edge_id,
                                 sizeof(edge_id)) != 0)
            return -1;
        zova_statement *stmt = NULL;
        int rc = prepare_zova(
            db,
            "SELECT source_node_id,target_node_id,edge_type,properties,url_path,local_name FROM "
            "cbm_edges_v1 WHERE workspace_id=?1 AND edge_id=?2",
            &stmt, "migration.manifest.edge_prepare");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.edge_ws");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 2, edge_id, "migration.manifest.edge_id");
        if (rc == 0) rc = migration_step_row(db, stmt);
        const char *values[] = {source_id, target_id, edge->type, edge->properties,
                                edge->url_path ? edge->url_path : "",
                                edge->local_name ? edge->local_name : ""};
        for (int column = 0; rc == 0 && column < 6; column++)
            rc = migration_column_text_equals(db, stmt, column, values[column]);
        if (stmt) (void)zova_statement_finalize(stmt);
        if (rc != 0) return -1;
    }
    return 0;
}

static int migration_live_project(zova_database *db, const char *workspace_id,
                                  const cbm_zova_legacy_snapshot_t *source) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "SELECT p.project,p.root_path,p.indexed_at,r.canonical_root FROM cbm_projects_v1 p JOIN "
        "cbm_workspace_registry r USING(workspace_id) WHERE p.workspace_id=?1",
        &stmt, "migration.manifest.project_prepare");
    if (rc == 0)
        rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.project_ws");
    if (rc == 0) rc = migration_step_row(db, stmt);
    const char *values[] = {input->project, input->root_path, input->indexed_at, input->root_path};
    for (int column = 0; rc == 0 && column < 4; column++)
        rc = migration_column_text_equals(db, stmt, column, values[column]);
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int migration_live_hashes_summary(zova_database *db, const char *workspace_id,
                                         const cbm_zova_legacy_snapshot_t *source) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    int64_t count = 0;
    if (zova_query_i64(db, "SELECT count(*) FROM cbm_file_hashes_v1 WHERE workspace_id=?1",
                       workspace_id, &count, "migration.manifest.hash_count") != 0 ||
        count != input->file_hash_count)
        return -1;
    for (int i = 0; i < input->file_hash_count; i++) {
        zova_statement *stmt = NULL;
        int rc = prepare_zova(
            db,
            "SELECT content_hash,mtime_ns,size_bytes FROM cbm_file_hashes_v1 WHERE "
            "workspace_id=?1 AND file_path=?2",
            &stmt, "migration.manifest.hash_prepare");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.hash_ws");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 2, input->file_hashes[i].file_path,
                                     "migration.manifest.hash_path");
        if (rc == 0) rc = migration_step_row(db, stmt);
        if (rc == 0)
            rc = migration_column_text_equals(db, stmt, 0, input->file_hashes[i].content_hash);
        int64_t mtime = 0, size = 0;
        if (rc == 0)
            rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                                                            .statement = stmt,
                                                            .index = 1,
                                                            .out_value = &mtime}),
                           db, "migration.manifest.hash_mtime");
        if (rc == 0)
            rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                                                            .statement = stmt,
                                                            .index = 2,
                                                            .out_value = &size}),
                           db, "migration.manifest.hash_size");
        if (rc == 0 && (mtime != input->file_hashes[i].mtime_ns ||
                        size != input->file_hashes[i].size_bytes))
            rc = -1;
        if (stmt) (void)zova_statement_finalize(stmt);
        if (rc != 0) return -1;
    }
    int64_t summary_count = 0;
    if (zova_query_i64(db,
                       "SELECT count(*) FROM cbm_project_summaries_v2 WHERE workspace_id=?1",
                       workspace_id, &summary_count, "migration.manifest.summary_count") != 0 ||
        summary_count != (input->project_summary.present ? 1 : 0))
        return -1;
    if (!input->project_summary.present) return 0;
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "SELECT summary,source_hash,created_at,updated_at FROM cbm_project_summaries_v2 WHERE "
        "workspace_id=?1",
        &stmt, "migration.manifest.summary_prepare");
    if (rc == 0)
        rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.summary_ws");
    if (rc == 0) rc = migration_step_row(db, stmt);
    const char *values[] = {input->project_summary.summary, input->project_summary.source_hash,
                            input->project_summary.created_at, input->project_summary.updated_at};
    for (int column = 0; rc == 0 && column < 4; column++)
        rc = migration_column_text_equals(db, stmt, column, values[column]);
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int migration_live_vectors(zova_database *db, const char *workspace_id,
                                  const cbm_zova_legacy_snapshot_t *source,
                                  uint64_t *out_node_native, uint64_t *out_token_native) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    uint64_t expected_native[2] = {0, 0};
    for (int i = 0; i < input->node_vector_count; i++)
        if (workspace_vector_nonzero(input->node_vectors[i].vector,
                                     (size_t)input->node_vectors[i].vector_len))
            expected_native[0]++;
    for (int i = 0; i < input->token_vector_count; i++)
        if (workspace_vector_nonzero(input->token_vectors[i].vector,
                                     (size_t)input->token_vectors[i].vector_len))
            expected_native[1]++;
    int64_t token_metadata_rows = 0;
    if (zova_query_i64(
            db,
            "SELECT count(*) FROM cbm_token_vector_metadata_v1 WHERE workspace_id=?1",
            workspace_id, &token_metadata_rows, "migration.manifest.token_metadata_rows") != 0 ||
        token_metadata_rows != (int64_t)expected_native[1])
        return -1;
    char node_collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    char token_collection[ZV_ID_MAX + CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (cbm_zova_workspace_node_vector_collection_name(
            workspace_id, input->model_fingerprint, input->vector_dimensions, node_collection,
            sizeof(node_collection)) != 0 ||
        cbm_zova_workspace_token_vector_collection_name(
            workspace_id, input->model_fingerprint, input->vector_dimensions, token_collection,
            sizeof(token_collection)) != 0)
        return -1;
    zova_vector_collection_list collections = {0};
    if (zova_vector_collections_list(&(zova_vector_collections_list_request){
            .db = db, .out_list = &collections}) != ZOVA_OK)
        return -1;
    int node_collection_found = 0, token_collection_found = 0;
    for (size_t i = 0; i < collections.len; i++) {
        if (collections.items[i].name && strcmp(collections.items[i].name, node_collection) == 0)
            node_collection_found = 1;
        if (collections.items[i].name && strcmp(collections.items[i].name, token_collection) == 0)
            token_collection_found = 1;
    }
    zova_vector_collection_list_free(&collections);
    if (!node_collection_found || !token_collection_found) return -1;
    const char *collection_names[] = {node_collection, token_collection};
    for (int i = 0; i < 2; i++) {
        zova_vector_collection_info info = {0};
        zova_status status = zova_vector_collection_info_get(
            &(zova_vector_collection_info_get_request){
                .db = db, .name = collection_names[i], .out_info = &info});
        int valid = status == ZOVA_OK && info.element_type == ZOVA_VECTOR_ELEMENT_TYPE_I8 &&
                    info.dimensions == (uint32_t)input->vector_dimensions &&
                    info.vector_count == expected_native[i];
        if (status == ZOVA_OK) zova_vector_collection_info_free(&info);
        if (!valid) return -1;
    }
    *out_node_native = 0;
    for (int i = 0; i < input->node_vector_count; i++) {
        int node_index = migration_source_node_index(input, input->node_vectors[i].node_id);
        if (node_index < 0) return -1;
        const char *stable_id = cbm_zova_legacy_snapshot_target_id(source, node_index);
        zova_vector native = {0};
        zova_status status = zova_vector_get(&(zova_vector_get_request){
            .db = db, .collection_name = node_collection, .vector_id = stable_id,
            .out_vector = &native});
        int nonzero = workspace_vector_nonzero(input->node_vectors[i].vector,
                                               (size_t)input->node_vectors[i].vector_len);
        if ((!nonzero && status != ZOVA_VECTOR_NOT_FOUND) ||
            (nonzero && (status != ZOVA_OK || native.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
                         native.values_len != (size_t)input->vector_dimensions ||
                         memcmp(native.i8_values, input->node_vectors[i].vector,
                                (size_t)input->vector_dimensions) != 0))) {
            if (status == ZOVA_OK) zova_vector_free(&native);
            return -1;
        }
        if (status == ZOVA_OK) {
            (*out_node_native)++;
            zova_vector_free(&native);
        }
    }
    *out_token_native = 0;
    for (int i = 0; i < input->token_vector_count; i++) {
        char token_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (cbm_zova_workspace_token_id_v1(workspace_id, input->token_vectors[i].token, token_id,
                                           sizeof(token_id)) != 0)
            return -1;
        zova_vector native = {0};
        zova_status status = zova_vector_get(&(zova_vector_get_request){
            .db = db, .collection_name = token_collection, .vector_id = token_id,
            .out_vector = &native});
        int nonzero = workspace_vector_nonzero(input->token_vectors[i].vector,
                                               (size_t)input->token_vectors[i].vector_len);
        if (nonzero) {
            zova_statement *metadata = NULL;
            int metadata_rc = prepare_zova(
                db,
                "SELECT token_id,token,idf FROM cbm_token_vector_metadata_v1 "
                "WHERE workspace_id=?1 AND token_id=?2",
                &metadata, "migration.manifest.token_metadata_prepare");
            if (metadata_rc == 0)
                metadata_rc = workspace_bind_text(db, metadata, 1, workspace_id,
                                                  "migration.manifest.token_metadata_ws");
            if (metadata_rc == 0)
                metadata_rc = workspace_bind_text(db, metadata, 2, token_id,
                                                  "migration.manifest.token_metadata_id");
            if (metadata_rc == 0) metadata_rc = migration_step_row(db, metadata);
            if (metadata_rc == 0)
                metadata_rc = migration_column_text_equals(db, metadata, 0, token_id);
            if (metadata_rc == 0)
                metadata_rc = migration_column_text_equals(
                    db, metadata, 1, input->token_vectors[i].token);
            double idf = 0.0;
            if (metadata_rc == 0)
                metadata_rc = status_ok(zova_statement_column_double(
                                            &(zova_statement_column_double_request){
                                                .statement = metadata,
                                                .index = 2,
                                                .out_value = &idf}),
                                        db, "migration.manifest.token_metadata_idf");
            if (metadata_rc == 0 && idf != input->token_vectors[i].idf) metadata_rc = -1;
            if (metadata) (void)zova_statement_finalize(metadata);
            if (metadata_rc != 0) {
                if (status == ZOVA_OK) zova_vector_free(&native);
                return -1;
            }
        }
        if ((!nonzero && status != ZOVA_VECTOR_NOT_FOUND) ||
            (nonzero && (status != ZOVA_OK || native.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
                         native.values_len != (size_t)input->vector_dimensions ||
                         memcmp(native.i8_values, input->token_vectors[i].vector,
                                (size_t)input->vector_dimensions) != 0))) {
            if (status == ZOVA_OK) zova_vector_free(&native);
            return -1;
        }
        if (status == ZOVA_OK) {
            (*out_token_native)++;
            zova_vector_free(&native);
        }
    }
    return 0;
}

static int migration_live_graph(zova_database *db, const char *workspace_id,
                                const cbm_zova_legacy_snapshot_t *source, uint64_t *out_nodes,
                                uint64_t *out_edges) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0)
        return -1;
    zova_graph_info graph = {0};
    if (zova_graph_info_get(&(zova_graph_info_get_request){
            .db = db, .name = graph_name, .out_info = &graph}) != ZOVA_OK)
        return -1;
    *out_nodes = graph.node_count;
    *out_edges = graph.edge_count;
    zova_graph_info_free(&graph);
    if (*out_nodes != (uint64_t)input->node_count) return -1;
    uint64_t adjacency_count = 0;
    for (int i = 0; i < input->node_count; i++) {
        const char *stable_id = cbm_zova_legacy_snapshot_target_id(source, i);
        zova_graph_node node = {0};
        zova_status status = zova_graph_node_get(&(zova_graph_node_get_request){
            .db = db, .graph_name = graph_name, .node_id = stable_id, .out_node = &node});
        if (status != ZOVA_OK || !node.node_id || strcmp(node.node_id, stable_id) != 0) {
            if (status == ZOVA_OK) zova_graph_node_free(&node);
            return -1;
        }
        zova_graph_node_free(&node);
        zova_graph_neighbor_results neighbors = {0};
        status = zova_graph_neighbors(&(zova_graph_neighbors_request){
            .db = db,
            .graph_name = graph_name,
            .node_id = stable_id,
            .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
            .limit = (size_t)input->edge_count + 1,
            .out_results = &neighbors,
        });
        if (status != ZOVA_OK) return -1;
        adjacency_count += neighbors.len;
        for (int e = 0; e < input->edge_count; e++) {
            if (input->edges[e].source_id != input->nodes[i].id) continue;
            int target_index = migration_source_node_index(input, input->edges[e].target_id);
            const char *target_id = cbm_zova_legacy_snapshot_target_id(source, target_index);
            int found = 0;
            for (size_t n = 0; target_id && n < neighbors.len; n++) {
                if (strcmp(neighbors.items[n].node_id, target_id) == 0 &&
                    strcmp(neighbors.items[n].edge_type, input->edges[e].type) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                zova_graph_neighbor_results_free(&neighbors);
                return -1;
            }
        }
        zova_graph_neighbor_results_free(&neighbors);
    }
    return adjacency_count == *out_edges ? 0 : -1;
}

static int migration_fts_phrase(const char *column, const char *value, char *out,
                                size_t out_size) {
    int prefix = snprintf(out, out_size, "%s : \"", column);
    if (prefix < 0 || (size_t)prefix >= out_size) return -1;
    size_t offset = (size_t)prefix;
    for (const char *p = value; *p && offset + 3 < out_size; p++) {
        if (*p == '"') out[offset++] = '"';
        out[offset++] = *p;
    }
    if (offset + 2 >= out_size) return -1;
    out[offset++] = '"';
    out[offset] = '\0';
    return 0;
}

static int migration_live_fts(zova_database *db, const char *workspace_id,
                              const cbm_zova_legacy_snapshot_t *source,
                              const char *row_digest, char out_digest[CBM_ZOVA_DIGEST_HEX_SIZE]) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    char sql[1024];
    int64_t row_count = 0;
    if (zova_query_i64(db,
                       "SELECT count(*) FROM cbm_nodes_fts_v1 WHERE workspace_id=?1",
                       workspace_id, &row_count, "migration.manifest.fts_count") != 0 ||
        row_count != input->node_count)
        return -1;
    const char *columns[] = {"name", "qualified_name", "label", "file_path"};
    for (int i = 0; i < input->node_count; i++) {
        const char *values[] = {input->nodes[i].name, input->nodes[i].qualified_name,
                                input->nodes[i].label, input->nodes[i].file_path};
        const char *stable_id = cbm_zova_legacy_snapshot_target_id(source, i);
        int rc = 0;
        for (int column = 0; column < 4; column++) {
            if (!cbm_zova_migration_fts_value_may_tokenize(values[column])) continue;
            char phrase[2048];
            if (migration_fts_phrase(columns[column], values[column], phrase, sizeof(phrase)) != 0 ||
                snprintf(sql, sizeof(sql),
                         "SELECT count(*) FROM cbm_nodes_fts_v1 WHERE workspace_id=?1 "
                         "AND node_id=?2 AND cbm_nodes_fts_v1 MATCH ?3") >= (int)sizeof(sql))
                return -1;
            zova_statement *match = NULL;
            rc = prepare_zova(db, sql, &match, "migration.manifest.fts_row_prepare");
            if (rc == 0)
                rc = workspace_bind_text(db, match, 1, workspace_id,
                                         "migration.manifest.fts_workspace");
            if (rc == 0)
                rc = workspace_bind_text(db, match, 2, stable_id,
                                         "migration.manifest.fts_node_id");
            if (rc == 0)
                rc = workspace_bind_text(db, match, 3, phrase, "migration.manifest.fts_phrase");
            if (rc == 0) rc = migration_step_row(db, match);
            int64_t matches = 0;
            if (rc == 0)
                rc = status_ok(zova_statement_column_int64(
                                   &(zova_statement_column_int64_request){
                                       .statement = match, .index = 0, .out_value = &matches}),
                               db, "migration.manifest.fts_row_count");
            if (match) (void)zova_statement_finalize(match);
            if (rc != 0 || matches != 1) return -1;
        }
    }
    int query_count = 0;
    const char *const *queries = cbm_zova_legacy_snapshot_fts_queries(source, &query_count);
    if (query_count < 0 || query_count > 512 || (query_count > 0 && !queries)) return -1;
    cbm_sha256_ctx results;
    cbm_sha256_init(&results);
    if (snprintf(sql, sizeof(sql),
                 "SELECT f.node_id,bm25(cbm_nodes_fts_v1) FROM cbm_nodes_fts_v1 f "
                 "JOIN cbm_nodes_v1 n ON n.workspace_id=f.workspace_id AND n.node_id=f.node_id "
                 "WHERE f.workspace_id=?1 AND cbm_nodes_fts_v1 MATCH ?2 "
                 "ORDER BY bm25(cbm_nodes_fts_v1),n.qualified_name,n.node_id") >=
        (int)sizeof(sql))
        return -1;
    for (int q = 0; q < query_count; q++) {
        zova_statement *stmt = NULL;
        int rc = prepare_zova(db, sql, &stmt, "migration.manifest.fts_query_prepare");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.fts_query_ws");
        if (rc == 0)
            rc = workspace_bind_text(db, stmt, 2, queries[q], "migration.manifest.fts_query");
        int rank = 0;
        while (rc == 0) {
            zova_step_result step = ZOVA_STEP_DONE;
            rc = status_ok(zova_statement_step(&(zova_statement_step_request){
                                                    .statement = stmt, .out_result = &step}),
                           db, "migration.manifest.fts_query_step");
            if (rc != 0 || step == ZOVA_STEP_DONE) break;
            zova_text stable = {0};
            double score = 0.0;
            rc = status_ok(zova_statement_column_text(&(zova_statement_column_text_request){
                                                           .statement = stmt,
                                                           .index = 0,
                                                           .out_text = &stable}),
                           db, "migration.manifest.fts_query_id");
            if (rc == 0)
                rc = status_ok(zova_statement_column_double(
                                   &(zova_statement_column_double_request){
                                       .statement = stmt, .index = 1, .out_value = &score}),
                               db, "migration.manifest.fts_query_score");
            if (rc == 0) {
                char rank_text[32], score_text[64];
                double normalized = nearbyint(score * 1e12) / 1e12;
                snprintf(rank_text, sizeof(rank_text), "%d", rank++);
                snprintf(score_text, sizeof(score_text), "%.12f", normalized);
                cbm_zova_migration_digest_text(&results, queries[q]);
                cbm_zova_migration_digest_text(&results, rank_text);
                char *stable_owned = zv_strndup(stable.data ? stable.data : "", stable.len);
                if (!stable_owned) rc = -1;
                else {
                    cbm_zova_migration_digest_text(&results, stable_owned);
                    free(stable_owned);
                }
                cbm_zova_migration_digest_text(&results, score_text);
            }
            zova_text_free(&stable);
        }
        if (stmt) (void)zova_statement_finalize(stmt);
        if (rc != 0) return -1;
    }
    char query_digest[CBM_ZOVA_DIGEST_HEX_SIZE];
    cbm_zova_migration_digest_finalize(&results, query_digest);
    cbm_sha256_ctx combined;
    cbm_sha256_init(&combined);
    cbm_zova_migration_digest_text(&combined, row_digest);
    cbm_zova_migration_digest_text(&combined, query_digest);
    cbm_zova_migration_digest_finalize(&combined, out_digest);
    return 0;
}

static int migration_integrity_manifest(zova_database *db, const char *workspace_id,
                                        int64_t generation,
                                        const cbm_zova_workspace_generation_result_t *source_rows,
                                        const cbm_zova_migration_manifest_t *source_manifest,
                                        cbm_zova_migration_manifest_t *out) {
    zova_statement *stmt = NULL;
    int rc = prepare_zova(
        db,
        "SELECT graph_nodes,graph_edges,metadata_nodes,metadata_topology_edges,node_vectors,"
        "token_vectors,metadata_sha256,fts_sha256,topology_sha256,node_vector_sha256,"
        "token_vector_sha256 FROM cbm_generation_integrity_v2 WHERE workspace_id=?1 AND "
        "generation=?2",
        &stmt, "migration.manifest.integrity_prepare");
    if (rc == 0)
        rc = workspace_bind_text(db, stmt, 1, workspace_id, "migration.manifest.integrity_ws");
    if (rc == 0)
        rc = workspace_bind_i64(db, stmt, 2, generation, "migration.manifest.integrity_gen");
    if (rc == 0) rc = migration_step_row(db, stmt);
    int64_t counts[6] = {0};
    for (int i = 0; rc == 0 && i < 6; i++)
        rc = status_ok(zova_statement_column_int64(&(zova_statement_column_int64_request){
                                                        .statement = stmt,
                                                        .index = i,
                                                        .out_value = &counts[i]}),
                       db, "migration.manifest.integrity_count");
    uint64_t expected_counts[] = {source_manifest->graph_node_count,
                                  source_manifest->graph_edge_count,
                                  source_manifest->stable_id_count,
                                  source_manifest->graph_edge_count,
                                  source_manifest->node_vector_count,
                                  source_manifest->token_vector_count};
    for (int i = 0; rc == 0 && i < 6; i++)
        if ((uint64_t)counts[i] != expected_counts[i]) rc = -1;
    const char *expected_digests[] = {
        source_rows->metadata_sha256, source_rows->fts_sha256, source_rows->topology_sha256,
        source_rows->node_vector_sha256, source_rows->token_vector_sha256};
    for (int i = 0; rc == 0 && i < 5; i++)
        rc = migration_column_text_equals(db, stmt, 6 + i, expected_digests[i]);
    if (stmt) (void)zova_statement_finalize(stmt);
    if (rc == 0) *out = *source_manifest;
    return rc;
}

int cbm_zova_migration_manifest_target_tx(zova_database *db, const char *workspace_id,
                                          int64_t generation,
                                          const cbm_zova_legacy_snapshot_t *source,
                                          cbm_zova_migration_manifest_t *out_manifest) {
    if (out_manifest) memset(out_manifest, 0, sizeof(*out_manifest));
    if (!db || !workspace_id || !workspace_id[0] || generation <= 0 || !source || !out_manifest)
        return CBM_ZOVA_MIGRATION_VERIFY_FAILED;
    const cbm_zova_migration_manifest_t *source_manifest =
        cbm_zova_legacy_snapshot_manifest(source);
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(source);
    cbm_zova_workspace_generation_result_t source_rows = {0};
    int64_t workspace_count = 0;
    uint64_t graph_nodes = 0, graph_edges = 0, node_vectors = 0, token_vectors = 0;
    char live_fts_digest[CBM_ZOVA_DIGEST_HEX_SIZE] = {0};
#define MIGRATION_MANIFEST_REQUIRE(condition, phase)                                             \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            fprintf(stderr, "migration manifest verification failed: %s\n", phase);             \
            return CBM_ZOVA_MIGRATION_VERIFY_FAILED;                                              \
        }                                                                                         \
    } while (0)
    MIGRATION_MANIFEST_REQUIRE(source_manifest && input, "source manifest");
    MIGRATION_MANIFEST_REQUIRE(
        strcmp(workspace_id, cbm_zova_legacy_snapshot_workspace_id(source)) == 0,
        "workspace identity");
    MIGRATION_MANIFEST_REQUIRE(
        cbm_zova_workspace_generation_digest_input(workspace_id, input, &source_rows) == 0,
        "source row digests");
    MIGRATION_MANIFEST_REQUIRE(
        zova_query_i64(db, "SELECT count(*) FROM cbm_projects_v1 WHERE workspace_id=?1",
                       workspace_id, &workspace_count, "migration.manifest.workspace_count") == 0 &&
            workspace_count == 1,
        "workspace count");
    MIGRATION_MANIFEST_REQUIRE(!cbm_zova_migration_test_fault("migration_after_counts"),
                               "fault after counts");
    MIGRATION_MANIFEST_REQUIRE(migration_live_project(db, workspace_id, source) == 0,
                               "project/root");
    MIGRATION_MANIFEST_REQUIRE(migration_live_nodes(db, workspace_id, source) == 0,
                               "stable IDs/nodes");
    MIGRATION_MANIFEST_REQUIRE(!cbm_zova_migration_test_fault("migration_after_stable_ids"),
                               "fault after stable IDs");
    MIGRATION_MANIFEST_REQUIRE(migration_live_edges(db, workspace_id, source) == 0, "edges");
    MIGRATION_MANIFEST_REQUIRE(migration_live_hashes_summary(db, workspace_id, source) == 0,
                               "hashes/summary");
    MIGRATION_MANIFEST_REQUIRE(!cbm_zova_migration_test_fault("migration_after_metadata"),
                               "fault after metadata");
    MIGRATION_MANIFEST_REQUIRE(
        migration_live_graph(db, workspace_id, source, &graph_nodes, &graph_edges) == 0,
        "native graph");
    MIGRATION_MANIFEST_REQUIRE(!cbm_zova_migration_test_fault("migration_after_graph"),
                               "fault after graph");
    MIGRATION_MANIFEST_REQUIRE(
        migration_live_vectors(db, workspace_id, source, &node_vectors, &token_vectors) == 0,
        "vectors");
    MIGRATION_MANIFEST_REQUIRE(!cbm_zova_migration_test_fault("migration_after_vectors"),
                               "fault after vectors");
    MIGRATION_MANIFEST_REQUIRE(
        migration_live_fts(db, workspace_id, source, source_rows.fts_sha256, live_fts_digest) == 0,
        "FTS rows/queries");
    MIGRATION_MANIFEST_REQUIRE(!cbm_zova_migration_test_fault("migration_after_fts"),
                               "fault after FTS");
    MIGRATION_MANIFEST_REQUIRE(strcmp(live_fts_digest, source_manifest->fts_sha256) == 0,
                               "FTS digest");
    MIGRATION_MANIFEST_REQUIRE(graph_nodes == source_manifest->graph_node_count &&
                                   graph_edges == source_manifest->graph_edge_count,
                               "graph inventory");
    MIGRATION_MANIFEST_REQUIRE(node_vectors == source_manifest->node_vector_count &&
                                   token_vectors == source_manifest->token_vector_count,
                               "vector inventory");
    MIGRATION_MANIFEST_REQUIRE(
        migration_integrity_manifest(db, workspace_id, generation, &source_rows, source_manifest,
                                     out_manifest) == 0,
        "integrity row");
#undef MIGRATION_MANIFEST_REQUIRE
    return CBM_ZOVA_MIGRATION_OK;
}

#endif

#if !CBM_WITH_ZOVA
int cbm_zova_migration_manifest_target_tx(zova_database *db, const char *workspace_id,
                                          int64_t generation,
                                          const cbm_zova_legacy_snapshot_t *source,
                                          cbm_zova_migration_manifest_t *out_manifest) {
    (void)db;
    (void)workspace_id;
    (void)generation;
    (void)source;
    if (out_manifest) memset(out_manifest, 0, sizeof(*out_manifest));
    return CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
}
#endif
