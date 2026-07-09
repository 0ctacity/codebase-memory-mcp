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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { TZC_PATH_MAX = 512 };

typedef struct {
    char db_path[TZC_PATH_MAX];
    char zova_path[TZC_PATH_MAX];
} c_sql_fixture_t;

static int c_sql_fixture_create(c_sql_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    snprintf(fx->db_path, sizeof(fx->db_path), "%s/cbm_zova_c_sql_%d_%p.db", cbm_tmpdir(),
             (int)getpid(), (void *)fx);
    ASSERT_EQ(cbm_zova_sidecar_path(fx->db_path, fx->zova_path, sizeof(fx->zova_path)), 0);
    cbm_unlink(fx->db_path);
    cbm_unlink(fx->zova_path);

    cbm_store_t *store = cbm_store_open_path(fx->db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "proj", "/tmp/proj"), CBM_STORE_OK);
    cbm_node_t node = {
        .project = "proj",
        .label = "Function",
        .name = "updateCloudClient",
        .qualified_name = "proj.updateCloudClient",
        .file_path = "src/client.c",
    };
    ASSERT_GT(cbm_store_upsert_node(store, &node), 0);
    cbm_store_close(store);

    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx->db_path), 0);
    cbm_unsetenv("CBM_ZOVA_MODE");
    return 0;
}

static void c_sql_fixture_cleanup(c_sql_fixture_t *fx) {
    cbm_unlink(fx->db_path);
    cbm_unlink(fx->zova_path);
    char tmp[TZC_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.zova", fx->zova_path);
    cbm_unlink(tmp);
}

#if !CBM_WITH_ZOVA

TEST(zova_c_sql_functions_disabled) {
    ASSERT_FALSE(cbm_zova_build_enabled());
    PASS();
}

#else

static int open_registered_zova(const char *path, zova_database **out_db) {
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

static int prepare_sql(zova_database *db, const char *sql, zova_statement **out) {
    zova_database_prepare_request req = {.db = db, .sql = sql, .out_statement = out};
    return zova_database_prepare(&req) == ZOVA_OK ? 0 : -1;
}

static int step_row(zova_statement *stmt) {
    zova_step_result step = ZOVA_STEP_DONE;
    zova_statement_step_request req = {.statement = stmt, .out_result = &step};
    return zova_statement_step(&req) == ZOVA_OK && step == ZOVA_STEP_ROW ? 0 : -1;
}

static int query_cosine(zova_database *db, const int8_t *a, size_t a_len, const int8_t *b,
                        size_t b_len, double *out) {
    zova_statement *stmt = NULL;
    if (prepare_sql(db, "select cbm_cosine_i8(?1, ?2)", &stmt) != 0) {
        return -1;
    }
    zova_statement_bind_blob_request areq = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)a,
        .len = a_len,
    };
    zova_statement_bind_blob_request breq = {
        .statement = stmt,
        .index = 2,
        .data = (const uint8_t *)b,
        .len = b_len,
    };
    int rc = zova_statement_bind_blob(&areq) == ZOVA_OK &&
                     zova_statement_bind_blob(&breq) == ZOVA_OK && step_row(stmt) == 0
                 ? 0
                 : -1;
    if (rc == 0) {
        zova_statement_column_double_request dreq = {.statement = stmt, .index = 0, .out_value = out};
        rc = zova_statement_column_double(&dreq) == ZOVA_OK ? 0 : -1;
    }
    zova_statement_finalize(stmt);
    return rc;
}

static int query_text_one_arg(zova_database *db, const char *sql, const char *arg, char *out,
                              size_t out_len) {
    zova_statement *stmt = NULL;
    if (prepare_sql(db, sql, &stmt) != 0 || out_len == 0) {
        return -1;
    }
    zova_statement_bind_text_request bind = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)arg,
        .len = strlen(arg),
    };
    int rc = zova_statement_bind_text(&bind) == ZOVA_OK && step_row(stmt) == 0 ? 0 : -1;
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

static int query_regex(zova_database *db, const char *sql, const char *pattern, const char *text,
                       int64_t *out) {
    zova_statement *stmt = NULL;
    if (prepare_sql(db, sql, &stmt) != 0) {
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
                     zova_statement_bind_text(&treq) == ZOVA_OK && step_row(stmt) == 0
                 ? 0
                 : -1;
    if (rc == 0) {
        zova_statement_column_int64_request col = {.statement = stmt, .index = 0, .out_value = out};
        rc = zova_statement_column_int64(&col) == ZOVA_OK ? 0 : -1;
    }
    zova_statement_finalize(stmt);
    return rc;
}

TEST(zova_c_sql_functions_match_cbm_semantics) {
    c_sql_fixture_t fx;
    ASSERT_EQ(c_sql_fixture_create(&fx), 0);
    zova_database *db = NULL;
    ASSERT_EQ(open_registered_zova(fx.zova_path, &db), 0);

    const int8_t a[] = {1, 0};
    const int8_t b[] = {1, 0};
    const int8_t c[] = {0, 1};
    const int8_t z[] = {0, 0};
    double score = -1.0;
    ASSERT_EQ(query_cosine(db, a, sizeof(a), b, sizeof(b), &score), 0);
    ASSERT_FLOAT_EQ(score, 1.0, 0.000001);
    ASSERT_EQ(query_cosine(db, a, sizeof(a), c, sizeof(c), &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);
    ASSERT_EQ(query_cosine(db, a, sizeof(a), z, sizeof(z), &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);
    ASSERT_EQ(query_cosine(db, a, sizeof(a), b, 1, &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);
    ASSERT_EQ(query_cosine(db, a, 0, b, 0, &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);

    char out[256];
    ASSERT_EQ(query_text_one_arg(db, "select cbm_camel_split(?1)", "updateCloudClient", out,
                                 sizeof(out)),
              0);
    ASSERT_STR_EQ(out, "updateCloudClient update Cloud Client");
    ASSERT_EQ(query_text_one_arg(db, "select cbm_camel_split(?1)", "XMLParser", out,
                                 sizeof(out)),
              0);
    ASSERT_STR_EQ(out, "XMLParser XML Parser");
    ASSERT_EQ(query_text_one_arg(db, "select cbm_camel_split(?1)", "", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "");

    int64_t matched = -1;
    ASSERT_EQ(query_regex(db, "select regexp(?1, ?2)", "^Foo", "FooBar", &matched), 0);
    ASSERT_EQ(matched, 1);
    ASSERT_EQ(query_regex(db, "select regexp(?1, ?2)", "^foo", "FooBar", &matched), 0);
    ASSERT_EQ(matched, 0);
    ASSERT_EQ(query_regex(db, "select iregexp(?1, ?2)", "^foo", "FooBar", &matched), 0);
    ASSERT_EQ(matched, 1);

    zova_database_close(db);
    c_sql_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_c_sql_invalid_regex_returns_sql_error) {
    c_sql_fixture_t fx;
    ASSERT_EQ(c_sql_fixture_create(&fx), 0);
    zova_database *db = NULL;
    ASSERT_EQ(open_registered_zova(fx.zova_path, &db), 0);

    zova_statement *stmt = NULL;
    ASSERT_EQ(prepare_sql(db, "select regexp(?1, ?2)", &stmt), 0);
    zova_statement_bind_text_request preq = {
        .statement = stmt,
        .index = 1,
        .data = (const uint8_t *)"[",
        .len = 1,
    };
    zova_statement_bind_text_request treq = {
        .statement = stmt,
        .index = 2,
        .data = (const uint8_t *)"text",
        .len = 4,
    };
    ASSERT_EQ(zova_statement_bind_text(&preq), ZOVA_OK);
    ASSERT_EQ(zova_statement_bind_text(&treq), ZOVA_OK);
    zova_step_result step = ZOVA_STEP_DONE;
    zova_statement_step_request sreq = {.statement = stmt, .out_result = &step};
    ASSERT_NEQ(zova_statement_step(&sreq), ZOVA_OK);
    ASSERT_NOT_NULL(strstr(zova_database_last_error_message(db), "invalid regex"));
    zova_statement_finalize(stmt);
    zova_database_close(db);
    c_sql_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_c_sql_compatibility_query_uses_app_tables) {
    c_sql_fixture_t fx;
    ASSERT_EQ(c_sql_fixture_create(&fx), 0);
    zova_database *db = NULL;
    ASSERT_EQ(open_registered_zova(fx.zova_path, &db), 0);

    char out[256];
    ASSERT_EQ(query_text_one_arg(db,
                                 "select cbm_camel_split(name) from nodes "
                                 "where regexp(?1, name) limit 1",
                                 "^update", out, sizeof(out)),
              0);
    ASSERT_STR_EQ(out, "updateCloudClient update Cloud Client");

    zova_database_close(db);
    c_sql_fixture_cleanup(&fx);
    PASS();
}

#endif

SUITE(zova_c_sql_functions) {
#if !CBM_WITH_ZOVA
    RUN_TEST(zova_c_sql_functions_disabled);
#else
    RUN_TEST(zova_c_sql_functions_match_cbm_semantics);
    RUN_TEST(zova_c_sql_invalid_regex_returns_sql_error);
    RUN_TEST(zova_c_sql_compatibility_query_uses_app_tables);
#endif
}
