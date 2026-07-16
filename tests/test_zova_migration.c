#include "test_framework.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_legacy_snapshot.h"
#include "zova/cbm_zova_migration.h"
#include "zova/cbm_zova_route.h"
#include "zova/cbm_zova_v5_snapshot.h"
#include "zova/cbm_zova_operations.h"
#include "../internal/cbm/sqlite_writer.h"

#include <sqlite3.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

enum { MIGRATION_PATH_MAX = 512 };
enum { MIGRATION_VECTOR_DIM = 8 };

static int migration_count_substring(const char *text, const char *needle) {
    int count = 0;
    size_t length = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += length;
    }
    return count;
}

static const char *migration_tmpdir(void) {
#ifndef _WIN32
    static char canonical[MIGRATION_PATH_MAX];
    if (!canonical[0] && !realpath(cbm_tmpdir(), canonical)) return cbm_tmpdir();
    return canonical;
#else
    return cbm_tmpdir();
#endif
}

static void migration_path(char *out, size_t out_size, const char *name) {
    snprintf(out, out_size, "%s/cbm-zova-migration-%s-%d.zova", migration_tmpdir(), name,
             (int)getpid());
    cbm_unlink(out);
    char related[MIGRATION_PATH_MAX + 16];
    snprintf(related, sizeof(related), "%s-wal", out);
    cbm_unlink(related);
    snprintf(related, sizeof(related), "%s-shm", out);
    cbm_unlink(related);
}

static int migration_sql(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    int sqlite_rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (sqlite_rc != SQLITE_OK)
        fprintf(stderr, "migration fixture SQL failed: %s\n", sqlite3_errmsg(db));
    int rc = sqlite_rc == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int64_t migration_scalar(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int64_t value = -1;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

static int migration_publish_v5_workspace(const char *path, const char *root,
                                          const char *project, int seed) {
    uint8_t node_vector[MIGRATION_VECTOR_DIM] = {0};
    uint8_t token_vector[MIGRATION_VECTOR_DIM] = {0};
    node_vector[seed % MIGRATION_VECTOR_DIM] = (uint8_t)(seed + 1);
    token_vector[(seed + 1) % MIGRATION_VECTOR_DIM] = (uint8_t)(seed + 2);
    CBMDumpNode nodes[] = {
        {.id = 1, .project = project, .label = "Function", .name = "Alpha",
         .qualified_name = "pkg.Alpha", .file_path = "src/a.c", .start_line = 1,
         .end_line = 4, .properties = "{\"role\":\"source\"}"},
        {.id = 2, .project = project, .label = "Function", .name = "Beta",
         .qualified_name = "pkg.Beta", .file_path = "src/b.c", .start_line = 5,
         .end_line = 9, .properties = "{\"role\":\"target\"}"},
    };
    CBMDumpEdge edges[] = {
        {.id = 1, .project = project, .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{\"weight\":1}", .url_path = "", .local_name = ""},
    };
    CBMDumpVector node_vectors[] = {
        {.node_id = 1, .project = project, .vector = node_vector,
         .vector_len = MIGRATION_VECTOR_DIM},
    };
    CBMDumpTokenVec token_vectors[] = {
        {.id = 1, .project = project, .token = "alpha", .vector = token_vector,
         .vector_len = MIGRATION_VECTOR_DIM, .idf = 1.5f},
    };
    cbm_zova_file_hash_input_t hashes[] = {
        {.file_path = "src/a.c", .content_hash = "hash-a", .mtime_ns = seed,
         .size_bytes = 42 + seed},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = root,
        .project = project,
        .indexed_at = "2026-07-15T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = MIGRATION_VECTOR_DIM,
        .nodes = nodes,
        .node_count = 2,
        .edges = edges,
        .edge_count = 1,
        .node_vectors = node_vectors,
        .node_vector_count = 1,
        .token_vectors = token_vectors,
        .token_vector_count = 1,
        .file_hashes = hashes,
        .file_hash_count = 1,
        .project_summary = {.present = true,
                            .summary = "{\"summary\":\"fixture\"}",
                            .source_hash = "summary-hash",
                            .created_at = "2026-07-15T00:00:00Z",
                            .updated_at = "2026-07-15T00:00:01Z"},
    };
    cbm_zova_workspace_generation_result_t result = {0};
    return cbm_zova_user_database_publish_workspace(path, &input, &result);
}

/* Reconstruct only the historical objects consumed by the raw v5/v7 reader.
 * Current publication must never create these compatibility representations. */
static int migration_finalize_v5_v7_fixture(const char *path) {
    return migration_sql(
        path,
        "CREATE TABLE cbm_node_vectors_compat_v1("
        "workspace_id TEXT NOT NULL,node_id TEXT NOT NULL,vector BLOB NOT NULL,"
        "PRIMARY KEY(workspace_id,node_id)) WITHOUT ROWID;"
        "CREATE TABLE cbm_token_vectors_compat_v1("
        "workspace_id TEXT NOT NULL,token_id INTEGER NOT NULL,token TEXT NOT NULL,"
        "vector BLOB NOT NULL,idf REAL NOT NULL,PRIMARY KEY(workspace_id,token_id)) WITHOUT ROWID;"
        "INSERT INTO cbm_node_vectors_compat_v1(workspace_id,node_id,vector) "
        "SELECT s.workspace_id,v.vector_id,v.\"values\" FROM cbm_workspace_index_state_v1 s "
        "JOIN _zova_vector_collections c ON c.name=printf('cbm_nodes_i8_%s_%s_d%d',"
        "s.workspace_id,s.model_fingerprint,s.vector_dimensions) "
        "JOIN _zova_vectors v ON v.collection_key=c.collection_key;"
        "INSERT INTO cbm_token_vectors_compat_v1(workspace_id,token_id,token,vector,idf) "
        "SELECT m.workspace_id,row_number() OVER(PARTITION BY m.workspace_id ORDER BY m.token_id),"
        "m.token,v.\"values\",m.idf "
        "FROM cbm_token_vector_metadata_v1 m JOIN _zova_vectors v ON v.vector_id=m.token_id "
        "JOIN _zova_vector_collections c ON c.collection_key=v.collection_key "
        "JOIN cbm_workspace_index_state_v1 s ON s.workspace_id=m.workspace_id "
        "AND c.name=printf('cbm_tokens_i8_%s_%s_d%d',s.workspace_id,s.model_fingerprint,"
        "s.vector_dimensions);"
        "CREATE TABLE cbm_fts_rowmap_v1("
        "workspace_id TEXT NOT NULL,fts_rowid INTEGER NOT NULL,node_id TEXT NOT NULL,"
        "PRIMARY KEY(workspace_id,fts_rowid),UNIQUE(workspace_id,node_id),"
        "FOREIGN KEY(workspace_id,node_id) REFERENCES cbm_nodes_v1(workspace_id,node_id)) "
        "WITHOUT ROWID;"
        "INSERT INTO cbm_fts_rowmap_v1(workspace_id,fts_rowid,node_id) "
        "SELECT workspace_id,row_number() OVER(PARTITION BY workspace_id ORDER BY rowid),node_id "
        "FROM cbm_nodes_fts_v1;"
        "UPDATE cbm_database_schema_v1 SET schema_version=5 WHERE id=1;"
        "UPDATE _zova_meta SET value='7' WHERE key='format_version'");
}

/* Deterministic two-workspace source fixture reused by the Task 14 atomic
 * replacement tests. */
static int migration_generate_v5_v7_fixture(const char *path) {
    if (migration_publish_v5_workspace(path, "/tmp/cbm-v5-a", "project-a", 1) != 0 ||
        migration_publish_v5_workspace(path, "/tmp/cbm-v5-b", "project-b", 2) != 0)
        return -1;
    return migration_finalize_v5_v7_fixture(path);
}

#if CBM_WITH_ZOVA
#include "zova.h"

typedef struct {
    char db_path[MIGRATION_PATH_MAX];
    char zova_path[MIGRATION_PATH_MAX];
    char root_path[MIGRATION_PATH_MAX];
    int8_t alpha[MIGRATION_VECTOR_DIM];
    int8_t beta[MIGRATION_VECTOR_DIM];
} migration_legacy_fixture_t;

typedef struct {
    int exists;
    off_t size;
    time_t mtime;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
} migration_artifact_stat_t;

static migration_artifact_stat_t migration_artifact_stat(const char *path) {
    struct stat st;
    migration_artifact_stat_t result = {0};
    if (stat(path, &st) == 0) {
        result.exists = 1;
        result.size = st.st_size;
        result.mtime = st.st_mtime;
        FILE *file = fopen(path, "rb");
        if (file) {
            cbm_sha256_ctx hash;
            cbm_sha256_init(&hash);
            uint8_t buffer[4096];
            size_t read_len = 0;
            while ((read_len = fread(buffer, 1, sizeof(buffer), file)) > 0)
                cbm_sha256_update(&hash, buffer, read_len);
            cbm_sha256_final(&hash, result.digest);
            fclose(file);
        }
    }
    return result;
}

static int migration_artifact_stat_matches(const char *path,
                                           migration_artifact_stat_t expected) {
    migration_artifact_stat_t actual = migration_artifact_stat(path);
    return actual.exists == expected.exists && actual.size == expected.size &&
                   memcmp(actual.digest, expected.digest, sizeof(actual.digest)) == 0
               ? 0
               : -1;
}

static int migration_companion_bytes_match(const char *path,
                                           migration_artifact_stat_t expected) {
    migration_artifact_stat_t actual = migration_artifact_stat(path);
    if (!expected.exists) return !actual.exists || actual.size == 0 ? 0 : -1;
    return actual.exists && actual.size == expected.size &&
                   memcmp(actual.digest, expected.digest, sizeof(actual.digest)) == 0
               ? 0
               : -1;
}

static int migration_shm_stat_matches(const char *path, migration_artifact_stat_t expected) {
    migration_artifact_stat_t actual = migration_artifact_stat(path);
    if (!expected.exists) {
        return !actual.exists || (actual.size > 0 && actual.size % 32768 == 0) ? 0 : -1;
    }
    return actual.exists && actual.size == expected.size ? 0 : -1;
}

static void migration_fixture_cleanup(migration_legacy_fixture_t *fixture) {
    cbm_unlink(fixture->db_path);
    cbm_unlink(fixture->zova_path);
}

static int migration_legacy_node_id(const char *workspace_id, const CBMDumpNode *node,
                                    char *out, size_t out_size) {
    char normalized_path[MIGRATION_PATH_MAX];
    int path_len = snprintf(normalized_path, sizeof(normalized_path), "%s", node->file_path);
    if (path_len < 0 || path_len >= (int)sizeof(normalized_path)) return -1;
    cbm_normalize_path_sep(normalized_path);

    char discriminator[64];
    int discriminator_len =
        node->qualified_name[0]
            ? snprintf(discriminator, sizeof(discriminator), "named")
            : snprintf(discriminator, sizeof(discriminator), "anon:%d:%d", node->start_line,
                       node->end_line);
    if (discriminator_len < 0 || discriminator_len >= (int)sizeof(discriminator)) return -1;
    return cbm_zova_workspace_node_id_v1(workspace_id, node->label, normalized_path,
                                         node->qualified_name, discriminator, out, out_size);
}

/* The current sidecar publisher intentionally omits compatibility projections.
 * Legacy migration fixtures must synthesize the exact historical v1 surfaces
 * consumed by cbm_zova_legacy_snapshot_open(). */
static int migration_synthesize_legacy_projections(const migration_legacy_fixture_t *fixture,
                                                   const CBMDumpNode *nodes, int node_count,
                                                   const CBMDumpEdge *edges, int edge_count) {
    char registry_path[MIGRATION_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)) != 0 ||
        cbm_zova_workspace_lookup_at(registry_path, fixture->root_path, workspace_id,
                                     sizeof(workspace_id)) != 0) {
        return -1;
    }

    sqlite3 *db = NULL;
    sqlite3_stmt *node_stmt = NULL;
    sqlite3_stmt *edge_stmt = NULL;
    int rc = sqlite3_open(fixture->zova_path, &db) == SQLITE_OK ? 0 : -1;
    const char *ddl =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS cbm_zova_trace_nodes_v1("
        "workspace_id TEXT NOT NULL,node_id TEXT NOT NULL,project TEXT NOT NULL,"
        "name TEXT NOT NULL,qualified_name TEXT NOT NULL,file_path TEXT NOT NULL,"
        "label TEXT NOT NULL,start_line INTEGER NOT NULL,end_line INTEGER NOT NULL,"
        "properties TEXT NOT NULL DEFAULT '{}',"
        "PRIMARY KEY(workspace_id,node_id)) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS cbm_zova_edge_metadata_v1("
        "workspace_id TEXT NOT NULL,edge_id TEXT NOT NULL,source_node_id TEXT NOT NULL,"
        "target_node_id TEXT NOT NULL,edge_type TEXT NOT NULL,"
        "properties TEXT NOT NULL DEFAULT '{}',url_path TEXT NOT NULL DEFAULT '',"
        "local_name TEXT NOT NULL DEFAULT '',"
        "PRIMARY KEY(workspace_id,edge_id)) WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS cbm_zova_edge_metadata_source_v1 "
        "ON cbm_zova_edge_metadata_v1(workspace_id,source_node_id,edge_type);"
        "DELETE FROM cbm_zova_trace_nodes_v1;"
        "DELETE FROM cbm_zova_edge_metadata_v1;";
    if (rc == 0 && sqlite3_exec(db, ddl, NULL, NULL, NULL) != SQLITE_OK) rc = -1;
    if (rc == 0 &&
        sqlite3_prepare_v2(
            db,
            "INSERT INTO cbm_zova_trace_nodes_v1("
            "workspace_id,node_id,project,name,qualified_name,file_path,label,start_line,"
            "end_line,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
            -1, &node_stmt, NULL) != SQLITE_OK)
        rc = -1;
    for (int i = 0; rc == 0 && i < node_count; i++) {
        char node_id[CBM_ZOVA_DIGEST_HEX_SIZE + 16];
        if (migration_legacy_node_id(workspace_id, &nodes[i], node_id, sizeof(node_id)) != 0 ||
            sqlite3_bind_text(node_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 2, node_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 3, nodes[i].project, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 4, nodes[i].name, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 5, nodes[i].qualified_name, -1, SQLITE_STATIC) !=
                SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 6, nodes[i].file_path, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 7, nodes[i].label, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(node_stmt, 8, nodes[i].start_line) != SQLITE_OK ||
            sqlite3_bind_int(node_stmt, 9, nodes[i].end_line) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 10, nodes[i].properties, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(node_stmt) != SQLITE_DONE) {
            rc = -1;
        }
        if (sqlite3_reset(node_stmt) != SQLITE_OK || sqlite3_clear_bindings(node_stmt) != SQLITE_OK)
            rc = -1;
    }
    if (rc == 0 &&
        sqlite3_prepare_v2(
            db,
            "INSERT INTO cbm_zova_edge_metadata_v1("
            "workspace_id,edge_id,source_node_id,target_node_id,edge_type,properties,url_path,"
            "local_name) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
            -1, &edge_stmt, NULL) != SQLITE_OK)
        rc = -1;
    for (int i = 0; rc == 0 && i < edge_count; i++) {
        const CBMDumpNode *source = NULL;
        const CBMDumpNode *target = NULL;
        for (int n = 0; n < node_count; n++) {
            if (nodes[n].id == edges[i].source_id) source = &nodes[n];
            if (nodes[n].id == edges[i].target_id) target = &nodes[n];
        }
        char source_id[CBM_ZOVA_DIGEST_HEX_SIZE + 16];
        char target_id[CBM_ZOVA_DIGEST_HEX_SIZE + 16];
        char edge_id[64];
        int edge_id_len = snprintf(edge_id, sizeof(edge_id), "migration-edge-%lld",
                                   (long long)edges[i].id);
        if (!source || !target || edge_id_len < 0 || edge_id_len >= (int)sizeof(edge_id) ||
            migration_legacy_node_id(workspace_id, source, source_id, sizeof(source_id)) != 0 ||
            migration_legacy_node_id(workspace_id, target, target_id, sizeof(target_id)) != 0 ||
            sqlite3_bind_text(edge_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 2, edge_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 3, source_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 4, target_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 5, edges[i].type, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 6, edges[i].properties, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 7, edges[i].url_path, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(edge_stmt, 8, edges[i].local_name, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(edge_stmt) != SQLITE_DONE) {
            rc = -1;
        }
        if (sqlite3_reset(edge_stmt) != SQLITE_OK || sqlite3_clear_bindings(edge_stmt) != SQLITE_OK)
            rc = -1;
    }
    sqlite3_finalize(node_stmt);
    sqlite3_finalize(edge_stmt);
    if (db) {
        if (rc == 0 && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) rc = -1;
        if (rc != 0) (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
    }
    return rc;
}

static int migration_fixture_create(migration_legacy_fixture_t *fixture, const char *name) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->alpha[0] = 7;
    fixture->beta[1] = 9;
    int tie_fixture = strcmp(name, "manifest-tie-order") == 0;
    int punctuation_fts_fixture = strcmp(name, "fts-punctuation-only") == 0;
    snprintf(fixture->root_path, sizeof(fixture->root_path), "%s",
             strcmp(name, "manifest-isolation-b") == 0 ? "/tmp/cbm-migration-legacy-b"
                                                        : "/tmp/cbm-migration-legacy");
    const char *alpha_name = tie_fixture ? "Zulu" : "Alpha";
    const char *alpha_qn = tie_fixture ? "legacy.Zulu" : "legacy.Alpha";
    const char *beta_name = tie_fixture ? "Able" : "Beta";
    const char *beta_qn = tie_fixture ? "legacy.Able" : "legacy.Beta";
    const char *alpha_label = punctuation_fts_fixture ? "Project" : "Function";
    const char *beta_label = punctuation_fts_fixture ? "Branch" : "Function";
    const char *alpha_path = punctuation_fts_fixture ? "{}" : "src/alpha.c";
    const char *beta_path = punctuation_fts_fixture ? "{}" : "src/beta.c";
    snprintf(fixture->db_path, sizeof(fixture->db_path), "%s/cbm-migration-legacy-%s-%d.db",
             migration_tmpdir(), name, (int)getpid());
    if (cbm_zova_sidecar_path(fixture->db_path, fixture->zova_path,
                              sizeof(fixture->zova_path)) != 0) {
        return -1;
    }
    migration_fixture_cleanup(fixture);
    cbm_store_t *store = cbm_store_open_path(fixture->db_path);
    if (!store || cbm_store_upsert_project(store, "legacy", fixture->root_path) !=
                      CBM_STORE_OK) {
        if (store) cbm_store_close(store);
        return -1;
    }
    cbm_node_t alpha_node = {.project = "legacy", .label = alpha_label, .name = alpha_name,
                             .qualified_name = alpha_qn, .file_path = alpha_path,
                             .start_line = 1, .end_line = 3, .properties_json = "{}"};
    cbm_node_t beta_node = {.project = "legacy", .label = beta_label, .name = beta_name,
                            .qualified_name = beta_qn, .file_path = beta_path,
                            .start_line = 4, .end_line = 6, .properties_json = "{}"};
    int64_t alpha_id = cbm_store_upsert_node(store, &alpha_node);
    int64_t beta_id = cbm_store_upsert_node(store, &beta_node);
    cbm_edge_t edge = {.project = "legacy", .source_id = alpha_id, .target_id = beta_id,
                       .type = "CALLS", .properties_json = "{}"};
    if (alpha_id <= 0 || beta_id <= 0 || cbm_store_insert_edge(store, &edge) <= 0 ||
        cbm_store_exec(store,
                       "CREATE TABLE node_vectors(node_id INTEGER PRIMARY KEY,project TEXT NOT "
                       "NULL,vector BLOB NOT NULL);"
                       "CREATE TABLE token_vectors(id INTEGER PRIMARY KEY,project TEXT NOT NULL,"
                       "token TEXT NOT NULL,vector BLOB NOT NULL,idf REAL NOT NULL);") !=
            CBM_STORE_OK) {
        cbm_store_close(store);
        return -1;
    }
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    const char *node_sql = "INSERT INTO node_vectors VALUES(?1,'legacy',?2)";
    for (int i = 0; i < 2; i++) {
        if (sqlite3_prepare_v2(db, node_sql, -1, &stmt, NULL) != SQLITE_OK) {
            cbm_store_close(store);
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, i == 0 ? alpha_id : beta_id);
        sqlite3_bind_blob(stmt, 2, i == 0 ? fixture->alpha : fixture->beta,
                          MIGRATION_VECTOR_DIM, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            cbm_store_close(store);
            return -1;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (sqlite3_prepare_v2(db, "INSERT INTO token_vectors VALUES(1,'legacy','alpha',?1,1)", -1,
                           &stmt, NULL) != SQLITE_OK) {
        cbm_store_close(store);
        return -1;
    }
    sqlite3_bind_blob(stmt, 1, fixture->alpha, MIGRATION_VECTOR_DIM, SQLITE_STATIC);
    int token_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (token_rc != SQLITE_DONE ||
        cbm_store_exec(store, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');"
                              "INSERT INTO nodes_fts(rowid,name,qualified_name,label,file_path) "
                              "SELECT id,cbm_camel_split(name),qualified_name,label,file_path "
                              "FROM nodes;") !=
            CBM_STORE_OK) {
        cbm_store_close(store);
        return -1;
    }
    cbm_store_close(store);

    CBMDumpNode nodes[] = {
        {.id = alpha_id, .project = "legacy", .label = alpha_label, .name = alpha_name,
         .qualified_name = alpha_qn, .file_path = alpha_path, .start_line = 1,
         .end_line = 3, .properties = "{}"},
        {.id = beta_id, .project = "legacy", .label = beta_label, .name = beta_name,
         .qualified_name = beta_qn, .file_path = beta_path, .start_line = 4,
         .end_line = 6, .properties = "{}"},
    };
    CBMDumpEdge edges[] = {
        {.id = 1, .project = "legacy", .source_id = alpha_id, .target_id = beta_id,
         .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    CBMDumpVector vectors[] = {
        {.node_id = alpha_id, .project = "legacy", .vector = (uint8_t *)fixture->alpha,
         .vector_len = MIGRATION_VECTOR_DIM},
        {.node_id = beta_id, .project = "legacy", .vector = (uint8_t *)fixture->beta,
         .vector_len = MIGRATION_VECTOR_DIM},
    };
    CBMDumpTokenVec tokens[] = {
        {.id = 1, .project = "legacy", .token = "alpha", .vector = (uint8_t *)fixture->alpha,
         .vector_len = MIGRATION_VECTOR_DIM, .idf = 1.0f},
    };
    if (cbm_zova_after_sqlite_dump_workspace_direct(
            fixture->db_path, fixture->root_path, "legacy", nodes, 2, edges, 1, vectors, 2,
            tokens, 1, MIGRATION_VECTOR_DIM) != 0)
        return -1;
    return migration_synthesize_legacy_projections(fixture, nodes, 2, edges, 1);
}

TEST(zova_migration_v5_snapshot_reads_two_workspaces_without_zova_open) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path, sizeof(path), "v5-snapshot-two-workspaces");
    ASSERT_EQ(migration_generate_v5_v7_fixture(path), 0);

    zova_database *rejected = NULL;
    zova_message error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = path, .out_db = &rejected, .out_error_message = &error}),
              ZOVA_UNSUPPORTED_ZOVA_VERSION);
    ASSERT_NULL(rejected);
    zova_message_free(&error);

    cbm_zova_v5_snapshot_t *snapshot = NULL;
    ASSERT_EQ(cbm_zova_v5_snapshot_open(path, &snapshot), 0);
    ASSERT_NOT_NULL(snapshot);
    ASSERT_EQ(cbm_zova_v5_snapshot_workspace_count(snapshot), 2);

    int found_a = 0;
    int found_b = 0;
    for (int i = 0; i < 2; i++) {
        int64_t generation = 0;
        const cbm_zova_workspace_generation_input_t *input =
            cbm_zova_v5_snapshot_input_at(snapshot, i, &generation);
        ASSERT_NOT_NULL(input);
        ASSERT_GT(generation, 0);
        ASSERT_EQ(input->node_count, 2);
        ASSERT_EQ(input->edge_count, 1);
        ASSERT_EQ(input->node_vector_count, 1);
        ASSERT_EQ(input->token_vector_count, 1);
        ASSERT_EQ(input->file_hash_count, 1);
        ASSERT_EQ(input->vector_dimensions, MIGRATION_VECTOR_DIM);
        ASSERT_TRUE(input->project_summary.present);
        ASSERT_STR_EQ(input->nodes[0].properties, "{\"role\":\"source\"}");
        ASSERT_STR_EQ(input->edges[0].type, "CALLS");
        ASSERT_EQ(input->node_vectors[0].vector_len, MIGRATION_VECTOR_DIM);
        ASSERT_EQ(input->token_vectors[0].vector_len, MIGRATION_VECTOR_DIM);
        ASSERT_FLOAT_EQ(input->token_vectors[0].idf, 1.5f, 0.0001f);
        ASSERT_STR_EQ(input->file_hashes[0].content_hash, "hash-a");
        if (strcmp(input->project, "project-a") == 0) found_a++;
        if (strcmp(input->project, "project-b") == 0) found_b++;
    }
    ASSERT_EQ(found_a, 1);
    ASSERT_EQ(found_b, 1);
    cbm_zova_v5_snapshot_close(snapshot);
    cbm_unlink(path);
    PASS();
}

TEST(zova_migration_repack_atomically_promotes_two_workspaces) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path,sizeof(path),"repack-two-workspaces");
    ASSERT_EQ(migration_generate_v5_v7_fixture(path),0);
    cbm_zova_repack_report_t report={0};
    ASSERT_EQ(cbm_zova_database_repack(&(cbm_zova_repack_request_t){
        .live_path=path,.keep_recovery=true},&report),CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(report.source_cbm_schema,5);
    ASSERT_EQ(report.target_cbm_schema,CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(report.workspace_count,2);
    ASSERT_EQ(strlen(report.source_sha256),64);
    ASSERT_EQ(strlen(report.target_sha256),64);
    ASSERT_TRUE(access(report.recovery_path,F_OK)==0);
    ASSERT_EQ(migration_scalar(path,"SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),6);
    ASSERT_EQ(migration_scalar(path,"SELECT count(*) FROM cbm_workspace_registry"),2);
    ASSERT_EQ(migration_scalar(path,
        "SELECT count(*) FROM sqlite_master WHERE name IN ('cbm_zova_trace_nodes_v1',"
        "'cbm_zova_edge_metadata_v1','cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
        "'cbm_token_vectors_compat_v1') OR name GLOB 'cbm_fts_w1_*'"),0);
    cbm_zova_v5_snapshot_t *source=NULL;
    ASSERT_EQ(cbm_zova_v5_snapshot_open(report.recovery_path,&source),0);
    ASSERT_EQ(cbm_zova_v5_snapshot_workspace_count(source),2);
    cbm_zova_v5_snapshot_close(source);
    cbm_unlink(report.recovery_path);
    cbm_unlink(path);
    PASS();
}

TEST(zova_migration_repack_resumes_every_atomic_replacement_fault) {
    const char *faults[]={
        "repack_after_temp_creation","repack_after_temp_verification",
        "repack_after_live_to_recovery","repack_after_temp_to_live",
        "repack_before_recovery_cleanup",
    };
    for(size_t i=0;i<sizeof(faults)/sizeof(faults[0]);i++){
        char name[96],path[MIGRATION_PATH_MAX];
        snprintf(name,sizeof(name),"repack-fault-%zu",i);
        migration_path(path,sizeof(path),name);
        ASSERT_EQ(migration_generate_v5_v7_fixture(path),0);
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE",faults[i],1);
        cbm_zova_repack_report_t interrupted={0};
        ASSERT_EQ(cbm_zova_database_repack(&(cbm_zova_repack_request_t){
            .live_path=path,.keep_recovery=false},&interrupted),
            CBM_ZOVA_OPERATION_VERIFY_FAILED);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        cbm_zova_repack_report_t resumed={0};
        cbm_zova_operation_code_t code=cbm_zova_database_repack(
            &(cbm_zova_repack_request_t){.live_path=path,.keep_recovery=false},&resumed);
        ASSERT_TRUE(code==CBM_ZOVA_OPERATION_OK||code==CBM_ZOVA_OPERATION_NOOP);
        ASSERT_EQ(migration_scalar(path,"SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),6);
        ASSERT_EQ(migration_scalar(path,"SELECT count(*) FROM cbm_workspace_registry"),2);
        ASSERT_TRUE(access(resumed.recovery_path,F_OK)!=0);
        char temporary[MIGRATION_PATH_MAX];
        size_t length=strlen(path);
        snprintf(temporary,sizeof(temporary),"%.*s.repack.tmp.zova",(int)(length-5),path);
        ASSERT_TRUE(access(temporary,F_OK)!=0);
        cbm_unlink(path);
    }
    PASS();
}

TEST(zova_migration_v5_snapshot_rejects_corrupt_sources) {
    static const struct {
        const char *name;
        const char *sql;
    } cases[] = {
        {"missing-table", "DROP TABLE cbm_project_summaries_v2"},
        {"bad-dimensions", "UPDATE cbm_workspace_index_state_v1 SET vector_dimensions=7"},
        {"duplicate-stable-id",
         "PRAGMA foreign_keys=OFF;"
         "ALTER TABLE cbm_nodes_v1 RENAME TO cbm_nodes_v1_original;"
         "CREATE TABLE cbm_nodes_v1 AS SELECT * FROM cbm_nodes_v1_original;"
         "INSERT INTO cbm_nodes_v1 SELECT * FROM cbm_nodes_v1_original LIMIT 1;"
         "DROP TABLE cbm_nodes_v1_original"},
        {"mismatched-counts",
         "UPDATE cbm_generation_integrity_v2 SET graph_nodes=graph_nodes+1"},
        {"future-schema", "UPDATE cbm_database_schema_v1 SET schema_version=6 WHERE id=1"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[MIGRATION_PATH_MAX];
        migration_path(path, sizeof(path), cases[i].name);
        ASSERT_EQ(migration_publish_v5_workspace(path, "/tmp/cbm-v5-corrupt", "project", 1),
                  0);
        ASSERT_EQ(migration_finalize_v5_v7_fixture(path), 0);
        ASSERT_EQ(migration_sql(path, cases[i].sql), 0);
        cbm_zova_v5_snapshot_t *snapshot = NULL;
        ASSERT_EQ(cbm_zova_v5_snapshot_open(path, &snapshot), -1);
        ASSERT_NULL(snapshot);
        cbm_unlink(path);
    }
    PASS();
}

static int migration_store_sql(const char *path, const char *sql) {
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) return -1;
    int rc = cbm_store_exec(store, sql) == CBM_STORE_OK ? 0 : -1;
    cbm_store_close(store);
    return rc;
}

static int migration_fixture_advance(migration_legacy_fixture_t *fixture) {
    if (migration_store_sql(
            fixture->db_path,
            "UPDATE nodes SET properties='{\"generation\":2}' WHERE name='Alpha';"
            "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');"
            "INSERT INTO nodes_fts(rowid,name,qualified_name,label,file_path) SELECT id,"
            "cbm_camel_split(name),qualified_name,label,file_path FROM nodes") != 0)
        return -1;
    CBMDumpNode nodes[] = {
        {.id = 1, .project = "legacy", .label = "Function", .name = "Alpha",
         .qualified_name = "legacy.Alpha", .file_path = "src/alpha.c", .start_line = 1,
         .end_line = 3, .properties = "{\"generation\":2}"},
        {.id = 2, .project = "legacy", .label = "Function", .name = "Beta",
         .qualified_name = "legacy.Beta", .file_path = "src/beta.c", .start_line = 4,
         .end_line = 6, .properties = "{}"},
    };
    CBMDumpEdge edges[] = {
        {.id = 1, .project = "legacy", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    CBMDumpVector vectors[] = {
        {.node_id = 1, .project = "legacy", .vector = (uint8_t *)fixture->alpha,
         .vector_len = MIGRATION_VECTOR_DIM},
        {.node_id = 2, .project = "legacy", .vector = (uint8_t *)fixture->beta,
         .vector_len = MIGRATION_VECTOR_DIM},
    };
    CBMDumpTokenVec tokens[] = {
        {.id = 1, .project = "legacy", .token = "alpha", .vector = (uint8_t *)fixture->alpha,
         .vector_len = MIGRATION_VECTOR_DIM, .idf = 1.0f},
    };
    if (cbm_zova_after_sqlite_dump_workspace_direct(
            fixture->db_path, fixture->root_path, "legacy", nodes, 2, edges, 1, vectors, 2,
            tokens, 1, MIGRATION_VECTOR_DIM) != 0)
        return -1;
    return migration_synthesize_legacy_projections(fixture, nodes, 2, edges, 1);
}

TEST(zova_migration_legacy_snapshot_reads_one_ready_generation) {
    migration_legacy_fixture_t fixture;
    ASSERT_EQ(migration_fixture_create(&fixture, "ready"), 0);
    char db_wal[MIGRATION_PATH_MAX + 16];
    char db_journal[MIGRATION_PATH_MAX + 16];
    char db_shm[MIGRATION_PATH_MAX + 16];
    char zova_wal[MIGRATION_PATH_MAX + 16];
    char zova_journal[MIGRATION_PATH_MAX + 16];
    char zova_shm[MIGRATION_PATH_MAX + 16];
    snprintf(db_wal, sizeof(db_wal), "%s-wal", fixture.db_path);
    snprintf(db_journal, sizeof(db_journal), "%s-journal", fixture.db_path);
    snprintf(db_shm, sizeof(db_shm), "%s-shm", fixture.db_path);
    snprintf(zova_wal, sizeof(zova_wal), "%s-wal", fixture.zova_path);
    snprintf(zova_journal, sizeof(zova_journal), "%s-journal", fixture.zova_path);
    snprintf(zova_shm, sizeof(zova_shm), "%s-shm", fixture.zova_path);
    const char *artifacts[] = {fixture.db_path, fixture.zova_path, db_wal, db_journal,
                               db_shm, zova_wal, zova_journal, zova_shm};
    migration_artifact_stat_t before[8];
    for (int i = 0; i < 8; i++) before[i] = migration_artifact_stat(artifacts[i]);
    char registry_path[MIGRATION_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    int64_t active_before = 0;
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/cbm-migration-legacy",
                                           workspace_id, sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_before),
              0);
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_NOT_NULL(snapshot);
    const cbm_zova_workspace_generation_input_t *input =
        cbm_zova_legacy_snapshot_input(snapshot);
    ASSERT_NOT_NULL(input);
    ASSERT_STR_EQ(input->root_path, "/tmp/cbm-migration-legacy");
    ASSERT_STR_EQ(input->project, "legacy");
    ASSERT_STR_EQ(input->model_fingerprint, CBM_ZOVA_MODEL_FINGERPRINT);
    ASSERT_EQ(input->node_count, 2);
    ASSERT_EQ(input->edge_count, 1);
    ASSERT_EQ(input->node_vector_count, 2);
    ASSERT_EQ(input->token_vector_count, 1);
    ASSERT_EQ(input->vector_dimensions, MIGRATION_VECTOR_DIM);
    int fts_count = 0;
    const cbm_zova_migration_fts_row_t *fts_rows =
        cbm_zova_legacy_snapshot_fts_rows(snapshot, &fts_count);
    ASSERT_NOT_NULL(fts_rows);
    ASSERT_EQ(fts_count, 2);
    ASSERT_STR_EQ(fts_rows[0].name, "Alpha");
    ASSERT_STR_EQ(fts_rows[1].name, "Beta");
    ASSERT_GT(cbm_zova_legacy_snapshot_source_generation(snapshot), 0);
    cbm_zova_legacy_snapshot_close(snapshot);
    ASSERT_EQ(migration_artifact_stat_matches(fixture.db_path, before[0]), 0);
    ASSERT_EQ(migration_artifact_stat_matches(fixture.zova_path, before[1]), 0);
    ASSERT_EQ(migration_companion_bytes_match(db_wal, before[2]), 0);
    ASSERT_EQ(migration_companion_bytes_match(db_journal, before[3]), 0);
    ASSERT_EQ(migration_companion_bytes_match(zova_wal, before[5]), 0);
    ASSERT_EQ(migration_companion_bytes_match(zova_journal, before[6]), 0);
    ASSERT_EQ(migration_shm_stat_matches(db_shm, before[4]), 0);
    ASSERT_EQ(migration_shm_stat_matches(zova_shm, before[7]), 0);
    int64_t active_after = 0;
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_after),
              0);
    ASSERT_EQ(active_after, active_before);
    migration_fixture_cleanup(&fixture);
    PASS();
}

TEST(zova_migration_legacy_snapshot_rejects_semantic_id_collision_and_fts_drift) {
    migration_legacy_fixture_t fixture;
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(migration_fixture_create(&fixture, "collision"), 0);
    ASSERT_EQ(migration_sql(fixture.db_path,
                            "CREATE TABLE nodes_collision(id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "project TEXT NOT NULL,label TEXT NOT NULL,name TEXT NOT NULL,"
                            "qualified_name TEXT NOT NULL,file_path TEXT,start_line INTEGER,"
                            "end_line INTEGER,properties TEXT);"
                            "INSERT INTO nodes_collision SELECT * FROM nodes;"
                            "DROP TABLE nodes;ALTER TABLE nodes_collision RENAME TO nodes;"
                            "INSERT INTO nodes(project,label,name,qualified_name,file_path,"
                            "start_line,end_line,properties) SELECT project,label,name,"
                            "qualified_name,file_path,start_line,end_line,properties FROM nodes "
                            "WHERE name='Alpha' LIMIT 1"),
              0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE);
    migration_fixture_cleanup(&fixture);

    ASSERT_EQ(migration_fixture_create(&fixture, "fts-drift"), 0);
    ASSERT_EQ(migration_store_sql(
                  fixture.db_path,
                  "DROP TABLE nodes_fts;"
                  "CREATE VIRTUAL TABLE nodes_fts USING fts5(name,qualified_name,label,"
                  "file_path,content='',tokenize='unicode61 remove_diacritics 2');"
                  "INSERT INTO nodes_fts(rowid,name,qualified_name,label,file_path) "
                  "SELECT id,cbm_camel_split(CASE WHEN name='Alpha' THEN 'Changed' ELSE name END),"
                  "qualified_name,label,file_path FROM nodes"),
              0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE);
    migration_fixture_cleanup(&fixture);
    PASS();
}

TEST(zova_migration_legacy_snapshot_accepts_punctuation_only_fts_field) {
    migration_legacy_fixture_t fixture;
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(migration_fixture_create(&fixture, "fts-punctuation-only"), 0);
    migration_artifact_stat_t db_before = migration_artifact_stat(fixture.db_path);
    migration_artifact_stat_t zova_before = migration_artifact_stat(fixture.zova_path);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_NOT_NULL(snapshot);
    int fts_count = 0;
    const cbm_zova_migration_fts_row_t *fts_rows =
        cbm_zova_legacy_snapshot_fts_rows(snapshot, &fts_count);
    ASSERT_NOT_NULL(fts_rows);
    ASSERT_EQ(fts_count, 2);
    ASSERT_STR_EQ(fts_rows[0].label, "Project");
    ASSERT_STR_EQ(fts_rows[0].file_path, "{}");
    ASSERT_STR_EQ(fts_rows[1].label, "Branch");
    ASSERT_STR_EQ(fts_rows[1].file_path, "{}");
    cbm_zova_legacy_snapshot_close(snapshot);
    ASSERT_EQ(migration_artifact_stat_matches(fixture.db_path, db_before), 0);
    ASSERT_EQ(migration_artifact_stat_matches(fixture.zova_path, zova_before), 0);

    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "fts-punctuation-only-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = fixture.db_path,
        .source_zova_path = fixture.zova_path,
        .target_zova_path = target,
        .canonical_root = fixture.root_path,
    };
    cbm_zova_migration_report_t report = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &report), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(report.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_EQ(report.source.fts_query_count, report.target.fts_query_count);
    ASSERT_STR_EQ(report.source.fts_sha256, report.target.fts_sha256);
    ASSERT_EQ(migration_artifact_stat_matches(fixture.db_path, db_before), 0);
    ASSERT_EQ(migration_artifact_stat_matches(fixture.zova_path, zova_before), 0);
    migration_fixture_cleanup(&fixture);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_legacy_snapshot_rejects_missing_artifacts) {
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(cbm_zova_legacy_snapshot_open("/tmp/cbm-missing.db", "/tmp/cbm-missing.zova",
                                            "/tmp/root", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_MISSING);
    migration_legacy_fixture_t fixture;
    ASSERT_EQ(migration_fixture_create(&fixture, "missing"), 0);
    cbm_unlink(fixture.zova_path);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_MISSING);
    migration_fixture_cleanup(&fixture);
    PASS();
}

TEST(zova_migration_legacy_snapshot_rejects_project_and_schema_mismatch) {
    migration_legacy_fixture_t fixture;
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(migration_fixture_create(&fixture, "root"), 0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path, "/tmp/wrong",
                                            &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_NOT_READY);
    ASSERT_EQ(migration_sql(fixture.db_path, "DROP TABLE file_hashes"), 0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE);
    migration_fixture_cleanup(&fixture);

    ASSERT_EQ(migration_fixture_create(&fixture, "projects"), 0);
    ASSERT_EQ(migration_sql(
                  fixture.db_path,
                  "INSERT INTO projects(name,indexed_at,root_path) "
                  "VALUES('duplicate-root','2026-07-13','/tmp/cbm-migration-legacy')"),
              0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_NOT_READY);
    migration_fixture_cleanup(&fixture);

    ASSERT_EQ(migration_fixture_create(&fixture, "sidecar-schema"), 0);
    ASSERT_EQ(migration_sql(fixture.zova_path,
                            "UPDATE cbm_zova_schema_v1 SET metadata_projection_version=99 "
                            "WHERE id=1"),
              0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE);
    migration_fixture_cleanup(&fixture);
    PASS();
}

TEST(zova_migration_legacy_snapshot_rejects_generation_and_vector_mismatch) {
    migration_legacy_fixture_t fixture;
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(migration_fixture_create(&fixture, "generation"), 0);
    ASSERT_EQ(migration_sql(
                  fixture.db_path,
                  "UPDATE cbm_zova_sidecar_generation_v1 SET generation=generation+1 WHERE id=1"),
              0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_NOT_READY);
    migration_fixture_cleanup(&fixture);

    ASSERT_EQ(migration_fixture_create(&fixture, "dimension"), 0);
    ASSERT_EQ(migration_sql(fixture.db_path,
                            "UPDATE node_vectors SET vector=zeroblob(3) WHERE node_id=1"),
              0);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE);
    migration_fixture_cleanup(&fixture);
    PASS();
}

TEST(zova_migration_manifest_matches_uncommitted_target) {
    migration_legacy_fixture_t fixture;
    ASSERT_EQ(migration_fixture_create(&fixture, "manifest"), 0);
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(fixture.db_path, fixture.zova_path,
                                            "/tmp/cbm-migration-legacy", &snapshot),
              CBM_ZOVA_MIGRATION_OK);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "manifest-target");
    ASSERT_EQ(cbm_zova_user_database_init(target), 0);
    zova_database *db = NULL;
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){.path = target, .out_db = &db}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){.db = db,
                                                               .sql = "BEGIN IMMEDIATE"}),
              ZOVA_OK);
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace_tx(
                  db, cbm_zova_legacy_snapshot_input(snapshot), &published),
              0);
    cbm_zova_migration_manifest_t target_manifest = {0};
    ASSERT_EQ(cbm_zova_migration_manifest_target_tx(db, published.workspace_id,
                                                    published.generation, snapshot,
                                                    &target_manifest),
              CBM_ZOVA_MIGRATION_OK);
    const cbm_zova_migration_manifest_t *source_manifest =
        cbm_zova_legacy_snapshot_manifest(snapshot);
    ASSERT_NOT_NULL(source_manifest);
    ASSERT_EQ(target_manifest.workspace_count, 1);
    ASSERT_EQ(target_manifest.stable_id_count, 2);
    ASSERT_STR_EQ(target_manifest.metadata_sha256, source_manifest->metadata_sha256);
    ASSERT_STR_EQ(target_manifest.fts_sha256, source_manifest->fts_sha256);
    ASSERT_STR_EQ(target_manifest.topology_sha256, source_manifest->topology_sha256);
    ASSERT_STR_EQ(target_manifest.node_vector_sha256, source_manifest->node_vector_sha256);
    ASSERT_STR_EQ(target_manifest.token_vector_sha256, source_manifest->token_vector_sha256);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){.db = db, .sql = "ROLLBACK"}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);
    cbm_zova_legacy_snapshot_close(snapshot);
    migration_fixture_cleanup(&fixture);
    cbm_unlink(target);
    PASS();
}

typedef struct {
    migration_legacy_fixture_t legacy;
    cbm_zova_legacy_snapshot_t *snapshot;
    char target[MIGRATION_PATH_MAX];
    zova_database *db;
    cbm_zova_workspace_generation_result_t published;
} migration_manifest_fixture_t;

static int migration_manifest_fixture_open(migration_manifest_fixture_t *fixture,
                                           const char *name) {
    memset(fixture, 0, sizeof(*fixture));
    if (migration_fixture_create(&fixture->legacy, name) != 0 ||
        cbm_zova_legacy_snapshot_open(fixture->legacy.db_path, fixture->legacy.zova_path,
                                      fixture->legacy.root_path, &fixture->snapshot) !=
            CBM_ZOVA_MIGRATION_OK)
        return -1;
    migration_path(fixture->target, sizeof(fixture->target), name);
    if (cbm_zova_user_database_init(fixture->target) != 0 ||
        zova_database_open(&(zova_database_open_request){
            .path = fixture->target, .out_db = &fixture->db}) != ZOVA_OK ||
        zova_database_exec(&(zova_database_exec_request){
            .db = fixture->db, .sql = "BEGIN IMMEDIATE"}) != ZOVA_OK ||
        cbm_zova_user_database_publish_workspace_tx(
            fixture->db, cbm_zova_legacy_snapshot_input(fixture->snapshot),
            &fixture->published) != 0)
        return -1;
    return 0;
}

static void migration_manifest_fixture_close(migration_manifest_fixture_t *fixture) {
    if (fixture->db) {
        (void)zova_database_exec(&(zova_database_exec_request){
            .db = fixture->db, .sql = "ROLLBACK"});
        (void)zova_database_close(fixture->db);
    }
    cbm_zova_legacy_snapshot_close(fixture->snapshot);
    migration_fixture_cleanup(&fixture->legacy);
    cbm_unlink(fixture->target);
}

static int migration_manifest_verify(migration_manifest_fixture_t *fixture) {
    cbm_zova_migration_manifest_t manifest = {0};
    return cbm_zova_migration_manifest_target_tx(
        fixture->db, fixture->published.workspace_id, fixture->published.generation,
        fixture->snapshot, &manifest);
}

TEST(zova_migration_manifest_rejects_workspace_ids_and_metadata_mismatch) {
    migration_manifest_fixture_t fixture;
    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-root-drift"), 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db = fixture.db,
                  .sql = "UPDATE cbm_projects_v1 SET root_path='/wrong'"}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);

    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-id-drift"), 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db = fixture.db,
                  .sql = "INSERT INTO cbm_nodes_v1(workspace_id,node_id,project,label,name,"
                         "qualified_name,file_path,start_line,end_line,properties) SELECT "
                         "workspace_id,'n:v2:extra',project,label,name,qualified_name,file_path,"
                         "start_line,end_line,properties FROM cbm_nodes_v1 LIMIT 1"}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);

    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-metadata-drift"), 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db = fixture.db,
                  .sql = "UPDATE cbm_nodes_v1 SET properties='{\"changed\":true}' WHERE "
                         "name='Alpha'"}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);
    PASS();
}

TEST(zova_migration_manifest_rejects_graph_and_vector_mismatch) {
    migration_manifest_fixture_t fixture;
    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-graph-drift"), 0);
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    ASSERT_EQ(cbm_zova_workspace_graph_name(fixture.published.workspace_id, graph_name,
                                            sizeof(graph_name)),
              0);
    ASSERT_EQ(zova_graph_node_delete(&(zova_graph_node_delete_request){
                  .db = fixture.db,
                  .graph_name = graph_name,
                  .node_id = cbm_zova_legacy_snapshot_target_id(fixture.snapshot, 1)}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);

    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-vector-native-delete"), 0);
    const cbm_zova_workspace_generation_input_t *input =
        cbm_zova_legacy_snapshot_input(fixture.snapshot);
    char collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  fixture.published.workspace_id, input->model_fingerprint,
                  input->vector_dimensions, collection, sizeof(collection)),
              0);
    ASSERT_EQ(zova_vector_delete(&(zova_vector_delete_request){
                  .db = fixture.db,
                  .collection_name = collection,
                  .vector_id = cbm_zova_legacy_snapshot_target_id(fixture.snapshot, 1)}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);

    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-vector-native-extra"), 0);
    input = cbm_zova_legacy_snapshot_input(fixture.snapshot);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  fixture.published.workspace_id, input->model_fingerprint,
                  input->vector_dimensions, collection, sizeof(collection)),
              0);
    int8_t extra[MIGRATION_VECTOR_DIM] = {1};
    ASSERT_EQ(zova_vector_put(&(zova_vector_put_request){
                  .db = fixture.db,
                  .collection_name = collection,
                  .vector_id = "n:v2:extra",
                  .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                             .i8_values = extra,
                             .values_len = MIGRATION_VECTOR_DIM}}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);

    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-vector-dimension"), 0);
    input = cbm_zova_legacy_snapshot_input(fixture.snapshot);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  fixture.published.workspace_id, input->model_fingerprint,
                  input->vector_dimensions, collection, sizeof(collection)),
              0);
    ASSERT_EQ(zova_vector_collection_delete(&(zova_vector_collection_delete_request){
                  .db = fixture.db, .name = collection}),
              ZOVA_OK);
    ASSERT_EQ(zova_vector_collection_create(&(zova_vector_collection_create_request){
                  .db = fixture.db,
                  .name = collection,
                  .options = {.dimensions = 3,
                              .metric = ZOVA_VECTOR_METRIC_COSINE,
                              .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8}}),
              ZOVA_OK);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);
    PASS();
}

static int migration_manifest_rebuild_fts(migration_manifest_fixture_t *fixture,
                                          int ordered_only) {
    char sql[2048];
    if (snprintf(sql, sizeof(sql),
                 "DROP TABLE cbm_nodes_fts_v1;"
                 "CREATE VIRTUAL TABLE cbm_nodes_fts_v1 USING fts5("
                 "workspace_id UNINDEXED,node_id UNINDEXED,name,qualified_name,file_path,label);"
                 "INSERT INTO cbm_nodes_fts_v1(workspace_id,node_id,name,qualified_name,"
                 "file_path,label) SELECT n.workspace_id,n.node_id,"
                 "cbm_camel_split(CASE WHEN n.name='Alpha' THEN %s ELSE n.name END),"
                 "n.qualified_name,n.file_path,n.label FROM cbm_nodes_v1 n "
                 "WHERE n.workspace_id='%s'",
                 ordered_only ? "n.name||' alpha'" : "'Changed'",
                 fixture->published.workspace_id) >= (int)sizeof(sql))
        return -1;
    return zova_database_exec(&(zova_database_exec_request){
               .db = fixture->db, .sql = sql}) == ZOVA_OK
               ? 0
               : -1;
}

TEST(zova_migration_manifest_rejects_fts_row_and_ordered_result_mismatch) {
    migration_manifest_fixture_t fixture;
    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-fts-row-drift"), 0);
    ASSERT_EQ(migration_manifest_rebuild_fts(&fixture, 0), 0);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);

    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-fts-order-drift"), 0);
    ASSERT_EQ(migration_manifest_rebuild_fts(&fixture, 1), 0);
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    migration_manifest_fixture_close(&fixture);
    PASS();
}

TEST(zova_migration_manifest_uses_public_fts_tie_breaker) {
    migration_manifest_fixture_t fixture;
    ASSERT_EQ(migration_manifest_fixture_open(&fixture, "manifest-tie-order"), 0);
    const cbm_zova_workspace_generation_input_t *input =
        cbm_zova_legacy_snapshot_input(fixture.snapshot);
    ASSERT_STR_EQ(input->nodes[0].qualified_name, "legacy.Zulu");
    ASSERT_STR_EQ(input->nodes[1].qualified_name, "legacy.Able");
    ASSERT_EQ(migration_manifest_verify(&fixture), CBM_ZOVA_MIGRATION_OK);
    migration_manifest_fixture_close(&fixture);
    PASS();
}

TEST(zova_migration_manifest_preserves_existing_workspace_b) {
    migration_legacy_fixture_t legacy_a, legacy_b;
    ASSERT_EQ(migration_fixture_create(&legacy_a, "manifest-isolation-a"), 0);
    ASSERT_EQ(migration_fixture_create(&legacy_b, "manifest-isolation-b"), 0);
    cbm_zova_legacy_snapshot_t *snapshot_a = NULL;
    cbm_zova_legacy_snapshot_t *snapshot_b = NULL;
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(legacy_a.db_path, legacy_a.zova_path,
                                            legacy_a.root_path, &snapshot_a),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(cbm_zova_legacy_snapshot_open(legacy_b.db_path, legacy_b.zova_path,
                                            legacy_b.root_path, &snapshot_b),
              CBM_ZOVA_MIGRATION_OK);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "manifest-isolation-target");
    ASSERT_EQ(cbm_zova_user_database_init(target), 0);
    zova_database *db = NULL;
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){.path = target, .out_db = &db}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db = db, .sql = "BEGIN IMMEDIATE"}),
              ZOVA_OK);
    cbm_zova_workspace_generation_result_t published_b = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace_tx(
                  db, cbm_zova_legacy_snapshot_input(snapshot_b), &published_b),
              0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){.db = db, .sql = "COMMIT"}),
              ZOVA_OK);
    cbm_zova_migration_manifest_t b_before = {0};
    ASSERT_EQ(cbm_zova_migration_manifest_target_tx(db, published_b.workspace_id,
                                                    published_b.generation, snapshot_b,
                                                    &b_before),
              CBM_ZOVA_MIGRATION_OK);

    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db = db, .sql = "BEGIN IMMEDIATE"}),
              ZOVA_OK);
    cbm_zova_workspace_generation_result_t published_a = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace_tx(
                  db, cbm_zova_legacy_snapshot_input(snapshot_a), &published_a),
              0);
    cbm_zova_migration_manifest_t a_manifest = {0}, b_after = {0};
    ASSERT_EQ(cbm_zova_migration_manifest_target_tx(db, published_a.workspace_id,
                                                    published_a.generation, snapshot_a,
                                                    &a_manifest),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(cbm_zova_migration_manifest_target_tx(db, published_b.workspace_id,
                                                    published_b.generation, snapshot_b,
                                                    &b_after),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(memcmp(&b_before, &b_after, sizeof(b_before)), 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){.db = db, .sql = "ROLLBACK"}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);
    cbm_zova_legacy_snapshot_close(snapshot_a);
    cbm_zova_legacy_snapshot_close(snapshot_b);
    migration_fixture_cleanup(&legacy_a);
    migration_fixture_cleanup(&legacy_b);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_run_activates_once_and_is_idempotent) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "run-active"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "run-active-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t first = {0}, second = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &first), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(first.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_FALSE(first.no_op);
    ASSERT_GT(first.target_generation, 0);
    ASSERT_EQ(cbm_zova_migration_run(&request, &second), CBM_ZOVA_MIGRATION_NOOP);
    ASSERT_EQ(second.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_TRUE(second.no_op);
    ASSERT_EQ(second.target_generation, first.target_generation);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 1);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_run_recovers_precommit_fault_without_visibility) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "run-restart"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "run-restart-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "migration_after_target_publish", 1), 0);
    cbm_zova_migration_report_t failed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &failed), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 0);
    cbm_zova_migration_report_t status = {0};
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_PREPARED);
    cbm_zova_migration_report_t recovered = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &recovered), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(recovered.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 1);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_run_recovers_every_named_fault_phase) {
    const char *phases[] = {
        "migration_after_prepare",       "migration_after_source_snapshot",
        "migration_after_target_publish", "migration_after_counts",
        "migration_after_stable_ids",    "migration_after_graph",
        "migration_after_vectors",       "migration_after_metadata",
        "migration_after_fts",           "migration_before_activate",
        "migration_after_activate_commit",
    };
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), "run-fault-%zu", i);
        migration_legacy_fixture_t legacy;
        ASSERT_EQ(migration_fixture_create(&legacy, name), 0);
        char target[MIGRATION_PATH_MAX];
        migration_path(target, sizeof(target), name);
        cbm_zova_migration_request_t request = {
            .source_db_path = legacy.db_path,
            .source_zova_path = legacy.zova_path,
            .target_zova_path = target,
            .canonical_root = legacy.root_path,
        };
        ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", phases[i], 1), 0);
        cbm_zova_migration_report_t interrupted = {0};
        ASSERT_EQ(cbm_zova_migration_run(&request, &interrupted),
                  CBM_ZOVA_MIGRATION_VERIFY_FAILED);
        ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
        cbm_zova_migration_report_t status = {0};
        ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
        int postcommit = strcmp(phases[i], "migration_after_activate_commit") == 0;
        ASSERT_EQ(status.state,
                  postcommit ? CBM_ZOVA_MIGRATION_ACTIVE : CBM_ZOVA_MIGRATION_PREPARED);
        ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"),
                  postcommit ? 1 : 0);
        cbm_zova_migration_report_t recovered = {0};
        ASSERT_EQ(cbm_zova_migration_run(&request, &recovered),
                  postcommit ? CBM_ZOVA_MIGRATION_NOOP : CBM_ZOVA_MIGRATION_OK);
        ASSERT_EQ(recovered.state, CBM_ZOVA_MIGRATION_ACTIVE);
        ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"),
                  1);
        migration_fixture_cleanup(&legacy);
        cbm_unlink(target);
    }
    PASS();
}

#ifndef _WIN32
static int migration_run_crash_child(const cbm_zova_migration_request_t *request,
                                     const char *phase) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        (void)setenv("CBM_ZOVA_TEST_CRASH_PHASE", phase, 1);
        cbm_zova_migration_report_t report = {0};
        (void)cbm_zova_migration_run(request, &report);
        _exit(87);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86
               ? 0
               : -1;
}

TEST(zova_migration_run_recovers_real_process_death) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "run-crash"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "run-crash-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    ASSERT_EQ(migration_run_crash_child(&request, "migration_after_target_publish"), 0);
    cbm_zova_migration_report_t status = {0};
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_PREPARED);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 0);
    cbm_zova_migration_report_t recovered = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &recovered), CBM_ZOVA_MIGRATION_OK);
    cbm_unlink(target);

    migration_path(target, sizeof(target), "run-crash-post-target");
    ASSERT_EQ(migration_run_crash_child(&request, "migration_after_activate_commit"), 0);
    memset(&status, 0, sizeof(status));
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 1);
    memset(&recovered, 0, sizeof(recovered));
    ASSERT_EQ(cbm_zova_migration_run(&request, &recovered), CBM_ZOVA_MIGRATION_NOOP);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}
#endif

TEST(zova_migration_run_changed_source_preserves_prior_ready_then_advances_once) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "run-changed"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "run-changed-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t first = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &first), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(first.target_generation, 1);
    ASSERT_EQ(migration_fixture_advance(&legacy), 0);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "migration_after_target_publish", 1), 0);
    cbm_zova_migration_report_t interrupted = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &interrupted),
              CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT active_generation FROM cbm_workspace_registry LIMIT 1"),
              1);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_database_generation_v1 WHERE "
                               "state='ready'"),
              1);
    cbm_zova_migration_report_t second = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &second), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(second.target_generation, 2);
    ASSERT_GT(second.source_generation, first.source_generation);
    cbm_zova_migration_report_t no_op = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &no_op), CBM_ZOVA_MIGRATION_NOOP);
    ASSERT_EQ(no_op.target_generation, 2);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 2);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_status_is_read_only_and_retired_survives_missing_source) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "status-retired"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "status-retired-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    migration_artifact_stat_t before = migration_artifact_stat(target);
    cbm_zova_migration_report_t status = {0};
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_GT(status.target.stable_id_count, 0);
    ASSERT_EQ(migration_artifact_stat_matches(target, before), 0);
    ASSERT_EQ(migration_sql(target,
                            "UPDATE cbm_workspace_migrations_v1 SET state='retired',"
                            "retired_at=CURRENT_TIMESTAMP"),
              0);
    cbm_unlink(legacy.db_path);
    cbm_unlink(legacy.zova_path);
    memset(&status, 0, sizeof(status));
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_RETIRED);
    ASSERT_GT(status.target.graph_node_count, 0);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_requires_exact_confirmation) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-confirm"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-confirm-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    migration_artifact_stat_t db_before = migration_artifact_stat(legacy.db_path);
    migration_artifact_stat_t zova_before = migration_artifact_stat(legacy.zova_path);
    cbm_zova_migration_report_t cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, NULL, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, "wrong-workspace", &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_artifact_stat_matches(legacy.db_path, db_before), 0);
    ASSERT_EQ(migration_artifact_stat_matches(legacy.zova_path, zova_before), 0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"),
              1);
    cbm_zova_migration_report_t rolled_back = {0};
    ASSERT_EQ(cbm_zova_migration_rollback(&request, &rolled_back), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_artifact_stat_matches(legacy.db_path, db_before), 0);
    ASSERT_EQ(migration_artifact_stat_matches(legacy.zova_path, zova_before), 0);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_valid_bundle_retires_source) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-valid"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-valid-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    char wal[MIGRATION_PATH_MAX], shm[MIGRATION_PATH_MAX], journal[MIGRATION_PATH_MAX];
    char zova_wal[MIGRATION_PATH_MAX], zova_shm[MIGRATION_PATH_MAX], zova_journal[MIGRATION_PATH_MAX];
    snprintf(wal, sizeof(wal), "%s-wal", legacy.db_path);
    snprintf(shm, sizeof(shm), "%s-shm", legacy.db_path);
    snprintf(journal, sizeof(journal), "%s-journal", legacy.db_path);
    snprintf(zova_wal, sizeof(zova_wal), "%s-wal", legacy.zova_path);
    snprintf(zova_shm, sizeof(zova_shm), "%s-shm", legacy.zova_path);
    snprintf(zova_journal, sizeof(zova_journal), "%s-journal", legacy.zova_path);
    const char *recovery[] = {wal, shm, journal, zova_wal, zova_shm, zova_journal};
    for (size_t i = 0; i < sizeof(recovery) / sizeof(recovery[0]); i++) {
        FILE *member = fopen(recovery[i], "wb");
        ASSERT_NOT_NULL(member);
        ASSERT_EQ(fclose(member), 0);
    }
    cbm_zova_migration_report_t cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(cleanup.state, CBM_ZOVA_MIGRATION_RETIRED);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_FALSE(migration_artifact_stat(legacy.zova_path).exists);
    for (size_t i = 0; i < sizeof(recovery) / sizeof(recovery[0]); i++)
        ASSERT_FALSE(migration_artifact_stat(recovery[i]).exists);
    cbm_zova_migration_report_t retired = {0};
    ASSERT_EQ(cbm_zova_migration_status(&request, &retired), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(retired.state, CBM_ZOVA_MIGRATION_RETIRED);
    cbm_zova_migration_report_t unavailable = {0};
    ASSERT_EQ(cbm_zova_migration_rollback(&request, &unavailable),
              CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE);
    char source_parent[MIGRATION_PATH_MAX];
    snprintf(source_parent, sizeof(source_parent), "%s", legacy.db_path);
    char *source_slash = strrchr(source_parent, '/');
    ASSERT_NOT_NULL(source_slash);
    *source_slash = '\0';
    char audit_path[MIGRATION_PATH_MAX * 2];
    snprintf(audit_path, sizeof(audit_path), "%s/.zova-retired/%s-g%lld/manifest.json",
             source_parent, active.workspace_id, (long long)active.source_generation);
    FILE *audit = fopen(audit_path, "rb");
    ASSERT_NOT_NULL(audit);
    char audit_json[16384] = {0};
    ASSERT_GT(fread(audit_json, 1, sizeof(audit_json) - 1, audit), 0);
    ASSERT_EQ(fclose(audit), 0);
    ASSERT_NOT_NULL(strstr(audit_json, "\"intent\""));
    ASSERT_NOT_NULL(strstr(audit_json, "\"sha256\""));
    ASSERT_NOT_NULL(strstr(audit_json, "\"state\":\"purged\""));
    ASSERT_NOT_NULL(strstr(audit_json, "-wal"));
    ASSERT_NOT_NULL(strstr(audit_json, "-shm"));
    ASSERT_NOT_NULL(strstr(audit_json, "-journal"));
    ASSERT_EQ(migration_count_substring(audit_json, "\"index\":"), 8);
    ASSERT_EQ(migration_count_substring(audit_json, "\"size\":"), 8);
    ASSERT_EQ(migration_count_substring(audit_json, "\"sha256\":"), 8);
    int present_members = migration_count_substring(audit_json, "\"present\":true");
    ASSERT_TRUE(present_members >= 2);
    ASSERT_EQ(migration_count_substring(audit_json, "\"state\":\"purged\""),
              present_members);
    ASSERT_EQ(migration_count_substring(audit_json, legacy.db_path), 5);
    ASSERT_EQ(migration_count_substring(audit_json, legacy.zova_path), 5);
#ifndef _WIN32
    char quarantine[MIGRATION_PATH_MAX * 2];
    snprintf(quarantine, sizeof(quarantine), "%s/.zova-retired/%s-g%lld", source_parent,
             active.workspace_id, (long long)active.source_generation);
    DIR *directory = opendir(quarantine);
    ASSERT_NOT_NULL(directory);
    int entries = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ASSERT_STR_EQ(entry->d_name, "manifest.json");
            entries++;
        }
    ASSERT_EQ(closedir(directory), 0);
    ASSERT_EQ(entries, 1);
#endif
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='retired'"),
              1);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_resumes_after_intent_fault) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-resume-intent"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-resume-intent-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_intent", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    cbm_store_t *blocked_writer = cbm_store_open_path(legacy.db_path);
    cbm_store_t *blocked_reader = cbm_store_open_path_query(legacy.db_path);
    ASSERT_NULL(blocked_writer);
    ASSERT_NULL(blocked_reader);
    cbm_zova_migration_report_t rollback = {0};
    ASSERT_EQ(cbm_zova_migration_rollback(&request, &rollback),
              CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_FALSE(migration_artifact_stat(legacy.zova_path).exists);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_resumes_after_first_member_move) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-resume-member0"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-resume-member0-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_member_0", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    ASSERT_FALSE(migration_artifact_stat(legacy.zova_path).exists);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_resumes_after_every_member_decision) {
    for (int member_index = 0; member_index < 8; member_index++) {
        char name[64];
        snprintf(name, sizeof(name), "cleanup-member-%d", member_index);
        migration_legacy_fixture_t legacy;
        ASSERT_EQ(migration_fixture_create(&legacy, name), 0);
        char target[MIGRATION_PATH_MAX];
        migration_path(target, sizeof(target), name);
        cbm_zova_migration_request_t request = {
            .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
            .target_zova_path = target, .canonical_root = legacy.root_path,
        };
        cbm_zova_migration_report_t active = {0}, interrupted = {0}, resumed = {0};
        ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
        char recovery[6][MIGRATION_PATH_MAX];
        static const char *const suffixes[] = {"-wal", "-shm", "-journal"};
        for (int base = 0; base < 2; base++) {
            for (int suffix = 0; suffix < 3; suffix++) {
                int index = base * 3 + suffix;
                snprintf(recovery[index], sizeof(recovery[index]), "%s%s",
                         base == 0 ? legacy.db_path : legacy.zova_path, suffixes[suffix]);
                FILE *file = fopen(recovery[index], "wb");
                ASSERT_NOT_NULL(file);
                ASSERT_EQ(fclose(file), 0);
            }
        }
        char phase[64];
        snprintf(phase, sizeof(phase), "cleanup_after_member_%d", member_index);
        ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", phase, 1), 0);
        ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
                  CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
        ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
        ASSERT_EQ(migration_scalar(target,
                                   "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                                   "state='cleanup_pending'"), 1);
        ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
                  CBM_ZOVA_MIGRATION_OK);
        ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
        char parent[MIGRATION_PATH_MAX], quarantine[MIGRATION_PATH_MAX * 2];
        snprintf(parent, sizeof(parent), "%s", legacy.db_path);
        char *slash = strrchr(parent, '/');
        ASSERT_NOT_NULL(slash);
        *slash = '\0';
        snprintf(quarantine, sizeof(quarantine), "%s/.zova-retired/%s-g%lld", parent,
                 active.workspace_id, (long long)active.source_generation);
        const char *db_base = strrchr(legacy.db_path, '/');
        db_base = db_base ? db_base + 1 : legacy.db_path;
        const char *zova_base = strrchr(legacy.zova_path, '/');
        zova_base = zova_base ? zova_base + 1 : legacy.zova_path;
        char quarantined_member[MIGRATION_PATH_MAX * 2];
        snprintf(quarantined_member, sizeof(quarantined_member), "%s/%s", quarantine, db_base);
        ASSERT_FALSE(migration_artifact_stat(quarantined_member).exists);
        snprintf(quarantined_member, sizeof(quarantined_member), "%s/%s", quarantine, zova_base);
        ASSERT_FALSE(migration_artifact_stat(quarantined_member).exists);
        for (int base = 0; base < 2; base++)
            for (int suffix = 0; suffix < 3; suffix++) {
                snprintf(quarantined_member, sizeof(quarantined_member), "%s/%s%s", quarantine,
                         base == 0 ? db_base : zova_base, suffixes[suffix]);
                ASSERT_FALSE(migration_artifact_stat(quarantined_member).exists);
            }
        migration_fixture_cleanup(&legacy); cbm_unlink(target);
    }
    PASS();
}

TEST(zova_migration_cleanup_refuses_live_quarantine_ambiguity) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-ambiguous"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-ambiguous-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, rejected = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_member_0", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    char parent[MIGRATION_PATH_MAX], quarantine_db[MIGRATION_PATH_MAX * 2];
    snprintf(parent, sizeof(parent), "%s", legacy.db_path);
    char *slash = strrchr(parent, '/'); ASSERT_NOT_NULL(slash); *slash = '\0';
    const char *db_base = strrchr(legacy.db_path, '/'); db_base = db_base ? db_base + 1 : legacy.db_path;
    snprintf(quarantine_db, sizeof(quarantine_db), "%s/.zova-retired/%s-g%lld/%s", parent,
             active.workspace_id, (long long)active.source_generation, db_base);
    FILE *source = fopen(quarantine_db, "rb"), *duplicate = fopen(legacy.db_path, "wb");
    ASSERT_NOT_NULL(source); ASSERT_NOT_NULL(duplicate);
    char bytes[4096]; size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), source)) > 0)
        ASSERT_EQ(fwrite(bytes, 1, count, duplicate), count);
    ASSERT_EQ(fclose(source), 0); ASSERT_EQ(fclose(duplicate), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(quarantine_db).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    cbm_unlink(legacy.db_path); cbm_unlink(quarantine_db); migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_unexpected_quarantine_member) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-extra-member"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-extra-member-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, rejected = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_member_0", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    char parent[MIGRATION_PATH_MAX], unexpected[MIGRATION_PATH_MAX * 2];
    snprintf(parent, sizeof(parent), "%s", legacy.db_path);
    char *slash = strrchr(parent, '/'); ASSERT_NOT_NULL(slash); *slash = '\0';
    snprintf(unexpected, sizeof(unexpected), "%s/.zova-retired/%s-g%lld/unexpected", parent,
             active.workspace_id, (long long)active.source_generation);
    FILE *extra = fopen(unexpected, "wb"); ASSERT_NOT_NULL(extra);
    ASSERT_FALSE(fputs("tamper", extra) == EOF); ASSERT_EQ(fclose(extra), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(unexpected).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    cbm_unlink(unexpected); migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_mismatched_audit_identity) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-audit-id"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-audit-id-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, rejected = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_member_0", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    char parent[MIGRATION_PATH_MAX], audit_path[MIGRATION_PATH_MAX * 2];
    snprintf(parent, sizeof(parent), "%s", legacy.db_path);
    char *slash = strrchr(parent, '/'); ASSERT_NOT_NULL(slash); *slash = '\0';
    snprintf(audit_path, sizeof(audit_path), "%s/.zova-retired/%s-g%lld/manifest.json", parent,
             active.workspace_id, (long long)active.source_generation);
    FILE *audit = fopen(audit_path, "rb"); ASSERT_NOT_NULL(audit);
    char text[16384] = {0}; size_t length = fread(text, 1, sizeof(text) - 1, audit);
    ASSERT_GT(length, 0); ASSERT_EQ(fclose(audit), 0);
    char *workspace = strstr(text, active.workspace_id); ASSERT_NOT_NULL(workspace);
    workspace[0] = workspace[0] == 'x' ? 'y' : 'x';
    audit = fopen(audit_path, "wb"); ASSERT_NOT_NULL(audit);
    ASSERT_EQ(fwrite(text, 1, length, audit), length); ASSERT_EQ(fclose(audit), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_mismatched_member_size) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-audit-size"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-audit-size-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, rejected = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_member_0", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    char parent[MIGRATION_PATH_MAX], audit_path[MIGRATION_PATH_MAX * 2];
    snprintf(parent, sizeof(parent), "%s", legacy.db_path);
    char *slash = strrchr(parent, '/'); ASSERT_NOT_NULL(slash); *slash = '\0';
    snprintf(audit_path, sizeof(audit_path), "%s/.zova-retired/%s-g%lld/manifest.json", parent,
             active.workspace_id, (long long)active.source_generation);
    FILE *audit = fopen(audit_path, "rb"); ASSERT_NOT_NULL(audit);
    char text[16384] = {0}; size_t length = fread(text, 1, sizeof(text) - 1, audit);
    ASSERT_GT(length, 0); ASSERT_EQ(fclose(audit), 0);
    char *size = strstr(text, "\"size\":"); ASSERT_NOT_NULL(size);
    size += strlen("\"size\":");
    ASSERT_TRUE(*size >= '0' && *size <= '9');
    *size = *size == '9' ? '8' : (char)(*size + 1);
    audit = fopen(audit_path, "wb"); ASSERT_NOT_NULL(audit);
    ASSERT_EQ(fwrite(text, 1, length, audit), length); ASSERT_EQ(fclose(audit), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

static int migration_cleanup_interrupt_first_member(
    migration_legacy_fixture_t *legacy, const char *name, char target[MIGRATION_PATH_MAX],
    cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *active) {
    if (migration_fixture_create(legacy, name) != 0) return -1;
    migration_path(target, MIGRATION_PATH_MAX, name);
    *request = (cbm_zova_migration_request_t){
        .source_db_path = legacy->db_path,
        .source_zova_path = legacy->zova_path,
        .target_zova_path = target,
        .canonical_root = legacy->root_path,
    };
    if (cbm_zova_migration_run(request, active) != CBM_ZOVA_MIGRATION_OK ||
        setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_member_0", 1) != 0)
        return -1;
    cbm_zova_migration_report_t interrupted = {0};
    int code = cbm_zova_migration_cleanup(request, active->workspace_id, &interrupted);
    (void)unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    return code == CBM_ZOVA_MIGRATION_CLEANUP_REFUSED ? 0 : -1;
}

static int migration_cleanup_interrupted_paths(
    const migration_legacy_fixture_t *legacy, const cbm_zova_migration_report_t *active,
    char quarantine[MIGRATION_PATH_MAX * 2], char audit[MIGRATION_PATH_MAX * 2],
    char quarantined_db[MIGRATION_PATH_MAX * 2]) {
    char parent[MIGRATION_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", legacy->db_path);
    char *slash = strrchr(parent, '/');
    if (!slash) return -1;
    *slash = '\0';
    const char *db_leaf = strrchr(legacy->db_path, '/');
    db_leaf = db_leaf ? db_leaf + 1 : legacy->db_path;
    snprintf(quarantine, MIGRATION_PATH_MAX * 2, "%s/.zova-retired/%s-g%lld", parent,
             active->workspace_id, (long long)active->source_generation);
    snprintf(audit, MIGRATION_PATH_MAX * 2, "%s/manifest.json", quarantine);
    snprintf(quarantined_db, MIGRATION_PATH_MAX * 2, "%s/%s", quarantine, db_leaf);
    return 0;
}

TEST(zova_migration_cleanup_refuses_tampered_quarantined_bytes) {
    migration_legacy_fixture_t legacy;
    char target[MIGRATION_PATH_MAX], quarantine[MIGRATION_PATH_MAX * 2];
    char audit[MIGRATION_PATH_MAX * 2], quarantined_db[MIGRATION_PATH_MAX * 2];
    cbm_zova_migration_request_t request;
    cbm_zova_migration_report_t active = {0}, rejected = {0};
    ASSERT_EQ(migration_cleanup_interrupt_first_member(&legacy, "cleanup-byte-tamper", target,
                                                       &request, &active),
              0);
    ASSERT_EQ(migration_cleanup_interrupted_paths(&legacy, &active, quarantine, audit,
                                                  quarantined_db),
              0);
    FILE *member = fopen(quarantined_db, "ab");
    ASSERT_NOT_NULL(member);
    ASSERT_FALSE(fputs("tamper", member) == EOF);
    ASSERT_EQ(fclose(member), 0);
    struct stat before, after;
    ASSERT_EQ(stat(quarantined_db, &before), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(stat(quarantined_db, &after), 0);
    ASSERT_EQ(after.st_size, before.st_size);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"),
              1);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_expected_member_missing_from_both_paths) {
    migration_legacy_fixture_t legacy;
    char target[MIGRATION_PATH_MAX], quarantine[MIGRATION_PATH_MAX * 2];
    char audit[MIGRATION_PATH_MAX * 2], quarantined_db[MIGRATION_PATH_MAX * 2];
    cbm_zova_migration_request_t request;
    cbm_zova_migration_report_t active = {0}, rejected = {0};
    ASSERT_EQ(migration_cleanup_interrupt_first_member(&legacy, "cleanup-both-missing", target,
                                                       &request, &active),
              0);
    ASSERT_EQ(migration_cleanup_interrupted_paths(&legacy, &active, quarantine, audit,
                                                  quarantined_db),
              0);
    ASSERT_EQ(remove(quarantined_db), 0);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"),
              1);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_malformed_or_rewritten_audit) {
    for (int mutation = 0; mutation < 5; mutation++) {
        char name[64];
        snprintf(name, sizeof(name), "cleanup-audit-mutation-%d", mutation);
        migration_legacy_fixture_t legacy;
        char target[MIGRATION_PATH_MAX], quarantine[MIGRATION_PATH_MAX * 2];
        char audit_path[MIGRATION_PATH_MAX * 2], quarantined_db[MIGRATION_PATH_MAX * 2];
        cbm_zova_migration_request_t request;
        cbm_zova_migration_report_t active = {0}, rejected = {0};
        ASSERT_EQ(migration_cleanup_interrupt_first_member(&legacy, name, target, &request,
                                                           &active),
                  0);
        ASSERT_EQ(migration_cleanup_interrupted_paths(&legacy, &active, quarantine, audit_path,
                                                      quarantined_db),
                  0);
        FILE *audit = fopen(audit_path, "rb");
        ASSERT_NOT_NULL(audit);
        char text[16384] = {0};
        size_t length = fread(text, 1, sizeof(text) - 1, audit);
        ASSERT_GT(length, 0);
        ASSERT_EQ(fclose(audit), 0);
        if (mutation == 0) {
            snprintf(text, sizeof(text), "{");
            length = 1;
        } else {
            const char *needles[] = {NULL, "\"source\":\"", "\"destination\":\"",
                                     "\"sha256\":\"", "\"index\":0"};
            char *field = strstr(text, needles[mutation]);
            ASSERT_NOT_NULL(field);
            field += strlen(needles[mutation]);
            if (mutation == 4) field[-1] = '7';
            else if (mutation == 3) field[0] = field[0] == '0' ? '1' : '0';
            else field[0] = field[0] == 'x' ? 'y' : 'x';
        }
        audit = fopen(audit_path, "wb");
        ASSERT_NOT_NULL(audit);
        ASSERT_EQ(fwrite(text, 1, length, audit), length);
        ASSERT_EQ(fclose(audit), 0);
        ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
                  CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
        ASSERT_TRUE(migration_artifact_stat(quarantined_db).exists);
        ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
        ASSERT_EQ(migration_scalar(target,
                                   "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                                   "state='cleanup_pending'"),
                  1);
        migration_fixture_cleanup(&legacy); cbm_unlink(target);
    }
    PASS();
}

TEST(zova_migration_cleanup_refuses_changed_source_identity) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-source-drift"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-source-drift-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, rejected = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(migration_fixture_advance(&legacy), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"),
              1);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_target_digest_or_readiness_drift) {
    const char *mutations[] = {
        "UPDATE cbm_nodes_v1 SET properties='{\"tampered\":true}'",
        "UPDATE cbm_database_generation_v1 SET state='building' WHERE state='ready'",
    };
    for (int mutation = 0; mutation < 2; mutation++) {
        char name[64];
        snprintf(name, sizeof(name), "cleanup-target-drift-%d", mutation);
        migration_legacy_fixture_t legacy;
        ASSERT_EQ(migration_fixture_create(&legacy, name), 0);
        char target[MIGRATION_PATH_MAX];
        migration_path(target, sizeof(target), name);
        cbm_zova_migration_request_t request = {
            .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
            .target_zova_path = target, .canonical_root = legacy.root_path,
        };
        cbm_zova_migration_report_t active = {0}, rejected = {0};
        ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
        ASSERT_EQ(migration_sql(target, mutations[mutation]), 0);
        ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
                  CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
        ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
        ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
        ASSERT_EQ(migration_scalar(target,
                                   "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                                   "state='active'"),
                  1);
        migration_fixture_cleanup(&legacy); cbm_unlink(target);
    }
    PASS();
}

TEST(zova_migration_cleanup_resumes_after_target_reverify_fault) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-reverify-fault"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-reverify-fault-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_target_reverify", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"), 1);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_FALSE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_resumes_after_retired_commit_fault) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-retired-fault"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-retired-fault-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_retired_commit", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='retired'"), 1);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_FALSE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_resumes_during_purge) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-purge-fault"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-purge-fault-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, interrupted = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "cleanup_after_purge_0", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &interrupted),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='retired'"), 1);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

#ifndef _WIN32
static int migration_cleanup_crash_child(const cbm_zova_migration_request_t *request,
                                         const char *workspace_id, const char *phase) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        (void)setenv("CBM_ZOVA_TEST_CRASH_PHASE", phase, 1);
        cbm_zova_migration_report_t report = {0};
        (void)cbm_zova_migration_cleanup(request, workspace_id, &report);
        _exit(87);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86
               ? 0
               : -1;
}

TEST(zova_migration_cleanup_recovers_real_process_death_and_keeps_target_readable) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-real-crash"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-real-crash-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(migration_cleanup_crash_child(&request, active.workspace_id,
                                            "cleanup_after_member_0"),
              0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='cleanup_pending'"),
              1);
    zova_database *target_reader = NULL;
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = target, .out_db = &target_reader}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db = target_reader,
                  .sql = "SELECT qualified_name FROM cbm_nodes_v1 ORDER BY qualified_name"}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_close(target_reader), ZOVA_OK);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    ASSERT_FALSE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_FALSE(migration_artifact_stat(legacy.zova_path).exists);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_recovers_preledger_marker_crash_and_rejects_corruption) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-marker-crash"), 0);
    char target[MIGRATION_PATH_MAX], marker[MIGRATION_PATH_MAX + 32];
    migration_path(target, sizeof(target), "cleanup-marker-crash-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, rejected = {0}, resumed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(migration_cleanup_crash_child(&request, active.workspace_id,
                                            "cleanup_after_marker"),
              0);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"),
              1);
    snprintf(marker, sizeof(marker), "%s.cbm-cleanup-pending", legacy.db_path);
    FILE *file = fopen(marker, "rb");
    ASSERT_NOT_NULL(file);
    char valid_marker[4096] = {0};
    size_t valid_length = fread(valid_marker, 1, sizeof(valid_marker) - 1, file);
    ASSERT_GT(valid_length, 0);
    ASSERT_EQ(fclose(file), 0);
    file = fopen(marker, "wb");
    ASSERT_NOT_NULL(file);
    ASSERT_EQ(fwrite("malformed\n", 1, 10, file), 10);
    ASSERT_EQ(fclose(file), 0);
    ASSERT_NULL(cbm_store_open_path(legacy.db_path));
    ASSERT_NULL(cbm_store_open_path_query(legacy.db_path));
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &rejected),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    file = fopen(marker, "wb");
    ASSERT_NOT_NULL(file);
    ASSERT_EQ(fwrite(valid_marker, 1, valid_length, file), valid_length);
    ASSERT_EQ(fclose(file), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &resumed),
              CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(resumed.state, CBM_ZOVA_MIGRATION_RETIRED);
    ASSERT_FALSE(migration_artifact_stat(marker).exists);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_symlinked_source_without_writes) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-symlink"), 0);
    char real_zova[MIGRATION_PATH_MAX], target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-symlink-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    snprintf(real_zova, sizeof(real_zova), "%s.real", legacy.zova_path);
    ASSERT_EQ(rename(legacy.zova_path, real_zova), 0);
    ASSERT_EQ(symlink(real_zova, legacy.zova_path), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    struct stat link_stat;
    ASSERT_EQ(lstat(legacy.zova_path, &link_stat), 0);
    ASSERT_TRUE(S_ISLNK(link_stat.st_mode));
    ASSERT_TRUE(migration_artifact_stat(real_zova).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"), 1);
    cbm_unlink(legacy.zova_path);
    cbm_unlink(real_zova);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_active_legacy_reader) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-reader"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-reader-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    sqlite3 *reader = NULL;
    ASSERT_EQ(sqlite3_open_v2(legacy.db_path, &reader, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(reader, "BEGIN; SELECT count(*) FROM nodes;", NULL, NULL, NULL),
              SQLITE_OK);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"), 1);
    ASSERT_EQ(sqlite3_close(reader), SQLITE_OK);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_active_legacy_writer) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-writer"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-writer-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    sqlite3 *writer = NULL;
    ASSERT_EQ(sqlite3_open_v2(legacy.db_path, &writer, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(writer, "BEGIN IMMEDIATE", NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(sqlite3_exec(writer, "ROLLBACK", NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(writer), SQLITE_OK);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"),
              1);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_linked_optional_members) {
    for (int linked_kind = 0; linked_kind < 2; linked_kind++) {
        char name[64];
        snprintf(name, sizeof(name), "cleanup-sidecar-link-%d", linked_kind);
        migration_legacy_fixture_t legacy;
        ASSERT_EQ(migration_fixture_create(&legacy, name), 0);
        char target[MIGRATION_PATH_MAX], sidecar[MIGRATION_PATH_MAX];
        char backing[MIGRATION_PATH_MAX], alias[MIGRATION_PATH_MAX];
        migration_path(target, sizeof(target), name);
        snprintf(sidecar, sizeof(sidecar), "%s-journal", legacy.zova_path);
        snprintf(backing, sizeof(backing), "%s.backing", sidecar);
        snprintf(alias, sizeof(alias), "%s.alias", sidecar);
        cbm_unlink(sidecar); cbm_unlink(backing); cbm_unlink(alias);
        cbm_zova_migration_request_t request = {
            .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
            .target_zova_path = target, .canonical_root = legacy.root_path,
        };
        cbm_zova_migration_report_t active = {0}, cleanup = {0};
        ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
        FILE *file = fopen(backing, "wb");
        ASSERT_NOT_NULL(file);
        ASSERT_FALSE(fputs("recovery", file) == EOF);
        ASSERT_EQ(fclose(file), 0);
        if (linked_kind == 0) ASSERT_EQ(symlink(backing, sidecar), 0);
        else {
            ASSERT_EQ(link(backing, sidecar), 0);
            ASSERT_EQ(link(backing, alias), 0);
        }
        ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
                  CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
        ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
        ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
        ASSERT_TRUE(migration_artifact_stat(sidecar).exists);
        ASSERT_EQ(migration_scalar(target,
                                   "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                                   "state='active'"),
                  1);
        cbm_unlink(sidecar); cbm_unlink(backing); cbm_unlink(alias);
        migration_fixture_cleanup(&legacy); cbm_unlink(target);
    }
    PASS();
}

TEST(zova_migration_cleanup_refuses_cross_device_quarantine) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-device"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-device-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_CLEANUP_CROSS_DEVICE", "1", 1), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_CLEANUP_CROSS_DEVICE"), 0);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE "
                               "state='active'"), 1);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_symlinked_parent_component) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-parent-link"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-parent-link-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    char real_dir[MIGRATION_PATH_MAX], link_dir[MIGRATION_PATH_MAX];
    snprintf(real_dir, sizeof(real_dir), "%s/cbm-cleanup-real-%d", migration_tmpdir(), (int)getpid());
    snprintf(link_dir, sizeof(link_dir), "%s/cbm-cleanup-link-%d", migration_tmpdir(), (int)getpid());
    cbm_rmdir(real_dir); cbm_unlink(link_dir);
    ASSERT_EQ(cbm_mkdir(real_dir), 0);
    const char *db_base = strrchr(legacy.db_path, '/'); db_base = db_base ? db_base + 1 : legacy.db_path;
    const char *zova_base = strrchr(legacy.zova_path, '/'); zova_base = zova_base ? zova_base + 1 : legacy.zova_path;
    char real_db[MIGRATION_PATH_MAX], real_zova[MIGRATION_PATH_MAX];
    char linked_db[MIGRATION_PATH_MAX], linked_zova[MIGRATION_PATH_MAX];
    snprintf(real_db, sizeof(real_db), "%s/%s", real_dir, db_base);
    snprintf(real_zova, sizeof(real_zova), "%s/%s", real_dir, zova_base);
    snprintf(linked_db, sizeof(linked_db), "%s/%s", link_dir, db_base);
    snprintf(linked_zova, sizeof(linked_zova), "%s/%s", link_dir, zova_base);
    ASSERT_EQ(rename(legacy.db_path, real_db), 0);
    ASSERT_EQ(rename(legacy.zova_path, real_zova), 0);
    ASSERT_EQ(symlink(real_dir, link_dir), 0);
    request.source_db_path = linked_db; request.source_zova_path = linked_zova;
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(real_db).exists);
    ASSERT_TRUE(migration_artifact_stat(real_zova).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE state='active'"), 1);
    cbm_unlink(link_dir); cbm_unlink(real_db); cbm_unlink(real_zova); cbm_rmdir(real_dir);
    migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}

TEST(zova_migration_cleanup_refuses_source_outside_configured_cache) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-outside-cache"), 0);
    char target[MIGRATION_PATH_MAX], cache[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-outside-cache-target");
    snprintf(cache, sizeof(cache), "%s/cbm-cleanup-approved-%d", migration_tmpdir(), (int)getpid());
    cbm_rmdir(cache);
    ASSERT_EQ(cbm_mkdir(cache), 0);
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(legacy.zova_path).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE state='active'"), 1);
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    migration_fixture_cleanup(&legacy); cbm_unlink(target); cbm_rmdir(cache);
    PASS();
}

TEST(zova_migration_cleanup_refuses_hardlinked_core) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "cleanup-hardlink"), 0);
    char target[MIGRATION_PATH_MAX], alias[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "cleanup-hardlink-target");
    snprintf(alias, sizeof(alias), "%s.alias", legacy.db_path);
    cbm_unlink(alias);
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path, .source_zova_path = legacy.zova_path,
        .target_zova_path = target, .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, cleanup = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(link(legacy.db_path, alias), 0);
    ASSERT_EQ(cbm_zova_migration_cleanup(&request, active.workspace_id, &cleanup),
              CBM_ZOVA_MIGRATION_CLEANUP_REFUSED);
    ASSERT_TRUE(migration_artifact_stat(legacy.db_path).exists);
    ASSERT_TRUE(migration_artifact_stat(alias).exists);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT count(*) FROM cbm_workspace_migrations_v1 WHERE state='active'"), 1);
    cbm_unlink(alias); migration_fixture_cleanup(&legacy); cbm_unlink(target);
    PASS();
}
#endif

TEST(zova_migration_run_records_normal_failure_and_rejects_corrupt_active) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "run-failed"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "run-failed-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    ASSERT_EQ(setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_after_metadata", 1), 0);
    cbm_zova_migration_report_t failed = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &failed), CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    ASSERT_EQ(unsetenv("CBM_ZOVA_TEST_FAIL_PHASE"), 0);
    cbm_zova_migration_report_t status = {0};
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_FAILED);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 0);
    cbm_zova_migration_report_t active = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(migration_sql(target,
                            "UPDATE cbm_generation_integrity_v2 SET metadata_sha256='broken'"),
              0);
    cbm_zova_migration_report_t rejected = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &rejected),
              CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 1);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_rollback_revalidates_and_reactivates_without_republish) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "rollback"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "rollback-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0}, rolled_back = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(cbm_zova_migration_rollback(&request, &rolled_back), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(rolled_back.state, CBM_ZOVA_MIGRATION_ROLLED_BACK);
    ASSERT_EQ(rolled_back.target_generation, active.target_generation);
    ASSERT_EQ(migration_scalar(target,
                               "SELECT active_generation FROM cbm_workspace_registry LIMIT 1"),
              active.target_generation);
    cbm_zova_migration_report_t reactivated = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &reactivated), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(reactivated.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_EQ(reactivated.target_generation, active.target_generation);
    ASSERT_EQ(migration_scalar(target, "SELECT count(*) FROM cbm_database_generation_v1"), 1);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_rollback_rejects_changed_retained_source) {
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "rollback-changed"), 0);
    char target[MIGRATION_PATH_MAX];
    migration_path(target, sizeof(target), "rollback-changed-target");
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(migration_sql(
                  legacy.db_path,
                  "UPDATE cbm_zova_sidecar_generation_v1 SET generation=generation+1 WHERE id=1"),
              0);
    cbm_zova_migration_report_t rejected = {0};
    ASSERT_EQ(cbm_zova_migration_rollback(&request, &rejected),
              CBM_ZOVA_MIGRATION_SOURCE_NOT_READY);
    cbm_zova_migration_report_t status = {0};
    ASSERT_EQ(cbm_zova_migration_status(&request, &status), CBM_ZOVA_MIGRATION_OK);
    ASSERT_EQ(status.state, CBM_ZOVA_MIGRATION_ACTIVE);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}

TEST(zova_migration_route_uses_per_project_migration_state) {
    char cache[MIGRATION_PATH_MAX];
    snprintf(cache, sizeof(cache), "%s/cbm-migration-route-%d", cbm_tmpdir(), (int)getpid());
    (void)cbm_mkdir(cache);
    char target[MIGRATION_PATH_MAX];
    snprintf(target, sizeof(target), "%s/cbm.zova", cache);
    cbm_unlink(target);
    migration_legacy_fixture_t legacy;
    ASSERT_EQ(migration_fixture_create(&legacy, "route-state"), 0);
    cbm_zova_migration_request_t request = {
        .source_db_path = legacy.db_path,
        .source_zova_path = legacy.zova_path,
        .target_zova_path = target,
        .canonical_root = legacy.root_path,
    };
    cbm_zova_migration_report_t active = {0};
    ASSERT_EQ(cbm_zova_migration_run(&request, &active), CBM_ZOVA_MIGRATION_OK);
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    const char *saved_flag = getenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    char *saved_flag_copy = saved_flag ? strdup(saved_flag) : NULL;
    ASSERT_EQ(setenv("CBM_CACHE_DIR", cache, 1), 0);
    ASSERT_EQ(setenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", "1", 1), 0);
    ASSERT_EQ(cbm_zova_route_for_project("legacy"), CBM_ZOVA_ROUTE_FULL_AUTHORITY);
    const char *legacy_states[] = {"prepared", "copying", "failed", "rolled_back"};
    for (size_t i = 0; i < sizeof(legacy_states) / sizeof(legacy_states[0]); i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "UPDATE cbm_workspace_migrations_v1 SET state='%s'",
                 legacy_states[i]);
        ASSERT_EQ(migration_sql(target, sql), 0);
        ASSERT_EQ(cbm_zova_route_for_project("legacy"), CBM_ZOVA_ROUTE_MIGRATION_LEGACY);
    }
    ASSERT_EQ(migration_sql(target,
                            "UPDATE cbm_workspace_migrations_v1 SET state='active'"),
              0);
    ASSERT_EQ(cbm_zova_route_for_project("legacy"), CBM_ZOVA_ROUTE_FULL_AUTHORITY);
    ASSERT_EQ(migration_sql(target,
                            "UPDATE cbm_workspace_migrations_v1 SET state='retired'"),
              0);
    ASSERT_EQ(cbm_zova_route_for_project("legacy"), CBM_ZOVA_ROUTE_FULL_AUTHORITY);
    ASSERT_EQ(migration_sql(target, "DELETE FROM cbm_workspace_migrations_v1"), 0);
    ASSERT_EQ(cbm_zova_route_for_project("legacy"), CBM_ZOVA_ROUTE_FULL_AUTHORITY);
    ASSERT_EQ(cbm_zova_route_for_project("absent"), CBM_ZOVA_ROUTE_FULL_AUTHORITY);
    if (saved_cache_copy) setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else unsetenv("CBM_CACHE_DIR");
    if (saved_flag_copy) setenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", saved_flag_copy, 1);
    else unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    free(saved_cache_copy);
    free(saved_flag_copy);
    migration_fixture_cleanup(&legacy);
    cbm_unlink(target);
    PASS();
}
#endif

#if CBM_WITH_ZOVA
TEST(zova_migration_empty_bootstrap_is_schema_v6) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path, sizeof(path), "empty");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT count(*) FROM pragma_table_info("
                               "'cbm_workspace_migrations_v1')"),
              17);
    cbm_unlink(path);
    PASS();
}

TEST(zova_migration_pre_v6_init_requires_repack_without_mutation) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path, sizeof(path), "upgrade");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(migration_sql(path,
                            "DROP TABLE cbm_workspace_health_v1;"
                            "DROP TABLE cbm_workspace_migrations_v1;"
                            "UPDATE cbm_database_schema_v1 SET schema_version=3 WHERE id=1;"),
              0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              3);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                               "name='cbm_workspace_migrations_v1'"),
              0);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                               "name='cbm_workspace_health_v1'"),
              0);
    cbm_unlink(path);
    PASS();
}

TEST(zova_migration_rejects_future_version_without_writes) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path, sizeof(path), "future");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(migration_sql(path,
                            "UPDATE cbm_database_schema_v1 SET schema_version=7 WHERE id=1;"
                            "INSERT INTO cbm_workspace_registry"
                            "(workspace_id,canonical_root,id_format_version,active_generation)"
                            "VALUES('future-workspace','/future',2,7);"),
              0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              7);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT active_generation FROM cbm_workspace_registry "
                               "WHERE workspace_id='future-workspace'"),
              7);
    cbm_unlink(path);
    PASS();
}

TEST(zova_migration_rejects_malformed_v3) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path, sizeof(path), "malformed");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(migration_sql(path,
                            "DROP TABLE cbm_workspace_health_v1;"
                            "DROP TABLE cbm_workspace_migrations_v1;"
                            "DROP TABLE cbm_nodes_v1;"
                            "UPDATE cbm_database_schema_v1 SET schema_version=3 WHERE id=1;"),
              0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              3);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT count(*) FROM sqlite_master WHERE "
                               "name='cbm_workspace_migrations_v1'"),
              0);
    cbm_unlink(path);
    PASS();
}

TEST(zova_migration_interrupted_v6_bootstrap_retries_atomically) {
    char path[MIGRATION_PATH_MAX];
    migration_path(path, sizeof(path), "interrupt");
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "schema_v6_before_commit", 1);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    ASSERT_EQ(migration_scalar(path,
                               "SELECT count(*) FROM sqlite_master WHERE "
                               "name='cbm_database_schema_v1'"),
              0);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(migration_scalar(path,
                               "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              6);
    cbm_unlink(path);
    PASS();
}
#endif

TEST(zova_migration_state_transition_contract) {
    static const bool allowed[7][7] = {
        [CBM_ZOVA_MIGRATION_PREPARED] = {[CBM_ZOVA_MIGRATION_COPYING] = true,
                                         [CBM_ZOVA_MIGRATION_FAILED] = true},
        [CBM_ZOVA_MIGRATION_COPYING] = {[CBM_ZOVA_MIGRATION_ACTIVE] = true,
                                        [CBM_ZOVA_MIGRATION_FAILED] = true},
        [CBM_ZOVA_MIGRATION_ACTIVE] = {[CBM_ZOVA_MIGRATION_ROLLED_BACK] = true,
                                       [CBM_ZOVA_MIGRATION_CLEANUP_PENDING] = true},
        [CBM_ZOVA_MIGRATION_FAILED] = {[CBM_ZOVA_MIGRATION_PREPARED] = true,
                                       [CBM_ZOVA_MIGRATION_COPYING] = true},
        [CBM_ZOVA_MIGRATION_ROLLED_BACK] = {[CBM_ZOVA_MIGRATION_ACTIVE] = true},
        [CBM_ZOVA_MIGRATION_CLEANUP_PENDING] = {[CBM_ZOVA_MIGRATION_RETIRED] = true},
    };
    static const char *names[] = {"prepared", "copying", "active", "failed", "rolled_back",
                                  "cleanup_pending", "retired"};
    for (int from = 0; from < 7; from++) {
        ASSERT_STR_EQ(cbm_zova_migration_state_name((cbm_zova_migration_state_t)from),
                      names[from]);
        for (int to = 0; to < 7; to++) {
            ASSERT_EQ(cbm_zova_migration_transition_allowed((cbm_zova_migration_state_t)from,
                                                             (cbm_zova_migration_state_t)to),
                      allowed[from][to]);
        }
    }
    ASSERT_NULL(cbm_zova_migration_state_name((cbm_zova_migration_state_t)-1));
    ASSERT_FALSE(cbm_zova_migration_transition_allowed((cbm_zova_migration_state_t)-1,
                                                        CBM_ZOVA_MIGRATION_PREPARED));
    PASS();
}

SUITE(zova_migration) {
#if CBM_WITH_ZOVA
    RUN_TEST(zova_migration_empty_bootstrap_is_schema_v6);
    RUN_TEST(zova_migration_pre_v6_init_requires_repack_without_mutation);
    RUN_TEST(zova_migration_rejects_future_version_without_writes);
    RUN_TEST(zova_migration_rejects_malformed_v3);
    RUN_TEST(zova_migration_interrupted_v6_bootstrap_retries_atomically);
    RUN_TEST(zova_migration_v5_snapshot_reads_two_workspaces_without_zova_open);
    RUN_TEST(zova_migration_repack_atomically_promotes_two_workspaces);
    RUN_TEST(zova_migration_repack_resumes_every_atomic_replacement_fault);
    RUN_TEST(zova_migration_v5_snapshot_rejects_corrupt_sources);
    RUN_TEST(zova_migration_legacy_snapshot_reads_one_ready_generation);
    RUN_TEST(zova_migration_legacy_snapshot_rejects_semantic_id_collision_and_fts_drift);
    RUN_TEST(zova_migration_legacy_snapshot_accepts_punctuation_only_fts_field);
    RUN_TEST(zova_migration_legacy_snapshot_rejects_missing_artifacts);
    RUN_TEST(zova_migration_legacy_snapshot_rejects_project_and_schema_mismatch);
    RUN_TEST(zova_migration_legacy_snapshot_rejects_generation_and_vector_mismatch);
    RUN_TEST(zova_migration_manifest_matches_uncommitted_target);
    RUN_TEST(zova_migration_manifest_rejects_workspace_ids_and_metadata_mismatch);
    RUN_TEST(zova_migration_manifest_rejects_graph_and_vector_mismatch);
    RUN_TEST(zova_migration_manifest_rejects_fts_row_and_ordered_result_mismatch);
    RUN_TEST(zova_migration_manifest_uses_public_fts_tie_breaker);
    RUN_TEST(zova_migration_manifest_preserves_existing_workspace_b);
    RUN_TEST(zova_migration_run_activates_once_and_is_idempotent);
    RUN_TEST(zova_migration_run_recovers_precommit_fault_without_visibility);
    RUN_TEST(zova_migration_run_recovers_every_named_fault_phase);
#ifndef _WIN32
    RUN_TEST(zova_migration_run_recovers_real_process_death);
#endif
    RUN_TEST(zova_migration_run_changed_source_preserves_prior_ready_then_advances_once);
    RUN_TEST(zova_migration_status_is_read_only_and_retired_survives_missing_source);
    const char *cleanup_saved_cache = getenv("CBM_CACHE_DIR");
    char *cleanup_saved_cache_copy = cleanup_saved_cache ? strdup(cleanup_saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", migration_tmpdir(), 1);
    char cleanup_db[MIGRATION_PATH_MAX];
    char cleanup_db_sidecar[MIGRATION_PATH_MAX + 8];
    snprintf(cleanup_db, sizeof(cleanup_db), "%s/cbm.zova", migration_tmpdir());
    cbm_unlink(cleanup_db);
    snprintf(cleanup_db_sidecar, sizeof(cleanup_db_sidecar), "%s-wal", cleanup_db);
    cbm_unlink(cleanup_db_sidecar);
    snprintf(cleanup_db_sidecar, sizeof(cleanup_db_sidecar), "%s-shm", cleanup_db);
    cbm_unlink(cleanup_db_sidecar);
    RUN_TEST(zova_migration_cleanup_requires_exact_confirmation);
    RUN_TEST(zova_migration_cleanup_valid_bundle_retires_source);
    RUN_TEST(zova_migration_cleanup_resumes_after_intent_fault);
    RUN_TEST(zova_migration_cleanup_resumes_after_first_member_move);
    RUN_TEST(zova_migration_cleanup_resumes_after_every_member_decision);
    RUN_TEST(zova_migration_cleanup_refuses_live_quarantine_ambiguity);
    RUN_TEST(zova_migration_cleanup_refuses_unexpected_quarantine_member);
    RUN_TEST(zova_migration_cleanup_refuses_mismatched_audit_identity);
    RUN_TEST(zova_migration_cleanup_refuses_mismatched_member_size);
    RUN_TEST(zova_migration_cleanup_refuses_tampered_quarantined_bytes);
    RUN_TEST(zova_migration_cleanup_refuses_expected_member_missing_from_both_paths);
    RUN_TEST(zova_migration_cleanup_refuses_malformed_or_rewritten_audit);
    RUN_TEST(zova_migration_cleanup_refuses_changed_source_identity);
    RUN_TEST(zova_migration_cleanup_refuses_target_digest_or_readiness_drift);
    RUN_TEST(zova_migration_cleanup_resumes_after_target_reverify_fault);
    RUN_TEST(zova_migration_cleanup_resumes_after_retired_commit_fault);
    RUN_TEST(zova_migration_cleanup_resumes_during_purge);
#ifndef _WIN32
    RUN_TEST(zova_migration_cleanup_recovers_real_process_death_and_keeps_target_readable);
    RUN_TEST(zova_migration_cleanup_recovers_preledger_marker_crash_and_rejects_corruption);
    RUN_TEST(zova_migration_cleanup_refuses_symlinked_source_without_writes);
    RUN_TEST(zova_migration_cleanup_refuses_active_legacy_reader);
    RUN_TEST(zova_migration_cleanup_refuses_active_legacy_writer);
    RUN_TEST(zova_migration_cleanup_refuses_linked_optional_members);
    RUN_TEST(zova_migration_cleanup_refuses_cross_device_quarantine);
    RUN_TEST(zova_migration_cleanup_refuses_symlinked_parent_component);
    RUN_TEST(zova_migration_cleanup_refuses_source_outside_configured_cache);
    RUN_TEST(zova_migration_cleanup_refuses_hardlinked_core);
#endif
    cbm_unlink(cleanup_db);
    snprintf(cleanup_db_sidecar, sizeof(cleanup_db_sidecar), "%s-wal", cleanup_db);
    cbm_unlink(cleanup_db_sidecar);
    snprintf(cleanup_db_sidecar, sizeof(cleanup_db_sidecar), "%s-shm", cleanup_db);
    cbm_unlink(cleanup_db_sidecar);
    if (cleanup_saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", cleanup_saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(cleanup_saved_cache_copy);
    RUN_TEST(zova_migration_run_records_normal_failure_and_rejects_corrupt_active);
    RUN_TEST(zova_migration_rollback_revalidates_and_reactivates_without_republish);
    RUN_TEST(zova_migration_rollback_rejects_changed_retained_source);
    RUN_TEST(zova_migration_route_uses_per_project_migration_state);
#endif
    RUN_TEST(zova_migration_state_transition_contract);
}
