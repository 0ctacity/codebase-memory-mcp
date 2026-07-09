#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "../internal/cbm/sqlite_writer.h"

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

TEST(zova_workspace_scoped_names_and_node_ids_are_deterministic) {
    const char *workspace_id = "w1_0123456789abcdef0123456789abcdef";
    char graph_name[128] = {0};
    char collection_name[192] = {0};
    char node_id[128] = {0};
    char changed_node_id[128] = {0};

    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_STR_EQ(graph_name, "cbm_graph_w1_0123456789abcdef0123456789abcdef");
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  workspace_id, "nomic_embed_code_v1", TZ_VEC_DIM, collection_name,
                  sizeof(collection_name)),
              0);
    ASSERT_STR_EQ(collection_name,
                  "cbm_nodes_i8_w1_0123456789abcdef0123456789abcdef_nomic_embed_code_v1_d768");

    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "src/main.c", "main.run",
                                             "named", node_id, sizeof(node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "src/main.c", "main.run",
                                             "named", changed_node_id, sizeof(changed_node_id)),
              0);
    ASSERT_STR_EQ(node_id, changed_node_id);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "src/main.c", "main.run",
                                             "lambda:1:8", changed_node_id,
                                             sizeof(changed_node_id)),
              0);
    ASSERT_NEQ(strcmp(node_id, changed_node_id), 0);
    ASSERT_TRUE(strncmp(node_id, "n:v1:w1_0123456789abcdef0123456789abcdef:", 40) == 0);
    PASS();
}

TEST(zova_workspace_registry_path_uses_cache_dir) {
    char path[TZ_PATH_MAX] = {0};
    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-zova-workspace-cache", 1);
    ASSERT_EQ(cbm_zova_workspace_registry_path(path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cbm-zova-workspace-cache/cbm.zova");
    cbm_unsetenv("CBM_CACHE_DIR");
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
    cbm_zova_vector_prefetch_metrics_t prefetch_metrics = {0};
    ASSERT_EQ(cbm_zova_vector_prefetch_nodes_ex(fx.zova_path, "proj", fx.alpha_vec, TZ_VEC_DIM,
                                                2, &candidates, &count, &prefetch_metrics),
              0);
    ASSERT_EQ(count, 2);
    ASSERT_FALSE(prefetch_metrics.used_full_search);
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

TEST(zova_i8_direct_vectors_do_not_read_sqlite_vector_rows) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_exec(store, "DELETE FROM node_vectors;"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_exec(store, "DELETE FROM token_vectors;"), CBM_STORE_OK);
    cbm_store_close(store);

    CBMDumpVector node_vectors[] = {
        {.node_id = fx.beta_id, .project = "proj", .vector = (const uint8_t *)fx.beta_vec,
         .vector_len = TZ_VEC_DIM},
    };
    CBMDumpTokenVec token_vectors[] = {
        {.id = 1, .project = "proj", .token = "direct", .vector = (const uint8_t *)fx.alpha_vec,
         .vector_len = TZ_VEC_DIM, .idf = 1.0f},
    };
    ASSERT_EQ(cbm_zova_after_sqlite_dump_with_i8_vectors(
                  fx.db_path, node_vectors, 1, token_vectors, 1, TZ_VEC_DIM),
              0);

    cbm_zova_vector_session_t *session = cbm_zova_vector_session_open(fx.zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_node_candidate_t *candidates = NULL;
    int count = 0;
    ASSERT_EQ(cbm_zova_vector_session_prefetch_nodes_ex(
                  session, "proj", fx.beta_vec, TZ_VEC_DIM, 2, false, &candidates, &count, NULL),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(candidates[0].node_id, fx.beta_id);
    cbm_zova_node_candidates_free(candidates, count);
    cbm_zova_vector_session_close(session);

    zova_database *db = NULL;
    zova_message err = {0};
    zova_database_open_request open_req = {
        .path = fx.zova_path,
        .out_db = &db,
        .out_error_message = &err,
    };
    ASSERT_EQ(zova_database_open(&open_req), ZOVA_OK);
    zova_message_free(&err);
    zova_vector_collection_info token_info = {0};
    zova_vector_collection_info_get_request info_req = {
        .db = db,
        .name = CBM_ZOVA_TOKEN_COLLECTION,
        .out_info = &token_info,
    };
    ASSERT_EQ(zova_vector_collection_info_get(&info_req), ZOVA_OK);
    ASSERT_EQ(token_info.vector_count, 1);
    zova_vector_collection_info_free(&token_info);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_workspace_registry_preserves_ready_generation) {
    char registry_path[TZ_PATH_MAX];
    snprintf(registry_path, sizeof(registry_path), "%s/cbm_workspace_registry_%d_%p.zova",
             cbm_tmpdir(), (int)getpid(), (void *)&registry_path);
    cbm_unlink(registry_path);

    char workspace_id[96] = {0};
    char repeated_id[96] = {0};
    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/cbm-registry-root",
                                                  workspace_id, sizeof(workspace_id)),
              0);
    ASSERT_TRUE(strncmp(workspace_id, "w1_", 3) == 0);
    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/cbm-registry-root",
                                                  repeated_id, sizeof(repeated_id)),
              0);
    ASSERT_STR_EQ(workspace_id, repeated_id);

    int64_t active_generation = -1;
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, 0);

    ASSERT_EQ(cbm_zova_workspace_generation_begin_at(registry_path, workspace_id, 1), 0);
    ASSERT_EQ(cbm_zova_workspace_generation_finish_at(registry_path, workspace_id, 1, true), 0);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, 1);

    ASSERT_EQ(cbm_zova_workspace_generation_begin_at(registry_path, workspace_id, 2), 0);
    ASSERT_EQ(cbm_zova_workspace_generation_finish_at(registry_path, workspace_id, 2, false), 0);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, 1);

    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "", repeated_id,
                                                  sizeof(repeated_id)),
              -1);
    cbm_unlink(registry_path);
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

TEST(zova_i8_vector_prefetch_uses_full_search_only_for_complete_candidate_sets) {
    ASSERT_FALSE(cbm_zova_should_use_full_vector_search(0, 0));
    ASSERT_FALSE(cbm_zova_should_use_full_vector_search(0, 10));
    ASSERT_FALSE(cbm_zova_should_use_full_vector_search(9, 10));
    ASSERT_FALSE(cbm_zova_should_use_full_vector_search(75, 100));
    ASSERT_TRUE(cbm_zova_should_use_full_vector_search(10, 10));
    PASS();
}

TEST(zova_i8_vector_prefetch_reports_full_search_stage_metrics) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_exec(store, "DELETE FROM node_vectors WHERE project = 'other';"),
              CBM_STORE_OK);
    cbm_store_close(store);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    cbm_zova_node_candidate_t *candidates = NULL;
    int count = 0;
    cbm_zova_vector_prefetch_metrics_t metrics = {0};
    ASSERT_EQ(cbm_zova_vector_prefetch_nodes_ex(fx.zova_path, "proj", fx.alpha_vec, TZ_VEC_DIM, 2,
                                                 &candidates, &count, &metrics),
              0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(metrics.used_full_search);
    ASSERT_FLOAT_EQ(metrics.candidate_id_collection_ms, 0.0, 0.000001);
    ASSERT_FLOAT_EQ(metrics.candidate_count_ms, 0.0, 0.000001);
    ASSERT_TRUE(metrics.open_ms >= 0.0);
    ASSERT_TRUE(metrics.vector_search_ms >= 0.0);
    ASSERT_TRUE(metrics.hydration_ms >= 0.0);

    cbm_zova_node_candidates_free(candidates, count);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_i8_vector_session_reuses_open_database) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    cbm_zova_vector_session_t *session = cbm_zova_vector_session_open(fx.zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_node_candidate_t *first = NULL;
    cbm_zova_node_candidate_t *second = NULL;
    int first_count = 0;
    int second_count = 0;
    cbm_zova_vector_prefetch_metrics_t metrics = {0};
    ASSERT_EQ(cbm_zova_vector_session_prefetch_nodes_ex(session, "proj", fx.alpha_vec, TZ_VEC_DIM,
                                                         2, false, &first, &first_count, &metrics),
              0);
    ASSERT_EQ(first_count, 2);
    ASSERT_FLOAT_EQ(metrics.open_ms, 0.0, 0.000001);
    ASSERT_EQ(cbm_zova_vector_session_prefetch_nodes_ex(session, "proj", fx.alpha_vec, TZ_VEC_DIM,
                                                         2, false, &second, &second_count, NULL),
              0);
    ASSERT_EQ(second_count, first_count);
    ASSERT_EQ(second[0].node_id, first[0].node_id);
    ASSERT_FLOAT_EQ(second[0].first_score, first[0].first_score, 0.000001);

    cbm_zova_node_candidates_free(first, first_count);
    cbm_zova_node_candidates_free(second, second_count);
    cbm_zova_vector_session_close(session);
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
    cbm_vector_search_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_vector_search_ex(store, "proj", keywords, 1, 2, &results, &count,
                                         &metrics),
              CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    ASSERT_STR_EQ(results[0].name, "Alpha");
    ASSERT_TRUE(metrics.used_zova);
    ASSERT_TRUE(metrics.query_vector_build_ms >= 0.0);
    ASSERT_TRUE(metrics.zova_prefetch_ms >= 0.0);
    ASSERT_EQ(metrics.zova_fetch_limit, 2);
    ASSERT_TRUE(metrics.rescore_ms >= 0.0);
    ASSERT_TRUE(metrics.sort_trim_ms >= 0.0);
    cbm_store_free_vector_results(results, count);
    cbm_store_close(store);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_i8_native_multi_query_matches_sqlite_topk) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    const char *keywords[] = {"alpha", "beta"};
    cbm_vector_result_t *sqlite_results = NULL;
    cbm_vector_result_t *zova_results = NULL;
    int sqlite_count = 0;
    int zova_count = 0;

    cbm_setenv("CBM_ZOVA_MODE", "off", 1);
    ASSERT_EQ(cbm_store_vector_search(store, "proj", keywords, 2, 2, &sqlite_results,
                                      &sqlite_count),
              CBM_STORE_OK);
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    cbm_vector_search_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_vector_search_ex(store, "proj", keywords, 2, 2, &zova_results,
                                         &zova_count, &metrics),
              CBM_STORE_OK);

    ASSERT_EQ(sqlite_count, zova_count);
    ASSERT_EQ(result_overlap(sqlite_results, sqlite_count, zova_results, zova_count), sqlite_count);
    ASSERT_EQ(ordering_diffs(sqlite_results, sqlite_count, zova_results, zova_count), 0);
    ASSERT_FLOAT_EQ(score_correlation(sqlite_results, sqlite_count, zova_results, zova_count), 1.0,
                    0.000001);
    ASSERT_TRUE(metrics.zova_native_multi_query);

    cbm_store_free_vector_results(sqlite_results, sqlite_count);
    cbm_store_free_vector_results(zova_results, zova_count);
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

TEST(zova_workspace_graph_mirror_is_project_scoped_and_matches_callers) {
    zova_fixture_t fx;
    const char *workspace_id = "w1_0123456789abcdef0123456789abcdef";
    char graph_name[128] = {0};
    char alpha_node_id[128] = {0};
    char beta_node_id[128] = {0};
    char other_node_id[128] = {0};
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "alpha.c", "proj.Alpha",
                                             "named", alpha_node_id, sizeof(alpha_node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "beta.c", "proj.Beta",
                                             "named", beta_node_id, sizeof(beta_node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "other.c", "other.Alpha",
                                             "named", other_node_id, sizeof(other_node_id)),
              0);

    ASSERT_EQ(cbm_zova_mirror_workspace_graph(fx.zova_path, workspace_id, "proj"), 0);

    zova_database *db = NULL;
    zova_message err = {0};
    zova_database_open_request open_req = {
        .path = fx.zova_path,
        .out_db = &db,
        .out_error_message = &err,
    };
    ASSERT_EQ(zova_database_open(&open_req), ZOVA_OK);
    zova_message_free(&err);

    zova_graph_neighbor_results neighbors = {0};
    zova_graph_neighbors_request neighbors_req = {
        .db = db,
        .graph_name = graph_name,
        .node_id = alpha_node_id,
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = "CALLS",
        .limit = 10,
        .out_results = &neighbors,
    };
    ASSERT_EQ(zova_graph_neighbors(&neighbors_req), ZOVA_OK);
    ASSERT_EQ(neighbors.len, 1);
    ASSERT_STR_EQ(neighbors.items[0].node_id, beta_node_id);
    ASSERT_STR_EQ(neighbors.items[0].edge_type, "CALLS");
    zova_graph_neighbor_results_free(&neighbors);

    uint64_t degree = 0;
    zova_graph_degree_request degree_req = {
        .db = db,
        .graph_name = graph_name,
        .node_id = alpha_node_id,
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = "CALLS",
        .out_degree = &degree,
    };
    ASSERT_EQ(zova_graph_degree(&degree_req), ZOVA_OK);
    ASSERT_EQ(degree, 1);

    zova_graph_walk_results walk = {0};
    zova_graph_walk_request walk_req = {
        .db = db,
        .graph_name = graph_name,
        .start_node_id = alpha_node_id,
        .edge_type = "CALLS",
        .max_depth = 1,
        .limit = 2,
        .out_results = &walk,
    };
    ASSERT_EQ(zova_graph_walk(&walk_req), ZOVA_OK);
    ASSERT_EQ(walk.len, 2);
    ASSERT_STR_EQ(walk.items[0].node_id, alpha_node_id);
    ASSERT_EQ(walk.items[0].depth, 0);
    ASSERT_STR_EQ(walk.items[1].node_id, beta_node_id);
    ASSERT_EQ(walk.items[1].depth, 1);
    zova_graph_walk_results_free(&walk);

    uint8_t other_exists = 1;
    zova_graph_node_exists_request exists_req = {
        .db = db,
        .graph_name = graph_name,
        .node_id = other_node_id,
        .out_exists = &other_exists,
    };
    ASSERT_EQ(zova_graph_node_exists(&exists_req), ZOVA_OK);
    ASSERT_FALSE(other_exists);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

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
    RUN_TEST(zova_workspace_scoped_names_and_node_ids_are_deterministic);
    RUN_TEST(zova_workspace_registry_path_uses_cache_dir);
#if !CBM_WITH_ZOVA
    RUN_TEST(zova_disabled_request_fails_clearly);
#else
    RUN_TEST(zova_container_sidecar_preserves_app_sql);
    RUN_TEST(zova_i8_vector_mirror_and_prefetch);
    RUN_TEST(zova_i8_direct_vectors_do_not_read_sqlite_vector_rows);
    RUN_TEST(zova_workspace_registry_preserves_ready_generation);
    RUN_TEST(zova_i8_vector_search_in_uses_candidate_ids);
    RUN_TEST(zova_i8_vector_prefetch_uses_full_search_only_for_complete_candidate_sets);
    RUN_TEST(zova_i8_vector_prefetch_reports_full_search_stage_metrics);
    RUN_TEST(zova_i8_vector_session_reuses_open_database);
    RUN_TEST(zova_i8_vector_search_matches_current_topk);
    RUN_TEST(zova_i8_native_multi_query_matches_sqlite_topk);
    RUN_TEST(zova_i8_vector_benchmark_smoke_report);
    RUN_TEST(zova_graph_mirror_neighbors);
    RUN_TEST(zova_workspace_graph_mirror_is_project_scoped_and_matches_callers);
#endif
}
