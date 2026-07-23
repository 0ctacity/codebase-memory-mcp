#include "zova/cbm_zova_operations.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/sha256.h"
#include "foundation/log.h"
#include "pipeline/pipeline.h"
#include "zova/cbm_zova_migration.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_route.h"
#include "zova/cbm_zova_writer_gate.h"
#include "zova/cbm_zova_v5_snapshot.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sqlite3.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

static void operations_report(cbm_zova_operation_report_t *out,
                              cbm_zova_operation_code_t code,
                              const char *operation, const char *reason) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->code = code;
    snprintf(out->operation, sizeof(out->operation), "%s", operation ? operation : "");
    snprintf(out->reason, sizeof(out->reason), "%s", reason ? reason : "");
}

#if CBM_WITH_ZOVA
static int operations_step(zova_statement *statement, zova_step_result *out) {
    return zova_statement_step(&(zova_statement_step_request){
               .statement = statement, .out_result = out}) == ZOVA_OK
               ? 0
               : -1;
}

static int operations_query_i64(zova_database *db, const char *sql, int64_t *out) {
    zova_statement *statement = NULL;
    if (!db || !sql || !out ||
        zova_database_prepare(&(zova_database_prepare_request){
            .db = db, .sql = sql, .out_statement = &statement}) != ZOVA_OK ||
        !statement) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int rc = operations_step(statement, &step) == 0 && step == ZOVA_STEP_ROW &&
                     zova_statement_column_int64(&(zova_statement_column_int64_request){
                         .statement = statement, .index = 0, .out_value = out}) == ZOVA_OK
                 ? 0
                 : -1;
    (void)zova_statement_finalize(statement);
    return rc;
}

static int operations_quick_check(zova_database *db) {
    zova_statement *statement = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = db, .sql = "PRAGMA quick_check", .out_statement = &statement}) != ZOVA_OK ||
        !statement) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    zova_text text = {0};
    int rc = operations_step(statement, &step) == 0 && step == ZOVA_STEP_ROW &&
                     zova_statement_column_text(&(zova_statement_column_text_request){
                         .statement = statement, .index = 0, .out_text = &text}) == ZOVA_OK &&
                     text.data && text.len == 2 && memcmp(text.data, "ok", 2) == 0
                 ? 0
                 : -1;
    zova_text_free(&text);
    (void)zova_statement_finalize(statement);
    return rc;
}

static int operations_foreign_keys_ok(zova_database *db) {
    zova_statement *statement = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = db, .sql = "PRAGMA foreign_key_check", .out_statement = &statement}) !=
            ZOVA_OK ||
        !statement) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_ROW;
    int rc = operations_step(statement, &step) == 0 && step == ZOVA_STEP_DONE ? 0 : -1;
    (void)zova_statement_finalize(statement);
    return rc;
}

static int operations_verify_database(const char *path, int *out_schema_version) {
    zova_database *db = NULL;
    zova_message error = {0};
    int rc = -1;
    if (!path ||
        zova_database_open_with_options(&(zova_database_open_options_request){
            .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &db, .out_error_message = &error}) != ZOVA_OK ||
        !db) {
        zova_message_free(&error);
        return -1;
    }
    zova_message_free(&error);
    int64_t schema_version = 0;
    int64_t invalid_generations = 0;
    if (operations_query_i64(
            db, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
            &schema_version) == 0 &&
        schema_version == CBM_ZOVA_DATABASE_SCHEMA_VERSION && operations_quick_check(db) == 0 &&
        operations_foreign_keys_ok(db) == 0 &&
        operations_query_i64(
            db,
            "SELECT count(*) FROM cbm_workspace_registry w LEFT JOIN "
            "cbm_database_generation_v1 g ON g.workspace_key=w.workspace_key AND "
            "g.generation=w.active_generation AND g.state='ready' LEFT JOIN "
            "cbm_generation_integrity_v2 i ON i.workspace_key=w.workspace_key AND "
            "i.generation=w.active_generation WHERE w.active_generation<=0 OR "
            "g.workspace_key IS NULL OR i.workspace_key IS NULL",
            &invalid_generations) == 0 &&
        invalid_generations == 0) {
        rc = 0;
        if (out_schema_version) *out_schema_version = (int)schema_version;
    }
    (void)zova_database_close(db);
    return rc;
}

static int operations_parent_directory(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0 || snprintf(out, out_size, "%s", path) >= (int)out_size)
        return -1;
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *backslash = strrchr(out, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (!slash) return snprintf(out, out_size, ".") < (int)out_size ? 0 : -1;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return 0;
}

static int operations_sync_directory(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? 0 : -1;
#endif
}

static int operations_same_file(const char *left, const char *right) {
    struct stat left_stat, right_stat;
    if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) return 0;
#ifdef _WIN32
    return strcmp(left, right) == 0;
#else
    return left_stat.st_dev == right_stat.st_dev && left_stat.st_ino == right_stat.st_ino;
#endif
}

static int operations_path_is_regular_nosymlink(const char *path) {
    struct stat value;
#ifdef _WIN32
    return stat(path, &value) == 0 && S_ISREG(value.st_mode);
#else
    return cbm_lstat(path, &value) == 0 && S_ISREG(value.st_mode) && !S_ISLNK(value.st_mode);
#endif
}

static int operations_restore_path_state(const char *path) {
    struct stat value;
    if (cbm_lstat(path, &value) != 0) return 0;
#ifdef _WIN32
    return S_ISREG(value.st_mode) ? 1 : -1;
#else
    return S_ISREG(value.st_mode) && !S_ISLNK(value.st_mode) ? 1 : -1;
#endif
}

static int operations_replacement_fault(const char *operation, const char *phase,
                                        cbm_zova_operation_report_t *out) {
    if (!cbm_zova_migration_test_fault(phase)) return 0;
    operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, operation, phase);
    return 1;
}

typedef struct {
    int archive_version;
    int schema_version;
    uint64_t database_bytes;
    char database_sha256[CBM_SHA256_HEX_LEN + 1];
} operations_archive_manifest_t;

static int operations_file_sha256(const char *path, char out[CBM_SHA256_HEX_LEN + 1],
                                  uint64_t *out_bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    cbm_sha256_ctx context;
    cbm_sha256_init(&context);
    uint8_t buffer[65536];
    uint64_t bytes = 0;
    int rc = 0;
    while (!feof(file)) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count) {
            cbm_sha256_update(&context, buffer, count);
            bytes += count;
        }
        if (ferror(file)) {
            rc = -1;
            break;
        }
    }
    fclose(file);
    if (rc != 0) return -1;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&context, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
    if (out_bytes) *out_bytes = bytes;
    return 0;
}

static int operations_workspace_manifest_entry(zova_statement *statement, char *out,
                                                size_t out_size) {
    zova_text values[6] = {{0}};
    int64_t generation = 0;
    int rc = zova_statement_column_int64(&(zova_statement_column_int64_request){
                 .statement = statement, .index = 1, .out_value = &generation}) == ZOVA_OK
                 ? 0
                 : -1;
    const int columns[] = {0, 2, 3, 4, 5, 6};
    for (size_t i = 0; rc == 0 && i < 6; i++) {
        if (zova_statement_column_text(&(zova_statement_column_text_request){
                .statement = statement, .index = columns[i], .out_text = &values[i]}) != ZOVA_OK ||
            !values[i].data) {
            rc = -1;
        }
    }
    if (rc == 0) {
        int written = snprintf(
            out, out_size,
            "{\"workspace_id\":\"%.*s\",\"generation\":%lld,"
            "\"metadata_sha256\":\"%.*s\",\"fts_sha256\":\"%.*s\","
            "\"topology_sha256\":\"%.*s\",\"node_vector_sha256\":\"%.*s\","
            "\"token_vector_sha256\":\"%.*s\"}",
            (int)values[0].len, values[0].data, (long long)generation,
            (int)values[1].len, values[1].data, (int)values[2].len, values[2].data,
            (int)values[3].len, values[3].data, (int)values[4].len, values[4].data,
            (int)values[5].len, values[5].data);
        if (written < 0 || written >= (int)out_size) rc = -1;
    }
    for (size_t i = 0; i < 6; i++) zova_text_free(&values[i]);
    return rc;
}

static int operations_workspace_manifest_query(zova_database *db, zova_statement **out) {
    return zova_database_prepare(&(zova_database_prepare_request){
               .db = db,
               .sql = "SELECT r.workspace_id,r.active_generation,i.metadata_sha256,"
                      "i.fts_sha256,i.topology_sha256,i.node_vector_sha256,"
                      "i.token_vector_sha256 FROM cbm_workspace_registry r JOIN "
                      "cbm_generation_integrity_v2 i ON i.workspace_key=r.workspace_key AND "
                      "i.generation=r.active_generation ORDER BY r.workspace_id",
               .out_statement = out}) == ZOVA_OK && *out
               ? 0
               : -1;
}

static char *operations_json_escape_alloc(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    if (length > (SIZE_MAX - 1) / 6) return NULL;
    char *escaped = malloc(length * 6 + 1);
    if (!escaped) return NULL;
    size_t used = 0;
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        const char *short_escape = NULL;
        switch (*cursor) {
        case '"': short_escape = "\\\""; break;
        case '\\': short_escape = "\\\\"; break;
        case '\b': short_escape = "\\b"; break;
        case '\f': short_escape = "\\f"; break;
        case '\n': short_escape = "\\n"; break;
        case '\r': short_escape = "\\r"; break;
        case '\t': short_escape = "\\t"; break;
        default: break;
        }
        if (short_escape) {
            escaped[used++] = short_escape[0];
            escaped[used++] = short_escape[1];
        } else if (*cursor < 0x20) {
            escaped[used++] = '\\';
            escaped[used++] = 'u';
            escaped[used++] = '0';
            escaped[used++] = '0';
            escaped[used++] = hex[*cursor >> 4];
            escaped[used++] = hex[*cursor & 15];
        } else {
            escaped[used++] = (char)*cursor;
        }
    }
    escaped[used] = '\0';
    return escaped;
}

static int operations_workspace_archive_manifest_text(
    const operations_archive_manifest_t *manifest,
    const cbm_zova_workspace_snapshot_t *snapshot, char **out_text) {
    if (!manifest || !snapshot || !out_text) return -1;
    char *workspace_id = operations_json_escape_alloc(snapshot->workspace_id);
    char *root = operations_json_escape_alloc(snapshot->root_path);
    char *project = operations_json_escape_alloc(snapshot->project);
    char *model = operations_json_escape_alloc(snapshot->model_fingerprint);
    if (!workspace_id || !root || !project || !model) {
        free(workspace_id); free(root); free(project); free(model);
        return -1;
    }
    const cbm_zova_workspace_generation_result_t *integrity = &snapshot->integrity;
    const char *format =
        "{\"archive_version\":%d,\"schema_version\":%d,\"archive_kind\":\"workspace\","
        "\"database\":{\"path\":\"data.zova\",\"bytes\":%llu,\"sha256\":\"%s\"},"
        "\"workspace\":{\"workspace_id\":\"%s\",\"canonical_root\":\"%s\","
        "\"project\":\"%s\",\"model_fingerprint\":\"%s\",\"vector_dimensions\":%d,"
        "\"generation\":%lld,\"graph_nodes\":%llu,\"graph_edges\":%llu,"
        "\"metadata_nodes\":%llu,\"metadata_edges\":%llu,"
        "\"metadata_topology_edges\":%llu,\"fts_rows\":%llu,"
        "\"node_vector_rows\":%llu,\"token_vector_rows\":%llu,"
        "\"node_vectors\":%llu,\"token_vectors\":%llu,"
        "\"metadata_sha256\":\"%s\",\"fts_sha256\":\"%s\","
        "\"topology_sha256\":\"%s\",\"node_vector_sha256\":\"%s\","
        "\"token_vector_sha256\":\"%s\"}}\n";
    int needed = snprintf(
        NULL, 0, format, manifest->archive_version, manifest->schema_version,
        (unsigned long long)manifest->database_bytes, manifest->database_sha256, workspace_id,
        root, project, model, snapshot->vector_dimensions, (long long)snapshot->generation,
        (unsigned long long)integrity->graph_nodes,
        (unsigned long long)integrity->graph_edges,
        (unsigned long long)integrity->metadata_nodes,
        (unsigned long long)integrity->metadata_edges,
        (unsigned long long)integrity->metadata_topology_edges,
        (unsigned long long)integrity->fts_rows,
        (unsigned long long)integrity->node_vector_rows,
        (unsigned long long)integrity->token_vector_rows,
        (unsigned long long)integrity->node_vectors,
        (unsigned long long)integrity->token_vectors, integrity->metadata_sha256,
        integrity->fts_sha256, integrity->topology_sha256, integrity->node_vector_sha256,
        integrity->token_vector_sha256);
    char *text = needed >= 0 ? malloc((size_t)needed + 1) : NULL;
    if (!text || snprintf(
            text, (size_t)needed + 1, format, manifest->archive_version,
            manifest->schema_version, (unsigned long long)manifest->database_bytes,
            manifest->database_sha256, workspace_id, root, project, model,
            snapshot->vector_dimensions, (long long)snapshot->generation,
            (unsigned long long)integrity->graph_nodes,
            (unsigned long long)integrity->graph_edges,
            (unsigned long long)integrity->metadata_nodes,
            (unsigned long long)integrity->metadata_edges,
            (unsigned long long)integrity->metadata_topology_edges,
            (unsigned long long)integrity->fts_rows,
            (unsigned long long)integrity->node_vector_rows,
            (unsigned long long)integrity->token_vector_rows,
            (unsigned long long)integrity->node_vectors,
            (unsigned long long)integrity->token_vectors, integrity->metadata_sha256,
            integrity->fts_sha256, integrity->topology_sha256,
            integrity->node_vector_sha256, integrity->token_vector_sha256) != needed) {
        free(text);
        text = NULL;
    }
    free(workspace_id); free(root); free(project); free(model);
    if (!text) return -1;
    *out_text = text;
    return 0;
}

static int operations_write_workspace_archive_manifest(
    const char *manifest_path, const operations_archive_manifest_t *manifest,
    const cbm_zova_workspace_snapshot_t *snapshot) {
    char *text = NULL;
    if (operations_workspace_archive_manifest_text(manifest, snapshot, &text) != 0) return -1;
    FILE *file = fopen(manifest_path, "wb");
    size_t length = strlen(text);
    int rc = file && fwrite(text, 1, length, file) == length && fflush(file) == 0 ? 0 : -1;
#ifndef _WIN32
    if (rc == 0 && fsync(fileno(file)) != 0) rc = -1;
#endif
    if (file) fclose(file);
    free(text);
    return rc;
}

static int operations_write_archive_manifest(const char *data_path, const char *manifest_path,
                                             const operations_archive_manifest_t *manifest) {
    zova_database *db = NULL;
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = data_path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &db, .out_error_message = &error}) != ZOVA_OK || !db) {
        zova_message_free(&error);
        return -1;
    }
    zova_message_free(&error);
    zova_statement *statement = NULL;
    FILE *file = fopen(manifest_path, "wb");
    int rc = file && operations_workspace_manifest_query(db, &statement) == 0 ? 0 : -1;
    if (rc == 0 && fprintf(file,
                           "{\"archive_version\":%d,\"schema_version\":%d,"
                           "\"database\":{\"path\":\"data.zova\",\"bytes\":%llu,"
                           "\"sha256\":\"%s\"},\"workspaces\":[",
                           manifest->archive_version, manifest->schema_version,
                           (unsigned long long)manifest->database_bytes,
                           manifest->database_sha256) < 0)
        rc = -1;
    int first = 1;
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (operations_step(statement, &step) != 0) {
            rc = -1;
            break;
        }
        if (step == ZOVA_STEP_DONE) break;
        char entry[1024];
        if (operations_workspace_manifest_entry(statement, entry, sizeof(entry)) != 0 ||
            fprintf(file, "%s%s", first ? "" : ",", entry) < 0)
            rc = -1;
        first = 0;
    }
    if (rc == 0 && (fprintf(file, "]}\n") < 0 || fflush(file) != 0)) rc = -1;
#ifndef _WIN32
    if (rc == 0 && fsync(fileno(file)) != 0) rc = -1;
#endif
    if (statement) (void)zova_statement_finalize(statement);
    if (file) fclose(file);
    (void)zova_database_close(db);
    return rc;
}

static int operations_read_file(const char *path, char **out_text) {
    struct stat value;
    if (stat(path, &value) != 0 || value.st_size < 0 || value.st_size > 16 * 1024 * 1024)
        return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    char *text = malloc((size_t)value.st_size + 1);
    int rc = text && fread(text, 1, (size_t)value.st_size, file) == (size_t)value.st_size &&
                     !ferror(file)
                 ? 0
                 : -1;
    fclose(file);
    if (rc != 0) {
        free(text);
        return -1;
    }
    text[value.st_size] = '\0';
    *out_text = text;
    return 0;
}

static int operations_remove_closed_database_sidecars(const char *database_path) {
    static const char *const suffixes[] = {"-wal", "-shm", "-journal"};
    char path[4096];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (snprintf(path, sizeof(path), "%s%s", database_path, suffixes[i]) >=
            (int)sizeof(path))
            return -1;
        int state = operations_restore_path_state(path);
        if (state < 0 || (state == 1 && cbm_unlink(path) != 0)) return -1;
    }
    return 0;
}

static int operations_remove_closed_writer_lock(const char *database_path) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s.writer.lock", database_path) >= (int)sizeof(path))
        return -1;
    int state = operations_restore_path_state(path);
    return state < 0 || (state == 1 && cbm_unlink(path) != 0) ? -1 : 0;
}

static int operations_archive_has_exact_members(const char *archive) {
    cbm_dir_t *directory = cbm_opendir(archive);
    if (!directory) return -1;
    int data_seen = 0;
    int manifest_seen = 0;
    int valid = 1;
    cbm_dirent_t *entry = NULL;
    while (valid && (entry = cbm_readdir(directory)) != NULL) {
        int is_data = strcmp(entry->name, "data.zova") == 0;
        int is_manifest = strcmp(entry->name, "manifest.json") == 0;
        char path[4096];
        if ((!is_data && !is_manifest) ||
            snprintf(path, sizeof(path), "%s/%s", archive, entry->name) >=
                (int)sizeof(path) ||
            !operations_path_is_regular_nosymlink(path)) {
            valid = 0;
            break;
        }
        if (is_data) data_seen++;
        if (is_manifest) manifest_seen++;
    }
    cbm_closedir(directory);
    return valid && data_seen == 1 && manifest_seen == 1 ? 0 : -1;
}

static int operations_cleanup_verified_archive_reads(const char *archive) {
    char data[4096];
    if (snprintf(data, sizeof(data), "%s/data.zova", archive) >= (int)sizeof(data)) return -1;
    return operations_remove_closed_database_sidecars(data) == 0 &&
                   operations_archive_has_exact_members(archive) == 0
               ? 0 : -1;
}

static int operations_single_ready_workspace(const char *data_path,
                                             char out[CBM_ZOVA_WORKSPACE_ID_MAX]) {
    zova_database *db = NULL;
    zova_message error = {0};
    zova_statement *statement = NULL;
    int rc = zova_database_open_with_options(&(zova_database_open_options_request){
                 .path = data_path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                 .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db
                 ? 0 : -1;
    zova_message_free(&error);
    if (rc == 0)
        rc = zova_database_prepare(&(zova_database_prepare_request){
                 .db = db,
                 .sql = "SELECT r.workspace_id FROM cbm_workspace_registry r "
                        "JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key "
                        "AND g.generation=r.active_generation AND g.state='ready' "
                        "JOIN cbm_workspace_index_state_v1 s ON s.workspace_key=r.workspace_key "
                        "AND s.generation=r.active_generation ORDER BY r.workspace_id",
                 .out_statement = &statement}) == ZOVA_OK && statement ? 0 : -1;
    zova_step_result step = ZOVA_STEP_DONE;
    zova_text workspace = {0};
    if (rc == 0 &&
        (operations_step(statement, &step) != 0 || step != ZOVA_STEP_ROW ||
         zova_statement_column_text(&(zova_statement_column_text_request){
             .statement = statement, .index = 0, .out_text = &workspace}) != ZOVA_OK ||
         !workspace.data || workspace.len >= CBM_ZOVA_WORKSPACE_ID_MAX)) rc = -1;
    if (rc == 0) {
        memcpy(out, workspace.data, workspace.len);
        out[workspace.len] = '\0';
        if (operations_step(statement, &step) != 0 || step != ZOVA_STEP_DONE ||
            cbm_zova_workspace_id_validate(out) != 0) rc = -1;
    }
    zova_text_free(&workspace);
    if (statement) (void)zova_statement_finalize(statement);
    if (db) (void)zova_database_close(db);
    return rc;
}

static int operations_workspace_count(const char *path, int64_t *out) {
    zova_database *db = NULL;
    zova_message error = {0};
    int rc = zova_database_open_with_options(&(zova_database_open_options_request){
                 .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                 .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db
                 ? operations_query_i64(db, "SELECT count(*) FROM cbm_workspace_registry", out)
                 : -1;
    zova_message_free(&error);
    if (db) (void)zova_database_close(db);
    return rc;
}

static int operations_generation_matches(
    const cbm_zova_workspace_generation_result_t *actual,
    const cbm_zova_workspace_generation_result_t *expected) {
    int matches = actual->generation == expected->generation &&
           strcmp(actual->workspace_id, expected->workspace_id) == 0 &&
           actual->graph_nodes == expected->graph_nodes &&
           actual->graph_edges == expected->graph_edges &&
           actual->metadata_nodes == expected->metadata_nodes &&
           actual->metadata_edges == expected->metadata_edges &&
           actual->metadata_topology_edges == expected->metadata_topology_edges &&
           actual->fts_rows == expected->fts_rows &&
           actual->node_vector_rows == expected->node_vector_rows &&
           actual->token_vector_rows == expected->token_vector_rows &&
           actual->node_vectors == expected->node_vectors &&
           actual->token_vectors == expected->token_vectors &&
           strcmp(actual->metadata_sha256, expected->metadata_sha256) == 0 &&
           strcmp(actual->fts_sha256, expected->fts_sha256) == 0 &&
           strcmp(actual->topology_sha256, expected->topology_sha256) == 0 &&
           strcmp(actual->node_vector_sha256, expected->node_vector_sha256) == 0 &&
           strcmp(actual->token_vector_sha256, expected->token_vector_sha256) == 0;
    return matches;
}

/* 0 valid, -1 malformed/verification failure, -2 incompatible, -3 path alias. */
static int operations_verify_archive(const char *archive, const char *live_path,
                                     operations_archive_manifest_t *out) {
    struct stat value;
    if (cbm_lstat(archive, &value) != 0 || !S_ISDIR(value.st_mode)
#ifndef _WIN32
        || S_ISLNK(value.st_mode)
#endif
    )
        return -1;
    if (operations_archive_has_exact_members(archive) != 0) return -1;
    char data[4096], manifest_path[4096];
    if (snprintf(data, sizeof(data), "%s/data.zova", archive) >= (int)sizeof(data) ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", archive) >=
            (int)sizeof(manifest_path) ||
        !operations_path_is_regular_nosymlink(data) ||
        !operations_path_is_regular_nosymlink(manifest_path))
        return -1;
    if (live_path && operations_restore_path_state(live_path) == 1 &&
        operations_same_file(data, live_path))
        return -3;
    char *text = NULL;
    if (operations_read_file(manifest_path, &text) != 0) return -1;
    operations_archive_manifest_t parsed = {0};
    unsigned long long bytes = 0;
    int consumed = 0;
    int fields = sscanf(text,
                        "{\"archive_version\":%d,\"schema_version\":%d,"
                        "\"database\":{\"path\":\"data.zova\",\"bytes\":%llu,"
                        "\"sha256\":\"%64[0-9a-f]\"},\"workspaces\":[%n",
                        &parsed.archive_version, &parsed.schema_version, &bytes,
                        parsed.database_sha256, &consumed);
    parsed.database_bytes = (uint64_t)bytes;
    if (fields != 4 || strlen(parsed.database_sha256) != CBM_SHA256_HEX_LEN) {
        free(text);
        return -1;
    }
    if (parsed.archive_version != CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION ||
        parsed.schema_version != CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
        free(text);
        return -2;
    }
    char digest[CBM_SHA256_HEX_LEN + 1];
    uint64_t actual_bytes = 0;
    if (operations_file_sha256(data, digest, &actual_bytes) != 0 ||
        actual_bytes != parsed.database_bytes || strcmp(digest, parsed.database_sha256) != 0 ||
        operations_verify_database(data, NULL) != 0) {
        free(text);
        return -1;
    }
    zova_database *db = NULL;
    zova_message error = {0};
    zova_statement *statement = NULL;
    int rc = zova_database_open_with_options(&(zova_database_open_options_request){
                 .path = data, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                 .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db &&
                     operations_workspace_manifest_query(db, &statement) == 0
                 ? 0
                 : -1;
    zova_message_free(&error);
    const char *cursor = text + consumed;
    int first = 1;
    while (rc == 0) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (operations_step(statement, &step) != 0) {
            rc = -1;
            break;
        }
        if (step == ZOVA_STEP_DONE) break;
        char entry[1024];
        if (operations_workspace_manifest_entry(statement, entry, sizeof(entry)) != 0) {
            rc = -1;
            break;
        }
        if (!first) {
            if (*cursor != ',') {
                rc = -1;
                break;
            }
            cursor++;
        }
        size_t length = strlen(entry);
        if (strncmp(cursor, entry, length) != 0) {
            rc = -1;
            break;
        }
        cursor += length;
        first = 0;
    }
    if (rc == 0 && strcmp(cursor, "]}\n") != 0) rc = -1;
    if (statement) (void)zova_statement_finalize(statement);
    if (db) (void)zova_database_close(db);
    free(text);
    if (rc == 0 && out) *out = parsed;
    return rc;
}

/* 0 valid, -1 malformed/verification failure, -2 incompatible, -3 path alias. */
static int operations_verify_workspace_archive(const char *archive, const char *live_path,
                                               operations_archive_manifest_t *out) {
    struct stat value;
    if (cbm_lstat(archive, &value) != 0 || !S_ISDIR(value.st_mode)
#ifndef _WIN32
        || S_ISLNK(value.st_mode)
#endif
    ) return -1;
    if (operations_archive_has_exact_members(archive) != 0) return -1;
    char data[4096], manifest_path[4096];
    if (snprintf(data, sizeof(data), "%s/data.zova", archive) >= (int)sizeof(data) ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", archive) >=
            (int)sizeof(manifest_path) ||
        !operations_path_is_regular_nosymlink(data) ||
        !operations_path_is_regular_nosymlink(manifest_path)) return -1;
    if (live_path && operations_restore_path_state(live_path) == 1 &&
        operations_same_file(data, live_path)) return -3;
    char *text = NULL;
    if (operations_read_file(manifest_path, &text) != 0) return -1;
    operations_archive_manifest_t parsed = {0};
    unsigned long long bytes = 0;
    int consumed = 0;
    int fields = sscanf(text,
                        "{\"archive_version\":%d,\"schema_version\":%d,"
                        "\"archive_kind\":\"workspace\","
                        "\"database\":{\"path\":\"data.zova\",\"bytes\":%llu,"
                        "\"sha256\":\"%64[0-9a-f]\"},\"workspace\":%n",
                        &parsed.archive_version, &parsed.schema_version, &bytes,
                        parsed.database_sha256, &consumed);
    parsed.database_bytes = (uint64_t)bytes;
    if (fields != 4 || consumed <= 0 ||
        strlen(parsed.database_sha256) != CBM_SHA256_HEX_LEN) {
        free(text);
        return -1;
    }
    if (parsed.archive_version != CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION ||
        parsed.schema_version != CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
        free(text);
        return -2;
    }
    char digest[CBM_SHA256_HEX_LEN + 1];
    uint64_t actual_bytes = 0;
    if (operations_file_sha256(data, digest, &actual_bytes) != 0 ||
        actual_bytes != parsed.database_bytes || strcmp(digest, parsed.database_sha256) != 0 ||
        operations_verify_database(data, NULL) != 0) {
        free(text);
        return -1;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    cbm_zova_workspace_snapshot_t snapshot = {0};
    char *expected = NULL;
    int rc = operations_single_ready_workspace(data, workspace_id) == 0 &&
                     cbm_zova_repository_export_snapshot(data, workspace_id, &snapshot) == 0 &&
                     operations_workspace_archive_manifest_text(&parsed, &snapshot, &expected) == 0 &&
                     strcmp(text, expected) == 0
                 ? 0 : -1;
    free(expected);
    cbm_zova_workspace_snapshot_free(&snapshot);
    free(text);
    if (rc == 0 && out) *out = parsed;
    return rc;
}

static void operations_remove_partial_archive(const char *partial) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/manifest.json", partial) < (int)sizeof(path))
        (void)cbm_unlink(path);
    if (snprintf(path, sizeof(path), "%s/data.zova", partial) < (int)sizeof(path)) {
        (void)operations_remove_closed_database_sidecars(path);
        (void)operations_remove_closed_writer_lock(path);
        (void)cbm_unlink(path);
    }
    (void)rmdir(partial);
}

#endif

const char *cbm_zova_operation_code_name(cbm_zova_operation_code_t code) {
    switch (code) {
    case CBM_ZOVA_OPERATION_OK: return "ok";
    case CBM_ZOVA_OPERATION_NOOP: return "noop";
    case CBM_ZOVA_OPERATION_INVALID: return "invalid";
    case CBM_ZOVA_OPERATION_BUSY: return "busy";
    case CBM_ZOVA_OPERATION_INCOMPATIBLE: return "incompatible";
    case CBM_ZOVA_OPERATION_VERIFY_FAILED: return "verify_failed";
    case CBM_ZOVA_OPERATION_DISK_REFUSED: return "disk_refused";
    case CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED: return "confirmation_required";
    case CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED: return "workspace_rebuild_required";
    case CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED:
        return "whole_file_recovery_required";
    }
    return "unknown";
}

cbm_zova_operation_code_t cbm_zova_database_status(
    const char *path, cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "status", "invalid_argument");
    if (!path || !path[0] || !out) return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    operations_report(out, CBM_ZOVA_OPERATION_INCOMPATIBLE, "status", "zova_disabled");
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    struct stat database_stat = {0};
    if (!operations_path_is_regular_nosymlink(path) || stat(path, &database_stat) != 0 ||
        database_stat.st_size < 0) {
        operations_report(out, CBM_ZOVA_OPERATION_INVALID, "status", "database_not_regular");
        return CBM_ZOVA_OPERATION_INVALID;
    }
    char wal_path[4096];
    struct stat wal_stat = {0};
    int wal_present = 0;
    if (snprintf(wal_path, sizeof(wal_path), "%s-wal", path) >= (int)sizeof(wal_path)) {
        operations_report(out, CBM_ZOVA_OPERATION_INVALID, "status", "path_too_long");
        return CBM_ZOVA_OPERATION_INVALID;
    }
    if (cbm_lstat(wal_path, &wal_stat) == 0) {
        if (!operations_path_is_regular_nosymlink(wal_path) || wal_stat.st_size < 0) {
            operations_report(out, CBM_ZOVA_OPERATION_INVALID, "status", "wal_not_regular");
            return CBM_ZOVA_OPERATION_INVALID;
        }
        wal_present = 1;
    }

    uint64_t free_bytes = 0;
#ifdef _WIN32
    char parent[4096];
    ULARGE_INTEGER available = {0};
    if (operations_parent_directory(path, parent, sizeof(parent)) != 0 ||
        !GetDiskFreeSpaceExA(parent, &available, NULL, NULL)) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status",
                          "free_space_unavailable");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    free_bytes = (uint64_t)available.QuadPart;
#else
    struct statvfs filesystem = {0};
    if (statvfs(path, &filesystem) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status",
                          "free_space_unavailable");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    uint64_t fragment_size = filesystem.f_frsize ? (uint64_t)filesystem.f_frsize
                                                 : (uint64_t)filesystem.f_bsize;
    if (fragment_size == 0 || (uint64_t)filesystem.f_bavail > UINT64_MAX / fragment_size) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status",
                          "free_space_overflow");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    free_bytes = (uint64_t)filesystem.f_bavail * fragment_size;
#endif

    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    zova_database *db = NULL;
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &db, .out_error_message = &error}) != ZOVA_OK || !db) {
        zova_message_free(&error);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status", "database_open_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    zova_message_free(&error);
    int64_t schema_version = 0;
    int64_t invalid_generations = 0;
    int64_t page_size = -1;
    int64_t page_count = -1;
    int64_t freelist_count = -1;
    int schema_read = operations_query_i64(
        db, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1", &schema_version);
    if (schema_read != 0 || schema_version != CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
        (void)zova_database_close(db);
        if (schema_read == 0 && schema_version > CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
            operations_report(out, CBM_ZOVA_OPERATION_INCOMPATIBLE, "status",
                              "schema_version_incompatible");
            out->schema_version = schema_version > INT_MAX ? 0 : (int)schema_version;
            return CBM_ZOVA_OPERATION_INCOMPATIBLE;
        }
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status",
                          "current_schema_required");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    int verified = cbm_zova_user_database_schema_is_current(db) == 0 &&
                           operations_quick_check(db) == 0 &&
                           operations_foreign_keys_ok(db) == 0 &&
                           operations_query_i64(
                               db,
                               "SELECT count(*) FROM cbm_workspace_registry w LEFT JOIN "
                               "cbm_database_generation_v1 g ON g.workspace_key=w.workspace_key "
                               "AND g.generation=w.active_generation AND g.state='ready' LEFT JOIN "
                               "cbm_generation_integrity_v2 i ON i.workspace_key=w.workspace_key "
                               "AND i.generation=w.active_generation WHERE w.active_generation<=0 "
                               "OR g.workspace_key IS NULL OR i.workspace_key IS NULL",
                               &invalid_generations) == 0 &&
                           invalid_generations == 0 &&
                           operations_query_i64(db, "PRAGMA page_size", &page_size) == 0 &&
                           operations_query_i64(db, "PRAGMA page_count", &page_count) == 0 &&
                           operations_query_i64(db, "PRAGMA freelist_count", &freelist_count) == 0;
    (void)zova_database_close(db);
    if (!verified || page_size <= 0 || page_count < 0 || freelist_count < 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status",
                          "database_verification_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    uint64_t unsigned_page_size = (uint64_t)page_size;
    uint64_t unsigned_freelist_count = (uint64_t)freelist_count;
    if (unsigned_freelist_count > UINT64_MAX / unsigned_page_size) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "status",
                          "reclaimable_bytes_overflow");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "status", "ok");
    out->schema_version = (int)schema_version;
    out->database_bytes = (uint64_t)database_stat.st_size;
    out->wal_bytes = wal_present ? (uint64_t)wal_stat.st_size : 0;
    out->free_bytes = free_bytes;
    out->page_size = unsigned_page_size;
    out->page_count = (uint64_t)page_count;
    out->freelist_count = unsigned_freelist_count;
    out->reclaimable_bytes = unsigned_page_size * unsigned_freelist_count;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    return CBM_ZOVA_OPERATION_OK;
#endif
}

static void operations_copy_status_metrics(cbm_zova_operation_report_t *out,
                                           const cbm_zova_operation_report_t *status) {
    out->schema_version = status->schema_version;
    out->database_bytes = status->database_bytes;
    out->wal_bytes = status->wal_bytes;
    out->free_bytes = status->free_bytes;
    out->page_size = status->page_size;
    out->page_count = status->page_count;
    out->freelist_count = status->freelist_count;
}

cbm_zova_operation_code_t cbm_zova_workspace_delete(
    const char *path, const char *workspace_id, const char *confirmation,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "delete_workspace", "invalid_argument");
    if (!path || !path[0] || !workspace_id || !workspace_id[0] || !out)
        return CBM_ZOVA_OPERATION_INVALID;
    if (!confirmation || !confirmation[0]) {
        operations_report(out, CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED, "delete_workspace",
                          "confirmation_required");
        return CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED;
    }
    if (strcmp(workspace_id, confirmation) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED, "delete_workspace",
                          "confirmation_mismatch");
        return CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED;
    }
    if (cbm_zova_workspace_id_validate(workspace_id) != 0)
        return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    operations_report(out, CBM_ZOVA_OPERATION_INCOMPATIBLE, "delete_workspace", "zova_disabled");
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_operation_report_t before = {0};
    cbm_zova_operation_code_t status = cbm_zova_database_status(path, &before);
    if (status != CBM_ZOVA_OPERATION_OK) {
        operations_report(out, status, "delete_workspace", "status_failed");
        return status;
    }
    cbm_zova_workspace_snapshot_t snapshot = {0};
    if (cbm_zova_repository_export_snapshot(path, workspace_id, &snapshot) != 0) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
        operations_report(out, CBM_ZOVA_OPERATION_NOOP, "delete_workspace", "already_absent");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        operations_copy_status_metrics(out, &before);
        out->reclaimable_bytes = 0;
        out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                          ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
        return CBM_ZOVA_OPERATION_NOOP;
    }
    int64_t generation = snapshot.generation;
    cbm_zova_workspace_snapshot_free(&snapshot);
    if (cbm_zova_user_database_delete_workspace(path, workspace_id) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "delete_workspace",
                          "delete_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    cbm_zova_operation_report_t after = {0};
    if (cbm_zova_database_status(path, &after) != CBM_ZOVA_OPERATION_OK) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "delete_workspace",
                          "post_delete_status_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "delete_workspace", "ok");
    snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
    out->generation = generation;
    operations_copy_status_metrics(out, &after);
    out->reclaimable_bytes = after.reclaimable_bytes >= before.reclaimable_bytes
                                 ? after.reclaimable_bytes - before.reclaimable_bytes
                                 : 0;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    return CBM_ZOVA_OPERATION_OK;
#endif
}

#if CBM_WITH_ZOVA
static int operations_checkpoint_truncate(zova_database *db) {
    zova_statement *statement = NULL;
    if (!db || zova_database_prepare(&(zova_database_prepare_request){
                   .db = db, .sql = "PRAGMA wal_checkpoint(TRUNCATE)",
                   .out_statement = &statement}) != ZOVA_OK || !statement)
        return -1;
    zova_step_result step = ZOVA_STEP_DONE;
    int64_t busy = -1, log_frames = -1, checkpointed_frames = -1;
    int rc = operations_step(statement, &step) == 0 && step == ZOVA_STEP_ROW &&
                     zova_statement_column_int64(&(zova_statement_column_int64_request){
                         .statement = statement, .index = 0, .out_value = &busy}) == ZOVA_OK &&
                     zova_statement_column_int64(&(zova_statement_column_int64_request){
                         .statement = statement, .index = 1, .out_value = &log_frames}) ==
                         ZOVA_OK &&
                     zova_statement_column_int64(&(zova_statement_column_int64_request){
                         .statement = statement, .index = 2,
                         .out_value = &checkpointed_frames}) == ZOVA_OK &&
                     busy == 0 && log_frames == checkpointed_frames
                 ? 0 : -1;
    (void)zova_statement_finalize(statement);
    return rc;
}

static int operations_raw_sqlite_checkpoint_truncate(const char *path) {
    sqlite3 *db=NULL;
    int log_frames=0,checkpointed_frames=0;
    int rc=sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,NULL);
    if(rc==SQLITE_OK)sqlite3_busy_timeout(db,5000);
    if(rc==SQLITE_OK)rc=sqlite3_wal_checkpoint_v2(db,NULL,SQLITE_CHECKPOINT_TRUNCATE,
                                                  &log_frames,&checkpointed_frames);
    if(db)sqlite3_close(db);
    return rc==SQLITE_OK&&checkpointed_frames==log_frames?0:-1;
}

static int operations_compact_below_threshold(
    const cbm_zova_operation_report_t *status) {
    const uint64_t absolute_threshold = 64ULL * 1024ULL * 1024ULL;
    int below_absolute = status->reclaimable_bytes < absolute_threshold;
    uint64_t quotient = status->database_bytes / 10ULL;
    uint64_t remainder = status->database_bytes % 10ULL;
    int below_ratio = status->reclaimable_bytes < quotient ||
                      (status->reclaimable_bytes == quotient && remainder != 0);
    return below_absolute && below_ratio;
}

typedef struct {
    const char *operation;
    const char *after_temp_verification;
    const char *after_live_to_recovery;
    const char *after_temp_to_live;
    const char *before_recovery_cleanup;
    bool remove_sidecars;
    bool require_source_verified;
    bool keep_recovery;
} operations_replacement_options_t;

/* Complete or resume a verified same-directory database replacement. */
static int operations_replace_verified_database(
    const char *live, const char *temporary, const char *recovery, const char *parent,
    const operations_replacement_options_t *options, int *out_schema_version,
    cbm_zova_operation_report_t *out) {
    int live_state = operations_restore_path_state(live);
    int temp_state = operations_restore_path_state(temporary);
    int recovery_state = operations_restore_path_state(recovery);
    int valid_state = (live_state == 1 && temp_state == 1 && recovery_state == 0) ||
                      (live_state == 0 && temp_state == 1 && recovery_state == 1) ||
                      (live_state == 1 && temp_state == 0 && recovery_state == 1);
    if (live_state < 0 || temp_state < 0 || recovery_state < 0 || !valid_state)
        return -1;

    int schema_version = 0;
    if (live_state == 1 && temp_state == 1 && recovery_state == 0) {
        if (operations_verify_database(temporary, &schema_version) != 0)
            return -1;
        if (operations_replacement_fault(options->operation,
                                         options->after_temp_verification, out))
            return 1;
        if ((options->require_source_verified &&
             operations_verify_database(live, NULL) != 0) ||
            (options->remove_sidecars &&
             (operations_remove_closed_database_sidecars(live) != 0 ||
              operations_remove_closed_database_sidecars(temporary) != 0)) ||
            rename(live, recovery) != 0 || operations_sync_directory(parent) != 0)
            return -1;
        live_state = 0;
        recovery_state = 1;
        if (operations_replacement_fault(options->operation,
                                         options->after_live_to_recovery, out))
            return 1;
    }
    if (live_state == 0 && temp_state == 1 && recovery_state == 1) {
        if (operations_verify_database(temporary, &schema_version) != 0 ||
            (options->require_source_verified &&
             operations_verify_database(recovery, NULL) != 0) ||
            (options->remove_sidecars &&
             (operations_remove_closed_database_sidecars(temporary) != 0 ||
              operations_remove_closed_database_sidecars(recovery) != 0)) ||
            rename(temporary, live) != 0 || operations_sync_directory(parent) != 0)
            return -1;
        live_state = 1;
        temp_state = 0;
        if (operations_replacement_fault(options->operation,
                                         options->after_temp_to_live, out))
            return 1;
    }
    if (live_state != 1 || temp_state != 0 || recovery_state != 1 ||
        operations_verify_database(live, &schema_version) != 0)
        return -1;
    if (operations_replacement_fault(options->operation,
                                     options->before_recovery_cleanup, out))
        return 1;
    if (!options->keep_recovery &&
        ((options->remove_sidecars &&
          operations_remove_closed_database_sidecars(recovery) != 0) ||
         cbm_unlink(recovery) != 0 || operations_sync_directory(parent) != 0))
        return -1;
    if (out_schema_version) *out_schema_version = schema_version;
    return 0;
}
#endif

cbm_zova_operation_code_t cbm_zova_database_compact(
    const char *path, cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "compact", "invalid_argument");
    if (!path || !path[0] || !out) return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    size_t path_len = strlen(path);
    char temporary[4096], recovery[4096], parent[4096];
    if (path_len <= 5 || strcmp(path + path_len - 5, ".zova") != 0 || path_len - 5 > 4000 ||
        snprintf(temporary, sizeof(temporary), "%.*s.compact.tmp.zova",
                 (int)(path_len - 5), path) >= (int)sizeof(temporary) ||
        snprintf(recovery, sizeof(recovery), "%.*s.compact.recovery.zova",
                 (int)(path_len - 5), path) >= (int)sizeof(recovery) ||
        operations_parent_directory(path, parent, sizeof(parent)) != 0)
        return CBM_ZOVA_OPERATION_INVALID;

    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_try_acquire(path, &guard) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_BUSY, "compact", "writer_busy");
        return CBM_ZOVA_OPERATION_BUSY;
    }
    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    int live_state = operations_restore_path_state(path);
    int temp_state = operations_restore_path_state(temporary);
    int recovery_state = operations_restore_path_state(recovery);
    int valid_state = (live_state == 1 && temp_state == 0 && recovery_state == 0) ||
                      (live_state == 1 && temp_state == 1 && recovery_state == 0) ||
                      (live_state == 0 && temp_state == 1 && recovery_state == 1) ||
                      (live_state == 1 && temp_state == 0 && recovery_state == 1);
    if (live_state < 0 || temp_state < 0 || recovery_state < 0 || !valid_state) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "compact",
                          "invalid_recovery_state");
        cbm_zova_writer_guard_release(&guard);
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }

    cbm_zova_operation_report_t before = {0};
    if (live_state == 1 && temp_state == 0 && recovery_state == 0) {
        cbm_zova_operation_code_t status_code = cbm_zova_database_status(path, &before);
        if (status_code != CBM_ZOVA_OPERATION_OK) {
            operations_report(out, status_code, "compact", "status_failed");
            cbm_zova_writer_guard_release(&guard);
            return status_code;
        }
        if (operations_compact_below_threshold(&before)) {
            operations_report(out, CBM_ZOVA_OPERATION_NOOP, "compact", "below_threshold");
            operations_copy_status_metrics(out, &before);
            out->reclaimable_bytes = before.reclaimable_bytes;
            cbm_zova_writer_guard_release(&guard);
            return CBM_ZOVA_OPERATION_NOOP;
        }
        const char *free_override = getenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES");
        if (free_override && free_override[0]) {
            unsigned long long overridden = 0;
            char trailing = '\0';
            if (sscanf(free_override, "%llu%c", &overridden, &trailing) != 1) {
                operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "compact",
                                  "invalid_test_free_bytes");
                cbm_zova_writer_guard_release(&guard);
                return CBM_ZOVA_OPERATION_VERIFY_FAILED;
            }
            before.free_bytes = (uint64_t)overridden;
        }
        const uint64_t reserve = 1ULL * 1024ULL * 1024ULL * 1024ULL;
        const uint64_t guard_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
        if (before.database_bytes > UINT64_MAX - reserve) {
            operations_report(out, CBM_ZOVA_OPERATION_DISK_REFUSED, "compact",
                              "required_space_overflow");
            cbm_zova_writer_guard_release(&guard);
            return CBM_ZOVA_OPERATION_DISK_REFUSED;
        }
        uint64_t required = before.database_bytes + reserve;
        if (required < guard_bytes) required = guard_bytes;
        if (before.free_bytes < required) {
            operations_report(out, CBM_ZOVA_OPERATION_DISK_REFUSED, "compact",
                              "insufficient_disk");
            operations_copy_status_metrics(out, &before);
            out->reclaimable_bytes = before.reclaimable_bytes;
            cbm_zova_writer_guard_release(&guard);
            return CBM_ZOVA_OPERATION_DISK_REFUSED;
        }

        zova_database *db = NULL;
        zova_message error = {0};
        int opened = zova_database_open_with_options(&(zova_database_open_options_request){
                         .path = path, .flags = 0, .busy_timeout_ms = 0,
                         .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db;
        zova_message_free(&error);
        int compacted = opened && operations_checkpoint_truncate(db) == 0 &&
                        zova_database_compact(&(zova_database_compact_request){
                            .db = db, .destination_path = temporary, .flags = 0}) == ZOVA_OK;
        if (db) (void)zova_database_close(db);
        if (!compacted) {
            (void)operations_remove_closed_database_sidecars(temporary);
            (void)cbm_unlink(temporary);
            operations_report(out, CBM_ZOVA_OPERATION_BUSY, "compact",
                              "checkpoint_or_compact_busy");
            cbm_zova_writer_guard_release(&guard);
            return CBM_ZOVA_OPERATION_BUSY;
        }
        if (operations_remove_closed_database_sidecars(path) != 0 ||
            operations_remove_closed_database_sidecars(temporary) != 0) {
            operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "compact",
                              "sidecar_cleanup_failed");
            cbm_zova_writer_guard_release(&guard);
            return CBM_ZOVA_OPERATION_VERIFY_FAILED;
        }
        temp_state = 1;
        if (operations_replacement_fault("compact", "compact_after_temp_creation", out))
            goto interrupted;
    }

    int schema_version = 0;
    const operations_replacement_options_t replacement = {
        .operation = "compact",
        .after_temp_verification = "compact_after_temp_verification",
        .after_live_to_recovery = "compact_after_live_to_recovery",
        .after_temp_to_live = "compact_after_temp_to_live",
        .before_recovery_cleanup = "compact_before_recovery_cleanup",
        .remove_sidecars = true,
        .require_source_verified = true,
    };
    int replacement_result = operations_replace_verified_database(
        path, temporary, recovery, parent, &replacement, &schema_version, out);
    if (replacement_result > 0) goto interrupted;
    if (replacement_result < 0) goto replacement_failed;

    cbm_zova_operation_report_t after = {0};
    if (cbm_zova_database_status(path, &after) != CBM_ZOVA_OPERATION_OK)
        goto replacement_failed;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "compact", "ok");
    operations_copy_status_metrics(out, &after);
    out->reclaimable_bytes = after.reclaimable_bytes;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    cbm_zova_writer_guard_release(&guard);
    return CBM_ZOVA_OPERATION_OK;

interrupted:
    cbm_zova_writer_guard_release(&guard);
    return CBM_ZOVA_OPERATION_VERIFY_FAILED;

replacement_failed:
    operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "compact",
                      "replacement_failed");
    cbm_zova_writer_guard_release(&guard);
    return CBM_ZOVA_OPERATION_VERIFY_FAILED;
#endif
}

#if CBM_WITH_ZOVA
static int operations_workspace_health_row(zova_database *db, const char *workspace_id,
                                           int64_t *out_generation, int *out_rebuild) {
    zova_statement *statement = NULL;
    if (!db || !workspace_id || !out_generation || !out_rebuild ||
        zova_database_prepare(&(zova_database_prepare_request){
            .db = db,
            .sql = "SELECT r.active_generation,COALESCE(h.state,'healthy') "
                   "FROM cbm_workspace_registry r LEFT JOIN cbm_workspace_health_v1 h "
                   "ON h.workspace_key=r.workspace_key WHERE r.workspace_id=?1",
            .out_statement = &statement}) != ZOVA_OK || !statement ||
        zova_statement_bind_text(&(zova_statement_bind_text_request){
            .statement = statement, .index = 1, .data = (const uint8_t *)workspace_id,
            .len = strlen(workspace_id)}) != ZOVA_OK) {
        if (statement) (void)zova_statement_finalize(statement);
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    zova_text state = {0};
    int rc = operations_step(statement, &step) == 0 && step == ZOVA_STEP_ROW &&
                     zova_statement_column_int64(&(zova_statement_column_int64_request){
                         .statement = statement, .index = 0,
                         .out_value = out_generation}) == ZOVA_OK &&
                     zova_statement_column_text(&(zova_statement_column_text_request){
                         .statement = statement, .index = 1, .out_text = &state}) == ZOVA_OK &&
                     state.data
                 ? 0 : -1;
    if (rc == 0)
        *out_rebuild = state.len == strlen("rebuild_required") &&
                       memcmp(state.data, "rebuild_required", state.len) == 0;
    zova_text_free(&state);
    (void)zova_statement_finalize(statement);
    return rc;
}

static int operations_mark_workspace_rebuild_required(const char *path,
                                                       const char *workspace_id,
                                                       int64_t generation,
                                                       const char *reason) {
    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_acquire(path, &guard) != 0) return -1;
    zova_database *db = NULL;
    zova_message error = {0};
    int rc = zova_database_open(&(zova_database_open_request){
                 .path = path, .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db
                 ? 0 : -1;
    zova_message_free(&error);
    if (rc == 0 &&
        zova_database_begin_immediate(&(zova_database_simple_request){.db = db}) != ZOVA_OK)
        rc = -1;
    zova_statement *statement = NULL;
    if (rc == 0 &&
        (zova_database_prepare(&(zova_database_prepare_request){
             .db = db,
             .sql = "INSERT INTO cbm_workspace_health_v1"
                    "(workspace_key,state,reason,checked_generation,checked_at) "
                    "SELECT workspace_key,'rebuild_required',?2,?3,"
                    "strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                    "FROM cbm_workspace_registry WHERE workspace_id=?1 "
                    "ON CONFLICT(workspace_key) DO UPDATE SET "
                    "state=excluded.state,reason=excluded.reason,"
                    "checked_generation=excluded.checked_generation,"
                    "checked_at=excluded.checked_at",
             .out_statement = &statement}) != ZOVA_OK || !statement))
        rc = -1;
    if (rc == 0 &&
        zova_statement_bind_text(&(zova_statement_bind_text_request){
            .statement = statement, .index = 1, .data = (const uint8_t *)workspace_id,
            .len = strlen(workspace_id)}) != ZOVA_OK)
        rc = -1;
    if (rc == 0 &&
        zova_statement_bind_text(&(zova_statement_bind_text_request){
            .statement = statement, .index = 2, .data = (const uint8_t *)reason,
            .len = strlen(reason)}) != ZOVA_OK)
        rc = -1;
    if (rc == 0 &&
        zova_statement_bind_int64(&(zova_statement_bind_int64_request){
            .statement = statement, .index = 3, .value = generation}) != ZOVA_OK)
        rc = -1;
    zova_step_result step = ZOVA_STEP_ROW;
    if (rc == 0 && (operations_step(statement, &step) != 0 || step != ZOVA_STEP_DONE)) rc = -1;
    if (statement) (void)zova_statement_finalize(statement);
    if (db) {
        if (rc == 0) {
            if (zova_database_commit(&(zova_database_simple_request){.db = db}) != ZOVA_OK)
                rc = -1;
        } else {
            (void)zova_database_rollback(&(zova_database_simple_request){.db = db});
        }
        (void)zova_database_close(db);
    }
    cbm_zova_writer_guard_release(&guard);
    return rc;
}

static int operations_workspace_project(const char *path, const char *workspace_id,
                                        char *out, size_t out_size) {
    zova_database *db = NULL;
    zova_message error = {0};
    zova_statement *statement = NULL;
    int rc = zova_database_open_with_options(&(zova_database_open_options_request){
                 .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                 .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db
                 ? 0 : -1;
    zova_message_free(&error);
    if (rc == 0 &&
        (zova_database_prepare(&(zova_database_prepare_request){
             .db = db,
             .sql = "SELECT p.project FROM cbm_projects_v1 p "
                    "JOIN cbm_workspace_registry r USING(workspace_key) "
                    "WHERE r.workspace_id=?1",
             .out_statement = &statement}) != ZOVA_OK || !statement))
        rc = -1;
    if (rc == 0 &&
        zova_statement_bind_text(&(zova_statement_bind_text_request){
            .statement = statement, .index = 1, .data = (const uint8_t *)workspace_id,
            .len = strlen(workspace_id)}) != ZOVA_OK)
        rc = -1;
    zova_step_result step = ZOVA_STEP_DONE;
    zova_text project = {0};
    if (rc == 0 &&
        (operations_step(statement, &step) != 0 || step != ZOVA_STEP_ROW ||
         zova_statement_column_text(&(zova_statement_column_text_request){
             .statement = statement, .index = 0, .out_text = &project}) != ZOVA_OK ||
         !project.data || project.len == 0 || project.len >= out_size))
        rc = -1;
    if (rc == 0) {
        memcpy(out, project.data, project.len);
        out[project.len] = '\0';
    }
    zova_text_free(&project);
    if (statement) (void)zova_statement_finalize(statement);
    if (db) (void)zova_database_close(db);
    return rc;
}

static int operations_quarantine_database(const char *path) {
    size_t length = strlen(path);
    char quarantine[4096], parent[4096];
    if (length <= 5 || strcmp(path + length - 5, ".zova") != 0 || length - 5 > 4000 ||
        snprintf(quarantine, sizeof(quarantine), "%.*s.corrupt.zova",
                 (int)(length - 5), path) >= (int)sizeof(quarantine) ||
        operations_parent_directory(path, parent, sizeof(parent)) != 0 ||
        operations_restore_path_state(path) != 1 ||
        operations_restore_path_state(quarantine) != 0)
        return -1;
    char source_sidecars[2][4096], quarantine_sidecars[2][4096];
    const char *suffixes[] = {"-wal", "-shm"};
    int present[2] = {0, 0};
    for (size_t i = 0; i < 2; i++) {
        if (snprintf(source_sidecars[i], sizeof(source_sidecars[i]), "%s%s", path,
                     suffixes[i]) >= (int)sizeof(source_sidecars[i]) ||
            snprintf(quarantine_sidecars[i], sizeof(quarantine_sidecars[i]), "%s%s",
                     quarantine, suffixes[i]) >= (int)sizeof(quarantine_sidecars[i]))
            return -1;
        int source_state = operations_restore_path_state(source_sidecars[i]);
        int quarantine_state = operations_restore_path_state(quarantine_sidecars[i]);
        if (source_state < 0 || quarantine_state != 0) return -1;
        present[i] = source_state == 1;
    }
    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_acquire(path, &guard) != 0) return -1;
    int moved[2] = {0, 0};
    int rc = 0;
    for (size_t i = 0; i < 2; i++) {
        if (present[i] && rename(source_sidecars[i], quarantine_sidecars[i]) != 0) {
            rc = -1;
            break;
        }
        if (present[i]) moved[i] = 1;
    }
    if (rc == 0 && rename(path, quarantine) != 0) rc = -1;
    if (rc == 0 && operations_sync_directory(parent) != 0) rc = -1;
    if (rc != 0) {
        for (int i = 1; i >= 0; i--)
            if (moved[i]) (void)rename(quarantine_sidecars[i], source_sidecars[i]);
    }
    cbm_zova_writer_guard_release(&guard);
    return rc;
}
#endif

cbm_zova_operation_code_t cbm_zova_database_health(
    const char *path, const char *workspace_id, cbm_zova_health_class_t *out_class,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "health", "invalid_argument");
    if (!path || !path[0] || !out_class || !out ||
        (workspace_id && cbm_zova_workspace_id_validate(workspace_id) != 0))
        return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    *out_class = CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY;
    zova_database *db = NULL;
    zova_message error = {0};
    int opened = zova_database_open_with_options(&(zova_database_open_options_request){
                     .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                     .out_db = &db, .out_error_message = &error}) == ZOVA_OK && db;
    zova_message_free(&error);
    int64_t schema_version = 0;
    int shared_ok = opened && operations_quick_check(db) == 0 &&
                    operations_query_i64(
                        db, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
                        &schema_version) == 0 &&
                    schema_version == CBM_ZOVA_DATABASE_SCHEMA_VERSION &&
                    cbm_zova_user_database_schema_is_current(db) == 0;
    if (!shared_ok) {
        if (db) (void)zova_database_close(db);
        operations_report(out, CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED, "health",
                          "shared_database_invalid");
        out->schema_version = (int)schema_version;
        return CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED;
    }
    if (operations_foreign_keys_ok(db) != 0) {
        (void)zova_database_close(db);
        operations_report(out, CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED, "health",
                          "foreign_key_violation");
        out->schema_version = (int)schema_version;
        return CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED;
    }
    if (!workspace_id) {
        int64_t workspace_count = 0;
        if (operations_query_i64(db, "SELECT count(*) FROM cbm_workspace_registry",
                                 &workspace_count) != 0 || workspace_count < 0 ||
            (uint64_t)workspace_count > SIZE_MAX / CBM_ZOVA_WORKSPACE_ID_MAX) {
            (void)zova_database_close(db);
            operations_report(out, CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED, "health",
                              "workspace_inventory_invalid");
            return CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED;
        }
        char(*workspace_ids)[CBM_ZOVA_WORKSPACE_ID_MAX] =
            workspace_count > 0 ? calloc((size_t)workspace_count, sizeof(*workspace_ids)) : NULL;
        zova_statement *statement = NULL;
        int inventory_ok = workspace_count == 0 ||
            (workspace_ids &&
             zova_database_prepare(&(zova_database_prepare_request){
                 .db = db,
                 .sql = "SELECT workspace_id FROM cbm_workspace_registry ORDER BY workspace_id",
                 .out_statement = &statement}) == ZOVA_OK && statement);
        for (int64_t i = 0; inventory_ok && i < workspace_count; i++) {
            zova_step_result step = ZOVA_STEP_DONE;
            zova_text value = {0};
            inventory_ok = operations_step(statement, &step) == 0 && step == ZOVA_STEP_ROW &&
                           zova_statement_column_text(&(zova_statement_column_text_request){
                               .statement = statement, .index = 0, .out_text = &value}) ==
                               ZOVA_OK && value.data && value.len > 0 &&
                           value.len < CBM_ZOVA_WORKSPACE_ID_MAX;
            if (inventory_ok) {
                memcpy(workspace_ids[i], value.data, value.len);
                workspace_ids[i][value.len] = '\0';
            }
            zova_text_free(&value);
        }
        if (statement) (void)zova_statement_finalize(statement);
        (void)zova_database_close(db);
        if (!inventory_ok) {
            free(workspace_ids);
            operations_report(out, CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED, "health",
                              "workspace_inventory_invalid");
            return CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED;
        }
        for (int64_t i = 0; i < workspace_count; i++) {
            cbm_zova_health_class_t workspace_health = CBM_ZOVA_HEALTH_OK;
            cbm_zova_operation_report_t workspace_report = {0};
            cbm_zova_operation_code_t code = cbm_zova_database_health(
                path, workspace_ids[i], &workspace_health, &workspace_report);
            if (code != CBM_ZOVA_OPERATION_OK) {
                free(workspace_ids);
                *out = workspace_report;
                *out_class = workspace_health;
                return code;
            }
        }
        free(workspace_ids);
        *out_class = CBM_ZOVA_HEALTH_OK;
        operations_report(out, CBM_ZOVA_OPERATION_OK, "health", "ok");
        out->schema_version = (int)schema_version;
        return CBM_ZOVA_OPERATION_OK;
    }
    int64_t generation = 0;
    int already_rebuild = 0;
    if (operations_workspace_health_row(db, workspace_id, &generation, &already_rebuild) != 0) {
        (void)zova_database_close(db);
        operations_report(out, CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED, "health",
                          "workspace_generation_invalid");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        *out_class = CBM_ZOVA_HEALTH_WORKSPACE_REBUILD;
        return CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED;
    }
    (void)zova_database_close(db);
    if (already_rebuild) {
        operations_report(out, CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED, "health",
                          "workspace_rebuild_required");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        out->generation = generation;
        out->schema_version = (int)schema_version;
        *out_class = CBM_ZOVA_HEALTH_WORKSPACE_REBUILD;
        return CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED;
    }
    cbm_zova_workspace_snapshot_t snapshot = {0};
    if (cbm_zova_repository_export_snapshot(path, workspace_id, &snapshot) != 0) {
        if (operations_mark_workspace_rebuild_required(
                path, workspace_id, generation, "workspace_public_state_invalid") != 0) {
            operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "health",
                              "health_state_write_failed");
            return CBM_ZOVA_OPERATION_VERIFY_FAILED;
        }
        operations_report(out, CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED, "health",
                          "workspace_public_state_invalid");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        out->generation = generation;
        out->schema_version = (int)schema_version;
        *out_class = CBM_ZOVA_HEALTH_WORKSPACE_REBUILD;
        return CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED;
    }
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = snapshot.root_path,
        .project = snapshot.project,
        .indexed_at = snapshot.indexed_at,
        .model_fingerprint = snapshot.model_fingerprint,
        .vector_dimensions = snapshot.vector_dimensions,
        .nodes = snapshot.nodes,
        .node_count = snapshot.node_count,
        .edges = snapshot.edges,
        .edge_count = snapshot.edge_count,
        .node_vectors = snapshot.node_vectors,
        .node_vector_count = snapshot.node_vector_count,
        .token_vectors = snapshot.token_vectors,
        .token_vector_count = snapshot.token_vector_count,
        .file_hashes = snapshot.file_hashes,
        .file_hash_count = snapshot.file_hash_count,
        .project_summary = snapshot.project_summary,
    };
    cbm_zova_workspace_generation_result_t readback = {0};
    int digest_ok = cbm_zova_workspace_generation_digest_input(
                        workspace_id, &input, &readback) == 0;
    if (digest_ok) {
        readback.generation = snapshot.generation;
        snprintf(readback.workspace_id, sizeof(readback.workspace_id), "%s", workspace_id);
        digest_ok = operations_generation_matches(&readback, &snapshot.integrity);
    }
    cbm_zova_workspace_snapshot_free(&snapshot);
    if (!digest_ok) {
        if (operations_mark_workspace_rebuild_required(
                path, workspace_id, generation, "workspace_digest_mismatch") != 0) {
            operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "health",
                              "health_state_write_failed");
            return CBM_ZOVA_OPERATION_VERIFY_FAILED;
        }
        operations_report(out, CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED, "health",
                          "workspace_digest_mismatch");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        out->generation = generation;
        out->schema_version = (int)schema_version;
        *out_class = CBM_ZOVA_HEALTH_WORKSPACE_REBUILD;
        return CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED;
    }
    operations_report(out, CBM_ZOVA_OPERATION_OK, "health", "ok");
    snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
    out->generation = generation;
    out->schema_version = (int)schema_version;
    *out_class = CBM_ZOVA_HEALTH_OK;
    return CBM_ZOVA_OPERATION_OK;
#endif
}

cbm_zova_operation_code_t cbm_zova_workspace_recover(
    const char *path, const char *workspace_id, const char *repo_path,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "recover_workspace",
                      "invalid_argument");
    if (!path || !path[0] || cbm_zova_workspace_id_validate(workspace_id) != 0 ||
        !repo_path || !repo_path[0] || !out)
        return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    char expected_workspace[CBM_ZOVA_WORKSPACE_ID_MAX];
    char user_path[4096];
    if (cbm_zova_workspace_id_for_root(repo_path, expected_workspace,
                                       sizeof(expected_workspace)) != 0 ||
        strcmp(expected_workspace, workspace_id) != 0 ||
        cbm_zova_route_from_env() != CBM_ZOVA_ROUTE_FULL_AUTHORITY ||
        cbm_zova_user_database_path(user_path, sizeof(user_path)) != 0 ||
        strcmp(user_path, path) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_INVALID, "recover_workspace",
                          "identity_or_route_mismatch");
        return CBM_ZOVA_OPERATION_INVALID;
    }
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t health_report = {0};
    cbm_zova_operation_code_t health_code =
        cbm_zova_database_health(path, workspace_id, &health, &health_report);
    if (health_code == CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED &&
        health == CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY) {
        if (operations_quarantine_database(path) != 0) {
            operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "recover_workspace",
                              "quarantine_failed");
            return CBM_ZOVA_OPERATION_VERIFY_FAILED;
        }
        operations_report(out, CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED,
                          "recover_workspace", "quarantined_no_verified_backup");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        return CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED;
    }
    if (health_code != CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED ||
        health != CBM_ZOVA_HEALTH_WORKSPACE_REBUILD) {
        operations_report(out,
                          health_code == CBM_ZOVA_OPERATION_OK
                              ? CBM_ZOVA_OPERATION_NOOP : health_code,
                          "recover_workspace",
                          health_code == CBM_ZOVA_OPERATION_OK ? "workspace_healthy"
                                                               : health_report.reason);
        return out->code;
    }
    char project[256];
    cbm_pipeline_t *pipeline = NULL;
    if (operations_workspace_project(path, workspace_id, project, sizeof(project)) != 0 ||
        !(pipeline = cbm_pipeline_new(repo_path, NULL, CBM_MODE_FULL)) ||
        !cbm_pipeline_set_project_name(pipeline, project)) {
        if (pipeline) cbm_pipeline_free(pipeline);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "recover_workspace",
                          "pipeline_create_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    int run_rc = cbm_pipeline_run(pipeline);
    cbm_pipeline_free(pipeline);
    if (run_rc != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "recover_workspace",
                          "source_rebuild_failed");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        out->generation = health_report.generation;
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    health_code = cbm_zova_database_health(path, workspace_id, &health, &health_report);
    if (health_code != CBM_ZOVA_OPERATION_OK || health != CBM_ZOVA_HEALTH_OK) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "recover_workspace",
                          "post_rebuild_health_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    operations_report(out, CBM_ZOVA_OPERATION_OK, "recover_workspace", "ok");
    snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
    out->generation = health_report.generation;
    out->schema_version = health_report.schema_version;
    return CBM_ZOVA_OPERATION_OK;
#endif
}

cbm_zova_operation_code_t cbm_zova_database_backup(
    const char *source_path, const char *destination_path,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "backup", "invalid_argument");
    if (!source_path || !source_path[0] || !destination_path || !destination_path[0] || !out ||
        strcmp(source_path, destination_path) == 0) {
        return CBM_ZOVA_OPERATION_INVALID;
    }
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    struct stat source_stat;
    struct stat destination_stat;
    if (!operations_path_is_regular_nosymlink(source_path) ||
        cbm_lstat(destination_path, &destination_stat) == 0) {
        return CBM_ZOVA_OPERATION_INVALID;
    }

    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    zova_database *source = NULL;
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = source_path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &source, .out_error_message = &error}) != ZOVA_OK ||
        !source) {
        zova_message_free(&error);
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    zova_message_free(&error);
    int backup_ok = zova_database_backup(&(zova_database_backup_request){
                        .db = source, .destination_path = destination_path, .flags = 0}) == ZOVA_OK;
    (void)zova_database_close(source);
    int schema_version = 0;
    if (!backup_ok || operations_verify_database(destination_path, &schema_version) != 0 ||
        stat(destination_path, &destination_stat) != 0) {
        (void)cbm_unlink(destination_path);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "backup",
                          "verification_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "backup", "ok");
    out->schema_version = schema_version;
    out->database_bytes = (uint64_t)destination_stat.st_size;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    char wal_path[4096];
    if (snprintf(wal_path, sizeof(wal_path), "%s-wal", source_path) < (int)sizeof(wal_path) &&
        stat(wal_path, &source_stat) == 0) {
        out->wal_bytes = (uint64_t)source_stat.st_size;
    }
    return CBM_ZOVA_OPERATION_OK;
#endif
}

cbm_zova_operation_code_t cbm_zova_database_restore(
    const char *live_path, const char *backup_path, bool confirm_replace,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "restore", "invalid_argument");
    if (!live_path || !live_path[0] || !backup_path || !backup_path[0] || !out) {
        return CBM_ZOVA_OPERATION_INVALID;
    }
    if (!confirm_replace) {
        operations_report(out, CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED, "restore",
                          "confirmation_required");
        return CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED;
    }
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    size_t live_path_len = strlen(live_path);
    char temporary[4096], recovery[4096], parent[4096];
    if (live_path_len <= 5 || strcmp(live_path + live_path_len - 5, ".zova") != 0 ||
        live_path_len - 5 > 4000 ||
        snprintf(temporary, sizeof(temporary), "%.*s.restore.tmp.zova",
                 (int)(live_path_len - 5), live_path) >= (int)sizeof(temporary) ||
        snprintf(recovery, sizeof(recovery), "%.*s.restore.recovery.zova",
                 (int)(live_path_len - 5), live_path) >= (int)sizeof(recovery) ||
        operations_parent_directory(live_path, parent, sizeof(parent)) != 0 ||
        strcmp(temporary, live_path) == 0 || strcmp(temporary, backup_path) == 0) {
        return CBM_ZOVA_OPERATION_INVALID;
    }
    if (!operations_path_is_regular_nosymlink(backup_path)) {
        return CBM_ZOVA_OPERATION_INVALID;
    }
    int preflight_live_state = operations_restore_path_state(live_path);
    if (preflight_live_state < 0 ||
        (preflight_live_state == 1 && operations_same_file(live_path, backup_path))) {
        return CBM_ZOVA_OPERATION_INVALID;
    }
    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_acquire(live_path, &guard) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_BUSY, "restore", "writer_busy");
        return CBM_ZOVA_OPERATION_BUSY;
    }
    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    int schema_version = 0;
    int live_state = operations_restore_path_state(live_path);
    int temp_state = operations_restore_path_state(temporary);
    int recovery_state = operations_restore_path_state(recovery);
    int valid_state = (live_state == 1 && temp_state == 0 && recovery_state == 0) ||
                      (live_state == 1 && temp_state == 1 && recovery_state == 0) ||
                      (live_state == 0 && temp_state == 1 && recovery_state == 1) ||
                      (live_state == 1 && temp_state == 0 && recovery_state == 1);
    if (live_state < 0 || temp_state < 0 || recovery_state < 0 || !valid_state ||
        (temp_state == 1 && operations_same_file(temporary, backup_path)) ||
        (recovery_state == 1 && operations_same_file(recovery, backup_path)) ||
        operations_verify_database(backup_path, NULL) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "restore",
                          "invalid_recovery_state");
        cbm_zova_writer_guard_release(&guard);
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }

    if (temp_state == 0 && recovery_state == 0) {
        zova_message error = {0};
        int restored = zova_database_restore(&(zova_database_restore_request){
                           .source_path = backup_path, .destination_path = temporary,
                           .flags = 0, .out_error_message = &error}) == ZOVA_OK;
        zova_message_free(&error);
        if (!restored) goto replacement_failed;
        temp_state = 1;
        if (operations_replacement_fault("restore", "restore_after_temp_creation", out))
            goto interrupted;
    }
    const operations_replacement_options_t replacement = {
        .operation = "restore",
        .after_temp_verification = "restore_after_temp_verification",
        .after_live_to_recovery = "restore_after_live_to_recovery",
        .after_temp_to_live = "restore_after_temp_to_live",
        .before_recovery_cleanup = "restore_before_recovery_cleanup",
        .remove_sidecars = false,
        .require_source_verified = false,
    };
    int replacement_result = operations_replace_verified_database(
        live_path, temporary, recovery, parent, &replacement, &schema_version, out);
    if (replacement_result > 0) goto interrupted;
    if (replacement_result < 0) goto replacement_failed;

    struct stat live_stat;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "restore", "ok");
    out->schema_version = schema_version;
    if (stat(live_path, &live_stat) == 0) out->database_bytes = (uint64_t)live_stat.st_size;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    cbm_zova_writer_guard_release(&guard);
    return CBM_ZOVA_OPERATION_OK;

interrupted:
    cbm_zova_writer_guard_release(&guard);
    return CBM_ZOVA_OPERATION_VERIFY_FAILED;

replacement_failed:
    operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "restore",
                      "replacement_failed");
    cbm_zova_writer_guard_release(&guard);
    return CBM_ZOVA_OPERATION_VERIFY_FAILED;
#endif
}

cbm_zova_operation_code_t cbm_zova_database_export(
    const char *source_path, const char *archive_directory,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "export_database", "invalid_argument");
    if (!source_path || !source_path[0] || !archive_directory || !archive_directory[0] || !out)
        return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    if (!operations_path_is_regular_nosymlink(source_path)) return CBM_ZOVA_OPERATION_INVALID;
    char partial[4096], data[4096], manifest_path[4096], parent[4096];
    if (snprintf(partial, sizeof(partial), "%s.partial", archive_directory) >=
            (int)sizeof(partial) ||
        snprintf(data, sizeof(data), "%s/data.zova", partial) >= (int)sizeof(data) ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", partial) >=
            (int)sizeof(manifest_path) ||
        operations_parent_directory(archive_directory, parent, sizeof(parent)) != 0)
        return CBM_ZOVA_OPERATION_INVALID;
    struct stat value;
    if (cbm_lstat(archive_directory, &value) == 0) return CBM_ZOVA_OPERATION_INVALID;
    if (cbm_lstat(partial, &value) == 0) {
        operations_report(out, CBM_ZOVA_OPERATION_BUSY, "export_database", "partial_exists");
        return CBM_ZOVA_OPERATION_BUSY;
    }
    if (cbm_mkdir_mode(partial, 0700) != 0) return CBM_ZOVA_OPERATION_INVALID;
    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_operation_report_t backup = {0};
    operations_archive_manifest_t manifest = {
        .archive_version = CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION,
        .schema_version = CBM_ZOVA_DATABASE_SCHEMA_VERSION,
    };
    int rc = cbm_zova_database_backup(source_path, data, &backup) == CBM_ZOVA_OPERATION_OK ? 0
                                                                                          : -1;
    if (rc == 0)
        rc = operations_file_sha256(data, manifest.database_sha256,
                                    &manifest.database_bytes);
    if (rc == 0) rc = operations_write_archive_manifest(data, manifest_path, &manifest);
    if (rc == 0) rc = operations_remove_closed_database_sidecars(data);
    if (rc == 0) rc = operations_verify_archive(partial, NULL, NULL);
    if (rc == 0) rc = operations_remove_closed_database_sidecars(data);
    if (rc == 0) rc = operations_archive_has_exact_members(partial);
    if (rc == 0) rc = operations_sync_directory(partial);
    if (rc == 0 && rename(partial, archive_directory) != 0) rc = -1;
    if (rc == 0) rc = operations_sync_directory(parent);
    if (rc != 0) {
        operations_remove_partial_archive(partial);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "export_database",
                          "archive_verification_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "export_database", "ok");
    out->archive_version = manifest.archive_version;
    out->schema_version = manifest.schema_version;
    out->database_bytes = manifest.database_bytes;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    return CBM_ZOVA_OPERATION_OK;
#endif
}

cbm_zova_operation_code_t cbm_zova_database_import(
    const char *live_path, const char *archive_directory, bool confirm_replace,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "import_database", "invalid_argument");
    if (!live_path || !live_path[0] || !archive_directory || !archive_directory[0] || !out)
        return CBM_ZOVA_OPERATION_INVALID;
    if (!confirm_replace) {
        operations_report(out, CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED, "import_database",
                          "confirmation_required");
        return CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED;
    }
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    struct stat value;
    if (cbm_lstat(archive_directory, &value) != 0 || !S_ISDIR(value.st_mode)
#ifndef _WIN32
        || S_ISLNK(value.st_mode)
#endif
    )
        return CBM_ZOVA_OPERATION_INVALID;
    operations_archive_manifest_t manifest = {0};
    int verified = operations_verify_archive(archive_directory, live_path, &manifest);
    if (verified == -2) {
        operations_report(out, CBM_ZOVA_OPERATION_INCOMPATIBLE, "import_database",
                          "archive_version_incompatible");
        return CBM_ZOVA_OPERATION_INCOMPATIBLE;
    }
    if (verified == -3) return CBM_ZOVA_OPERATION_INVALID;
    if (verified != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_database",
                          "archive_verification_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    char data[4096];
    if (snprintf(data, sizeof(data), "%s/data.zova", archive_directory) >= (int)sizeof(data))
        return CBM_ZOVA_OPERATION_INVALID;
    cbm_zova_operation_code_t code = cbm_zova_database_restore(live_path, data, true, out);
    if (code == CBM_ZOVA_OPERATION_OK) {
        snprintf(out->operation, sizeof(out->operation), "%s", "import_database");
        out->archive_version = manifest.archive_version;
    }
    return code;
#endif
}

cbm_zova_operation_code_t cbm_zova_workspace_export(
    const char *source_path, const char *workspace_id,
    const char *archive_directory, cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "export_workspace", "invalid_argument");
    if (!source_path || !source_path[0] || !archive_directory || !archive_directory[0] || !out ||
        cbm_zova_workspace_id_validate(workspace_id) != 0)
        return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    if (!operations_path_is_regular_nosymlink(source_path)) return CBM_ZOVA_OPERATION_INVALID;
    cbm_zova_workspace_snapshot_t snapshot = {0};
    if (cbm_zova_repository_export_snapshot(source_path, workspace_id, &snapshot) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_INVALID, "export_workspace",
                          "workspace_not_ready");
        return CBM_ZOVA_OPERATION_INVALID;
    }
    char partial[4096], data[4096], manifest_path[4096], parent[4096];
    struct stat value;
    cbm_zova_operation_code_t code = CBM_ZOVA_OPERATION_VERIFY_FAILED;
    if (snprintf(partial, sizeof(partial), "%s.partial", archive_directory) >=
            (int)sizeof(partial) ||
        snprintf(data, sizeof(data), "%s/data.zova", partial) >= (int)sizeof(data) ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", partial) >=
            (int)sizeof(manifest_path) ||
        operations_parent_directory(archive_directory, parent, sizeof(parent)) != 0 ||
        cbm_lstat(archive_directory, &value) == 0) {
        code = CBM_ZOVA_OPERATION_INVALID;
        goto done;
    }
    if (cbm_lstat(partial, &value) == 0) {
        operations_report(out, CBM_ZOVA_OPERATION_BUSY, "export_workspace", "partial_exists");
        code = CBM_ZOVA_OPERATION_BUSY;
        goto done;
    }
    if (cbm_mkdir_mode(partial, 0700) != 0) {
        code = CBM_ZOVA_OPERATION_INVALID;
        goto done;
    }
    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = snapshot.root_path, .project = snapshot.project,
        .indexed_at = snapshot.indexed_at, .model_fingerprint = snapshot.model_fingerprint,
        .vector_dimensions = snapshot.vector_dimensions, .nodes = snapshot.nodes,
        .node_count = snapshot.node_count, .edges = snapshot.edges,
        .edge_count = snapshot.edge_count, .node_vectors = snapshot.node_vectors,
        .node_vector_count = snapshot.node_vector_count,
        .token_vectors = snapshot.token_vectors,
        .token_vector_count = snapshot.token_vector_count, .file_hashes = snapshot.file_hashes,
        .file_hash_count = snapshot.file_hash_count, .project_summary = snapshot.project_summary,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    int rc = 0;
    const char *failure_phase = "publish_failed";
    rc = cbm_zova_user_database_publish_workspace_at_generation(
        data, &input, snapshot.generation, &published);
    if (rc == 0) failure_phase = "published_inventory_mismatch";
    if (rc == 0 && !operations_generation_matches(&published, &snapshot.integrity)) rc = -1;
    if (rc == 0) { failure_phase = "sidecar_cleanup_failed";
        rc = operations_remove_closed_database_sidecars(data); }
    if (rc == 0) { failure_phase = "writer_lock_cleanup_failed";
        rc = operations_remove_closed_writer_lock(data); }
    cbm_zova_workspace_snapshot_t verified = {0};
    if (rc == 0) { failure_phase = "published_snapshot_verification_failed";
        rc = cbm_zova_repository_export_snapshot(data, workspace_id, &verified); }
    cbm_zova_workspace_snapshot_free(&verified);
    operations_archive_manifest_t manifest = {
        .archive_version = CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION,
        .schema_version = CBM_ZOVA_DATABASE_SCHEMA_VERSION,
    };
    if (rc == 0) { failure_phase = "database_digest_failed";
        rc = operations_file_sha256(data, manifest.database_sha256, &manifest.database_bytes);
    }
    if (rc == 0) { failure_phase = "manifest_write_failed";
        rc = operations_write_workspace_archive_manifest(manifest_path, &manifest, &snapshot); }
    if (rc == 0) { failure_phase = "manifest_sidecar_cleanup_failed";
        rc = operations_remove_closed_database_sidecars(data); }
    if (rc == 0) { failure_phase = "archive_verification_failed";
        rc = operations_verify_workspace_archive(partial, NULL, NULL); }
    if (rc == 0) { failure_phase = "verification_sidecar_cleanup_failed";
        rc = operations_remove_closed_database_sidecars(data); }
    if (rc == 0) { failure_phase = "archive_members_failed";
        rc = operations_archive_has_exact_members(partial); }
    if (rc == 0) { failure_phase = "partial_sync_failed";
        rc = operations_sync_directory(partial); }
    if (rc == 0) { failure_phase = "archive_rename_failed";
        if (rename(partial, archive_directory) != 0) rc = -1; }
    if (rc == 0) { failure_phase = "parent_sync_failed";
        rc = operations_sync_directory(parent); }
    if (rc != 0) {
        operations_remove_partial_archive(partial);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "export_workspace",
                          failure_phase);
        code = CBM_ZOVA_OPERATION_VERIFY_FAILED;
        goto done;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "export_workspace", "ok");
    snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
    out->archive_version = manifest.archive_version;
    out->schema_version = manifest.schema_version;
    out->generation = snapshot.generation;
    out->database_bytes = manifest.database_bytes;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    code = CBM_ZOVA_OPERATION_OK;
done:
    cbm_zova_workspace_snapshot_free(&snapshot);
    return code;
#endif
}

cbm_zova_operation_code_t cbm_zova_workspace_import(
    const char *target_path, const char *archive_directory, bool replace,
    cbm_zova_operation_report_t *out) {
    operations_report(out, CBM_ZOVA_OPERATION_INVALID, "import_workspace", "invalid_argument");
    if (!target_path || !target_path[0] || !archive_directory || !archive_directory[0] || !out)
        return CBM_ZOVA_OPERATION_INVALID;
#if !CBM_WITH_ZOVA
    return CBM_ZOVA_OPERATION_INCOMPATIBLE;
#else
    int target_state = operations_restore_path_state(target_path);
    if (target_state < 0) return CBM_ZOVA_OPERATION_INVALID;
    operations_archive_manifest_t manifest = {0};
    int archive_status = operations_verify_workspace_archive(archive_directory, target_path,
                                                              &manifest);
    if (archive_status == -2) {
        operations_report(out, CBM_ZOVA_OPERATION_INCOMPATIBLE, "import_workspace",
                          "archive_version_incompatible");
        return CBM_ZOVA_OPERATION_INCOMPATIBLE;
    }
    if (archive_status == -3) return CBM_ZOVA_OPERATION_INVALID;
    if (archive_status != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "archive_verification_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    char data[4096];
    if (snprintf(data, sizeof(data), "%s/data.zova", archive_directory) >= (int)sizeof(data))
        return CBM_ZOVA_OPERATION_INVALID;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    if (operations_single_ready_workspace(data, workspace_id) != 0) {
        (void)operations_cleanup_verified_archive_reads(archive_directory);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "workspace_inventory_invalid");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    cbm_zova_workspace_snapshot_t snapshot = {0};
    if (cbm_zova_repository_export_snapshot(data, workspace_id, &snapshot) != 0) {
        (void)operations_cleanup_verified_archive_reads(archive_directory);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "workspace_snapshot_invalid");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    if (operations_cleanup_verified_archive_reads(archive_directory) != 0) {
        cbm_zova_workspace_snapshot_free(&snapshot);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "archive_read_cleanup_failed");
        return CBM_ZOVA_OPERATION_VERIFY_FAILED;
    }
    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_operation_code_t code = CBM_ZOVA_OPERATION_VERIFY_FAILED;
    int schema_version = 0;
    if ((target_state == 0 && cbm_zova_user_database_init(target_path) != 0) ||
        operations_verify_database(target_path, &schema_version) != 0 ||
        schema_version != CBM_ZOVA_DATABASE_SCHEMA_VERSION) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "target_verification_failed");
        goto done;
    }
    cbm_zova_workspace_snapshot_t collision = {0};
    int existing_present =
        cbm_zova_repository_export_snapshot(target_path, workspace_id, &collision) == 0;
    if (existing_present &&
        (strcmp(collision.workspace_id, snapshot.workspace_id) != 0 ||
         strcmp(collision.root_path, snapshot.root_path) != 0)) {
        cbm_zova_workspace_snapshot_free(&collision);
        operations_report(out, CBM_ZOVA_OPERATION_INVALID, "import_workspace",
                          "identity_mismatch");
        code = CBM_ZOVA_OPERATION_INVALID;
        goto done;
    }
    if (existing_present && !replace) {
        cbm_zova_workspace_snapshot_free(&collision);
        operations_report(out, CBM_ZOVA_OPERATION_INVALID, "import_workspace",
                          "workspace_exists");
        code = CBM_ZOVA_OPERATION_INVALID;
        goto done;
    }
    if (existing_present && snapshot.generation < collision.generation) {
        cbm_zova_workspace_snapshot_free(&collision);
        operations_report(out, CBM_ZOVA_OPERATION_INCOMPATIBLE, "import_workspace",
                          "archive_generation_older");
        code = CBM_ZOVA_OPERATION_INCOMPATIBLE;
        goto done;
    }
    if (existing_present && snapshot.generation == collision.generation &&
        !operations_generation_matches(&collision.integrity, &snapshot.integrity)) {
        cbm_zova_workspace_snapshot_free(&collision);
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "same_generation_digest_mismatch");
        code = CBM_ZOVA_OPERATION_VERIFY_FAILED;
        goto done;
    }
    if (existing_present && snapshot.generation == collision.generation) {
        cbm_zova_workspace_snapshot_free(&collision);
        cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
        operations_report(out, CBM_ZOVA_OPERATION_NOOP, "import_workspace", "already_current");
        snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
        out->schema_version = schema_version;
        out->archive_version = manifest.archive_version;
        out->generation = snapshot.generation;
        struct stat target_stat;
        if (stat(target_path, &target_stat) == 0)
            out->database_bytes = (uint64_t)target_stat.st_size;
        out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                          ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
        code = CBM_ZOVA_OPERATION_NOOP;
        goto done;
    }
    cbm_zova_workspace_snapshot_free(&collision);
    int64_t workspaces_before = 0, workspaces_after = 0;
    if (operations_workspace_count(target_path, &workspaces_before) != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "target_inventory_failed");
        goto done;
    }
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = snapshot.root_path, .project = snapshot.project,
        .indexed_at = snapshot.indexed_at, .model_fingerprint = snapshot.model_fingerprint,
        .vector_dimensions = snapshot.vector_dimensions, .nodes = snapshot.nodes,
        .node_count = snapshot.node_count, .edges = snapshot.edges,
        .edge_count = snapshot.edge_count, .node_vectors = snapshot.node_vectors,
        .node_vector_count = snapshot.node_vector_count,
        .token_vectors = snapshot.token_vectors,
        .token_vector_count = snapshot.token_vector_count, .file_hashes = snapshot.file_hashes,
        .file_hash_count = snapshot.file_hash_count, .project_summary = snapshot.project_summary,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    int rc = cbm_zova_user_database_publish_workspace_at_generation(
        target_path, &input, snapshot.generation, &published);
    if (rc == 0 && !operations_generation_matches(&published, &snapshot.integrity)) rc = -1;
    cbm_zova_workspace_snapshot_t imported = {0};
    if (rc == 0) rc = cbm_zova_repository_export_snapshot(target_path, workspace_id, &imported);
    if (rc == 0 && !operations_generation_matches(&imported.integrity, &snapshot.integrity))
        rc = -1;
    if (rc == 0) rc = operations_workspace_count(target_path, &workspaces_after);
    if (rc == 0 && workspaces_after != workspaces_before + (existing_present ? 0 : 1)) rc = -1;
    cbm_zova_workspace_snapshot_free(&imported);
    if (rc != 0) {
        operations_report(out, CBM_ZOVA_OPERATION_VERIFY_FAILED, "import_workspace",
                          "published_verification_failed");
        goto done;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    operations_report(out, CBM_ZOVA_OPERATION_OK, "import_workspace", "ok");
    snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", workspace_id);
    out->schema_version = schema_version;
    out->archive_version = manifest.archive_version;
    out->generation = snapshot.generation;
    struct stat target_stat;
    if (stat(target_path, &target_stat) == 0)
        out->database_bytes = (uint64_t)target_stat.st_size;
    out->elapsed_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    code = CBM_ZOVA_OPERATION_OK;
done:
    cbm_zova_workspace_snapshot_free(&snapshot);
    return code;
#endif
}

cbm_zova_operation_code_t cbm_zova_database_repack(
    const cbm_zova_repack_request_t *request, cbm_zova_repack_report_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!request || !request->live_path || !request->live_path[0] || !out) {
        if (out) out->code = CBM_ZOVA_OPERATION_INVALID;
        return CBM_ZOVA_OPERATION_INVALID;
    }
#if !CBM_WITH_ZOVA
    out->code = CBM_ZOVA_OPERATION_INCOMPATIBLE;
    return out->code;
#else
    const char *live = request->live_path;
    size_t length = strlen(live);
    char temporary[4096], recovery[4096], parent[4096];
    if (length <= 5 || strcmp(live + length - 5, ".zova") != 0 || length - 5 > 4000 ||
        snprintf(temporary,sizeof(temporary),"%.*s.repack.tmp.zova",(int)(length-5),live)>=(int)sizeof(temporary)||
        snprintf(recovery,sizeof(recovery),"%.*s.repack.recovery.zova",(int)(length-5),live)>=(int)sizeof(recovery)||
        operations_parent_directory(live,parent,sizeof(parent))!=0) {
        out->code=CBM_ZOVA_OPERATION_INVALID;return out->code;
    }
    snprintf(out->recovery_path,sizeof(out->recovery_path),"%s",recovery);
    cbm_zova_writer_guard_t guard={0};
    if(cbm_zova_writer_guard_try_acquire(live,&guard)!=0){out->code=CBM_ZOVA_OPERATION_BUSY;return out->code;}
    int live_state=operations_restore_path_state(live);
    int temp_state=operations_restore_path_state(temporary);
    int recovery_state=operations_restore_path_state(recovery);
    if(live_state<0||temp_state<0||recovery_state<0){out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;}

    if(live_state==1&&temp_state==0&&recovery_state==0){
        if(operations_verify_database(live,NULL)==0){
            out->code=CBM_ZOVA_OPERATION_NOOP;
            out->source_cbm_schema=CBM_ZOVA_DATABASE_SCHEMA_VERSION;
            out->target_cbm_schema=CBM_ZOVA_DATABASE_SCHEMA_VERSION;
            goto done;
        }
        if(operations_raw_sqlite_checkpoint_truncate(live)!=0){
            out->code=CBM_ZOVA_OPERATION_BUSY;goto done;
        }
        cbm_log_info("zova.repack","phase","source_checkpointed");
        cbm_zova_v5_snapshot_t *snapshot=NULL;
        if(cbm_zova_v5_snapshot_open(live,&snapshot)!=0||!snapshot){
            out->code=CBM_ZOVA_OPERATION_INCOMPATIBLE;goto done;
        }
        int count=cbm_zova_v5_snapshot_workspace_count(snapshot);
        cbm_log_info("zova.repack","phase","source_snapshot","workspaces",count==2?"2":"other");
        out->source_cbm_schema=5;
        out->target_cbm_schema=CBM_ZOVA_DATABASE_SCHEMA_VERSION;
        out->workspace_count=count;
        uint64_t ignored_bytes=0;
        if(count<1||operations_file_sha256(live,out->source_sha256,&ignored_bytes)!=0){
            cbm_zova_v5_snapshot_close(snapshot);out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;
        }
        (void)operations_remove_closed_database_sidecars(temporary);
        (void)cbm_unlink(temporary);
        /* The gate has one process-wide mutex, so publishing the isolated
         * sibling temp while holding the live guard would self-deadlock.
         * Revalidate the source digest after reacquiring before replacement. */
        cbm_zova_writer_guard_release(&guard);
        int rc=0;
        for(int i=0;rc==0&&i<count;i++){
            int64_t generation=0;
            const cbm_zova_workspace_generation_input_t *input=
                cbm_zova_v5_snapshot_input_at(snapshot,i,&generation);
            cbm_zova_workspace_generation_result_t expected={0},published={0};
            char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
            if(!input||generation<=0||
               cbm_zova_workspace_id_for_root(input->root_path,workspace_id,sizeof(workspace_id))!=0||
               cbm_zova_workspace_generation_digest_input(workspace_id,input,&expected)!=0)rc=-1;
            if(rc==0){
                expected.generation=generation;
                snprintf(expected.workspace_id,sizeof(expected.workspace_id),"%s",workspace_id);
                if(cbm_zova_user_database_publish_workspace_at_generation(
                       temporary,input,generation,&published)!=0||
                   !operations_generation_matches(&published,&expected))rc=-1;
            }
            if(rc==0)cbm_log_info("zova.repack","phase","workspace_published");
        }
        cbm_zova_v5_snapshot_close(snapshot);
        if(rc==0&&cbm_zova_writer_guard_try_acquire(live,&guard)!=0){
            out->code=CBM_ZOVA_OPERATION_BUSY;goto done;
        }
        if(rc==0){
            char current_source[CBM_ZOVA_DIGEST_HEX_SIZE];
            if(operations_file_sha256(live,current_source,&ignored_bytes)!=0||
               strcmp(current_source,out->source_sha256)!=0)rc=-1;
        }
        int schema=0;
        zova_database *target_db=NULL;
        zova_message error={0};
        int64_t target_workspaces=-1,forbidden=-1;
        if(rc==0&&operations_verify_database(temporary,&schema)!=0)rc=-1;
        if(rc==0)cbm_log_info("zova.repack","phase","target_verified");
        if(rc==0&&(zova_database_open_with_options(&(zova_database_open_options_request){
            .path=temporary,.flags=0,.busy_timeout_ms=5000,.out_db=&target_db,
            .out_error_message=&error})!=ZOVA_OK||!target_db))rc=-1;
        zova_message_free(&error);
        if(rc==0)rc=operations_checkpoint_truncate(target_db);
        if(rc==0)rc=operations_query_i64(target_db,"SELECT count(*) FROM cbm_workspace_registry",&target_workspaces);
        if(rc==0)rc=operations_query_i64(target_db,
            "SELECT count(*) FROM sqlite_master WHERE name IN ('cbm_zova_trace_nodes_v1',"
            "'cbm_zova_edge_metadata_v1','cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
            "'cbm_token_vectors_compat_v1') OR name GLOB 'cbm_fts_w1_*'",&forbidden);
        if(target_db)(void)zova_database_close(target_db);
        if(rc==0&&(schema!=CBM_ZOVA_DATABASE_SCHEMA_VERSION||target_workspaces!=count||forbidden!=0))rc=-1;
        if(rc==0&&operations_file_sha256(temporary,out->target_sha256,&ignored_bytes)!=0)rc=-1;
#ifndef _WIN32
        if(rc==0){int fd=open(temporary,O_RDONLY);if(fd<0||fsync(fd)!=0)rc=-1;if(fd>=0)close(fd);}
        if(rc==0){int fd=open(live,O_RDONLY);if(fd<0||fsync(fd)!=0)rc=-1;if(fd>=0)close(fd);}
#endif
        if(rc==0&&operations_sync_directory(parent)!=0)rc=-1;
        if(rc!=0){out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;}
        temp_state=1;
        if(operations_replacement_fault("repack","repack_after_temp_creation",NULL)){
            out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;
        }
    }

    if(live_state==1&&temp_state==0&&recovery_state==1&&operations_verify_database(live,NULL)==0){
        out->code=CBM_ZOVA_OPERATION_NOOP;
        out->source_cbm_schema=5;
        out->target_cbm_schema=CBM_ZOVA_DATABASE_SCHEMA_VERSION;
        uint64_t ignored=0;
        (void)operations_file_sha256(recovery,out->source_sha256,&ignored);
        (void)operations_file_sha256(live,out->target_sha256,&ignored);
        if(!request->keep_recovery&&
           (operations_remove_closed_database_sidecars(recovery)!=0||
            cbm_unlink(recovery)!=0||operations_sync_directory(parent)!=0)){
            out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;
        }
        goto done;
    }
    const operations_replacement_options_t replacement={
        .operation="repack",.after_temp_verification="repack_after_temp_verification",
        .after_live_to_recovery="repack_after_live_to_recovery",
        .after_temp_to_live="repack_after_temp_to_live",
        .before_recovery_cleanup="repack_before_recovery_cleanup",
        .remove_sidecars=true,.require_source_verified=false,.keep_recovery=request->keep_recovery,
    };
    if(out->source_sha256[0]=='\0'){
        uint64_t ignored=0;
        const char *source_path=recovery_state==1?recovery:live;
        if(operations_file_sha256(source_path,out->source_sha256,&ignored)!=0){
            out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;
        }
    }
    int target_schema=0;
    int replaced=operations_replace_verified_database(live,temporary,recovery,parent,
                                                       &replacement,&target_schema,NULL);
    cbm_log_info("zova.repack","phase","replacement_returned");
    if(replaced!=0){out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;}
    out->source_cbm_schema=5;
    out->target_cbm_schema=target_schema;
    int64_t count=0;
    if(operations_workspace_count(live,&count)!=0||count>INT_MAX){
        out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;
    }
    out->workspace_count=(int)count;
    uint64_t ignored=0;
    if(request->keep_recovery)
        (void)operations_file_sha256(recovery,out->source_sha256,&ignored);
    if(operations_file_sha256(live,out->target_sha256,&ignored)!=0){
        out->code=CBM_ZOVA_OPERATION_VERIFY_FAILED;goto done;
    }
    out->code=CBM_ZOVA_OPERATION_OK;
done:
    cbm_zova_writer_guard_release(&guard);
    return out->code;
#endif
}
