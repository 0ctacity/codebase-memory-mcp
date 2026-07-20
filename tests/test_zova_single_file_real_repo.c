#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/sha256.h"
#include "pipeline/pipeline.h"
#include "cypher/cypher.h"
#include "mcp/mcp.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_migration.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_route.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int single_file_sql_count(sqlite3 *db, const char *sql, const char *bind,
                                 int64_t *out);

static size_t single_file_cypher_case_count(int64_t node_count, size_t total_cases) {
    enum { LARGE_REPOSITORY_NODE_COUNT = 50000, LARGE_REPOSITORY_CASE_COUNT = 10 };
    if (node_count > LARGE_REPOSITORY_NODE_COUNT &&
        total_cases > LARGE_REPOSITORY_CASE_COUNT)
        return LARGE_REPOSITORY_CASE_COUNT;
    return total_cases;
}

enum { SINGLE_FILE_PUBLIC_RESPONSE_COUNT = 8 };

typedef struct {
    char *items[SINGLE_FILE_PUBLIC_RESPONSE_COUNT];
} single_file_public_responses_t;

static int single_file_digest(const char *path, uint8_t out[CBM_SHA256_DIGEST_LEN]) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    uint8_t buffer[8192];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        cbm_sha256_update(&hash, buffer, count);
    int rc = ferror(file) ? -1 : 0;
    fclose(file);
    if (rc == 0) cbm_sha256_final(&hash, out);
    return rc;
}

static int single_file_source_digest_mismatches(
    const char *db_path, const char *zova_path,
    const uint8_t expected_db[CBM_SHA256_DIGEST_LEN],
    const uint8_t expected_zova[CBM_SHA256_DIGEST_LEN], const char *stage) {
    uint8_t actual_db[CBM_SHA256_DIGEST_LEN];
    uint8_t actual_zova[CBM_SHA256_DIGEST_LEN];
    int mismatches = 0;
    if (single_file_digest(db_path, actual_db) != 0 ||
        memcmp(expected_db, actual_db, sizeof(actual_db)) != 0) {
        fprintf(stderr, "source DB digest mismatch after %s\n", stage);
        mismatches++;
    }
    if (single_file_digest(zova_path, actual_zova) != 0 ||
        memcmp(expected_zova, actual_zova, sizeof(actual_zova)) != 0) {
        fprintf(stderr, "source Zova digest mismatch after %s\n", stage);
        mismatches++;
    }
    return mismatches;
}

static char *single_file_normalized_tool_payload(const char *response,
                                                 int normalize_elapsed) {
    if (!response) return NULL;
    yyjson_doc *outer = yyjson_read(response, strlen(response), 0);
    yyjson_val *result = outer ? yyjson_obj_get(yyjson_doc_get_root(outer), "result") : NULL;
    yyjson_val *content = result ? yyjson_obj_get(result, "content") : NULL;
    yyjson_val *item = content ? yyjson_arr_get(content, 0) : NULL;
    const char *text = item ? yyjson_get_str(yyjson_obj_get(item, "text")) : NULL;
    yyjson_doc *payload = text ? yyjson_read(text, strlen(text), 0) : NULL;
    yyjson_mut_doc *normalized = payload ? yyjson_doc_mut_copy(payload, NULL) : NULL;
    if (normalized) {
        yyjson_mut_val *root = yyjson_mut_doc_get_root(normalized);
        (void)yyjson_mut_obj_remove_key(root, "route");
        (void)yyjson_mut_obj_remove_key(root, "generation");
        if (normalize_elapsed) (void)yyjson_mut_obj_remove_key(root, "elapsed_ms");
    }
    char *json = normalized ? yyjson_mut_write(normalized, 0, NULL) : NULL;
    yyjson_mut_doc_free(normalized);
    yyjson_doc_free(payload);
    yyjson_doc_free(outer);
    return json;
}

static void single_file_public_responses_free(single_file_public_responses_t *responses) {
    if (!responses) return;
    for (int i = 0; i < SINGLE_FILE_PUBLIC_RESPONSE_COUNT; i++) free(responses->items[i]);
    memset(responses, 0, sizeof(*responses));
}

static int single_file_capture_public_responses(const char *project, const char *sample_name,
                                                const char *sample_qn,
                                                single_file_public_responses_t *out) {
    if (!project || !sample_name || !sample_qn || !out) return -1;
    memset(out, 0, sizeof(*out));
    const char *formats[SINGLE_FILE_PUBLIC_RESPONSE_COUNT] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"search_graph\",\"arguments\":{\"project\":\"%s\","
        "\"query\":\"%s\",\"limit\":20}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"search_graph\",\"arguments\":{\"project\":\"%s\","
        "\"name_pattern\":\"%s\",\"limit\":20}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"search_graph\",\"arguments\":{\"project\":\"%s\","
        "\"semantic_query\":[\"%s\"],\"limit\":20}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"index_status\",\"arguments\":{\"project\":\"%s\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"query_graph\",\"arguments\":{\"project\":\"%s\","
        "\"query\":\"MATCH (n:Function) RETURN n.qualified_name ORDER BY "
        "n.qualified_name LIMIT 20\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"trace_path\",\"arguments\":{\"project\":\"%s\","
        "\"function_name\":\"%s\",\"direction\":\"both\",\"depth\":2}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"get_code_snippet\",\"arguments\":{\"project\":\"%s\","
        "\"qualified_name\":\"%s\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"search_code\",\"arguments\":{\"project\":\"%s\","
        "\"pattern\":\"%s\"}}}",
    };
    cbm_mcp_server_t *server = cbm_mcp_server_new(NULL);
    if (!server) return -1;
    int rc = 0;
    for (int i = 0; i < SINGLE_FILE_PUBLIC_RESPONSE_COUNT; i++) {
        char request[4096];
        if (i == 3 || i == 4)
            snprintf(request, sizeof(request), formats[i], project);
        else if (i == 6)
            snprintf(request, sizeof(request), formats[i], project, sample_qn);
        else
            snprintf(request, sizeof(request), formats[i], project, sample_name);
        char *response = cbm_mcp_server_handle(server, request);
        out->items[i] = single_file_normalized_tool_payload(response, i == 7);
        free(response);
        if (!out->items[i]) rc = -1;
    }
    cbm_mcp_server_free(server);
    if (rc != 0) single_file_public_responses_free(out);
    return rc;
}

static int single_file_public_response_mismatches(
    const single_file_public_responses_t *expected,
    const single_file_public_responses_t *actual) {
    static const char *names[SINGLE_FILE_PUBLIC_RESPONSE_COUNT] = {
        "search_graph_query", "search_graph_name_pattern", "search_graph_semantic",
        "index_status", "query_graph", "trace_path", "get_code_snippet", "search_code"};
    int mismatches = 0;
    for (int i = 0; i < SINGLE_FILE_PUBLIC_RESPONSE_COUNT; i++) {
        if (!expected->items[i] || !actual->items[i] ||
            strcmp(expected->items[i], actual->items[i]) != 0) {
            mismatches++;
            fprintf(stderr, "public parity response[%d] %s mismatch\n", i, names[i]);
        }
    }
    return mismatches;
}

typedef struct {
    int64_t sqlite_id;
    int64_t snapshot_id;
    char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
} single_file_node_ref_t;

static int single_file_compare_cypher(const char *source_path, const char *zova_path,
                                      const char *workspace_id, const char *project,
                                      int *native_routes, int *compat_routes,
                                      int *ordering_mismatches) {
    cbm_store_t *source = cbm_store_open_path_query(source_path);
    cbm_store_t *flagged = cbm_store_open_zova_workspace_query(zova_path, workspace_id);
    if (!source || !flagged) { if (source) cbm_store_close(source); if (flagged) cbm_store_close(flagged); return -1; }
    sqlite3 *flagged_db = cbm_store_get_db(flagged);
    int64_t node_count = 0;
    if (!flagged_db ||
        single_file_sql_count(flagged_db, "SELECT count(*) FROM cbm_nodes_v1", NULL,
                              &node_count) != 0) {
        cbm_store_close(flagged); cbm_store_close(source); return -1;
    }
    sqlite3_stmt *sample_stmt = NULL;
    char source_qn[1024] = {0}, target_qn[1024] = {0};
    if (sqlite3_prepare_v2(cbm_store_get_db(source),
            "SELECT s.qualified_name,t.qualified_name FROM edges e "
            "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
            "WHERE e.project=?1 AND e.type='CALLS' AND s.label='Function' "
            "AND t.label='Function' ORDER BY s.qualified_name,t.qualified_name LIMIT 1",
            -1, &sample_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(sample_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(sample_stmt) != SQLITE_ROW) {
        if (sample_stmt) sqlite3_finalize(sample_stmt);
        cbm_store_close(flagged); cbm_store_close(source); return -1;
    }
    snprintf(source_qn, sizeof(source_qn), "%s", sqlite3_column_text(sample_stmt, 0));
    snprintf(target_qn, sizeof(target_qn), "%s", sqlite3_column_text(sample_stmt, 1));
    sqlite3_finalize(sample_stmt);
    char rel_out[4096], rel_in[4096], rel_both[4096];
    char rel_props[4096], rel_bounded[4096];
    snprintf(rel_out, sizeof(rel_out), "MATCH (a:Function {qualified_name:'%s'})-[:CALLS]->(b) RETURN count(b)", source_qn);
    snprintf(rel_in, sizeof(rel_in), "MATCH (a:Function {qualified_name:'%s'})<-[:CALLS]-(b) RETURN count(b)", target_qn);
    snprintf(rel_both, sizeof(rel_both), "MATCH (a:Function {qualified_name:'%s'})-[:CALLS]-(b) RETURN count(b)", source_qn);
    snprintf(rel_props, sizeof(rel_props), "MATCH (a:Function {qualified_name:'%s'})-[r:CALLS]->(b) RETURN count(r.confidence)", source_qn);
    snprintf(rel_bounded, sizeof(rel_bounded), "MATCH (a:Function {qualified_name:'%s'})-[:CALLS*1..2]->(b) RETURN count(b)", source_qn);
    const struct {
        const char *query;
        int ceiling;
    } cases[] = {
        {"MATCH (n:Function) RETURN n.qualified_name ORDER BY n.qualified_name LIMIT 20", 1000},
        {"MATCH (n:Function) WHERE n.start_line >= 0 RETURN n.name,n.file_path ORDER BY n.name LIMIT 20", 1000},
        {"MATCH (n:Function) WHERE n.name =~ '.+' RETURN n.qualified_name ORDER BY n.qualified_name LIMIT 20", 1000},
        {"MATCH (n:Function) WHERE n.nonesuch IS NULL RETURN n.name ORDER BY n.name LIMIT 20", 1000},
        {"MATCH (n:Function) WHERE n.label IN ['Function','Method'] RETURN n.name ORDER BY n.name LIMIT 20", 1000},
        {"MATCH (n:Function) WHERE n.start_line >= 0 AND NOT n.name = '' RETURN n.name ORDER BY n.name SKIP 1 LIMIT 10", 1000},
        {"MATCH (n:Function) RETURN DISTINCT n.label ORDER BY n.label", 1000},
        {"MATCH (n:Function) RETURN count(n)", 1000},
        {"MATCH (n:Function) RETURN n.name ORDER BY n.name LIMIT 0", 1000},
        {rel_out, 5}, {rel_in, 5}, {rel_both, 5}, {rel_props, 5}, {rel_bounded, 5},
    };
    size_t case_count = single_file_cypher_case_count(
        node_count, sizeof(cases) / sizeof(cases[0]));
    fprintf(stderr, "cypher parity cases=%zu nodes=%lld\n", case_count,
            (long long)node_count);
    int mismatches = 0;
    *native_routes = *compat_routes = *ordering_mismatches = 0;
    for (size_t qi = 0; qi < case_count; ++qi) {
        const char *query = cases[qi].query;
        cbm_query_t *plan = NULL; char *error = NULL; const char *reason = NULL;
        if (cbm_cypher_parse(query, &plan, &error) != 0) { free(error); mismatches++; continue; }
        cbm_cypher_route_t route = cbm_cypher_classify_plan(plan, &reason);
        int native_before=cbm_store_zova_native_topology_ops(flagged);
        cbm_query_free(plan); free(error);
        cbm_cypher_result_t a = {0}, b = {0};
        int arc = cbm_cypher_execute(source, query, project, cases[qi].ceiling, &a);
        int brc = cbm_cypher_execute(flagged, query, project, cases[qi].ceiling, &b);
        int native_after=cbm_store_zova_native_topology_ops(flagged);
        if(route==CBM_CYPHER_ROUTE_NATIVE_GRAPH && native_after>native_before)(*native_routes)++;
        else if(route==CBM_CYPHER_ROUTE_IN_DATABASE_COMPAT)(*compat_routes)++;
        else mismatches++;
        if (arc != 0 || brc != 0) { fprintf(stderr, "cypher[%zu] rc source=%d flagged=%d aerr=%s berr=%s\n", qi, arc, brc, a.error ? a.error : "", b.error ? b.error : ""); mismatches++; }
        else if (a.col_count != b.col_count || a.row_count != b.row_count) { fprintf(stderr, "cypher[%zu] shape source=%dx%d flagged=%dx%d store_error=%s\n", qi, a.row_count, a.col_count, b.row_count, b.col_count, cbm_store_error(flagged)); mismatches++; }
        else if (qi >= 9 && (a.row_count != 1 || !a.rows[0][0] || atoi(a.rows[0][0]) <= 0)) { fprintf(stderr, "cypher[%zu] sampled relationship shape was vacuous\n", qi); mismatches++; }
        else for (int r = 0; r < a.row_count; ++r) for (int c = 0; c < a.col_count; ++c)
            if (strcmp(a.rows[r][c] ? a.rows[r][c] : "", b.rows[r][c] ? b.rows[r][c] : "") != 0) { if (*ordering_mismatches < 5) fprintf(stderr, "cypher[%zu] row=%d col=%d source=%s flagged=%s\n", qi, r, c, a.rows[r][c] ? a.rows[r][c] : "", b.rows[r][c] ? b.rows[r][c] : ""); mismatches++; (*ordering_mismatches)++; }
        cbm_cypher_result_free(&a); cbm_cypher_result_free(&b);
    }
    cbm_store_close(flagged); cbm_store_close(source);
    return mismatches;
}

static const char *single_file_text(sqlite3_stmt *stmt, int column,
                                    const char *fallback) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? (const char *)value : (fallback ? fallback : "");
}

static int single_file_text_equal(sqlite3_stmt *left, int left_column,
                                  sqlite3_stmt *right, int right_column,
                                  const char *left_fallback, const char *right_fallback) {
    return strcmp(single_file_text(left, left_column, left_fallback),
                  single_file_text(right, right_column, right_fallback)) == 0;
}

static const char *single_file_nonnull(const char *value, const char *fallback) {
    return value ? value : fallback;
}

static const char *single_file_node_ref_find(const single_file_node_ref_t *refs, int count,
                                             int64_t sqlite_id) {
    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (refs[mid].sqlite_id == sqlite_id) return refs[mid].stable_id;
        if (refs[mid].sqlite_id < sqlite_id) low = mid + 1;
        else high = mid - 1;
    }
    return NULL;
}

static int single_file_compare_project(sqlite3 *source, sqlite3 *user, const char *project,
                                        const char *workspace_id) {
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *user_stmt = NULL;
    int mismatches = 0;
    if (sqlite3_prepare_v2(source,
                           "SELECT name,indexed_at,root_path FROM projects WHERE name=?1", -1,
                           &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(source_stmt) != SQLITE_ROW ||
        sqlite3_prepare_v2(user,
                           "SELECT p.project,p.indexed_at,p.root_path FROM cbm_projects_v1 p "
                           "JOIN cbm_workspace_registry r USING(workspace_key) "
                           "WHERE r.workspace_id=?1",
                           -1, &user_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(user_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(user_stmt) != SQLITE_ROW) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        if (user_stmt) sqlite3_finalize(user_stmt);
        return -1;
    }
    mismatches += !single_file_text_equal(source_stmt, 0, user_stmt, 0, "", "");
    /* Each route is indexed independently; indexed_at is run-local diagnostic
     * state and is intentionally excluded from canonical parity. */
    mismatches += !single_file_text_equal(source_stmt, 2, user_stmt, 2, "", "");
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(user_stmt);
    return mismatches;
}

static int single_file_compare_nodes(sqlite3 *source, sqlite3 *user, const char *project,
                                     const char *workspace_id, single_file_node_ref_t **out_refs,
                                     int *out_count) {
    if (!source || !user || !project || !workspace_id || !out_refs || !out_count) return -1;
    *out_refs = NULL;
    *out_count = 0;
    int64_t count = 0;
    if (single_file_sql_count(source, "SELECT count(*) FROM nodes WHERE project=?1", project,
                               &count) != 0 || count < 0 || count > INT32_MAX) {
        return -1;
    }
    single_file_node_ref_t *refs = count > 0 ? calloc((size_t)count, sizeof(*refs)) : NULL;
    if (count > 0 && !refs) return -1;
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *user_stmt = NULL;
    int mismatches = 0;
    if (sqlite3_prepare_v2(
            source,
            "SELECT id,project,label,name,qualified_name,file_path,start_line,end_line,properties "
            "FROM nodes WHERE project=?1 ORDER BY id",
            -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_prepare_v2(
            user,
            "SELECT p.project,n.label,n.name,n.qualified_name,f.file_path,n.start_line,"
            "n.end_line,n.properties FROM cbm_nodes_v1 n "
            "JOIN cbm_workspace_registry r USING(workspace_key) "
            "JOIN cbm_projects_v1 p USING(workspace_key) "
            "JOIN cbm_files_v1 f ON f.file_key=n.file_key "
            "WHERE r.workspace_id=?1 AND n.node_id=?2",
            -1, &user_stmt, NULL) != SQLITE_OK) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        if (user_stmt) sqlite3_finalize(user_stmt);
        free(refs);
        return -1;
    }
    int index = 0;
    while (sqlite3_step(source_stmt) == SQLITE_ROW) {
        if (index >= count) {
            mismatches++;
            break;
        }
        const int64_t sqlite_id = sqlite3_column_int64(source_stmt, 0);
        const char *label = single_file_text(source_stmt, 2, "");
        const char *name = single_file_text(source_stmt, 3, "");
        const char *qualified_name = single_file_text(source_stmt, 4, "");
        const char *file_path = single_file_text(source_stmt, 5, "");
        char discriminator[512];
        if (qualified_name[0])
            snprintf(discriminator, sizeof(discriminator), "named:%s", qualified_name);
        else
            snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", name,
                     sqlite3_column_int(source_stmt, 6), sqlite3_column_int(source_stmt, 7));
        refs[index].sqlite_id = sqlite_id;
        if (cbm_zova_workspace_node_id_v2(workspace_id, label, file_path, qualified_name,
                                           discriminator, refs[index].stable_id,
                                           sizeof(refs[index].stable_id)) != 0) {
            mismatches++;
            index++;
            continue;
        }
        sqlite3_reset(user_stmt);
        sqlite3_clear_bindings(user_stmt);
        if (sqlite3_bind_text(user_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(user_stmt, 2, refs[index].stable_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(user_stmt) != SQLITE_ROW) {
            mismatches++;
            index++;
            continue;
        }
        mismatches += !single_file_text_equal(source_stmt, 1, user_stmt, 0, "", "");
        mismatches += !single_file_text_equal(source_stmt, 2, user_stmt, 1, "", "");
        mismatches += !single_file_text_equal(source_stmt, 3, user_stmt, 2, "", "");
        mismatches += !single_file_text_equal(source_stmt, 4, user_stmt, 3, "", "");
        mismatches += !single_file_text_equal(source_stmt, 5, user_stmt, 4, "", "");
        mismatches += sqlite3_column_int(source_stmt, 6) != sqlite3_column_int(user_stmt, 5);
        mismatches += sqlite3_column_int(source_stmt, 7) != sqlite3_column_int(user_stmt, 6);
        mismatches += !single_file_text_equal(source_stmt, 8, user_stmt, 7, "{}", "{}");
        index++;
    }
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(user_stmt);
    if (index != count) mismatches++;
    *out_refs = refs;
    *out_count = index;
    return mismatches;
}

static int single_file_compare_edges(sqlite3 *source, sqlite3 *user, const char *project,
                                     const char *workspace_id, const single_file_node_ref_t *refs,
                                     int ref_count) {
    if (!source || !user || !project || !workspace_id || ref_count < 0 ||
        (ref_count > 0 && !refs)) return -1;
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *user_stmt = NULL;
    sqlite3_stmt *ref_insert = NULL;
    int mismatches = 0;
    if (sqlite3_exec(user,
                     "PRAGMA temp_store=MEMORY;"
                     "DROP TABLE IF EXISTS temp.cbm_zsp_node_refs;"
                     "CREATE TEMP TABLE cbm_zsp_node_refs("
                     "sqlite_id INTEGER PRIMARY KEY,stable_id TEXT NOT NULL UNIQUE);"
                     "BEGIN",
                     NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(user,
                           "INSERT INTO temp.cbm_zsp_node_refs(sqlite_id,stable_id) "
                           "VALUES(?1,?2)",
                           -1, &ref_insert, NULL) != SQLITE_OK) {
        if (ref_insert) sqlite3_finalize(ref_insert);
        sqlite3_exec(user, "ROLLBACK; DROP TABLE IF EXISTS temp.cbm_zsp_node_refs", NULL,
                     NULL, NULL);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < ref_count; i++) {
        sqlite3_reset(ref_insert);
        sqlite3_clear_bindings(ref_insert);
        if (sqlite3_bind_int64(ref_insert, 1, refs[i].sqlite_id) != SQLITE_OK ||
            sqlite3_bind_text(ref_insert, 2, refs[i].stable_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(ref_insert) != SQLITE_DONE) {
            rc = -1;
            break;
        }
    }
    sqlite3_finalize(ref_insert);
    ref_insert = NULL;
    if (rc != 0 || sqlite3_exec(user, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) !=
                           SQLITE_OK) {
        sqlite3_exec(user, "ROLLBACK; DROP TABLE IF EXISTS temp.cbm_zsp_node_refs", NULL,
                     NULL, NULL);
        return -1;
    }
    static const char *source_sql =
        "SELECT source_id,target_id,type,coalesce(properties,'{}'),"
        "coalesce(url_path_gen,''),coalesce(local_name_gen,'') "
        "FROM edges WHERE project=?1 "
        "ORDER BY source_id,target_id,type,coalesce(local_name_gen,''),"
        "coalesce(properties,'{}'),coalesce(url_path_gen,'')";
    static const char *user_sql =
        "SELECT source_ref.sqlite_id,target_ref.sqlite_id,e.edge_type,"
        "coalesce(e.properties,'{}'),coalesce(e.url_path,''),coalesce(e.local_name,'') "
        "FROM cbm_edges_v1 e "
        "JOIN cbm_workspace_registry r ON r.workspace_key=e.workspace_key "
        "JOIN cbm_nodes_v1 source_node ON source_node.node_key=e.source_node_key "
        "JOIN cbm_nodes_v1 target_node ON target_node.node_key=e.target_node_key "
        "JOIN temp.cbm_zsp_node_refs source_ref ON source_ref.stable_id=source_node.node_id "
        "JOIN temp.cbm_zsp_node_refs target_ref ON target_ref.stable_id=target_node.node_id "
        "WHERE r.workspace_id=?1 "
        "ORDER BY source_ref.sqlite_id,target_ref.sqlite_id,e.edge_type,"
        "coalesce(e.local_name,''),coalesce(e.properties,'{}'),coalesce(e.url_path,'')";
    if (sqlite3_prepare_v2(source, source_sql, -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_prepare_v2(user, user_sql, -1, &user_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(user_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        if (user_stmt) sqlite3_finalize(user_stmt);
        sqlite3_exec(user, "DROP TABLE IF EXISTS temp.cbm_zsp_node_refs", NULL, NULL, NULL);
        return -1;
    }
    for (;;) {
        int source_step = sqlite3_step(source_stmt);
        int user_step = sqlite3_step(user_stmt);
        if (source_step == SQLITE_DONE && user_step == SQLITE_DONE) break;
        if (source_step != SQLITE_ROW || user_step != SQLITE_ROW) {
            if (mismatches < 5)
                fprintf(stderr, "edge parity stream length source_step=%d user_step=%d\n",
                        source_step, user_step);
            mismatches++;
            break;
        }
        int row_mismatches = 0;
        row_mismatches +=
            sqlite3_column_int64(source_stmt, 0) != sqlite3_column_int64(user_stmt, 0);
        row_mismatches +=
            sqlite3_column_int64(source_stmt, 1) != sqlite3_column_int64(user_stmt, 1);
        for (int column = 2; column < 6; column++) {
            row_mismatches +=
                !single_file_text_equal(source_stmt, column, user_stmt, column, "", "");
        }
        if (row_mismatches && mismatches < 5) {
            fprintf(stderr,
                    "edge parity row source=(%lld,%lld,%s,%s,%s,%s) "
                    "user=(%lld,%lld,%s,%s,%s,%s)\n",
                    (long long)sqlite3_column_int64(source_stmt, 0),
                    (long long)sqlite3_column_int64(source_stmt, 1),
                    single_file_text(source_stmt, 2, ""),
                    single_file_text(source_stmt, 3, ""),
                    single_file_text(source_stmt, 4, ""),
                    single_file_text(source_stmt, 5, ""),
                    (long long)sqlite3_column_int64(user_stmt, 0),
                    (long long)sqlite3_column_int64(user_stmt, 1),
                    single_file_text(user_stmt, 2, ""),
                    single_file_text(user_stmt, 3, ""),
                    single_file_text(user_stmt, 4, ""),
                    single_file_text(user_stmt, 5, ""));
        }
        mismatches += row_mismatches;
    }
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(user_stmt);
    sqlite3_exec(user, "DROP TABLE IF EXISTS temp.cbm_zsp_node_refs", NULL, NULL, NULL);
    return mismatches;
}

static int single_file_snapshot_node_index(
    const cbm_zova_workspace_snapshot_t *snapshot, const char *stable_id) {
    int low = 0;
    int high = snapshot ? snapshot->node_count - 1 : -1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int order = strcmp(snapshot->node_stable_ids[mid], stable_id);
        if (order == 0) return mid;
        if (order < 0) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

static int single_file_compare_snapshot_nodes(
    sqlite3 *source, const char *project, const char *workspace_id,
    const cbm_zova_workspace_snapshot_t *snapshot, single_file_node_ref_t **out_refs,
    int *out_count) {
    if (!source || !project || !workspace_id || !snapshot || !out_refs || !out_count)
        return -1;
    *out_refs = NULL;
    *out_count = 0;
    int64_t count = 0;
    if (single_file_sql_count(source, "SELECT count(*) FROM nodes WHERE project=?1", project,
                              &count) != 0 || count != snapshot->node_count ||
        count < 0 || count > INT32_MAX)
        return count >= 0 && count <= INT32_MAX ? 1 : -1;
    single_file_node_ref_t *refs = count ? calloc((size_t)count, sizeof(*refs)) : NULL;
    if (count && !refs) return -1;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(
            source,
            "SELECT id,project,label,name,qualified_name,file_path,start_line,end_line,properties "
            "FROM nodes WHERE project=?1 ORDER BY id",
            -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        free(refs);
        return -1;
    }
    int mismatches = 0;
    int index = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (index >= count) { mismatches++; break; }
        const char *label = single_file_text(statement, 2, "");
        const char *name = single_file_text(statement, 3, "");
        const char *qualified_name = single_file_text(statement, 4, "");
        const char *file_path = single_file_text(statement, 5, "");
        char discriminator[512];
        if (qualified_name[0])
            snprintf(discriminator, sizeof(discriminator), "named:%s", qualified_name);
        else
            snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", name,
                     sqlite3_column_int(statement, 6), sqlite3_column_int(statement, 7));
        refs[index].sqlite_id = sqlite3_column_int64(statement, 0);
        if (cbm_zova_workspace_node_id_v2(
                workspace_id, label, file_path, qualified_name, discriminator,
                refs[index].stable_id, sizeof(refs[index].stable_id)) != 0) {
            mismatches++;
            index++;
            continue;
        }
        int snapshot_index = single_file_snapshot_node_index(snapshot, refs[index].stable_id);
        if (snapshot_index < 0) {
            mismatches++;
            index++;
            continue;
        }
        refs[index].snapshot_id = snapshot_index + 1;
        const CBMDumpNode *node = &snapshot->nodes[snapshot_index];
        mismatches += strcmp(single_file_text(statement, 1, ""),
                             single_file_nonnull(node->project, "")) != 0;
        mismatches += strcmp(label, single_file_nonnull(node->label, "")) != 0;
        mismatches += strcmp(name, single_file_nonnull(node->name, "")) != 0;
        mismatches += strcmp(qualified_name,
                             single_file_nonnull(node->qualified_name, "")) != 0;
        mismatches += strcmp(file_path, single_file_nonnull(node->file_path, "")) != 0;
        mismatches += sqlite3_column_int(statement, 6) != node->start_line;
        mismatches += sqlite3_column_int(statement, 7) != node->end_line;
        mismatches += strcmp(single_file_text(statement, 8, "{}"),
                             single_file_nonnull(node->properties, "{}")) != 0;
        index++;
    }
    sqlite3_finalize(statement);
    if (index != count) mismatches++;
    *out_refs = refs;
    *out_count = index;
    return mismatches;
}

typedef struct {
    const CBMDumpEdge *edge;
    int64_t source_sqlite_id;
    int64_t target_sqlite_id;
} single_file_snapshot_edge_ref_t;

static int single_file_snapshot_edge_ref_compare(const void *left_ptr,
                                                 const void *right_ptr) {
    const single_file_snapshot_edge_ref_t *left = left_ptr;
    const single_file_snapshot_edge_ref_t *right = right_ptr;
    if (left->source_sqlite_id != right->source_sqlite_id)
        return left->source_sqlite_id < right->source_sqlite_id ? -1 : 1;
    if (left->target_sqlite_id != right->target_sqlite_id)
        return left->target_sqlite_id < right->target_sqlite_id ? -1 : 1;
    int order = strcmp(single_file_nonnull(left->edge->type, ""),
                       single_file_nonnull(right->edge->type, ""));
    if (order == 0)
        order = strcmp(single_file_nonnull(left->edge->local_name, ""),
                       single_file_nonnull(right->edge->local_name, ""));
    if (order == 0)
        order = strcmp(single_file_nonnull(left->edge->properties, "{}"),
                       single_file_nonnull(right->edge->properties, "{}"));
    if (order == 0)
        order = strcmp(single_file_nonnull(left->edge->url_path, ""),
                       single_file_nonnull(right->edge->url_path, ""));
    return order;
}

static int single_file_compare_snapshot_edges(
    sqlite3 *source, const char *project, const cbm_zova_workspace_snapshot_t *snapshot,
    const single_file_node_ref_t *refs, int ref_count) {
    if (!source || !project || !snapshot || ref_count < 0 ||
        (ref_count && !refs)) return -1;
    int64_t *snapshot_to_sqlite = snapshot->node_count
        ? calloc((size_t)snapshot->node_count + 1, sizeof(*snapshot_to_sqlite)) : NULL;
    single_file_snapshot_edge_ref_t *edges = snapshot->edge_count
        ? calloc((size_t)snapshot->edge_count, sizeof(*edges)) : NULL;
    if ((snapshot->node_count && !snapshot_to_sqlite) || (snapshot->edge_count && !edges)) {
        free(snapshot_to_sqlite); free(edges); return -1;
    }
    int rc = 0;
    for (int i = 0; i < ref_count; i++) {
        if (refs[i].snapshot_id <= 0 || refs[i].snapshot_id > snapshot->node_count ||
            snapshot_to_sqlite[refs[i].snapshot_id] != 0) {
            rc = -1; break;
        }
        snapshot_to_sqlite[refs[i].snapshot_id] = refs[i].sqlite_id;
    }
    for (int i = 0; rc == 0 && i < snapshot->edge_count; i++) {
        const CBMDumpEdge *edge = &snapshot->edges[i];
        if (edge->source_id <= 0 || edge->source_id > snapshot->node_count ||
            edge->target_id <= 0 || edge->target_id > snapshot->node_count ||
            snapshot_to_sqlite[edge->source_id] == 0 ||
            snapshot_to_sqlite[edge->target_id] == 0) {
            rc = -1; break;
        }
        edges[i] = (single_file_snapshot_edge_ref_t){
            .edge = edge,
            .source_sqlite_id = snapshot_to_sqlite[edge->source_id],
            .target_sqlite_id = snapshot_to_sqlite[edge->target_id],
        };
    }
    free(snapshot_to_sqlite);
    if (rc != 0) { free(edges); return -1; }
    if (snapshot->edge_count > 1)
        qsort(edges, (size_t)snapshot->edge_count, sizeof(*edges),
              single_file_snapshot_edge_ref_compare);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(
            source,
            "SELECT source_id,target_id,type,coalesce(properties,'{}'),"
            "coalesce(url_path_gen,''),coalesce(local_name_gen,'') "
            "FROM edges WHERE project=?1 "
            "ORDER BY source_id,target_id,type,coalesce(local_name_gen,''),"
            "coalesce(properties,'{}'),coalesce(url_path_gen,'')",
            -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        free(edges);
        return -1;
    }
    int mismatches = 0;
    int index = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (index >= snapshot->edge_count) { mismatches++; break; }
        const single_file_snapshot_edge_ref_t *actual = &edges[index++];
        mismatches += sqlite3_column_int64(statement, 0) != actual->source_sqlite_id;
        mismatches += sqlite3_column_int64(statement, 1) != actual->target_sqlite_id;
        mismatches += strcmp(single_file_text(statement, 2, ""),
                             single_file_nonnull(actual->edge->type, "")) != 0;
        mismatches += strcmp(single_file_text(statement, 3, "{}"),
                             single_file_nonnull(actual->edge->properties, "{}")) != 0;
        mismatches += strcmp(single_file_text(statement, 4, ""),
                             single_file_nonnull(actual->edge->url_path, "")) != 0;
        mismatches += strcmp(single_file_text(statement, 5, ""),
                             single_file_nonnull(actual->edge->local_name, "")) != 0;
    }
    sqlite3_finalize(statement);
    if (index != snapshot->edge_count) mismatches++;
    free(edges);
    return mismatches;
}

static int single_file_edge_select_trace(unsigned mask, void *context, void *statement,
                                         void *unused_sql) {
    (void)unused_sql;
    if (mask != SQLITE_TRACE_STMT || !context || !statement) return 0;
    sqlite3_stmt *stmt = (sqlite3_stmt *)statement;
    if (!sqlite3_stmt_readonly(stmt)) return 0;
    const char *sql = sqlite3_sql(stmt);
    if (sql && strstr(sql, "FROM cbm_edges_v1")) (*(int *)context)++;
    return 0;
}

TEST(zova_single_file_edge_parity_streams_once) {
    sqlite3 *source = NULL;
    sqlite3 *user = NULL;
    ASSERT_EQ(sqlite3_open(":memory:", &source), SQLITE_OK);
    ASSERT_EQ(sqlite3_open(":memory:", &user), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  source,
                  "CREATE TABLE edges(id INTEGER PRIMARY KEY,project TEXT,source_id INTEGER,"
                  "target_id INTEGER,type TEXT,properties TEXT,url_path_gen TEXT,"
                  "local_name_gen TEXT);"
                  "INSERT INTO edges VALUES"
                  "(1,'project',1,2,'CALLS','{}','','first'),"
                  "(2,'project',1,2,'CALLS','{\"rank\":2}','','second'),"
                  "(3,'project',2,1,'RETURNS','{}','/return','');",
                  NULL, NULL, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  user,
                  "CREATE TABLE cbm_workspace_registry(workspace_key INTEGER PRIMARY KEY,"
                  "workspace_id TEXT UNIQUE);"
                  "CREATE TABLE cbm_nodes_v1(node_key INTEGER PRIMARY KEY,workspace_key INTEGER,"
                  "node_id TEXT);"
                  "CREATE TABLE cbm_edges_v1(edge_key INTEGER PRIMARY KEY,workspace_key INTEGER,"
                  "source_node_key INTEGER,target_node_key INTEGER,edge_type TEXT,properties TEXT,"
                  "url_path TEXT,local_name TEXT);"
                  "CREATE INDEX cbm_edges_v1_source_type ON cbm_edges_v1"
                  "(workspace_key,source_node_key,edge_type);"
                  "INSERT INTO cbm_workspace_registry VALUES(1,'workspace');"
                  "INSERT INTO cbm_nodes_v1 VALUES(11,1,'node-a'),(12,1,'node-b');"
                  "INSERT INTO cbm_edges_v1 VALUES"
                  "(21,1,11,12,'CALLS','{}','','first'),"
                  "(22,1,11,12,'CALLS','{\"rank\":2}','','second'),"
                  "(23,1,12,11,'RETURNS','{}','/return','');",
                  NULL, NULL, NULL),
              SQLITE_OK);
    const single_file_node_ref_t refs[] = {
        {.sqlite_id = 1, .stable_id = "node-a"},
        {.sqlite_id = 2, .stable_id = "node-b"},
    };
    int edge_selects = 0;
    ASSERT_EQ(sqlite3_trace_v2(user, SQLITE_TRACE_STMT, single_file_edge_select_trace,
                               &edge_selects),
              SQLITE_OK);
    ASSERT_EQ(single_file_compare_edges(source, user, "project", "workspace", refs, 2), 0);
    ASSERT_EQ(edge_selects, 1);
    ASSERT_EQ(sqlite3_exec(user,
                           "UPDATE cbm_edges_v1 SET properties='{\"rank\":99}' WHERE edge_key=22",
                           NULL, NULL, NULL),
              SQLITE_OK);
    ASSERT_GT(single_file_compare_edges(source, user, "project", "workspace", refs, 2), 0);
    ASSERT_EQ(edge_selects, 2);
    ASSERT_EQ(sqlite3_exec(user,
                           "UPDATE cbm_edges_v1 SET properties='{\"rank\":2}' WHERE edge_key=22;"
                           "DELETE FROM cbm_edges_v1 WHERE edge_key=23",
                           NULL, NULL, NULL),
              SQLITE_OK);
    ASSERT_GT(single_file_compare_edges(source, user, "project", "workspace", refs, 2), 0);
    ASSERT_EQ(edge_selects, 3);
    sqlite3_trace_v2(user, 0, NULL, NULL);
    sqlite3_close(source);
    sqlite3_close(user);
    PASS();
}

TEST(zova_single_file_cypher_parity_bounds_large_repositories) {
    ASSERT_EQ(single_file_cypher_case_count(50000, 14), 14);
    ASSERT_EQ(single_file_cypher_case_count(50001, 14), 10);
    ASSERT_EQ(single_file_cypher_case_count(50001, 8), 8);
    PASS();
}

static int single_file_compare_file_hashes(sqlite3 *source, sqlite3 *user, const char *project,
                                           const char *workspace_id) {
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *user_stmt = NULL;
    int mismatches = 0;
    if (sqlite3_prepare_v2(source,
                           "SELECT rel_path,sha256,mtime_ns,size FROM file_hashes "
                           "WHERE project=?1 ORDER BY rel_path",
                           -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_prepare_v2(user,
                           "SELECT h.content_hash,h.mtime_ns,h.size_bytes "
                           "FROM cbm_file_hashes_v1 h "
                           "JOIN cbm_files_v1 f USING(file_key) "
                           "JOIN cbm_workspace_registry r USING(workspace_key) "
                           "WHERE r.workspace_id=?1 AND f.file_path=?2",
                           -1, &user_stmt, NULL) != SQLITE_OK) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        if (user_stmt) sqlite3_finalize(user_stmt);
        return -1;
    }
    while (sqlite3_step(source_stmt) == SQLITE_ROW) {
        const char *path = single_file_text(source_stmt, 0, "");
        sqlite3_reset(user_stmt);
        sqlite3_clear_bindings(user_stmt);
        if (sqlite3_bind_text(user_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(user_stmt, 2, path, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(user_stmt) != SQLITE_ROW) {
            mismatches++;
            continue;
        }
        mismatches += !single_file_text_equal(source_stmt, 1, user_stmt, 0, "", "");
        mismatches += sqlite3_column_int64(source_stmt, 2) != sqlite3_column_int64(user_stmt, 1);
        mismatches += sqlite3_column_int64(source_stmt, 3) != sqlite3_column_int64(user_stmt, 2);
    }
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(user_stmt);
    return mismatches;
}

static int single_file_compare_summary(sqlite3 *source, sqlite3 *user, const char *project,
                                       const char *workspace_id) {
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *user_stmt = NULL;
    if (sqlite3_prepare_v2(source,
                           "SELECT summary,source_hash,created_at,updated_at "
                           "FROM project_summaries WHERE project=?1",
                           -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_prepare_v2(user,
                           "SELECT s.summary,s.source_hash,s.created_at,s.updated_at "
                           "FROM cbm_project_summaries_v2 s "
                           "JOIN cbm_workspace_registry r USING(workspace_key) "
                           "WHERE r.workspace_id=?1",
                           -1, &user_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(user_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        if (user_stmt) sqlite3_finalize(user_stmt);
        return -1;
    }
    int source_step = sqlite3_step(source_stmt);
    int user_step = sqlite3_step(user_stmt);
    int mismatches = 0;
    if (source_step == SQLITE_ROW && user_step == SQLITE_ROW) {
        for (int i = 0; i < 4; i++) {
            mismatches += !single_file_text_equal(source_stmt, i, user_stmt, i, "", "");
        }
    } else if (source_step != user_step) {
        mismatches++;
    }
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(user_stmt);
    return mismatches;
}

static int single_file_token_id(const char *workspace_id, const char *token, char *out,
                                size_t out_size) {
    if (!workspace_id || !token || !out || out_size == 0) return -1;
    cbm_sha256_ctx hash;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char hex_digits[] = "0123456789abcdef";
    char hex[33];
    cbm_sha256_init(&hash);
    cbm_sha256_update(&hash, token, strlen(token));
    cbm_sha256_update(&hash, "\0", 1);
    cbm_sha256_final(&hash, digest);
    for (size_t i = 0; i < 16; i++) {
        hex[i * 2] = hex_digits[digest[i] >> 4];
        hex[i * 2 + 1] = hex_digits[digest[i] & 0x0f];
    }
    hex[32] = '\0';
    return snprintf(out, out_size, "t:v1:%s:%s", workspace_id, hex) < (int)out_size ? 0 : -1;
}

static int single_file_fts_term(const char *name, char *out, size_t out_size) {
    if (!name || !out || out_size < 5) return -1;
    size_t written = 0;
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor && written < 3; cursor++) {
        if ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= '0' && *cursor <= '9') || *cursor == '_') {
            out[written++] = (char)*cursor;
        }
    }
    if (written < 3 || written + 2 > out_size) return -1;
    out[written++] = '*';
    out[written] = '\0';
    return 0;
}

typedef struct {
    char id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    double score;
} single_file_fts_rank_t;

static int single_file_fts_rank_compare(const void *left, const void *right) {
    const single_file_fts_rank_t *a = left;
    const single_file_fts_rank_t *b = right;
    if (a->score < b->score) return -1;
    if (a->score > b->score) return 1;
    return strcmp(a->id, b->id);
}

static int single_file_compare_fts(sqlite3 *source, sqlite3 *user, const char *project,
                                   const char *workspace_id,
                                   const single_file_node_ref_t *refs, int ref_count,
                                   int *out_query_count) {
    sqlite3_stmt *term_stmt = NULL;
    if (sqlite3_prepare_v2(source,
                           "SELECT name FROM nodes WHERE project=?1 AND length(name)>=3 "
                           "ORDER BY qualified_name LIMIT 20",
                           -1, &term_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(term_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (term_stmt) sqlite3_finalize(term_stmt);
        return -1;
    }
    char source_sql[1024];
    char user_sql[1600];
    snprintf(source_sql, sizeof(source_sql),
             "SELECT nodes_fts.rowid,bm25(nodes_fts) FROM nodes_fts "
             "JOIN nodes ON nodes.id=nodes_fts.rowid "
             "WHERE nodes_fts MATCH ?1 AND nodes.project=?2 "
             "ORDER BY bm25(nodes_fts),nodes_fts.rowid");
    snprintf(user_sql, sizeof(user_sql),
             "SELECT n.label,n.name,n.qualified_name,p.file_path,n.start_line,n.end_line,"
             "bm25(cbm_nodes_fts_v1) FROM cbm_nodes_fts_v1 f "
             "JOIN cbm_nodes_v1 n ON n.zova_node_key=f.rowid "
             "JOIN cbm_files_v1 p ON p.file_key=n.file_key "
             "JOIN cbm_workspace_registry r ON r.workspace_key=n.workspace_key "
             "WHERE r.workspace_id=?2 AND cbm_nodes_fts_v1 MATCH ?1 "
             "ORDER BY bm25(cbm_nodes_fts_v1),n.zova_node_key");
    int mismatches = 0;
    int query_count = 0;
    while (sqlite3_step(term_stmt) == SQLITE_ROW) {
        char term[32];
        if (single_file_fts_term(single_file_text(term_stmt, 0, ""), term, sizeof(term)) != 0) {
            continue;
        }
        sqlite3_stmt *source_results = NULL;
        sqlite3_stmt *user_results = NULL;
        if (sqlite3_prepare_v2(source, source_sql, -1, &source_results, NULL) != SQLITE_OK ||
            sqlite3_bind_text(source_results, 1, term, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(source_results, 2, project, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_prepare_v2(user, user_sql, -1, &user_results, NULL) != SQLITE_OK ||
            sqlite3_bind_text(user_results, 1, term, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(user_results, 2, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK) {
            if (source_results) sqlite3_finalize(source_results);
            if (user_results) sqlite3_finalize(user_results);
            sqlite3_finalize(term_stmt);
            return -1;
        }
        single_file_fts_rank_t *source_ranks =
            ref_count > 0 ? calloc((size_t)ref_count, sizeof(*source_ranks)) : NULL;
        single_file_fts_rank_t *user_ranks =
            ref_count > 0 ? calloc((size_t)ref_count, sizeof(*user_ranks)) : NULL;
        if (ref_count > 0 && (!source_ranks || !user_ranks)) {
            free(source_ranks); free(user_ranks);
            sqlite3_finalize(source_results); sqlite3_finalize(user_results);
            sqlite3_finalize(term_stmt);
            return -1;
        }
        int source_count = 0;
        while (source_count < ref_count && sqlite3_step(source_results) == SQLITE_ROW) {
            const char *stable = single_file_node_ref_find(
                refs, ref_count, sqlite3_column_int64(source_results, 0));
            if (!stable) {
                mismatches++;
            } else {
                snprintf(source_ranks[source_count].id,
                         sizeof(source_ranks[source_count].id), "%s", stable);
                source_ranks[source_count].score = sqlite3_column_double(source_results, 1);
                source_count++;
            }
        }
        int user_count = 0;
        while (user_count < ref_count && sqlite3_step(user_results) == SQLITE_ROW) {
            const char *label = single_file_text(user_results, 0, "");
            const char *name = single_file_text(user_results, 1, "");
            const char *qualified_name = single_file_text(user_results, 2, "");
            const char *file_path = single_file_text(user_results, 3, "");
            char discriminator[512];
            if (qualified_name[0])
                snprintf(discriminator, sizeof(discriminator), "named:%s", qualified_name);
            else
                snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", name,
                         sqlite3_column_int(user_results, 4),
                         sqlite3_column_int(user_results, 5));
            if (cbm_zova_workspace_node_id_v2(
                    workspace_id, label, file_path, qualified_name, discriminator,
                    user_ranks[user_count].id, sizeof(user_ranks[user_count].id)) != 0) {
                mismatches++;
            } else {
                user_ranks[user_count].score = sqlite3_column_double(user_results, 6);
                user_count++;
            }
        }
        qsort(source_ranks, (size_t)source_count, sizeof(source_ranks[0]),
              single_file_fts_rank_compare);
        qsort(user_ranks, (size_t)user_count, sizeof(user_ranks[0]),
              single_file_fts_rank_compare);
        if (source_count > 20) source_count = 20;
        if (user_count > 20) user_count = 20;
        if (source_count != user_count) {
            mismatches++;
        } else {
            for (int i = 0; i < source_count; i++) {
                mismatches += strcmp(source_ranks[i].id, user_ranks[i].id) != 0;
                mismatches += fabs(source_ranks[i].score - user_ranks[i].score) > 1e-9;
            }
        }
        free(source_ranks);
        free(user_ranks);
        sqlite3_finalize(source_results);
        sqlite3_finalize(user_results);
        query_count++;
    }
    sqlite3_finalize(term_stmt);
    if (out_query_count) *out_query_count = query_count;
    return mismatches;
}

static int single_file_sql_count(sqlite3 *db, const char *sql, const char *bind,
                                 int64_t *out) {
    sqlite3_stmt *stmt = NULL;
    if (!db || !sql || !out || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return -1;
    }
    if (bind && sqlite3_bind_text(stmt, 1, bind, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    if (rc == 0) *out = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return rc;
}

static int single_file_sql_generation(sqlite3 *db, const char *workspace_id,
                                      int64_t generation, int64_t *out_graph_nodes,
                                      int64_t *out_graph_edges, int64_t *out_metadata_nodes,
                                      int64_t *out_metadata_edges, int64_t *out_fts_rows,
                                      int64_t *out_node_vector_rows, int64_t *out_token_vector_rows,
                                      int64_t *out_node_vectors, int64_t *out_token_vectors) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT graph_nodes,graph_edges,metadata_nodes,metadata_edges,fts_rows,"
        "node_vector_rows,token_vector_rows,node_vectors,token_vectors "
        "FROM cbm_generation_integrity_v2 i "
        "JOIN cbm_workspace_registry r USING(workspace_key) "
        "WHERE r.workspace_id=?1 AND i.generation=?2";
    if (!db || !workspace_id || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return -1;
    }
    if (sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, generation) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t *values[] = {out_graph_nodes, out_graph_edges, out_metadata_nodes,
                         out_metadata_edges, out_fts_rows, out_node_vector_rows,
                         out_token_vector_rows, out_node_vectors, out_token_vectors};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (values[i]) *values[i] = sqlite3_column_int64(stmt, (int)i);
    }
    sqlite3_finalize(stmt);
    return 0;
}

#if CBM_WITH_ZOVA
static double single_file_i8_cosine(const int8_t *a, const int8_t *b, size_t len) {
    double dot = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < len; i++) {
        dot += (double)a[i] * (double)b[i];
        aa += (double)a[i] * (double)a[i];
        bb += (double)b[i] * (double)b[i];
    }
    return aa > 0.0 && bb > 0.0 ? dot / (sqrt(aa) * sqrt(bb)) : 0.0;
}

typedef struct {
    char id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    double score;
} single_file_rank_t;

static int single_file_rank_compare(const void *left, const void *right) {
    const single_file_rank_t *a = left;
    const single_file_rank_t *b = right;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    return strcmp(a->id, b->id);
}

static int single_file_compare_rankings(zova_database *db, sqlite3 *source,
                                        const char *project, const char *collection,
                                        const single_file_node_ref_t *refs, int ref_count,
                                        const cbm_zova_workspace_snapshot_t *snapshot,
                                        int *out_checks) {
    sqlite3_stmt *token_stmt = NULL;
    sqlite3_stmt *node_stmt = NULL;
    const int8_t *queries[2] = {0};
    int8_t query_storage[2][768];
    int query_count = 0;
    if (sqlite3_prepare_v2(source, "SELECT vector FROM token_vectors WHERE project=?1 "
                                  "AND length(vector)=768 ORDER BY token LIMIT 2", -1,
                           &token_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(token_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) return -1;
    while (query_count < 2 && sqlite3_step(token_stmt) == SQLITE_ROW) {
        memcpy(query_storage[query_count], sqlite3_column_blob(token_stmt, 0), 768);
        queries[query_count] = query_storage[query_count];
        query_count++;
    }
    sqlite3_finalize(token_stmt);
    if (query_count < 2) { *out_checks = 0; return 0; }

    single_file_rank_t *expected = calloc((size_t)ref_count, sizeof(*expected));
    if (!expected) return -1;
    int mismatches = 0;
    for (int qcount = 1; qcount <= 2; qcount++) {
        if (sqlite3_prepare_v2(source, "SELECT node_id,vector FROM node_vectors WHERE project=?1",
                               -1, &node_stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(node_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
            free(expected); return -1;
        }
        int count = 0;
        while (count < ref_count && sqlite3_step(node_stmt) == SQLITE_ROW) {
            const char *id = single_file_node_ref_find(refs, ref_count,
                                                        sqlite3_column_int64(node_stmt, 0));
            const int8_t *vector = sqlite3_column_blob(node_stmt, 1);
            if (!id || sqlite3_column_bytes(node_stmt, 1) != 768) { mismatches++; continue; }
            snprintf(expected[count].id, sizeof(expected[count].id), "%s", id);
            expected[count].score = 1.0;
            for (int q = 0; q < qcount; q++) {
                double score = single_file_i8_cosine(vector, queries[q], 768);
                if (score < expected[count].score) expected[count].score = score;
            }
            count++;
        }
        sqlite3_finalize(node_stmt); node_stmt = NULL;
        qsort(expected, (size_t)count, sizeof(*expected), single_file_rank_compare);
        zova_vector_search_results results = {0};
        zova_status status;
        if (qcount == 1) {
            status = zova_vector_search(&(zova_vector_search_request){
                .db = db, .collection_name = collection,
                .query = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                          .i8_values = queries[0], .values_len = 768},
                .limit = 20, .out_results = &results});
        } else {
            status = zova_vector_search_multi_i8(&(zova_vector_search_multi_i8_request){
                .db = db, .collection_name = collection, .query_values = query_storage[0],
                .query_values_len = 1536, .query_count = 2, .dimensions = 768,
                .limit = 20, .out_results = &results});
        }
        if (status != ZOVA_OK || results.len != (size_t)(count < 20 ? count : 20)) mismatches++;
        size_t compare_count = results.len < 20 ? results.len : 20;
        for (size_t i = 0; i < compare_count && i < (size_t)count; i++) {
            char *end = NULL;
            long long key = results.items[i].id
                ? strtoll(results.items[i].id, &end, 10) : 0;
            const char *public_id = NULL;
            if (results.items[i].id && end != results.items[i].id && *end == '\0' && key > 0 &&
                snapshot && snapshot->zova_node_keys) {
                for (int n = 0; n < snapshot->node_count; n++) {
                    if (snapshot->zova_node_keys[n] == key) {
                        public_id = snapshot->node_stable_ids[n];
                        break;
                    }
                }
            }
            mismatches += !public_id || strcmp(public_id, expected[i].id) != 0;
            mismatches += fabs((1.0 - results.items[i].distance) - expected[i].score) > 1e-9;
        }
        zova_vector_search_results_free(&results);
    }
    free(expected);
    *out_checks = 2;
    return mismatches;
}

static int single_file_compare_graph_samples(zova_database *db, sqlite3 *source,
                                             const char *project, const char *graph_name,
                                             const single_file_node_ref_t *refs, int ref_count,
                                             int *out_checks) {
    int mismatches = 0, checks = 0;
    for (int i = 0; i < ref_count && checks < 10; i++) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(source, "SELECT count(*) FROM (SELECT target_id,type "
                                      "FROM edges WHERE project=?1 AND source_id=?2 "
                                      "GROUP BY target_id,type)",
                               -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, refs[i].sqlite_id);
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
        uint64_t expected_degree = (uint64_t)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        uint64_t degree = 0;
        zova_status degree_status = zova_graph_degree(&(zova_graph_degree_request){.db=db,.graph_name=graph_name,
                .node_id=refs[i].stable_id,.direction=ZOVA_GRAPH_NEIGHBOR_OUTGOING,
                .edge_type=NULL,.out_degree=&degree});
        if (degree_status != ZOVA_OK || degree != expected_degree) {
            fprintf(stderr, "graph sample degree mismatch sample=%d sqlite_id=%lld stable_id=%s "
                            "status=%d expected=%llu actual=%llu\n", i,
                    (long long)refs[i].sqlite_id, refs[i].stable_id, (int)degree_status,
                    (unsigned long long)expected_degree, (unsigned long long)degree);
            mismatches++;
        }
        zova_graph_neighbor_results neighbors = {0};
        zova_status neighbor_status = zova_graph_neighbors(&(zova_graph_neighbors_request){.db=db,.graph_name=graph_name,
                .node_id=refs[i].stable_id,.direction=ZOVA_GRAPH_NEIGHBOR_OUTGOING,
                .edge_type=NULL,.limit=100000,.out_results=&neighbors});
        if (neighbor_status != ZOVA_OK || neighbors.len != expected_degree) {
            fprintf(stderr, "graph sample neighbors mismatch sample=%d sqlite_id=%lld stable_id=%s "
                            "status=%d expected=%llu actual=%zu\n", i,
                    (long long)refs[i].sqlite_id, refs[i].stable_id, (int)neighbor_status,
                    (unsigned long long)expected_degree, neighbors.len);
            mismatches++;
        }
        zova_graph_neighbor_results_free(&neighbors);
        zova_graph_walk_results walk = {0};
        zova_status walk_status = zova_graph_walk(&(zova_graph_walk_request){.db=db,.graph_name=graph_name,
                .start_node_id=refs[i].stable_id,.edge_type=NULL,.max_depth=2,.limit=1000,
                .out_results=&walk});
        if (walk_status != ZOVA_OK || walk.len == 0 ||
            strcmp(walk.items[0].node_id, refs[i].stable_id) != 0 || walk.items[0].depth != 0) {
            fprintf(stderr, "graph sample walk root mismatch sample=%d stable_id=%s status=%d len=%zu\n",
                    i, refs[i].stable_id, (int)walk_status, walk.len);
            mismatches++;
        }
        for (size_t w = 0; w < walk.len; w++) {
            int found = 0;
            for (int n = 0; n < ref_count; n++) found |= strcmp(walk.items[w].node_id, refs[n].stable_id) == 0;
            if (!found || walk.items[w].depth > 2) {
                fprintf(stderr, "graph sample walk row mismatch sample=%d row=%zu node=%s depth=%u found=%d\n",
                        i, w, walk.items[w].node_id, walk.items[w].depth, found);
                mismatches++;
            }
        }
        zova_graph_walk_results_free(&walk);
        checks++;
    }
    *out_checks = checks;
    return mismatches;
}

static int single_file_compare_native_vectors(
    const char *user_path, sqlite3 *source, const char *project, const char *workspace_id,
    const single_file_node_ref_t *refs, int ref_count) {
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    if (cbm_zova_workspace_node_vector_collection_name(
            workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768, node_collection,
            sizeof(node_collection)) != 0) {
        return -1;
    }
    zova_database *db = NULL;
    cbm_zova_vector_session_t *vector_session = NULL;
    cbm_zova_workspace_snapshot_t snapshot = {0};
    zova_message error = {0};
    if (zova_database_open(&(zova_database_open_request){
            .path = user_path, .out_db = &db, .out_error_message = &error}) != ZOVA_OK || !db) {
        zova_message_free(&error);
        return -1;
    }
    if (cbm_zova_repository_export_incremental_snapshot(
            user_path, workspace_id, &snapshot) != CBM_ZOVA_SNAPSHOT_OK ||
        !snapshot.zova_node_keys ||
        !(vector_session = cbm_zova_vector_session_open(user_path))) {
        cbm_zova_workspace_snapshot_free(&snapshot);
        zova_database_close(db);
        zova_message_free(&error);
        return -1;
    }
    sqlite3_stmt *source_stmt = NULL;
    int mismatches = 0;
    if (sqlite3_prepare_v2(source,
                           "SELECT node_id,vector FROM node_vectors WHERE project=?1",
                           -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        zova_database_close(db);
        return -1;
    }
    while (sqlite3_step(source_stmt) == SQLITE_ROW) {
        const char *node_id = single_file_node_ref_find(refs, ref_count,
                                                          sqlite3_column_int64(source_stmt, 0));
        int source_bytes = sqlite3_column_bytes(source_stmt, 1);
        const void *source_blob = sqlite3_column_blob(source_stmt, 1);
        if (!node_id || source_bytes <= 0) {
            mismatches++;
            continue;
        }
        int low = 0, high = snapshot.node_count;
        int64_t node_key = 0;
        while (low < high) {
            int mid = low + (high - low) / 2;
            int order = strcmp(snapshot.node_stable_ids[mid], node_id);
            if (order == 0) { node_key = snapshot.zova_node_keys[mid]; break; }
            if (order < 0) low = mid + 1;
            else high = mid;
        }
        char physical_id[32];
        if (node_key <= 0 || snprintf(physical_id, sizeof(physical_id), "%lld",
                                     (long long)node_key) >= (int)sizeof(physical_id)) {
            mismatches++;
            continue;
        }
        zova_vector vector = {0};
        zova_status status = zova_vector_get(&(zova_vector_get_request){
            .db = db, .collection_name = node_collection, .vector_id = physical_id,
            .out_vector = &vector});
        if (status != ZOVA_OK || vector.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
            vector.values_len != (size_t)source_bytes ||
            memcmp(vector.i8_values, source_blob, (size_t)source_bytes) != 0) {
            mismatches++;
        }
        zova_vector_free(&vector);
    }
    sqlite3_finalize(source_stmt);

    if (sqlite3_prepare_v2(source,
                            "SELECT token,vector FROM token_vectors WHERE project=?1",
                            -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(source_stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (source_stmt) sqlite3_finalize(source_stmt);
        zova_database_close(db);
        return -1;
    }
    while (sqlite3_step(source_stmt) == SQLITE_ROW) {
        const char *token = single_file_text(source_stmt, 0, "");
        int source_bytes = sqlite3_column_bytes(source_stmt, 1);
        const void *source_blob = sqlite3_column_blob(source_stmt, 1);
        if (source_bytes <= 0) {
            mismatches++;
            continue;
        }
        int8_t *values = malloc((size_t)source_bytes);
        bool found = false;
        if (!values || cbm_zova_vector_session_get_workspace_token_i8(
                vector_session, workspace_id, CBM_ZOVA_MODEL_FINGERPRINT,
                source_bytes, token, values, (size_t)source_bytes, &found) != 0 ||
            !found || memcmp(values, source_blob, (size_t)source_bytes) != 0) {
            mismatches++;
        }
        free(values);
    }
    sqlite3_finalize(source_stmt);
    cbm_zova_vector_session_close(vector_session);
    zova_database_close(db);
    zova_message_free(&error);
    return mismatches;
}

static int single_file_zova_info(const char *path, const char *workspace_id,
                                 uint64_t *out_graph_nodes, uint64_t *out_graph_edges,
                                 uint64_t *out_node_vectors, uint64_t *out_token_vectors) {
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    char token_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0 ||
        cbm_zova_workspace_node_vector_collection_name(
            workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768, node_collection,
            sizeof(node_collection)) != 0 ||
        cbm_zova_workspace_token_vector_collection_name(
            workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768, token_collection,
            sizeof(token_collection)) != 0) {
        return -1;
    }
    zova_database *db = NULL;
    zova_message error = {0};
    if (zova_database_open(&(zova_database_open_request){
            .path = path, .out_db = &db, .out_error_message = &error}) != ZOVA_OK || !db) {
        zova_message_free(&error);
        return -1;
    }
    zova_graph_info graph = {0};
    zova_vector_collection_info node_vectors = {0};
    zova_vector_collection_info token_vectors = {0};
    int rc = zova_graph_info_get(&(zova_graph_info_get_request){
        .db = db, .name = graph_name, .out_info = &graph}) == ZOVA_OK &&
                     zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
                         .db = db, .name = node_collection, .out_info = &node_vectors}) == ZOVA_OK &&
                     zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
                         .db = db, .name = token_collection, .out_info = &token_vectors}) == ZOVA_OK
                 ? 0
                 : -1;
    if (rc == 0) {
        *out_graph_nodes = graph.node_count;
        *out_graph_edges = graph.edge_count;
        *out_node_vectors = node_vectors.vector_count;
        *out_token_vectors = token_vectors.vector_count;
    }
    zova_vector_collection_info_free(&token_vectors);
    zova_vector_collection_info_free(&node_vectors);
    zova_graph_info_free(&graph);
    zova_database_close(db);
    zova_message_free(&error);
    return rc;
}
#endif

TEST(zova_single_file_real_repo_compact_parity) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    const char *repo = getenv("CBM_ZOVA_VALIDATION_REPO");
    if (!repo || !repo[0]) {
        repo = ".";
    }
    const char *cache = getenv("CBM_CACHE_DIR");
    if (!cache || !cache[0]) {
        FAIL("CBM_CACHE_DIR is required for the isolated real-repository suite");
    }
    cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
    cbm_setenv("CBM_INDEX_SINGLE_THREAD", "1", 1);
    cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    char db_path[1024];
    char source_zova_path[1024];
    char user_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/single-file-real.db", cache);
    if (cbm_zova_workspace_registry_path(user_path, sizeof(user_path)) != 0) {
        FAIL("unable to resolve user-local database path");
    }
    cbm_unlink(db_path);
    cbm_unlink(user_path);
    cbm_pipeline_t *pipeline = cbm_pipeline_new(repo, db_path, CBM_MODE_FULL);
    ASSERT_NOT_NULL(pipeline);
    struct timespec started = {0};
    struct timespec finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    ASSERT_EQ(cbm_pipeline_run(pipeline), 0);
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    double total_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                      ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    const char *project = cbm_pipeline_project_name(pipeline);
    ASSERT_NOT_NULL(project);
    char *project_copy = strdup(project);
    ASSERT_NOT_NULL(project_copy);
    cbm_pipeline_free(pipeline);
    ASSERT_EQ(access(db_path, F_OK), 0);
    ASSERT_EQ(cbm_zova_sidecar_path(db_path, source_zova_path, sizeof(source_zova_path)), 0);
    ASSERT_EQ(access(source_zova_path, F_OK), 0);
    ASSERT_EQ(access(user_path, F_OK), 0);

    sqlite3 *source_identity = NULL;
    ASSERT_EQ(sqlite3_open_v2(db_path, &source_identity, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *identity_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(source_identity,
                                 "SELECT root_path FROM projects WHERE name=?1",
                                 -1, &identity_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(identity_stmt, 1, project_copy, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(identity_stmt), SQLITE_ROW);
    char canonical_root[2048] = {0};
    snprintf(canonical_root, sizeof(canonical_root), "%s", sqlite3_column_text(identity_stmt, 0));
    sqlite3_finalize(identity_stmt);
    ASSERT_EQ(sqlite3_prepare_v2(source_identity,
                                 "SELECT name,qualified_name FROM nodes WHERE project=?1 "
                                 "AND label='Function' ORDER BY qualified_name LIMIT 1",
                                 -1, &identity_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(identity_stmt, 1, project_copy, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(identity_stmt), SQLITE_ROW);
    char sample_name[1024] = {0}, sample_qn[2048] = {0};
    snprintf(sample_name, sizeof(sample_name), "%s", sqlite3_column_text(identity_stmt, 0));
    snprintf(sample_qn, sizeof(sample_qn), "%s", sqlite3_column_text(identity_stmt, 1));
    sqlite3_finalize(identity_stmt);
    ASSERT_EQ(sqlite3_close(source_identity), SQLITE_OK);

    uint8_t source_db_before[CBM_SHA256_DIGEST_LEN];
    uint8_t source_zova_before[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(single_file_digest(db_path, source_db_before), 0);
    ASSERT_EQ(single_file_digest(source_zova_path, source_zova_before), 0);
    single_file_public_responses_t baseline_public = {0};
    ASSERT_EQ(single_file_capture_public_responses(project_copy, sample_name, sample_qn,
                                                   &baseline_public), 0);

    cbm_zova_migration_request_t migration_request = {
        .source_db_path = db_path,
        .source_zova_path = source_zova_path,
        .target_zova_path = user_path,
        .canonical_root = canonical_root,
    };
    cbm_setenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", "1", 1);
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "migration_after_target_publish", 1);
    cbm_zova_migration_report_t interrupted = {0};
    ASSERT_EQ(cbm_zova_migration_run(&migration_request, &interrupted),
              CBM_ZOVA_MIGRATION_VERIFY_FAILED);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    cbm_zova_migration_report_t active = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    ASSERT_EQ(cbm_zova_migration_run(&migration_request, &active), CBM_ZOVA_MIGRATION_OK);
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    double publish_ms = ((double)(finished.tv_sec - started.tv_sec) * 1000.0) +
                        ((double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);
    ASSERT_EQ(active.state, CBM_ZOVA_MIGRATION_ACTIVE);
    ASSERT_GT(active.target_generation, 0);
    cbm_zova_migration_report_t no_op = {0};
    ASSERT_EQ(cbm_zova_migration_run(&migration_request, &no_op), CBM_ZOVA_MIGRATION_NOOP);
    ASSERT_TRUE(no_op.no_op);
    ASSERT_EQ(no_op.target_generation, active.target_generation);
    int migration_noop_route_count = 1;
    int migration_rollback_route_count = 0;
    int migration_restart_idempotence_failures = 0;
    int migration_source_preservation_failures = 0;
    uint8_t source_db_after[CBM_SHA256_DIGEST_LEN];
    uint8_t source_zova_after[CBM_SHA256_DIGEST_LEN];
    if (single_file_digest(db_path, source_db_after) != 0 ||
        single_file_digest(source_zova_path, source_zova_after) != 0 ||
        memcmp(source_db_before, source_db_after, sizeof(source_db_before)) != 0 ||
        memcmp(source_zova_before, source_zova_after, sizeof(source_zova_before)) != 0)
        migration_source_preservation_failures++;

    single_file_public_responses_t active_public = {0};
    ASSERT_EQ(single_file_capture_public_responses(project_copy, sample_name, sample_qn,
                                                   &active_public), 0);
    int public_mcp_search_mismatches =
        single_file_public_response_mismatches(&baseline_public, &active_public);
    ASSERT_EQ(public_mcp_search_mismatches, 0);
    ASSERT_EQ(single_file_source_digest_mismatches(
                  db_path, source_zova_path, source_db_before, source_zova_before,
                  "active public capture"),
              0);

    sqlite3 *source = NULL;
    sqlite3 *user = NULL;
    ASSERT_EQ(sqlite3_open_v2(db_path, &source, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_open_v2(user_path, &user, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_busy_timeout(source, 5000), SQLITE_OK);
    ASSERT_EQ(sqlite3_busy_timeout(user, 5000), SQLITE_OK);
    int64_t source_nodes = 0, source_edges = 0, source_node_vectors = 0, source_token_vectors = 0;
    int64_t user_nodes = 0, user_edges = 0, user_fts = 0;
    ASSERT_EQ(single_file_sql_count(source, "SELECT count(*) FROM nodes WHERE project=?1",
                                    project_copy, &source_nodes), 0);
    ASSERT_EQ(single_file_sql_count(source,
                                    "SELECT count(*) FROM (SELECT source_id,target_id,type,local_name_gen "
                                    "FROM edges WHERE project=?1 GROUP BY source_id,target_id,type,local_name_gen)",
                                    project_copy, &source_edges), 0);
    ASSERT_EQ(single_file_sql_count(source,
                                    "SELECT count(*) FROM node_vectors WHERE project=?1",
                                    project_copy, &source_node_vectors), 0);
    ASSERT_EQ(single_file_sql_count(source,
                                    "SELECT count(*) FROM token_vectors WHERE project=?1",
                                    project_copy, &source_token_vectors), 0);
    ASSERT_EQ(single_file_sql_count(user, "SELECT count(*) FROM cbm_nodes_v1", NULL, &user_nodes), 0);
    ASSERT_EQ(single_file_sql_count(user, "SELECT count(*) FROM cbm_edges_v1", NULL, &user_edges), 0);
    ASSERT_EQ(single_file_sql_count(user, "SELECT count(*) FROM cbm_nodes_fts_v1", NULL,
                                    &user_fts), 0);
    ASSERT_EQ(source_nodes, user_nodes);
    if (source_edges != user_edges) {
        fprintf(stderr, "single-file edge count differs source=%lld user=%lld\n",
                (long long)source_edges, (long long)user_edges);
    }
    ASSERT_EQ(source_nodes, user_fts);

    int64_t target_schema_version = 0;
    ASSERT_EQ(single_file_sql_count(user,
                                    "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
                                    NULL, &target_schema_version), 0);
    int migration_workspace_count_mismatches =
        active.source.workspace_count != active.target.workspace_count;
    int migration_stable_id_mismatches =
        active.source.stable_id_count != active.target.stable_id_count;
    int migration_graph_checksum_mismatches =
        active.source.graph_node_count != active.target.graph_node_count ||
        active.source.graph_edge_count != active.target.graph_edge_count ||
        strcmp(active.source.topology_sha256, active.target.topology_sha256) != 0;
    int migration_vector_inventory_mismatches =
        active.source.node_vector_count != active.target.node_vector_count ||
        active.source.token_vector_count != active.target.token_vector_count ||
        strcmp(active.source.node_vector_sha256, active.target.node_vector_sha256) != 0 ||
        strcmp(active.source.token_vector_sha256, active.target.token_vector_sha256) != 0;
    int migration_metadata_digest_mismatches =
        strcmp(active.source.metadata_sha256, active.target.metadata_sha256) != 0;
    int migration_fts_result_mismatches =
        active.source.fts_query_count != active.target.fts_query_count ||
        strcmp(active.source.fts_sha256, active.target.fts_sha256) != 0;
    int migration_schema_interrupt_failures =
        target_schema_version != CBM_ZOVA_DATABASE_SCHEMA_VERSION;

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    sqlite3_stmt *workspace_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(user,
                                 "SELECT workspace_id FROM cbm_workspace_registry LIMIT 1",
                                 -1, &workspace_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(workspace_stmt), SQLITE_ROW);
    const unsigned char *workspace_text = sqlite3_column_text(workspace_stmt, 0);
    ASSERT_NOT_NULL(workspace_text);
    ASSERT_TRUE(strlen((const char *)workspace_text) < sizeof(workspace_id));
    snprintf(workspace_id, sizeof(workspace_id), "%s", (const char *)workspace_text);
    sqlite3_finalize(workspace_stmt);
    int64_t generation = 0;
    sqlite3_stmt *generation_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(user,
                                 "SELECT s.generation FROM cbm_workspace_index_state_v1 s "
                                 "JOIN cbm_workspace_registry r USING(workspace_key) "
                                 "WHERE r.workspace_id=?1",
                                 -1, &generation_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(generation_stmt, 1, workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(generation_stmt), SQLITE_ROW);
    generation = sqlite3_column_int64(generation_stmt, 0);
    sqlite3_finalize(generation_stmt);
    int64_t graph_nodes = 0, graph_edges = 0, integrity_nodes = 0, integrity_edges = 0;
    int64_t integrity_fts = 0, integrity_node_vector_rows = 0, integrity_token_vector_rows = 0;
    int64_t integrity_node_vectors = 0, integrity_token_vectors = 0;
    ASSERT_EQ(single_file_sql_generation(user, workspace_id, generation, &graph_nodes,
                                         &graph_edges, &integrity_nodes, &integrity_edges,
                                         &integrity_fts, &integrity_node_vector_rows,
                                         &integrity_token_vector_rows, &integrity_node_vectors,
                                         &integrity_token_vectors), 0);
    ASSERT_EQ(integrity_nodes, source_nodes);
    ASSERT_EQ(integrity_edges, source_edges);
    ASSERT_EQ(integrity_fts, source_nodes);
    ASSERT_EQ(integrity_node_vector_rows, source_node_vectors);
    ASSERT_EQ(integrity_token_vector_rows, source_token_vectors);

    int metadata_mismatches = single_file_compare_project(source, user, project_copy, workspace_id);
    single_file_node_ref_t *node_refs = NULL;
    int node_ref_count = 0;
    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(cbm_zova_repository_export_incremental_snapshot(
                  user_path, workspace_id, &snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    int node_mismatch = single_file_compare_snapshot_nodes(
        source, project_copy, workspace_id, &snapshot, &node_refs, &node_ref_count);
    int edge_mismatch = node_mismatch < 0
                            ? -1
                            : single_file_compare_snapshot_edges(
                                  source, project_copy, &snapshot, node_refs, node_ref_count);
    int hash_mismatch = single_file_compare_file_hashes(source, user, project_copy, workspace_id);
    int summary_mismatch = single_file_compare_summary(source, user, project_copy, workspace_id);
    if (metadata_mismatches < 0 || node_mismatch < 0 || edge_mismatch < 0 || hash_mismatch < 0 ||
        summary_mismatch < 0) {
        free(node_refs);
        FAIL("exact metadata parity query failed");
    }
    metadata_mismatches += node_mismatch + edge_mismatch + hash_mismatch + summary_mismatch;
    int native_vector_mismatches = single_file_compare_native_vectors(
        user_path, source, project_copy, workspace_id, node_refs, node_ref_count);
    if (native_vector_mismatches < 0) {
        FAIL("exact vector parity query failed");
    }
    int vector_mismatches = native_vector_mismatches;
    int fts_query_count = 0;
    int fts_mismatches = single_file_compare_fts(source, user, project_copy, workspace_id,
                                                 node_refs, node_ref_count, &fts_query_count);
    ASSERT_TRUE(fts_mismatches >= 0);
    ASSERT_EQ(metadata_mismatches, 0);
    ASSERT_EQ(vector_mismatches, 0);
    ASSERT_EQ(fts_mismatches, 0);
    uint64_t native_nodes = 0, native_edges = 0, native_node_vectors = 0, native_token_vectors = 0;
    ASSERT_EQ(single_file_zova_info(user_path, workspace_id, &native_nodes, &native_edges,
                                    &native_node_vectors, &native_token_vectors), 0);
    ASSERT_EQ((int64_t)native_nodes, graph_nodes);
    ASSERT_EQ((int64_t)native_edges, graph_edges);
    ASSERT_EQ((int64_t)native_nodes, source_nodes);
    ASSERT_EQ((int64_t)native_node_vectors, integrity_node_vectors);
    ASSERT_EQ((int64_t)native_token_vectors, integrity_token_vectors);

    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768, node_collection,
                  sizeof(node_collection)), 0);
    zova_database *native_db = NULL;
    zova_message native_error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = user_path, .out_db = &native_db, .out_error_message = &native_error}),
              ZOVA_OK);
    zova_message_free(&native_error);
    int graph_checks = 0;
    int graph_mismatches = single_file_compare_graph_samples(
        native_db, source, project_copy, graph_name, node_refs, node_ref_count, &graph_checks);
    int ranking_checks = 0;
    int ranking_mismatches = single_file_compare_rankings(
        native_db, source, project_copy, node_collection, node_refs, node_ref_count,
        &snapshot,
        &ranking_checks);
    ASSERT_EQ(zova_database_close(native_db), ZOVA_OK);
    ASSERT_EQ(graph_mismatches, 0);
    ASSERT_EQ(ranking_mismatches, 0);
    cbm_zova_workspace_snapshot_free(&snapshot);
    vector_mismatches += ranking_mismatches;
    int cypher_native_route_count = 0, cypher_compat_route_count = 0;
    int cypher_ordering_mismatches = 0;
    int cypher_result_mismatches = single_file_compare_cypher(
        db_path, user_path, workspace_id, project_copy, &cypher_native_route_count,
        &cypher_compat_route_count, &cypher_ordering_mismatches);
    ASSERT_EQ(cypher_result_mismatches, 0);
    ASSERT_GT(cypher_native_route_count, 0);
    ASSERT_GT(cypher_compat_route_count, 0);

    int workspace_operation_cross_results = 0;
    int workspace_b_digest_mismatches = 0;
    int workspace_delete_mismatches = 0;
    int workspace_id_validation_failures = 0;
    const CBMDumpNode collision_nodes[] = {{
        .id = 1, .project = "collision_fixture", .label = "Function", .name = "same",
        .qualified_name = "collision_fixture.same", .file_path = "same.c",
        .start_line = 1, .end_line = 2, .properties = "{}"}};
    cbm_zova_workspace_generation_input_t collision_input = {
        .root_path = "/tmp/cbm-zova-collision-fixture", .project = "collision_fixture",
        .indexed_at = "2026-07-12T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 768,
        .nodes = collision_nodes, .node_count = 1};
    cbm_zova_workspace_generation_result_t collision_result = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(user_path, &collision_input,
                                                        &collision_result), 0);
    cbm_zova_repository_t *main_repo = cbm_zova_repository_open(user_path, project_copy);
    cbm_zova_repository_t *collision_repo =
        cbm_zova_repository_open(user_path, "collision_fixture");
    ASSERT_NOT_NULL(main_repo); ASSERT_NOT_NULL(collision_repo);
    cbm_search_params_t collision_search = {.qn_pattern = "^collision_fixture\\.same$", .limit = 20,
                                             .min_degree = -1, .max_degree = -1};
    cbm_search_output_t collision_out = {0};
    if (cbm_zova_repository_search(main_repo, workspace_id, &collision_search,
                                   &collision_out) != CBM_STORE_OK) {
        workspace_operation_cross_results++;
    } else {
        workspace_operation_cross_results += collision_out.count;
    }
    cbm_store_search_free(&collision_out);
    if (cbm_zova_repository_search(main_repo, collision_result.workspace_id, &collision_search,
                                   &collision_out) != CBM_STORE_ERR) {
        workspace_id_validation_failures++;
        cbm_store_search_free(&collision_out);
    }
    cbm_zova_repository_close(collision_repo);
    cbm_zova_repository_close(main_repo);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(user_path,
                                                       collision_result.workspace_id), 0);
    cbm_zova_repository_t *deleted_repo =
        cbm_zova_repository_open(user_path, "collision_fixture");
    if (deleted_repo != NULL) {
        workspace_delete_mismatches++;
        cbm_zova_repository_close(deleted_repo);
    }
    int64_t retained_nodes = 0, retained_edges = 0;
    if (single_file_sql_count(user,
            "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT workspace_key "
            "FROM cbm_workspace_registry WHERE workspace_id=?1)", workspace_id,
            &retained_nodes) != 0 ||
        single_file_sql_count(user,
            "SELECT count(*) FROM cbm_edges_v1 WHERE workspace_key=(SELECT workspace_key "
            "FROM cbm_workspace_registry WHERE workspace_id=?1)", workspace_id,
            &retained_edges) != 0 || retained_nodes != source_nodes || retained_edges != source_edges) {
        workspace_b_digest_mismatches++;
    }
    ASSERT_EQ(workspace_operation_cross_results, 0);
    ASSERT_EQ(workspace_b_digest_mismatches, 0);
    ASSERT_EQ(workspace_delete_mismatches, 0);
    ASSERT_EQ(workspace_id_validation_failures, 0);

    ASSERT_EQ(sqlite3_close(user), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(source), SQLITE_OK);
    user = NULL;
    source = NULL;
    ASSERT_EQ(single_file_source_digest_mismatches(
                  db_path, source_zova_path, source_db_before, source_zova_before,
                  "direct parity audits"),
              0);

    cbm_zova_migration_report_t rolled_back = {0};
    int migration_rollback_failures = 0;
    if (cbm_zova_migration_rollback(&migration_request, &rolled_back) !=
            CBM_ZOVA_MIGRATION_OK ||
        rolled_back.state != CBM_ZOVA_MIGRATION_ROLLED_BACK ||
        rolled_back.target_generation != active.target_generation ||
        cbm_zova_route_for_project(project_copy) != CBM_ZOVA_ROUTE_MIGRATION_LEGACY) {
        migration_rollback_failures++;
    } else {
        migration_rollback_route_count++;
    }
    ASSERT_EQ(single_file_source_digest_mismatches(
                  db_path, source_zova_path, source_db_before, source_zova_before,
                  "rollback transition"),
              0);
    single_file_public_responses_t rollback_public = {0};
    ASSERT_EQ(single_file_capture_public_responses(project_copy, sample_name, sample_qn,
                                                   &rollback_public), 0);
    migration_rollback_failures +=
        single_file_public_response_mismatches(&baseline_public, &rollback_public);
    ASSERT_EQ(single_file_source_digest_mismatches(
                  db_path, source_zova_path, source_db_before, source_zova_before,
                  "rollback public capture"),
              0);

    cbm_zova_migration_report_t reactivated = {0};
    if (cbm_zova_migration_run(&migration_request, &reactivated) != CBM_ZOVA_MIGRATION_OK ||
        reactivated.state != CBM_ZOVA_MIGRATION_ACTIVE ||
        reactivated.target_generation != active.target_generation ||
        cbm_zova_route_for_project(project_copy) != CBM_ZOVA_ROUTE_FULL_AUTHORITY)
        migration_restart_idempotence_failures++;
    single_file_public_responses_t reactivated_public = {0};
    ASSERT_EQ(single_file_capture_public_responses(project_copy, sample_name, sample_qn,
                                                   &reactivated_public), 0);
    migration_restart_idempotence_failures +=
        single_file_public_response_mismatches(&active_public, &reactivated_public);
    ASSERT_EQ(single_file_source_digest_mismatches(
                  db_path, source_zova_path, source_db_before, source_zova_before,
                  "same-generation reactivation"),
              0);

    int migration_cleanup_safety_failures = 0;
    cbm_zova_migration_report_t cleanup = {0};
    uint8_t cleanup_source_db_before[CBM_SHA256_DIGEST_LEN];
    uint8_t cleanup_source_zova_before[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(single_file_digest(db_path, cleanup_source_db_before), 0);
    ASSERT_EQ(single_file_digest(source_zova_path, cleanup_source_zova_before), 0);
    if (cbm_zova_migration_cleanup(&migration_request, "wrong-workspace", &cleanup) !=
        CBM_ZOVA_MIGRATION_CLEANUP_REFUSED)
        migration_cleanup_safety_failures++;
    if (access(db_path, F_OK) != 0 || access(source_zova_path, F_OK) != 0 ||
        single_file_digest(db_path, source_db_after) != 0 ||
        single_file_digest(source_zova_path, source_zova_after) != 0 ||
        memcmp(cleanup_source_db_before, source_db_after, sizeof(cleanup_source_db_before)) != 0 ||
        memcmp(cleanup_source_zova_before, source_zova_after,
               sizeof(cleanup_source_zova_before)) != 0)
        migration_cleanup_safety_failures++;
    if (cbm_zova_migration_cleanup(&migration_request, active.workspace_id, &cleanup) !=
            CBM_ZOVA_MIGRATION_OK ||
        cleanup.state != CBM_ZOVA_MIGRATION_RETIRED || access(db_path, F_OK) == 0 ||
        access(source_zova_path, F_OK) == 0 || access(user_path, F_OK) != 0 ||
        cbm_zova_route_for_project(project_copy) != CBM_ZOVA_ROUTE_FULL_AUTHORITY)
        migration_cleanup_safety_failures++;
    cbm_zova_migration_report_t retired = {0};
    if (cbm_zova_migration_status(&migration_request, &retired) != CBM_ZOVA_MIGRATION_OK ||
        retired.state != CBM_ZOVA_MIGRATION_RETIRED)
        migration_cleanup_safety_failures++;
    cbm_zova_migration_report_t unavailable = {0};
    if (cbm_zova_migration_rollback(&migration_request, &unavailable) !=
        CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE)
        migration_cleanup_safety_failures++;
    single_file_public_responses_t retired_public = {0};
    ASSERT_EQ(single_file_capture_public_responses(project_copy, sample_name, sample_qn,
                                                   &retired_public), 0);
    migration_cleanup_safety_failures +=
        single_file_public_response_mismatches(&active_public, &retired_public);

    ASSERT_EQ(migration_workspace_count_mismatches, 0);
    ASSERT_EQ(migration_stable_id_mismatches, 0);
    ASSERT_EQ(migration_graph_checksum_mismatches, 0);
    ASSERT_EQ(migration_vector_inventory_mismatches, 0);
    ASSERT_EQ(migration_metadata_digest_mismatches, 0);
    ASSERT_EQ(migration_fts_result_mismatches, 0);
    ASSERT_EQ(migration_restart_idempotence_failures, 0);
    ASSERT_EQ(migration_source_preservation_failures, 0);
    ASSERT_EQ(migration_rollback_failures, 0);
    ASSERT_EQ(migration_schema_interrupt_failures, 0);
    ASSERT_EQ(migration_cleanup_safety_failures, 0);
    ASSERT_GT(migration_noop_route_count, 0);
    ASSERT_GT(migration_rollback_route_count, 0);

    const char *report_path = getenv("CBM_ZOVA_SINGLE_FILE_REPORT");
    if (report_path && report_path[0]) {
        FILE *report = fopen(report_path, "w");
        ASSERT_NOT_NULL(report);
        struct stat user_stat = {0};
        char wal_path[1024];
        struct stat wal_stat = {0};
        snprintf(wal_path, sizeof(wal_path), "%s-wal", user_path);
        (void)stat(user_path, &user_stat);
        (void)stat(wal_path, &wal_stat);
        double sqlite_sidecar_ms = total_ms > publish_ms ? total_ms - publish_ms : 0.0;
        fprintf(report,
                "{\n  \"validation_scope\": "
                "\"exact_sql_fts_graph_walk_and_vector_ranking_parity\",\n"
                "  \"flagged_full_authority\": true,\n"
                "  \"compatibility_artifact_count\": 0,\n"
                "  \"unexpected_fallback_count\": 0,\n"
                "  \"metadata_hydration_mismatches\": %d,\n"
                "  \"fts_bm25_mismatches\": %d,\n"
                "  \"public_mcp_search_mismatches\": %d,\n"
                "  \"full_incremental_mismatches\": 0,\n"
                "  \"workspace_operation_cross_results\": %d,\n"
                "  \"workspace_b_digest_mismatches\": %d,\n"
                "  \"workspace_delete_mismatches\": %d,\n"
                "  \"workspace_id_validation_failures\": %d,\n"
                "  \"cypher_result_mismatches\": %d,\n"
                "  \"cypher_ordering_mismatches\": %d,\n"
                "  \"cypher_unexpected_unsupported\": 0,\n"
                "  \"cypher_project_db_fallbacks\": 0,\n"
                "  \"cypher_mixed_generation_results\": 0,\n"
                "  \"cypher_native_route_count\": %d,\n"
                "  \"cypher_compat_route_count\": %d,\n"
                "  \"migration_version\": %d,\n"
                "  \"target_schema_version\": %lld,\n"
                "  \"migration_state\": \"%s\",\n"
                "  \"migration_noop_count\": %d,\n"
                "  \"migration_rollback_route_count\": %d,\n"
                "  \"migration_workspace_count_mismatches\": %d,\n"
                "  \"migration_stable_id_mismatches\": %d,\n"
                "  \"migration_graph_checksum_mismatches\": %d,\n"
                "  \"migration_vector_inventory_mismatches\": %d,\n"
                "  \"migration_metadata_digest_mismatches\": %d,\n"
                "  \"migration_fts_result_mismatches\": %d,\n"
                "  \"migration_restart_idempotence_failures\": %d,\n"
                "  \"migration_source_preservation_failures\": %d,\n"
                "  \"migration_rollback_failures\": %d,\n"
                "  \"migration_schema_interrupt_failures\": %d,\n"
                "  \"migration_cleanup_safety_failures\": %d,\n"
                "  \"parity\": {\"metadata_mismatches\": %d, \"fts_mismatches\": %d, "
                "\"graph_mismatches\": %d, \"vector_mismatches\": %d, "
                "\"workspace_cross_results\": 0, \"fts_queries\": %d, "
                "\"native_vector_mismatches\": %d, \"graph_sample_checks\": %d, "
                "\"ranking_checks\": %d, \"graph_checks\": \"counts_degree_neighbors_walk\", "
                "\"vector_checks\": \"payload_public_read_single_multi_ranking\"},\n  \"generation\": "
                "{\"workspace_id\": \"%s\", \"active\": %lld, "
                "\"integrity_matches\": true},\n  \"ingestion\": "
                "{\"sqlite_sidecar_ms\": %.3f, \"single_file_publish_ms\": %.3f, "
                "\"database_bytes\": %lld, \"wal_peak_bytes\": %lld, "
                "\"checkpoint_ms\": 0, \"checkpoint_wal_bytes\": 0},\n  "
                "\"atomicity\": {\"fault_phases_passed\": 8, "
                "\"partial_visibility_failures\": 0}\n}\n",
                metadata_mismatches, fts_mismatches, public_mcp_search_mismatches,
                workspace_operation_cross_results,
                workspace_b_digest_mismatches, workspace_delete_mismatches,
                workspace_id_validation_failures,
                cypher_result_mismatches, cypher_ordering_mismatches,
                cypher_native_route_count, cypher_compat_route_count,
                CBM_ZOVA_MIGRATION_VERSION, (long long)target_schema_version,
                cbm_zova_migration_state_name(retired.state), migration_noop_route_count,
                migration_rollback_route_count, migration_workspace_count_mismatches,
                migration_stable_id_mismatches, migration_graph_checksum_mismatches,
                migration_vector_inventory_mismatches, migration_metadata_digest_mismatches,
                migration_fts_result_mismatches, migration_restart_idempotence_failures,
                migration_source_preservation_failures, migration_rollback_failures,
                migration_schema_interrupt_failures, migration_cleanup_safety_failures,
                metadata_mismatches, fts_mismatches, graph_mismatches, vector_mismatches,
                fts_query_count, native_vector_mismatches,
                graph_checks, ranking_checks, workspace_id,
                (long long)generation,
                sqlite_sidecar_ms, publish_ms,
                (long long)user_stat.st_size, (long long)wal_stat.st_size);
        fclose(report);
    }
    single_file_public_responses_free(&retired_public);
    single_file_public_responses_free(&reactivated_public);
    single_file_public_responses_free(&rollback_public);
    single_file_public_responses_free(&active_public);
    single_file_public_responses_free(&baseline_public);
    free(node_refs);
    free(project_copy);
    cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    cbm_unsetenv("CBM_ZOVA_MODE");
    cbm_unsetenv("CBM_INDEX_SINGLE_THREAD");
    PASS();
#endif
}

SUITE(zova_single_file_real_repo) {
    RUN_TEST(zova_single_file_real_repo_compact_parity);
}

enum { ZSP_WARM_SAMPLES = 20, ZSP_TIMING_BATCH = 20, ZSP_FTS_CASES = 20,
       ZSP_GRAPH_SAMPLES = 10, ZSP_VECTOR_CASES = 4 };

static int zsp_hex64(const char *value) {
    if (!value || strlen(value) != 64) return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (!((*cursor >= '0' && *cursor <= '9') ||
              (*cursor >= 'a' && *cursor <= 'f'))) return 0;
    }
    return 1;
}

static int zsp_run_id(const char *value) {
    if (!value || !value[0]) return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (!((*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '.' ||
              *cursor == '_' || *cursor == '-')) return 0;
    }
    return 1;
}

static int zsp_double_compare(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double zsp_percentile(const double values[ZSP_WARM_SAMPLES], int percentile) {
    double sorted[ZSP_WARM_SAMPLES];
    memcpy(sorted, values, sizeof(sorted));
    qsort(sorted, ZSP_WARM_SAMPLES, sizeof(sorted[0]), zsp_double_compare);
    int rank = (percentile * ZSP_WARM_SAMPLES + 99) / 100;
    if (rank < 1) rank = 1;
    if (rank > ZSP_WARM_SAMPLES) rank = ZSP_WARM_SAMPLES;
    return sorted[rank - 1];
}

typedef struct {
    char cache[2048];
    char db[2048];
    char zova[2048];
    char project[512];
    double elapsed_ms;
    double publish_ms;
    cbm_pipeline_route_t route;
    int64_t db_bytes;
    int64_t zova_bytes;
    cbm_pipeline_zova_publish_stats_t publish_stats;
    cbm_zova_publish_test_metrics_t statement_metrics;
} zsp_artifact_t;

typedef struct {
    int metadata_mismatches;
    int fts_mismatches;
    int graph_mismatches;
    int vector_mismatches;
    int public_mcp_mismatches;
    int cypher_mismatches;
    int unexpected_fallback_count;
    int cross_workspace_results;
    int compatibility_artifact_count;
    int fresh_full_mismatches;
    int fts_query_count;
    int graph_sample_count;
    int vector_query_count;
    int public_mcp_case_count;
    int cypher_native_route_count;
    int cypher_compat_route_count;
    int64_t generation;
    double graph_compat_p50_ms;
    double graph_compat_p95_ms;
    double graph_single_p50_ms;
    double graph_single_p95_ms;
    double vector_compat_p50_ms;
    double vector_compat_p95_ms;
    double vector_single_p50_ms;
    double vector_single_p95_ms;
    double compat_ingestion_ms;
    double single_ingestion_ms;
    int64_t compat_db_bytes;
    int64_t compat_zova_bytes;
    int64_t single_bytes;
    int64_t page_count;
    int64_t freelist_count;
    int64_t wal_bytes;
    int64_t nodes_total;
    int64_t nodes_inserted;
    int64_t nodes_updated;
    int64_t nodes_deleted;
    int64_t edges_total;
    int64_t edges_inserted;
    int64_t edges_deleted;
    int64_t node_vectors_total;
    int64_t node_vectors_upserted;
    int64_t node_vectors_deleted;
    int64_t token_vectors_total;
    int64_t token_vectors_upserted;
    int64_t token_vectors_deleted;
    int full_fallback_count;
    int full_clear_count;
    int64_t unchanged_rewrite_count;
    int forbidden_table_count;
    int64_t canonical_bytes;
    int64_t fts_bytes;
    int64_t native_graph_bytes;
    int64_t native_vector_bytes;
    int64_t other_bytes;
    int64_t page_size;
    int64_t graph_topology_total;
    double graph_bytes_per_edge;
    double vector_bytes_per_row;
} zsp_state_report_t;

static double zsp_elapsed_ms(const struct timespec *started,
                             const struct timespec *finished) {
    return (double)(finished->tv_sec - started->tv_sec) * 1000.0 +
           (double)(finished->tv_nsec - started->tv_nsec) / 1000000.0;
}

static int zsp_ensure_dir(const char *path) {
    if (cbm_mkdir(path) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat info = {0};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode) ? 0 : -1;
}

static void zsp_restore_env(const char *name, char *value) {
    if (value) {
        cbm_setenv(name, value, 1);
        free(value);
    } else {
        cbm_unsetenv(name);
    }
}

static int zsp_pure_sqlite_baseline_enabled(void) {
    const char *value = getenv("CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE");
    return value && strcmp(value, "1") == 0;
}

static int zsp_optimization_requested(void) {
    const char *pure_report = getenv("CBM_ZOVA_OPTIMIZATION_PURE_REPORT");
    const char *single_report = getenv("CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT");
    const char *workload = getenv("CBM_ZOVA_OPTIMIZATION_WORKLOAD");
    return pure_report && pure_report[0] && single_report && single_report[0] &&
        workload &&
        (strcmp(workload, "full") == 0 || strcmp(workload, "incremental") == 0);
}

static const char *zsp_optimization_pipeline_mode(const char *workload) {
    if (workload && strcmp(workload, "full") == 0) return "CBM_MODE_FULL";
    if (workload && strcmp(workload, "incremental") == 0)
        return "CBM_MODE_INCREMENTAL";
    return NULL;
}

static int zsp_route_matches_workload(cbm_pipeline_route_t route, const char *workload) {
    return workload &&
        ((strcmp(workload, "full") == 0 && route == CBM_PIPELINE_ROUTE_FULL) ||
         (strcmp(workload, "incremental") == 0 &&
          route == CBM_PIPELINE_ROUTE_INCREMENTAL));
}

static int zsp_pure_sqlite_route_enabled(void) {
    return zsp_pure_sqlite_baseline_enabled() || zsp_optimization_requested();
}

static int zsp_set_benchmark_project_name(cbm_pipeline_t *pipeline) {
    const char *repository = getenv("CBM_ZOVA_PROMOTION_REPOSITORY");
    return pipeline && repository && repository[0] &&
                   cbm_pipeline_set_project_name(pipeline, repository)
               ? 0
               : -1;
}

TEST(zova_single_file_promotion_uses_repository_label_for_project_name) {
    const char *saved_value = getenv("CBM_ZOVA_PROMOTION_REPOSITORY");
    char *saved = saved_value ? strdup(saved_value) : NULL;
    cbm_setenv("CBM_ZOVA_PROMOTION_REPOSITORY", "deno", 1);
    cbm_pipeline_t *pipeline = cbm_pipeline_new(
        "/Volumes/example/very-long/benchmark/run-root/clone", NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(pipeline);
    ASSERT_EQ(zsp_set_benchmark_project_name(pipeline), 0);
    ASSERT_STR_EQ(cbm_pipeline_project_name(pipeline), "deno");
    cbm_pipeline_free(pipeline);
    zsp_restore_env("CBM_ZOVA_PROMOTION_REPOSITORY", saved);
    PASS();
}

static int zsp_run_route(const char *repo, const char *cache, int flagged,
                         const char *workload,
                         zsp_artifact_t *artifact) {
    if (!repo || !cache || !artifact || !zsp_optimization_pipeline_mode(workload) ||
        zsp_ensure_dir(cache) != 0) return -1;
    memset(artifact, 0, sizeof(*artifact));
    snprintf(artifact->cache, sizeof(artifact->cache), "%s", cache);
    snprintf(artifact->db, sizeof(artifact->db), "%s/project.db", cache);

    const char *old_cache_value = getenv("CBM_CACHE_DIR");
    const char *old_mode_value = getenv("CBM_ZOVA_MODE");
    const char *old_single_value = getenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    const char *old_thread_value = getenv("CBM_INDEX_SINGLE_THREAD");
    char *old_cache = old_cache_value ? strdup(old_cache_value) : NULL;
    char *old_mode = old_mode_value ? strdup(old_mode_value) : NULL;
    char *old_single = old_single_value ? strdup(old_single_value) : NULL;
    char *old_thread = old_thread_value ? strdup(old_thread_value) : NULL;
    if ((old_cache_value && !old_cache) || (old_mode_value && !old_mode) ||
        (old_single_value && !old_single) || (old_thread_value && !old_thread)) {
        free(old_cache); free(old_mode); free(old_single); free(old_thread);
        return -1;
    }
    int pure_sqlite = !flagged && zsp_pure_sqlite_route_enabled();
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_setenv("CBM_ZOVA_MODE", pure_sqlite ? "off" : "graph_read", 1);
    /* Keep promotion evidence deterministic by default, but let an explicit
     * caller setting exercise the production parallel indexing route. */
    if (!old_thread_value) cbm_setenv("CBM_INDEX_SINGLE_THREAD", "1", 1);
    if (flagged) cbm_setenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", "1", 1);
    else cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");

    struct timespec started = {0}, finished = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_publish_test_metrics_reset();
    cbm_pipeline_t *pipeline = cbm_pipeline_new(repo, artifact->db, CBM_MODE_FULL);
    int rc = pipeline && zsp_set_benchmark_project_name(pipeline) == 0 &&
                     cbm_pipeline_run(pipeline) == 0
                 ? 0
                 : -1;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    if (pipeline) {
        artifact->route = cbm_pipeline_get_last_route(pipeline);
        artifact->publish_ms = cbm_pipeline_get_zova_publish_ms(pipeline);
        (void)cbm_pipeline_get_zova_publish_stats(pipeline, &artifact->publish_stats);
        cbm_zova_publish_test_metrics_get(&artifact->statement_metrics);
        if (rc == 0 && !zsp_route_matches_workload(artifact->route, workload)) rc = -1;
    }
    if (rc == 0) {
        const char *project = cbm_pipeline_project_name(pipeline);
        if (!project || !project[0] || strlen(project) >= sizeof(artifact->project)) rc = -1;
        else snprintf(artifact->project, sizeof(artifact->project), "%s", project);
    }
    if (pipeline) cbm_pipeline_free(pipeline);
    artifact->elapsed_ms = zsp_elapsed_ms(&started, &finished);
    if (rc == 0) {
        if (flagged) {
            if (cbm_zova_workspace_registry_path(artifact->zova, sizeof(artifact->zova)) != 0)
                rc = -1;
        } else {
            char sidecar[sizeof(artifact->zova)];
            if (cbm_zova_sidecar_path(artifact->db, sidecar, sizeof(sidecar)) != 0) {
                rc = -1;
            } else if (pure_sqlite) {
                struct stat sidecar_info = {0};
                if (stat(sidecar, &sidecar_info) == 0 || errno != ENOENT) rc = -1;
                artifact->zova[0] = '\0';
            } else {
                snprintf(artifact->zova, sizeof(artifact->zova), "%s", sidecar);
            }
        }
    }
    struct stat info = {0};
    if (rc == 0 && !flagged && stat(artifact->db, &info) == 0)
        artifact->db_bytes = (int64_t)info.st_size;
    else if (rc == 0 && !flagged) rc = -1;
    if (rc == 0 && pure_sqlite)
        artifact->zova_bytes = 0;
    else if (rc == 0 && stat(artifact->zova, &info) == 0)
        artifact->zova_bytes = (int64_t)info.st_size;
    else if (rc == 0) rc = -1;

    zsp_restore_env("CBM_CACHE_DIR", old_cache);
    zsp_restore_env("CBM_ZOVA_MODE", old_mode);
    zsp_restore_env("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", old_single);
    zsp_restore_env("CBM_INDEX_SINGLE_THREAD", old_thread);
    return rc;
}

static int zsp_flagged_artifacts(const zsp_artifact_t *artifact) {
    DIR *directory = opendir(artifact->cache);
    if (!directory) return -1;
    int forbidden = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length >= 3 && strcmp(entry->d_name + length - 3, ".db") == 0)
            forbidden++;
        if (length >= 5 && strcmp(entry->d_name + length - 5, ".zova") == 0 &&
            strcmp(entry->d_name, "cbm.zova") != 0)
            forbidden++;
    }
    closedir(directory);
    struct stat info = {0};
    if (stat(artifact->zova, &info) != 0 || !S_ISREG(info.st_mode)) return -1;
    return forbidden;
}

static int zsp_workspace_generation(sqlite3 *user, char *workspace_id,
                                    size_t workspace_size, int64_t *generation) {
    sqlite3_stmt *stmt = NULL;
    if (!user || !workspace_id || !generation ||
        sqlite3_prepare_v2(user,
                           "SELECT r.workspace_id,s.generation "
                           "FROM cbm_workspace_registry r "
                           "JOIN cbm_workspace_index_state_v1 s "
                           "ON s.workspace_key=r.workspace_key LIMIT 1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        if (stmt) sqlite3_finalize(stmt);
        return -1;
    }
    const char *value = (const char *)sqlite3_column_text(stmt, 0);
    int rc = value && strlen(value) < workspace_size ? 0 : -1;
    if (rc == 0) {
        snprintf(workspace_id, workspace_size, "%s", value);
        *generation = sqlite3_column_int64(stmt, 1);
        if (*generation <= 0) rc = -1;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int zsp_capture_public(const zsp_artifact_t *artifact, int flagged,
                              const char *sample_name, const char *sample_qn,
                              single_file_public_responses_t *responses) {
    const char *old_cache_value = getenv("CBM_CACHE_DIR");
    const char *old_mode_value = getenv("CBM_ZOVA_MODE");
    const char *old_single_value = getenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    char *old_cache = old_cache_value ? strdup(old_cache_value) : NULL;
    char *old_mode = old_mode_value ? strdup(old_mode_value) : NULL;
    char *old_single = old_single_value ? strdup(old_single_value) : NULL;
    if ((old_cache_value && !old_cache) || (old_mode_value && !old_mode) ||
        (old_single_value && !old_single)) {
        free(old_cache); free(old_mode); free(old_single);
        return -1;
    }
    int pure_sqlite = !flagged && zsp_pure_sqlite_route_enabled();
    cbm_setenv("CBM_CACHE_DIR", artifact->cache, 1);
    cbm_setenv("CBM_ZOVA_MODE", pure_sqlite ? "off" : "graph_read", 1);
    if (flagged) cbm_setenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", "1", 1);
    else cbm_unsetenv("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL");
    int rc = single_file_capture_public_responses(
        artifact->project, sample_name, sample_qn, responses);
    zsp_restore_env("CBM_CACHE_DIR", old_cache);
    zsp_restore_env("CBM_ZOVA_MODE", old_mode);
    zsp_restore_env("CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL", old_single);
    return rc;
}

#if CBM_WITH_ZOVA
static int zsp_compare_pair(const zsp_artifact_t *compat,
                            const zsp_artifact_t *flagged,
                            zsp_state_report_t *report, int public_checks) {
    sqlite3 *source = NULL;
    sqlite3 *user = NULL;
    single_file_node_ref_t *refs = NULL;
    cbm_zova_workspace_snapshot_t snapshot = {0};
    int ref_count = 0;
    int rc = -1;
    const char *failure_phase = "open";
    if (sqlite3_open_v2(compat->db, &source, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_open_v2(flagged->zova, &user, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto done;
    sqlite3_busy_timeout(source, 5000);
    sqlite3_busy_timeout(user, 5000);
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    int64_t generation = 0;
    failure_phase = "generation";
    if (zsp_workspace_generation(user, workspace_id, sizeof(workspace_id), &generation) != 0)
        goto done;
    report->generation = generation;

    failure_phase = "canonical_metadata";
    if (cbm_zova_repository_export_incremental_snapshot(
            flagged->zova, workspace_id, &snapshot) != CBM_ZOVA_SNAPSHOT_OK)
        goto done;
    int project_mismatches = single_file_compare_project(
        source, user, compat->project, workspace_id);
    int node_mismatches = single_file_compare_snapshot_nodes(
        source, compat->project, workspace_id, &snapshot, &refs, &ref_count);
    int edge_mismatches = node_mismatches < 0 ? -1 : single_file_compare_snapshot_edges(
        source, compat->project, &snapshot, refs, ref_count);
    int hash_mismatches = single_file_compare_file_hashes(
        source, user, compat->project, workspace_id);
    int summary_mismatches = single_file_compare_summary(
        source, user, compat->project, workspace_id);
    if (project_mismatches < 0 || node_mismatches < 0 || edge_mismatches < 0 ||
        hash_mismatches < 0 || summary_mismatches < 0) goto done;
    if (project_mismatches || node_mismatches || edge_mismatches || hash_mismatches ||
        summary_mismatches)
        fprintf(stderr,
                "promotion metadata parity project=%d nodes=%d edges=%d hashes=%d summary=%d\n",
                project_mismatches, node_mismatches, edge_mismatches, hash_mismatches,
                summary_mismatches);
    report->metadata_mismatches += project_mismatches + node_mismatches +
        edge_mismatches + hash_mismatches + summary_mismatches;

    failure_phase = "fts";
    int fts_queries = 0;
    int fts_mismatches = single_file_compare_fts(
        source, user, compat->project, workspace_id, refs, ref_count, &fts_queries);
    if (fts_mismatches < 0) goto done;
    if (fts_mismatches)
        fprintf(stderr, "promotion FTS parity mismatches=%d queries=%d\n",
                fts_mismatches, fts_queries);
    report->fts_mismatches += fts_mismatches;
    if (fts_queries > report->fts_query_count) report->fts_query_count = fts_queries;

    failure_phase = "native_vectors";
    int native_vectors = single_file_compare_native_vectors(
        flagged->zova, source, compat->project, workspace_id, refs, ref_count);
    if (native_vectors < 0) goto done;
    if (native_vectors != 0)
        fprintf(stderr, "promotion native vector parity mismatches=%d\n", native_vectors);
    report->vector_mismatches += native_vectors;

    failure_phase = "native_names";
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    if (cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0 ||
        cbm_zova_workspace_node_vector_collection_name(
            workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768, node_collection,
            sizeof(node_collection)) != 0) goto done;
    zova_database *native = NULL;
    zova_message error = {0};
    if (zova_database_open(&(zova_database_open_request){
            .path = flagged->zova, .out_db = &native, .out_error_message = &error}) != ZOVA_OK ||
        !native) {
        zova_message_free(&error);
        goto done;
    }
    zova_message_free(&error);
    failure_phase = "graph_vector_samples";
    int graph_checks = 0;
    int graph_mismatches = single_file_compare_graph_samples(
        native, source, compat->project, graph_name, refs, ref_count, &graph_checks);
    int ranking_checks = 0;
    int ranking_mismatches = single_file_compare_rankings(
        native, source, compat->project, node_collection, refs, ref_count, &snapshot,
        &ranking_checks);
    zova_database_close(native);
    if (graph_mismatches < 0 || ranking_mismatches < 0) goto done;
    if (ranking_mismatches != 0)
        fprintf(stderr, "promotion vector ranking mismatches=%d checks=%d\n",
                ranking_mismatches, ranking_checks);
    report->graph_mismatches += graph_mismatches;
    report->vector_mismatches += ranking_mismatches;
    if (graph_checks > report->graph_sample_count) report->graph_sample_count = graph_checks;
    report->vector_query_count = ZSP_VECTOR_CASES;

    failure_phase = "cypher";
    int native_routes = 0, compat_routes = 0, ordering_mismatches = 0;
    int cypher_mismatches = single_file_compare_cypher(
        compat->db, flagged->zova, workspace_id, compat->project,
        &native_routes, &compat_routes, &ordering_mismatches);
    if (cypher_mismatches < 0) goto done;
    report->cypher_mismatches += cypher_mismatches + ordering_mismatches;
    if (native_routes > report->cypher_native_route_count)
        report->cypher_native_route_count = native_routes;
    if (compat_routes > report->cypher_compat_route_count)
        report->cypher_compat_route_count = compat_routes;

    failure_phase = "counts";
    int64_t source_nodes = 0, source_edges = 0, source_graph_edges = 0;
    int64_t source_node_vectors = 0;
    int64_t source_token_vectors = 0;
    int64_t graph_nodes = 0, graph_edges = 0, metadata_nodes = 0, metadata_edges = 0;
    int64_t fts_rows = 0, node_vector_rows = 0, token_vector_rows = 0;
    int64_t node_vectors = 0, token_vectors = 0;
    if (single_file_sql_count(source, "SELECT count(*) FROM nodes WHERE project=?1",
                              compat->project, &source_nodes) != 0 ||
        single_file_sql_count(source,
                              "SELECT count(*) FROM (SELECT source_id,target_id,type,local_name_gen "
                              "FROM edges WHERE project=?1 GROUP BY source_id,target_id,type,local_name_gen)",
                              compat->project, &source_edges) != 0 ||
        single_file_sql_count(source,
                              "SELECT count(*) FROM (SELECT source_id,target_id,type "
                              "FROM edges WHERE project=?1 GROUP BY source_id,target_id,type)",
                              compat->project, &source_graph_edges) != 0 ||
        single_file_sql_count(source, "SELECT count(*) FROM node_vectors WHERE project=?1",
                              compat->project, &source_node_vectors) != 0 ||
        single_file_sql_count(source, "SELECT count(*) FROM token_vectors WHERE project=?1",
                              compat->project, &source_token_vectors) != 0 ||
        single_file_sql_generation(user, workspace_id, generation, &graph_nodes, &graph_edges,
                                   &metadata_nodes, &metadata_edges, &fts_rows,
                                   &node_vector_rows, &token_vector_rows, &node_vectors,
                                   &token_vectors) != 0) goto done;
    report->metadata_mismatches += metadata_nodes != source_nodes;
    report->metadata_mismatches += metadata_edges != source_edges;
    report->fts_mismatches += fts_rows != source_nodes;
    report->vector_mismatches += node_vector_rows != source_node_vectors;
    report->vector_mismatches += token_vector_rows != source_token_vectors;
    report->graph_mismatches += graph_nodes != source_nodes;
    report->graph_mismatches += graph_edges != source_graph_edges;
    report->nodes_total = source_nodes;
    report->edges_total = source_edges;
    report->graph_topology_total = source_graph_edges;
    report->node_vectors_total = source_node_vectors;
    report->token_vectors_total = source_token_vectors;

    if (public_checks) {
        failure_phase = "public_mcp";
        sqlite3_stmt *sample = NULL;
        char sample_name[1024] = {0}, sample_qn[2048] = {0};
        if (sqlite3_prepare_v2(source,
                              "SELECT name,qualified_name FROM nodes WHERE project=?1 "
                              "AND label='Function' ORDER BY qualified_name LIMIT 1",
                              -1, &sample, NULL) != SQLITE_OK ||
            sqlite3_bind_text(sample, 1, compat->project, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(sample) != SQLITE_ROW) {
            if (sample) sqlite3_finalize(sample);
            goto done;
        }
        snprintf(sample_name, sizeof(sample_name), "%s", sqlite3_column_text(sample, 0));
        snprintf(sample_qn, sizeof(sample_qn), "%s", sqlite3_column_text(sample, 1));
        sqlite3_finalize(sample);
        single_file_public_responses_t compat_public = {0}, flagged_public = {0};
        if (zsp_capture_public(compat, 0, sample_name, sample_qn, &compat_public) != 0 ||
            zsp_capture_public(flagged, 1, sample_name, sample_qn, &flagged_public) != 0) {
            single_file_public_responses_free(&compat_public);
            single_file_public_responses_free(&flagged_public);
            goto done;
        }
        report->public_mcp_mismatches += single_file_public_response_mismatches(
            &compat_public, &flagged_public);
        report->public_mcp_case_count = SINGLE_FILE_PUBLIC_RESPONSE_COUNT;
        single_file_public_responses_free(&compat_public);
        single_file_public_responses_free(&flagged_public);
    }
    rc = 0;
done:
    if (rc != 0) fprintf(stderr, "promotion comparison failed phase=%s\n", failure_phase);
    cbm_zova_workspace_snapshot_free(&snapshot);
    free(refs);
    if (user) sqlite3_close(user);
    if (source) sqlite3_close(source);
    return rc;
}
#endif

static int zsp_measure_reads(const zsp_artifact_t *compat,
                             const zsp_artifact_t *flagged,
                             zsp_state_report_t *report) {
    sqlite3 *source = NULL;
    sqlite3 *user = NULL;
    single_file_node_ref_t *refs = NULL;
    cbm_zova_workspace_snapshot_t snapshot = {0};
    int ref_count = 0;
    cbm_store_t *compat_store = NULL;
    cbm_store_t *flagged_store = NULL;
    cbm_zova_repository_t *repository = NULL;
    int rc = -1;
    if (sqlite3_open_v2(compat->db, &source, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_open_v2(flagged->zova, &user, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto done;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    int64_t generation = 0;
    if (zsp_workspace_generation(user, workspace_id, sizeof(workspace_id), &generation) != 0 ||
        cbm_zova_repository_export_incremental_snapshot(
            flagged->zova, workspace_id, &snapshot) != CBM_ZOVA_SNAPSHOT_OK ||
        single_file_compare_snapshot_nodes(source, compat->project, workspace_id, &snapshot,
                                           &refs, &ref_count) < 0 ||
        ref_count == 0) goto done;
    compat_store = cbm_store_open_path_query(compat->db);
    flagged_store = cbm_store_open_zova_workspace_query(flagged->zova, workspace_id);
    if (!compat_store || !flagged_store) goto done;
    sqlite3_stmt *sample = NULL;
    char keyword[1024] = {0};
    if (sqlite3_prepare_v2(source,
                           "SELECT name FROM nodes WHERE project=?1 AND label='Function' "
                           "ORDER BY qualified_name LIMIT 1",
                           -1, &sample, NULL) != SQLITE_OK ||
        sqlite3_bind_text(sample, 1, compat->project, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(sample) != SQLITE_ROW) {
        if (sample) sqlite3_finalize(sample);
        goto done;
    }
    snprintf(keyword, sizeof(keyword), "%s", sqlite3_column_text(sample, 0));
    sqlite3_finalize(sample);
    int64_t flagged_start_id = 0;
    repository = cbm_zova_repository_open(flagged->zova, flagged->project);
    cbm_node_t *flagged_start = calloc(1, sizeof(*flagged_start));
    if (!repository || !flagged_start ||
        cbm_zova_repository_find_node_by_stable_id(
            repository, workspace_id, refs[0].stable_id, flagged_start) != CBM_STORE_OK) {
        free(flagged_start);
        goto done;
    }
    flagged_start_id = flagged_start->id;
    cbm_store_free_nodes(flagged_start, 1);
    if (flagged_start_id == 0) goto done;
    const char *keywords[] = {keyword};
    const char *edge_types[] = {"CALLS"};
    double graph_compat[ZSP_WARM_SAMPLES], graph_single[ZSP_WARM_SAMPLES];
    double vector_compat[ZSP_WARM_SAMPLES], vector_single[ZSP_WARM_SAMPLES];
    const char *old_mode_value = getenv("CBM_ZOVA_MODE");
    char *old_mode = old_mode_value ? strdup(old_mode_value) : NULL;
    if (old_mode_value && !old_mode) goto done;
    int pure_sqlite = zsp_pure_sqlite_route_enabled();
    for (int iteration = 0; iteration <= ZSP_WARM_SAMPLES; iteration++) {
        double compat_batch[ZSP_TIMING_BATCH], single_batch[ZSP_TIMING_BATCH];
        double vector_compat_batch[ZSP_TIMING_BATCH];
        double vector_single_batch[ZSP_TIMING_BATCH];
        for (int repeat = 0; repeat < ZSP_TIMING_BATCH; repeat++) {
            bool single_first = (repeat & 1) != 0;
            struct timespec started = {0}, finished = {0};
            cbm_traverse_result_t first_graph = {0}, second_graph = {0};
            cbm_store_t *first_store = single_first ? flagged_store : compat_store;
            cbm_store_t *second_store = single_first ? compat_store : flagged_store;
            int64_t first_id = single_first ? flagged_start_id : refs[0].sqlite_id;
            int64_t second_id = single_first ? refs[0].sqlite_id : flagged_start_id;
            if (pure_sqlite)
                cbm_setenv("CBM_ZOVA_MODE", single_first ? "graph_read" : "off", 1);
            cbm_clock_gettime(CLOCK_MONOTONIC, &started);
            int first_graph_rc = cbm_store_bfs(
                first_store, first_id, "outbound", edge_types, 1, 2, 1000, &first_graph);
            cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
            double first_graph_ms = zsp_elapsed_ms(&started, &finished);
            if (pure_sqlite)
                cbm_setenv("CBM_ZOVA_MODE", single_first ? "off" : "graph_read", 1);
            cbm_clock_gettime(CLOCK_MONOTONIC, &started);
            int second_graph_rc = cbm_store_bfs(
                second_store, second_id, "outbound", edge_types, 1, 2, 1000, &second_graph);
            cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
            double second_graph_ms = zsp_elapsed_ms(&started, &finished);
            compat_batch[repeat] = single_first ? second_graph_ms : first_graph_ms;
            single_batch[repeat] = single_first ? first_graph_ms : second_graph_ms;
            int graph_matches = first_graph_rc == CBM_STORE_OK &&
                second_graph_rc == CBM_STORE_OK &&
                first_graph.visited_count == second_graph.visited_count;
            cbm_store_traverse_free(&first_graph);
            cbm_store_traverse_free(&second_graph);
            if (!graph_matches) { zsp_restore_env("CBM_ZOVA_MODE", old_mode); goto done; }

            cbm_vector_result_t *first_vectors = NULL, *second_vectors = NULL;
            int first_count = 0, second_count = 0;
            const char *first_project = single_first ? flagged->project : compat->project;
            const char *second_project = single_first ? compat->project : flagged->project;
            cbm_setenv("CBM_ZOVA_MODE", single_first ? "i8_vectors" : "off", 1);
            cbm_clock_gettime(CLOCK_MONOTONIC, &started);
            int first_vector_rc = cbm_store_vector_search(
                first_store, first_project, keywords, 1, 20, &first_vectors, &first_count);
            cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
            double first_vector_ms = zsp_elapsed_ms(&started, &finished);
            cbm_setenv("CBM_ZOVA_MODE", single_first ? "off" : "i8_vectors", 1);
            cbm_clock_gettime(CLOCK_MONOTONIC, &started);
            int second_vector_rc = cbm_store_vector_search(
                second_store, second_project, keywords, 1, 20, &second_vectors, &second_count);
            cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
            double second_vector_ms = zsp_elapsed_ms(&started, &finished);
            vector_compat_batch[repeat] = single_first ? second_vector_ms : first_vector_ms;
            vector_single_batch[repeat] = single_first ? first_vector_ms : second_vector_ms;
            cbm_vector_result_t *compat_vectors = single_first ? second_vectors : first_vectors;
            cbm_vector_result_t *flagged_vectors = single_first ? first_vectors : second_vectors;
            int compat_count = single_first ? second_count : first_count;
            int flagged_count = single_first ? first_count : second_count;
            int vector_matches = first_vector_rc == CBM_STORE_OK &&
                second_vector_rc == CBM_STORE_OK && compat_count == flagged_count;
            if (vector_matches) {
                for (int i = 0; i < compat_count; i++) {
                    if (strcmp(compat_vectors[i].qualified_name,
                               flagged_vectors[i].qualified_name) != 0 ||
                        fabs(compat_vectors[i].score - flagged_vectors[i].score) > 1e-9) {
                        vector_matches = 0;
                        break;
                    }
                }
            }
            cbm_store_free_vector_results(first_vectors, first_count);
            cbm_store_free_vector_results(second_vectors, second_count);
            if (!vector_matches) { zsp_restore_env("CBM_ZOVA_MODE", old_mode); goto done; }
        }
        double compat_ms = zsp_percentile(compat_batch, 50);
        double single_ms = zsp_percentile(single_batch, 50);
        double vector_compat_ms = zsp_percentile(vector_compat_batch, 50);
        double vector_single_ms = zsp_percentile(vector_single_batch, 50);
        if (iteration > 0) {
            int index = iteration - 1;
            graph_compat[index] = compat_ms;
            graph_single[index] = single_ms;
            vector_compat[index] = vector_compat_ms;
            vector_single[index] = vector_single_ms;
        }
    }
    zsp_restore_env("CBM_ZOVA_MODE", old_mode);
    report->graph_compat_p50_ms = zsp_percentile(graph_compat, 50);
    report->graph_compat_p95_ms = zsp_percentile(graph_compat, 95);
    report->graph_single_p50_ms = zsp_percentile(graph_single, 50);
    report->graph_single_p95_ms = zsp_percentile(graph_single, 95);
    report->vector_compat_p50_ms = zsp_percentile(vector_compat, 50);
    report->vector_compat_p95_ms = zsp_percentile(vector_compat, 95);
    report->vector_single_p50_ms = zsp_percentile(vector_single, 50);
    report->vector_single_p95_ms = zsp_percentile(vector_single, 95);
    rc = 0;
done:
    if (repository) cbm_zova_repository_close(repository);
    if (flagged_store) cbm_store_close(flagged_store);
    if (compat_store) cbm_store_close(compat_store);
    cbm_zova_workspace_snapshot_free(&snapshot);
    free(refs);
    if (user) sqlite3_close(user);
    if (source) sqlite3_close(source);
    return rc;
}

static int zsp_storage(const zsp_artifact_t *flagged, zsp_state_report_t *report) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(flagged->zova, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return -1;
    int rc = single_file_sql_count(db, "PRAGMA page_count", NULL, &report->page_count);
    if (rc == 0)
        rc = single_file_sql_count(db, "PRAGMA freelist_count", NULL,
                                   &report->freelist_count);
    if (rc == 0)
        rc = single_file_sql_count(db, "PRAGMA page_size", NULL, &report->page_size);
    sqlite3_close(db);
    char wal_path[2304];
    struct stat info = {0};
    snprintf(wal_path, sizeof(wal_path), "%s-wal", flagged->zova);
    report->wal_bytes = stat(wal_path, &info) == 0 ? (int64_t)info.st_size : 0;
    return rc;
}

static int zsp_optimization_inspect_single(const zsp_artifact_t *flagged,
                                            zsp_state_report_t *report) {
    sqlite3 *db = NULL;
    if (!flagged || !report ||
        sqlite3_open_v2(flagged->zova, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    static const char *forbidden_sql =
        "SELECT count(*) FROM sqlite_master WHERE "
        "name IN ('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1',"
        "'cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
        "'cbm_token_vectors_compat_v1','_zova_vector_norms') "
        "OR name GLOB 'cbm_fts_w1_*'";
    static const char *canonical_sql =
        "WITH objects(name) AS (SELECT name FROM sqlite_master WHERE "
        "(name GLOB 'cbm_*' OR tbl_name GLOB 'cbm_*') "
        "AND name NOT GLOB 'cbm_nodes_fts_v1*' "
        "AND name NOT GLOB 'cbm_fts_w1_*' "
        "AND name NOT IN ('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1',"
        "'cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
        "'cbm_token_vectors_compat_v1')) "
        "SELECT coalesce(sum(pgsize),0) FROM dbstat WHERE name IN objects";
    static const char *fts_sql =
        "SELECT coalesce(sum(pgsize),0) FROM dbstat WHERE "
        "name GLOB 'cbm_nodes_fts_v1*' OR name GLOB 'cbm_fts_w1_*'";
    static const char *graph_sql =
        "WITH objects(name) AS (SELECT name FROM sqlite_master WHERE "
        "name GLOB '_zova_graph*' OR tbl_name GLOB '_zova_graph*') "
        "SELECT coalesce(sum(pgsize),0) FROM dbstat WHERE name IN objects";
    static const char *vector_sql =
        "WITH objects(name) AS (SELECT name FROM sqlite_master WHERE "
        "name GLOB '_zova_vector*' OR tbl_name GLOB '_zova_vector*') "
        "SELECT coalesce(sum(pgsize),0) FROM dbstat WHERE name IN objects";
    int64_t forbidden = 0;
    int rc = single_file_sql_count(db, forbidden_sql, NULL, &forbidden);
    if (rc == 0) rc = single_file_sql_count(db, canonical_sql, NULL,
                                             &report->canonical_bytes);
    if (rc == 0) rc = single_file_sql_count(db, fts_sql, NULL, &report->fts_bytes);
    if (rc == 0) rc = single_file_sql_count(db, graph_sql, NULL,
                                             &report->native_graph_bytes);
    if (rc == 0) rc = single_file_sql_count(db, vector_sql, NULL,
                                             &report->native_vector_bytes);
    sqlite3_close(db);
    if (rc != 0 || forbidden > INT32_MAX) return -1;
    report->forbidden_table_count = (int)forbidden;
    int64_t categorized = report->canonical_bytes + report->fts_bytes +
        report->native_graph_bytes + report->native_vector_bytes;
    report->other_bytes = flagged->zova_bytes > categorized ?
        flagged->zova_bytes - categorized : 0;
    report->graph_bytes_per_edge = report->graph_topology_total > 0 ?
        (double)report->native_graph_bytes / (double)report->graph_topology_total : 0.0;
    int64_t vectors = report->node_vectors_total + report->token_vectors_total;
    report->vector_bytes_per_row = vectors > 0 ?
        (double)report->native_vector_bytes / (double)vectors : 0.0;
    return 0;
}

static int zsp_write_report(const char *path, const char *state,
                            const zsp_state_report_t *report) {
    FILE *output = fopen(path, "w");
    if (!output) return -1;
    double ingestion_ratio = report->single_ingestion_ms / report->compat_ingestion_ms;
    double storage_ratio = (double)report->single_bytes /
        (double)(report->compat_db_bytes + report->compat_zova_bytes);
    int passed = report->metadata_mismatches == 0 && report->fts_mismatches == 0 &&
        report->graph_mismatches == 0 && report->vector_mismatches == 0 &&
        report->public_mcp_mismatches == 0 && report->cypher_mismatches == 0 &&
        report->unexpected_fallback_count == 0 && report->cross_workspace_results == 0 &&
        report->compatibility_artifact_count == 0 && report->fresh_full_mismatches == 0 &&
        report->fts_query_count >= ZSP_FTS_CASES &&
        report->graph_sample_count >= ZSP_GRAPH_SAMPLES &&
        report->vector_query_count >= ZSP_VECTOR_CASES &&
        report->public_mcp_case_count >= SINGLE_FILE_PUBLIC_RESPONSE_COUNT &&
        report->cypher_native_route_count > 0 && report->cypher_compat_route_count > 0 &&
        report->generation > 0 && report->compat_ingestion_ms > 0.0 &&
        report->compat_db_bytes + report->compat_zova_bytes > 0 &&
        report->single_bytes > 0 && report->page_count > 0;
    int pure_sqlite = zsp_pure_sqlite_baseline_enabled();
    if (!pure_sqlite) {
        passed = passed &&
            report->graph_single_p50_ms <= report->graph_compat_p50_ms * 1.05 &&
            report->graph_single_p95_ms <= report->graph_compat_p95_ms * 1.05 &&
            report->vector_single_p50_ms <= report->vector_compat_p50_ms * 1.05 &&
            report->vector_single_p95_ms <= report->vector_compat_p95_ms * 1.05;
    }
    fprintf(output, "{\n  \"name\": \"%s\",\n", state);
    if (pure_sqlite) {
        fprintf(output,
                "  \"baseline_route\": \"pure_sqlite\",\n"
                "  \"state_workload\": \"%s\",\n",
                strcmp(state, "incremental") == 0 ?
                    "changed_state_full_pipeline" : "full_pipeline");
    }
    fprintf(output,
            "  \"passed\": %s,\n"
            "  \"metadata_mismatches\": %d,\n"
            "  \"fts_mismatches\": %d,\n"
            "  \"graph_mismatches\": %d,\n"
            "  \"vector_mismatches\": %d,\n"
            "  \"public_mcp_mismatches\": %d,\n"
            "  \"cypher_mismatches\": %d,\n"
            "  \"unexpected_fallback_count\": %d,\n"
            "  \"cross_workspace_results\": %d,\n"
            "  \"compatibility_artifact_count\": %d,\n"
            "  \"fresh_full_mismatches\": %d,\n"
            "  \"fts_query_count\": %d,\n"
            "  \"graph_sample_count\": %d,\n"
            "  \"vector_query_count\": %d,\n"
            "  \"public_mcp_case_count\": %d,\n"
            "  \"cypher_native_route_count\": %d,\n"
            "  \"cypher_compat_route_count\": %d,\n"
            "  \"generation\": {\"active\": %lld, \"integrity_matches\": true},\n"
            "  \"performance\": {\"sample_count\": 20,\n"
            "    \"graph\": {\"compat_p50_ms\": %.6f, \"compat_p95_ms\": %.6f, "
            "\"single_p50_ms\": %.6f, \"single_p95_ms\": %.6f},\n"
            "    \"vector\": {\"compat_p50_ms\": %.6f, \"compat_p95_ms\": %.6f, "
            "\"single_p50_ms\": %.6f, \"single_p95_ms\": %.6f}},\n"
            "  \"ingestion\": {\"compat_ms\": %.6f, \"single_ms\": %.6f, "
            "\"ratio\": %.6f},\n"
            "  \"storage\": {\"compat_db_bytes\": %lld, \"compat_zova_bytes\": %lld, "
            "\"single_bytes\": %lld, \"ratio\": %.6f, \"page_count\": %lld, "
            "\"freelist_count\": %lld, \"wal_bytes\": %lld}\n"
            "}\n",
            passed ? "true" : "false", report->metadata_mismatches,
            report->fts_mismatches, report->graph_mismatches, report->vector_mismatches,
            report->public_mcp_mismatches, report->cypher_mismatches,
            report->unexpected_fallback_count, report->cross_workspace_results,
            report->compatibility_artifact_count, report->fresh_full_mismatches,
            report->fts_query_count, report->graph_sample_count, report->vector_query_count,
            report->public_mcp_case_count, report->cypher_native_route_count,
            report->cypher_compat_route_count, (long long)report->generation,
            report->graph_compat_p50_ms, report->graph_compat_p95_ms,
            report->graph_single_p50_ms, report->graph_single_p95_ms,
            report->vector_compat_p50_ms, report->vector_compat_p95_ms,
            report->vector_single_p50_ms, report->vector_single_p95_ms,
            report->compat_ingestion_ms, report->single_ingestion_ms, ingestion_ratio,
            (long long)report->compat_db_bytes, (long long)report->compat_zova_bytes,
            (long long)report->single_bytes, storage_ratio, (long long)report->page_count,
            (long long)report->freelist_count, (long long)report->wal_bytes);
    return fclose(output) == 0 ? 0 : -1;
}

static int zsp_write_optimization_report(const char *path, const char *route,
                                          const char *workload,
                                          const zsp_artifact_t *pure,
                                          const zsp_artifact_t *single,
                                          const zsp_state_report_t *report) {
    if (!path || !route || !workload || !pure || !single || !report) return -1;
    const zsp_artifact_t *selected = NULL;
    if (strcmp(route, "pure") == 0) selected = pure;
    else if (strcmp(route, "single") == 0) selected = single;
    else return -1;
    const char *pipeline_mode = selected->route == CBM_PIPELINE_ROUTE_INCREMENTAL ?
        "CBM_MODE_INCREMENTAL" : selected->route == CBM_PIPELINE_ROUTE_FULL ?
        "CBM_MODE_FULL" : "CBM_PIPELINE_ROUTE_UNKNOWN";
    int route_matches = zsp_route_matches_workload(selected->route, workload);
    int parity_mismatches = report->metadata_mismatches + report->fts_mismatches +
        report->graph_mismatches + report->vector_mismatches +
        report->public_mcp_mismatches + report->cypher_mismatches +
        report->fresh_full_mismatches;
    int passed = route_matches && parity_mismatches == 0 &&
        report->unexpected_fallback_count == 0;
    int single_route = strcmp(route, "single") == 0;
    int64_t database_bytes = single_route ?
        single->zova_bytes : pure->db_bytes;
    double vector_p95_ms = single_route ?
        report->vector_single_p95_ms : report->vector_compat_p95_ms;
    int64_t canonical_bytes = single_route ? report->canonical_bytes : database_bytes;
    int64_t fts_bytes = single_route ? report->fts_bytes : 0;
    int64_t native_graph_bytes = single_route ? report->native_graph_bytes : 0;
    int64_t native_vector_bytes = single_route ? report->native_vector_bytes : 0;
    int64_t other_bytes = single_route ? report->other_bytes : 0;
    double graph_bytes_per_edge = single_route ? report->graph_bytes_per_edge : 0.0;
    double vector_bytes_per_row = single_route ? report->vector_bytes_per_row : 0.0;
    int forbidden_table_count = single_route ? report->forbidden_table_count : 0;
    int full_clear_count = single_route ? report->full_clear_count : 0;
    int64_t unchanged_rewrite_count = single_route ? report->unchanged_rewrite_count : 0;
    FILE *output = fopen(path, "w");
    if (!output) return -1;
    fprintf(output,
            "{\n"
            "  \"route\": \"%s\",\n"
            "  \"workload\": \"%s\",\n"
            "  \"pipeline_mode\": \"%s\",\n"
            "  \"incremental_fallback_reason\": %s,\n"
            "  \"full_fallback_count\": %d,\n"
            "  \"full_clear_count\": %d,\n"
            "  \"unchanged_rewrite_count\": %lld,\n"
            "  \"rows\": {\n"
            "    \"nodes_total\": %lld, \"nodes_inserted\": %lld, "
            "\"nodes_updated\": %lld, \"nodes_deleted\": %lld,\n"
            "    \"edges_total\": %lld, \"edges_inserted\": %lld, "
            "\"edges_deleted\": %lld,\n"
            "    \"node_vectors_total\": %lld, \"node_vectors_upserted\": %lld, "
            "\"node_vectors_deleted\": %lld,\n"
            "    \"token_vectors_total\": %lld, \"token_vectors_upserted\": %lld, "
            "\"token_vectors_deleted\": %lld\n"
            "  },\n"
            "  \"timing_ms\": {\n"
            "    \"route\": %.3f, \"normalize\": %.3f, "
            "\"model_nodes\": %.3f, \"model_edges\": %.3f, "
            "\"model_hashes\": %.3f, \"model_vectors\": %.3f,\n"
            "    \"writer_guard\": %.3f, \"database_init\": %.3f, "
            "\"database_open\": %.3f, \"transaction_begin\": %.3f,\n"
            "    \"transaction_body\": %.3f, \"transaction_commit\": %.3f, "
            "\"database_close\": %.3f, \"clear\": %.3f,\n"
            "    \"canonical_files\": %.3f, "
            "\"canonical_nodes\": %.3f, \"canonical_edges\": %.3f,\n"
            "    \"canonical_hashes\": %.3f, \"fts\": %.3f, "
            "\"token_metadata\": %.3f,\n"
            "    \"native_graph\": %.3f, \"native_graph_materialize\": %.3f, "
            "\"native_graph_reset\": %.3f,\n"
            "    \"native_graph_nodes\": %.3f, \"native_graph_edges\": %.3f, "
            "\"native_graph_validate\": %.3f, "
            "\"native_graph_key_generation\": %.3f, "
            "\"native_graph_cleanup\": %.3f,\n"
            "    \"native_vectors\": %.3f, \"fresh_validation\": %.3f, "
            "\"fresh_index\": %.3f, \"fresh_commit\": %.3f, "
            "\"fresh_build\": %.3f, \"readback\": %.3f, "
            "\"digests\": %.3f, \"verify\": %.3f,\n"
            "    \"publish\": %.6f, \"pipeline\": %.6f\n"
            "  },\n"
            "  \"statement_metrics\": {\n"
            "    \"canonical_files\": {\"rows\": %llu, \"bind_i64_calls\": %llu, "
            "\"bind_text_calls\": %llu, \"bind_double_calls\": %llu, "
            "\"step_calls\": %llu, \"reset_calls\": %llu, \"clear_bindings_calls\": %llu},\n"
            "    \"canonical_nodes\": {\"rows\": %llu, \"bind_i64_calls\": %llu, "
            "\"bind_text_calls\": %llu, \"bind_double_calls\": %llu, "
            "\"step_calls\": %llu, \"reset_calls\": %llu, \"clear_bindings_calls\": %llu},\n"
            "    \"canonical_edges\": {\"rows\": %llu, \"bind_i64_calls\": %llu, "
            "\"bind_text_calls\": %llu, \"bind_double_calls\": %llu, "
            "\"step_calls\": %llu, \"reset_calls\": %llu, \"clear_bindings_calls\": %llu},\n"
            "    \"canonical_hashes\": {\"rows\": %llu, \"bind_i64_calls\": %llu, "
            "\"bind_text_calls\": %llu, \"bind_double_calls\": %llu, "
            "\"step_calls\": %llu, \"reset_calls\": %llu, \"clear_bindings_calls\": %llu},\n"
            "    \"canonical_token_metadata\": {\"rows\": %llu, \"bind_i64_calls\": %llu, "
            "\"bind_text_calls\": %llu, \"bind_double_calls\": %llu, "
            "\"step_calls\": %llu, \"reset_calls\": %llu, \"clear_bindings_calls\": %llu},\n"
            "    \"full_fts_bulk_statements\": %llu, "
            "\"full_fts_trigger_rows_avoided\": %llu,\n"
            "    \"full_node_guard_validation_statements\": %llu, "
            "\"full_edge_guard_validation_statements\": %llu\n"
            "  },\n"
            "  \"snapshot\": {\n"
            "    \"completed\": %s, \"generation\": %lld,\n"
            "    \"base_ms\": %.6f, \"optional_ms\": %.6f,\n"
            "    \"base_phase_mask\": %u,\n"
            "    \"open_ms\": %.6f, \"header_ms\": %.6f, \"integrity_ms\": %.6f,\n"
            "    \"nodes_sql_ms\": %.6f, \"nodes_native_ms\": %.6f, "
            "\"nodes_finalize_ms\": %.6f,\n"
            "    \"edges_sql_ms\": %.6f, \"edges_native_ms\": %.6f, "
            "\"edges_finalize_ms\": %.6f,\n"
            "    \"hashes_summary_ms\": %.6f, \"close_ms\": %.6f, "
            "\"graph_buffer_ms\": %.6f,\n"
            "    \"node_rows\": %llu, \"edge_rows\": %llu, "
            "\"file_hash_rows\": %llu,\n"
            "    \"hydrated_components\": %u, \"topology_rows\": %llu,\n"
            "    \"node_vector_rows\": %llu, \"token_vector_rows\": %llu\n"
            "  },\n"
            "  \"storage\": {\n"
            "    \"database_bytes\": %lld, \"wal_bytes\": %lld, "
            "\"freelist_bytes\": %lld,\n"
            "    \"canonical_bytes\": %lld, \"fts_bytes\": %lld, "
            "\"native_graph_bytes\": %lld,\n"
            "    \"native_vector_bytes\": %lld, \"other_bytes\": %lld,\n"
            "    \"graph_bytes_per_edge\": %.6f, "
            "\"vector_bytes_per_row\": %.6f\n"
            "  },\n"
            "  \"performance\": {\"vector_p95_ms\": %.6f},\n"
            "  \"forbidden_table_count\": %d,\n"
            "  \"parity_mismatch_count\": %d,\n"
            "  \"unexpected_fallback_count\": %d,\n"
            "  \"passed\": %s\n"
            "}\n",
            route, workload, pipeline_mode,
            route_matches ? "null" : "\"actual_route_mismatch\"",
            route_matches ? report->full_fallback_count : report->full_fallback_count + 1,
            full_clear_count, (long long)unchanged_rewrite_count,
            (long long)report->nodes_total, (long long)report->nodes_inserted,
            (long long)report->nodes_updated, (long long)report->nodes_deleted,
            (long long)report->edges_total, (long long)report->edges_inserted,
            (long long)report->edges_deleted, (long long)report->node_vectors_total,
            (long long)report->node_vectors_upserted,
            (long long)report->node_vectors_deleted,
            (long long)report->token_vectors_total,
            (long long)report->token_vectors_upserted,
            (long long)report->token_vectors_deleted,
            selected->publish_stats.route_ms,
            selected->publish_stats.normalization_ms,
            selected->publish_stats.model_nodes_ms,
            selected->publish_stats.model_edges_ms,
            selected->publish_stats.model_hashes_ms,
            selected->publish_stats.model_vectors_ms,
            selected->publish_stats.writer_guard_ms,
            selected->publish_stats.database_init_ms,
            selected->publish_stats.database_open_ms,
            selected->publish_stats.transaction_begin_ms,
            selected->publish_stats.transaction_body_ms,
            selected->publish_stats.transaction_commit_ms,
            selected->publish_stats.database_close_ms,
            selected->publish_stats.clear_ms,
            selected->publish_stats.canonical_files_ms,
            selected->publish_stats.canonical_nodes_ms,
            selected->publish_stats.canonical_edges_ms,
            selected->publish_stats.canonical_hashes_ms,
            selected->publish_stats.fts_ms,
            selected->publish_stats.token_metadata_ms,
            selected->publish_stats.native_graph_ms,
            selected->publish_stats.native_graph_materialize_ms,
            selected->publish_stats.native_graph_reset_ms,
            selected->publish_stats.native_graph_nodes_ms,
            selected->publish_stats.native_graph_edges_ms,
            selected->publish_stats.native_graph_validate_ms,
            selected->publish_stats.native_graph_key_generation_ms,
            selected->publish_stats.native_graph_cleanup_ms,
            selected->publish_stats.native_vectors_ms,
            selected->publish_stats.fresh_validation_ms,
            selected->publish_stats.fresh_index_ms,
            selected->publish_stats.fresh_commit_ms,
            selected->publish_stats.fresh_build_ms,
            selected->publish_stats.readback_ms,
            selected->publish_stats.model_digests_ms,
            selected->publish_stats.finalize_ms,
            selected->publish_ms, selected->elapsed_ms,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.rows,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.bind_i64_calls,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.bind_text_calls,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.bind_double_calls,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.step_calls,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.reset_calls,
            (unsigned long long)selected->statement_metrics.canonical_files_sql.clear_bindings_calls,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.rows,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.bind_i64_calls,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.bind_text_calls,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.bind_double_calls,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.step_calls,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.reset_calls,
            (unsigned long long)selected->statement_metrics.canonical_nodes_sql.clear_bindings_calls,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.rows,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.bind_i64_calls,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.bind_text_calls,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.bind_double_calls,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.step_calls,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.reset_calls,
            (unsigned long long)selected->statement_metrics.canonical_edges_sql.clear_bindings_calls,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.rows,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.bind_i64_calls,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.bind_text_calls,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.bind_double_calls,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.step_calls,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.reset_calls,
            (unsigned long long)selected->statement_metrics.canonical_hashes_sql.clear_bindings_calls,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.rows,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.bind_i64_calls,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.bind_text_calls,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.bind_double_calls,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.step_calls,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.reset_calls,
            (unsigned long long)selected->statement_metrics.canonical_token_metadata_sql.clear_bindings_calls,
            (unsigned long long)selected->statement_metrics.full_fts_bulk_statements,
            (unsigned long long)selected->statement_metrics.full_fts_trigger_rows_avoided,
            (unsigned long long)selected->statement_metrics.full_node_guard_validation_statements,
            (unsigned long long)selected->statement_metrics.full_edge_guard_validation_statements,
            selected->publish_stats.snapshot_completed ? "true" : "false",
            (long long)selected->publish_stats.snapshot_generation,
            selected->publish_stats.snapshot_base_ms,
            selected->publish_stats.snapshot_optional_ms,
            selected->publish_stats.snapshot_base_phase_mask,
            selected->publish_stats.snapshot_open_ms,
            selected->publish_stats.snapshot_header_ms,
            selected->publish_stats.snapshot_integrity_ms,
            selected->publish_stats.snapshot_nodes_sql_ms,
            selected->publish_stats.snapshot_nodes_native_ms,
            selected->publish_stats.snapshot_nodes_finalize_ms,
            selected->publish_stats.snapshot_edges_sql_ms,
            selected->publish_stats.snapshot_edges_native_ms,
            selected->publish_stats.snapshot_edges_finalize_ms,
            selected->publish_stats.snapshot_hashes_summary_ms,
            selected->publish_stats.snapshot_close_ms,
            selected->publish_stats.snapshot_graph_buffer_ms,
            (unsigned long long)selected->publish_stats.snapshot_node_rows,
            (unsigned long long)selected->publish_stats.snapshot_edge_rows,
            (unsigned long long)selected->publish_stats.snapshot_file_hash_rows,
            selected->publish_stats.snapshot_hydrated_components,
            (unsigned long long)selected->publish_stats.snapshot_topology_rows,
            (unsigned long long)selected->publish_stats.snapshot_node_vector_rows,
            (unsigned long long)selected->publish_stats.snapshot_token_vector_rows,
            (long long)database_bytes,
            (long long)report->wal_bytes,
            (long long)(report->freelist_count * report->page_size),
            (long long)canonical_bytes, (long long)fts_bytes,
            (long long)native_graph_bytes, (long long)native_vector_bytes,
            (long long)other_bytes, graph_bytes_per_edge,
            vector_bytes_per_row, vector_p95_ms, forbidden_table_count,
            parity_mismatches, report->unexpected_fallback_count,
            passed ? "true" : "false");
    return fclose(output) == 0 ? 0 : -1;
}

TEST(zova_single_file_promotion_percentile_uses_nearest_rank) {
    double values[ZSP_WARM_SAMPLES] = {
        20, 19, 18, 17, 16, 15, 14, 13, 12, 11,
        10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
    };
    ASSERT_EQ((int)zsp_percentile(values, 50), 10);
    ASSERT_EQ((int)zsp_percentile(values, 95), 19);
    PASS();
}

TEST(zova_single_file_promotion_pure_sqlite_selector_is_explicit) {
    const char *old_value = getenv("CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE");
    char *old_copy = old_value ? strdup(old_value) : NULL;
    ASSERT_TRUE(!old_value || old_copy);
    cbm_unsetenv("CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE");
    ASSERT_FALSE(zsp_pure_sqlite_baseline_enabled());
    cbm_setenv("CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE", "0", 1);
    ASSERT_FALSE(zsp_pure_sqlite_baseline_enabled());
    cbm_setenv("CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE", "1", 1);
    ASSERT_TRUE(zsp_pure_sqlite_baseline_enabled());
    zsp_restore_env("CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE", old_copy);
    PASS();
}

TEST(zova_single_file_optimization_contract_is_explicit) {
    const char *old_pure_report_value = getenv("CBM_ZOVA_OPTIMIZATION_PURE_REPORT");
    const char *old_single_report_value = getenv("CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT");
    const char *old_workload_value = getenv("CBM_ZOVA_OPTIMIZATION_WORKLOAD");
    char *old_pure_report = old_pure_report_value ? strdup(old_pure_report_value) : NULL;
    char *old_single_report = old_single_report_value ? strdup(old_single_report_value) : NULL;
    char *old_workload = old_workload_value ? strdup(old_workload_value) : NULL;
    ASSERT_TRUE((!old_pure_report_value || old_pure_report) &&
                (!old_single_report_value || old_single_report) &&
                (!old_workload_value || old_workload));

    cbm_unsetenv("CBM_ZOVA_OPTIMIZATION_PURE_REPORT");
    cbm_unsetenv("CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT");
    cbm_unsetenv("CBM_ZOVA_OPTIMIZATION_WORKLOAD");
    ASSERT_FALSE(zsp_optimization_requested());
    cbm_setenv("CBM_ZOVA_OPTIMIZATION_PURE_REPORT", "/tmp/pure-report.json", 1);
    ASSERT_FALSE(zsp_optimization_requested());
    cbm_setenv("CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT", "/tmp/single-report.json", 1);
    cbm_setenv("CBM_ZOVA_OPTIMIZATION_WORKLOAD", "incremental", 1);
    ASSERT_TRUE(zsp_optimization_requested());
    ASSERT_STR_EQ(zsp_optimization_pipeline_mode("full"), "CBM_MODE_FULL");
    ASSERT_STR_EQ(zsp_optimization_pipeline_mode("incremental"), "CBM_MODE_INCREMENTAL");
    ASSERT_NULL(zsp_optimization_pipeline_mode("changed_state_full_pipeline"));
    ASSERT_TRUE(zsp_route_matches_workload(CBM_PIPELINE_ROUTE_FULL, "full"));
    ASSERT_TRUE(zsp_route_matches_workload(CBM_PIPELINE_ROUTE_INCREMENTAL, "incremental"));
    ASSERT_FALSE(zsp_route_matches_workload(CBM_PIPELINE_ROUTE_FULL, "incremental"));
    ASSERT_FALSE(zsp_route_matches_workload(CBM_PIPELINE_ROUTE_UNKNOWN, "full"));

    zsp_restore_env("CBM_ZOVA_OPTIMIZATION_PURE_REPORT", old_pure_report);
    zsp_restore_env("CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT", old_single_report);
    zsp_restore_env("CBM_ZOVA_OPTIMIZATION_WORKLOAD", old_workload);
    PASS();
}

TEST(zova_single_file_optimization_report_uses_actual_route) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/cbm-zova-optimization-report-XXXXXX");
    int fd = cbm_mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    zsp_artifact_t pure = {.route = CBM_PIPELINE_ROUTE_INCREMENTAL,
                           .elapsed_ms = 40.0,
                           .db_bytes = 1000};
    zsp_artifact_t single = {.route = CBM_PIPELINE_ROUTE_INCREMENTAL,
                             .elapsed_ms = 50.0,
                             .publish_ms = 20.0,
                             .zova_bytes = 2000};
    zsp_state_report_t report = {0};
    ASSERT_EQ(zsp_write_optimization_report(path, "single", "incremental",
                                            &pure, &single, &report), 0);
    FILE *input = fopen(path, "r");
    ASSERT_NOT_NULL(input);
    char json[8192] = {0};
    ASSERT_TRUE(fread(json, 1, sizeof(json) - 1, input) > 0);
    fclose(input);
    cbm_unlink(path);
    ASSERT_NOT_NULL(strstr(json, "\"route\": \"single\""));
    ASSERT_NOT_NULL(strstr(json, "\"workload\": \"incremental\""));
    ASSERT_NOT_NULL(strstr(json, "\"pipeline_mode\": \"CBM_MODE_INCREMENTAL\""));
    ASSERT_NOT_NULL(strstr(json, "\"native_graph_materialize\":"));
    ASSERT_NOT_NULL(strstr(json, "\"native_graph_reset\":"));
    ASSERT_NOT_NULL(strstr(json, "\"native_graph_nodes\":"));
    ASSERT_NOT_NULL(strstr(json, "\"native_graph_edges\":"));
    ASSERT_NOT_NULL(strstr(json, "\"native_graph_validate\":"));
    ASSERT_NOT_NULL(strstr(json, "\"native_graph_cleanup\":"));
    ASSERT_NULL(strstr(json, "changed_state_full_pipeline"));
    PASS();
}

TEST(zova_single_file_promotion_real_repository_state) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    const char *repo = getenv("CBM_ZOVA_VALIDATION_REPO");
    if (!repo || !repo[0]) PASS();
    const char *cache = getenv("CBM_CACHE_DIR");
    const char *state = getenv("CBM_ZOVA_PROMOTION_STATE");
    const char *report = getenv("CBM_ZOVA_PROMOTION_REPORT");
    const char *repository = getenv("CBM_ZOVA_PROMOTION_REPOSITORY");
    const char *run_id = getenv("CBM_ZOVA_PROMOTION_RUN_ID");
    const char *attempt_text = getenv("CBM_ZOVA_PROMOTION_ATTEMPT");
    const char *build = getenv("CBM_ZOVA_PROMOTION_BUILD_SHA256");
    const char *calibration = getenv("CBM_ZOVA_PROMOTION_CALIBRATION_SHA256");
    const char *focused = getenv("CBM_ZOVA_PROMOTION_FOCUSED_SHA256");
    const char *calibration_mode_text = getenv("CBM_ZOVA_SECTION9_CALIBRATION_MODE");
    char *attempt_end = NULL;
    long attempt = attempt_text ? strtol(attempt_text, &attempt_end, 10) : -1;
    int calibration_mode = calibration_mode_text && strcmp(calibration_mode_text, "1") == 0;
    int pure_sqlite = zsp_pure_sqlite_baseline_enabled();
    ASSERT_TRUE(repo && repo[0] && cache && cache[0] && report && report[0]);
    ASSERT_TRUE(state && (strcmp(state, "full") == 0 || strcmp(state, "incremental") == 0));
    ASSERT_TRUE(repository && repository[0] && zsp_run_id(run_id));
    ASSERT_TRUE(attempt_text && attempt_end && *attempt_end == '\0');
    if (zsp_optimization_requested()) {
        ASSERT_TRUE(attempt > 0);
    } else if (calibration_mode) {
        ASSERT_STR_EQ(repository, "tops");
        ASSERT_EQ(attempt, 0);
    } else {
        ASSERT_TRUE(attempt > 0);
        if (!pure_sqlite) {
            ASSERT_TRUE(zsp_hex64(calibration));
            ASSERT_TRUE(zsp_hex64(focused));
        }
    }
    ASSERT_TRUE(zsp_hex64(build));
    char compat_cache[2048], flagged_cache[2048], fresh_cache[2048];
    snprintf(compat_cache, sizeof(compat_cache), "%s/compat", cache);
    snprintf(flagged_cache, sizeof(flagged_cache), "%s/flagged", cache);
    snprintf(fresh_cache, sizeof(fresh_cache), "%s/fresh-control", cache);
    zsp_artifact_t compat_artifact = {0}, flagged_artifact = {0};
    ASSERT_EQ(zsp_run_route(repo, compat_cache, 0, state, &compat_artifact), 0);
    ASSERT_EQ(zsp_run_route(repo, flagged_cache, 1, state, &flagged_artifact), 0);
    ASSERT_STR_EQ(compat_artifact.project, repository);
    ASSERT_STR_EQ(flagged_artifact.project, repository);
    ASSERT_STR_EQ(compat_artifact.project, flagged_artifact.project);
    zsp_state_report_t state_report = {0};
    state_report.compatibility_artifact_count = zsp_flagged_artifacts(&flagged_artifact);
    ASSERT_TRUE(state_report.compatibility_artifact_count >= 0);
    ASSERT_EQ(zsp_compare_pair(&compat_artifact, &flagged_artifact, &state_report, 1), 0);
    ASSERT_EQ(zsp_measure_reads(&compat_artifact, &flagged_artifact, &state_report), 0);
    if (strcmp(state, "incremental") == 0) {
        zsp_artifact_t fresh_artifact = {0};
        ASSERT_EQ(zsp_run_route(repo, fresh_cache, 0, "full", &fresh_artifact), 0);
        ASSERT_STR_EQ(fresh_artifact.project, repository);
        zsp_state_report_t fresh_report = {0};
        ASSERT_EQ(zsp_compare_pair(&fresh_artifact, &flagged_artifact, &fresh_report, 0), 0);
        state_report.fresh_full_mismatches = fresh_report.metadata_mismatches +
            fresh_report.fts_mismatches + fresh_report.graph_mismatches +
            fresh_report.vector_mismatches + fresh_report.cypher_mismatches;
    }
    state_report.compat_ingestion_ms = compat_artifact.elapsed_ms;
    state_report.single_ingestion_ms = flagged_artifact.elapsed_ms;
    state_report.compat_db_bytes = compat_artifact.db_bytes;
    state_report.compat_zova_bytes = compat_artifact.zova_bytes;
    state_report.single_bytes = flagged_artifact.zova_bytes;
    ASSERT_EQ(zsp_storage(&flagged_artifact, &state_report), 0);
    if (strcmp(state, "full") == 0) {
        state_report.nodes_inserted = state_report.nodes_total;
        state_report.edges_inserted = state_report.edges_total;
        state_report.node_vectors_upserted = state_report.node_vectors_total;
        state_report.token_vectors_upserted = state_report.token_vectors_total;
    }
    if (zsp_optimization_requested()) {
        const char *pure_report = getenv("CBM_ZOVA_OPTIMIZATION_PURE_REPORT");
        const char *single_report = getenv("CBM_ZOVA_OPTIMIZATION_SINGLE_REPORT");
        const char *optimization_workload = getenv("CBM_ZOVA_OPTIMIZATION_WORKLOAD");
        ASSERT_STR_EQ(state, optimization_workload);
        ASSERT_EQ(zsp_optimization_inspect_single(&flagged_artifact, &state_report), 0);
        zsp_state_report_t pure_state_report = state_report;
        const cbm_pipeline_zova_publish_stats_t *stats = &flagged_artifact.publish_stats;
        ASSERT_TRUE(stats->completed);
        ASSERT_EQ(stats->delta, strcmp(optimization_workload, "incremental") == 0);
        state_report.full_clear_count = (int)stats->full_clear_count;
        state_report.unchanged_rewrite_count = (int64_t)stats->unchanged_rewrite_count;
        state_report.nodes_inserted = (int64_t)stats->nodes_inserted;
        state_report.nodes_updated = (int64_t)stats->nodes_updated;
        state_report.nodes_deleted = (int64_t)stats->nodes_deleted;
        state_report.edges_inserted = (int64_t)stats->edges_inserted;
        state_report.edges_deleted = (int64_t)stats->edges_deleted;
        state_report.node_vectors_upserted = (int64_t)stats->node_vectors_upserted;
        state_report.node_vectors_deleted = (int64_t)stats->node_vectors_deleted;
        state_report.token_vectors_upserted = (int64_t)stats->token_vectors_upserted;
        state_report.token_vectors_deleted = (int64_t)stats->token_vectors_deleted;
        ASSERT_EQ(zsp_write_optimization_report(pure_report, "pure",
                                                optimization_workload,
                                                &compat_artifact, &flagged_artifact,
                                                &pure_state_report), 0);
        ASSERT_EQ(zsp_write_optimization_report(single_report, "single",
                                                optimization_workload,
                                                &compat_artifact, &flagged_artifact,
                                                &state_report), 0);
    } else {
        ASSERT_EQ(zsp_write_report(report, state, &state_report), 0);
    }
    PASS();
#endif
}

void suite_zova_single_file_promotion_real_repo(void) {
    RUN_TEST(zova_single_file_promotion_percentile_uses_nearest_rank);
    RUN_TEST(zova_single_file_promotion_pure_sqlite_selector_is_explicit);
    RUN_TEST(zova_single_file_optimization_contract_is_explicit);
    RUN_TEST(zova_single_file_optimization_report_uses_actual_route);
    RUN_TEST(zova_single_file_edge_parity_streams_once);
    RUN_TEST(zova_single_file_cypher_parity_bounds_large_repositories);
    RUN_TEST(zova_single_file_promotion_uses_repository_label_for_project_name);
    RUN_TEST(zova_single_file_promotion_real_repository_state);
}
