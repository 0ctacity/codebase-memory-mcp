#include "zova/cbm_zova_migration.h"
#include "zova/cbm_zova_legacy_snapshot.h"
#include "zova/cbm_zova_writer_gate.h"
#include "foundation/compat_fs.h"

#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#include <sqlite3.h>
#include <yyjson/yyjson.h>

static int migration_file_digest(const char *path, int64_t *out_size,
                                 char out_digest[CBM_ZOVA_DIGEST_HEX_SIZE]) {
    struct stat st;
    FILE *file = NULL;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !(file = fopen(path, "rb")))
        return -1;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    uint8_t buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        cbm_sha256_update(&hash, buffer, count);
    int ok = ferror(file) == 0 && fclose(file) == 0;
    if (!ok) return -1;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char hex[] = "0123456789abcdef";
    cbm_sha256_final(&hash, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        out_digest[i * 2] = hex[digest[i] >> 4];
        out_digest[i * 2 + 1] = hex[digest[i] & 15];
    }
    out_digest[64] = '\0';
    *out_size = (int64_t)st.st_size;
    return 0;
}

static int migration_cleanup_member_safe(const char *path, int required) {
    struct stat st;
#ifdef _WIN32
    int rc = stat(path, &st);
#else
    int rc = lstat(path, &st);
#endif
    if (rc != 0) return required ? -1 : 0;
    return S_ISREG(st.st_mode) && st.st_nlink == 1 ? 0 : -1;
}

static int migration_cleanup_parent_components_safe(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    if (!path || path[0] != '/') return -1;
    char component[2048];
    int n = snprintf(component, sizeof(component), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(component)) return -1;
    for (char *cursor = component + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        struct stat st;
        int rc = lstat(component, &st);
        *cursor = '/';
        if (rc != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return -1;
    }
    return 0;
#endif
}

static int migration_cleanup_source_bundle_safe(const cbm_zova_migration_request_t *request) {
    struct stat db_stat, zova_stat;
    if (migration_cleanup_parent_components_safe(request->source_db_path) != 0 ||
        migration_cleanup_parent_components_safe(request->source_zova_path) != 0 ||
        migration_cleanup_member_safe(request->source_db_path, 1) != 0 ||
        migration_cleanup_member_safe(request->source_zova_path, 1) != 0 ||
        stat(request->source_db_path, &db_stat) != 0 ||
        stat(request->source_zova_path, &zova_stat) != 0 || db_stat.st_dev != zova_stat.st_dev)
        return -1;
    static const char *const suffixes[] = {"-wal", "-shm", "-journal"};
    const char *bases[] = {request->source_db_path, request->source_zova_path};
    for (int base = 0; base < 2; base++) {
        for (int suffix = 0; suffix < 3; suffix++) {
            char path[2048];
            int n = snprintf(path, sizeof(path), "%s%s", bases[base], suffixes[suffix]);
            if (n <= 0 || (size_t)n >= sizeof(path) ||
                migration_cleanup_member_safe(path, 0) != 0)
                return -1;
            struct stat member;
            if (stat(path, &member) == 0 && member.st_dev != db_stat.st_dev) return -1;
        }
    }
    return 0;
}

static int migration_cleanup_explicit_cache_contains(const cbm_zova_migration_request_t *request) {
    const char *configured = getenv("CBM_CACHE_DIR");
    if (!configured || !configured[0]) return 0;
#ifdef _WIN32
    (void)request;
    return 0;
#else
    char cache[2048], source_db[2048], source_zova[2048];
    if (!realpath(configured, cache) || !realpath(request->source_db_path, source_db) ||
        !realpath(request->source_zova_path, source_zova))
        return -1;
    size_t cache_len = strlen(cache);
    return strncmp(source_db, cache, cache_len) == 0 && source_db[cache_len] == '/' &&
                   strncmp(source_zova, cache, cache_len) == 0 && source_zova[cache_len] == '/'
               ? 0
               : -1;
#endif
}

static int migration_cleanup_quarantine_allowlisted(const char *directory,
                                                     const char *db_name,
                                                     const char *zova_name) {
#ifdef _WIN32
    (void)directory; (void)db_name; (void)zova_name;
    return 0;
#else
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    static const char *const suffixes[] = {"", "-wal", "-shm", "-journal"};
    int ok = 1;
    struct dirent *entry = NULL;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        int allowed = strcmp(entry->d_name, "manifest.json") == 0;
        for (int base = 0; !allowed && base < 2; base++)
            for (int suffix = 0; !allowed && suffix < 4; suffix++) {
                char expected[1024];
                snprintf(expected, sizeof(expected), "%s%s", base == 0 ? db_name : zova_name,
                         suffixes[suffix]);
                allowed = strcmp(entry->d_name, expected) == 0;
            }
        char path[2048];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (!allowed || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) ok = 0;
    }
    closedir(dir);
    return ok ? 0 : -1;
#endif
}

static int migration_cleanup_audit_header_valid(const char *text, const char *workspace_id,
                                                int64_t generation,
                                                const cbm_zova_migration_request_t *request) {
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *workspace = root ? yyjson_obj_get(root, "workspace_id") : NULL;
    yyjson_val *generation_value = root ? yyjson_obj_get(root, "generation") : NULL;
    yyjson_val *source_db = root ? yyjson_obj_get(root, "source_db") : NULL;
    yyjson_val *source_zova = root ? yyjson_obj_get(root, "source_zova") : NULL;
    int valid = yyjson_is_obj(root) && yyjson_is_str(workspace) &&
                strcmp(yyjson_get_str(workspace), workspace_id) == 0 &&
                yyjson_is_int(generation_value) && yyjson_get_sint(generation_value) == generation &&
                yyjson_is_str(source_db) &&
                strcmp(yyjson_get_str(source_db), request->source_db_path) == 0 &&
                yyjson_is_str(source_zova) &&
                strcmp(yyjson_get_str(source_zova), request->source_zova_path) == 0;
    yyjson_doc_free(doc);
    return valid ? 0 : -1;
}

typedef struct {
    int present;
    int purge_state; /* 0=live, 1=purge_authorized, 2=purged */
    int64_t size;
    char sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
} migration_cleanup_member_expectation_t;

static int migration_cleanup_digest_valid(const char *digest, int present) {
    size_t length = digest ? strlen(digest) : 0;
    if (!present) return length == 0 ? 0 : -1;
    if (length != 64) return -1;
    for (size_t i = 0; i < length; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f')))
            return -1;
    return 0;
}

static int migration_cleanup_audit_members_parse(
    const char *text, char sources[8][2048], char destinations[8][2048],
    migration_cleanup_member_expectation_t out[8]) {
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *members = root ? yyjson_obj_get(root, "intent") : NULL;
    int valid = yyjson_is_arr(members) && yyjson_arr_size(members) == 8;
    for (int i = 0; valid && i < 8; i++) {
        yyjson_val *member = yyjson_arr_get(members, (size_t)i);
        yyjson_val *index = yyjson_obj_get(member, "index");
        yyjson_val *source = yyjson_obj_get(member, "source");
        yyjson_val *destination = yyjson_obj_get(member, "destination");
        yyjson_val *present = yyjson_obj_get(member, "present");
        yyjson_val *size = yyjson_obj_get(member, "size");
        yyjson_val *sha = yyjson_obj_get(member, "sha256");
        yyjson_val *state = yyjson_obj_get(member, "state");
        int64_t parsed_size = yyjson_is_int(size) ? yyjson_get_sint(size) : -1;
        int parsed_present = yyjson_is_bool(present) ? yyjson_get_bool(present) : -1;
        const char *parsed_sha = yyjson_is_str(sha) ? yyjson_get_str(sha) : NULL;
        const char *parsed_state = yyjson_is_str(state) ? yyjson_get_str(state) : NULL;
        valid = yyjson_is_obj(member) && yyjson_obj_size(member) == 7 &&
                yyjson_is_int(index) && yyjson_get_sint(index) == i &&
                yyjson_is_str(source) && strcmp(yyjson_get_str(source), sources[i]) == 0 &&
                yyjson_is_str(destination) &&
                strcmp(yyjson_get_str(destination), destinations[i]) == 0 &&
                parsed_present >= 0 && parsed_size >= 0 && parsed_state &&
                (strcmp(parsed_state, "live") == 0 ||
                 strcmp(parsed_state, "purge_authorized") == 0 ||
                 strcmp(parsed_state, "purged") == 0) &&
                !(strcmp(parsed_state, "live") != 0 && !parsed_present) &&
                migration_cleanup_digest_valid(parsed_sha, parsed_present) == 0 &&
                (parsed_present || parsed_size == 0);
        if (valid) {
            out[i].present = parsed_present;
            out[i].purge_state = strcmp(parsed_state, "purged") == 0
                                     ? 2
                                     : (strcmp(parsed_state, "purge_authorized") == 0 ? 1 : 0);
            out[i].size = parsed_size;
            snprintf(out[i].sha256, sizeof(out[i].sha256), "%s", parsed_sha);
        }
    }
    yyjson_doc_free(doc);
    return valid ? 0 : -1;
}

static int migration_cleanup_audit_write_states(
    const char *manifest, const char *workspace_id, int64_t generation,
    const cbm_zova_migration_request_t *request, char sources[8][2048],
    char destinations[8][2048], int present[8], int64_t sizes[8],
    char digests[8][CBM_ZOVA_DIGEST_HEX_SIZE], int purge_states[8]) {
    char temporary[2048];
    int temporary_len = snprintf(temporary, sizeof(temporary), "%s.tmp", manifest);
    if (temporary_len <= 0 || (size_t)temporary_len >= sizeof(temporary)) return -1;
    FILE *audit = fopen(temporary, "wb");
    int ok = audit && fprintf(audit,
                              "{\"workspace_id\":\"%s\",\"generation\":%lld,"
                              "\"source_db\":\"%s\",\"source_zova\":\"%s\","
                              "\"db_size\":%lld,\"db_sha256\":\"%s\",\"intent\":[",
                              workspace_id, (long long)generation, request->source_db_path,
                              request->source_zova_path, (long long)sizes[0], digests[0]) >= 0;
    for (int i = 0; ok && i < 8; i++)
        ok = fprintf(audit,
                     "%s{\"index\":%d,\"source\":\"%s\",\"destination\":\"%s\","
                     "\"present\":%s,\"size\":%lld,\"sha256\":\"%s\","
                     "\"state\":\"%s\"}",
                     i ? "," : "", i, sources[i], destinations[i],
                     present[i] ? "true" : "false", (long long)sizes[i], digests[i],
                     purge_states[i] == 2 ? "purged"
                                          : (purge_states[i] == 1 ? "purge_authorized" : "live")) >= 0;
    if (ok) ok = fprintf(audit, "]}\n") >= 0;
    if (audit && fflush(audit) != 0) ok = 0;
#ifndef _WIN32
    if (audit && ok && fsync(fileno(audit)) != 0) ok = 0;
#endif
    if (audit && fclose(audit) != 0) ok = 0;
    if (!ok || rename(temporary, manifest) != 0) {
        (void)remove(temporary);
        return -1;
    }
#ifndef _WIN32
    char parent[2048];
    snprintf(parent, sizeof(parent), "%s", manifest);
    char *slash = strrchr(parent, '/');
    if (!slash) return -1;
    *slash = '\0';
    int directory_fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0 || fsync(directory_fd) != 0) {
        if (directory_fd >= 0) close(directory_fd);
        return -1;
    }
    close(directory_fd);
#endif
    return 0;
}

static int migration_cleanup_sync_directory(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    int directory_fd = open(path, O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0) return -1;
    int rc = fsync(directory_fd);
    close(directory_fd);
    return rc == 0 ? 0 : -1;
#endif
}

static int migration_cleanup_marker_path(const char *source_db_path, char out[2048]) {
    int n = snprintf(out, 2048, "%s.cbm-cleanup-pending", source_db_path);
    return n > 0 && n < 2048 ? 0 : -1;
}

static int migration_cleanup_marker_write(const char *marker, const char *workspace_id,
                                          int64_t generation, const char *source_db_path,
                                          const char *source_db_digest,
                                          const char *source_directory, int create_if_missing) {
    if (strchr(source_db_path, '\n') || strchr(workspace_id, '\n')) return -1;
    char expected[4096];
    int expected_length = snprintf(
        expected, sizeof(expected),
        "CBM_ZOVA_CLEANUP_PENDING_V1\nworkspace=%s\ngeneration=%lld\nsource=%s\nsha256=%s\n",
        workspace_id, (long long)generation, source_db_path, source_db_digest);
    if (expected_length <= 0 || (size_t)expected_length >= sizeof(expected)) return -1;
    FILE *existing = fopen(marker, "rb");
    if (existing) {
        char actual[4096] = {0};
        size_t actual_length = fread(actual, 1, sizeof(actual) - 1, existing);
        int ok = ferror(existing) == 0 && fclose(existing) == 0 &&
                 actual_length == (size_t)expected_length && strcmp(actual, expected) == 0;
        return ok ? 0 : -1;
    }
    if (!create_if_missing) return errno == ENOENT ? 0 : -1;
    char temporary[2048];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", marker);
    if (n <= 0 || (size_t)n >= sizeof(temporary)) return -1;
    FILE *file = fopen(temporary, "wb");
    int ok = file && fwrite(expected, 1, (size_t)expected_length, file) ==
                         (size_t)expected_length;
    if (file && fflush(file) != 0) ok = 0;
#ifndef _WIN32
    if (file && ok && fsync(fileno(file)) != 0) ok = 0;
#endif
    if (file && fclose(file) != 0) ok = 0;
    if (!ok || rename(temporary, marker) != 0 ||
        migration_cleanup_sync_directory(source_directory) != 0) {
        (void)remove(temporary);
        return -1;
    }
    return 0;
}

static int migration_cleanup_marker_remove(const char *marker, const char *source_directory) {
    struct stat marker_stat;
    if (lstat(marker, &marker_stat) != 0) return errno == ENOENT ? 0 : -1;
    return remove(marker) == 0 && migration_cleanup_sync_directory(source_directory) == 0 ? 0
                                                                                          : -1;
}

static int migration_exec(zova_database *db, const char *sql) {
    return zova_database_exec(&(zova_database_exec_request){.db = db, .sql = sql}) == ZOVA_OK
               ? 0
               : -1;
}

static int migration_bind_text(zova_statement *stmt, int index, const char *value) {
    const char *text = value ? value : "";
    return zova_statement_bind_text(&(zova_statement_bind_text_request){
               .statement = stmt, .index = index, .data = (const uint8_t *)text,
               .len = strlen(text)}) == ZOVA_OK
               ? 0
               : -1;
}

static int migration_bind_i64(zova_statement *stmt, int index, int64_t value) {
    return zova_statement_bind_int64(&(zova_statement_bind_int64_request){
               .statement = stmt, .index = index, .value = value}) == ZOVA_OK
               ? 0
               : -1;
}

static int migration_prepare(zova_database *db, const char *sql, zova_statement **out) {
    *out = NULL;
    return zova_database_prepare(&(zova_database_prepare_request){
               .db = db, .sql = sql, .out_statement = out}) == ZOVA_OK
               ? 0
               : -1;
}

static int migration_step(zova_statement *stmt, zova_step_result expected) {
    zova_step_result actual = ZOVA_STEP_DONE;
    return zova_statement_step(&(zova_statement_step_request){
               .statement = stmt, .out_result = &actual}) == ZOVA_OK &&
                   actual == expected
               ? 0
               : -1;
}

static int migration_column_i64(zova_statement *stmt, int index, int64_t *out) {
    return zova_statement_column_int64(&(zova_statement_column_int64_request){
               .statement = stmt, .index = index, .out_value = out}) == ZOVA_OK
               ? 0
               : -1;
}

static int migration_column_text(zova_statement *stmt, int index, char *out, size_t out_size) {
    zova_text text = {0};
    if (!out || out_size == 0 ||
        zova_statement_column_text(&(zova_statement_column_text_request){
            .statement = stmt, .index = index, .out_text = &text}) != ZOVA_OK) {
        zova_text_free(&text);
        return -1;
    }
    if (text.len >= out_size) {
        zova_text_free(&text);
        return -1;
    }
    memcpy(out, text.data ? text.data : "", text.len);
    out[text.len] = '\0';
    zova_text_free(&text);
    return 0;
}

static int migration_state_parse(const char *value, cbm_zova_migration_state_t *out) {
    for (int i = CBM_ZOVA_MIGRATION_PREPARED; i <= CBM_ZOVA_MIGRATION_RETIRED; i++) {
        if (strcmp(value, cbm_zova_migration_state_name((cbm_zova_migration_state_t)i)) == 0) {
            *out = (cbm_zova_migration_state_t)i;
            return 0;
        }
    }
    return -1;
}

static int migration_generation_ready(zova_database *db, const char *workspace_id,
                                      int64_t generation);

static int migration_status_integrity(zova_database *db, const char *workspace_id,
                                      cbm_zova_migration_report_t *report) {
    zova_statement *stmt = NULL;
    int rc = migration_prepare(
        db,
        "SELECT graph_nodes,graph_edges,metadata_nodes,node_vectors,token_vectors,"
        "metadata_sha256,topology_sha256,node_vector_sha256,token_vector_sha256 FROM "
        "cbm_generation_integrity_v2 WHERE workspace_id=?1 AND generation=?2",
        &stmt);
    if (rc == 0) rc = migration_bind_text(stmt, 1, workspace_id);
    if (rc == 0) rc = migration_bind_i64(stmt, 2, report->target_generation);
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_ROW);
    int64_t counts[5] = {0};
    for (int i = 0; rc == 0 && i < 5; i++) rc = migration_column_i64(stmt, i, &counts[i]);
    const char *expected[] = {report->source.metadata_sha256, report->source.topology_sha256,
                              report->source.node_vector_sha256,
                              report->source.token_vector_sha256};
    for (int i = 0; rc == 0 && i < 4; i++) {
        char digest[CBM_ZOVA_DIGEST_HEX_SIZE];
        rc = migration_column_text(stmt, 5 + i, digest, sizeof(digest));
        if (rc == 0 && strcmp(digest, expected[i]) != 0) rc = -1;
    }
    if (stmt) (void)zova_statement_finalize(stmt);
    if (rc != 0) return -1;
    report->target = report->source;
    report->target.workspace_count = 1;
    report->target.stable_id_count = (uint64_t)counts[2];
    report->target.graph_node_count = (uint64_t)counts[0];
    report->target.graph_edge_count = (uint64_t)counts[1];
    report->target.node_vector_count = (uint64_t)counts[3];
    report->target.token_vector_count = (uint64_t)counts[4];
    return 0;
}

static int migration_source_paths_match(zova_database *db, const char *workspace_id,
                                        const cbm_zova_migration_request_t *request) {
    zova_statement *stmt = NULL;
    int rc = migration_prepare(
        db,
        "SELECT source_db_path,source_zova_path,root_path FROM cbm_workspace_migrations_v1 "
        "WHERE workspace_id=?1 AND migration_version=1",
        &stmt);
    if (rc == 0) rc = migration_bind_text(stmt, 1, workspace_id);
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_ROW);
    char source_db[2048] = {0}, source_zova[2048] = {0}, root[2048] = {0};
    if (rc == 0) rc = migration_column_text(stmt, 0, source_db, sizeof(source_db));
    if (rc == 0) rc = migration_column_text(stmt, 1, source_zova, sizeof(source_zova));
    if (rc == 0) rc = migration_column_text(stmt, 2, root, sizeof(root));
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc == 0 && strcmp(source_db, request->source_db_path) == 0 &&
                   strcmp(source_zova, request->source_zova_path) == 0 &&
                   strcmp(root, request->canonical_root) == 0
               ? 0
               : -1;
}

static int migration_status_db(zova_database *db, const char *workspace_id,
                               cbm_zova_migration_report_t *report) {
    zova_statement *stmt = NULL;
    int rc = migration_prepare(
        db,
        "SELECT state,source_generation,target_generation,metadata_sha256,fts_sha256,"
        "topology_sha256,node_vector_sha256,token_vector_sha256 FROM "
        "cbm_workspace_migrations_v1 WHERE workspace_id=?1 AND migration_version=1",
        &stmt);
    if (rc == 0) rc = migration_bind_text(stmt, 1, workspace_id);
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_ROW);
    char state[32] = {0};
    if (rc == 0) rc = migration_column_text(stmt, 0, state, sizeof(state));
    if (rc == 0) rc = migration_state_parse(state, &report->state);
    if (rc == 0) rc = migration_column_i64(stmt, 1, &report->source_generation);
    if (rc == 0) rc = migration_column_i64(stmt, 2, &report->target_generation);
    char *digests[] = {report->source.metadata_sha256, report->source.fts_sha256,
                       report->source.topology_sha256, report->source.node_vector_sha256,
                       report->source.token_vector_sha256};
    for (int i = 0; rc == 0 && i < 5; i++)
        rc = migration_column_text(stmt, 3 + i, digests[i], CBM_ZOVA_DIGEST_HEX_SIZE);
    if (stmt) (void)zova_statement_finalize(stmt);
    if (rc == 0) {
        snprintf(report->workspace_id, sizeof(report->workspace_id), "%s", workspace_id);
        if (report->state == CBM_ZOVA_MIGRATION_ACTIVE) {
            if (migration_generation_ready(db, workspace_id, report->target_generation) != 0 ||
                migration_status_integrity(db, workspace_id, report) != 0)
                rc = -1;
        } else if (report->state == CBM_ZOVA_MIGRATION_RETIRED) {
            if (migration_status_integrity(db, workspace_id, report) != 0) rc = -1;
        }
    }
    return rc;
}

bool cbm_zova_migration_test_fault(const char *phase) {
    const char *crash = getenv("CBM_ZOVA_TEST_CRASH_PHASE");
    if (crash && strcmp(crash, phase) == 0) {
#ifdef _WIN32
        abort();
#else
        _exit(86);
#endif
    }
    const char *fault = getenv("CBM_ZOVA_TEST_FAIL_PHASE");
    return fault && strcmp(fault, phase) == 0;
}

static int migration_manifest_digests_equal(const cbm_zova_migration_manifest_t *left,
                                            const cbm_zova_migration_manifest_t *right) {
    return strcmp(left->metadata_sha256, right->metadata_sha256) == 0 &&
           strcmp(left->fts_sha256, right->fts_sha256) == 0 &&
           strcmp(left->topology_sha256, right->topology_sha256) == 0 &&
           strcmp(left->node_vector_sha256, right->node_vector_sha256) == 0 &&
           strcmp(left->token_vector_sha256, right->token_vector_sha256) == 0;
}

static int migration_generation_ready(zova_database *db, const char *workspace_id,
                                      int64_t generation) {
    zova_statement *stmt = NULL;
    int rc = migration_prepare(
        db,
        "SELECT count(*) FROM cbm_workspace_registry r JOIN cbm_database_generation_v1 g ON "
        "g.workspace_id=r.workspace_id WHERE r.workspace_id=?1 AND r.active_generation=?2 AND "
        "g.generation=?2 AND g.state='ready'",
        &stmt);
    if (rc == 0) rc = migration_bind_text(stmt, 1, workspace_id);
    if (rc == 0) rc = migration_bind_i64(stmt, 2, generation);
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_ROW);
    int64_t count = 0;
    if (rc == 0) rc = migration_column_i64(stmt, 0, &count);
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc == 0 && count == 1 ? 0 : -1;
}

static int migration_manifest_phase_fault_configured(void) {
    const char *fault = getenv("CBM_ZOVA_TEST_FAIL_PHASE");
    if (!fault) return 0;
    const char *phases[] = {"migration_after_counts", "migration_after_stable_ids",
                            "migration_after_graph", "migration_after_vectors",
                            "migration_after_metadata", "migration_after_fts"};
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++)
        if (strcmp(fault, phases[i]) == 0) return 1;
    return 0;
}

static int migration_write_prepared(zova_database *db, const cbm_zova_migration_request_t *request,
                                    const cbm_zova_legacy_snapshot_t *snapshot) {
    const cbm_zova_workspace_generation_input_t *input = cbm_zova_legacy_snapshot_input(snapshot);
    const cbm_zova_migration_manifest_t *manifest = cbm_zova_legacy_snapshot_manifest(snapshot);
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_id, sizeof(workspace_id), "%s",
             cbm_zova_legacy_snapshot_workspace_id(snapshot));
    if (migration_exec(db, "BEGIN IMMEDIATE") != 0) return -1;
    zova_statement *registry = NULL;
    int rc = migration_prepare(
        db,
        "INSERT INTO cbm_workspace_registry(workspace_id,canonical_root,id_format_version,"
        "active_generation) VALUES(?1,?2,2,0) ON CONFLICT(canonical_root) DO NOTHING",
        &registry);
    if (rc == 0) rc = migration_bind_text(registry, 1, workspace_id);
    if (rc == 0) rc = migration_bind_text(registry, 2, input->root_path);
    if (rc == 0) rc = migration_step(registry, ZOVA_STEP_DONE);
    if (registry) (void)zova_statement_finalize(registry);
    zova_statement *stmt = NULL;
    if (rc == 0)
        rc = migration_prepare(
            db,
            "INSERT INTO cbm_workspace_migrations_v1(workspace_id,migration_version,project,"
            "root_path,source_db_path,source_zova_path,source_generation,target_generation,state,"
            "metadata_sha256,fts_sha256,topology_sha256,node_vector_sha256,token_vector_sha256,"
            "prepared_at) VALUES(?1,1,?2,?3,?4,?5,?6,0,'prepared',?7,?8,?9,?10,?11,"
            "CURRENT_TIMESTAMP) ON CONFLICT(workspace_id) DO UPDATE SET project=excluded.project,"
            "root_path=excluded.root_path,source_db_path=excluded.source_db_path,"
            "source_zova_path=excluded.source_zova_path,source_generation=excluded.source_generation,"
            "target_generation=0,state='prepared',metadata_sha256=excluded.metadata_sha256,"
            "fts_sha256=excluded.fts_sha256,topology_sha256=excluded.topology_sha256,"
            "node_vector_sha256=excluded.node_vector_sha256,"
            "token_vector_sha256=excluded.token_vector_sha256,prepared_at=CURRENT_TIMESTAMP,"
            "activated_at=NULL,retired_at=NULL",
            &stmt);
    const char *texts[] = {workspace_id, input->project, input->root_path, request->source_db_path,
                           request->source_zova_path};
    for (int i = 0; rc == 0 && i < 5; i++) rc = migration_bind_text(stmt, 1 + i, texts[i]);
    if (rc == 0)
        rc = migration_bind_i64(stmt, 6, cbm_zova_legacy_snapshot_source_generation(snapshot));
    const char *digests[] = {manifest->metadata_sha256, manifest->fts_sha256,
                             manifest->topology_sha256, manifest->node_vector_sha256,
                             manifest->token_vector_sha256};
    for (int i = 0; rc == 0 && i < 5; i++) rc = migration_bind_text(stmt, 7 + i, digests[i]);
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_DONE);
    if (stmt) (void)zova_statement_finalize(stmt);
    if (rc == 0) rc = migration_exec(db, "COMMIT");
    if (rc != 0) (void)migration_exec(db, "ROLLBACK");
    return rc;
}

static int migration_set_state_tx(zova_database *db, const char *workspace_id,
                                  const char *state, int64_t generation, int activate) {
    zova_statement *stmt = NULL;
    int rc = migration_prepare(
        db,
        activate ? "UPDATE cbm_workspace_migrations_v1 SET state=?1,target_generation=?2,"
                   "activated_at=CURRENT_TIMESTAMP WHERE workspace_id=?3"
                 : "UPDATE cbm_workspace_migrations_v1 SET state=?1 WHERE workspace_id=?2",
        &stmt);
    if (rc == 0) rc = migration_bind_text(stmt, 1, state);
    if (activate) {
        if (rc == 0) rc = migration_bind_i64(stmt, 2, generation);
        if (rc == 0) rc = migration_bind_text(stmt, 3, workspace_id);
    } else if (rc == 0) {
        rc = migration_bind_text(stmt, 2, workspace_id);
    }
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_DONE);
    if (stmt) (void)zova_statement_finalize(stmt);
    return rc;
}

static int migration_record_failed(zova_database *db, const char *workspace_id) {
    if (migration_exec(db, "BEGIN IMMEDIATE") != 0) return -1;
    int rc = migration_set_state_tx(db, workspace_id, "failed", 0, 0);
    if (rc == 0) rc = migration_exec(db, "COMMIT");
    if (rc != 0) (void)migration_exec(db, "ROLLBACK");
    return rc;
}

static int migration_transition_only(zova_database *db, const char *workspace_id,
                                     const char *from, const char *to) {
    if (migration_exec(db, "BEGIN IMMEDIATE") != 0) return -1;
    zova_statement *stmt = NULL;
    int rc = migration_prepare(
        db,
        "UPDATE cbm_workspace_migrations_v1 SET state=?1 WHERE workspace_id=?2 AND state=?3",
        &stmt);
    if (rc == 0) rc = migration_bind_text(stmt, 1, to);
    if (rc == 0) rc = migration_bind_text(stmt, 2, workspace_id);
    if (rc == 0) rc = migration_bind_text(stmt, 3, from);
    if (rc == 0) rc = migration_step(stmt, ZOVA_STEP_DONE);
    if (stmt) (void)zova_statement_finalize(stmt);
    zova_statement *changes = NULL;
    if (rc == 0) rc = migration_prepare(db, "SELECT changes()", &changes);
    if (rc == 0) rc = migration_step(changes, ZOVA_STEP_ROW);
    int64_t changed = 0;
    if (rc == 0) rc = migration_column_i64(changes, 0, &changed);
    if (changes) (void)zova_statement_finalize(changes);
    if (rc == 0 && changed != 1) rc = -1;
    if (rc == 0) rc = migration_exec(db, "COMMIT");
    if (rc != 0) (void)migration_exec(db, "ROLLBACK");
    return rc;
}

cbm_zova_migration_code_t cbm_zova_migration_cleanup(
    const cbm_zova_migration_request_t *request, const char *confirmed_workspace_id,
    cbm_zova_migration_report_t *out_report) {
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!request || !out_report || !request->source_db_path || !request->source_zova_path ||
        !request->target_zova_path || !request->canonical_root)
        return CBM_ZOVA_MIGRATION_INVALID;
    if (!confirmed_workspace_id || !confirmed_workspace_id[0])
        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
    struct stat live_db_stat;
    struct stat live_zova_stat;
    int live_db_exists = stat(request->source_db_path, &live_db_stat) == 0;
    int live_zova_exists = stat(request->source_zova_path, &live_zova_stat) == 0;
    if (live_db_exists && live_zova_exists) {
        sqlite3 *ledger = NULL;
        sqlite3_stmt *state_stmt = NULL;
        int64_t generation = 0;
        const char *state = NULL;
        if (sqlite3_open_v2(request->target_zova_path, &ledger, SQLITE_OPEN_READONLY, NULL) ==
                SQLITE_OK &&
            sqlite3_prepare_v2(ledger,
                               "SELECT state,source_generation FROM "
                               "cbm_workspace_migrations_v1 WHERE workspace_id=?1",
                               -1, &state_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(state_stmt, 1, confirmed_workspace_id, -1, SQLITE_STATIC);
            if (sqlite3_step(state_stmt) == SQLITE_ROW) {
                state = (const char *)sqlite3_column_text(state_stmt, 0);
                generation = sqlite3_column_int64(state_stmt, 1);
            }
            if (state && strcmp(state, "cleanup_pending") == 0) {
                char parent[2048], quarantine_db[2048];
                snprintf(parent, sizeof(parent), "%s", request->source_db_path);
                char *separator = strrchr(parent, '/');
                const char *basename = separator ? separator + 1 : "";
                if (separator) *separator = '\0';
                snprintf(quarantine_db, sizeof(quarantine_db),
                         "%s/.zova-retired/%s-g%lld/%s", parent, confirmed_workspace_id,
                         (long long)generation, basename);
                struct stat quarantine_stat;
                if (stat(quarantine_db, &quarantine_stat) == 0) {
                    sqlite3_finalize(state_stmt);
                    sqlite3_close(ledger);
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                }
            }
        }
        sqlite3_finalize(state_stmt);
        if (ledger) sqlite3_close(ledger);
    }
    if (!live_db_exists || !live_zova_exists) {
        sqlite3 *ledger = NULL;
        sqlite3_stmt *stmt = NULL;
        int64_t generation = 0;
        char state[32] = {0}, recorded_db[2048] = {0};
        int recovery_ok = sqlite3_open_v2(request->target_zova_path, &ledger,
                                          SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
                          sqlite3_prepare_v2(
                              ledger,
                              "SELECT state,source_generation,source_db_path FROM "
                              "cbm_workspace_migrations_v1 WHERE workspace_id=?1",
                              -1, &stmt, NULL) == SQLITE_OK;
        if (recovery_ok) sqlite3_bind_text(stmt, 1, confirmed_workspace_id, -1, SQLITE_STATIC);
        if (recovery_ok && sqlite3_step(stmt) == SQLITE_ROW) {
            snprintf(state, sizeof(state), "%s", sqlite3_column_text(stmt, 0));
            generation = sqlite3_column_int64(stmt, 1);
            snprintf(recorded_db, sizeof(recorded_db), "%s", sqlite3_column_text(stmt, 2));
        } else recovery_ok = 0;
        sqlite3_finalize(stmt);
        sqlite3_close(ledger);
        char parent[2048], quarantine_db[2048], audit_path[2048];
        snprintf(parent, sizeof(parent), "%s", request->source_db_path);
        char *separator = strrchr(parent, '/');
        const char *basename = separator ? separator + 1 : "";
        if (!separator) recovery_ok = 0;
        else *separator = '\0';
        snprintf(quarantine_db, sizeof(quarantine_db), "%s/.zova-retired/%s-g%lld/%s",
                 parent, confirmed_workspace_id, (long long)generation, basename);
        snprintf(audit_path, sizeof(audit_path), "%s/.zova-retired/%s-g%lld/manifest.json",
                 parent, confirmed_workspace_id, (long long)generation);
        int64_t quarantine_size = 0;
        char quarantine_digest[CBM_ZOVA_DIGEST_HEX_SIZE] = {0}, audit_text[4096] = {0};
        FILE *audit_file = fopen(audit_path, "rb");
        if (audit_file) {
            (void)fread(audit_text, 1, sizeof(audit_text) - 1, audit_file);
            fclose(audit_file);
        }
        const char *zova_leaf = strrchr(request->source_zova_path, '/');
        zova_leaf = zova_leaf ? zova_leaf + 1 : request->source_zova_path;
        char quarantine_root[2048];
        snprintf(quarantine_root, sizeof(quarantine_root), "%s/.zova-retired/%s-g%lld", parent,
                 confirmed_workspace_id, (long long)generation);
        char expected_sources[8][2048] = {{0}}, expected_destinations[8][2048] = {{0}};
        migration_cleanup_member_expectation_t expectations[8] = {{0}};
        snprintf(expected_sources[0], sizeof(expected_sources[0]), "%s", request->source_db_path);
        snprintf(expected_sources[1], sizeof(expected_sources[1]), "%s", request->source_zova_path);
        static const char *const expected_suffixes[] = {"-wal", "-shm", "-journal"};
        for (int base = 0; base < 2; base++)
            for (int suffix = 0; suffix < 3; suffix++)
                snprintf(expected_sources[2 + base * 3 + suffix],
                         sizeof(expected_sources[2 + base * 3 + suffix]), "%s%s",
                         base == 0 ? request->source_db_path : request->source_zova_path,
                         expected_suffixes[suffix]);
        for (int i = 0; i < 8; i++) {
            const char *leaf = strrchr(expected_sources[i], '/');
            leaf = leaf ? leaf + 1 : expected_sources[i];
            snprintf(expected_destinations[i], sizeof(expected_destinations[i]), "%s/%s",
                     quarantine_root, leaf);
        }
        int header_rc = migration_cleanup_audit_header_valid(audit_text, confirmed_workspace_id,
                                                              generation, request);
        int members_rc = migration_cleanup_audit_members_parse(
            audit_text, expected_sources, expected_destinations, expectations);
        int allowlist_rc = migration_cleanup_quarantine_allowlisted(quarantine_root, basename,
                                                                    zova_leaf);
        recovery_ok = recovery_ok && header_rc == 0 && members_rc == 0 && allowlist_rc == 0;
        if (recovery_ok && strcmp(state, "retired") == 0) {
            cbm_zova_migration_report_t retired_status = {0};
            if (cbm_zova_migration_status(request, &retired_status) != CBM_ZOVA_MIGRATION_OK ||
                retired_status.state != CBM_ZOVA_MIGRATION_RETIRED)
                return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            int parsed_present[8] = {0}, parsed_purge_states[8] = {0};
            int64_t parsed_sizes[8] = {0};
            char parsed_digests[8][CBM_ZOVA_DIGEST_HEX_SIZE] = {{0}};
            for (int i = 0; i < 8; i++) {
                struct stat quarantined_stat;
                int exists = stat(expected_destinations[i], &quarantined_stat) == 0;
                parsed_present[i] = expectations[i].present;
                parsed_purge_states[i] = expectations[i].purge_state;
                parsed_sizes[i] = expectations[i].size;
                snprintf(parsed_digests[i], sizeof(parsed_digests[i]), "%s",
                         expectations[i].sha256);
                if ((!expectations[i].present && exists) ||
                    (expectations[i].present && expectations[i].purge_state == 2 && exists) ||
                    (expectations[i].present && expectations[i].purge_state == 0 && !exists))
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                if (exists) {
                    int64_t actual_size = 0;
                    char actual_digest[CBM_ZOVA_DIGEST_HEX_SIZE] = {0};
                    if (migration_file_digest(expected_destinations[i], &actual_size,
                                              actual_digest) != 0 ||
                        actual_size != expectations[i].size ||
                        strcmp(actual_digest, expectations[i].sha256) != 0)
                        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                }
            }
            for (int i = 0; i < 8; i++) {
                if (!parsed_present[i] || parsed_purge_states[i] == 2) continue;
                if (parsed_purge_states[i] == 0) {
                    parsed_purge_states[i] = 1;
                    if (migration_cleanup_audit_write_states(
                            audit_path, confirmed_workspace_id, generation, request,
                            expected_sources, expected_destinations, parsed_present, parsed_sizes,
                            parsed_digests, parsed_purge_states) != 0)
                        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                }
                struct stat purge_stat;
                if (stat(expected_destinations[i], &purge_stat) == 0 &&
                    (remove(expected_destinations[i]) != 0 ||
                     migration_cleanup_sync_directory(quarantine_root) != 0))
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                parsed_purge_states[i] = 2;
                if (migration_cleanup_audit_write_states(
                        audit_path, confirmed_workspace_id, generation, request,
                        expected_sources, expected_destinations, parsed_present, parsed_sizes,
                        parsed_digests, parsed_purge_states) != 0)
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                char purge_phase[64];
                snprintf(purge_phase, sizeof(purge_phase), "cleanup_after_purge_%d", i);
                if (cbm_zova_migration_test_fault(purge_phase))
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            }
            char marker[2048];
            if (migration_cleanup_marker_path(recorded_db, marker) != 0 ||
                migration_cleanup_marker_write(marker, confirmed_workspace_id, generation,
                                               recorded_db, expectations[0].sha256, parent, 0) != 0 ||
                migration_cleanup_marker_remove(marker, parent) != 0)
                return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            *out_report = retired_status;
            out_report->code = CBM_ZOVA_MIGRATION_OK;
            return CBM_ZOVA_MIGRATION_OK;
        }
        recovery_ok = recovery_ok && strcmp(state, "cleanup_pending") == 0 &&
                      strcmp(recorded_db, request->source_db_path) == 0 && audit_text[0] &&
                      migration_file_digest(quarantine_db, &quarantine_size,
                                            quarantine_digest) == 0 &&
                      expectations[0].present && quarantine_size == expectations[0].size &&
                      strcmp(quarantine_digest, expectations[0].sha256) == 0;
        if (!recovery_ok ||
            (stat(request->source_db_path, &live_db_stat) != 0 &&
             rename(quarantine_db, request->source_db_path) != 0)) {
            return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        }
        if (stat(request->source_zova_path, &live_zova_stat) != 0) {
            const char *zova_basename = strrchr(request->source_zova_path, '/');
            zova_basename = zova_basename ? zova_basename + 1 : request->source_zova_path;
            char quarantine_zova[2048], zova_digest[CBM_ZOVA_DIGEST_HEX_SIZE] = {0};
            int64_t zova_size = 0;
            snprintf(quarantine_zova, sizeof(quarantine_zova),
                     "%s/.zova-retired/%s-g%lld/%s", parent, confirmed_workspace_id,
                     (long long)generation, zova_basename);
            if (migration_file_digest(quarantine_zova, &zova_size, zova_digest) != 0 ||
                !expectations[1].present || zova_size != expectations[1].size ||
                strcmp(zova_digest, expectations[1].sha256) != 0 ||
                rename(quarantine_zova, request->source_zova_path) != 0)
            {
                return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            }
        }
        static const char *const resume_suffixes[] = {"-wal", "-shm", "-journal"};
        const char *resume_bases[] = {request->source_db_path, request->source_zova_path};
        for (int base = 0; base < 2; base++) {
            const char *resume_name = strrchr(resume_bases[base], '/');
            resume_name = resume_name ? resume_name + 1 : resume_bases[base];
            for (int suffix = 0; suffix < 3; suffix++) {
                char live_optional[2048], quarantine_optional[2048];
                snprintf(live_optional, sizeof(live_optional), "%s%s", resume_bases[base],
                         resume_suffixes[suffix]);
                snprintf(quarantine_optional, sizeof(quarantine_optional),
                         "%s/.zova-retired/%s-g%lld/%s%s", parent, confirmed_workspace_id,
                         (long long)generation, resume_name, resume_suffixes[suffix]);
                struct stat live_optional_stat, quarantine_optional_stat;
                int live_exists = stat(live_optional, &live_optional_stat) == 0;
                int quarantine_exists = stat(quarantine_optional, &quarantine_optional_stat) == 0;
                if (live_exists && quarantine_exists) {
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                }
                int expectation_index = 2 + base * 3 + suffix;
                if (!expectations[expectation_index].present &&
                    (live_exists || quarantine_exists))
                {
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                }
                if (expectations[expectation_index].present &&
                    !live_exists && !quarantine_exists)
                {
                    return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                }
                if (quarantine_exists) {
                    char optional_digest[CBM_ZOVA_DIGEST_HEX_SIZE] = {0};
                    int64_t optional_size = 0;
                    if (migration_file_digest(quarantine_optional, &optional_size,
                                              optional_digest) != 0 ||
                        optional_size != expectations[expectation_index].size ||
                        strcmp(optional_digest, expectations[expectation_index].sha256) != 0 ||
                        rename(quarantine_optional, live_optional) != 0)
                    {
                        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                    }
                }
            }
        }
    }
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    cbm_zova_migration_code_t code = cbm_zova_legacy_snapshot_open(
        request->source_db_path, request->source_zova_path, request->canonical_root, &snapshot);
    if (code != CBM_ZOVA_MIGRATION_OK) {
        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_id, sizeof(workspace_id), "%s",
             cbm_zova_legacy_snapshot_workspace_id(snapshot));
    if (strcmp(confirmed_workspace_id, workspace_id) != 0) {
        cbm_zova_legacy_snapshot_close(snapshot);
        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
    }
    if (migration_cleanup_explicit_cache_contains(request) != 0 ||
        migration_cleanup_source_bundle_safe(request) != 0) {
        cbm_zova_legacy_snapshot_close(snapshot);
        return CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
    }
    cbm_zova_writer_guard_t guard = {0};
    zova_database *db = NULL;
    sqlite3 *source_lock = NULL;
    if (cbm_zova_writer_guard_acquire(request->target_zova_path, &guard) != 0 ||
        zova_database_open(&(zova_database_open_request){
            .path = request->target_zova_path, .out_db = &db}) != ZOVA_OK) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    cbm_zova_migration_report_t active = {0};
    if (migration_source_paths_match(db, workspace_id, request) != 0 ||
        migration_status_db(db, workspace_id, &active) != 0 ||
        (active.state != CBM_ZOVA_MIGRATION_ACTIVE &&
         active.state != CBM_ZOVA_MIGRATION_CLEANUP_PENDING) ||
        active.source_generation != cbm_zova_legacy_snapshot_source_generation(snapshot) ||
        !migration_manifest_digests_equal(&active.source,
                                          cbm_zova_legacy_snapshot_manifest(snapshot)) ||
        migration_generation_ready(db, workspace_id, active.target_generation) != 0 ||
        cbm_zova_migration_manifest_target_tx(db, workspace_id, active.target_generation,
                                              snapshot, &active.target) !=
            CBM_ZOVA_MIGRATION_OK) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    cbm_zova_legacy_snapshot_close(snapshot);
    snapshot = NULL;
    int wal_frames = 0, checkpointed_frames = 0;
    if (sqlite3_open_v2(request->source_db_path, &source_lock,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL) != SQLITE_OK ||
        sqlite3_busy_timeout(source_lock, 0) != SQLITE_OK ||
        sqlite3_wal_checkpoint_v2(source_lock, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                                  &wal_frames, &checkpointed_frames) != SQLITE_OK ||
        wal_frames != checkpointed_frames ||
        sqlite3_exec(source_lock, "PRAGMA locking_mode=EXCLUSIVE;BEGIN EXCLUSIVE", NULL, NULL,
                     NULL) != SQLITE_OK) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    char source_dir[1024];
    snprintf(source_dir, sizeof(source_dir), "%s", request->source_db_path);
    char *slash = strrchr(source_dir, '/');
#ifdef _WIN32
    char *backslash = strrchr(source_dir, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (!slash) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    *slash = '\0';
    struct stat source_parent_stat, source_db_stat;
    const char *device_fault = getenv("CBM_ZOVA_TEST_CLEANUP_CROSS_DEVICE");
    if (stat(source_dir, &source_parent_stat) != 0 ||
        stat(request->source_db_path, &source_db_stat) != 0 ||
        source_parent_stat.st_dev != source_db_stat.st_dev ||
        (device_fault && strcmp(device_fault, "1") == 0)) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    char retired_root[2048];
    snprintf(retired_root, sizeof(retired_root), "%s/.zova-retired", source_dir);
    struct stat retired_root_stat;
#ifndef _WIN32
    if (lstat(retired_root, &retired_root_stat) == 0 &&
        (!S_ISDIR(retired_root_stat.st_mode) || retired_root_stat.st_dev != source_db_stat.st_dev)) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
#endif
    char quarantine[2048], retired_db[2048], retired_zova[2048], manifest[2048];
    int qn = snprintf(quarantine, sizeof(quarantine), "%s/.zova-retired/%s-g%lld", source_dir,
                      workspace_id, (long long)active.source_generation);
    const char *db_name = slash + 1;
    const char *zova_name = strrchr(request->source_zova_path, '/');
    zova_name = zova_name ? zova_name + 1 : request->source_zova_path;
    if (qn <= 0 || (size_t)qn >= sizeof(quarantine) || !cbm_mkdir_p(quarantine, 0700) ||
        snprintf(retired_db, sizeof(retired_db), "%s/%s", quarantine, db_name) <= 0 ||
        snprintf(retired_zova, sizeof(retired_zova), "%s/%s", quarantine, zova_name) <= 0 ||
        snprintf(manifest, sizeof(manifest), "%s/manifest.json", quarantine) <= 0) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    char intent_sources[8][2048] = {{0}};
    char intent_destinations[8][2048] = {{0}};
    char intent_digests[8][CBM_ZOVA_DIGEST_HEX_SIZE] = {{0}};
    int64_t intent_sizes[8] = {0};
    snprintf(intent_sources[0], sizeof(intent_sources[0]), "%s", request->source_db_path);
    snprintf(intent_sources[1], sizeof(intent_sources[1]), "%s", request->source_zova_path);
    static const char *const intent_suffixes[] = {"-wal", "-shm", "-journal"};
    for (int base = 0; base < 2; base++)
        for (int suffix = 0; suffix < 3; suffix++)
            snprintf(intent_sources[2 + base * 3 + suffix],
                     sizeof(intent_sources[2 + base * 3 + suffix]), "%s%s",
                     base == 0 ? request->source_db_path : request->source_zova_path,
                     intent_suffixes[suffix]);
    for (int i = 0; i < 8; i++)
    {
        (void)migration_file_digest(intent_sources[i], &intent_sizes[i], intent_digests[i]);
        const char *leaf = strrchr(intent_sources[i], '/');
        leaf = leaf ? leaf + 1 : intent_sources[i];
        snprintf(intent_destinations[i], sizeof(intent_destinations[i]), "%s/%s", quarantine,
                 leaf);
    }
    int intent_present[8] = {0}, intent_states[8] = {0};
    for (int i = 0; i < 8; i++) intent_present[i] = intent_digests[i][0] != '\0';
    if (migration_cleanup_audit_write_states(
            manifest, workspace_id, active.source_generation, request, intent_sources,
            intent_destinations, intent_present, intent_sizes, intent_digests,
            intent_states) != 0) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    char cleanup_marker[2048];
    if (migration_cleanup_marker_path(request->source_db_path, cleanup_marker) != 0 ||
        migration_cleanup_marker_write(cleanup_marker, workspace_id, active.source_generation,
                                       request->source_db_path, intent_digests[0], source_dir, 1) !=
            0) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    bool wrote_intent = active.state == CBM_ZOVA_MIGRATION_ACTIVE;
    if (wrote_intent && cbm_zova_migration_test_fault("cleanup_after_marker")) {
        (void)migration_cleanup_marker_remove(cleanup_marker, source_dir);
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    if (wrote_intent &&
        migration_transition_only(db, workspace_id, "active", "cleanup_pending") != 0) {
        (void)migration_cleanup_marker_remove(cleanup_marker, source_dir);
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    if (wrote_intent && cbm_zova_migration_test_fault("cleanup_after_intent")) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    if (sqlite3_exec(source_lock, "COMMIT", NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_close(source_lock) != SQLITE_OK) {
        source_lock = NULL;
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    source_lock = NULL;
    /* Closing the exclusive SQLite handle may remove transient WAL/SHM files.
     * Refresh the durable intent inventory at the stable post-close boundary,
     * before any member rename. */
    memset(intent_digests, 0, sizeof(intent_digests));
    memset(intent_sizes, 0, sizeof(intent_sizes));
    for (int i = 0; i < 8; i++)
        (void)migration_file_digest(intent_sources[i], &intent_sizes[i], intent_digests[i]);
    for (int i = 0; i < 8; i++) intent_present[i] = intent_digests[i][0] != '\0';
    if (migration_cleanup_audit_write_states(
            manifest, workspace_id, active.source_generation, request, intent_sources,
            intent_destinations, intent_present, intent_sizes, intent_digests,
            intent_states) != 0) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    (void)zova_database_close(db);
    db = NULL;
    int db_move = rename(request->source_db_path, retired_db);
    if (db_move == 0 && migration_cleanup_sync_directory(quarantine) != 0) db_move = -1;
    if (db_move == 0 && cbm_zova_migration_test_fault("cleanup_after_member_0")) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    int zova_move = db_move == 0 ? rename(request->source_zova_path, retired_zova) : -1;
    if (zova_move == 0 && migration_cleanup_sync_directory(quarantine) != 0) zova_move = -1;
    if (zova_move == 0 && cbm_zova_migration_test_fault("cleanup_after_member_1")) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    static const char *const recovery_suffixes[] = {"-wal", "-shm", "-journal"};
    char moved_recovery[6][2048] = {{0}};
    int recovery_move = 0;
    for (int base = 0; zova_move == 0 && base < 2; base++) {
        const char *source_base = base == 0 ? request->source_db_path : request->source_zova_path;
        const char *base_name = strrchr(source_base, '/');
        base_name = base_name ? base_name + 1 : source_base;
        for (int suffix = 0; suffix < 3; suffix++) {
            char live[2048];
            struct stat member_stat;
            snprintf(live, sizeof(live), "%s%s", source_base, recovery_suffixes[suffix]);
            int index = base * 3 + suffix;
            if (stat(live, &member_stat) == 0) {
                snprintf(moved_recovery[index], sizeof(moved_recovery[index]), "%s/%s%s",
                         quarantine, base_name, recovery_suffixes[suffix]);
                if (rename(live, moved_recovery[index]) != 0) {
                    recovery_move = -1;
                    break;
                }
                if (migration_cleanup_sync_directory(quarantine) != 0) {
                    recovery_move = -1;
                    break;
                }
            }
            char member_phase[64];
            snprintf(member_phase, sizeof(member_phase), "cleanup_after_member_%d", index + 2);
            if (cbm_zova_migration_test_fault(member_phase)) {
                code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
                goto cleanup_done;
            }
        }
        if (recovery_move != 0) break;
    }
    int target_reopen = zova_move == 0 && recovery_move == 0
                            ? (zova_database_open(&(zova_database_open_request){
                                   .path = request->target_zova_path, .out_db = &db}) == ZOVA_OK
                                   ? 0
                                   : -1)
                            : -1;
    int ready_after_move = target_reopen == 0
                               ? migration_generation_ready(db, workspace_id,
                                                            active.target_generation)
                               : -1;
    if (ready_after_move == 0 &&
        cbm_zova_migration_test_fault("cleanup_after_target_reverify")) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    int retire = ready_after_move == 0
                     ? migration_transition_only(db, workspace_id, "cleanup_pending", "retired")
                     : -1;
    if (db_move != 0 || zova_move != 0 || recovery_move != 0 || ready_after_move != 0 ||
        retire != 0) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    if (cbm_zova_migration_test_fault("cleanup_after_retired_commit")) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    char member_source[8][2048] = {{0}};
    char member_digest[8][CBM_ZOVA_DIGEST_HEX_SIZE] = {{0}};
    int64_t member_size[8] = {0};
    int member_present[8] = {0};
    snprintf(member_source[0], sizeof(member_source[0]), "%s", request->source_db_path);
    snprintf(member_source[1], sizeof(member_source[1]), "%s", request->source_zova_path);
    char member_dest[8][2048] = {{0}};
    for (int i = 0; i < 8; i++)
        snprintf(member_dest[i], sizeof(member_dest[i]), "%s", intent_destinations[i]);
    for (int base = 0; base < 2; base++)
        for (int suffix = 0; suffix < 3; suffix++)
            snprintf(member_source[2 + base * 3 + suffix],
                     sizeof(member_source[2 + base * 3 + suffix]), "%s%s",
                     base == 0 ? request->source_db_path : request->source_zova_path,
                     recovery_suffixes[suffix]);
    for (int i = 0; i < 8; i++)
        member_present[i] = member_dest[i][0] &&
                            migration_file_digest(member_dest[i], &member_size[i],
                                                  member_digest[i]) == 0;
    int purge_states[8] = {0};
    for (int i = 0; i < 8; i++) {
        if (!member_present[i]) continue;
        purge_states[i] = 1;
        if (migration_cleanup_audit_write_states(
                manifest, workspace_id, active.source_generation, request, member_source,
                member_dest, member_present, member_size, member_digest, purge_states) != 0 ||
            remove(member_dest[i]) != 0 ||
            migration_cleanup_sync_directory(quarantine) != 0) {
            code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            goto cleanup_done;
        }
        purge_states[i] = 2;
        if (migration_cleanup_audit_write_states(
                manifest, workspace_id, active.source_generation, request, member_source,
                member_dest, member_present, member_size, member_digest, purge_states) != 0) {
            code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            goto cleanup_done;
        }
        char purge_phase[64];
        snprintf(purge_phase, sizeof(purge_phase), "cleanup_after_purge_%d", i);
        if (cbm_zova_migration_test_fault(purge_phase)) {
            code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
            goto cleanup_done;
        }
    }
    if (migration_cleanup_marker_remove(cleanup_marker, source_dir) != 0) {
        code = CBM_ZOVA_MIGRATION_CLEANUP_REFUSED;
        goto cleanup_done;
    }
    *out_report = active;
    out_report->state = CBM_ZOVA_MIGRATION_RETIRED;
    out_report->code = CBM_ZOVA_MIGRATION_OK;
    code = CBM_ZOVA_MIGRATION_OK;
cleanup_done:
    if (source_lock) {
        (void)sqlite3_exec(source_lock, "ROLLBACK", NULL, NULL, NULL);
        (void)sqlite3_close(source_lock);
    }
    if (db) (void)zova_database_close(db);
    cbm_zova_legacy_snapshot_close(snapshot);
    cbm_zova_writer_guard_release(&guard);
    return code;
}

cbm_zova_migration_code_t cbm_zova_migration_rollback(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report) {
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!request || !out_report || !request->source_db_path || !request->source_zova_path ||
        !request->target_zova_path || !request->canonical_root)
        return CBM_ZOVA_MIGRATION_INVALID;
    cbm_zova_migration_report_t current = {0};
    if (cbm_zova_migration_status(request, &current) == CBM_ZOVA_MIGRATION_OK &&
        current.state == CBM_ZOVA_MIGRATION_RETIRED) {
        *out_report = current;
        out_report->code = CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE;
        return CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE;
    }
    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_acquire(request->target_zova_path, &guard) != 0)
        return CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    cbm_zova_migration_code_t code = cbm_zova_legacy_snapshot_open(
        request->source_db_path, request->source_zova_path, request->canonical_root, &snapshot);
    zova_database *db = NULL;
    if (code != CBM_ZOVA_MIGRATION_OK) goto done;
    const char *workspace_id = cbm_zova_legacy_snapshot_workspace_id(snapshot);
    if (zova_database_open(&(zova_database_open_request){
            .path = request->target_zova_path, .out_db = &db}) != ZOVA_OK) {
        code = CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
        goto done;
    }
    cbm_zova_migration_report_t active = {0};
    if (migration_status_db(db, workspace_id, &active) != 0 ||
        active.state != CBM_ZOVA_MIGRATION_ACTIVE ||
        active.source_generation != cbm_zova_legacy_snapshot_source_generation(snapshot) ||
        !migration_manifest_digests_equal(&active.source,
                                          cbm_zova_legacy_snapshot_manifest(snapshot)) ||
        migration_generation_ready(db, workspace_id, active.target_generation) != 0 ||
        cbm_zova_migration_manifest_target_tx(db, workspace_id, active.target_generation, snapshot,
                                              &active.target) != CBM_ZOVA_MIGRATION_OK) {
        code = CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE;
        goto done;
    }
    if (migration_transition_only(db, workspace_id, "active", "rolled_back") != 0) {
        code = CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE;
        goto done;
    }
    *out_report = active;
    out_report->state = CBM_ZOVA_MIGRATION_ROLLED_BACK;
    out_report->source = *cbm_zova_legacy_snapshot_manifest(snapshot);
    out_report->code = CBM_ZOVA_MIGRATION_OK;
    code = CBM_ZOVA_MIGRATION_OK;
done:
    if (db) (void)zova_database_close(db);
    cbm_zova_legacy_snapshot_close(snapshot);
    cbm_zova_writer_guard_release(&guard);
    return code;
}

cbm_zova_migration_code_t cbm_zova_migration_status(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report) {
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!request || !out_report || !request->target_zova_path || !request->canonical_root)
        return CBM_ZOVA_MIGRATION_INVALID;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_workspace_lookup_at(request->target_zova_path, request->canonical_root,
                                     workspace_id, sizeof(workspace_id)) != 0)
        return CBM_ZOVA_MIGRATION_SOURCE_NOT_READY;
    zova_database *db = NULL;
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = request->target_zova_path,
            .flags = ZOVA_OPEN_READ_ONLY,
            .busy_timeout_ms = 5000,
            .out_db = &db,
            .out_error_message = &error}) != ZOVA_OK) {
        zova_message_free(&error);
        return CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
    }
    zova_message_free(&error);
    int rc = migration_status_db(db, workspace_id, out_report);
    (void)zova_database_close(db);
    out_report->code = rc == 0 ? CBM_ZOVA_MIGRATION_OK : CBM_ZOVA_MIGRATION_SOURCE_NOT_READY;
    return out_report->code;
}

cbm_zova_migration_code_t cbm_zova_migration_run(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report) {
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!request || !out_report || !request->source_db_path || !request->source_zova_path ||
        !request->target_zova_path || !request->canonical_root)
        return CBM_ZOVA_MIGRATION_INVALID;
    cbm_zova_writer_guard_t guard = {0};
    if (cbm_zova_writer_guard_acquire(request->target_zova_path, &guard) != 0)
        return CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
    cbm_zova_migration_code_t code = CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
    cbm_zova_legacy_snapshot_t *snapshot = NULL;
    zova_database *db = NULL;
    if (cbm_zova_user_database_init(request->target_zova_path) != 0) goto done;
    code = cbm_zova_legacy_snapshot_open(request->source_db_path, request->source_zova_path,
                                         request->canonical_root, &snapshot);
    if (code != CBM_ZOVA_MIGRATION_OK) goto done;
    const char *workspace_id = cbm_zova_legacy_snapshot_workspace_id(snapshot);
    if (zova_database_open(&(zova_database_open_request){
            .path = request->target_zova_path, .out_db = &db}) != ZOVA_OK) {
        code = CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
        goto done;
    }
    cbm_zova_migration_report_t existing = {0};
    int existing_status = migration_status_db(db, workspace_id, &existing);
    if (existing_status == 0 && existing.state == CBM_ZOVA_MIGRATION_ROLLED_BACK &&
        existing.source_generation == cbm_zova_legacy_snapshot_source_generation(snapshot) &&
        migration_manifest_digests_equal(&existing.source,
                                         cbm_zova_legacy_snapshot_manifest(snapshot))) {
        cbm_zova_migration_manifest_t target = {0};
        if (migration_generation_ready(db, workspace_id, existing.target_generation) != 0 ||
            cbm_zova_migration_manifest_target_tx(db, workspace_id, existing.target_generation,
                                                  snapshot, &target) != CBM_ZOVA_MIGRATION_OK ||
            migration_transition_only(db, workspace_id, "rolled_back", "active") != 0) {
            code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
            goto done;
        }
        *out_report = existing;
        out_report->state = CBM_ZOVA_MIGRATION_ACTIVE;
        out_report->source = *cbm_zova_legacy_snapshot_manifest(snapshot);
        out_report->target = target;
        out_report->code = CBM_ZOVA_MIGRATION_OK;
        code = CBM_ZOVA_MIGRATION_OK;
        goto done;
    }
    if (existing.state == CBM_ZOVA_MIGRATION_ACTIVE &&
        existing.source_generation == cbm_zova_legacy_snapshot_source_generation(snapshot) &&
        migration_manifest_digests_equal(&existing.source,
                                         cbm_zova_legacy_snapshot_manifest(snapshot))) {
        cbm_zova_migration_manifest_t target = {0};
        if (existing_status == 0 &&
            migration_generation_ready(db, workspace_id, existing.target_generation) == 0 &&
            cbm_zova_migration_manifest_target_tx(db, workspace_id, existing.target_generation,
                                                  snapshot, &target) == CBM_ZOVA_MIGRATION_OK) {
            *out_report = existing;
            out_report->source = *cbm_zova_legacy_snapshot_manifest(snapshot);
            out_report->target = target;
            out_report->no_op = true;
            out_report->code = CBM_ZOVA_MIGRATION_NOOP;
            code = CBM_ZOVA_MIGRATION_NOOP;
            goto done;
        }
        *out_report = existing;
        out_report->code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto done;
    }
    if (migration_write_prepared(db, request, snapshot) != 0) {
        code = CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
        goto done;
    }
    out_report->state = CBM_ZOVA_MIGRATION_PREPARED;
    if (cbm_zova_migration_test_fault("migration_after_prepare") ||
        cbm_zova_migration_test_fault("migration_after_source_snapshot")) {
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto fill;
    }
    if (migration_exec(db, "BEGIN IMMEDIATE") != 0 ||
        migration_set_state_tx(db, workspace_id, "copying", 0, 0) != 0) {
        (void)migration_exec(db, "ROLLBACK");
        code = CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
        goto done;
    }
    cbm_zova_workspace_generation_result_t published = {0};
    if (cbm_zova_user_database_publish_workspace_tx(
            db, cbm_zova_legacy_snapshot_input(snapshot), &published) != 0) {
        (void)migration_exec(db, "ROLLBACK");
        (void)migration_record_failed(db, workspace_id);
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto fill;
    }
    if (cbm_zova_migration_test_fault("migration_after_target_publish")) {
        (void)migration_exec(db, "ROLLBACK");
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto fill;
    }
    cbm_zova_migration_manifest_t target_manifest = {0};
    if (cbm_zova_migration_manifest_target_tx(db, workspace_id, published.generation, snapshot,
                                              &target_manifest) != CBM_ZOVA_MIGRATION_OK) {
        (void)migration_exec(db, "ROLLBACK");
        if (!migration_manifest_phase_fault_configured())
            (void)migration_record_failed(db, workspace_id);
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto fill;
    }
    if (cbm_zova_migration_test_fault("migration_before_activate")) {
        (void)migration_exec(db, "ROLLBACK");
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto fill;
    }
    if (migration_set_state_tx(db, workspace_id, "active", published.generation, 1) != 0 ||
        migration_exec(db, "COMMIT") != 0) {
        (void)migration_exec(db, "ROLLBACK");
        code = CBM_ZOVA_MIGRATION_VERIFY_FAILED;
        goto fill;
    }
    out_report->target = target_manifest;
    out_report->target_generation = published.generation;
    out_report->state = CBM_ZOVA_MIGRATION_ACTIVE;
    code = cbm_zova_migration_test_fault("migration_after_activate_commit")
               ? CBM_ZOVA_MIGRATION_VERIFY_FAILED
               : CBM_ZOVA_MIGRATION_OK;
fill:
    snprintf(out_report->workspace_id, sizeof(out_report->workspace_id), "%s", workspace_id);
    out_report->source_generation = cbm_zova_legacy_snapshot_source_generation(snapshot);
    out_report->source = *cbm_zova_legacy_snapshot_manifest(snapshot);
    out_report->code = code;
done:
    if (db) (void)zova_database_close(db);
    cbm_zova_legacy_snapshot_close(snapshot);
    cbm_zova_writer_guard_release(&guard);
    return code;
}

#else
cbm_zova_migration_code_t cbm_zova_migration_status(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report) {
    (void)request;
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    return CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE;
}
cbm_zova_migration_code_t cbm_zova_migration_run(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report) {
    return cbm_zova_migration_status(request, out_report);
}
cbm_zova_migration_code_t cbm_zova_migration_rollback(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report) {
    return cbm_zova_migration_status(request, out_report);
}
cbm_zova_migration_code_t cbm_zova_migration_cleanup(
    const cbm_zova_migration_request_t *request, const char *confirmed_workspace_id,
    cbm_zova_migration_report_t *out_report) {
    (void)confirmed_workspace_id;
    return cbm_zova_migration_status(request, out_report);
}
#endif

void cbm_zova_migration_digest_text(cbm_sha256_ctx *hash, const char *text) {
    const char *value = text ? text : "";
    cbm_sha256_update(hash, value, strlen(value));
    cbm_sha256_update(hash, "\0", 1);
}

void cbm_zova_migration_digest_finalize(cbm_sha256_ctx *hash,
                                        char out[CBM_ZOVA_DIGEST_HEX_SIZE]) {
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char digits[] = "0123456789abcdef";
    cbm_sha256_final(hash, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[64] = '\0';
}

bool cbm_zova_migration_transition_allowed(cbm_zova_migration_state_t from,
                                           cbm_zova_migration_state_t to) {
    if (from < CBM_ZOVA_MIGRATION_PREPARED || from > CBM_ZOVA_MIGRATION_RETIRED ||
        to < CBM_ZOVA_MIGRATION_PREPARED || to > CBM_ZOVA_MIGRATION_RETIRED) {
        return false;
    }
    switch (from) {
        case CBM_ZOVA_MIGRATION_PREPARED:
            return to == CBM_ZOVA_MIGRATION_COPYING || to == CBM_ZOVA_MIGRATION_FAILED;
        case CBM_ZOVA_MIGRATION_COPYING:
            return to == CBM_ZOVA_MIGRATION_ACTIVE || to == CBM_ZOVA_MIGRATION_FAILED;
        case CBM_ZOVA_MIGRATION_ACTIVE:
            return to == CBM_ZOVA_MIGRATION_ROLLED_BACK ||
                   to == CBM_ZOVA_MIGRATION_CLEANUP_PENDING;
        case CBM_ZOVA_MIGRATION_FAILED:
            return to == CBM_ZOVA_MIGRATION_PREPARED || to == CBM_ZOVA_MIGRATION_COPYING;
        case CBM_ZOVA_MIGRATION_ROLLED_BACK:
            return to == CBM_ZOVA_MIGRATION_ACTIVE;
        case CBM_ZOVA_MIGRATION_CLEANUP_PENDING:
            return to == CBM_ZOVA_MIGRATION_RETIRED;
        case CBM_ZOVA_MIGRATION_RETIRED:
            return false;
    }
    return false;
}

const char *cbm_zova_migration_state_name(cbm_zova_migration_state_t state) {
    static const char *const names[] = {
        "prepared", "copying", "active", "failed", "rolled_back", "cleanup_pending", "retired",
    };
    if (state < CBM_ZOVA_MIGRATION_PREPARED || state > CBM_ZOVA_MIGRATION_RETIRED) {
        return NULL;
    }
    return names[state];
}
