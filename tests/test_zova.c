#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_edge_payload.h"
#include "zova/cbm_zova_publish_model.h"
#include "zova/cbm_zova_delta.h"
#include "zova/cbm_zova_route.h"
#include "zova/cbm_zova_repository.h"
#include "cypher/cypher.h"
#include "zova/cbm_zova_writer_gate.h"
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
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#endif

enum { TZ_VEC_DIM = 768, TZ_PATH_MAX = 512 };

typedef struct {
    cbm_zova_edge_payload_record_t records[4];
    size_t count;
} zova_payload_capture_t;

static int zova_capture_payload_record(
    const cbm_zova_edge_payload_record_t *record, void *context) {
    zova_payload_capture_t *capture = context;
    if (!capture || !record || capture->count >= 4) return -1;
    capture->records[capture->count++] = *record;
    return 0;
}

static int zova_payload_slice_equals(cbm_zova_edge_payload_slice_t slice,
                                     const char *expected) {
    size_t expected_len = strlen(expected);
    return slice.len == expected_len &&
           (expected_len == 0 || memcmp(slice.data, expected, expected_len) == 0);
}

TEST(zova_edge_payload_codec_is_compact_deterministic_and_strict) {
    const CBMDumpEdge default_edge = {
        .properties = "{}", .url_path = "", .local_name = "",
    };
    const CBMDumpEdge *default_edges[] = {&default_edge};
    size_t payload_size = SIZE_MAX;
    ASSERT_EQ(cbm_zova_edge_payload_encoded_size(default_edges, 1, &payload_size), 0);
    ASSERT_EQ(payload_size, 0);
    size_t payload_len = SIZE_MAX;
    ASSERT_EQ(cbm_zova_edge_payload_encode(default_edges, 1, NULL, 0, &payload_len), 0);
    ASSERT_EQ(payload_len, 0);

    zova_payload_capture_t capture = {0};
    size_t decoded_count = 0;
    ASSERT_EQ(cbm_zova_edge_payload_visit(NULL, 0, zova_capture_payload_record,
                                          &capture, &decoded_count), 0);
    ASSERT_EQ(decoded_count, 1);
    ASSERT_EQ(capture.count, 1);
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].properties, "{}"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].url_path, ""));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].local_name, ""));

    const CBMDumpEdge first = {
        .properties = "{\"slot\":1}", .url_path = "", .local_name = "alpha",
    };
    const CBMDumpEdge second = {
        .properties = "{}", .url_path = "/docs", .local_name = "beta",
    };
    const CBMDumpEdge *logical_edges[] = {&first, &second};
    ASSERT_EQ(cbm_zova_edge_payload_encoded_size(logical_edges, 2, &payload_size), 0);
    ASSERT_TRUE(payload_size > 0 && payload_size < 64);
    uint8_t payload[64] = {0};
    ASSERT_EQ(cbm_zova_edge_payload_encode(logical_edges, 2, payload, sizeof(payload),
                                           &payload_len), 0);
    ASSERT_EQ(payload_len, payload_size);
    uint8_t second_encoding[64] = {0};
    size_t second_len = 0;
    ASSERT_EQ(cbm_zova_edge_payload_encode(logical_edges, 2, second_encoding,
                                           sizeof(second_encoding), &second_len), 0);
    ASSERT_EQ(second_len, payload_len);
    ASSERT_EQ(memcmp(payload, second_encoding, payload_len), 0);

    memset(&capture, 0, sizeof(capture));
    ASSERT_EQ(cbm_zova_edge_payload_visit(payload, payload_len,
                                          zova_capture_payload_record, &capture,
                                          &decoded_count), 0);
    ASSERT_EQ(decoded_count, 2);
    ASSERT_EQ(capture.count, 2);
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].properties,
                                          "{\"slot\":1}"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].url_path, ""));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].local_name, "alpha"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[1].properties, "{}"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[1].url_path, "/docs"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[1].local_name, "beta"));

    const uint8_t invalid_flags[] = {1, 1, 0x80};
    ASSERT_TRUE(cbm_zova_edge_payload_visit(invalid_flags, sizeof(invalid_flags),
                                            NULL, NULL, NULL) != 0);
    PASS();
}

#ifndef _WIN32
TEST(zova_writer_gate_serializes_processes_and_releases_after_crash) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-writer-gate-%d.zova", cbm_tmpdir(), getpid());
    int ready[2], release[2], acquired[2];
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(pipe(release), 0);
    ASSERT_EQ(pipe(acquired), 0);

    pid_t holder = fork();
    ASSERT_TRUE(holder >= 0);
    if (holder == 0) {
        close(ready[0]); close(release[1]); close(acquired[0]); close(acquired[1]);
        cbm_zova_writer_guard_t guard = {0};
        if (cbm_zova_writer_guard_acquire(path, &guard) != 0) _exit(10);
        (void)write(ready[1], "R", 1);
        char byte;
        if (read(release[0], &byte, 1) != 1) _exit(11);
        cbm_zova_writer_guard_release(&guard);
        _exit(0);
    }
    close(ready[1]); close(release[0]);
    char byte;
    ASSERT_EQ(read(ready[0], &byte, 1), 1);

    pid_t waiter = fork();
    ASSERT_TRUE(waiter >= 0);
    if (waiter == 0) {
        close(acquired[0]); close(release[1]); close(ready[0]);
        cbm_zova_writer_guard_t guard = {0};
        if (cbm_zova_writer_guard_acquire(path, &guard) != 0) _exit(12);
        (void)write(acquired[1], "A", 1);
        cbm_zova_writer_guard_release(&guard);
        _exit(0);
    }
    close(acquired[1]);
    int flags = fcntl(acquired[0], F_GETFL, 0);
    ASSERT_TRUE(flags >= 0);
    ASSERT_EQ(fcntl(acquired[0], F_SETFL, flags | O_NONBLOCK), 0);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
    nanosleep(&delay, NULL);
    ASSERT_EQ(read(acquired[0], &byte, 1), -1);
    ASSERT_EQ(errno, EAGAIN);
    ASSERT_EQ(write(release[1], "X", 1), 1);
    ASSERT_EQ(fcntl(acquired[0], F_SETFL, flags), 0);
    ASSERT_EQ(read(acquired[0], &byte, 1), 1);
    int status = 0;
    ASSERT_EQ(waitpid(holder, &status, 0), holder);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ASSERT_EQ(waitpid(waiter, &status, 0), waiter);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    int crash_ready[2];
    ASSERT_EQ(pipe(crash_ready), 0);
    pid_t crashed = fork();
    ASSERT_TRUE(crashed >= 0);
    if (crashed == 0) {
        close(crash_ready[0]);
        cbm_zova_writer_guard_t guard = {0};
        if (cbm_zova_writer_guard_acquire(path, &guard) != 0) _exit(13);
        (void)write(crash_ready[1], "C", 1);
        for (;;) pause();
    }
    close(crash_ready[1]);
    ASSERT_EQ(read(crash_ready[0], &byte, 1), 1);
    ASSERT_EQ(kill(crashed, SIGKILL), 0);
    ASSERT_EQ(waitpid(crashed, &status, 0), crashed);
    cbm_zova_writer_guard_t after_crash = {0};
    ASSERT_EQ(cbm_zova_writer_guard_acquire(path, &after_crash), 0);
    cbm_zova_writer_guard_release(&after_crash);
    char lock_path[TZ_PATH_MAX + 32];
    snprintf(lock_path, sizeof(lock_path), "%s.writer.lock", path);
    cbm_unlink(lock_path);
    PASS();
}
#endif

TEST(zova_route_requires_flag_and_respects_off) {
    cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_EQ(cbm_zova_route_from_env(), CBM_ZOVA_ROUTE_COMPATIBILITY);
    cbm_setenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", "1", 1);
    ASSERT_EQ(cbm_zova_route_from_env(), CBM_ZOVA_ROUTE_FULL_AUTHORITY);
    cbm_setenv("CBM_ZOVA_MODE", "off", 1);
    ASSERT_EQ(cbm_zova_route_from_env(), CBM_ZOVA_ROUTE_COMPATIBILITY);
    cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_route_uses_user_cache_database) {
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-route-cache", 1);
    char path[1024];
    ASSERT_EQ(cbm_zova_user_database_path(path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cbm-route-cache/cbm.zova");
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    PASS();
}

#if CBM_WITH_ZOVA
TEST(zova_workspace_id_validation_rejects_missing_and_malformed_values) {
    ASSERT_EQ(cbm_zova_workspace_id_validate(NULL), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate(""), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate("workspace-with-dash"), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate("workspace/path"), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate("workspace_A1"), CBM_STORE_OK);
    PASS();
}

TEST(zova_repository_requires_ready_workspace_and_reads_metadata) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-repository-%d.zova", cbm_tmpdir(), getpid());
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "fixture", .label = "Function", .name = "alpha",
         .qualified_name = "fixture.alpha", .file_path = "src/alpha.c", .start_line = 2,
         .end_line = 4, .properties = "{\"visibility\":\"public\"}"},
        {.id = 2, .project = "fixture", .label = "Function", .name = "beta",
         .qualified_name = "fixture.beta", .file_path = "src/beta.c", .start_line = 5,
         .end_line = 8, .properties = "{}"},
    };
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/fixture", "fixture", 1,
                                                       nodes, 2, NULL, 0), 0);
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, "fixture");
    ASSERT_NOT_NULL(repo);
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    ASSERT_TRUE(workspace_id[0] != '\0');
    cbm_node_t node = {0};
    ASSERT_EQ(cbm_zova_repository_find_node_by_qn(repo, workspace_id, "fixture.alpha", &node),
              CBM_STORE_OK);
    ASSERT_STR_EQ(node.file_path, "src/alpha.c");
    ASSERT_STR_EQ(node.properties_json, "{\"visibility\":\"public\"}");
    cbm_node_free_fields(&node);
    cbm_search_output_t out = {0};
    cbm_search_params_t params = {.project = "fixture", .name_pattern = "alpha", .limit = 20,
                                  .min_degree = -1, .max_degree = -1};
    ASSERT_EQ(cbm_zova_repository_search(repo, workspace_id, &params, &out), CBM_STORE_OK);
    ASSERT_EQ(out.count, 1);
    cbm_store_search_free(&out);
    ASSERT_EQ(cbm_zova_repository_find_node_by_qn(repo, NULL, "fixture.alpha", &node),
              CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_repository_find_node_by_qn(repo, "workspace_B", "fixture.alpha", &node),
              CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_repository_search(repo, "workspace_B", &params, &out), CBM_STORE_ERR);
    cbm_zova_repository_close(repo);
    ASSERT_NULL(cbm_zova_repository_open(path, "missing"));
    cbm_unlink(path);
    PASS();
}
#endif

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

static int zova_generation_state(const char *path, const char *workspace_id, int64_t generation,
                                 char *out, size_t out_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (!path || !workspace_id || !out || out_size == 0 ||
        sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return -1;
    }
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT state FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT workspace_key "
        "FROM cbm_workspace_registry WHERE workspace_id=?1) AND generation=?2",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 2, generation);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (!text || strlen((const char *)text) + 1 > out_size) {
            rc = SQLITE_TOOBIG;
        } else {
            snprintf(out, out_size, "%s", (const char *)text);
            rc = SQLITE_OK;
        }
    } else if (rc == SQLITE_OK) {
        rc = SQLITE_NOTFOUND;
    }
    if (stmt)
        sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
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
    ASSERT_FALSE(cbm_zova_vector_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_CONTAINER);
    ASSERT_FALSE(cbm_zova_graph_read_is_enabled());
    ASSERT_FALSE(cbm_zova_vector_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "i8_vectors", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_I8_VECTORS);
    ASSERT_TRUE(cbm_zova_vector_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "graph_mirror", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_GRAPH_MIRROR);
    ASSERT_FALSE(cbm_zova_graph_read_is_enabled());
    ASSERT_TRUE(cbm_zova_vector_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_GRAPH_READ);
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
    ASSERT_TRUE(cbm_zova_vector_read_is_enabled());
    cbm_setenv("CBM_ZOVA_MODE", "authority", 1);
    ASSERT_EQ(cbm_zova_mode_from_env(), CBM_ZOVA_MODE_AUTHORITY);
    ASSERT_TRUE(cbm_zova_graph_read_is_enabled());
    ASSERT_TRUE(cbm_zova_vector_read_is_enabled());
    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_TRUE(cbm_zova_vector_read_is_enabled());
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
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(
                  workspace_id, "Variable", "src/main.c", "", "owner:main|local:item|span:8:8",
                  node_id, sizeof(node_id)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(
                  workspace_id, "Variable", "src/main.c", "", "owner:main|local:item|span:8:8",
                  changed_node_id, sizeof(changed_node_id)), 0);
    ASSERT_STR_EQ(node_id, changed_node_id);
    ASSERT_TRUE(strncmp(node_id, "n:v2:", 5) == 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(
                  workspace_id, "Variable", "src/main.c", "", "owner:other|local:item|span:8:8",
                  changed_node_id, sizeof(changed_node_id)), 0);
    ASSERT_NEQ(strcmp(node_id, changed_node_id), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Variable", "src/main.c", "", "",
                                             changed_node_id, sizeof(changed_node_id)), -1);
    PASS();
}

TEST(zova_workspace_registry_path_uses_cache_dir) {
    char path[TZ_PATH_MAX] = {0};
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-zova-workspace-cache", 1);
    ASSERT_EQ(cbm_zova_workspace_registry_path(path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cbm-zova-workspace-cache/cbm.zova");
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_copy);
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

TEST(zova_default_authority_writes_and_reads_direct_vectors) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_unsetenv("CBM_ZOVA_MODE");

    ASSERT_EQ(build_direct_fixture_sidecar(&fx), 0);

    cbm_zova_vector_session_t *session = cbm_zova_vector_session_open(fx.zova_path);
    ASSERT_NOT_NULL(session);
    cbm_zova_node_candidate_t *candidates = NULL;
    int count = 0;
    ASSERT_EQ(cbm_zova_vector_session_prefetch_nodes_ex(
                  session, "proj", fx.alpha_vec, TZ_VEC_DIM, 2, false, &candidates, &count, NULL),
              0);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(candidates[0].node_id, fx.alpha_id);
    cbm_zova_node_candidates_free(candidates, count);
    cbm_zova_vector_session_close(session);

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    const char *keywords[] = {"alpha"};
    cbm_vector_result_t *results = NULL;
    int result_count = 0;
    cbm_vector_search_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_vector_search_ex(store, "proj", keywords, 1, 2, &results, &result_count,
                                         &metrics),
              CBM_STORE_OK);
    ASSERT_EQ(result_count, 2);
    ASSERT_TRUE(metrics.used_zova);
    cbm_store_free_vector_results(results, result_count);
    cbm_store_close(store);
    cleanup_fixture(&fx);
    PASS();
}

TEST(zova_authority_rollback_uses_sqlite) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_unsetenv("CBM_ZOVA_MODE");
    ASSERT_EQ(build_direct_fixture_sidecar(&fx), 0);

    cbm_store_t *store = cbm_store_open_path(fx.db_path);
    ASSERT_NOT_NULL(store);
    cbm_setenv("CBM_ZOVA_MODE", "off", 1);
    const char *keywords[] = {"alpha"};
    cbm_vector_result_t *results = NULL;
    int count = 0;
    cbm_vector_search_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_vector_search_ex(store, "proj", keywords, 1, 2, &results, &count,
                                         &metrics),
              CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    ASSERT_FALSE(metrics.used_zova);
    cbm_store_free_vector_results(results, count);
    cbm_store_close(store);
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

TEST(zova_sidecar_schema_and_integrity_record_matches_generation) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump_workspace_with_i8_vectors(
                  fx.db_path, "/tmp/proj", "proj", NULL, 0, NULL, 0, TZ_VEC_DIM),
              0);

    int64_t generation = 0;
    int64_t schema_version = 0;
    int64_t metadata_projection_version = 0;
    int64_t edge_projection_version = 0;
    int64_t integrity_generation = 0;
    int64_t graph_nodes = 0;
    int64_t graph_edges = 0;
    int64_t metadata_nodes = 0;
    int64_t metadata_edges = 0;
    int64_t metadata_topology_edges = 0;
    int64_t fts_rows = 0;
    ASSERT_EQ(cbm_zova_sidecar_generation_get(fx.zova_path, &generation), 0);
    ASSERT_EQ(zova_scalar_int64(fx.zova_path,
                                "SELECT schema_version FROM cbm_zova_schema_v1 WHERE id = 1",
                                &schema_version),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT metadata_projection_version FROM cbm_zova_schema_v1 WHERE id = 1",
                  &metadata_projection_version),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT edge_metadata_projection_version FROM cbm_zova_schema_v1 WHERE id = 1",
                  &edge_projection_version),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT generation FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &integrity_generation),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT graph_nodes FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &graph_nodes),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT graph_edges FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &graph_edges),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT metadata_nodes FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &metadata_nodes),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT metadata_edges FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &metadata_edges),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT metadata_topology_edges FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &metadata_topology_edges),
              0);
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT fts_rows FROM cbm_zova_generation_integrity_v1 LIMIT 1",
                  &fts_rows),
              0);

    ASSERT_EQ(schema_version, 1);
    ASSERT_EQ(metadata_projection_version, 1);
    ASSERT_EQ(edge_projection_version, 1);
    ASSERT_EQ(integrity_generation, generation);
    ASSERT_EQ(graph_nodes, 3);
    ASSERT_EQ(graph_edges, 1);
    ASSERT_EQ(metadata_nodes, 3);
    ASSERT_EQ(metadata_edges, 1);
    ASSERT_EQ(metadata_topology_edges, 1);
    ASSERT_EQ(fts_rows, 4);

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
    ASSERT_TRUE(metrics.generation_mismatch);
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
              CBM_STORE_NOT_FOUND);
    ASSERT_TRUE(metrics.fallback);
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
        ASSERT_TRUE(graph_metrics.generation_mismatch);
        cbm_zova_graph_visits_free(visits, visit_count);

        const char *keywords[] = {"alpha"};
        cbm_vector_result_t *results = NULL;
        int result_count = 0;
        cbm_vector_search_metrics_t vector_metrics = {0};
        ASSERT_EQ(cbm_store_vector_search_ex(store, "proj", keywords, 1, 2, &results,
                                             &result_count, &vector_metrics),
                  CBM_STORE_OK);
        ASSERT_FALSE(vector_metrics.used_zova);
        ASSERT_TRUE(vector_metrics.generation_mismatch);
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

    int64_t forbidden_projection_tables = -1;
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT count(*) FROM sqlite_master WHERE name IN "
                  "('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1')",
                  &forbidden_projection_tables),
              0);
    ASSERT_EQ(forbidden_projection_tables, 0);

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
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-canonical-graph-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id=1,.project="proj",.label="Function",.name="Alpha",
         .qualified_name="proj.Alpha",.file_path="alpha.c",.start_line=1,.end_line=4,
         .properties="{}"},
        {.id=2,.project="proj",.label="Function",.name="Beta",
         .qualified_name="proj.Beta",.file_path="beta.c",.start_line=5,.end_line=9,
         .properties="{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id=1,.project="proj",.source_id=1,.target_id=2,.type="CALLS",
         .properties="{\"kind\":\"call\"}",.url_path="/api/beta",.local_name="Beta"},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/canonical-graph",.project="proj",
        .indexed_at="2026-07-15T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=2,
        .nodes=nodes,.node_count=2,.edges=edges,.edge_count=1,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &published), 0);

    const char *workspace_id = published.workspace_id;
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char alpha_node_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    char beta_node_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Function", "alpha.c", "proj.Alpha",
                                             "named:proj.Alpha", alpha_node_id,
                                             sizeof(alpha_node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Function", "beta.c", "proj.Beta",
                                             "named:proj.Beta", beta_node_id,
                                             sizeof(beta_node_id)),
              0);

    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(path);
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
    ASSERT_NOT_NULL(visits[0].properties);
    ASSERT_STR_EQ(visits[0].properties, "{}");
    ASSERT_EQ(visits[0].hop, 1);
    ASSERT_FALSE(metrics.fallback);
    ASSERT_TRUE(metrics.native_profiled);
    ASSERT_GT(metrics.native_result_count, 0);
    ASSERT_GT(metrics.frontier_expansions, 0);
    cbm_unsetenv("CBM_ZOVA_GRAPH_PROFILE");
    cbm_zova_graph_visits_free(visits, count);

    const char *call_types[] = {"CALLS"};
    cbm_zova_graph_adjacency_t *adjacency = NULL;
    int adjacency_count = 0;
    ASSERT_EQ(cbm_zova_graph_session_adjacency(
                  session, workspace_id, graph_name, alpha_node_id, "outbound", call_types, 1,
                  10, &adjacency, &adjacency_count), 0);
    ASSERT_EQ(adjacency_count, 1);
    ASSERT_STR_EQ(adjacency[0].source_node_id, alpha_node_id);
    ASSERT_STR_EQ(adjacency[0].target_node_id, beta_node_id);
    ASSERT_STR_EQ(adjacency[0].edge_type, "CALLS");
    ASSERT_NOT_NULL(adjacency[0].properties);
    ASSERT_STR_EQ(adjacency[0].properties, "{\"kind\":\"call\"}");
    cbm_zova_graph_adjacency_free(adjacency, adjacency_count);
    ASSERT_EQ(cbm_zova_graph_session_adjacency_prepare_count(session), 0);
    adjacency = NULL;
    adjacency_count = 0;
    ASSERT_EQ(cbm_zova_graph_session_adjacency(
                  session, workspace_id, graph_name, beta_node_id, "inbound", call_types, 1,
                  10, &adjacency, &adjacency_count), 0);
    ASSERT_EQ(adjacency_count, 1);
    ASSERT_EQ(cbm_zova_graph_session_adjacency_prepare_count(session), 0);
    cbm_zova_graph_adjacency_free(adjacency, adjacency_count);
    int native_degree = -1;
    ASSERT_EQ(cbm_zova_graph_session_degree(session, workspace_id, graph_name, alpha_node_id,
                                             "both", call_types, 1, &native_degree), 0);
    ASSERT_EQ(native_degree, 1);

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
    cbm_unlink(path);
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
         .properties = "{\"source\":\"direct\"}"},
        {.id = 2,
         .project = "direct-proj",
         .label = "Function",
         .name = "DirectLeaf",
         .qualified_name = "direct.Leaf",
         .file_path = "direct/leaf.c",
         .start_line = 3,
         .end_line = 4,
         .properties = "{\"source\":\"direct\",\"kind\":\"leaf\"}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1,
         .project = "direct-proj",
         .source_id = 1,
         .target_id = 2,
         .type = "CALLS",
         .properties = "{\"kind\":\"call\"}",
         .url_path = "",
         .local_name = ""},
        {.id = 2,
         .project = "direct-proj",
         .source_id = 1,
         .target_id = 2,
         .type = "CALLS",
         .properties = "{\"kind\":\"call\"}",
         .url_path = "",
         .local_name = ""},
    };
    ASSERT_EQ(cbm_zova_write_workspace_graph_direct(fx.zova_path, workspace_id, "direct-proj",
                                                     nodes, 2, edges, 2),
              0);
    int64_t forbidden_projection_tables = -1;
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT count(*) FROM sqlite_master WHERE name IN "
                  "('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1')",
                  &forbidden_projection_tables),
              0);
    ASSERT_EQ(forbidden_projection_tables, 0);

    zova_database *db = NULL;
    zova_message error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = fx.zova_path, .out_db = &db, .out_error_message = &error}), ZOVA_OK);
    zova_message_free(&error);
    zova_graph_neighbor_results neighbors = {0};
    ASSERT_EQ(zova_graph_neighbors(&(zova_graph_neighbors_request){
                  .db = db, .graph_name = graph_name, .node_id = root_id,
                  .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING, .edge_type = "CALLS",
                  .limit = 10, .out_results = &neighbors}), ZOVA_OK);
    ASSERT_EQ(neighbors.len, 1);
    ASSERT_STR_EQ(neighbors.items[0].node_id, leaf_id);
    zova_graph_neighbor_results_free(&neighbors);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

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

    zova_database *db = NULL;
    zova_message error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = fx.zova_path, .out_db = &db, .out_error_message = &error}), ZOVA_OK);
    zova_message_free(&error);
    uint8_t exists = 1;
    ASSERT_EQ(zova_graph_node_exists(&(zova_graph_node_exists_request){
                  .db = db, .graph_name = graph_a, .node_id = a_root,
                  .out_exists = &exists}), ZOVA_OK);
    ASSERT_FALSE(exists);
    zova_graph_neighbor_results neighbors = {0};
    ASSERT_EQ(zova_graph_neighbors(&(zova_graph_neighbors_request){
                  .db = db, .graph_name = graph_a, .node_id = a_replacement_root,
                  .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING, .edge_type = "CALLS",
                  .limit = 10, .out_results = &neighbors}), ZOVA_OK);
    ASSERT_EQ(neighbors.len, 1);
    zova_graph_neighbor_results_free(&neighbors);
    ASSERT_EQ(zova_graph_neighbors(&(zova_graph_neighbors_request){
                  .db = db, .graph_name = graph_b, .node_id = b_root,
                  .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING, .edge_type = "CALLS",
                  .limit = 10, .out_results = &neighbors}), ZOVA_OK);
    ASSERT_EQ(neighbors.len, 1);
    ASSERT_STR_EQ(neighbors.items[0].node_id, b_leaf);
    zova_graph_neighbor_results_free(&neighbors);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    ASSERT_EQ(cbm_zova_delete_workspace_graph(fx.zova_path, workspace_a), 0);
    db = NULL;
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = fx.zova_path, .out_db = &db, .out_error_message = &error}), ZOVA_OK);
    zova_message_free(&error);
    ASSERT_EQ(zova_graph_neighbors(&(zova_graph_neighbors_request){
                  .db = db, .graph_name = graph_b, .node_id = b_root,
                  .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING, .edge_type = "CALLS",
                  .limit = 10, .out_results = &neighbors}), ZOVA_OK);
    ASSERT_EQ(neighbors.len, 1);
    zova_graph_neighbor_results_free(&neighbors);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);

    int64_t forbidden_projection_tables = -1;
    ASSERT_EQ(zova_scalar_int64(
                  fx.zova_path,
                  "SELECT count(*) FROM sqlite_master WHERE name IN "
                  "('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1')",
                  &forbidden_projection_tables), 0);
    ASSERT_EQ(forbidden_projection_tables, 0);

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

    char cache_path[TZ_PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s/cbm-zova-gbuf-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache_path));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache_path, 1);

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
    /* The SQLite dump and user-local publication must share one generation
     * timestamp; a delay makes accidental regeneration in the publisher
     * observable instead of relying on wall-clock second coincidence. */
    sleep(2);
    cbm_zova_workspace_generation_result_t user_result = {0};
    cbm_zova_publish_model_test_reset_build_count();
    ASSERT_EQ(cbm_gbuf_publish_zova_user_database(gb, NULL, 0, NULL, &user_result), 0);
    ASSERT_EQ(cbm_zova_publish_model_test_build_count(), 0);

    char user_database_path[TZ_PATH_MAX];
    ASSERT_EQ(cbm_zova_workspace_registry_path(user_database_path,
                                                sizeof(user_database_path)), 0);
    int64_t user_generation = 0;
    char user_sql[512];
    snprintf(user_sql, sizeof(user_sql),
             "SELECT generation FROM cbm_workspace_index_state_v1 WHERE generation > 0");
    ASSERT_EQ(zova_scalar_int64(user_database_path, user_sql, &user_generation), 0);
    ASSERT_EQ(user_generation, 1);

    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, root_path, workspace_id,
                                            sizeof(workspace_id)),
              0);
    sqlite3 *source_db = NULL;
    sqlite3 *user_db = NULL;
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *user_stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(db_path, &source_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_open_v2(user_database_path, &user_db, SQLITE_OPEN_READONLY, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(source_db, "SELECT indexed_at FROM projects WHERE name=?1", -1,
                                 &source_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(source_stmt), SQLITE_ROW);
    const char *source_indexed_at = (const char *)sqlite3_column_text(source_stmt, 0);
    ASSERT_NOT_NULL(source_indexed_at);
    char source_indexed_copy[64];
    snprintf(source_indexed_copy, sizeof(source_indexed_copy), "%s", source_indexed_at);
    ASSERT_EQ(sqlite3_prepare_v2(user_db,
                                 "SELECT p.indexed_at FROM cbm_projects_v1 p "
                                 "JOIN cbm_workspace_registry r USING(workspace_key) "
                                 "WHERE r.workspace_id=?1",
                                 -1, &user_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(user_stmt, 1, workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(user_stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(user_stmt, 0), source_indexed_copy);
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(user_stmt);
    sqlite3_close(source_db);
    sqlite3_close(user_db);

    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char root_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Function", "root.c", "direct.Root",
                                             "named:direct.Root", root_id, sizeof(root_id)),
              0);
    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(user_database_path);
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
    cbm_unlink(user_database_path);
    char user_wal[TZ_PATH_MAX];
    char user_shm[TZ_PATH_MAX];
    snprintf(user_wal, sizeof(user_wal), "%s-wal", user_database_path);
    snprintf(user_shm, sizeof(user_shm), "%s-shm", user_database_path);
    cbm_unlink(user_wal);
    cbm_unlink(user_shm);
    cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    PASS();
#endif
}

TEST(zova_gbuf_prepare_owns_full_publication_view) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char cache_path[TZ_PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s/cbm-zova-prepared-view-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache_path));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache_path, 1);

    cbm_gbuf_t *gb = cbm_gbuf_new("prepared", "/tmp/prepared-view");
    ASSERT_NOT_NULL(gb);
    int64_t root = cbm_gbuf_upsert_node(
        gb, "Function", "Root", "prepared.Root", "root.c", 1, 2, "{}");
    int64_t leaf = cbm_gbuf_upsert_node(
        gb, "Function", "Leaf", "prepared.Leaf", "leaf.c", 3, 4, "{}");
    ASSERT_GT(root, 0);
    ASSERT_GT(leaf, 0);
    ASSERT_GT(cbm_gbuf_insert_edge(gb, root, leaf, "CALLS", "{}"), 0);
    ASSERT_EQ(cbm_gbuf_prepare_zova_dump(gb), 0);

    cbm_zova_publish_model_test_fail_allocation_at(0);
    cbm_zova_workspace_generation_result_t result = {0};
    ASSERT_EQ(cbm_gbuf_publish_zova_user_database(gb, NULL, 0, NULL, &result), 0);
    cbm_zova_publish_model_test_fail_allocation_at(-1);
    ASSERT_EQ(result.graph_nodes, 2);
    ASSERT_EQ(result.graph_edges, 1);

    char user_database_path[TZ_PATH_MAX];
    ASSERT_EQ(cbm_zova_workspace_registry_path(user_database_path,
                                                sizeof(user_database_path)), 0);
    cbm_gbuf_free(gb);
    cbm_unlink(user_database_path);
    char user_wal[TZ_PATH_MAX];
    char user_shm[TZ_PATH_MAX];
    snprintf(user_wal, sizeof(user_wal), "%s-wal", user_database_path);
    snprintf(user_shm, sizeof(user_shm), "%s-shm", user_database_path);
    cbm_unlink(user_wal);
    cbm_unlink(user_shm);
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
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
                                "SELECT count(*) FROM _zova_vectors v "
                                "JOIN _zova_vector_collections c USING(collection_key) "
                                "WHERE c.name = 'cbm_node_vectors_i8'",
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

TEST(zova_sqlite_schema_inventory_has_migration_coverage) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx.db_path), 0);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(fx.db_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    const char *required[] = {
        "projects", "file_hashes", "nodes", "edges", "project_summaries",
        "node_vectors", "token_vectors", "nodes_fts", "cbm_zova_sidecar_generation_v1",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db,
                      "SELECT count(*) FROM sqlite_master WHERE name = ?1 AND "
                      "type IN ('table','view')",
                      -1, &stmt, NULL),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(stmt, 1, required[i], -1, SQLITE_STATIC), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_user_database_bootstrap_creates_versioned_workspace_schema) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-schema-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    /* Bootstrap is intentionally idempotent so a restarted migration can
     * safely resume without replacing the existing user-local database. */
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);

    const char *required[] = {
        "cbm_database_schema_v1", "cbm_workspace_registry", "cbm_database_generation_v1",
        "cbm_projects_v1", "cbm_nodes_v1",
        "cbm_files_v1", "cbm_file_hashes_v1",
        "cbm_token_vector_metadata_v1", "cbm_nodes_fts_v1",
        "cbm_workspace_index_state_v1", "cbm_project_summaries_v2",
        "cbm_generation_integrity_v2", "cbm_workspace_migrations_v1",
    };
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db,
                      "SELECT count(*) FROM sqlite_master WHERE name = ?1 AND "
                      "type IN ('table','virtual table')",
                      -1, &stmt, NULL),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(stmt, 1, required[i], -1, SQLITE_STATIC), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
        sqlite3_finalize(stmt);
    }
    const char *forbidden[] = {
        "cbm_node_vectors_compat_v1", "cbm_token_vectors_compat_v1", "cbm_fts_rowmap_v1",
        "cbm_workspace_generations", "cbm_project_summaries_v1",
        "cbm_generation_integrity_v1", "cbm_edges_v1", "cbm_edges_v1_workspace_zova_edge",
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db, "SELECT count(*) FROM sqlite_master WHERE name=?1", -1, &stmt, NULL),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(stmt, 1, forbidden[i], -1, SQLITE_STATIC), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_stmt *metadata_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_token_vector_metadata_v1')",
                  -1, &metadata_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(metadata_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(metadata_columns, 0),
                  "token_key,workspace_key,token,idf");
    sqlite3_finalize(metadata_columns);
    sqlite3_stmt *workspace_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_workspace_registry')",
                  -1, &workspace_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(workspace_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(workspace_columns, 0),
                  "workspace_key,workspace_id,canonical_root,id_format_version,active_generation");
    sqlite3_finalize(workspace_columns);
    sqlite3_stmt *node_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_nodes_v1')",
                  -1, &node_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(node_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(node_columns, 0),
                  "zova_node_key,workspace_key,label,name,qualified_name,file_key,start_line,"
                  "end_line,properties,source_ordinal");
    sqlite3_finalize(node_columns);
    sqlite3_stmt *file_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_files_v1')",
                  -1, &file_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(file_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(file_columns, 0),
                  "file_key,workspace_key,file_path");
    sqlite3_finalize(file_columns);
    sqlite3_close(db);

    int64_t schema_version = 0;
    ASSERT_EQ(zova_scalar_int64(path, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id = 1",
                                &schema_version),
              0);
    ASSERT_EQ(schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_stmt *fk_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM pragma_foreign_key_list('cbm_nodes_v1') "
                  "WHERE \"table\"='cbm_files_v1' AND ((seq=0 AND \"from\"='workspace_key' "
                  "AND \"to\"='workspace_key') OR (seq=1 AND \"from\"='file_key' "
                  "AND \"to\"='file_key'))",
                  -1, &fk_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(fk_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(fk_stmt, 0), 2);
    sqlite3_finalize(fk_stmt);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
                  "'cbm_nodes_v1_file_workspace_bi','cbm_nodes_v1_file_workspace_bu',"
                  "'cbm_edges_v1_endpoint_workspace_bi','cbm_edges_v1_endpoint_workspace_bu')",
                  -1, &fk_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(fk_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(fk_stmt, 0), 0);
    sqlite3_finalize(fk_stmt);
    sqlite3_close(db);
    zova_database *zdb = NULL;
    zova_message zerr = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path=path,.out_db=&zdb,.out_error_message=&zerr}), ZOVA_OK);
    zova_message_free(&zerr);
    ASSERT_EQ(cbm_zova_register_sql_functions(zdb), 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="PRAGMA foreign_keys=ON"}), ZOVA_OK);
    zova_status bootstrap_insert_status = zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql=
                      "INSERT INTO cbm_workspace_registry(workspace_id,canonical_root,id_format_version) "
                      "VALUES('workspace_A','/tmp/a',1),('workspace_B','/tmp/b',1);"
                      "INSERT INTO cbm_files_v1(workspace_key,file_path) VALUES"
                      "((SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_A'),'a.c'),"
                      "((SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_B'),'b.c');"
                      "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,"
                      "file_key,start_line,end_line) VALUES"
                      "(101,(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_A'),"
                      "'Function','a','p.a',(SELECT file_key FROM cbm_files_v1 WHERE file_path='a.c'),1,1),"
                      "(102,(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_B'),"
                      "'Function','b','p.b',(SELECT file_key FROM cbm_files_v1 WHERE file_path='b.c'),1,1)"});
    if (bootstrap_insert_status != ZOVA_OK)
        fprintf(stderr, "bootstrap compact insert: %s\n", zova_database_last_error_message(zdb));
    ASSERT_EQ(bootstrap_insert_status, ZOVA_OK);
    ASSERT_NEQ(zova_database_exec(&(zova_database_exec_request){
                   .db=zdb,.sql=
                       "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,"
                       "file_key,start_line,end_line) VALUES("
                       "103,(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_A'),"
                       "'Function','bad','p.bad',"
                       "(SELECT file_key FROM cbm_files_v1 WHERE file_path='b.c'),1,1)"}),
               ZOVA_OK);
    ASSERT_NEQ(zova_database_exec(&(zova_database_exec_request){
                   .db=zdb,.sql=
                       "UPDATE cbm_nodes_v1 SET file_key=(SELECT file_key FROM cbm_files_v1 "
                       "WHERE file_path='b.c') WHERE node_id='node_A'"}),
               ZOVA_OK);
    int64_t fts_rows = 0;
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1_docsize",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 2);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="UPDATE cbm_nodes_v1 SET name='GammaNode' "
                               "WHERE zova_node_key=101"}), ZOVA_OK);
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1 "
                                "WHERE cbm_nodes_fts_v1 MATCH 'Gamma'",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 1);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="BEGIN; UPDATE cbm_nodes_v1 SET name='DeltaNode' "
                               "WHERE zova_node_key=101; ROLLBACK"}), ZOVA_OK);
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1 "
                                "WHERE cbm_nodes_fts_v1 MATCH 'Delta'",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="DELETE FROM cbm_nodes_v1 WHERE zova_node_key=101;"
                               "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1) VALUES('rebuild');"
                               "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1,rank) "
                               "VALUES('integrity-check',1)"}), ZOVA_OK);
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1_docsize",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 1);
    ASSERT_EQ(cbm_zova_user_database_verify_workspace_schema(zdb), 0);
    ASSERT_EQ(zova_database_close(zdb), ZOVA_OK);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "EXPLAIN QUERY PLAN SELECT file_key FROM cbm_files_v1 f "
                  "WHERE f.workspace_key=1 AND NOT EXISTS(SELECT 1 FROM cbm_nodes_v1 n "
                  "WHERE n.workspace_key=f.workspace_key AND n.file_key=f.file_key)",
                  -1, &fk_stmt, NULL), SQLITE_OK);
    bool indexed_file_reference_lookup=false;
    while(sqlite3_step(fk_stmt)==SQLITE_ROW){
        const char *detail=(const char *)sqlite3_column_text(fk_stmt,3);
        if(detail&&strstr(detail,"cbm_nodes_v1_workspace_file")&&
           strstr(detail,"workspace_key=? AND file_key=?"))
            indexed_file_reference_lookup=true;
    }
    ASSERT_TRUE(indexed_file_reference_lookup);
    sqlite3_finalize(fk_stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA foreign_key_check", -1, &fk_stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(fk_stmt), SQLITE_DONE);
    sqlite3_finalize(fk_stmt);
    sqlite3_close(db);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_schema_uses_zova_as_sole_topology_authority) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-sole-topology-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);

    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_nodes_v1')",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "zova_node_key,workspace_key,label,name,qualified_name,file_key,start_line,"
                  "end_line,properties,source_ordinal");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='table' "
                  "AND name='cbm_edges_v1'",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='index' AND name IN ("
                  "'cbm_edges_v1_source_type','cbm_edges_v1_target_type')",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='index' "
                  "AND name='cbm_edges_v1_workspace_zova_edge'",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    cbm_unlink(path);
    PASS();
}

TEST(zova_database_format_status_requires_atomic_repack_for_v5_v7) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-format-status-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_COMPATIBLE);

    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
                           "UPDATE _zova_meta SET value='7' WHERE key='format_version';"
                           "UPDATE cbm_database_schema_v1 SET schema_version=5 WHERE id=1;",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_REPACK_REQUIRED);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE cbm_database_schema_v1 SET schema_version=999 WHERE id=1",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_INCOMPATIBLE);

    static const char *expected[] = {
        "source_snapshot", "temp_creation", "workspace_publish", "verification", "fsync",
        "live_to_recovery_rename", "temp_to_live_rename", "final_reopen",
    };
    ASSERT_EQ((int)(sizeof(expected) / sizeof(expected[0])), CBM_ZOVA_REPACK_PHASE_COUNT);
    for (int i = 0; i < CBM_ZOVA_REPACK_PHASE_COUNT; i++)
        ASSERT_STR_EQ(cbm_zova_repack_phase_name((cbm_zova_repack_phase_t)i), expected[i]);
    ASSERT_NULL(cbm_zova_repack_phase_name((cbm_zova_repack_phase_t)-1));
    ASSERT_NULL(cbm_zova_repack_phase_name(CBM_ZOVA_REPACK_PHASE_COUNT));
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_has_one_canonical_fts_without_rowmap) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-fts-schema-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    int64_t count = 0;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE sql LIKE 'CREATE VIRTUAL TABLE%'",
                  &count),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name='cbm_fts_rowmap_v1' "
                  "OR name GLOB 'cbm_fts_w1_*'",
                  &count),
              0);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name='cbm_nodes_fts_v1_content'",
                  &count),
              0);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='view' "
                  "AND name='cbm_nodes_fts_content_v1'",
                  &count),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='trigger' "
                  "AND name IN ('cbm_nodes_v1_fts_ai','cbm_nodes_v1_fts_bd',"
                  "'cbm_nodes_v1_fts_bu','cbm_nodes_v1_fts_au')",
                  &count),
              0);
    ASSERT_EQ(count, 4);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_generation_state_commits_and_rolls_back) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-generation-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_user_database_generation_begin(
                  path, "/tmp/cbm-user-generation-project", 1, workspace_id,
                  sizeof(workspace_id)),
              0);
    ASSERT_TRUE(workspace_id[0] != '\0');
    char state[16] = {0};
    ASSERT_EQ(zova_generation_state(path, workspace_id, 1, state, sizeof(state)), 0);
    ASSERT_STR_EQ(state, "building");

    ASSERT_EQ(cbm_zova_user_database_generation_finish(path, workspace_id, 1, true), 0);
    ASSERT_EQ(zova_generation_state(path, workspace_id, 1, state, sizeof(state)), 0);
    ASSERT_STR_EQ(state, "ready");
    int64_t active_generation = 0;
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(path, workspace_id, &active_generation), 0);
    ASSERT_EQ(active_generation, 1);

    ASSERT_EQ(cbm_zova_user_database_generation_begin(
                  path, "/tmp/cbm-user-generation-project", 2, workspace_id,
                  sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_user_database_generation_finish(path, workspace_id, 2, false), 0);
    ASSERT_EQ(zova_generation_state(path, workspace_id, 2, state, sizeof(state)), 0);
    ASSERT_STR_EQ(state, "failed");
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(path, workspace_id, &active_generation), 0);
    ASSERT_EQ(active_generation, 1);

    /* A generation can only transition once from building; a failed/ready
     * generation cannot be silently rewritten by a finishing call. */
    ASSERT_EQ(cbm_zova_user_database_generation_finish(path, workspace_id, 2, true), -1);
    cbm_unlink(path);
    PASS();
}

TEST(zova_sql_capability_probe_covers_workspace_metadata_and_sql_surface) {
    zova_fixture_t fx;
    ASSERT_EQ(create_fixture(&fx), 0);
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    ASSERT_EQ(build_direct_fixture_sidecar(&fx), 0);

    cbm_zova_sql_capabilities_t capabilities = {0};
    ASSERT_EQ(cbm_zova_probe_sql_capabilities(fx.zova_path, &capabilities), 0);
    ASSERT_FALSE(capabilities.canonical_workspace_metadata);
    ASSERT_TRUE(capabilities.fts5);
    ASSERT_TRUE(capabilities.bm25);
    ASSERT_TRUE(capabilities.json);
    ASSERT_TRUE(capabilities.recursive_cte);
    ASSERT_TRUE(capabilities.transactions);
    ASSERT_TRUE(capabilities.cbm_scalar_functions);

    ASSERT_EQ(cbm_zova_probe_sql_capabilities("/tmp/does-not-exist-cbm.zova", &capabilities), -1);
    cleanup_fixture(&fx);
    cbm_unsetenv("CBM_ZOVA_MODE");
    PASS();
}

TEST(zova_user_database_capability_probe_covers_empty_schema_surface) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-capabilities-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    cbm_zova_sql_capabilities_t capabilities = {0};
    ASSERT_EQ(cbm_zova_probe_sql_capabilities(path, &capabilities), 0);
    ASSERT_TRUE(capabilities.canonical_workspace_metadata);
    ASSERT_TRUE(capabilities.fts5);
    ASSERT_TRUE(capabilities.bm25);
    ASSERT_TRUE(capabilities.json);
    ASSERT_TRUE(capabilities.recursive_cte);
    ASSERT_TRUE(capabilities.transactions);
    ASSERT_TRUE(capabilities.cbm_scalar_functions);

    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_imports_workspace_metadata_and_fts) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-import-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    const CBMDumpNode nodes[] = {
        {.id = 1,
         .project = "proj",
         .label = "Function",
         .name = "AlphaBeta",
         .qualified_name = "proj.AlphaBeta",
         .file_path = "src/alpha.c",
         .start_line = 3,
         .end_line = 8,
         .properties = "{\"risk\":2}"},
        {.id = 2,
         .project = "proj",
         .label = "Function",
         .name = "Helper",
         .qualified_name = "proj.Helper",
         .file_path = "src/helper.c",
         .start_line = 10,
         .end_line = 12,
         .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1,
         .project = "proj",
         .source_id = 1,
         .target_id = 2,
         .type = "CALLS",
         .properties = "{\"weight\":1}",
         .url_path = "",
         .local_name = ""},
    };

    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 1,
                                                       nodes, 2, edges, 1),
              0);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 2,
                                                       nodes, 2, edges, 1),
              0);
    cbm_zova_graph_session_t *generation_session = cbm_zova_graph_session_open(path);
    ASSERT_NOT_NULL(generation_session);
    int64_t session_generation = 0;
    ASSERT_EQ(cbm_zova_graph_session_generation(generation_session, &session_generation), 0);
    ASSERT_EQ(session_generation, 2);
    cbm_zova_graph_session_close(generation_session);

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_workspace_lookup_at(path, "/tmp/user-import-a", workspace_id,
                                            sizeof(workspace_id)),
              0);
    cbm_store_t *workspace_store = cbm_store_open_zova_workspace_query(path, workspace_id);
    ASSERT_NOT_NULL(workspace_store);
    sqlite3_stmt *cache_size_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(cbm_store_get_db(workspace_store), "PRAGMA cache_size", -1,
                                 &cache_size_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(cache_size_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(cache_size_stmt, 0), -262144);
    sqlite3_finalize(cache_size_stmt);
    cbm_search_output_t workspace_search = {0};
    cbm_search_params_t workspace_params = {.project = "proj", .min_degree = -1,
                                             .max_degree = -1, .limit = 20};
    (void)cbm_store_search(workspace_store, &workspace_params, &workspace_search);
    if (workspace_search.count != 2) fprintf(stderr, "workspace search error: %s\n", cbm_store_error(workspace_store));
    ASSERT_EQ(workspace_search.count, 2);
    cbm_store_search_free(&workspace_search);
    cbm_cypher_result_t workspace_cypher = {0};
    ASSERT_EQ(cbm_cypher_execute(workspace_store,
              "MATCH (n) WHERE n.name IS NOT NULL RETURN n.name ORDER BY n.name", "proj", 20,
              &workspace_cypher), 0);
    ASSERT_EQ(workspace_cypher.row_count, 2);
    cbm_cypher_result_free(&workspace_cypher);
    cbm_store_close(workspace_store);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    int64_t count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')", workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1 f JOIN cbm_nodes_v1 n ON n.zova_node_key=f.rowid "
             "WHERE n.workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
             "WHERE workspace_id='%s') AND cbm_nodes_fts_v1 MATCH 'Alpha'",
             workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM _zova_graph_nodes n JOIN _zova_graphs g "
             "ON g.graph_key=n.graph_key WHERE g.name='%s' AND "
             "(n.target_type<>'none' OR n.target_namespace IS NOT NULL OR n.target_ref IS NOT NULL)",
             graph_name);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 0);

    /* A failed replacement must roll back the clear-and-rewrite transaction,
     * leaving generation 2's rows available. */
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_after_nodes", 1);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 3,
                                                       nodes, 2, edges, 1),
              -1);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')", workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);

    /* Reusing the same numeric dump ids for another workspace must not cause
     * metadata or FTS rows to collide. */
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-b", "proj-b", 1,
                                                       nodes, 2, edges, 1),
              0);
    char workspace_b[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_workspace_lookup_at(path, "/tmp/user-import-b", workspace_b,
                                            sizeof(workspace_b)),
              0);
    ASSERT_TRUE(strcmp(workspace_id, workspace_b) != 0);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             workspace_b);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);

    /* Replacing workspace A must leave workspace B untouched. */
    const CBMDumpNode replacement_nodes[] = {nodes[0]};
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 4,
                                                       replacement_nodes, 1, NULL, 0),
              0);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             workspace_b);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);
    sqlite3_close(db);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_delete_workspace_is_isolated_and_idempotent) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-delete-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    const CBMDumpNode a[] = {{.id=1,.project="shared",.label="Function",.name="same",
        .qualified_name="shared.sameA",.file_path="same.c",.start_line=1,.end_line=2,
        .properties="{}"}};
    const CBMDumpNode b[] = {{.id=1,.project="shared_b",.label="Function",.name="same",
        .qualified_name="shared.sameB",.file_path="same.c",.start_line=1,.end_line=2,
        .properties="{}"}};
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/delete-a", "shared", 1,
                                                       a, 1, NULL, 0), 0);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/delete-b", "shared_b", 1,
                                                       b, 1, NULL, 0), 0);
    cbm_zova_repository_t *repo_a = cbm_zova_repository_open(path, "shared");
    cbm_zova_repository_t *repo_b = cbm_zova_repository_open(path, "shared_b");
    ASSERT_NOT_NULL(repo_a); ASSERT_NOT_NULL(repo_b);
    char workspace_a[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_a, sizeof(workspace_a), "%s", cbm_zova_repository_workspace_id(repo_a));
    char workspace_b[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_b, sizeof(workspace_b), "%s", cbm_zova_repository_workspace_id(repo_b));
    cbm_zova_repository_close(repo_a); cbm_zova_repository_close(repo_b);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, workspace_a), 0);
    ASSERT_NULL(cbm_zova_repository_open(path, "shared"));
    repo_b = cbm_zova_repository_open(path, "shared_b");
    ASSERT_NOT_NULL(repo_b);
    ASSERT_STR_EQ(cbm_zova_repository_workspace_id(repo_b), workspace_b);
    cbm_zova_repository_close(repo_b);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, workspace_a), 0);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, "bad-id"), -1);
    cbm_unlink(path);
    PASS();
}

TEST(zova_atomic_workspace_publisher_commits_complete_generation) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-atomic-publish-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "atomic", .label = "Function", .name = "Root",
         .qualified_name = "atomic.Root", .file_path = "root.c", .properties = "{}"},
        {.id = 2, .project = "atomic", .label = "Function", .name = "Leaf",
         .qualified_name = "atomic.Leaf", .file_path = "leaf.c", .properties = "{\"risk\":1}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "atomic", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    const int8_t root_vec[] = {127, 0};
    const int8_t leaf_vec[] = {0, 127};
    const CBMDumpVector vectors[] = {
        {.node_id = 1, .project = "atomic", .vector = (const uint8_t *)root_vec, .vector_len = 2},
        {.node_id = 2, .project = "atomic", .vector = (const uint8_t *)leaf_vec, .vector_len = 2},
    };
    const CBMDumpTokenVec tokens[] = {
        {.id = 1, .project = "atomic", .token = "root", .vector = (const uint8_t *)root_vec,
         .vector_len = 2, .idf = 1.0f},
        {.id = 2, .project = "atomic", .token = "leaf", .vector = (const uint8_t *)leaf_vec,
         .vector_len = 2, .idf = 0.5f},
    };
    const cbm_zova_file_hash_input_t hashes[] = {
        {.file_path = "root.c", .content_hash = "hash-root", .mtime_ns = 1, .size_bytes = 10},
        {.file_path = "leaf.c", .content_hash = "hash-leaf", .mtime_ns = 2, .size_bytes = 11},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/atomic-publish",
        .project = "atomic",
        .indexed_at = "2026-07-12T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = 2,
        .edges = edges,
        .edge_count = 1,
        .node_vectors = vectors,
        .node_vector_count = 2,
        .token_vectors = tokens,
        .token_vector_count = 2,
        .file_hashes = hashes,
        .file_hash_count = 2,
        .project_summary = {.present = true, .summary = "atomic summary", .source_hash = "sum-hash",
                            .created_at = "created", .updated_at = "updated"},
    };
    cbm_zova_workspace_generation_result_t result = {0};
    cbm_zova_publish_test_metrics_reset();
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &result), 0);
    cbm_zova_publish_test_metrics_t publish_metrics = {0};
    cbm_zova_publish_test_metrics_get(&publish_metrics);
    ASSERT_EQ(publish_metrics.database_open_count, 1);
    ASSERT_EQ(publish_metrics.database_close_count, 1);
    ASSERT_EQ(publish_metrics.database_handle_open_count, 1);
    ASSERT_EQ(publish_metrics.fresh_page_size_vacuum_count, 0);
    ASSERT_EQ(publish_metrics.transaction_count, 1);
    ASSERT_EQ(publish_metrics.full_clear_count, 1);
    ASSERT_EQ(publish_metrics.canonical_node_fts_passes, 1);
    ASSERT_EQ(publish_metrics.canonical_edge_passes, 0);
    ASSERT_EQ(publish_metrics.native_graph_fresh_calls, 1);
    ASSERT_EQ(publish_metrics.native_graph_prepared_calls, 1);
    ASSERT_EQ(publish_metrics.native_graph_node_calls, 0);
    ASSERT_EQ(publish_metrics.native_graph_edge_calls, 0);
    ASSERT_EQ(publish_metrics.native_node_vector_calls, 0);
    ASSERT_EQ(publish_metrics.native_token_vector_calls, 0);
    ASSERT_EQ(publish_metrics.integrity_writes, 1);
    ASSERT_EQ(publish_metrics.full_fts_bulk_statements, 0);
    ASSERT_EQ(publish_metrics.full_fts_trigger_rows_avoided, 2);
    ASSERT_EQ(publish_metrics.readback_count_scan_count, 0);
    ASSERT_EQ(publish_metrics.full_node_guard_validation_statements, 0);
    ASSERT_EQ(publish_metrics.full_edge_guard_validation_statements, 0);
    ASSERT_EQ(publish_metrics.canonical_files_sql.rows, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.bind_i64_calls, 4);
    ASSERT_EQ(publish_metrics.canonical_files_sql.bind_text_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_files_sql.step_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.reset_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.rows, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.bind_i64_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.bind_text_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.step_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.reset_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.rows, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.bind_i64_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.bind_text_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.step_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.reset_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.rows, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.bind_i64_calls, 6);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.bind_text_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.step_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.reset_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.rows, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.bind_i64_calls, 4);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.bind_text_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.bind_double_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.step_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.reset_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.clear_bindings_calls, 0);
    ASSERT_TRUE(isfinite(result.canonical_files_ms) && result.canonical_files_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.canonical_nodes_ms) && result.canonical_nodes_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.canonical_edges_ms) && result.canonical_edges_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.canonical_hashes_ms) && result.canonical_hashes_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fts_ms) && result.fts_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.token_metadata_ms) && result.token_metadata_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_ms) && result.native_graph_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_materialize_ms) &&
                result.native_graph_materialize_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_reset_ms) && result.native_graph_reset_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_nodes_ms) && result.native_graph_nodes_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_edges_ms) && result.native_graph_edges_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_validate_ms) &&
                result.native_graph_validate_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_key_generation_ms) &&
                result.native_graph_key_generation_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_cleanup_ms) &&
                result.native_graph_cleanup_ms >= 0.0);
    double native_graph_subtotal = result.native_graph_materialize_ms +
        result.native_graph_reset_ms + result.native_graph_nodes_ms +
        result.native_graph_edges_ms + result.native_graph_validate_ms +
        result.native_graph_key_generation_ms + result.native_graph_cleanup_ms;
    ASSERT_TRUE(native_graph_subtotal <= result.native_graph_ms + 1.0);
    ASSERT_TRUE(isfinite(result.fresh_validation_ms) && result.fresh_validation_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fresh_index_ms) && result.fresh_index_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fresh_commit_ms) && result.fresh_commit_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fresh_build_ms) && result.fresh_build_ms >= 0.0);
    double fresh_phase_subtotal = result.fresh_validation_ms +
        result.native_graph_validate_ms + result.native_graph_key_generation_ms +
        result.native_graph_nodes_ms + result.native_graph_edges_ms +
        result.canonical_nodes_ms + result.fts_ms + result.native_vectors_ms +
        result.fresh_index_ms + result.fresh_commit_ms;
    ASSERT_TRUE(fresh_phase_subtotal <= result.fresh_build_ms + 1.0);
    ASSERT_TRUE(result.native_graph_cleanup_ms <= result.fresh_build_ms + 1.0);
    ASSERT_TRUE(isfinite(result.native_vectors_ms) && result.native_vectors_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.readback_ms) && result.readback_ms >= 0.0);
    ASSERT_TRUE(result.workspace_id[0] != '\0');
    ASSERT_EQ(result.generation, 1);
    ASSERT_EQ(result.graph_nodes, 2);
    ASSERT_EQ(result.graph_edges, 1);
    ASSERT_EQ(result.metadata_nodes, 2);
    ASSERT_EQ(result.metadata_edges, 1);
    ASSERT_EQ(result.fts_rows, 2);
    ASSERT_EQ(result.node_vectors, 2);
    ASSERT_EQ(result.token_vectors, 2);
    int64_t schema_version = 0;
    ASSERT_EQ(zova_scalar_int64(
                  path, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
                  &schema_version),
              0);
    ASSERT_EQ(schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    int64_t projection_tables = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                  "('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1')",
                  &projection_tables),
              0);
    ASSERT_EQ(projection_tables, 0);
    int64_t forbidden_objects = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE "
                  "name IN ('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1',"
                  "'cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
                  "'cbm_token_vectors_compat_v1','_zova_vector_norms') "
                  "OR name GLOB 'cbm_fts_w1_*'",
                  &forbidden_objects),
              0);
    ASSERT_EQ(forbidden_objects, 0);
    int64_t token_columns = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM pragma_table_info('cbm_token_vector_metadata_v1') "
                  "WHERE (cid=0 AND name='token_key' AND type='INTEGER' AND pk=1) OR "
                  "(cid=1 AND name='workspace_key' AND type='INTEGER') OR "
                  "(cid=2 AND name='token' AND type='TEXT') OR "
                  "(cid=3 AND name='idf' AND type='REAL')",
                  &token_columns),
              0);
    ASSERT_EQ(token_columns, 4);
    int64_t duplicate_fts_objects = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name='cbm_fts_rowmap_v1' "
                  "OR name GLOB 'cbm_fts_w1_*'",
                  &duplicate_fts_objects),
              0);
    ASSERT_EQ(duplicate_fts_objects, 0);
    int64_t canonical_fts_rows = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path, "SELECT count(*) FROM cbm_nodes_fts_v1_docsize", &canonical_fts_rows),
              0);
    ASSERT_EQ(canonical_fts_rows, result.metadata_nodes);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND "
                  "name='cbm_nodes_v1_fts_ai'",
                  &canonical_fts_rows),
              0);
    ASSERT_EQ(canonical_fts_rows, 1);
    cbm_store_t *cypher_store =
        cbm_store_open_zova_workspace_query(path, result.workspace_id);
    ASSERT_NOT_NULL(cypher_store);
    cbm_cypher_result_t native_cypher = {0};
    ASSERT_EQ(cbm_cypher_execute(cypher_store,
              "MATCH (a:Function)-[:CALLS]->(b:Function) RETURN a.name,b.name",
              "atomic", 20, &native_cypher), 0);
    ASSERT_EQ(native_cypher.row_count, 1);
    ASSERT_GT(cbm_store_zova_native_topology_ops(cypher_store), 0);
    cbm_cypher_result_free(&native_cypher);
    int topology_before_degree = cbm_store_zova_native_topology_ops(cypher_store);
    cbm_cypher_result_t degree_cypher = {0};
    ASSERT_EQ(cbm_cypher_execute(cypher_store,
              "MATCH (a:Function) RETURN a.name,a.in_degree,a.out_degree ORDER BY a.name",
              "atomic", 20, &degree_cypher), 0);
    ASSERT_EQ(degree_cypher.row_count, 2);
    ASSERT_GT(cbm_store_zova_native_topology_ops(cypher_store), topology_before_degree);
    cbm_cypher_result_free(&degree_cypher);
    cbm_store_close(cypher_store);
    int64_t count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_workspace_index_state_v1 WHERE "
                              "workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                              "WHERE workspace_id='%s') AND generation=1", result.workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_generation_integrity_v2 WHERE "
                              "workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                              "WHERE workspace_id='%s') AND generation=1", result.workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);

    const char *fault_phases[] = {
        "user_after_nodes_before_bulk_finalize", "user_after_bulk_fts",
        "user_after_edges_before_bulk_finalize",
        "user_after_metadata",     "user_after_fts",       "user_after_graph_nodes",
        "user_after_graph_edges",  "user_after_node_vectors", "user_after_token_vectors",
        "user_after_integrity",    "user_before_commit",
    };
    for (size_t i = 0; i < sizeof(fault_phases) / sizeof(fault_phases[0]); i++) {
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", fault_phases[i], 1);
        cbm_zova_workspace_generation_result_t failed = {0};
        ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &failed), -1);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
                 "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
                 result.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
        ASSERT_EQ(count, 2);
        snprintf(sql, sizeof(sql),
                 "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",
                 result.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
        ASSERT_EQ(count, 1);
        ASSERT_EQ(zova_scalar_int64(
                      path,
                      "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND "
                      "name='cbm_nodes_v1_fts_ai'",
                      &count),
                  0);
        ASSERT_EQ(count, 1);
        ASSERT_EQ(zova_scalar_int64(path, "SELECT count(*) FROM cbm_nodes_fts_v1_docsize",
                                    &count), 0);
        ASSERT_EQ(count, 2);
    }
    const CBMDumpNode reordered_nodes[] = {nodes[1], nodes[0]};
    const CBMDumpEdge reordered_edges[] = {edges[0]};
    const CBMDumpVector reordered_vectors[] = {vectors[1], vectors[0]};
    const CBMDumpTokenVec reordered_tokens[] = {tokens[1], tokens[0]};
    const cbm_zova_file_hash_input_t reordered_hashes[] = {hashes[1], hashes[0]};
    cbm_zova_workspace_generation_input_t reordered_input = input;
    reordered_input.nodes = reordered_nodes;
    reordered_input.edges = reordered_edges;
    reordered_input.node_vectors = reordered_vectors;
    reordered_input.token_vectors = reordered_tokens;
    reordered_input.file_hashes = reordered_hashes;
    cbm_zova_workspace_generation_result_t replacement = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &reordered_input, &replacement), 0);
    ASSERT_EQ(replacement.generation, 2);
    ASSERT_STR_EQ(replacement.metadata_sha256, result.metadata_sha256);
    ASSERT_STR_EQ(replacement.fts_sha256, result.fts_sha256);
    ASSERT_STR_EQ(replacement.topology_sha256, result.topology_sha256);
    ASSERT_STR_EQ(replacement.node_vector_sha256, result.node_vector_sha256);
    ASSERT_STR_EQ(replacement.token_vector_sha256, result.token_vector_sha256);

    /* A semantic property byte must affect only the metadata digest, while a
     * raw vector byte must affect only the corresponding vector digest. */
    CBMDumpNode changed_nodes[2];
    memcpy(changed_nodes, nodes, sizeof(changed_nodes));
    changed_nodes[1].properties = "{\"risk\":2}";
    int8_t changed_leaf_values[] = {1, 126};
    CBMDumpVector changed_vectors[2];
    memcpy(changed_vectors, vectors, sizeof(changed_vectors));
    changed_vectors[1].vector = (const uint8_t *)changed_leaf_values;
    cbm_zova_workspace_generation_input_t changed_input = input;
    changed_input.nodes = changed_nodes;
    changed_input.node_vectors = changed_vectors;
    cbm_zova_workspace_generation_result_t changed = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &changed_input, &changed), 0);
    ASSERT_EQ(changed.generation, 3);
    ASSERT_TRUE(strcmp(changed.metadata_sha256, replacement.metadata_sha256) != 0);
    ASSERT_STR_EQ(changed.fts_sha256, replacement.fts_sha256);
    ASSERT_STR_EQ(changed.topology_sha256, replacement.topology_sha256);
    ASSERT_TRUE(strcmp(changed.node_vector_sha256, replacement.node_vector_sha256) != 0);
    ASSERT_STR_EQ(changed.token_vector_sha256, replacement.token_vector_sha256);
    cbm_unlink(path);
    PASS();
}

TEST(zova_full_edge_publication_batches_128_rows) {
    enum { NODE_COUNT = 130, EDGE_COUNT = 129 };
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-edge-batch-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    CBMDumpNode nodes[NODE_COUNT];
    CBMDumpEdge edges[EDGE_COUNT];
    char names[NODE_COUNT][32];
    char qualified_names[NODE_COUNT][48];
    char file_paths[NODE_COUNT][32];
    memset(nodes, 0, sizeof(nodes));
    memset(edges, 0, sizeof(edges));
    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(names[i], sizeof(names[i]), "Node%d", i);
        snprintf(qualified_names[i], sizeof(qualified_names[i]), "edge_batch.Node%d", i);
        snprintf(file_paths[i], sizeof(file_paths[i]), "node_%d.c", i);
        nodes[i] = (CBMDumpNode){
            .id = i + 1,
            .project = "edge_batch",
            .label = "Function",
            .name = names[i],
            .qualified_name = qualified_names[i],
            .file_path = file_paths[i],
            .start_line = 1,
            .end_line = 1,
            .properties = "{}",
        };
    }
    for (int i = 0; i < EDGE_COUNT; i++) {
        edges[i] = (CBMDumpEdge){
            .id = i + 1,
            .project = "edge_batch",
            .source_id = i + 1,
            .target_id = i + 2,
            .type = "CALLS",
            .properties = "{}",
            .url_path = "",
            .local_name = "",
        };
    }
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/edge-batch",
        .project = "edge_batch",
        .indexed_at = "2026-07-17T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = NODE_COUNT,
        .edges = edges,
        .edge_count = EDGE_COUNT,
    };
    cbm_zova_workspace_generation_result_t result = {0};
    cbm_zova_publish_test_metrics_reset();
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &result), 0);
    cbm_zova_publish_test_metrics_t metrics = {0};
    cbm_zova_publish_test_metrics_get(&metrics);
    ASSERT_EQ(result.metadata_edges, EDGE_COUNT);
    ASSERT_EQ(result.graph_edges, EDGE_COUNT);
    ASSERT_EQ(metrics.canonical_edges_sql.rows, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.bind_i64_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.bind_text_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.step_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.reset_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.clear_bindings_calls, 0);
    cbm_unlink(path);
    PASS();
}

TEST(zova_workspace_vector_search_uses_native_node_and_token_collections) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-native-vector-search-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);

    int8_t root_vec[768] = {0};
    int8_t leaf_vec[768] = {0};
    root_vec[0] = 127;
    leaf_vec[1] = 127;
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "native", .label = "Function", .name = "Root",
         .qualified_name = "native.Root", .file_path = "root.c", .properties = "{}"},
        {.id = 2, .project = "native", .label = "Function", .name = "Leaf",
         .qualified_name = "native.Leaf", .file_path = "leaf.c", .properties = "{}"},
        {.id = 3, .project = "native", .label = "Function", .name = "Twin",
         .qualified_name = "native.ZTwin", .file_path = "twin.c", .properties = "{}"},
    };
    const CBMDumpVector node_vectors[] = {
        {.node_id = 1, .project = "native", .vector = (const uint8_t *)root_vec,
         .vector_len = sizeof(root_vec)},
        {.node_id = 2, .project = "native", .vector = (const uint8_t *)leaf_vec,
         .vector_len = sizeof(leaf_vec)},
        {.node_id = 3, .project = "native", .vector = (const uint8_t *)root_vec,
         .vector_len = sizeof(root_vec)},
    };
    const CBMDumpTokenVec token_vectors[] = {
        {.id = 1, .project = "native", .token = "root", .vector = (const uint8_t *)root_vec,
         .vector_len = sizeof(root_vec), .idf = 1.0f},
        {.id = 2, .project = "native", .token = "leaf", .vector = (const uint8_t *)leaf_vec,
         .vector_len = sizeof(leaf_vec), .idf = 1.0f},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/native-vector-search",
        .project = "native",
        .indexed_at = "2026-07-15T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 768,
        .nodes = nodes,
        .node_count = 3,
        .node_vectors = node_vectors,
        .node_vector_count = 3,
        .token_vectors = token_vectors,
        .token_vector_count = 2,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &published), 0);

    int64_t forbidden = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name IN "
                  "('cbm_node_vectors_compat_v1','cbm_token_vectors_compat_v1')",
                  &forbidden),
              0);
    ASSERT_EQ(forbidden, 0);

    int64_t root_node_key = 0;
    int64_t root_token_key = 0;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT zova_node_key FROM cbm_nodes_v1 WHERE qualified_name='native.Root'",
                  &root_node_key),
              0);
    ASSERT_GT(root_node_key, 0);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT token_key FROM cbm_token_vector_metadata_v1 WHERE token='root'",
                  &root_token_key),
              0);
    ASSERT_GT(root_token_key, 0);
    zova_database *native_db = NULL;
    zova_message native_error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = path, .out_db = &native_db, .out_error_message = &native_error}),
              ZOVA_OK);
    zova_message_free(&native_error);
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    char token_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  published.workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768,
                  node_collection, sizeof(node_collection)),
              0);
    ASSERT_EQ(cbm_zova_workspace_token_vector_collection_name(
                  published.workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768,
                  token_collection, sizeof(token_collection)),
              0);
    char physical_id[32];
    snprintf(physical_id, sizeof(physical_id), "%lld", (long long)root_node_key);
    uint8_t exists = 0;
    ASSERT_EQ(zova_vector_exists(&(zova_vector_exists_request){
                  .db = native_db, .collection_name = node_collection,
                  .vector_id = physical_id, .out_exists = &exists}),
              ZOVA_OK);
    ASSERT_EQ(exists, 1);
    snprintf(physical_id, sizeof(physical_id), "%lld", (long long)root_token_key);
    exists = 0;
    ASSERT_EQ(zova_vector_exists(&(zova_vector_exists_request){
                  .db = native_db, .collection_name = token_collection,
                  .vector_id = physical_id, .out_exists = &exists}),
              ZOVA_OK);
    ASSERT_EQ(exists, 1);
    zova_database_close(native_db);

    cbm_zova_workspace_snapshot_t public_snapshot = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(
                  path, published.workspace_id, &public_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(public_snapshot.node_vector_count, 3);
    ASSERT_EQ(public_snapshot.token_vector_count, 2);
    for (int i = 0; i < public_snapshot.node_vector_count; i++)
        ASSERT_TRUE(strncmp(public_snapshot.node_vector_ids[i], "n:v2:", 5) == 0);
    for (int i = 0; i < public_snapshot.token_vector_count; i++)
        ASSERT_TRUE(strncmp(public_snapshot.token_vector_ids[i], "t:v1:", 5) == 0);
    cbm_zova_workspace_snapshot_free(&public_snapshot);

    cbm_store_t *store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);
    const char *keywords[] = {"root"};
    cbm_vector_result_t *results = NULL;
    int result_count = 0;
    cbm_vector_search_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 3, &results,
                                         &result_count, &metrics),
              CBM_STORE_OK);
    ASSERT_EQ(result_count, 3);
    ASSERT_STR_EQ(results[0].qualified_name, "native.Root");
    ASSERT_STR_EQ(results[0].name, "Root");
    ASSERT_STR_EQ(results[0].file_path, "root.c");
    ASSERT_STR_EQ(results[0].label, "Function");
    ASSERT_STR_EQ(results[1].qualified_name, "native.ZTwin");
    ASSERT_EQ(results[0].score, results[1].score);
    ASSERT_TRUE(results[1].score > results[2].score);
    ASSERT_TRUE(metrics.used_zova);
    cbm_store_free_vector_results(results, result_count);

    results = NULL;
    result_count = 0;
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 1, &results,
                                         &result_count, &metrics), CBM_STORE_OK);
    ASSERT_EQ(result_count, 1);
    ASSERT_STR_EQ(results[0].qualified_name, "native.Root");
    cbm_store_free_vector_results(results, result_count);

    const char *multi_keywords[] = {"root", "leaf"};
    results = NULL;
    result_count = 0;
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", multi_keywords, 2, 3, &results,
                                         &result_count, &metrics), CBM_STORE_OK);
    ASSERT_EQ(result_count, 3);
    ASSERT_TRUE(metrics.used_zova);
    ASSERT_TRUE(metrics.zova_native_multi_query);
    cbm_store_free_vector_results(results, result_count);
    cbm_store_close(store);

    sqlite3 *sql_db = NULL;
    ASSERT_EQ(sqlite3_open(path, &sql_db), SQLITE_OK);
    sqlite3_stmt *tamper = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(sql_db,
              "UPDATE cbm_workspace_index_state_v1 SET model_fingerprint=?1 "
              "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
              "WHERE workspace_id=?2)",
              -1, &tamper, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 1, "missing_model", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 2, published.workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(tamper), SQLITE_DONE);
    sqlite3_finalize(tamper);
    sqlite3_close(sql_db);
    store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);
    results = NULL;
    result_count = 0;
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 3, &results,
                                         &result_count, NULL), CBM_STORE_ERR);
    ASSERT_NULL(results);
    cbm_store_close(store);

    ASSERT_EQ(sqlite3_open(path, &sql_db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(sql_db,
              "UPDATE cbm_workspace_index_state_v1 SET model_fingerprint=?1,generation=999 "
              "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
              "WHERE workspace_id=?2)", -1, &tamper, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 1, CBM_ZOVA_MODEL_FINGERPRINT, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 2, published.workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(tamper), SQLITE_DONE);
    sqlite3_finalize(tamper);
    sqlite3_close(sql_db);
    store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 3, &results,
                                         &result_count, &metrics), CBM_STORE_ERR);
    ASSERT_TRUE(metrics.generation_mismatch);
    cbm_store_close(store);

    ASSERT_EQ(sqlite3_open(path, &sql_db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(sql_db,
              "UPDATE cbm_workspace_index_state_v1 SET generation=?1 "
              "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
              "WHERE workspace_id=?2)",
              -1, &tamper, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int64(tamper, 1, published.generation), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 2, published.workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(tamper), SQLITE_DONE);
    sqlite3_finalize(tamper);
    sqlite3_close(sql_db);

    const CBMDumpNode empty_nodes[] = {{
        .id = 1, .project = "empty", .label = "Function", .name = "Only",
        .qualified_name = "empty.Only", .file_path = "only.c", .properties = "{}"}};
    const CBMDumpTokenVec empty_tokens[] = {{
        .id = 1, .project = "empty", .token = "root", .vector = (const uint8_t *)root_vec,
        .vector_len = sizeof(root_vec), .idf = 1.0f}};
    cbm_zova_workspace_generation_input_t empty_input = input;
    empty_input.root_path = "/tmp/native-vector-search-empty";
    empty_input.project = "empty";
    empty_input.nodes = empty_nodes;
    empty_input.node_count = 1;
    empty_input.node_vectors = NULL;
    empty_input.node_vector_count = 0;
    empty_input.token_vectors = empty_tokens;
    empty_input.token_vector_count = 1;
    cbm_zova_workspace_generation_result_t empty_published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &empty_input, &empty_published), 0);
    store = cbm_store_open_zova_workspace_query(path, empty_published.workspace_id);
    ASSERT_NOT_NULL(store);
    results = NULL;
    result_count = -1;
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "empty", keywords, 1, 3, &results,
                                         &result_count, &metrics), CBM_STORE_OK);
    ASSERT_EQ(result_count, 0);
    ASSERT_NULL(results);
    ASSERT_TRUE(metrics.used_zova);
    cbm_store_close(store);
    cbm_unlink(path);
    PASS();
}

/* Exercise the public native-object APIs inside a caller-owned transaction.
 * This deliberately uses a separate reader handle and only public graph,
 * vector, and SQL operations; it must not depend on Zova private tables. */
static int zova_tx_exec(zova_database *db, const char *sql) {
    zova_database_exec_request req = {.db = db, .sql = sql};
    zova_status status = zova_database_exec(&req);
    if (status != ZOVA_OK) {
        fprintf(stderr, "zova tx exec failed status=%s sql=%s err=%s\n", zova_status_name(status),
                sql, zova_database_last_error_message(db));
    }
    return status == ZOVA_OK ? 0 : -1;
}

static int zova_tx_scalar_i64(zova_database *db, const char *sql, int64_t *out) {
    if (!db || !sql || !out) return -1;
    zova_statement *stmt = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = db, .sql = sql, .out_statement = &stmt}) != ZOVA_OK || !stmt) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int rc = zova_statement_step(&(zova_statement_step_request){
        .statement = stmt, .out_result = &step}) == ZOVA_OK && step == ZOVA_STEP_ROW
                 ? 0 : -1;
    if (rc == 0) {
        rc = zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 0, .out_value = out}) == ZOVA_OK ? 0 : -1;
    }
    (void)zova_statement_finalize(stmt);
    return rc;
}

static int zova_tx_graph_count(zova_database *db, const char *graph_name, uint64_t expected) {
    zova_graph_info info = {0};
    zova_status status = zova_graph_info_get(&(zova_graph_info_get_request){
        .db = db, .name = graph_name, .out_info = &info});
    int rc = status == ZOVA_OK && info.node_count == expected ? 0 : -1;
    zova_graph_info_free(&info);
    return rc;
}

static int zova_tx_vector_count(zova_database *db, const char *collection, uint64_t expected) {
    zova_vector_collection_info info = {0};
    zova_status status = zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
        .db = db, .name = collection, .out_info = &info});
    int rc = status == ZOVA_OK && info.vector_count == expected ? 0 : -1;
    zova_vector_collection_info_free(&info);
    return rc;
}

static int zova_tx_commit_checked(zova_database *db, const char *who) {
    zova_status status = zova_database_commit(&(zova_database_simple_request){.db = db});
    if (status != ZOVA_OK) {
        fprintf(stderr, "zova tx commit failed who=%s status=%s err=%s\n", who,
                zova_status_name(status), zova_database_last_error_message(db));
    }
    return status == ZOVA_OK ? 0 : -1;
}

static int zova_tx_graph_is(zova_database *db, const char *expected) {
    zova_graph_neighbor_results results = {0};
    zova_graph_neighbors_request req = {
        .db = db,
        .graph_name = "tx_graph",
        .node_id = "A",
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = "CALLS",
        .limit = 8,
        .out_results = &results,
    };
    zova_status status = zova_graph_neighbors(&req);
    int rc = status == ZOVA_OK && results.len == 1 && results.items &&
                     results.items[0].node_id && strcmp(results.items[0].node_id, expected) == 0
                 ? 0
                 : -1;
    if (rc != 0) {
        fprintf(stderr, "zova tx graph mismatch expected=%s status=%s len=%zu\n", expected,
                zova_status_name(status), results.len);
    }
    zova_graph_neighbor_results_free(&results);
    return rc;
}

static int zova_tx_vector_is(zova_database *db, const char *expected) {
    const int8_t query_values[] = {127, 0};
    zova_vector_values query = {
        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
        .i8_values = query_values,
        .values_len = 2,
    };
    zova_vector_search_results results = {0};
    zova_vector_search_request req = {
        .db = db,
        .collection_name = "tx_vectors",
        .query = query,
        .limit = 8,
        .out_results = &results,
    };
    zova_status status = zova_vector_search(&req);
    int rc = status == ZOVA_OK && results.len == 1 && results.items &&
                     results.items[0].id && strcmp(results.items[0].id, expected) == 0
                 ? 0
                 : -1;
    if (rc != 0) {
        fprintf(stderr, "zova tx vector mismatch expected=%s status=%s len=%zu\n", expected,
                zova_status_name(status), results.len);
    }
    zova_vector_search_results_free(&results);
    return rc;
}

static int zova_tx_fts_is(zova_database *db, const char *expected) {
    zova_statement *stmt = NULL;
    zova_database_prepare_request prepare = {
        .db = db,
        .sql = "SELECT rowid FROM tx_fts WHERE tx_fts MATCH ?1 LIMIT 1",
        .out_statement = &stmt,
    };
    zova_status prepare_status = zova_database_prepare(&prepare);
    int rc = prepare_status == ZOVA_OK && stmt ? 0 : -1;
    if (rc == 0) {
        zova_statement_bind_text_request bind = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)expected,
            .len = strlen(expected),
        };
        rc = zova_statement_bind_text(&bind) == ZOVA_OK ? 0 : -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        rc = zova_statement_step(&step_req) == ZOVA_OK && step == ZOVA_STEP_ROW ? 0 : -1;
    }
    if (rc == 0) {
        int64_t rowid = 0;
        zova_statement_column_int64_request column = {
            .statement = stmt,
            .index = 0,
            .out_value = &rowid,
        };
        rc = zova_statement_column_int64(&column) == ZOVA_OK && rowid == 1 ? 0 : -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    if (rc != 0) {
        fprintf(stderr, "zova tx fts mismatch expected=%s prepare=%s\n", expected,
                zova_status_name(prepare_status));
    }
    return rc;
}

static int zova_tx_seed(zova_database *db, const char *node_id, const char *term) {
    if (zova_database_begin_immediate(&(zova_database_simple_request){.db = db}) != ZOVA_OK) {
        return -1;
    }
    int rc = 0;
    zova_graph_create_request graph_create = {.db = db, .name = "tx_graph"};
    if (zova_graph_create(&graph_create) != ZOVA_OK) {
        rc = -1;
    }
    const zova_graph_node_input nodes[] = {
        {.graph_name = "tx_graph", .node_id = "A", .kind = "Function"},
        {.graph_name = "tx_graph", .node_id = node_id, .kind = "Function"},
    };
    if (rc == 0) {
        zova_graph_node_put_many_request put_nodes = {
            .db = db,
            .nodes = nodes,
            .nodes_len = sizeof(nodes) / sizeof(nodes[0]),
        };
        rc = zova_graph_node_put_many(&put_nodes) == ZOVA_OK ? 0 : -1;
    }
    const zova_graph_edge_input edges[] = {
        {.graph_name = "tx_graph", .from_node_id = "A", .edge_type = "CALLS", .to_node_id = node_id},
    };
    if (rc == 0) {
        zova_graph_edge_put_many_request put_edges = {
            .db = db,
            .edges = edges,
            .edges_len = sizeof(edges) / sizeof(edges[0]),
        };
        rc = zova_graph_edge_put_many(&put_edges) == ZOVA_OK ? 0 : -1;
    }
    zova_vector_collection_create_request create_collection = {
        .db = db,
        .name = "tx_vectors",
        .options = {
            .dimensions = 2,
            .metric = ZOVA_VECTOR_METRIC_COSINE,
            .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
        },
    };
    if (rc == 0) {
        rc = zova_vector_collection_create(&create_collection) == ZOVA_OK ? 0 : -1;
    }
    const int8_t vector_values[] = {127, 0};
    const zova_vector_input vectors[] = {
        {.id = node_id,
         .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = vector_values,
                    .values_len = 2}},
    };
    if (rc == 0) {
        zova_vector_put_many_request put_vectors = {
            .db = db,
            .collection_name = "tx_vectors",
            .vectors = vectors,
            .vectors_len = sizeof(vectors) / sizeof(vectors[0]),
        };
        rc = zova_vector_put_many(&put_vectors) == ZOVA_OK ? 0 : -1;
    }
    if (rc == 0) {
        char sql[256];
        if (snprintf(sql, sizeof(sql),
                     "CREATE VIRTUAL TABLE tx_fts USING fts5(term, content='');"
                     "INSERT INTO tx_fts(rowid,term) VALUES(1,'%s');",
                     term) >= (int)sizeof(sql) || zova_tx_exec(db, sql) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        rc = zova_database_commit(&(zova_database_simple_request){.db = db}) == ZOVA_OK ? 0 : -1;
    } else {
        (void)zova_database_rollback(&(zova_database_simple_request){.db = db});
    }
    return rc;
}

TEST(zova_graph_vector_objects_obey_caller_transaction) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-tx-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    zova_database *writer = NULL;
    zova_database *reader = NULL;
    zova_message error = {0};
    int rc = -1;
    zova_database_open_request create = {
        .path = path,
        .out_db = &writer,
        .out_error_message = &error,
    };
    if (zova_database_create(&create) != ZOVA_OK || !writer) {
        goto done;
    }
    zova_message_free(&error);
    error = (zova_message){0};
    if (zova_tx_exec(writer, "PRAGMA journal_mode=WAL") != 0) {
        goto done;
    }
    if (zova_tx_seed(writer, "Old", "oldterm") != 0) {
        goto done;
    }
    zova_database_open_request open_reader = {
        .path = path,
        .out_db = &reader,
        .out_error_message = &error,
    };
    if (zova_database_open(&open_reader) != ZOVA_OK || !reader) {
        goto done;
    }
    zova_message_free(&error);
    error = (zova_message){0};

    if (zova_database_exec(&(zova_database_exec_request){.db = reader, .sql = "BEGIN"}) != ZOVA_OK ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0 ||
        zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}) != ZOVA_OK) {
        goto done;
    }
    /* The replacement helper starts its own transaction; perform the same
     * native replacement inline here because this phase is already inside the
     * caller-owned writer transaction. */
    if (zova_graph_delete(&(zova_graph_delete_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK ||
        zova_graph_create(&(zova_graph_create_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK) {
        goto done_writer;
    }
    const zova_graph_node_input new_nodes[] = {
        {.graph_name = "tx_graph", .node_id = "A", .kind = "Function"},
        {.graph_name = "tx_graph", .node_id = "New", .kind = "Function"},
    };
    const zova_graph_edge_input new_edges[] = {
        {.graph_name = "tx_graph", .from_node_id = "A", .edge_type = "CALLS", .to_node_id = "New"},
    };
    const int8_t new_vector_values[] = {127, 0};
    const zova_vector_input new_vectors[] = {
        {.id = "New",
         .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = new_vector_values,
                    .values_len = 2}},
    };
    if (zova_graph_node_put_many(&(zova_graph_node_put_many_request){
                                     .db = writer,
                                     .nodes = new_nodes,
                                     .nodes_len = sizeof(new_nodes) / sizeof(new_nodes[0]),
                                 }) != ZOVA_OK ||
        zova_graph_edge_put_many(&(zova_graph_edge_put_many_request){
                                     .db = writer,
                                     .edges = new_edges,
                                     .edges_len = sizeof(new_edges) / sizeof(new_edges[0]),
                                 }) != ZOVA_OK ||
        zova_vector_collection_delete(&(zova_vector_collection_delete_request){
            .db = writer,
            .name = "tx_vectors",
        }) != ZOVA_OK ||
        zova_vector_collection_create(&(zova_vector_collection_create_request){
            .db = writer,
            .name = "tx_vectors",
            .options = {.dimensions = 2,
                        .metric = ZOVA_VECTOR_METRIC_COSINE,
                        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8},
        }) != ZOVA_OK ||
        zova_vector_put_many(&(zova_vector_put_many_request){
            .db = writer,
            .collection_name = "tx_vectors",
            .vectors = new_vectors,
            .vectors_len = sizeof(new_vectors) / sizeof(new_vectors[0]),
        }) != ZOVA_OK ||
        zova_tx_exec(writer, "DROP TABLE tx_fts") != 0 ||
        zova_tx_exec(writer,
                     "CREATE VIRTUAL TABLE tx_fts USING fts5(term, content='');"
                     "INSERT INTO tx_fts(rowid,term) VALUES(1,'newterm');") != 0) {
        goto done_writer;
    }
    /* The reader's open transaction must continue to expose the old snapshot. */
    if (zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0) {
        goto done_writer;
    }
    if (zova_database_rollback(&(zova_database_simple_request){.db = writer}) != ZOVA_OK ||
        zova_tx_commit_checked(reader, "reader-after-rollback") != 0) {
        goto done;
    }
    /* Reopen after rollback so the post-rollback assertion is made from a
     * fresh reader snapshot, not merely the prior transaction handle. */
    (void)zova_database_close(reader);
    reader = NULL;
    zova_message_free(&error);
    error = (zova_message){0};
    if (zova_database_open(&open_reader) != ZOVA_OK || !reader ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0) {
        goto done;
    }

    if (zova_database_exec(&(zova_database_exec_request){.db = reader, .sql = "BEGIN"}) != ZOVA_OK ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0 ||
        zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}) != ZOVA_OK) {
        goto done;
    }
    if (zova_graph_delete(&(zova_graph_delete_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK ||
        zova_graph_create(&(zova_graph_create_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK ||
        zova_graph_node_put_many(&(zova_graph_node_put_many_request){
            .db = writer,
            .nodes = new_nodes,
            .nodes_len = sizeof(new_nodes) / sizeof(new_nodes[0]),
        }) != ZOVA_OK ||
        zova_graph_edge_put_many(&(zova_graph_edge_put_many_request){
            .db = writer,
            .edges = new_edges,
            .edges_len = sizeof(new_edges) / sizeof(new_edges[0]),
        }) != ZOVA_OK ||
        zova_vector_collection_delete(&(zova_vector_collection_delete_request){
            .db = writer,
            .name = "tx_vectors",
        }) != ZOVA_OK ||
        zova_vector_collection_create(&(zova_vector_collection_create_request){
            .db = writer,
            .name = "tx_vectors",
            .options = {.dimensions = 2,
                        .metric = ZOVA_VECTOR_METRIC_COSINE,
                        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8},
        }) != ZOVA_OK ||
        zova_vector_put_many(&(zova_vector_put_many_request){
            .db = writer,
            .collection_name = "tx_vectors",
            .vectors = new_vectors,
            .vectors_len = sizeof(new_vectors) / sizeof(new_vectors[0]),
        }) != ZOVA_OK ||
        zova_tx_exec(writer, "DROP TABLE tx_fts") != 0 ||
        zova_tx_exec(writer,
                     "CREATE VIRTUAL TABLE tx_fts USING fts5(term, content='');"
                     "INSERT INTO tx_fts(rowid,term) VALUES(1,'newterm');") != 0 ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0 ||
        zova_tx_commit_checked(writer, "writer-replacement") != 0 ||
        zova_tx_commit_checked(reader, "reader-replacement") != 0) {
        goto done_writer;
    }
    /* A new reader snapshot observes the committed generation atomically. */
    zova_database_close(reader);
    reader = NULL;
    if (zova_database_open(&open_reader) != ZOVA_OK || !reader || zova_tx_graph_is(reader, "New") != 0 ||
        zova_tx_vector_is(reader, "New") != 0 || zova_tx_fts_is(reader, "newterm") != 0) {
        goto done;
    }
    rc = 0;
    goto done;

done_writer:
    (void)zova_database_rollback(&(zova_database_simple_request){.db = writer});
done:
    zova_message_free(&error);
    if (reader) {
        (void)zova_database_close(reader);
    }
    if (writer) {
        (void)zova_database_close(writer);
    }
    cbm_unlink(path);
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(zova_atomic_publisher_reader_snapshot_is_generation_consistent) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-reader-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    const int8_t root_values[] = {127, 0};
    const int8_t leaf_values[] = {0, 127};
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "reader", .label = "Function", .name = "Root",
         .qualified_name = "reader.Root", .file_path = "root.c", .properties = "{}"},
        {.id = 2, .project = "reader", .label = "Function", .name = "Leaf",
         .qualified_name = "reader.Leaf", .file_path = "leaf.c", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "reader", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    const CBMDumpVector vectors[] = {
        {.node_id = 1, .project = "reader", .vector = (const uint8_t *)root_values,
         .vector_len = 2},
        {.node_id = 2, .project = "reader", .vector = (const uint8_t *)leaf_values,
         .vector_len = 2},
    };
    const CBMDumpTokenVec tokens[] = {
        {.id = 1, .project = "reader", .token = "root",
         .vector = (const uint8_t *)root_values, .vector_len = 2, .idf = 1.0f},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/reader-snapshot",
        .project = "reader",
        .indexed_at = "2026-07-12T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = 2,
        .edges = edges,
        .edge_count = 1,
        .node_vectors = vectors,
        .node_vector_count = 2,
        .token_vectors = tokens,
        .token_vector_count = 1,
    };
    cbm_zova_workspace_generation_result_t first = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &first), 0);

    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_graph_name(first.workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  first.workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 2,
                  node_collection, sizeof(node_collection)), 0);

    const CBMDumpNode replacement_nodes[] = {nodes[0]};
    const CBMDumpVector replacement_vectors[] = {vectors[0]};
    cbm_zova_workspace_generation_input_t replacement_input = input;
    replacement_input.nodes = replacement_nodes;
    replacement_input.node_count = 1;
    replacement_input.edges = NULL;
    replacement_input.edge_count = 0;
    replacement_input.node_vectors = replacement_vectors;
    replacement_input.node_vector_count = 1;
    replacement_input.token_vectors = NULL;
    replacement_input.token_vector_count = 0;

    zova_database *writer = NULL;
    zova_database *reader = NULL;
    zova_message writer_error = {0};
    zova_message reader_error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
        .path = path, .out_db = &writer, .out_error_message = &writer_error}), ZOVA_OK);
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
        .path = path, .out_db = &reader, .out_error_message = &reader_error}), ZOVA_OK);
    zova_message_free(&writer_error);
    zova_message_free(&reader_error);
    ASSERT_EQ(zova_tx_exec(reader, "BEGIN"), 0);
    int64_t count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1_docsize");
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_graph_count(reader, graph_name, 2), 0);
    ASSERT_EQ(zova_tx_vector_count(reader, node_collection, 2), 0);

    ASSERT_EQ(zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}), ZOVA_OK);
    cbm_zova_workspace_generation_result_t second = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace_tx(writer, &replacement_input, &second), 0);
    ASSERT_EQ(second.generation, 2);
    ASSERT_EQ(zova_tx_scalar_i64(writer, "PRAGMA cache_size", &count), 0);
    ASSERT_EQ(count, -65536);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(zova_tx_graph_count(reader, graph_name, 2), 0);
    ASSERT_EQ(zova_tx_vector_count(reader, node_collection, 2), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1_docsize");
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);

    ASSERT_EQ(zova_tx_commit_checked(writer, "reader_snapshot_writer"), 0);
    /* The already-open reader transaction keeps generation 1 even after the
     * writer commits. */
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(zova_tx_commit_checked(reader, "reader_snapshot_reader"), 0);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 1);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1_docsize");
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(zova_tx_graph_count(reader, graph_name, 1), 0);
    ASSERT_EQ(zova_tx_vector_count(reader, node_collection, 1), 0);

    (void)zova_database_close(reader);
    (void)zova_database_close(writer);
    cbm_unlink(path);
    PASS();
}

static int delta_test_node_index(const cbm_zova_workspace_snapshot_t *snapshot,
                                 const char *stable_id) {
    for (int i = 0; i < snapshot->node_count; i++)
        if (strcmp(snapshot->node_stable_ids[i], stable_id) == 0) return i;
    return -1;
}

static int delta_test_snapshot_from_model(const cbm_zova_publish_model_t *model,
                                          cbm_zova_workspace_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = 1;
    snapshot->integrity = *cbm_zova_publish_model_digests(model);
    snapshot->hydrated_components = CBM_ZOVA_SNAPSHOT_COMPONENT_ALL;
    snapshot->node_count = cbm_zova_publish_model_node_count(model);
    snapshot->edge_count = cbm_zova_publish_model_edge_count(model);
    snapshot->node_vector_count = cbm_zova_publish_model_node_vector_count(model);
    snapshot->token_vector_count = cbm_zova_publish_model_token_vector_count(model);
    snapshot->file_hash_count = cbm_zova_publish_model_file_hash_count(model);
#define DELTA_TEST_ALLOC(field, count) \
    do { \
        snapshot->field = (count) ? calloc((size_t)(count), sizeof(*snapshot->field)) : NULL; \
        if ((count) && !snapshot->field) return -1; \
    } while (0)
    DELTA_TEST_ALLOC(nodes, snapshot->node_count);
    DELTA_TEST_ALLOC(node_stable_ids, snapshot->node_count);
    DELTA_TEST_ALLOC(node_source_ordinals, snapshot->node_count);
    DELTA_TEST_ALLOC(edges, snapshot->edge_count);
    DELTA_TEST_ALLOC(edge_ids, snapshot->edge_count);
    snapshot->topology_edge_count = cbm_zova_publish_model_topology_count(model);
    DELTA_TEST_ALLOC(topology_edges, snapshot->topology_edge_count);
    DELTA_TEST_ALLOC(node_vectors, snapshot->node_vector_count);
    DELTA_TEST_ALLOC(node_vector_ids, snapshot->node_vector_count);
    DELTA_TEST_ALLOC(token_vectors, snapshot->token_vector_count);
    DELTA_TEST_ALLOC(token_vector_ids, snapshot->token_vector_count);
    DELTA_TEST_ALLOC(file_hashes, snapshot->file_hash_count);
#undef DELTA_TEST_ALLOC
    for (int i = 0; i < snapshot->node_count; i++) {
        const cbm_zova_publish_node_t *node = cbm_zova_publish_model_node_at(model, i);
        snapshot->nodes[i] = *node->source;
        snapshot->nodes[i].id = i + 1;
        snapshot->node_stable_ids[i] = (char *)node->stable_id;
        snapshot->node_source_ordinals[i] = node->source_ordinal;
    }
    for (int i = 0; i < snapshot->edge_count; i++) {
        const cbm_zova_publish_edge_t *edge = cbm_zova_publish_model_edge_at(model, i);
        snapshot->edges[i] = *edge->source;
        snapshot->edges[i].id = i + 1;
        snapshot->edges[i].source_id =
            delta_test_node_index(snapshot, edge->source_stable_id) + 1;
        snapshot->edges[i].target_id =
            delta_test_node_index(snapshot, edge->target_stable_id) + 1;
        snapshot->edge_ids[i] = (char *)edge->edge_id;
    }
    for (int i = 0; i < snapshot->topology_edge_count; i++) {
        const cbm_zova_publish_edge_t *edge = cbm_zova_publish_model_topology_at(model, i);
        snapshot->topology_edges[i] = (cbm_zova_snapshot_topology_edge_t){
            .source_stable_id=(char *)edge->source_stable_id,
            .edge_type=(char *)edge->source->type,
            .target_stable_id=(char *)edge->target_stable_id};
    }
    for (int i = 0; i < snapshot->node_vector_count; i++) {
        const cbm_zova_publish_node_vector_t *vector =
            cbm_zova_publish_model_node_vector_at(model, i);
        snapshot->node_vectors[i] = *vector->source;
        snapshot->node_vectors[i].node_id =
            delta_test_node_index(snapshot, vector->stable_id) + 1;
        snapshot->node_vector_ids[i] = (char *)vector->stable_id;
    }
    for (int i = 0; i < snapshot->token_vector_count; i++) {
        const cbm_zova_publish_token_vector_t *vector =
            cbm_zova_publish_model_token_vector_at(model, i);
        snapshot->token_vectors[i] = *vector->source;
        snapshot->token_vector_ids[i] = (char *)vector->token_id;
    }
    for (int i = 0; i < snapshot->file_hash_count; i++)
        snapshot->file_hashes[i] = *cbm_zova_publish_model_file_hash_at(model, i);
    snapshot->project_summary = cbm_zova_publish_model_input(model)->project_summary;
    return 0;
}

static void delta_test_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot);

TEST(zova_delta_digest_planner_selects_only_changed_components) {
    static const uint8_t node_bytes[] = {1, 2, 3, 4};
    static const uint8_t token_bytes[] = {4, 3, 2, 1};
    CBMDumpNode nodes[] = {
        {.id=1,.project="digest-plan",.label="Function",.name="Alpha",
         .qualified_name="digest_plan.Alpha",.file_path="src/a.c",.start_line=1,
         .end_line=4,.properties="{}"},
        {.id=2,.project="digest-plan",.label="Function",.name="Beta",
         .qualified_name="digest_plan.Beta",.file_path="src/b.c",.start_line=5,
         .end_line=8,.properties="{}"},
    };
    CBMDumpEdge edge = {.id=1,.project="digest-plan",.source_id=1,.target_id=2,
        .type="CALLS",.properties="{}",.url_path="",.local_name="beta"};
    CBMDumpVector node_vector = {.node_id=1,.project="digest-plan",
        .vector=node_bytes,.vector_len=4};
    CBMDumpTokenVec token_vector = {.id=1,.project="digest-plan",.token="alpha",
        .vector=token_bytes,.vector_len=4,.idf=1.0f};
    cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/digest-plan",.project="digest-plan",
        .indexed_at="2026-07-16T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=4,.nodes=nodes,.node_count=2,.edges=&edge,.edge_count=1,
        .node_vectors=&node_vector,.node_vector_count=1,
        .token_vectors=&token_vector,.token_vector_count=1,
    };
    cbm_zova_publish_model_t *model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build("w1_11111111111111111111111111111111",
                                           &input, &model), 0);
    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(delta_test_snapshot_from_model(model, &snapshot), 0);

    cbm_zova_snapshot_components_t required = UINT32_MAX;
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);

    snapshot.integrity.topology_sha256[0] =
        snapshot.integrity.topology_sha256[0] == '0' ? '1' : '0';
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY);
    snapshot.integrity = *cbm_zova_publish_model_digests(model);

    snapshot.integrity.node_vector_sha256[0] =
        snapshot.integrity.node_vector_sha256[0] == '0' ? '1' : '0';
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS);
    snapshot.integrity = *cbm_zova_publish_model_digests(model);

    snapshot.integrity.token_vector_sha256[0] =
        snapshot.integrity.token_vector_sha256[0] == '0' ? '1' : '0';
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS);

    cbm_zova_workspace_generation_input_t no_vectors = input;
    no_vectors.node_vectors = NULL;
    no_vectors.node_vector_count = 0;
    no_vectors.token_vectors = NULL;
    no_vectors.token_vector_count = 0;
    cbm_zova_publish_model_t *reused = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build("w1_11111111111111111111111111111111",
                                           &no_vectors, &reused), 0);
    ASSERT_EQ(cbm_zova_publish_model_reuse_vector_digests(
                  reused, &snapshot.integrity,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
                      CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS), 0);
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, reused, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);
    ASSERT_EQ((int)cbm_zova_publish_model_digests(reused)->node_vectors, 1);
    ASSERT_EQ((int)cbm_zova_publish_model_digests(reused)->token_vectors, 1);
    cbm_zova_publish_model_free(reused);

    snapshot.integrity = *cbm_zova_publish_model_digests(model);
    snapshot.integrity.topology_sha256[0] = 'G';
    ASSERT_TRUE(cbm_zova_workspace_delta_required_components(&snapshot, model, &required) != 0);

    delta_test_snapshot_free(&snapshot);
    cbm_zova_publish_model_free(model);
    PASS();
}

static void delta_test_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot) {
    free(snapshot->nodes); free(snapshot->node_stable_ids); free(snapshot->node_source_ordinals);
    free(snapshot->edges); free(snapshot->edge_ids);
    free(snapshot->topology_edges);
    free(snapshot->node_vectors); free(snapshot->node_vector_ids);
    free(snapshot->token_vectors); free(snapshot->token_vector_ids);
    free(snapshot->file_hashes);
    memset(snapshot, 0, sizeof(*snapshot));
}

TEST(zova_workspace_delta_is_exact_and_deterministic) {
    static const uint8_t node_bytes[] = {1, 2, 3, 4};
    static const uint8_t changed_node_bytes[] = {1, 9, 3, 4};
    static const uint8_t token_bytes[] = {4, 3, 2, 1};
    CBMDumpNode base_nodes[] = {
        {.id=1,.project="delta",.label="Function",.name="AlphaNode",
         .qualified_name="delta.AlphaNode",.file_path="src/a.c",.start_line=1,
         .end_line=4,.properties="{\"rank\":1}"},
        {.id=2,.project="delta",.label="Function",.name="BetaNode",
         .qualified_name="delta.BetaNode",.file_path="src/b.c",.start_line=5,
         .end_line=8,.properties="{\"rank\":2}"},
    };
    CBMDumpEdge base_edge = {
        .id=1,.project="delta",.source_id=1,.target_id=2,.type="CALLS",
        .properties="{\"line\":2}",.url_path="",.local_name="beta"};
    CBMDumpVector base_vector = {
        .node_id=1,.project="delta",.vector=node_bytes,.vector_len=4};
    CBMDumpTokenVec base_token = {
        .id=1,.project="delta",.token="alpha",.vector=token_bytes,.vector_len=4,.idf=1.5f};
    cbm_zova_file_hash_input_t base_hash = {
        .file_path="src/a.c",.content_hash="one",.mtime_ns=1,.size_bytes=10};
    cbm_zova_workspace_generation_input_t base = {
        .root_path="/tmp/delta",.project="delta",.indexed_at="2026-07-15T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=4,
        .nodes=base_nodes,.node_count=2,.edges=&base_edge,.edge_count=1,
        .node_vectors=&base_vector,.node_vector_count=1,
        .token_vectors=&base_token,.token_vector_count=1,
        .file_hashes=&base_hash,.file_hash_count=1,
        .project_summary={.present=true,.summary="before",.source_hash="one",
                          .created_at="created",.updated_at="before"},
    };
    const char *workspace_id = "w1_11111111111111111111111111111111";
    cbm_zova_publish_model_t *before_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &base, &before_model), 0);
    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(delta_test_snapshot_from_model(before_model, &snapshot), 0);

    cbm_zova_workspace_delta_t *delta = NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, before_model, &delta), 0);
    cbm_zova_workspace_delta_metrics_t metrics = {0};
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.node_inserts + metrics.node_updates + metrics.node_deletes, 0);
    ASSERT_EQ(metrics.edge_inserts + metrics.edge_deletes, 0);
    ASSERT_EQ(metrics.node_vector_upserts + metrics.node_vector_deletes, 0);
    ASSERT_EQ(metrics.token_vector_upserts + metrics.token_vector_deletes, 0);
    ASSERT_EQ(metrics.file_hash_upserts + metrics.file_hash_deletes, 0);
    ASSERT_FALSE(cbm_zova_workspace_delta_replaces_summary(delta));
    cbm_zova_workspace_delta_free(delta);

    snapshot.hydrated_components = CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, before_model, &delta), 0);
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.topology_inserts + metrics.topology_deletes, 0);
    ASSERT_EQ(metrics.node_vector_upserts + metrics.node_vector_deletes, 0);
    ASSERT_EQ(metrics.token_vector_upserts + metrics.token_vector_deletes, 0);
    cbm_zova_workspace_delta_free(delta);

    CBMDumpNode changed_nodes[] = {base_nodes[0], base_nodes[1]};
    changed_nodes[0].properties = "{\"rank\":9}";
    CBMDumpEdge changed_edge = base_edge;
    changed_edge.properties = "{\"line\":3}";
    CBMDumpVector changed_vector = base_vector;
    changed_vector.vector = changed_node_bytes;
    CBMDumpTokenVec changed_token = base_token;
    changed_token.idf = 2.0f;
    cbm_zova_file_hash_input_t changed_hash = base_hash;
    changed_hash.content_hash = "two";
    cbm_zova_workspace_generation_input_t changed = base;
    changed.nodes = changed_nodes;
    changed.edges = &changed_edge;
    changed.node_vectors = &changed_vector;
    changed.token_vectors = &changed_token;
    changed.file_hashes = &changed_hash;
    changed.project_summary.summary = "after";
    changed.project_summary.updated_at = "after";
    cbm_zova_publish_model_t *changed_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &changed, &changed_model), 0);
    ASSERT_TRUE(cbm_zova_workspace_delta_build(&snapshot, changed_model, &delta) != 0);
    ASSERT_NULL(delta);
    snapshot.hydrated_components = CBM_ZOVA_SNAPSHOT_COMPONENT_ALL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, changed_model, &delta), 0);
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.node_updates, 1);
    ASSERT_EQ(metrics.edge_deletes, 1);
    ASSERT_EQ(metrics.edge_inserts, 1);
    ASSERT_EQ(metrics.topology_inserts + metrics.topology_deletes, 0);
    ASSERT_EQ(metrics.node_vector_upserts, 1);
    ASSERT_EQ(metrics.token_vector_upserts, 1);
    ASSERT_EQ(metrics.file_hash_upserts, 1);
    ASSERT_EQ(metrics.summary_replacements, 1);
    ASSERT_EQ(cbm_zova_workspace_delta_node_update_at(delta, 0)->source, &changed_nodes[0]);
    ASSERT_STR_EQ(cbm_zova_workspace_delta_edge_delete_at(delta, 0)->edge_id,
                  snapshot.edge_ids[0]);
    cbm_zova_workspace_delta_free(delta);
    cbm_zova_publish_model_free(changed_model);

    cbm_zova_workspace_generation_input_t replacement = base;
    CBMDumpNode replacement_nodes[] = {base_nodes[0], base_nodes[1]};
    replacement_nodes[0].qualified_name = "delta.ReplacedAlpha";
    replacement_nodes[0].name = "ReplacedAlpha";
    replacement.nodes = replacement_nodes;
    replacement.edges = NULL;
    replacement.edge_count = 0;
    replacement.node_vectors = NULL;
    replacement.node_vector_count = 0;
    replacement.token_vectors = NULL;
    replacement.token_vector_count = 0;
    replacement.file_hashes = NULL;
    replacement.file_hash_count = 0;
    replacement.project_summary.present = false;
    cbm_zova_publish_model_t *replacement_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &replacement, &replacement_model), 0);
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, replacement_model, &delta), 0);
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.node_inserts, 1);
    ASSERT_EQ(metrics.node_deletes, 1);
    ASSERT_EQ(metrics.edge_deletes, 1);
    ASSERT_EQ(metrics.topology_deletes, 1);
    ASSERT_EQ(metrics.node_vector_deletes, 1);
    ASSERT_EQ(metrics.token_vector_deletes, 1);
    ASSERT_EQ(metrics.file_hash_deletes, 1);
    ASSERT_EQ(metrics.summary_replacements, 1);
    ASSERT_NOT_NULL(cbm_zova_workspace_delta_node_insert_at(delta, 0));
    ASSERT_NOT_NULL(cbm_zova_workspace_delta_node_delete_at(delta, 0));
    cbm_zova_workspace_delta_free(delta);

    char *original_second = snapshot.node_stable_ids[1];
    snapshot.node_stable_ids[1] = snapshot.node_stable_ids[0];
    ASSERT_TRUE(cbm_zova_workspace_delta_build(&snapshot, replacement_model, &delta) != 0);
    ASSERT_NULL(delta);
    snapshot.node_stable_ids[1] = original_second;

    cbm_zova_publish_model_free(replacement_model);
    delta_test_snapshot_free(&snapshot);
    cbm_zova_publish_model_free(before_model);
    PASS();
}

TEST(zova_publish_model_is_deterministic_and_validates_identity) {
    static const uint8_t node_vector_a[] = {1, 2, 3, 4};
    static const uint8_t node_vector_zero[] = {0, 0, 0, 0};
    static const uint8_t token_vector[] = {4, 3, 2, 1};
    CBMDumpNode nodes[] = {
        {.id=2,.project="model",.label="Function",.name="SecondNode",
         .qualified_name="model.SecondNode",.file_path="src/model.c",.start_line=20,
         .end_line=24,.properties="{\"rank\":2}"},
        {.id=1,.project="model",.label="Function",.name="FirstNode",
         .qualified_name="model.FirstNode",.file_path="src/model.c",.start_line=10,
         .end_line=14,.properties="{\"rank\":1}"},
    };
    CBMDumpEdge edges[] = {
        {.id=2,.project="model",.source_id=1,.target_id=2,.type="CALLS",
         .properties="{\"line\":12}",.url_path="",.local_name="second"},
        {.id=1,.project="model",.source_id=1,.target_id=2,.type="CALLS",
         .properties="{\"line\":11}",.url_path="",.local_name="first"},
    };
    CBMDumpVector node_vectors[] = {
        {.node_id=1,.project="model",.vector=node_vector_a,.vector_len=4},
        {.node_id=2,.project="model",.vector=node_vector_zero,.vector_len=4},
    };
    CBMDumpTokenVec token_vectors[] = {
        {.id=1,.project="model",.token="model",.vector=token_vector,.vector_len=4,.idf=1.5f},
    };
    cbm_zova_file_hash_input_t hashes[] = {
        {.file_path="src/z.c",.content_hash="z",.mtime_ns=2,.size_bytes=20},
        {.file_path="src/a.c",.content_hash="a",.mtime_ns=1,.size_bytes=10},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/model",.project="model",.indexed_at="2026-07-15T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=4,
        .nodes=nodes,.node_count=2,.edges=edges,.edge_count=2,
        .node_vectors=node_vectors,.node_vector_count=2,
        .token_vectors=token_vectors,.token_vector_count=1,
        .file_hashes=hashes,.file_hash_count=2,
    };
    const char *workspace_id = "w1_00000000000000000000000000000000";
    cbm_zova_publish_model_t *first = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &input, &first), 0);
    ASSERT_NOT_NULL(first);
    ASSERT_EQ(cbm_zova_publish_model_node_count(first), 2);
    ASSERT_EQ(cbm_zova_publish_model_edge_count(first), 2);
    ASSERT_EQ(cbm_zova_publish_model_topology_count(first), 1);
    ASSERT_EQ(cbm_zova_publish_model_node_vector_count(first), 1);
    ASSERT_EQ(cbm_zova_publish_model_token_vector_count(first), 1);
    const cbm_zova_publish_node_t *node0 = cbm_zova_publish_model_node_at(first, 0);
    const cbm_zova_publish_node_t *node1 = cbm_zova_publish_model_node_at(first, 1);
    ASSERT_NOT_NULL(node0);
    ASSERT_NOT_NULL(node1);
    ASSERT_TRUE(strcmp(node0->stable_id, node1->stable_id) < 0);
    ASSERT_NOT_NULL(strstr(cbm_zova_publish_model_fts_name_at(first, 0), " "));
    const cbm_zova_publish_edge_t *edge0 = cbm_zova_publish_model_edge_at(first, 0);
    const cbm_zova_publish_edge_t *edge1 = cbm_zova_publish_model_edge_at(first, 1);
    ASSERT_NOT_NULL(edge0);
    ASSERT_NOT_NULL(edge1);
    ASSERT_EQ(edge0->topology_ordinal, 0);
    ASSERT_EQ(edge1->topology_ordinal, 0);
    const cbm_zova_publish_edge_t *topology =
        cbm_zova_publish_model_topology_at(first, 0);
    ASSERT_NOT_NULL(topology);
    ASSERT_EQ(topology->logical_edge_count, 2);
    zova_payload_capture_t payload_records = {0};
    size_t payload_edge_count = 0;
    ASSERT_EQ(cbm_zova_edge_payload_visit(topology->payload, topology->payload_len,
                                          zova_capture_payload_record,
                                          &payload_records, &payload_edge_count), 0);
    ASSERT_EQ(payload_edge_count, 2);
    ASSERT_TRUE(zova_payload_slice_equals(payload_records.records[0].local_name,
                                          edge0->source->local_name));
    ASSERT_TRUE(zova_payload_slice_equals(payload_records.records[1].local_name,
                                          edge1->source->local_name));
    const cbm_zova_publish_node_vector_t *node_vector =
        cbm_zova_publish_model_node_vector_at(first, 0);
    ASSERT_NOT_NULL(node_vector);
    ASSERT_LT(node_vector->node_ordinal,
              (uint64_t)cbm_zova_publish_model_node_count(first));
    ASSERT_STR_EQ(cbm_zova_publish_model_node_at(first, (int)node_vector->node_ordinal)->stable_id,
                  node_vector->stable_id);
    cbm_zova_publish_model_metrics_t metrics = {0};
    cbm_zova_publish_model_metrics(first, &metrics);
    ASSERT_EQ(metrics.stable_node_id_computations, 2);
    ASSERT_EQ(metrics.edge_id_computations, 2);
    ASSERT_EQ(metrics.token_id_computations, 1);
    ASSERT_EQ(metrics.camel_split_computations, 2);
    ASSERT_EQ(metrics.endpoint_lookups, 4);
    /* Dense graph-buffer IDs are already a complete 1..node_count keyspace.
     * The publication model must resolve them directly instead of sorting a
     * second dump-ID lookup table. */
    ASSERT_EQ(metrics.global_sorts, 6);

    CBMDumpNode reversed_nodes[] = {nodes[1], nodes[0]};
    CBMDumpEdge reversed_edges[] = {edges[1], edges[0]};
    cbm_zova_file_hash_input_t reversed_hashes[] = {hashes[1], hashes[0]};
    cbm_zova_workspace_generation_input_t reversed = input;
    reversed.nodes = reversed_nodes;
    reversed.edges = reversed_edges;
    reversed.file_hashes = reversed_hashes;
    cbm_zova_publish_model_t *second = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &reversed, &second), 0);
    ASSERT_NOT_NULL(second);
    const cbm_zova_workspace_generation_result_t *first_digest =
        cbm_zova_publish_model_digests(first);
    const cbm_zova_workspace_generation_result_t *second_digest =
        cbm_zova_publish_model_digests(second);
    ASSERT_STR_EQ(first_digest->metadata_sha256,
                  "cd66fce53b3bd59a8c26b73a0ef7439721cff26734e31650d9ab249ac9e83473");
    ASSERT_STR_EQ(first_digest->fts_sha256,
                  "8f84b1e76baa8136522a66bfeef02459eb32f0c9c33e5f8bd80f3eb4eb82f82e");
    ASSERT_STR_EQ(first_digest->topology_sha256,
                  "6ac5475f393fd82ef029ffd734cb26cbf86ded48d985101bfc436d1b6d73b2b7");
    ASSERT_STR_EQ(first_digest->node_vector_sha256,
                  "9aa146ef90870e005714046363bf34b73108fe04820c430895f27cafaf274311");
    ASSERT_STR_EQ(first_digest->token_vector_sha256,
                  "90093c29d678824c6bdaa6df3ae60824edc56d07addfb9c24941b0cef3d34412");
    ASSERT_STR_EQ(first_digest->metadata_sha256, second_digest->metadata_sha256);
    ASSERT_STR_EQ(first_digest->fts_sha256, second_digest->fts_sha256);
    ASSERT_STR_EQ(first_digest->topology_sha256, second_digest->topology_sha256);
    ASSERT_STR_EQ(first_digest->node_vector_sha256, second_digest->node_vector_sha256);
    ASSERT_STR_EQ(first_digest->token_vector_sha256, second_digest->token_vector_sha256);
    ASSERT_EQ(metrics.digest_row_revisit_count, 0);
    cbm_zova_publish_model_free(second);

    cbm_zova_prepared_view_t *prepared = NULL;
    ASSERT_EQ(cbm_zova_prepared_view_build(workspace_id, &input, &prepared), 0);
    ASSERT_NOT_NULL(prepared);
    const cbm_zova_workspace_generation_result_t *prepared_digest =
        cbm_zova_publish_model_digests(prepared);
    ASSERT_STR_EQ(first_digest->metadata_sha256, prepared_digest->metadata_sha256);
    ASSERT_STR_EQ(first_digest->fts_sha256, prepared_digest->fts_sha256);
    ASSERT_STR_EQ(first_digest->topology_sha256, prepared_digest->topology_sha256);
    ASSERT_STR_EQ(first_digest->node_vector_sha256, prepared_digest->node_vector_sha256);
    ASSERT_STR_EQ(first_digest->token_vector_sha256, prepared_digest->token_vector_sha256);
    cbm_zova_prepared_view_free(prepared);

    CBMDumpNode sparse_nodes[] = {nodes[0], nodes[1]};
    sparse_nodes[0].id = 20;
    sparse_nodes[1].id = 10;
    CBMDumpEdge sparse_edge = edges[0];
    sparse_edge.source_id = 10;
    sparse_edge.target_id = 20;
    CBMDumpVector sparse_vector = node_vectors[0];
    sparse_vector.node_id = 10;
    cbm_zova_workspace_generation_input_t sparse = input;
    sparse.nodes = sparse_nodes;
    sparse.edges = &sparse_edge;
    sparse.edge_count = 1;
    sparse.node_vectors = &sparse_vector;
    sparse.node_vector_count = 1;
    cbm_zova_publish_model_t *sparse_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &sparse, &sparse_model), 0);
    ASSERT_NOT_NULL(cbm_zova_publish_model_stable_id_for_dump_id(sparse_model, 10));
    ASSERT_NOT_NULL(cbm_zova_publish_model_stable_id_for_dump_id(sparse_model, 20));
    ASSERT_NULL(cbm_zova_publish_model_stable_id_for_dump_id(sparse_model, 1));
    cbm_zova_publish_model_metrics_t sparse_metrics = {0};
    cbm_zova_publish_model_metrics(sparse_model, &sparse_metrics);
    ASSERT_EQ(sparse_metrics.global_sorts, 7);
    cbm_zova_publish_model_free(sparse_model);

    cbm_zova_publish_model_test_fail_allocation_at(-1);
    cbm_zova_publish_model_t *allocation_probe = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &input, &allocation_probe), 0);
    int64_t allocation_count = cbm_zova_publish_model_test_allocation_count();
    ASSERT_GT(allocation_count, 0);
    cbm_zova_publish_model_free(allocation_probe);
    for (int64_t allocation = 0; allocation < allocation_count; allocation++) {
        cbm_zova_publish_model_t *failed = NULL;
        cbm_zova_publish_model_test_fail_allocation_at(allocation);
        ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &input, &failed) != 0);
        ASSERT_NULL(failed);
    }
    cbm_zova_publish_model_test_fail_allocation_at(-1);

    CBMDumpNode bad_nodes[] = {nodes[0], nodes[1]};
    cbm_zova_publish_model_t *bad = NULL;
    bad_nodes[1].id = bad_nodes[0].id;
    cbm_zova_workspace_generation_input_t invalid = input;
    invalid.nodes = bad_nodes;
    ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &invalid, &bad) != 0);
    ASSERT_NULL(bad);
    bad_nodes[1] = bad_nodes[0];
    bad_nodes[1].id = 99;
    ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &invalid, &bad) != 0);
    ASSERT_NULL(bad);
    CBMDumpEdge missing = edges[0];
    missing.target_id = 999;
    invalid = input;
    invalid.edges = &missing;
    invalid.edge_count = 1;
    ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &invalid, &bad) != 0);
    ASSERT_NULL(bad);

    cbm_zova_publish_model_free(first);
    PASS();
}

TEST(zova_publish_model_dense_camel_name_is_owned_without_overflow) {
    char name[241];
    char expected[602];
    for (size_t i = 0; i < sizeof(name) - 1; i++) name[i] = i % 2 ? 'A' : 'a';
    name[sizeof(name) - 1] = '\0';
    size_t output = 0;
    memcpy(expected + output, name, sizeof(name) - 1);
    output += sizeof(name) - 1;
    expected[output++] = ' ';
    for (size_t i = 0; i < sizeof(name) - 1; i++) {
        if (i > 0 && name[i] == 'A' && name[i - 1] == 'a') expected[output++] = ' ';
        expected[output++] = name[i];
    }
    expected[output] = '\0';
    ASSERT_EQ(output, sizeof(expected) - 1);

    CBMDumpNode node = {
        .id = 1, .project = "camel", .label = "Function", .name = name,
        .qualified_name = "", .file_path = "src/camel.c", .start_line = 1,
        .end_line = 2, .properties = "{}",
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/camel", .project = "camel",
        .indexed_at = "2026-07-15T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 4,
        .nodes = &node, .node_count = 1,
    };
    cbm_zova_publish_model_t *model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build("w1_00000000000000000000000000000000",
                                           &input, &model), 0);
    ASSERT_NOT_NULL(model);
    ASSERT_STR_EQ(cbm_zova_publish_model_fts_name_at(model, 0), expected);
    cbm_zova_publish_model_free(model);
    PASS();
}

TEST(zova_publish_model_uses_bounded_row_storage_allocations) {
    enum { NODE_COUNT = 64, EDGE_COUNT = 63 };
    CBMDumpNode nodes[NODE_COUNT];
    CBMDumpEdge edges[EDGE_COUNT];
    char names[NODE_COUNT][24];
    char qualified_names[NODE_COUNT][48];
    char file_paths[NODE_COUNT][32];
    memset(nodes, 0, sizeof(nodes));
    memset(edges, 0, sizeof(edges));
    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(names[i], sizeof(names[i]), "Node%d", i);
        snprintf(qualified_names[i], sizeof(qualified_names[i]), "arena.Node%d", i);
        snprintf(file_paths[i], sizeof(file_paths[i]), "src/node_%d.c", i);
        nodes[i] = (CBMDumpNode){
            .id = i + 1,
            .project = "arena",
            .label = "Function",
            .name = names[i],
            .qualified_name = qualified_names[i],
            .file_path = file_paths[i],
            .start_line = i + 1,
            .end_line = i + 1,
            .properties = "{}",
        };
    }
    for (int i = 0; i < EDGE_COUNT; i++) {
        edges[i] = (CBMDumpEdge){
            .id = i + 1,
            .project = "arena",
            .source_id = i + 1,
            .target_id = i + 2,
            .type = "CALLS",
            .properties = "{}",
            .url_path = "",
            .local_name = "",
        };
    }
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/arena",
        .project = "arena",
        .indexed_at = "2026-07-20T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = NODE_COUNT,
        .edges = edges,
        .edge_count = EDGE_COUNT,
    };
    cbm_zova_publish_model_test_fail_allocation_at(-1);
    cbm_zova_publish_model_t *model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(
                  "w1_55555555555555555555555555555555", &input, &model),
              0);
    ASSERT_LT(cbm_zova_publish_model_test_allocation_count(), 16);
    cbm_zova_publish_model_free(model);
    PASS();
}

TEST(zova_atomic_delta_publisher_preserves_old_generation_on_every_fault) {
    char path[256];
    snprintf(path,sizeof(path),"/tmp/cbm-zova-delta-%ld.zova",(long)getpid());
    cbm_unlink(path);
    static const uint8_t vector_a[]={1,2,3,4};
    static const uint8_t vector_changed[]={4,3,2,1};
    static const uint8_t token_vector[]={2,2,2,2};
    CBMDumpNode nodes[]={
        {.id=1,.project="atomic-delta",.label="Function",.name="Alpha",
         .qualified_name="atomic_delta.Alpha",.file_path="src/a.c",.start_line=1,.end_line=3,
         .properties="{\"rank\":1}"},
        {.id=2,.project="atomic-delta",.label="Function",.name="Beta",
         .qualified_name="atomic_delta.Beta",.file_path="src/b.c",.start_line=4,.end_line=7,
         .properties="{\"rank\":2}"},
    };
    CBMDumpEdge edge={.id=1,.project="atomic-delta",.source_id=1,.target_id=2,
        .type="CALLS",.properties="{}",.url_path="",.local_name="beta"};
    CBMDumpVector vectors[]={
        {.node_id=1,.project="atomic-delta",.vector=vector_a,.vector_len=4},
        {.node_id=2,.project="atomic-delta",.vector=vector_a,.vector_len=4},
    };
    CBMDumpTokenVec token={.id=1,.project="atomic-delta",.token="alpha",
        .vector=token_vector,.vector_len=4,.idf=1.0f};
    cbm_zova_file_hash_input_t hashes[]={
        {.file_path="src/a.c",.content_hash="a1",.mtime_ns=1,.size_bytes=10},
        {.file_path="src/b.c",.content_hash="b1",.mtime_ns=1,.size_bytes=20},
    };
    cbm_zova_workspace_generation_input_t input={
        .root_path="/tmp/atomic-delta",.project="atomic-delta",
        .indexed_at="2026-07-15T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=4,.nodes=nodes,.node_count=2,.edges=&edge,.edge_count=1,
        .node_vectors=vectors,.node_vector_count=2,.token_vectors=&token,.token_vector_count=1,
        .file_hashes=hashes,.file_hash_count=2,
        .project_summary={.present=true,.summary="before",.source_hash="a1",
                          .created_at="created",.updated_at="before"},
    };
    cbm_zova_workspace_generation_result_t first={0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path,&input,&first),0);
    ASSERT_EQ(first.generation,1);
    cbm_zova_workspace_snapshot_t incremental_snapshot={0};
    ASSERT_EQ(cbm_zova_repository_export_incremental_snapshot(
                  path,first.workspace_id,&incremental_snapshot),0);
    ASSERT_EQ(incremental_snapshot.generation,first.generation);
    ASSERT_EQ(incremental_snapshot.hydrated_components,
              CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);
    ASSERT_NULL(incremental_snapshot.topology_edges);
    ASSERT_EQ(incremental_snapshot.topology_edge_count,0);
    ASSERT_NULL(incremental_snapshot.node_vectors);
    ASSERT_EQ(incremental_snapshot.node_vector_count,0);
    ASSERT_NULL(incremental_snapshot.token_vectors);
    ASSERT_EQ(incremental_snapshot.token_vector_count,0);
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(incremental_snapshot.hydrated_components,
              CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY);
    ASSERT_EQ(incremental_snapshot.topology_edge_count,1);
    ASSERT_NULL(incremental_snapshot.node_vectors);
    ASSERT_EQ(incremental_snapshot.node_vector_count,0);
    ASSERT_NULL(incremental_snapshot.token_vectors);
    ASSERT_EQ(incremental_snapshot.token_vector_count,0);
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(incremental_snapshot.node_vector_count,2);
    ASSERT_NULL(incremental_snapshot.token_vectors);
    ASSERT_EQ(incremental_snapshot.token_vector_count,0);
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(incremental_snapshot.token_vector_count,1);
    cbm_zova_snapshot_components_t hydrated_before_stale =
        incremental_snapshot.hydrated_components;
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation+1,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_ALL,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_STALE);
    ASSERT_EQ(incremental_snapshot.hydrated_components,hydrated_before_stale);
    cbm_zova_workspace_snapshot_free(&incremental_snapshot);

    cbm_zova_workspace_snapshot_t snapshot={0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.metrics.base_phase_mask, CBM_ZOVA_SNAPSHOT_BASE_PHASE_ALL);
    ASSERT_EQ(snapshot.metrics.node_rows, 2);
    ASSERT_EQ(snapshot.metrics.edge_rows, 1);
    ASSERT_EQ(snapshot.metrics.file_hash_rows, 2);
    ASSERT_TRUE(snapshot.metrics.open_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.header_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.integrity_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.nodes_sql_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.nodes_native_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.nodes_finalize_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.edges_sql_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.edges_native_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.edges_finalize_ms >= 0.0);
    ASSERT_EQ(snapshot.metrics.edge_scan_pages, 1);
    ASSERT_EQ(snapshot.metrics.edge_native_rows, 1);
    ASSERT_EQ(snapshot.metrics.edge_logical_rows, 1);
    ASSERT_EQ(snapshot.metrics.edge_keyed_read_count, 1);
    ASSERT_TRUE(snapshot.metrics.edge_string_arena_chunks >= 1);
    ASSERT_TRUE(snapshot.metrics.edge_string_arena_bytes > 0);
    ASSERT_TRUE(snapshot.edges[0].project == snapshot.project);
    ASSERT_NOT_NULL(snapshot.edge_ids[0]);
    ASSERT_TRUE(snapshot.metrics.hashes_summary_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.close_ms >= 0.0);

    /* A no-op delta cannot create an orphan file, so it must not perform a
     * workspace-wide reconciliation pass. Keep one pre-existing orphan as a
     * sentinel: the old unconditional cleanup deletes it. */
    zova_database *orphan_db=NULL;
    zova_message orphan_error={0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path=path,.out_db=&orphan_db,.out_error_message=&orphan_error}),ZOVA_OK);
    zova_message_free(&orphan_error);
    char orphan_insert_sql[1024];
    snprintf(orphan_insert_sql,sizeof(orphan_insert_sql),
             "INSERT INTO cbm_files_v1(workspace_key,file_path) "
             "SELECT workspace_key,'src/preexisting-orphan.c' FROM cbm_workspace_registry "
             "WHERE workspace_id='%s'",first.workspace_id);
    ASSERT_EQ(zova_tx_exec(orphan_db,orphan_insert_sql),0);
    ASSERT_EQ(zova_database_close(orphan_db),ZOVA_OK);

    cbm_zova_publish_model_t *unchanged_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&input,&unchanged_model),0);
    cbm_zova_workspace_delta_t *unchanged_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,unchanged_model,&unchanged_delta),0);
    cbm_zova_publish_test_metrics_reset();
    cbm_zova_workspace_generation_result_t noop={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,unchanged_model,unchanged_delta,&noop),0);
    ASSERT_EQ(noop.generation,2);
    ASSERT_EQ(noop.inserted_count+noop.updated_count+noop.deleted_count,0);
    ASSERT_EQ(noop.full_clear_count,0);
    cbm_zova_publish_test_metrics_t noop_metrics={0};
    cbm_zova_publish_test_metrics_get(&noop_metrics);
    ASSERT_EQ(noop_metrics.full_clear_count,0);
    ASSERT_EQ(noop_metrics.canonical_node_fts_passes,0);
    ASSERT_EQ(noop_metrics.canonical_edge_passes,0);
    ASSERT_EQ(noop_metrics.delta_authoritative_rows_touched,0);
    ASSERT_EQ(noop_metrics.delta_clear_violation_count,0);
    ASSERT_EQ(noop_metrics.full_fts_bulk_statements,0);
    ASSERT_EQ(noop_metrics.full_node_guard_validation_statements,0);
    ASSERT_EQ(noop_metrics.full_edge_guard_validation_statements,0);
    int64_t orphan_files_after_noop=0;
    ASSERT_EQ(zova_scalar_int64(path,
              "SELECT count(*) FROM cbm_files_v1 "
              "WHERE file_path='src/preexisting-orphan.c'",
              &orphan_files_after_noop),0);
    ASSERT_EQ(orphan_files_after_noop,1);
    cbm_zova_workspace_delta_free(unchanged_delta);
    cbm_zova_publish_model_free(unchanged_model);

    CBMDumpNode changed_node=nodes[0];
    changed_node.properties="{\"rank\":9}";
    CBMDumpVector changed_vector={.node_id=1,.project="atomic-delta",
        .vector=vector_changed,.vector_len=4};
    cbm_zova_file_hash_input_t changed_hash={.file_path="src/a.c",.content_hash="a2",
        .mtime_ns=2,.size_bytes=11};
    cbm_zova_workspace_generation_input_t changed=input;
    changed.indexed_at="2026-07-15T00:01:00Z";
    changed.nodes=&changed_node;changed.node_count=1;
    changed.edges=NULL;changed.edge_count=0;
    changed.node_vectors=&changed_vector;changed.node_vector_count=1;
    changed.token_vectors=NULL;changed.token_vector_count=0;
    changed.file_hashes=&changed_hash;changed.file_hash_count=1;
    changed.project_summary.summary="after";
    changed.project_summary.source_hash="a2";
    changed.project_summary.updated_at="after";
    cbm_zova_publish_model_t *model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&changed,&model),0);
    cbm_zova_workspace_delta_t *delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,model,&delta),0);
    cbm_zova_workspace_delta_metrics_t metrics={0};
    cbm_zova_workspace_delta_metrics(delta,&metrics);
    ASSERT_EQ(metrics.node_updates,1);
    ASSERT_EQ(metrics.node_deletes,1);
    ASSERT_EQ(metrics.edge_deletes,1);
    ASSERT_EQ(metrics.topology_deletes,1);
    ASSERT_EQ(metrics.node_vector_upserts,1);
    ASSERT_EQ(metrics.node_vector_deletes,1);
    ASSERT_EQ(metrics.token_vector_deletes,1);

    cbm_zova_workspace_generation_result_t stale={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,model,delta,&stale),-1);
    int64_t active_after_stale=0;
    char active_sql[512];
    snprintf(active_sql,sizeof(active_sql),
             "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",
             first.workspace_id);
    ASSERT_EQ(zova_scalar_int64(path,active_sql,&active_after_stale),0);
    ASSERT_EQ(active_after_stale,2);
    cbm_zova_workspace_delta_free(delta);
    delta=NULL;
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.generation,2);
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,model,&delta),0);

    const char *faults[]={
        "user_delta_after_generation","user_delta_after_vector_deletes",
        "user_delta_after_graph_deletes","user_delta_after_canonical_deletes",
        "user_delta_after_node_upserts","user_delta_after_edge_inserts",
        "user_delta_after_graph_puts","user_delta_after_vector_puts",
        "user_delta_after_metadata_upserts","user_delta_after_integrity",
        "user_delta_before_commit",
    };
    for(size_t i=0;i<sizeof(faults)/sizeof(faults[0]);i++){
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE",faults[i],1);
        cbm_zova_workspace_generation_result_t failed={0};
        ASSERT_EQ(cbm_zova_user_database_publish_delta(path,model,delta,&failed),-1);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        int64_t active=0;
        char sql[512];
        snprintf(sql,sizeof(sql),"SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",first.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path,sql,&active),0);
        ASSERT_EQ(active,2);
        snprintf(sql,sizeof(sql),"SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
                 "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
                 first.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path,sql,&active),0);
        ASSERT_EQ(active,2);
    }
    cbm_zova_publish_test_metrics_reset();
    cbm_zova_workspace_generation_result_t result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,model,delta,&result),0);
    ASSERT_EQ(result.generation,3);
    ASSERT_EQ(result.publication_mode,CBM_ZOVA_PUBLICATION_MODE_DELTA);
    ASSERT_EQ(result.full_clear_count,0);
    ASSERT_EQ(result.unchanged_rewrite_count,0);
    ASSERT_EQ(result.metadata_nodes,1);
    ASSERT_EQ(result.metadata_edges,0);
    ASSERT_EQ(result.graph_nodes,1);
    ASSERT_EQ(result.graph_edges,0);
    ASSERT_STR_EQ(result.metadata_sha256,cbm_zova_publish_model_digests(model)->metadata_sha256);
    cbm_zova_publish_test_metrics_t changed_metrics={0};
    cbm_zova_publish_test_metrics_get(&changed_metrics);
    ASSERT_EQ(changed_metrics.native_graph_fresh_calls,0);
    ASSERT_EQ(changed_metrics.native_graph_prepared_calls,0);
    ASSERT_EQ(changed_metrics.database_open_count,1);
    ASSERT_EQ(changed_metrics.database_close_count,1);
    ASSERT_EQ(changed_metrics.delta_file_key_resolutions,1);
    ASSERT_EQ(changed_metrics.delta_endpoint_key_lookups,0);
    ASSERT_EQ(zova_scalar_int64(path,
              "SELECT count(*) FROM cbm_files_v1 "
              "WHERE file_path='src/preexisting-orphan.c'",
              &orphan_files_after_noop),0);
    ASSERT_EQ(orphan_files_after_noop,0);

    cbm_zova_workspace_delta_free(delta);
    cbm_zova_publish_model_free(model);
    cbm_zova_workspace_snapshot_free(&snapshot);

    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    CBMDumpEdge shared_endpoint_edges[]={
        {.id=10,.project="atomic-delta",.source_id=1,.target_id=1,
         .type="CALLS",.properties="{\"slot\":1}",.url_path="",.local_name="alpha"},
        {.id=11,.project="atomic-delta",.source_id=1,.target_id=1,
         .type="CALLS",.properties="{\"slot\":2}",.url_path="",.local_name="alpha-second"},
    };
    cbm_zova_workspace_generation_input_t edge_changed=changed;
    edge_changed.indexed_at="2026-07-15T00:02:00Z";
    edge_changed.edges=shared_endpoint_edges;
    edge_changed.edge_count=2;
    cbm_zova_publish_model_t *edge_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&edge_changed,&edge_model),0);
    cbm_zova_workspace_delta_t *edge_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,edge_model,&edge_delta),0);
    cbm_zova_workspace_delta_metrics_t edge_metrics={0};
    cbm_zova_workspace_delta_metrics(edge_delta,&edge_metrics);
    ASSERT_EQ(edge_metrics.edge_inserts,2);
    cbm_zova_publish_test_metrics_reset();
    cbm_zova_workspace_generation_result_t edge_result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,edge_model,edge_delta,&edge_result),0);
    cbm_zova_publish_test_metrics_t edge_publish_metrics={0};
    cbm_zova_publish_test_metrics_get(&edge_publish_metrics);
    ASSERT_EQ(edge_publish_metrics.delta_endpoint_key_lookups,0);
    ASSERT_EQ(edge_publish_metrics.delta_file_key_resolutions,0);
    cbm_zova_workspace_delta_free(edge_delta);
    cbm_zova_publish_model_free(edge_model);
    cbm_zova_workspace_snapshot_free(&snapshot);

    /* Both logical edges share one authoritative Zova topology edge. */
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.edge_count,2);
    ASSERT_EQ(snapshot.topology_edge_count,1);
    ASSERT_EQ(snapshot.metrics.edge_scan_pages,1);
    ASSERT_EQ(snapshot.metrics.edge_native_rows,1);
    ASSERT_EQ(snapshot.metrics.edge_logical_rows,2);
    ASSERT_EQ(snapshot.metrics.edge_keyed_read_count,1);

    cbm_zova_workspace_generation_input_t one_logical_edge=edge_changed;
    one_logical_edge.indexed_at="2026-07-15T00:03:00Z";
    one_logical_edge.edge_count=1;
    cbm_zova_publish_model_t *one_edge_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&one_logical_edge,
                                           &one_edge_model),0);
    cbm_zova_workspace_delta_t *one_edge_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,one_edge_model,&one_edge_delta),0);
    cbm_zova_workspace_delta_metrics_t one_edge_metrics={0};
    cbm_zova_workspace_delta_metrics(one_edge_delta,&one_edge_metrics);
    ASSERT_EQ(one_edge_metrics.edge_deletes,1);
    ASSERT_EQ(one_edge_metrics.topology_deletes,0);
    cbm_zova_workspace_generation_result_t one_edge_result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,one_edge_model,one_edge_delta,
                                                    &one_edge_result),0);
    cbm_zova_workspace_delta_free(one_edge_delta);
    cbm_zova_publish_model_free(one_edge_model);
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.edge_count,1);
    ASSERT_EQ(snapshot.topology_edge_count,1);

    cbm_zova_workspace_generation_input_t no_logical_edges=one_logical_edge;
    no_logical_edges.indexed_at="2026-07-15T00:04:00Z";
    no_logical_edges.edges=NULL;
    no_logical_edges.edge_count=0;
    cbm_zova_publish_model_t *no_edge_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&no_logical_edges,
                                           &no_edge_model),0);
    cbm_zova_workspace_delta_t *no_edge_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,no_edge_model,&no_edge_delta),0);
    cbm_zova_workspace_delta_metrics_t no_edge_metrics={0};
    cbm_zova_workspace_delta_metrics(no_edge_delta,&no_edge_metrics);
    ASSERT_EQ(no_edge_metrics.edge_deletes,1);
    ASSERT_EQ(no_edge_metrics.topology_deletes,1);
    cbm_zova_workspace_generation_result_t no_edge_result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,no_edge_model,no_edge_delta,
                                                    &no_edge_result),0);
    cbm_zova_workspace_delta_free(no_edge_delta);
    cbm_zova_publish_model_free(no_edge_model);
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.edge_count,0);
    ASSERT_EQ(snapshot.topology_edge_count,0);
    cbm_zova_workspace_snapshot_free(&snapshot);
    cbm_unlink(path);
    PASS();
}

#endif

SUITE(zova) {
    RUN_TEST(zova_edge_payload_codec_is_compact_deterministic_and_strict);
#ifndef _WIN32
    RUN_TEST(zova_writer_gate_serializes_processes_and_releases_after_crash);
#endif
    RUN_TEST(zova_route_requires_flag_and_respects_off);
    RUN_TEST(zova_route_uses_user_cache_database);
    RUN_TEST(zova_mode_parser);
    RUN_TEST(zova_workspace_scoped_names_and_node_ids_are_deterministic);
    RUN_TEST(zova_workspace_registry_path_uses_cache_dir);
#if !CBM_WITH_ZOVA
    RUN_TEST(zova_disabled_request_fails_clearly);
#else
    RUN_TEST(zova_workspace_id_validation_rejects_missing_and_malformed_values);
    RUN_TEST(zova_repository_requires_ready_workspace_and_reads_metadata);
    RUN_TEST(zova_container_sidecar_preserves_app_sql);
    RUN_TEST(zova_i8_vector_mirror_and_prefetch);
    RUN_TEST(zova_i8_direct_vectors_do_not_read_sqlite_vector_rows);
    RUN_TEST(zova_default_authority_writes_and_reads_direct_vectors);
    RUN_TEST(zova_authority_rollback_uses_sqlite);
    RUN_TEST(zova_workspace_registry_preserves_ready_generation);
    RUN_TEST(zova_workspace_registry_lookup_is_read_only);
    RUN_TEST(zova_sidecar_generation_matches_source_and_ready_workspace);
    RUN_TEST(zova_sidecar_schema_and_integrity_record_matches_generation);
    RUN_TEST(zova_sidecar_failure_falls_back_until_a_ready_rebuild);
    RUN_TEST(zova_direct_sidecar_fault_phases_preserve_ready_read_boundary);
    RUN_TEST(zova_i8_vector_search_in_uses_candidate_ids);
    RUN_TEST(zova_i8_vector_prefetch_uses_full_search_only_for_complete_candidate_sets);
    RUN_TEST(zova_i8_vector_prefetch_reports_full_search_stage_metrics);
    RUN_TEST(zova_i8_vector_session_reuses_open_database);
    RUN_TEST(zova_i8_vector_search_matches_current_topk);
    RUN_TEST(zova_i8_native_multi_query_matches_sqlite_topk);
    RUN_TEST(zova_i8_vector_benchmark_smoke_report);
    RUN_TEST(zova_sqlite_schema_inventory_has_migration_coverage);
    RUN_TEST(zova_user_database_bootstrap_creates_versioned_workspace_schema);
    RUN_TEST(zova_user_database_schema_uses_zova_as_sole_topology_authority);
    RUN_TEST(zova_database_format_status_requires_atomic_repack_for_v5_v7);
    RUN_TEST(zova_user_database_has_one_canonical_fts_without_rowmap);
    RUN_TEST(zova_user_database_generation_state_commits_and_rolls_back);
    RUN_TEST(zova_sql_capability_probe_covers_workspace_metadata_and_sql_surface);
    RUN_TEST(zova_graph_vector_objects_obey_caller_transaction);
    RUN_TEST(zova_atomic_publisher_reader_snapshot_is_generation_consistent);
    RUN_TEST(zova_user_database_capability_probe_covers_empty_schema_surface);
    RUN_TEST(zova_user_database_imports_workspace_metadata_and_fts);
    RUN_TEST(zova_user_database_delete_workspace_is_isolated_and_idempotent);
    RUN_TEST(zova_atomic_workspace_publisher_commits_complete_generation);
    RUN_TEST(zova_full_edge_publication_batches_128_rows);
    RUN_TEST(zova_workspace_vector_search_uses_native_node_and_token_collections);
    RUN_TEST(zova_graph_mirror_neighbors);
    RUN_TEST(zova_workspace_graph_mirror_is_project_scoped_and_matches_callers);
    RUN_TEST(zova_graph_session_hydrates_scoped_directional_walk);
    RUN_TEST(zova_direct_workspace_graph_uses_finalized_dump_arrays);
    RUN_TEST(zova_workspace_graph_ingestion_benchmark_compares_equivalent_topology);
    RUN_TEST(zova_direct_workspace_lifecycle_isolates_replace_rollback_and_delete);
    RUN_TEST(zova_workspace_node_vectors_isolate_replace_rollback_and_delete);
    RUN_TEST(zova_workspace_token_vectors_isolate_replace_rollback_and_delete);
    RUN_TEST(zova_gbuf_sidecar_uses_retained_finalized_topology);
    RUN_TEST(zova_gbuf_prepare_owns_full_publication_view);
    RUN_TEST(zova_publish_model_is_deterministic_and_validates_identity);
    RUN_TEST(zova_workspace_delta_is_exact_and_deterministic);
    RUN_TEST(zova_delta_digest_planner_selects_only_changed_components);
    RUN_TEST(zova_atomic_delta_publisher_preserves_old_generation_on_every_fault);
    RUN_TEST(zova_publish_model_dense_camel_name_is_owned_without_overflow);
    RUN_TEST(zova_publish_model_uses_bounded_row_storage_allocations);
#endif
}
