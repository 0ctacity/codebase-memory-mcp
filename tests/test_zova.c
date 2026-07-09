#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "store/store.h"
#include "zova/cbm_zova.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

#include <sqlite3.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { TZ_VEC_DIM = 768, TZ_PATH_MAX = 512 };

typedef struct {
    char db_path[TZ_PATH_MAX];
    char zova_path[TZ_PATH_MAX];
    int64_t alpha_id;
    int64_t beta_id;
    int64_t gamma_id;
    int64_t other_id;
    int8_t alpha_vec[TZ_VEC_DIM];
    int8_t beta_vec[TZ_VEC_DIM];
    int8_t zero_vec[TZ_VEC_DIM];
} zova_fixture_t;

static void fill_fixture_vectors(zova_fixture_t *fx) {
    memset(fx->alpha_vec, 0, sizeof(fx->alpha_vec));
    memset(fx->beta_vec, 0, sizeof(fx->beta_vec));
    memset(fx->zero_vec, 0, sizeof(fx->zero_vec));
    fx->alpha_vec[0] = 127;
    fx->alpha_vec[1] = 1;
    fx->beta_vec[0] = 1;
    fx->beta_vec[1] = 127;
}

static int insert_vector(sqlite3 *db, const char *table, int64_t id, const char *project,
                         const char *token, const int8_t *vec) {
    const char *sql_node =
        "INSERT INTO node_vectors(node_id, project, vector) VALUES(?1, ?2, ?3)";
    const char *sql_token =
        "INSERT INTO token_vectors(id, project, token, vector, idf) VALUES(?1, ?2, ?3, ?4, 1)";
    const char *sql = strcmp(table, "node_vectors") == 0 ? sql_node : sql_token;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, project, -1, SQLITE_STATIC);
    if (strcmp(table, "node_vectors") == 0) {
        sqlite3_bind_blob(stmt, 3, vec, TZ_VEC_DIM, SQLITE_STATIC);
    } else {
        sqlite3_bind_text(stmt, 3, token, -1, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 4, vec, TZ_VEC_DIM, SQLITE_STATIC);
    }
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int create_fixture(zova_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    fill_fixture_vectors(fx);
    snprintf(fx->db_path, sizeof(fx->db_path), "%s/cbm_zova_%d_%p.db", cbm_tmpdir(),
             (int)getpid(), (void *)fx);
    ASSERT_EQ(cbm_zova_sidecar_path(fx->db_path, fx->zova_path, sizeof(fx->zova_path)), 0);
    cbm_unlink(fx->db_path);
    cbm_unlink(fx->zova_path);

    cbm_store_t *store = cbm_store_open_path(fx->db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "proj", "/tmp/proj"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_upsert_project(store, "other", "/tmp/other"), CBM_STORE_OK);

    cbm_node_t alpha = {
        .project = "proj",
        .label = "Function",
        .name = "Alpha",
        .qualified_name = "proj.Alpha",
        .file_path = "alpha.c",
    };
    cbm_node_t beta = {
        .project = "proj",
        .label = "Function",
        .name = "Beta",
        .qualified_name = "proj.Beta",
        .file_path = "beta.c",
    };
    cbm_node_t gamma = {
        .project = "proj",
        .label = "Function",
        .name = "Gamma",
        .qualified_name = "proj.Gamma",
        .file_path = "gamma.c",
    };
    cbm_node_t other = {
        .project = "other",
        .label = "Function",
        .name = "OtherAlpha",
        .qualified_name = "other.Alpha",
        .file_path = "other.c",
    };
    fx->alpha_id = cbm_store_upsert_node(store, &alpha);
    fx->beta_id = cbm_store_upsert_node(store, &beta);
    fx->gamma_id = cbm_store_upsert_node(store, &gamma);
    fx->other_id = cbm_store_upsert_node(store, &other);
    ASSERT_GT(fx->alpha_id, 0);
    ASSERT_GT(fx->beta_id, 0);
    ASSERT_GT(fx->gamma_id, 0);
    ASSERT_GT(fx->other_id, 0);
    cbm_edge_t edge = {
        .project = "proj",
        .source_id = fx->alpha_id,
        .target_id = fx->beta_id,
        .type = "CALLS",
    };
    ASSERT_GT(cbm_store_insert_edge(store, &edge), 0);

    ASSERT_EQ(cbm_store_exec(store,
                             "CREATE TABLE node_vectors("
                             "node_id INTEGER PRIMARY KEY, project TEXT NOT NULL, "
                             "vector BLOB NOT NULL);"
                             "CREATE TABLE token_vectors("
                             "id INTEGER PRIMARY KEY, project TEXT NOT NULL, token TEXT NOT NULL, "
                             "vector BLOB NOT NULL, idf INTEGER NOT NULL);"),
              CBM_STORE_OK);

    sqlite3 *db = cbm_store_get_db(store);
    ASSERT_EQ(insert_vector(db, "node_vectors", fx->alpha_id, "proj", NULL, fx->alpha_vec), 0);
    ASSERT_EQ(insert_vector(db, "node_vectors", fx->beta_id, "proj", NULL, fx->beta_vec), 0);
    ASSERT_EQ(insert_vector(db, "node_vectors", fx->gamma_id, "proj", NULL, fx->zero_vec), 0);
    ASSERT_EQ(insert_vector(db, "node_vectors", fx->other_id, "other", NULL, fx->alpha_vec), 0);
    ASSERT_EQ(insert_vector(db, "token_vectors", 1, "proj", "alpha", fx->alpha_vec), 0);

    ASSERT_EQ(cbm_store_exec(store, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');"),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_store_exec(store,
                             "INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path) "
                             "SELECT id, name, qualified_name, label, file_path FROM nodes;"),
              CBM_STORE_OK);
    cbm_store_close(store);
    return 0;
}

static void cleanup_fixture(zova_fixture_t *fx) {
    cbm_unlink(fx->db_path);
    cbm_unlink(fx->zova_path);
    char tmp[TZ_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.zova", fx->zova_path);
    cbm_unlink(tmp);
}

static int sqlite_cosine_for(cbm_store_t *store, const int8_t *a, const int8_t *b, double *out) {
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db = cbm_store_get_db(store);
    if (sqlite3_prepare_v2(db, "SELECT cbm_cosine_i8(?1, ?2)", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_blob(stmt, 1, a, TZ_VEC_DIM, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, b, TZ_VEC_DIM, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    *out = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

static long long file_size_or_zero(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (long long)st.st_size;
}

static double elapsed_ms(clock_t start, clock_t end) {
    return ((double)(end - start) * 1000.0) / (double)CLOCKS_PER_SEC;
}

static int zova_scalar_int64(const char *path, const char *sql, int64_t *out) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    if (rc == 0) {
        *out = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int result_overlap(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc) {
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

static int ordering_diffs(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc) {
    int n = ac < bc ? ac : bc;
    int diffs = 0;
    for (int i = 0; i < n; i++) {
        if (a[i].node_id != b[i].node_id) {
            diffs++;
        }
    }
    return diffs + (ac > bc ? ac - bc : bc - ac);
}

static double score_correlation(cbm_vector_result_t *a, int ac, cbm_vector_result_t *b, int bc) {
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

TEST(zova_mode_parser) {
    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_OFF);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_CONTAINER);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_I8_VECTORS);
    cbm_setenv("CBM_ZOVA_MODE", "graph_mirror", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_GRAPH_MIRROR);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

#if !CBM_WITH_ZOVA

TEST(zova_disabled_request_fails_clearly) {
    ASSERT_FALSE(cbm_zova_build_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump("/tmp/cbm-zova-disabled.db"), -1);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

#else

TEST(zova_container_sidecar_preserves_app_sql) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);
    ASSERT_TRUE(cbm_file_exists(fx.db_path));
    ASSERT_TRUE(cbm_file_exists(fx.zova_path));

    cbm_store_t *store = cbm_store_open_path_query(fx.zova_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_count_nodes(store, "proj"), 3);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(cbm_store_get_db(store),
                                 "SELECT bm25(nodes_fts) FROM nodes_fts LIMIT 1", -1, &stmt,
                                 NULL),
              SQLITE_OK);
    int step = sqlite3_step(stmt);
    ASSERT(step == SQLITE_ROW || step == SQLITE_DONE);
    sqlite3_finalize(stmt);
    cbm_store_close(store);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_i8_vector_mirror_and_prefetch) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    cbm_zova_node_candidate_t *candidates = NULL;
    int count = 0;
    ASSERT_EQ(cbm_zova_vector_prefetch_nodes(fx.zova_path, "proj", fx.alpha_vec, TZ_VEC_DIM, 2,
                                             &candidates, &count),
              0);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(candidates[0].node_id, fx.alpha_id);
    for (int i = 0; i < count; i++) {
        ASSERT_NEQ(candidates[i].node_id, fx.other_id);
    }

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    double sqlite_score = 0.0;
    ASSERT_EQ(sqlite_cosine_for(store, fx.alpha_vec, fx.alpha_vec, &sqlite_score), 0);
    cbm_store_close(store);
    ASSERT_FLOAT_EQ(candidates[0].first_score, sqlite_score, 0.000001);
    cbm_zova_node_candidates_free(candidates, count);

    candidates = NULL;
    count = 0;
    ASSERT_EQ(cbm_zova_vector_prefetch_nodes(fx.zova_path, "proj", fx.zero_vec, TZ_VEC_DIM, 2,
                                             &candidates, &count),
              -1);
    ASSERT_NULL(candidates);
    ASSERT_EQ(count, 0);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_i8_vector_search_in_uses_candidate_ids) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    zova_database *db = NULL;
    zova_message err = {0};
    zova_database_open_options_request open_req = {
        .path = fx.zova_path,
        .flags = ZOVA_OPEN_READ_ONLY,
        .busy_timeout_ms = 5000,
        .out_db = &db,
        .out_error_message = &err,
    };
    zova_status st = zova_database_open_with_options(&open_req);
    zova_message_free(&err);
    ASSERT_EQ(st, ZOVA_OK);
    ASSERT_NOT_NULL(db);

    char beta_id[32];
    snprintf(beta_id, sizeof(beta_id), "%lld", (long long)fx.beta_id);
    const char *candidate_ids[] = {beta_id};
    zova_vector_search_results results = {0};
    zova_vector_search_in_request search_req = {
        .db = db,
        .collection_name = CBM_ZOVA_NODE_COLLECTION,
        .query =
            {
                .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                .f32_values = NULL,
                .f16_values = NULL,
                .i8_values = fx.alpha_vec,
                .values_len = TZ_VEC_DIM,
            },
        .candidate_ids = candidate_ids,
        .candidate_count = 1,
        .limit = 5,
        .out_results = &results,
    };
    ASSERT_EQ(zova_vector_search_in(&search_req), ZOVA_OK);
    ASSERT_EQ(results.len, 1);
    ASSERT_EQ(results.items[0].id_len, strlen(beta_id));
    ASSERT_EQ(memcmp(results.items[0].id, beta_id, strlen(beta_id)), 0);

    zova_vector_search_results_free(&results);
    zova_database_close(db);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_i8_vector_search_matches_current_topk) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    const char *keywords[] = {"alpha"};
    cbm_vector_result_t *results = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_vector_search(store, "proj", keywords, 1, 2, &results, &count),
              CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    ASSERT_STR_EQ(results[0].name, "Alpha");
    cbm_store_free_vector_results(results, count);
    cbm_store_close(store);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_graph_mirror_neighbors) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_mirror", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%lld", (long long)fx.alpha_id);
    int count = 0;
    ASSERT_EQ(cbm_zova_graph_neighbor_count(fx.zova_path, id_buf, &count), 0);
    ASSERT_EQ(count, 1);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_i8_vector_benchmark_smoke_report) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    clock_t ingest_start = clock();
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);
    clock_t ingest_end = clock();

    const char *keywords[] = {"alpha"};
    cbm_vector_result_t *sqlite_results = NULL;
    cbm_vector_result_t *zova_results = NULL;
    int sqlite_count = 0;
    int zova_count = 0;

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);

    cbm_setenv("CBM_ZOVA_MODE", "off", 1);
    clock_t sqlite_start = clock();
    ASSERT_EQ(cbm_store_vector_search(store, "proj", keywords, 1, 2, &sqlite_results,
                                      &sqlite_count),
              CBM_STORE_OK);
    clock_t sqlite_end = clock();

    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    clock_t zova_start = clock();
    ASSERT_EQ(cbm_store_vector_search(store, "proj", keywords, 1, 2, &zova_results, &zova_count),
              CBM_STORE_OK);
    clock_t zova_end = clock();

    int64_t node_rows = 0;
    int64_t mirrored_rows = 0;
    ASSERT_EQ(zova_scalar_int64(fx.zova_path, "SELECT count(*) FROM node_vectors", &node_rows), 0);
    ASSERT_EQ(zova_scalar_int64(fx.zova_path,
                                "SELECT count(*) FROM _zova_vectors "
                                "WHERE collection_name = 'cbm_node_vectors_i8'",
                                &mirrored_rows),
              0);

    int overlap = result_overlap(sqlite_results, sqlite_count, zova_results, zova_count);
    int diffs = ordering_diffs(sqlite_results, sqlite_count, zova_results, zova_count);
    double corr = score_correlation(sqlite_results, sqlite_count, zova_results, zova_count);
    int fallback_count = zova_count == 0 && sqlite_count > 0 ? 1 : 0;

    printf("\n{\"zova_benchmark_smoke\":{"
           "\"top_k_overlap\":%d,"
           "\"sqlite_count\":%d,"
           "\"zova_count\":%d,"
           "\"ordering_differences\":%d,"
           "\"score_correlation\":%.6f,"
           "\"ingestion_ms\":%.3f,"
           "\"sqlite_query_ms\":%.3f,"
           "\"zova_query_ms\":%.3f,"
           "\"db_size_bytes\":%lld,"
           "\"zova_size_bytes\":%lld,"
           "\"mirrored_vector_count\":%lld,"
           "\"skipped_zero_vector_count\":%lld,"
           "\"fallback_count\":%d}}\n",
           overlap, sqlite_count, zova_count, diffs, corr, elapsed_ms(ingest_start, ingest_end),
           elapsed_ms(sqlite_start, sqlite_end), elapsed_ms(zova_start, zova_end),
           file_size_or_zero(fx.db_path), file_size_or_zero(fx.zova_path),
           (long long)mirrored_rows, (long long)(node_rows - mirrored_rows), fallback_count);

    ASSERT_EQ(sqlite_count, zova_count);
    ASSERT_EQ(overlap, sqlite_count);
    ASSERT_EQ(diffs, 0);
    ASSERT_FLOAT_EQ(corr, 1.0, 0.000001);
    ASSERT_GT(mirrored_rows, 0);
    ASSERT_GT(node_rows - mirrored_rows, 0);

    cbm_store_free_vector_results(sqlite_results, sqlite_count);
    cbm_store_free_vector_results(zova_results, zova_count);
    cbm_store_close(store);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

#endif

SUITE(zova) {
    RUN_TEST(zova_mode_parser);
#if !CBM_WITH_ZOVA
    RUN_TEST(zova_disabled_request_fails_clearly);
#else
    RUN_TEST(zova_container_sidecar_preserves_app_sql);
    RUN_TEST(zova_i8_vector_mirror_and_prefetch);
    RUN_TEST(zova_i8_vector_search_in_uses_candidate_ids);
    RUN_TEST(zova_i8_vector_search_matches_current_topk);
    RUN_TEST(zova_i8_vector_benchmark_smoke_report);
    RUN_TEST(zova_graph_mirror_neighbors);
#endif
}
