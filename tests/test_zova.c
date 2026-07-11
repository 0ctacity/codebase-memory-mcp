#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "graph_buffer/graph_buffer.h"
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

static int build_direct_fixture_sidecar(const zova_fixture_t *fx) {
    const CBMDumpNode nodes[] = {
        {.id = fx->alpha_id, .project = "proj", .label = "Function", .name = "Alpha",
         .qualified_name = "proj.Alpha", .file_path = "alpha.c", .properties = "{}"},
        {.id = fx->beta_id, .project = "proj", .label = "Function", .name = "Beta",
         .qualified_name = "proj.Beta", .file_path = "beta.c", .properties = "{}"},
        {.id = fx->gamma_id, .project = "proj", .label = "Function", .name = "Gamma",
         .qualified_name = "proj.Gamma", .file_path = "gamma.c", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1,
         .project = "proj",
         .source_id = fx->alpha_id,
         .target_id = fx->beta_id,
         .type = "CALLS",
         .properties = "{}",
         .url_path = "",
         .local_name = ""},
    };
    const CBMDumpVector node_vectors[] = {
        {.node_id = fx->alpha_id,
         .project = "proj",
         .vector = (const uint8_t *)fx->alpha_vec,
         .vector_len = TZ_VEC_DIM},
        {.node_id = fx->beta_id,
         .project = "proj",
         .vector = (const uint8_t *)fx->beta_vec,
         .vector_len = TZ_VEC_DIM},
        {.node_id = fx->gamma_id,
         .project = "proj",
         .vector = (const uint8_t *)fx->zero_vec,
         .vector_len = TZ_VEC_DIM},
    };
    const CBMDumpTokenVec token_vectors[] = {
        {.id = 1,
         .project = "proj",
         .token = "alpha",
         .vector = (const uint8_t *)fx->alpha_vec,
         .vector_len = TZ_VEC_DIM,
         .idf = 1.0f},
    };
    return cbm_zova_after_sqlite_dump_workspace_direct(
        fx->db_path, "/tmp/proj", "proj", nodes, 3, edges, 1, node_vectors, 3, token_vectors,
        1, TZ_VEC_DIM);
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
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "off", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_OFF);
    ASSERT_FALSE(cbm_zova_graph_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_CONTAINER);
    ASSERT_FALSE(cbm_zova_graph_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_I8_VECTORS);
    cbm_setenv("CBM_ZOVA_MODE", "graph_mirror", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_GRAPH_MIRROR);
    ASSERT_FALSE(cbm_zova_graph_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_GRAPH_READ);
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
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

TEST(zova_workspace_registry_lookup_is_read_only) {
    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char lookup_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(registry_path, sizeof(registry_path), "%s/cbm_workspace_lookup_%d_%p.zova",
             cbm_tmpdir(), (int)getpid(), (void *)registry_path);
    cbm_unlink(registry_path);

    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/cbm-lookup-missing", lookup_id,
                                            sizeof(lookup_id)),
              -1);
    ASSERT_FALSE(cbm_file_exists(registry_path));

    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/cbm-lookup-present",
                                                   workspace_id, sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/cbm-lookup-present", lookup_id,
                                            sizeof(lookup_id)),
              0);
    ASSERT_STR_EQ(lookup_id, workspace_id);

    cbm_unlink(registry_path);
    PASS();
}

TEST(zova_sidecar_generation_matches_source_and_ready_workspace) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
                  fx.db_path, "/tmp/proj", "proj", NULL, 0, NULL, 0, TZ_VEC_DIM),
              0);

    int64_t source_generation = 0;
    int64_t sidecar_generation = 0;
    int64_t active_generation = 0;
    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    ASSERT_EQ(zova_scalar_int64(fx.db_path,
                                "SELECT generation FROM cbm_zova_sidecar_generation_v1 "
                                "WHERE id = 1",
                                &source_generation),
              0);
    ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &sidecar_generation), 0);
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/proj", workspace_id,
                                            sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_GT(source_generation, 0);
    ASSERT_EQ(sidecar_generation, source_generation);
    ASSERT_EQ(active_generation, source_generation);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_sidecar_failure_falls_back_until_a_ready_rebuild) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
                  fx.db_path, "/tmp/proj", "proj", NULL, 0, NULL, 0, TZ_VEC_DIM),
              0);

    int64_t ready_generation = 0;
    int64_t failed_source_generation = 0;
    int64_t active_generation = 0;
    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char tmp_path[TZ_PATH_MAX];
    ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &ready_generation), 0);
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/proj", workspace_id,
                                            sizeof(workspace_id)),
              0);

    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "after_vectors", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
                  fx.db_path, "/tmp/proj", "proj", NULL, 0, NULL, 0, TZ_VEC_DIM),
              -1);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    ASSERT_TRUE(cbm_file_exists(fx.zova_path));
    ASSERT_TRUE(snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.zova", fx.zova_path) <
                (int)sizeof(tmp_path));
    ASSERT_FALSE(cbm_file_exists(tmp_path));
    ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &active_generation), 0);
    ASSERT_EQ(active_generation, ready_generation);
    ASSERT_EQ(zova_scalar_int64(fx.db_path,
                                "SELECT generation FROM cbm_zova_sidecar_generation_v1 "
                                "WHERE id = 1",
                                &failed_source_generation),
              0);
    ASSERT_GT(failed_source_generation, ready_generation);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, ready_generation);

    cbm_store_t *store = cbm_store_open_path_query(fx.db_path);
    ASSERT_NOT_NULL(store);
    cbm_node_t alpha = {0};
    ASSERT_EQ(cbm_store_find_node_by_id(store, fx.alpha_id, &alpha), CBM_STORE_OK);
    cbm_zova_graph_visit_t *visits = NULL;
    int visit_count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_zova_walk_calls(store, "proj", &alpha, "outbound", 2, 20, &visits,
                                        &visit_count, &metrics),
              CBM_STORE_NOT_FOUND);
    ASSERT_TRUE(metrics.fallback);
    cbm_zova_graph_visits_free(visits, visit_count);

    ASSERT_EQ(cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
                  fx.db_path, "/tmp/proj", "proj", NULL, 0, NULL, 0, TZ_VEC_DIM),
              0);
    /* The query handle intentionally observed the failed source generation.
     * A new store represents a client opening the now-current SQLite file. */
    cbm_node_free_fields(&alpha);
    cbm_store_close(store);
    store = cbm_store_open_path_query(fx.db_path);
    ASSERT_NOT_NULL(store);
    alpha = (cbm_node_t){0};
    ASSERT_EQ(cbm_store_find_node_by_id(store, fx.alpha_id, &alpha), CBM_STORE_OK);
    visits = NULL;
    visit_count = 0;
    ASSERT_EQ(cbm_store_zova_walk_calls(store, "proj", &alpha, "outbound", 2, 20, &visits,
                                        &visit_count, &metrics),
              CBM_STORE_OK);
    ASSERT_FALSE(metrics.fallback);
    cbm_zova_graph_visits_free(visits, visit_count);
    cbm_node_free_fields(&alpha);
    cbm_store_close(store);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_direct_sidecar_fault_phases_preserve_ready_read_boundary) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    static const char *fault_phases[] = {
        "after_vectors", "after_graph_nodes", "after_graph_edges", "after_publish",
    };
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(build_direct_fixture_sidecar(&fx), 0);

    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char tmp_path[TZ_PATH_MAX];
    int64_t ready_generation = 0;
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/proj", workspace_id,
                                            sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &ready_generation), 0);
    ASSERT_TRUE(snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.zova", fx.zova_path) <
                (int)sizeof(tmp_path));

    for (size_t i = 0; i < sizeof(fault_phases) / sizeof(fault_phases[0]); i++) {
        int64_t source_generation = 0;
        int64_t published_generation = 0;
        int64_t active_generation = 0;
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", fault_phases[i], 1);
        ASSERT_EQ(build_direct_fixture_sidecar(&fx), -1);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        ASSERT_TRUE(cbm_file_exists(fx.zova_path));
        ASSERT_FALSE(cbm_file_exists(tmp_path));
        ASSERT_EQ(zova_scalar_int64(fx.db_path,
                                    "SELECT generation FROM cbm_zova_sidecar_generation_v1 "
                                    "WHERE id = 1",
                                    &source_generation),
                  0);
        ASSERT_GT(source_generation, ready_generation);
        ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &published_generation), 0);
        ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                           &active_generation),
                  0);
        ASSERT_EQ(active_generation, ready_generation);
        if (strcmp(fault_phases[i], "after_publish") == 0) {
            ASSERT_EQ(published_generation, source_generation);
        } else {
            ASSERT_EQ(published_generation, ready_generation);
        }

        cbm_store_t *store = cbm_store_open_path_query(fx.db_path);
        ASSERT_NOT_NULL(store);
        cbm_node_t alpha = {0};
        ASSERT_EQ(cbm_store_find_node_by_id(store, fx.alpha_id, &alpha), CBM_STORE_OK);
        cbm_zova_graph_visit_t *visits = NULL;
        int visit_count = 0;
        cbm_zova_graph_metrics_t graph_metrics = {0};
        ASSERT_EQ(cbm_store_zova_walk_calls(store, "proj", &alpha, "outbound", 2, 20, &visits,
                                            &visit_count, &graph_metrics),
                  CBM_STORE_NOT_FOUND);
        ASSERT_TRUE(graph_metrics.fallback);
        cbm_zova_graph_visits_free(visits, visit_count);

        const char *keywords[] = {"alpha"};
        cbm_vector_result_t *results = NULL;
        int result_count = 0;
        cbm_vector_search_metrics_t vector_metrics = {0};
        ASSERT_EQ(cbm_store_vector_search_ex(store, "proj", keywords, 1, 2, &results,
                                             &result_count, &vector_metrics),
                  CBM_STORE_OK);
        ASSERT_FALSE(vector_metrics.used_zova);
        cbm_store_free_vector_results(results, result_count);
        cbm_node_free_fields(&alpha);
        cbm_store_close(store);

        ASSERT_EQ(build_direct_fixture_sidecar(&fx), 0);
        ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &ready_generation), 0);
        ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                           &active_generation),
                  0);
        ASSERT_EQ(active_generation, ready_generation);
    }

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
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

    int64_t projection_count = 0;
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT count(*) FROM cbm_zova_trace_nodes_v1 "
                  "WHERE workspace_id = 'w1_0123456789abcdef0123456789abcdef'",
                  &projection_count),
              0);
    ASSERT_EQ(projection_count, 3);
    char projection_sql[512];
    ASSERT_LT(snprintf(projection_sql, sizeof(projection_sql),
                       "SELECT count(*) FROM cbm_zova_trace_nodes_v1 "
                       "WHERE workspace_id = 'w1_0123456789abcdef0123456789abcdef' "
                       "AND node_id = '%s'",
                       alpha_node_id),
              (int)sizeof(projection_sql));
    ASSERT_EQ(zova_scalar_int64(fx.zova_path, projection_sql, &projection_count), 0);
    ASSERT_EQ(projection_count, 1);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT count(*) FROM pragma_table_info('cbm_zova_trace_nodes_v1') "
                  "WHERE name = 'properties'",
                  &projection_count),
              0);
    ASSERT_EQ(projection_count, 0);

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

TEST(zova_graph_session_hydrates_scoped_directional_walk) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
    ASSERT_EQ(cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
                  fx.db_path, "/tmp/proj", "proj", NULL, 0, NULL, 0, TZ_VEC_DIM),
              0);

    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char alpha_node_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    char beta_node_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/proj", workspace_id,
                                            sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "alpha.c", "proj.Alpha",
                                             "named", alpha_node_id, sizeof(alpha_node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "beta.c", "proj.Beta",
                                             "named", beta_node_id, sizeof(beta_node_id)),
              0);

    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(fx.zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_graph_visit_t *visits = NULL;
    int count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    cbm_setenv("CBM_ZOVA_GRAPH_PROFILE", "1", 1);
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_id, graph_name, alpha_node_id,
                                                 "outbound", 2, 20, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, beta_node_id);
    ASSERT_STR_EQ(visits[0].name, "Beta");
    ASSERT_EQ(visits[0].hop, 1);
    ASSERT_FALSE(metrics.fallback);
    ASSERT_TRUE(metrics.native_profiled);
    ASSERT_GT(metrics.native_result_count, 0);
    ASSERT_GT(metrics.frontier_expansions, 0);
    cbm_unsetenv("CBM_ZOVA_GRAPH_PROFILE");
    cbm_zova_graph_visits_free(visits, count);

    visits = NULL;
    count = 0;
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_id, graph_name, beta_node_id,
                                                 "inbound", 2, 20, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, alpha_node_id);
    ASSERT_EQ(visits[0].hop, 1);
    cbm_zova_graph_visits_free(visits, count);
    cbm_zova_graph_session_close(session);

    cbm_store_t *store = cbm_store_open_path_query(fx.db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_OFF);
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/proj", workspace_id,
                                            sizeof(workspace_id)),
              0);
    ASSERT_TRUE(cbm_file_exists(fx.zova_path));
    cbm_node_t alpha = {0};
    ASSERT_EQ(cbm_store_find_node_by_id(store, fx.alpha_id, &alpha), CBM_STORE_OK);
    /* Unset mode is the graph-read default. Keep the parser at OFF so this
     * promotion does not also make the vector experiment the default. */
    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_OFF);
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
    visits = NULL;
    count = 0;
    ASSERT_EQ(cbm_store_zova_walk_calls(store, "proj", &alpha, "outbound", 2, 20, &visits,
                                        &count, &metrics),
              CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, beta_node_id);
    cbm_zova_graph_visits_free(visits, count);
    cbm_node_free_fields(&alpha);
    cbm_store_close(store);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_direct_workspace_graph_uses_finalized_dump_arrays) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char root_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    char leaf_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/direct-proj", workspace_id,
                                                    sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "direct/root.c",
                                             "direct.Root", "named", root_id, sizeof(root_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "direct/leaf.c",
                                             "direct.Leaf", "named", leaf_id, sizeof(leaf_id)),
              0);

    const CBMDumpNode nodes[] = {
        {.id = 1,
         .project = "direct-proj",
         .label = "Function",
         .name = "DirectRoot",
         .qualified_name = "direct.Root",
         .file_path = "direct/root.c",
         .start_line = 1,
         .end_line = 2,
         .properties = "{}"},
        {.id = 2,
         .project = "direct-proj",
         .label = "Function",
         .name = "DirectLeaf",
         .qualified_name = "direct.Leaf",
         .file_path = "direct/leaf.c",
         .start_line = 3,
         .end_line = 4,
         .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1,
         .project = "direct-proj",
         .source_id = 1,
         .target_id = 2,
         .type = "CALLS",
         .properties = "{}",
         .url_path = "",
         .local_name = ""},
    };
    ASSERT_EQ(cbm_zova_write_workspace_graph_direct(fx.zova_path, workspace_id, "direct-proj",
                                                     nodes, 2, edges, 1),
              0);

    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(fx.zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_graph_visit_t *visits = NULL;
    int count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_id, graph_name, root_id,
                                                 "outbound", 2, 10, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, leaf_id);
    ASSERT_STR_EQ(visits[0].name, "DirectLeaf");
    cbm_zova_graph_visits_free(visits, count);
    cbm_zova_graph_session_close(session);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_workspace_graph_ingestion_benchmark_compares_equivalent_topology) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    const char *workspace_id = "w1_0123456789abcdef0123456789abcdef";
    char direct_path[TZ_PATH_MAX];
    char mirror_path[TZ_PATH_MAX];
    ASSERT_LT(snprintf(direct_path, sizeof(direct_path), "%s.direct-benchmark.zova", fx.db_path),
              (int)sizeof(direct_path));
    ASSERT_LT(snprintf(mirror_path, sizeof(mirror_path), "%s.mirror-benchmark.zova", fx.db_path),
              (int)sizeof(mirror_path));
    cbm_unlink(direct_path);
    cbm_unlink(mirror_path);

    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "proj", .label = "Function", .name = "Alpha",
         .qualified_name = "proj.Alpha", .file_path = "alpha.c", .properties = "{}"},
        {.id = 2, .project = "proj", .label = "Function", .name = "Beta",
         .qualified_name = "proj.Beta", .file_path = "beta.c", .properties = "{}"},
        {.id = 3, .project = "proj", .label = "Function", .name = "Gamma",
         .qualified_name = "proj.Gamma", .file_path = "gamma.c", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "proj", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    cbm_zova_graph_ingestion_metrics_t metrics = {0};
    ASSERT_EQ(cbm_zova_benchmark_workspace_graph_ingestion(
                  fx.db_path, direct_path, mirror_path, workspace_id, "proj", nodes, 3, edges,
                  1, &metrics),
              0);
    ASSERT_EQ(metrics.direct_node_count, 3);
    ASSERT_EQ(metrics.mirror_node_count, 3);
    ASSERT_EQ(metrics.direct_edge_count, 1);
    ASSERT_EQ(metrics.mirror_edge_count, 1);
    ASSERT(metrics.direct_graph_write_ms >= 0.0);
    ASSERT(metrics.sqlite_row_mirror_ms >= 0.0);

    cbm_unlink(direct_path);
    cbm_unlink(mirror_path);
    cleanup_fixture(&fx);
    PASS();
#endif
}

TEST(zova_direct_workspace_lifecycle_isolates_replace_rollback_and_delete) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    const char *workspace_a = "wa_0123456789abcdef0123456789abcdef";
    const char *workspace_b = "wb_0123456789abcdef0123456789abcdef";
    char graph_a[128] = {0};
    char graph_b[128] = {0};
    char a_root[128] = {0};
    char a_replacement_root[128] = {0};
    char b_root[128] = {0};
    char b_leaf[128] = {0};
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_a, graph_a, sizeof(graph_a)), 0);
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_b, graph_b, sizeof(graph_b)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_a, "Function", "a/root.c", "a.Root",
                                             "named", a_root, sizeof(a_root)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_a, "Function", "a/new-root.c",
                                             "a.NewRoot", "named", a_replacement_root,
                                             sizeof(a_replacement_root)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_b, "Function", "b/root.c", "b.Root",
                                             "named", b_root, sizeof(b_root)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_b, "Function", "b/leaf.c", "b.Leaf",
                                             "named", b_leaf, sizeof(b_leaf)),
              0);

    const CBMDumpNode a_v1_nodes[] = {
        {.id = 1, .project = "a", .label = "Function", .name = "ARoot",
         .qualified_name = "a.Root", .file_path = "a/root.c", .properties = "{}"},
        {.id = 2, .project = "a", .label = "Function", .name = "ALeaf",
         .qualified_name = "a.Leaf", .file_path = "a/leaf.c", .properties = "{}"},
    };
    const CBMDumpEdge a_edges[] = {
        {.id = 1, .project = "a", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    const CBMDumpNode b_nodes[] = {
        {.id = 1, .project = "b", .label = "Function", .name = "BRoot",
         .qualified_name = "b.Root", .file_path = "b/root.c", .properties = "{}"},
        {.id = 2, .project = "b", .label = "Function", .name = "BLeaf",
         .qualified_name = "b.Leaf", .file_path = "b/leaf.c", .properties = "{}"},
    };
    const CBMDumpEdge b_edges[] = {
        {.id = 1, .project = "b", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    ASSERT_EQ(cbm_zova_write_workspace_graph_direct(fx.zova_path, workspace_a, "a", a_v1_nodes,
                                                     2, a_edges, 1),
              0);
    ASSERT_EQ(cbm_zova_write_workspace_graph_direct(fx.zova_path, workspace_b, "b", b_nodes, 2,
                                                     b_edges, 1),
              0);

    const CBMDumpNode a_v2_nodes[] = {
        {.id = 1, .project = "a", .label = "Function", .name = "ANewRoot",
         .qualified_name = "a.NewRoot", .file_path = "a/new-root.c", .properties = "{}"},
        {.id = 2, .project = "a", .label = "Function", .name = "ANewLeaf",
         .qualified_name = "a.NewLeaf", .file_path = "a/new-leaf.c", .properties = "{}"},
    };
    const CBMDumpEdge invalid_a_edges[] = {
        {.id = 1, .project = "a", .source_id = 1, .target_id = 999, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    ASSERT_EQ(cbm_zova_write_workspace_graph_direct(fx.zova_path, workspace_a, "a", a_v2_nodes,
                                                     2, a_edges, 1),
              0);
    ASSERT_EQ(cbm_zova_write_workspace_graph_direct(fx.zova_path, workspace_a, "a", a_v2_nodes,
                                                     2, invalid_a_edges, 1),
              -1);

    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(fx.zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_graph_visit_t *visits = NULL;
    int count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_a, graph_a, a_root, "outbound",
                                                 2, 10, &visits, &count, &metrics),
              -1);
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_a, graph_a, a_replacement_root,
                                                 "outbound", 2, 10, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    cbm_zova_graph_visits_free(visits, count);
    visits = NULL;
    count = 0;
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_b, graph_b, b_root, "outbound",
                                                 2, 10, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, b_leaf);
    cbm_zova_graph_visits_free(visits, count);

    ASSERT_EQ(cbm_zova_delete_workspace_graph(fx.zova_path, workspace_a), 0);
    visits = NULL;
    count = 0;
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_b, graph_b, b_root, "outbound",
                                                 2, 10, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    cbm_zova_graph_visits_free(visits, count);
    cbm_zova_graph_session_close(session);

    int64_t a_projection_count = -1;
    int64_t b_projection_count = -1;
    ASSERT_EQ(zova_scalar_int64(fx.zova_path,
                                "SELECT count(*) FROM cbm_zova_trace_nodes_v1 "
                                "WHERE workspace_id = 'wa_0123456789abcdef0123456789abcdef'",
                                &a_projection_count),
              0);
    ASSERT_EQ(zova_scalar_int64(fx.zova_path,
                                "SELECT count(*) FROM cbm_zova_trace_nodes_v1 "
                                "WHERE workspace_id = 'wb_0123456789abcdef0123456789abcdef'",
                                &b_projection_count),
              0);
    ASSERT_EQ(a_projection_count, 0);
    ASSERT_EQ(b_projection_count, 2);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_workspace_node_vectors_isolate_replace_rollback_and_delete) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    const char *workspace_a = "wa_0123456789abcdef0123456789abcdef";
    const char *workspace_b = "wb_0123456789abcdef0123456789abcdef";
    const char *model = "testmodel";
    char collection_a[192] = {0};
    char collection_b[192] = {0};
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(workspace_a, model, TZ_VEC_DIM,
                                                              collection_a, sizeof(collection_a)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(workspace_b, model, TZ_VEC_DIM,
                                                              collection_b, sizeof(collection_b)),
              0);

    CBMDumpVector a_vectors[] = {
        {.node_id = 1, .project = "a", .vector = (const uint8_t *)fx.alpha_vec,
         .vector_len = TZ_VEC_DIM},
    };
    CBMDumpVector b_vectors[] = {
        {.node_id = 1, .project = "b", .vector = (const uint8_t *)fx.beta_vec,
         .vector_len = TZ_VEC_DIM},
    };
    ASSERT_EQ(cbm_zova_write_workspace_node_i8_vectors_direct(
                  fx.zova_path, workspace_a, model, a_vectors, 1, TZ_VEC_DIM),
              0);
    ASSERT_EQ(cbm_zova_write_workspace_node_i8_vectors_direct(
                  fx.zova_path, workspace_b, model, b_vectors, 1, TZ_VEC_DIM),
              0);

    /* Replacing A and then rejecting an invalid A write must not alter B. */
    ASSERT_EQ(cbm_zova_write_workspace_node_i8_vectors_direct(
                  fx.zova_path, workspace_a, model, b_vectors, 1, TZ_VEC_DIM),
              0);
    CBMDumpVector invalid_a[] = {
        {.node_id = 1, .project = "a", .vector = (const uint8_t *)fx.alpha_vec,
         .vector_len = TZ_VEC_DIM - 1},
    };
    ASSERT_EQ(cbm_zova_write_workspace_node_i8_vectors_direct(
                  fx.zova_path, workspace_a, model, invalid_a, 1, TZ_VEC_DIM),
              -1);

    zova_database *db = NULL;
    zova_message err = {0};
    zova_database_open_request open_req = {
        .path = fx.zova_path,
        .out_db = &db,
        .out_error_message = &err,
    };
    ASSERT_EQ(zova_database_open(&open_req), ZOVA_OK);
    zova_message_free(&err);
    zova_vector_collection_info info = {0};
    zova_vector_collection_info_get_request info_req = {
        .db = db,
        .name = collection_b,
        .out_info = &info,
    };
    ASSERT_EQ(zova_vector_collection_info_get(&info_req), ZOVA_OK);
    ASSERT_EQ(info.vector_count, 1);
    zova_vector_collection_info_free(&info);
    info = (zova_vector_collection_info){0};
    info_req.name = collection_a;
    ASSERT_EQ(zova_vector_collection_info_get(&info_req), ZOVA_OK);
    ASSERT_EQ(info.vector_count, 1);
    zova_vector_collection_info_free(&info);

    zova_vector_search_results results = {0};
    zova_vector_search_request search_req = {
        .db = db,
        .collection_name = collection_b,
        .query = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                  .i8_values = fx.beta_vec,
                  .values_len = TZ_VEC_DIM},
        .limit = 1,
        .out_results = &results,
    };
    ASSERT_EQ(zova_vector_search(&search_req), ZOVA_OK);
    ASSERT_EQ(results.len, 1);
    ASSERT_EQ(results.items[0].id_len, 1);
    ASSERT_EQ(memcmp(results.items[0].id, "1", 1), 0);
    zova_vector_search_results_free(&results);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    ASSERT_EQ(cbm_zova_delete_workspace_node_i8_vectors(fx.zova_path, workspace_a, model,
                                                          TZ_VEC_DIM),
              0);
    db = NULL;
    err = (zova_message){0};
    ASSERT_EQ(zova_database_open(&open_req), ZOVA_OK);
    zova_message_free(&err);
    info = (zova_vector_collection_info){0};
    info_req.db = db;
    uint8_t a_exists = 1;
    uint8_t b_exists = 0;
    zova_vector_collection_exists_request a_exists_req = {
        .db = db,
        .name = collection_a,
        .out_exists = &a_exists,
    };
    zova_vector_collection_exists_request b_exists_req = {
        .db = db,
        .name = collection_b,
        .out_exists = &b_exists,
    };
    ASSERT_EQ(zova_vector_collection_exists(&a_exists_req), ZOVA_OK);
    ASSERT_EQ(zova_vector_collection_exists(&b_exists_req), ZOVA_OK);
    ASSERT_FALSE(a_exists);
    ASSERT_TRUE(b_exists);
    info_req.name = collection_b;
    ASSERT_EQ(zova_vector_collection_info_get(&info_req), ZOVA_OK);
    ASSERT_EQ(info.vector_count, 1);
    zova_vector_collection_info_free(&info);
    results = (zova_vector_search_results){0};
    search_req.db = db;
    ASSERT_EQ(zova_vector_search(&search_req), ZOVA_OK);
    ASSERT_EQ(results.len, 1);
    ASSERT_EQ(memcmp(results.items[0].id, "1", 1), 0);
    zova_vector_search_results_free(&results);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_workspace_token_vectors_isolate_replace_rollback_and_delete) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);

    const char *workspace_a = "wa_0123456789abcdef0123456789abcdef";
    const char *workspace_b = "wb_0123456789abcdef0123456789abcdef";
    const char *model = "testmodel";
    char collection_a[192] = {0};
    char collection_b[192] = {0};
    ASSERT_EQ(cbm_zova_workspace_token_vector_collection_name(workspace_a, model, TZ_VEC_DIM,
                                                               collection_a, sizeof(collection_a)),
              0);
    ASSERT_EQ(cbm_zova_workspace_token_vector_collection_name(workspace_b, model, TZ_VEC_DIM,
                                                               collection_b, sizeof(collection_b)),
              0);
    CBMDumpTokenVec a_vectors[] = {
        {.id = 1, .project = "a", .token = "alpha", .vector = (const uint8_t *)fx.alpha_vec,
         .vector_len = TZ_VEC_DIM, .idf = 1.0f},
    };
    CBMDumpTokenVec b_vectors[] = {
        {.id = 1, .project = "b", .token = "beta", .vector = (const uint8_t *)fx.beta_vec,
         .vector_len = TZ_VEC_DIM, .idf = 1.0f},
    };
    ASSERT_EQ(cbm_zova_write_workspace_token_i8_vectors_direct(
                  fx.zova_path, workspace_a, model, a_vectors, 1, TZ_VEC_DIM),
              0);
    ASSERT_EQ(cbm_zova_write_workspace_token_i8_vectors_direct(
                  fx.zova_path, workspace_b, model, b_vectors, 1, TZ_VEC_DIM),
              0);
    ASSERT_EQ(cbm_zova_write_workspace_token_i8_vectors_direct(
                  fx.zova_path, workspace_a, model, b_vectors, 1, TZ_VEC_DIM),
              0);
    CBMDumpTokenVec invalid_a[] = {
        {.id = 1, .project = "a", .token = "invalid", .vector = (const uint8_t *)fx.alpha_vec,
         .vector_len = TZ_VEC_DIM - 1, .idf = 1.0f},
    };
    ASSERT_EQ(cbm_zova_write_workspace_token_i8_vectors_direct(
                  fx.zova_path, workspace_a, model, invalid_a, 1, TZ_VEC_DIM),
              -1);

    zova_database *db = NULL;
    zova_message err = {0};
    zova_database_open_request open_req = {
        .path = fx.zova_path,
        .out_db = &db,
        .out_error_message = &err,
    };
    ASSERT_EQ(zova_database_open(&open_req), ZOVA_OK);
    zova_message_free(&err);
    zova_vector_search_results results = {0};
    zova_vector_search_request search_req = {
        .db = db,
        .collection_name = collection_b,
        .query = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                  .i8_values = fx.beta_vec,
                  .values_len = TZ_VEC_DIM},
        .limit = 1,
        .out_results = &results,
    };
    ASSERT_EQ(zova_vector_search(&search_req), ZOVA_OK);
    ASSERT_EQ(results.len, 1);
    ASSERT_EQ(memcmp(results.items[0].id, "1", 1), 0);
    zova_vector_search_results_free(&results);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    ASSERT_EQ(cbm_zova_delete_workspace_token_i8_vectors(fx.zova_path, workspace_a, model,
                                                           TZ_VEC_DIM),
              0);
    db = NULL;
    err = (zova_message){0};
    ASSERT_EQ(zova_database_open(&open_req), ZOVA_OK);
    zova_message_free(&err);
    uint8_t a_exists = 1;
    uint8_t b_exists = 0;
    zova_vector_collection_exists_request a_exists_req = {
        .db = db, .name = collection_a, .out_exists = &a_exists};
    zova_vector_collection_exists_request b_exists_req = {
        .db = db, .name = collection_b, .out_exists = &b_exists};
    ASSERT_EQ(zova_vector_collection_exists(&a_exists_req), ZOVA_OK);
    ASSERT_EQ(zova_vector_collection_exists(&b_exists_req), ZOVA_OK);
    ASSERT_FALSE(a_exists);
    ASSERT_TRUE(b_exists);
    search_req.db = db;
    ASSERT_EQ(zova_vector_search(&search_req), ZOVA_OK);
    ASSERT_EQ(results.len, 1);
    zova_vector_search_results_free(&results);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
#endif
}

TEST(zova_gbuf_sidecar_uses_retained_finalized_topology) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char db_path[TZ_PATH_MAX];
    char zova_path[TZ_PATH_MAX];
    snprintf(db_path, sizeof(db_path), "%s/cbm-zova-gbuf-direct-%d.db", cbm_tmpdir(),
             (int)getpid());
    ASSERT_EQ(cbm_zova_sidecar_path(db_path, zova_path, sizeof(zova_path)), 0);
    cbm_unlink(db_path);
    cbm_unlink(zova_path);

    const char *project = "gbuf-direct";
    const char *root_path = "/tmp/gbuf-direct";
    cbm_gbuf_t *gb = cbm_gbuf_new(project, root_path);
    ASSERT_NOT_NULL(gb);
    int64_t root = cbm_gbuf_upsert_node(gb, "Function", "Root", "direct.Root", "root.c", 1,
                                         2, "{}");
    int64_t leaf = cbm_gbuf_upsert_node(gb, "Function", "Leaf", "direct.Leaf", "leaf.c", 3,
                                         4, "{}");
    ASSERT_GT(root, 0);
    ASSERT_GT(leaf, 0);
    ASSERT_GT(cbm_gbuf_insert_edge(gb, root, leaf, "CALLS", "{}"), 0);
    ASSERT_EQ(cbm_gbuf_dump_to_sqlite(gb, db_path), 0);

    /* If finalization scanned SQLite graph rows, this deletion would yield an
     * empty Zova graph. The retained finalized dump must still yield Root→Leaf. */
    cbm_store_t *store = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_delete_edges_by_project(store, project), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_delete_nodes_by_project(store, project), CBM_STORE_OK);
    cbm_store_close(store);

    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_EQ(cbm_gbuf_finalize_zova_sidecar(gb, db_path), 0);

    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char root_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, root_path, workspace_id,
                                            sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "root.c", "direct.Root",
                                             "named", root_id, sizeof(root_id)),
              0);
    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_graph_visit_t *visits = NULL;
    int count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_id, graph_name, root_id,
                                                 "outbound", 2, 10, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].name, "Leaf");
    cbm_zova_graph_visits_free(visits, count);
    cbm_zova_graph_session_close(session);
    cbm_gbuf_free(gb);
    cbm_unlink(zova_path);
    cbm_unlink(db_path);
    PASS();
#endif
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
    RUN_TEST(zova_workspace_registry_lookup_is_read_only);
    RUN_TEST(zova_sidecar_generation_matches_source_and_ready_workspace);
    RUN_TEST(zova_sidecar_failure_falls_back_until_a_ready_rebuild);
    RUN_TEST(zova_direct_sidecar_fault_phases_preserve_ready_read_boundary);
    RUN_TEST(zova_i8_vector_search_in_uses_candidate_ids);
    RUN_TEST(zova_i8_vector_prefetch_uses_full_search_only_for_complete_candidate_sets);
    RUN_TEST(zova_i8_vector_prefetch_reports_full_search_stage_metrics);
    RUN_TEST(zova_i8_vector_session_reuses_open_database);
    RUN_TEST(zova_i8_vector_search_matches_current_topk);
    RUN_TEST(zova_i8_native_multi_query_matches_sqlite_topk);
    RUN_TEST(zova_i8_vector_benchmark_smoke_report);
    RUN_TEST(zova_graph_mirror_neighbors);
    RUN_TEST(zova_workspace_graph_mirror_is_project_scoped_and_matches_callers);
    RUN_TEST(zova_graph_session_hydrates_scoped_directional_walk);
    RUN_TEST(zova_direct_workspace_graph_uses_finalized_dump_arrays);
    RUN_TEST(zova_workspace_graph_ingestion_benchmark_compares_equivalent_topology);
    RUN_TEST(zova_direct_workspace_lifecycle_isolates_replace_rollback_and_delete);
    RUN_TEST(zova_workspace_node_vectors_isolate_replace_rollback_and_delete);
    RUN_TEST(zova_workspace_token_vectors_isolate_replace_rollback_and_delete);
    RUN_TEST(zova_gbuf_sidecar_uses_retained_finalized_topology);
#endif
}
