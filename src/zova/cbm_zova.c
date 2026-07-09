#include "zova/cbm_zova.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_regex.h"
#include "foundation/log.h"
#include "foundation/platform.h"

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
    }
    return "off";
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

int cbm_zova_after_sqlite_dump(const char *db_path) {
    (void)db_path;
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

int cbm_zova_mirror_graph(const char *zova_path) {
    (void)zova_path;
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

typedef struct {
    zova_database *db;
} cbm_zova_db_t;

enum {
    ZV_CAMEL_SPLIT_BUF = 2048,
    ZV_CAMEL_BUF_GUARD = 2,
};

#define ZV_DENOM_EPS_D 1e-10

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

int cbm_zova_after_sqlite_dump(const char *db_path) {
    cbm_zova_mode_t mode = cbm_zova_mode_from_env();
    if (mode == CBM_ZOVA_MODE_OFF) {
        return 0;
    }
    if (!db_path || !cbm_file_exists(db_path)) {
        cbm_log_error("zova.convert", "reason", "missing_db_path");
        return -1;
    }

    char zova_path[ZV_PATH_MAX];
    char tmp_path[ZV_PATH_MAX];
    if (cbm_zova_sidecar_path(db_path, zova_path, sizeof(zova_path)) != 0 ||
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.zova", zova_path) >= (int)sizeof(tmp_path)) {
        cbm_log_error("zova.convert", "reason", "path_too_long", "db_path", db_path);
        return -1;
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
        cbm_log_error("zova.convert", "status", zova_status_name(st), "msg", err.data ? err.data : "");
        zova_message_free(&err);
        cbm_unlink(tmp_path);
        return -1;
    }
    zova_message_free(&err);

    int rc = cbm_zova_validate_container(tmp_path);
    if (rc == 0 && mode >= CBM_ZOVA_MODE_I8_VECTORS) {
        rc = cbm_zova_mirror_i8_vectors(tmp_path, 768);
    }
    if (rc == 0 && mode >= CBM_ZOVA_MODE_GRAPH_MIRROR) {
        rc = cbm_zova_mirror_graph(tmp_path);
    }
    if (rc != 0) {
        cbm_unlink(tmp_path);
        return rc;
    }

    cbm_unlink(zova_path);
    if (rename(tmp_path, zova_path) != 0) {
        cbm_log_error("zova.convert", "phase", "rename", "dest", zova_path);
        cbm_unlink(tmp_path);
        return -1;
    }
    cbm_log_info("zova.convert", "mode", cbm_zova_mode_name(mode), "path", zova_path);
    return 0;
}

static int append_candidate(cbm_zova_node_candidate_t **items, int *count, int *cap) {
    if (*count < *cap) {
        memset(&(*items)[*count], 0, sizeof((*items)[*count]));
        return 0;
    }
    int next = *cap == 0 ? 16 : *cap * 2;
    cbm_zova_node_candidate_t *grown =
        (cbm_zova_node_candidate_t *)realloc(*items, (size_t)next * sizeof(**items));
    if (!grown) {
        return -1;
    }
    *items = grown;
    *cap = next;
    memset(&(*items)[*count], 0, sizeof((*items)[*count]));
    return 0;
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

static int collect_prefetch_candidate_ids(zova_database *db, const char *project, int vector_dim,
                                          char ***out_ids, int *out_count) {
    *out_ids = NULL;
    *out_count = 0;
    const char *sql =
        "SELECT CAST(n.id AS TEXT) "
        "FROM node_vectors v "
        "INNER JOIN nodes n ON n.id = v.node_id "
        "INNER JOIN _zova_vectors zv "
        "        ON zv.collection_name = 'cbm_node_vectors_i8' "
        "       AND zv.vector_id = CAST(n.id AS TEXT) "
        "WHERE v.project = ?1 "
        "  AND n.project = ?1 "
        "  AND n.label IN ('Function','Method','Class') "
        "  AND length(v.vector) = ?2";

    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, "vector.candidates_prepare");
    if (rc == 0) {
        zova_statement_bind_text_request preq = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)project,
            .len = strlen(project),
        };
        zova_statement_bind_int64_request dreq = {.statement = stmt, .index = 2, .value = vector_dim};
        if (status_ok(zova_statement_bind_text(&preq), db, "vector.candidates_bind_project") != 0 ||
            status_ok(zova_statement_bind_int64(&dreq), db, "vector.candidates_bind_dim") != 0) {
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

static int fetch_prefetch_node(zova_database *db, const char *project, int vector_dim,
                               int64_t node_id, double first_score,
                               cbm_zova_node_candidate_t *row) {
    const char *sql =
        "SELECT n.id, n.name, n.qualified_name, n.file_path, n.label, v.vector "
        "FROM node_vectors v "
        "INNER JOIN nodes n ON n.id = v.node_id "
        "WHERE n.id = ?1 "
        "  AND n.project = ?2 "
        "  AND v.project = ?2 "
        "  AND length(v.vector) = ?3 "
        "LIMIT 1";

    zova_statement *stmt = NULL;
    int rc = prepare_zova(db, sql, &stmt, "vector.fetch_prepare");
    if (rc == 0) {
        zova_statement_bind_int64_request idreq = {.statement = stmt, .index = 1, .value = node_id};
        zova_statement_bind_text_request preq = {
            .statement = stmt,
            .index = 2,
            .data = (const uint8_t *)project,
            .len = strlen(project),
        };
        zova_statement_bind_int64_request dreq = {.statement = stmt, .index = 3, .value = vector_dim};
        if (status_ok(zova_statement_bind_int64(&idreq), db, "vector.fetch_bind_id") != 0 ||
            status_ok(zova_statement_bind_text(&preq), db, "vector.fetch_bind_project") != 0 ||
            status_ok(zova_statement_bind_int64(&dreq), db, "vector.fetch_bind_dim") != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        zova_status st = zova_statement_step(&sreq);
        if (st != ZOVA_OK) {
            rc = status_ok(st, db, "vector.fetch_step");
        } else if (step == ZOVA_STEP_DONE) {
            rc = -1;
        }
    }
    if (rc == 0) {
        zova_statement_column_int64_request idcol = {
            .statement = stmt,
            .index = 0,
            .out_value = &row->node_id,
        };
        zova_buffer blob = {0};
        zova_statement_column_blob_request blob_req = {
            .statement = stmt,
            .index = 5,
            .out_buffer = &blob,
        };
        if (status_ok(zova_statement_column_int64(&idcol), db, "vector.fetch_node_id") != 0 ||
            status_ok(zova_statement_column_blob(&blob_req), db, "vector.fetch_blob") != 0) {
            zova_buffer_free(&blob);
            rc = -1;
        } else {
            row->name = column_text_owned(db, stmt, 1);
            row->qualified_name = column_text_owned(db, stmt, 2);
            row->file_path = column_text_owned(db, stmt, 3);
            row->label = column_text_owned(db, stmt, 4);
            row->first_score = first_score;
            if (!row->name || !row->qualified_name || !row->file_path || !row->label ||
                blob.len > (size_t)INT32_MAX) {
                rc = -1;
            } else {
                row->vector = (int8_t *)malloc(blob.len);
                if (!row->vector) {
                    rc = -1;
                } else {
                    memcpy(row->vector, blob.data, blob.len);
                    row->vector_len = (int)blob.len;
                }
            }
            zova_buffer_free(&blob);
        }
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    return rc;
}

int cbm_zova_vector_prefetch_nodes(const char *zova_path, const char *project,
                                   const int8_t *query, int vector_dim, int limit,
                                   cbm_zova_node_candidate_t **out, int *out_count) {
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!zova_path || !project || !query || vector_dim <= 0 || limit <= 0 || !out || !out_count) {
        return -1;
    }
    if (!i8_nonzero((const uint8_t *)query, (size_t)vector_dim)) {
        return -1;
    }

    cbm_zova_db_t zdb;
    if (open_zova(zova_path, true, &zdb) != 0) {
        return -1;
    }

    char **candidate_ids = NULL;
    int candidate_count = 0;
    int rc = collect_prefetch_candidate_ids(zdb.db, project, vector_dim, &candidate_ids, &candidate_count);
    cbm_zova_node_candidate_t *items = NULL;
    int count = 0;
    int cap = 0;
    int cleanup_count = 0;

    zova_vector_search_results results = {0};
    if (rc == 0 && candidate_count > 0) {
        zova_vector_search_in_request req = {
            .db = zdb.db,
            .collection_name = CBM_ZOVA_NODE_COLLECTION,
            .query =
                {
                    .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .f32_values = NULL,
                    .f16_values = NULL,
                    .i8_values = query,
                    .values_len = (size_t)vector_dim,
                },
            .candidate_ids = (const char *const *)candidate_ids,
            .candidate_count = (size_t)candidate_count,
            .limit = (size_t)limit,
            .out_results = &results,
        };
        rc = status_ok(zova_vector_search_in(&req), zdb.db, "vector.search_in_i8");
    }

    if (rc == 0) {
        for (size_t i = 0; i < results.len; i++) {
            if (append_candidate(&items, &count, &cap) != 0) {
                rc = -1;
                break;
            }
            cleanup_count = count + 1;
            int64_t node_id = 0;
            if (parse_i64_slice(results.items[i].id, results.items[i].id_len, &node_id) != 0 ||
                fetch_prefetch_node(zdb.db, project, vector_dim, node_id,
                                    1.0 - results.items[i].distance, &items[count]) != 0) {
                rc = -1;
                break;
            }
            count++;
            cleanup_count = count;
        }
    }
    zova_vector_search_results_free(&results);
    string_list_free(candidate_ids, candidate_count);
    close_zova(&zdb);
    if (rc != 0) {
        cbm_zova_node_candidates_free(items, cleanup_count);
        return -1;
    }
    *out = items;
    *out_count = count;
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
