#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/store/store.h"
#include "zova/cbm_zova.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

#include <math.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum {
    ZRR_MAX_QUERIES = 5,
    ZRR_WARM_RUNS = 20,
    ZRR_MAX_QUERY_METRICS = ZRR_MAX_QUERIES + 1,
    ZRR_MAX_WARM_SAMPLES = ZRR_MAX_QUERY_METRICS * ZRR_WARM_RUNS,
    ZRR_TOKEN_BUF = 256,
    ZRR_TEXT_BUF = 512,
};

typedef struct {
    char *terms[ZRR_MAX_QUERIES];
    int8_t *vectors[ZRR_MAX_QUERIES];
    int vector_lens[ZRR_MAX_QUERIES];
    int count;
} zrr_terms_t;

typedef struct {
    const char *term;
    bool multi_keyword;
    int sqlite_count;
    int zova_count;
    int pure_search_in_count;
    int pure_full_count;
    int top_k_overlap;
    int ordering_differences;
    int fallback_count;
    double overlap_ratio;
    double score_correlation;
    double sqlite_query_ms;
    double zova_query_ms;
    int sqlite_warm_sample_count;
    int zova_warm_sample_count;
    double sqlite_warm_p50_ms;
    double sqlite_warm_p95_ms;
    double zova_warm_p50_ms;
    double zova_warm_p95_ms;
    double pure_search_in_ms;
    double pure_full_search_ms;
    double hydration_ms;
    cbm_vector_search_metrics_t cbm_zova;
} zrr_query_metric_t;

typedef struct {
    char **ids;
    int count;
    double collect_ms;
} zrr_candidate_ids_t;

typedef struct {
    int count;
    double search_in_ms;
} zrr_threshold_metric_t;

static long long zrr_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (long long)st.st_size;
}

static double zrr_elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static int zrr_result_overlap(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc);
static int zrr_ordering_diffs(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc);
static double zrr_score_correlation(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b,
                                    int bc);

static double zrr_percentile(const double *values, int count, int percentile) {
    double sorted[ZRR_MAX_WARM_SAMPLES];
    if (!values || count <= 0 || count > ZRR_MAX_WARM_SAMPLES) {
        return 0.0;
    }
    for (int i = 0; i < count; i++) {
        sorted[i] = values[i];
    }
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (sorted[j] < sorted[i]) {
                double tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    int rank = (percentile * count + 99) / 100;
    return sorted[rank - 1];
}

static int zrr_compare_store_vector_search(
    cbm_store_t *store, const char *project, const char **keywords, int keyword_count,
    cbm_vector_search_metrics_t *zova_metrics, int *sqlite_count_out, int *zova_count_out,
    int *overlap_out, int *diffs_out, double *corr_out, double *sqlite_ms_out,
    double *zova_ms_out) {
    cbm_vector_result_t *sqlite_results = NULL;
    cbm_vector_result_t *zova_results = NULL;
    int sqlite_count = 0;
    int zova_count = 0;
    int rc = -1;

    cbm_setenv("CBM_ZOVA_MODE", "off", 1);
    struct timespec sqlite_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &sqlite_start);
    int sqlite_rc = cbm_store_vector_search(store, project, keywords, keyword_count, 10,
                                             &sqlite_results, &sqlite_count);
    struct timespec sqlite_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &sqlite_end);

    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    struct timespec zova_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &zova_start);
    int zova_rc = cbm_store_vector_search_ex(store, project, keywords, keyword_count, 10,
                                              &zova_results, &zova_count, zova_metrics);
    struct timespec zova_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &zova_end);

    if (sqlite_rc == CBM_STORE_OK && zova_rc == CBM_STORE_OK) {
        *sqlite_count_out = sqlite_count;
        *zova_count_out = zova_count;
        *overlap_out = zrr_result_overlap(sqlite_results, sqlite_count, zova_results, zova_count);
        *diffs_out = zrr_ordering_diffs(sqlite_results, sqlite_count, zova_results, zova_count);
        *corr_out = zrr_score_correlation(sqlite_results, sqlite_count, zova_results, zova_count);
        *sqlite_ms_out = zrr_elapsed_ms(&sqlite_start, &sqlite_end);
        *zova_ms_out = zrr_elapsed_ms(&zova_start, &zova_end);
        rc = 0;
    }

    cbm_store_free_vector_results(sqlite_results, sqlite_count);
    cbm_store_free_vector_results(zova_results, zova_count);
    return rc;
}

static int zrr_result_overlap(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc) {
    int overlap = 0;
    for (int i = 0; i < ac; i++) {
        for (int j = 0; j < bc; j++) {
            if (a[i].node_id == b[j].node_id) {
                overlap++;
                break;
            }
        }
    }
    return overlap;
}

static int zrr_ordering_diffs(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc) {
    int n = ac < bc ? ac : bc;
    int diffs = 0;
    for (int i = 0; i < n; i++) {
        if (a[i].node_id != b[i].node_id) {
            diffs++;
        }
    }
    return diffs + (ac > bc ? ac - bc : bc - ac);
}

static double zrr_score_correlation(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b,
                                    int bc) {
    int n = ac < bc ? ac : bc;
    if (n <= 1) {
        return 1.0;
    }
    double mean_a = 0.0;
    double mean_b = 0.0;
    for (int i = 0; i < n; i++) {
        mean_a += a[i].score;
        mean_b += b[i].score;
    }
    mean_a /= (double)n;
    mean_b /= (double)n;
    double num = 0.0;
    double den_a = 0.0;
    double den_b = 0.0;
    for (int i = 0; i < n; i++) {
        double da = a[i].score - mean_a;
        double db = b[i].score - mean_b;
        num += da * db;
        den_a += da * da;
        den_b += db * db;
    }
    double den = den_a * den_b;
    if (den <= 1e-12) {
        return 1.0;
    }
    return num / sqrt(den);
}

static int zrr_blob_is_zero(const void *blob, int len) {
    const int8_t *v = (const int8_t *)blob;
    for (int i = 0; i < len; i++) {
        if (v[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int zrr_load_terms(const char *db_path, const char *project, zrr_terms_t *out) {
    memset(out, 0, sizeof(*out));
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    const char *sql = "SELECT token, vector FROM token_vectors "
                      "WHERE project = ?1 AND token IS NOT NULL AND length(token) > 2 "
                      "AND token GLOB '[A-Za-z]*' "
                      "ORDER BY idf DESC, token ASC LIMIT 32";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *token = (const char *)sqlite3_column_text(stmt, 0);
        const void *blob = sqlite3_column_blob(stmt, 1);
        int blob_len = sqlite3_column_bytes(stmt, 1);
        if (token && blob && blob_len > 0 && !zrr_blob_is_zero(blob, blob_len) &&
            out->count < ZRR_MAX_QUERIES) {
            char *term = strdup(token);
            int8_t *vector = (int8_t *)malloc((size_t)blob_len);
            if (!term || !vector) {
                free(term);
                free(vector);
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return -1;
            }
            memcpy(vector, blob, (size_t)blob_len);
            out->terms[out->count] = term;
            out->vectors[out->count] = vector;
            out->vector_lens[out->count] = blob_len;
            out->count++;
        }
        if (out->count >= ZRR_MAX_QUERIES) {
            break;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out->count > 0 ? 0 : -1;
}

static void zrr_free_terms(zrr_terms_t *terms) {
    for (int i = 0; i < terms->count; i++) {
        free(terms->terms[i]);
        free(terms->vectors[i]);
    }
}

static void zrr_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '\\':
            fputs("\\\\", f);
            break;
        case '"':
            fputs("\\\"", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            if (*p < 0x20) {
                fprintf(f, "\\u%04x", *p);
            } else {
                fputc(*p, f);
            }
            break;
        }
    }
    fputc('"', f);
}

static void zrr_candidate_ids_free(zrr_candidate_ids_t *candidates) {
    if (!candidates || !candidates->ids) {
        return;
    }
    for (int i = 0; i < candidates->count; i++) {
        free(candidates->ids[i]);
    }
    free(candidates->ids);
    candidates->ids = NULL;
    candidates->count = 0;
    candidates->collect_ms = 0.0;
}

#if CBM_WITH_ZOVA

static int zrr_open_zova(const char *path, zova_database **out_db) {
    *out_db = NULL;
    zova_message err = {0};
    zova_database_open_options_request req = {
        .path = path,
        .flags = ZOVA_OPEN_READ_ONLY,
        .busy_timeout_ms = 5000,
        .out_db = out_db,
        .out_error_message = &err,
    };
    zova_status st = zova_database_open_with_options(&req);
    zova_message_free(&err);
    if (st != ZOVA_OK || !*out_db) {
        return -1;
    }
    return cbm_zova_register_sql_functions(*out_db);
}

static int zrr_prepare(zova_database *db, const char *sql, zova_statement **out) {
    zova_database_prepare_request req = {.db = db, .sql = sql, .out_statement = out};
    return zova_database_prepare(&req) == ZOVA_OK ? 0 : -1;
}

static int zrr_step_row(zova_statement *stmt) {
    zova_step_result step = ZOVA_STEP_DONE;
    zova_statement_step_request req = {.statement = stmt, .out_result = &step};
    return zova_statement_step(&req) == ZOVA_OK && step == ZOVA_STEP_ROW ? 0 : -1;
}

static int zrr_zova_i64(zova_database *db, const char *sql, int64_t *out) {
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0) {
        return -1;
    }
    int rc = zrr_step_row(stmt);
    if (rc == 0) {
        zova_statement_column_int64_request col = {.statement = stmt, .index = 0, .out_value = out};
        rc = zova_statement_column_int64(&col) == ZOVA_OK ? 0 : -1;
    }
    zova_statement_finalize(stmt);
    return rc;
}

static int zrr_zova_double(zova_database *db, const char *sql, double *out) {
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0) {
        return -1;
    }
    int rc = zrr_step_row(stmt);
    if (rc == 0) {
        zova_statement_column_double_request col = {.statement = stmt, .index = 0, .out_value = out};
        rc = zova_statement_column_double(&col) == ZOVA_OK ? 0 : -1;
    }
    zova_statement_finalize(stmt);
    return rc;
}

static int zrr_zova_text(zova_database *db, const char *sql, char *out, size_t out_len) {
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0 || out_len == 0) {
        return -1;
    }
    int rc = zrr_step_row(stmt);
    if (rc == 0) {
        zova_text text = {0};
        zova_statement_column_text_request col = {.statement = stmt, .index = 0, .out_text = &text};
        if (zova_statement_column_text(&col) != ZOVA_OK || text.len >= out_len) {
            rc = -1;
        } else {
            memcpy(out, text.data ? text.data : "", text.len);
            out[text.len] = '\0';
        }
        zova_text_free(&text);
    }
    zova_statement_finalize(stmt);
    return rc;
}

static int zrr_zova_text_arg(zova_database *db, const char *sql, const char *arg, char *out,
                             size_t out_len) {
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0 || out_len == 0) {
        return -1;
    }
    zova_statement_bind_text_request bind = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)arg,
        .len = strlen(arg),
    };
    int rc = zova_statement_bind_text(&bind) == ZOVA_OK && zrr_step_row(stmt) == 0 ? 0 : -1;
    if (rc == 0) {
        zova_text text = {0};
        zova_statement_column_text_request col = {.statement = stmt, .index = 0, .out_text = &text};
        if (zova_statement_column_text(&col) != ZOVA_OK || text.len >= out_len) {
            rc = -1;
        } else {
            memcpy(out, text.data ? text.data : "", text.len);
            out[text.len] = '\0';
        }
        zova_text_free(&text);
    }
    zova_statement_finalize(stmt);
    return rc;
}

static int zrr_zova_regex(zova_database *db, const char *sql, const char *pattern,
                          const char *text, int64_t *out) {
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0) {
        return -1;
    }
    zova_statement_bind_text_request preq = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)pattern,
        .len = strlen(pattern),
    };
    zova_statement_bind_text_request treq = {
        .statement = stmt,
        .index = 2,
        .data = (const uint8_t *)text,
        .len = strlen(text),
    };
    int rc = zova_statement_bind_text(&preq) == ZOVA_OK &&
                     zova_statement_bind_text(&treq) == ZOVA_OK && zrr_step_row(stmt) == 0
                 ? 0
                 : -1;
    if (rc == 0) {
        zova_statement_column_int64_request col = {.statement = stmt, .index = 0, .out_value = out};
        rc = zova_statement_column_int64(&col) == ZOVA_OK ? 0 : -1;
    }
    zova_statement_finalize(stmt);
    return rc;
}

static int zrr_append_candidate_id(zrr_candidate_ids_t *candidates, const char *data,
                                   size_t len) {
    char **grown =
        (char **)realloc(candidates->ids, (size_t)(candidates->count + 1) * sizeof(char *));
    if (!grown) {
        return -1;
    }
    candidates->ids = grown;
    candidates->ids[candidates->count] = (char *)malloc(len + 1);
    if (!candidates->ids[candidates->count]) {
        return -1;
    }
    memcpy(candidates->ids[candidates->count], data, len);
    candidates->ids[candidates->count][len] = '\0';
    candidates->count++;
    return 0;
}

static int zrr_collect_candidate_ids(zova_database *db, const char *project, int vector_dim,
                                     zrr_candidate_ids_t *out) {
    memset(out, 0, sizeof(*out));
    const char *sql =
        "SELECT CAST(n.id AS TEXT) "
        "FROM node_vectors v "
        "INNER JOIN nodes n ON n.id = v.node_id "
        "INNER JOIN _zova_vectors zv "
        "        ON zv.vector_id = CAST(n.id AS TEXT) "
        "INNER JOIN _zova_vector_collections zc "
        "        ON zc.collection_key = zv.collection_key "
        "       AND zc.name = 'cbm_node_vectors_i8' "
        "WHERE v.project = ?1 "
        "  AND n.project = ?1 "
        "  AND n.label IN ('Function','Method','Class') "
        "  AND length(v.vector) = ?2 "
        "ORDER BY n.id";
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0) {
        return -1;
    }
    zova_statement_bind_text_request preq = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)project,
        .len = strlen(project),
    };
    zova_statement_bind_int64_request dreq = {.statement = stmt, .index = 2, .value = vector_dim};
    int rc = zova_statement_bind_text(&preq) == ZOVA_OK &&
                     zova_statement_bind_int64(&dreq) == ZOVA_OK
                 ? 0
                 : -1;
    struct timespec start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        if (zova_statement_step(&sreq) != ZOVA_OK) {
            rc = -1;
            break;
        }
        if (step == ZOVA_STEP_DONE) {
            break;
        }
        zova_text text = {0};
        zova_statement_column_text_request col = {.statement = stmt, .index = 0, .out_text = &text};
        if (zova_statement_column_text(&col) != ZOVA_OK ||
            zrr_append_candidate_id(out, text.data ? (const char *)text.data : "",
                                    text.data ? text.len : 0) != 0) {
            zova_text_free(&text);
            rc = -1;
            break;
        }
        zova_text_free(&text);
    }
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    out->collect_ms = zrr_elapsed_ms(&start, &end);
    zova_statement_finalize(stmt);
    if (rc != 0) {
        zrr_candidate_ids_free(out);
    }
    return rc;
}

static zova_vector_values zrr_i8_values(const int8_t *vector, int vector_len) {
    zova_vector_values values = {
        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
        .f32_values = NULL,
        .f16_values = NULL,
        .i8_values = vector,
        .values_len = (size_t)vector_len,
    };
    return values;
}

static int zrr_measure_search_in(zova_database *db, const int8_t *vector, int vector_len,
                                 char **candidate_ids, int candidate_count, size_t limit,
                                 zova_vector_search_results *out_results, double *out_ms) {
    memset(out_results, 0, sizeof(*out_results));
    const char *const *ids = (const char *const *)candidate_ids;
    zova_vector_search_in_request req = {
        .db = db,
        .collection_name = CBM_ZOVA_NODE_COLLECTION,
        .query = zrr_i8_values(vector, vector_len),
        .candidate_ids = ids,
        .candidate_count = (size_t)candidate_count,
        .limit = limit,
        .out_results = out_results,
    };
    struct timespec start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    zova_status st = zova_vector_search_in(&req);
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zrr_elapsed_ms(&start, &end);
    return st == ZOVA_OK ? 0 : -1;
}

static int zrr_measure_full_search(zova_database *db, const int8_t *vector, int vector_len,
                                   size_t limit, zova_vector_search_results *out_results,
                                   double *out_ms) {
    memset(out_results, 0, sizeof(*out_results));
    zova_vector_search_request req = {
        .db = db,
        .collection_name = CBM_ZOVA_NODE_COLLECTION,
        .query = zrr_i8_values(vector, vector_len),
        .limit = limit,
        .out_results = out_results,
    };
    struct timespec start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    zova_status st = zova_vector_search(&req);
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zrr_elapsed_ms(&start, &end);
    return st == ZOVA_OK ? 0 : -1;
}

static int zrr_measure_hydration(zova_database *db, const char *project, int vector_dim,
                                 const zova_vector_search_results *results, double *out_ms,
                                 int *out_rows) {
    *out_rows = 0;
    const char *sql =
        "SELECT n.name, n.qualified_name, n.file_path, n.label, v.vector "
        "FROM node_vectors v "
        "INNER JOIN nodes n ON n.id = v.node_id "
        "WHERE CAST(n.id AS TEXT) = ?1 "
        "  AND n.project = ?2 "
        "  AND v.project = ?2 "
        "  AND length(v.vector) = ?3 "
        "LIMIT 1";
    zova_statement *stmt = NULL;
    if (zrr_prepare(db, sql, &stmt) != 0) {
        return -1;
    }
    zova_statement_bind_text_request preq = {
        .statement = stmt,
        .index = 2,
        .data = (const uint8_t *)project,
        .len = strlen(project),
    };
    zova_statement_bind_int64_request dreq = {.statement = stmt, .index = 3, .value = vector_dim};
    int rc = zova_statement_bind_text(&preq) == ZOVA_OK &&
                     zova_statement_bind_int64(&dreq) == ZOVA_OK
                 ? 0
                 : -1;
    struct timespec start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t i = 0; rc == 0 && i < results->len; i++) {
        zova_statement_bind_text_request idreq = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)results->items[i].id,
            .len = results->items[i].id_len,
        };
        if (zova_statement_bind_text(&idreq) != ZOVA_OK) {
            rc = -1;
            break;
        }
        zova_step_result step = ZOVA_STEP_DONE;
        zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
        if (zova_statement_step(&sreq) != ZOVA_OK) {
            rc = -1;
            break;
        }
        if (step == ZOVA_STEP_ROW) {
            zova_buffer blob = {0};
            zova_statement_column_blob_request blob_req = {
                .statement = stmt,
                .index = 4,
                .out_buffer = &blob,
            };
            if (zova_statement_column_blob(&blob_req) != ZOVA_OK) {
                zova_buffer_free(&blob);
                rc = -1;
                break;
            }
            zova_buffer_free(&blob);
            (*out_rows)++;
        }
        if (zova_statement_reset(stmt) != ZOVA_OK) {
            rc = -1;
        }
    }
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zrr_elapsed_ms(&start, &end);
    zova_statement_finalize(stmt);
    return rc;
}

#endif

TEST(zova_real_repo_report_and_sql_compatibility) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    const char *db_path = getenv("CBM_ZOVA_REAL_DB");
    const char *zova_path = getenv("CBM_ZOVA_REAL_ZOVA");
    const char *project = getenv("CBM_ZOVA_REAL_PROJECT");
    const char *repo_path = getenv("CBM_ZOVA_REAL_REPO");
    const char *report_path = getenv("CBM_ZOVA_REAL_REPORT");
    const char *index_ms_env = getenv("CBM_ZOVA_REAL_INDEX_MS");
    ASSERT_NOT_NULL(db_path);
    ASSERT_NOT_NULL(zova_path);
    ASSERT_NOT_NULL(project);
    ASSERT_NOT_NULL(report_path);
    ASSERT_NOT_NULL(repo_path);

    zrr_terms_t terms;
    ASSERT_EQ(zrr_load_terms(db_path, project, &terms), 0);

    cbm_store_t *store = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(store);

    zrr_query_metric_t metrics[ZRR_MAX_QUERY_METRICS];
    memset(metrics, 0, sizeof(metrics));
    ASSERT_GT(terms.count, 1);
    char multi_keyword_name[ZRR_TOKEN_BUF * 2 + 4];
    snprintf(multi_keyword_name, sizeof(multi_keyword_name), "%s + %s", terms.terms[0],
             terms.terms[1]);
    int metric_count = terms.count + 1;
    int queries_with_sqlite_results = 0;
    int total_overlap = 0;
    int total_sqlite_count = 0;
    int total_ordering_diffs = 0;
    int total_fallback = 0;
    double total_corr = 0.0;
    double total_sqlite_ms = 0.0;
    double total_zova_ms = 0.0;
    double total_pure_search_in_ms = 0.0;
    double total_pure_full_ms = 0.0;
    double total_hydration_ms = 0.0;
    double total_cbm_query_vector_build_ms = 0.0;
    double total_cbm_zova_prefetch_ms = 0.0;
    double total_cbm_rescore_ms = 0.0;
    double total_cbm_sort_trim_ms = 0.0;
    int total_hydration_rows = 0;
    double sqlite_warm_samples[ZRR_MAX_WARM_SAMPLES] = {0};
    double zova_warm_samples[ZRR_MAX_WARM_SAMPLES] = {0};
    int warm_sample_count = 0;

    for (int i = 0; i < metric_count; i++) {
        bool multi_keyword = i == terms.count;
        const char *keywords[] = {multi_keyword ? terms.terms[0] : terms.terms[i],
                                  multi_keyword ? terms.terms[1] : NULL};
        int keyword_count = multi_keyword ? 2 : 1;
        int sqlite_count = 0;
        int zova_count = 0;
        int overlap = 0;
        int diffs = 0;
        double corr = 0.0;
        ASSERT_EQ(zrr_compare_store_vector_search(store, project, keywords, keyword_count,
                                                   &metrics[i].cbm_zova, &sqlite_count, &zova_count,
                                                   &overlap, &diffs, &corr,
                                                   &metrics[i].sqlite_query_ms,
                                                   &metrics[i].zova_query_ms),
                  0);
        int fallback = zova_count == 0 && sqlite_count > 0 ? 1 : 0;

        metrics[i].term = multi_keyword ? multi_keyword_name : terms.terms[i];
        metrics[i].multi_keyword = multi_keyword;
        if (multi_keyword) {
            ASSERT_TRUE(metrics[i].cbm_zova.zova_native_multi_query);
        } else {
            ASSERT_FALSE(metrics[i].cbm_zova.zova_native_multi_query);
        }
        metrics[i].sqlite_count = sqlite_count;
        metrics[i].zova_count = zova_count;
        metrics[i].top_k_overlap = overlap;
        metrics[i].ordering_differences = diffs;
        metrics[i].fallback_count = fallback;
        metrics[i].overlap_ratio = sqlite_count > 0 ? (double)overlap / (double)sqlite_count : 1.0;
        metrics[i].score_correlation = corr;
        double sqlite_query_warm[ZRR_WARM_RUNS] = {0};
        double zova_query_warm[ZRR_WARM_RUNS] = {0};
        for (int warm_i = 0; warm_i < ZRR_WARM_RUNS; warm_i++) {
            int warm_sqlite_count = 0;
            int warm_zova_count = 0;
            int warm_overlap = 0;
            int warm_diffs = 0;
            double warm_corr = 0.0;
            ASSERT_EQ(zrr_compare_store_vector_search(store, project, keywords, keyword_count,
                                                       NULL, &warm_sqlite_count, &warm_zova_count,
                                                       &warm_overlap, &warm_diffs, &warm_corr,
                                                       &sqlite_query_warm[warm_i],
                                                       &zova_query_warm[warm_i]),
                      0);
            ASSERT_EQ(warm_sqlite_count, warm_zova_count);
            ASSERT_EQ(warm_overlap, warm_sqlite_count);
            ASSERT_EQ(warm_diffs, 0);
            ASSERT_FLOAT_EQ(warm_corr, 1.0, 0.000001);
            sqlite_warm_samples[warm_sample_count] = sqlite_query_warm[warm_i];
            zova_warm_samples[warm_sample_count] = zova_query_warm[warm_i];
            warm_sample_count++;
        }
        metrics[i].sqlite_warm_sample_count = ZRR_WARM_RUNS;
        metrics[i].zova_warm_sample_count = ZRR_WARM_RUNS;
        metrics[i].sqlite_warm_p50_ms = zrr_percentile(sqlite_query_warm, ZRR_WARM_RUNS, 50);
        metrics[i].sqlite_warm_p95_ms = zrr_percentile(sqlite_query_warm, ZRR_WARM_RUNS, 95);
        metrics[i].zova_warm_p50_ms = zrr_percentile(zova_query_warm, ZRR_WARM_RUNS, 50);
        metrics[i].zova_warm_p95_ms = zrr_percentile(zova_query_warm, ZRR_WARM_RUNS, 95);
        ASSERT_GT(metrics[i].sqlite_warm_sample_count, 0);
        ASSERT_GT(metrics[i].zova_warm_sample_count, 0);

        if (sqlite_count > 0) {
            queries_with_sqlite_results++;
        }
        total_overlap += overlap;
        total_sqlite_count += sqlite_count;
        total_ordering_diffs += diffs;
        total_fallback += fallback;
        total_corr += corr;
        total_sqlite_ms += metrics[i].sqlite_query_ms;
        total_zova_ms += metrics[i].zova_query_ms;
        total_cbm_query_vector_build_ms += metrics[i].cbm_zova.query_vector_build_ms;
        total_cbm_zova_prefetch_ms += metrics[i].cbm_zova.zova_prefetch_ms;
        total_cbm_rescore_ms += metrics[i].cbm_zova.rescore_ms;
        total_cbm_sort_trim_ms += metrics[i].cbm_zova.sort_trim_ms;

    }

    ASSERT_GT(queries_with_sqlite_results, 0);

    cbm_zova_node_candidate_t *candidates = NULL;
    int prefetch_result_count = 0;
    cbm_zova_vector_prefetch_metrics_t cbm_prefetch_metrics = {0};
    ASSERT_EQ(cbm_zova_vector_prefetch_nodes_ex(zova_path, project, terms.vectors[0],
                                                terms.vector_lens[0], 10, &candidates,
                                                &prefetch_result_count, &cbm_prefetch_metrics),
              0);
    ASSERT_GT(prefetch_result_count, 0);
    cbm_zova_node_candidates_free(candidates, prefetch_result_count);

    zova_database *zdb = NULL;
    struct timespec zova_open_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &zova_open_start);
    ASSERT_EQ(zrr_open_zova(zova_path, &zdb), 0);
    struct timespec zova_open_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &zova_open_end);
    double zova_open_ms = zrr_elapsed_ms(&zova_open_start, &zova_open_end);

    int64_t nodes = 0;
    int64_t edges = 0;
    int64_t node_vectors = 0;
    int64_t token_vectors = 0;
    int64_t mirrored_vectors = 0;
    ASSERT_EQ(zrr_zova_i64(zdb, "SELECT count(*) FROM nodes", &nodes), 0);
    ASSERT_EQ(zrr_zova_i64(zdb, "SELECT count(*) FROM edges", &edges), 0);
    ASSERT_EQ(zrr_zova_i64(zdb, "SELECT count(*) FROM node_vectors", &node_vectors), 0);
    ASSERT_EQ(zrr_zova_i64(zdb, "SELECT count(*) FROM token_vectors", &token_vectors), 0);
    ASSERT_EQ(zrr_zova_i64(zdb,
                           "SELECT count(*) FROM _zova_vectors v "
                           "JOIN _zova_vector_collections c USING(collection_key) "
                           "WHERE c.name = 'cbm_node_vectors_i8'",
                           &mirrored_vectors),
              0);
    ASSERT_GT(nodes, 0);
    ASSERT_GT(edges, 0);
    ASSERT_GT(node_vectors, 0);
    ASSERT_GT(token_vectors, 0);
    ASSERT_GT(mirrored_vectors, 0);

    zrr_candidate_ids_t prefilter_candidates;
    ASSERT_EQ(zrr_collect_candidate_ids(zdb, project, terms.vector_lens[0], &prefilter_candidates),
              0);
    ASSERT_GT(prefilter_candidates.count, 0);

    for (int i = 0; i < terms.count; i++) {
        zova_vector_search_results search_in_results = {0};
        zova_vector_search_results full_results = {0};
        double search_in_ms = 0.0;
        double full_ms = 0.0;
        double hydration_ms = 0.0;
        int hydrated_rows = 0;

        ASSERT_EQ(zrr_measure_search_in(zdb, terms.vectors[i], terms.vector_lens[i],
                                        prefilter_candidates.ids, prefilter_candidates.count, 50,
                                        &search_in_results, &search_in_ms),
                  0);
        ASSERT_GT((int)search_in_results.len, 0);
        ASSERT_EQ(zrr_measure_hydration(zdb, project, terms.vector_lens[i], &search_in_results,
                                        &hydration_ms, &hydrated_rows),
                  0);
        ASSERT_GT(hydrated_rows, 0);
        ASSERT_EQ(zrr_measure_full_search(zdb, terms.vectors[i], terms.vector_lens[i], 50,
                                          &full_results, &full_ms),
                  0);
        ASSERT_GT((int)full_results.len, 0);

        metrics[i].pure_search_in_count = (int)search_in_results.len;
        metrics[i].pure_full_count = (int)full_results.len;
        metrics[i].pure_search_in_ms = search_in_ms;
        metrics[i].pure_full_search_ms = full_ms;
        metrics[i].hydration_ms = hydration_ms;
        total_pure_search_in_ms += search_in_ms;
        total_pure_full_ms += full_ms;
        total_hydration_ms += hydration_ms;
        total_hydration_rows += hydrated_rows;

        zova_vector_search_results_free(&search_in_results);
        zova_vector_search_results_free(&full_results);
    }

    zrr_threshold_metric_t threshold_metrics[5];
    memset(threshold_metrics, 0, sizeof(threshold_metrics));
    int threshold_metric_count = 0;
    const int requested_ratios[5] = {1, 10, 25, 75, 100};
    int requested_thresholds[5];
    for (int i = 0; i < 5; i++) {
        long long count = ((long long)prefilter_candidates.count * requested_ratios[i] + 99) / 100;
        requested_thresholds[i] = count > 0 ? (int)count : 1;
    }
    for (int i = 0; i < 5; i++) {
        int count = requested_thresholds[i];
        if (count <= 0 || count > prefilter_candidates.count) {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < threshold_metric_count; j++) {
            if (threshold_metrics[j].count == count) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        zova_vector_search_results threshold_results = {0};
        double threshold_ms = 0.0;
        ASSERT_EQ(zrr_measure_search_in(zdb, terms.vectors[0], terms.vector_lens[0],
                                        prefilter_candidates.ids, count, 50, &threshold_results,
                                        &threshold_ms),
                  0);
        threshold_metrics[threshold_metric_count].count = count;
        threshold_metrics[threshold_metric_count].search_in_ms = threshold_ms;
        threshold_metric_count++;
        zova_vector_search_results_free(&threshold_results);
    }

    double cosine_score = -1.0;
    ASSERT_EQ(zrr_zova_double(zdb,
                              "SELECT cbm_cosine_i8(vector, vector) "
                              "FROM node_vectors WHERE length(vector) > 0 LIMIT 1",
                              &cosine_score),
              0);
    ASSERT(cosine_score >= 0.0);

    char first_name[ZRR_TEXT_BUF];
    char split_name[ZRR_TEXT_BUF];
    ASSERT_EQ(zrr_zova_text(zdb,
                            "SELECT name FROM nodes "
                            "WHERE name IS NOT NULL AND length(name) > 0 "
                            "AND name GLOB '[A-Za-z]*' LIMIT 1",
                            first_name, sizeof(first_name)),
              0);
    ASSERT_EQ(zrr_zova_text_arg(zdb, "SELECT cbm_camel_split(?1)", first_name, split_name,
                                sizeof(split_name)),
              0);
    ASSERT_GT((int)strlen(split_name), 0);

    int64_t regexp_match = 0;
    int64_t iregexp_match = 0;
    ASSERT_EQ(zrr_zova_regex(zdb, "SELECT regexp(?1, ?2)", ".*", first_name, &regexp_match), 0);
    ASSERT_EQ(regexp_match, 1);
    ASSERT_EQ(zrr_zova_regex(zdb, "SELECT iregexp(?1, ?2)", ".*", first_name, &iregexp_match),
              0);
    ASSERT_EQ(iregexp_match, 1);

    double bm25_score = 0.0;
    ASSERT_EQ(zrr_zova_double(zdb,
                              "SELECT bm25(nodes_fts) FROM nodes_fts "
                              "WHERE nodes_fts MATCH 'function' LIMIT 1",
                              &bm25_score),
              0);

    zova_database_close(zdb);

    FILE *report = fopen(report_path, "wb");
    ASSERT_NOT_NULL(report);
    double aggregate_overlap_ratio =
        total_sqlite_count > 0 ? (double)total_overlap / (double)total_sqlite_count : 1.0;
    double avg_corr = metric_count > 0 ? total_corr / (double)metric_count : 1.0;
    double avg_sqlite_ms = metric_count > 0 ? total_sqlite_ms / (double)metric_count : 0.0;
    double avg_zova_ms = metric_count > 0 ? total_zova_ms / (double)metric_count : 0.0;
    double avg_pure_search_in_ms =
        terms.count > 0 ? total_pure_search_in_ms / (double)terms.count : 0.0;
    double avg_pure_full_ms = terms.count > 0 ? total_pure_full_ms / (double)terms.count : 0.0;
    double avg_hydration_ms = terms.count > 0 ? total_hydration_ms / (double)terms.count : 0.0;
    double avg_hydration_rows =
        terms.count > 0 ? (double)total_hydration_rows / (double)terms.count : 0.0;
    double avg_cbm_query_vector_build_ms = metric_count > 0
                                                ? total_cbm_query_vector_build_ms / (double)metric_count
                                                : 0.0;
    double avg_cbm_zova_prefetch_ms = metric_count > 0
                                           ? total_cbm_zova_prefetch_ms / (double)metric_count
                                           : 0.0;
    double avg_cbm_rescore_ms =
        metric_count > 0 ? total_cbm_rescore_ms / (double)metric_count : 0.0;
    double avg_cbm_sort_trim_ms =
        metric_count > 0 ? total_cbm_sort_trim_ms / (double)metric_count : 0.0;
    double sqlite_warm_p50_ms = zrr_percentile(sqlite_warm_samples, warm_sample_count, 50);
    double sqlite_warm_p95_ms = zrr_percentile(sqlite_warm_samples, warm_sample_count, 95);
    double zova_warm_p50_ms = zrr_percentile(zova_warm_samples, warm_sample_count, 50);
    double zova_warm_p95_ms = zrr_percentile(zova_warm_samples, warm_sample_count, 95);
    bool exact_parity = total_fallback == 0 && total_ordering_diffs == 0 &&
                        aggregate_overlap_ratio == 1.0 && avg_corr == 1.0;
    bool warm_performance = zova_warm_p50_ms <= sqlite_warm_p50_ms &&
                            zova_warm_p95_ms <= sqlite_warm_p95_ms;
    bool promotion_passed = exact_parity && warm_performance;
    double candidate_ratio = mirrored_vectors > 0
                                 ? (double)prefilter_candidates.count / (double)mirrored_vectors
                                 : 0.0;
    double ingestion_ms = index_ms_env ? atof(index_ms_env) : 0.0;
    long long skipped_zero = (long long)node_vectors - (long long)mirrored_vectors;
    if (skipped_zero < 0) {
        skipped_zero = 0;
    }

    fputs("{\n  \"repo_path\": ", report);
    zrr_json_string(report, repo_path);
    fputs(",\n  \"project\": ", report);
    zrr_json_string(report, project);
    fputs(",\n  \"db_path\": ", report);
    zrr_json_string(report, db_path);
    fputs(",\n  \"zova_path\": ", report);
    zrr_json_string(report, zova_path);
    fprintf(report,
            ",\n  \"db_size_bytes\": %lld,\n"
            "  \"zova_size_bytes\": %lld,\n"
            "  \"ingestion_ms\": %.3f,\n"
            "  \"node_count\": %lld,\n"
            "  \"edge_count\": %lld,\n"
            "  \"node_vector_count\": %lld,\n"
            "  \"token_vector_count\": %lld,\n"
            "  \"mirrored_vector_count\": %lld,\n"
            "  \"skipped_zero_vector_count\": %lld,\n"
            "  \"prefilter_candidate_count\": %d,\n"
            "  \"prefilter_candidate_ratio\": %.9f,\n"
            "  \"zova_prefetch_result_count\": %d,\n"
            "  \"zova_open_ms\": %.3f,\n"
            "  \"diagnostic_candidate_collection_ms\": %.3f,\n"
            "  \"cbm_prefetch_strategy\": \"%s\",\n"
            "  \"cbm_prefetch\": {\n"
            "    \"used_full_search\": %s,\n"
            "    \"open_ms\": %.3f,\n"
            "    \"candidate_count_ms\": %.3f,\n"
            "    \"candidate_id_collection_ms\": %.3f,\n"
            "    \"vector_search_ms\": %.3f,\n"
            "    \"hydration_ms\": %.3f\n"
            "  },\n"
            "  \"zova_candidate_strategy\": {\n"
            "    \"benchmark_candidate_ratios\": [1, 10, 25, 75, 100]\n"
            "  },\n"
            "  \"sql_compat\": {\n"
            "    \"cbm_cosine_i8_score\": %.9f,\n"
            "    \"camel_split_sample\": ",
            zrr_file_size(db_path), zrr_file_size(zova_path), ingestion_ms, (long long)nodes,
            (long long)edges, (long long)node_vectors, (long long)token_vectors,
            (long long)mirrored_vectors, skipped_zero, prefilter_candidates.count, candidate_ratio,
            prefetch_result_count, zova_open_ms, prefilter_candidates.collect_ms,
            cbm_zova_should_use_full_vector_search(prefilter_candidates.count,
                                                   (int)mirrored_vectors)
                ? "full_search"
                : "search_in",
            cbm_prefetch_metrics.used_full_search ? "true" : "false",
            cbm_prefetch_metrics.open_ms, cbm_prefetch_metrics.candidate_count_ms,
            cbm_prefetch_metrics.candidate_id_collection_ms, cbm_prefetch_metrics.vector_search_ms,
            cbm_prefetch_metrics.hydration_ms,
            cosine_score);
    zrr_json_string(report, split_name);
    fprintf(report,
            ",\n    \"regexp_match\": %lld,\n"
            "    \"iregexp_match\": %lld,\n"
            "    \"fts_bm25_score\": %.9f\n"
            "  },\n"
            "  \"aggregate\": {\n"
            "    \"query_count\": %d,\n"
            "    \"top_k_overlap\": %d,\n"
            "    \"overlap_ratio\": %.9f,\n"
            "    \"ordering_differences\": %d,\n"
            "    \"score_correlation\": %.9f,\n"
            "    \"sqlite_query_ms_avg\": %.3f,\n"
            "    \"zova_query_ms_avg\": %.3f,\n"
            "    \"pure_zova_search_in_ms_avg\": %.3f,\n"
            "    \"pure_zova_full_search_ms_avg\": %.3f,\n"
            "    \"hydration_ms_avg\": %.3f,\n"
            "    \"hydration_rows_avg\": %.3f,\n"
            "    \"cbm_query_vector_build_ms_avg\": %.3f,\n"
            "    \"cbm_zova_prefetch_ms_avg\": %.3f,\n"
            "    \"cbm_rescore_ms_avg\": %.3f,\n"
            "    \"cbm_sort_trim_ms_avg\": %.3f,\n"
            "    \"sqlite_warm_p50_ms\": %.3f,\n"
            "    \"sqlite_warm_p95_ms\": %.3f,\n"
            "    \"zova_warm_p50_ms\": %.3f,\n"
            "    \"zova_warm_p95_ms\": %.3f,\n"
            "    \"fallback_count\": %d\n"
            "  },\n"
            "  \"promotion_gate\": {\n"
            "    \"passed\": %s,\n"
            "    \"reasons\": {\"exact_parity\": %s, \"warm_p50_not_slower\": %s, "
            "\"warm_p95_not_slower\": %s}\n"
            "  },\n"
            "  \"candidate_thresholds\": [\n",
            (long long)regexp_match, (long long)iregexp_match, bm25_score, metric_count,
            total_overlap, aggregate_overlap_ratio, total_ordering_diffs, avg_corr, avg_sqlite_ms,
            avg_zova_ms, avg_pure_search_in_ms, avg_pure_full_ms, avg_hydration_ms,
            avg_hydration_rows, avg_cbm_query_vector_build_ms, avg_cbm_zova_prefetch_ms,
            avg_cbm_rescore_ms, avg_cbm_sort_trim_ms, sqlite_warm_p50_ms, sqlite_warm_p95_ms,
            zova_warm_p50_ms, zova_warm_p95_ms, total_fallback,
            promotion_passed ? "true" : "false", exact_parity ? "true" : "false",
            zova_warm_p50_ms <= sqlite_warm_p50_ms ? "true" : "false",
            zova_warm_p95_ms <= sqlite_warm_p95_ms ? "true" : "false");
    for (int i = 0; i < threshold_metric_count; i++) {
        fprintf(report,
                "    {\"candidate_count\": %d, \"candidate_ratio\": %.9f, "
                "\"search_in_ms\": %.3f}%s\n",
                threshold_metrics[i].count,
                mirrored_vectors > 0 ? (double)threshold_metrics[i].count / (double)mirrored_vectors
                                     : 0.0,
                threshold_metrics[i].search_in_ms, i + 1 == threshold_metric_count ? "" : ",");
    }
    fputs("  ],\n", report);
    fputs("  \"queries\": [\n", report);
    for (int i = 0; i < metric_count; i++) {
        fputs("    {\"term\": ", report);
        zrr_json_string(report, metrics[i].term);
        fprintf(report,
                ", \"multi_keyword\": %s, \"sqlite_count\": %d, \"zova_count\": %d, "
                "\"pure_search_in_count\": %d, \"pure_full_count\": %d, "
                "\"top_k_overlap\": %d, \"overlap_ratio\": %.9f, "
                "\"ordering_differences\": %d, \"score_correlation\": %.9f, "
                "\"sqlite_query_ms\": %.3f, \"zova_query_ms\": %.3f, "
                "\"sqlite_warm_p50_ms\": %.3f, \"sqlite_warm_p95_ms\": %.3f, "
                "\"zova_warm_p50_ms\": %.3f, \"zova_warm_p95_ms\": %.3f, "
                "\"pure_zova_search_in_ms\": %.3f, "
                "\"pure_zova_full_search_ms\": %.3f, "
                "\"hydration_ms\": %.3f, "
                "\"cbm_zova_used\": %s, \"cbm_zova_native_multi_query\": %s, "
                "\"cbm_query_vector_build_ms\": %.3f, "
                "\"cbm_zova_prefetch_ms\": %.3f, \"cbm_rescore_ms\": %.3f, "
                "\"cbm_sort_trim_ms\": %.3f, "
                "\"fallback_count\": %d}%s\n",
                metrics[i].multi_keyword ? "true" : "false", metrics[i].sqlite_count,
                metrics[i].zova_count, metrics[i].pure_search_in_count,
                metrics[i].pure_full_count, metrics[i].top_k_overlap, metrics[i].overlap_ratio,
                metrics[i].ordering_differences, metrics[i].score_correlation,
                metrics[i].sqlite_query_ms, metrics[i].zova_query_ms,
                metrics[i].sqlite_warm_p50_ms, metrics[i].sqlite_warm_p95_ms,
                metrics[i].zova_warm_p50_ms, metrics[i].zova_warm_p95_ms,
                metrics[i].pure_search_in_ms, metrics[i].pure_full_search_ms,
                metrics[i].hydration_ms, metrics[i].cbm_zova.used_zova ? "true" : "false",
                metrics[i].cbm_zova.zova_native_multi_query ? "true" : "false",
                metrics[i].cbm_zova.query_vector_build_ms, metrics[i].cbm_zova.zova_prefetch_ms,
                metrics[i].cbm_zova.rescore_ms, metrics[i].cbm_zova.sort_trim_ms,
                metrics[i].fallback_count,
                i + 1 == metric_count ? "" : ",");
    }
    fputs("  ]\n}\n", report);
    ASSERT_EQ(fclose(report), 0);

    cbm_store_close(store);
    zrr_candidate_ids_free(&prefilter_candidates);
    zrr_free_terms(&terms);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_real_repo_percentile_uses_nearest_rank) {
    double values[20];
    for (int i = 0; i < 20; i++) {
        values[i] = (double)(i + 1);
    }
    ASSERT_FLOAT_EQ(zrr_percentile(values, 20, 50), 10.0, 0.000001);
    ASSERT_FLOAT_EQ(zrr_percentile(values, 20, 95), 19.0, 0.000001);
    PASS();
}

SUITE(zova_real_repo) {
    RUN_TEST(zova_real_repo_percentile_uses_nearest_rank);
    RUN_TEST(zova_real_repo_report_and_sql_compatibility);
}
