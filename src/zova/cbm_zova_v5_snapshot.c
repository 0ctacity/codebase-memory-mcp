#include "zova/cbm_zova_v5_snapshot.h"
#include "foundation/platform.h"

#include <sqlite3.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *workspace_id;
    int64_t active_generation;
    cbm_zova_workspace_generation_input_t input;
    char *root_path;
    char *project;
    char *indexed_at;
    char *model_fingerprint;
    CBMDumpNode *nodes;
    char **stable_node_ids;
    CBMDumpEdge *edges;
    CBMDumpVector *node_vectors;
    CBMDumpTokenVec *token_vectors;
    cbm_zova_file_hash_input_t *file_hashes;
    char *summary;
    char *summary_source_hash;
    char *summary_created_at;
    char *summary_updated_at;
} v5_workspace_t;

struct cbm_zova_v5_snapshot {
    sqlite3 *db;
    v5_workspace_t *workspaces;
    int workspace_count;
};

static char *v5_text(sqlite3_stmt *stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) return NULL;
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? strdup((const char *)value) : NULL;
}

static uint8_t *v5_blob(sqlite3_stmt *stmt, int column, int expected_length) {
    int length = sqlite3_column_bytes(stmt, column);
    const void *value = sqlite3_column_blob(stmt, column);
    if (!value || length != expected_length || length <= 0) return NULL;
    uint8_t *copy = malloc((size_t)length);
    if (copy) memcpy(copy, value, (size_t)length);
    return copy;
}

static int v5_scalar(sqlite3 *db, const char *sql, const char *text,
                     int64_t *out_value) {
    sqlite3_stmt *stmt = NULL;
    if (!db || !sql || !out_value || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = 0;
    if (text && sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC) != SQLITE_OK) rc = -1;
    if (rc == 0 && sqlite3_step(stmt) != SQLITE_ROW) rc = -1;
    if (rc == 0) *out_value = sqlite3_column_int64(stmt, 0);
    if (rc == 0 && sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int v5_count(sqlite3 *db, const char *table, const char *workspace_id,
                    int *out_count) {
    char sql[256];
    if (!table || !workspace_id || !out_count ||
        snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s WHERE workspace_id=?1", table) >=
            (int)sizeof(sql))
        return -1;
    int64_t count = 0;
    if (v5_scalar(db, sql, workspace_id, &count) != 0 || count < 0 || count > INT_MAX) return -1;
    *out_count = (int)count;
    return 0;
}

static int v5_bind_workspace(sqlite3_stmt *stmt, const char *workspace_id) {
    return sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC) == SQLITE_OK ? 0 : -1;
}

static int v5_node_index(const v5_workspace_t *workspace, const char *stable_id) {
    if (!workspace || !stable_id) return -1;
    for (int i = 0; i < workspace->input.node_count; i++) {
        if (strcmp(workspace->stable_node_ids[i], stable_id) == 0) return i;
    }
    return -1;
}

static int v5_node_stable_id_matches(const char *workspace_id, const CBMDumpNode *node,
                                     const char *stable_id) {
    if (!workspace_id || !node || !node->file_path || !stable_id) return -1;
    char relative_path[4096];
    char discriminator[512];
    char expected[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    if (snprintf(relative_path, sizeof(relative_path), "%s", node->file_path) >=
        (int)sizeof(relative_path))
        return -1;
    cbm_normalize_path_sep(relative_path);
    if (node->qualified_name && node->qualified_name[0]) {
        if (snprintf(discriminator, sizeof(discriminator), "named:%s", node->qualified_name) >=
            (int)sizeof(discriminator))
            return -1;
    } else if (!node->name || !node->name[0] || node->start_line < 0 ||
               node->end_line < node->start_line ||
               snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", node->name,
                        node->start_line, node->end_line) >= (int)sizeof(discriminator)) {
        return -1;
    }
    return cbm_zova_workspace_node_id_v2(workspace_id, node->label, relative_path,
                                         node->qualified_name, discriminator, expected,
                                         sizeof(expected)) == 0 &&
                   strcmp(expected, stable_id) == 0
               ? 0
               : -1;
}

static int v5_camel_split(const char *input, char **out) {
    if (out) *out = NULL;
    if (!input || !out) return -1;
    size_t input_len = strlen(input);
    if (input_len == 0) {
        *out = strdup("");
        return *out ? 0 : -1;
    }
    if (input_len > (SIZE_MAX - 2) / 2) return -1;
    char *result = malloc(input_len * 2 + 2);
    if (!result) return -1;
    memcpy(result, input, input_len);
    size_t len = input_len;
    result[len++] = ' ';
    for (size_t i = 0; i < input_len; i++) {
        char curr = input[i];
        char prev = i > 0 ? input[i - 1] : '\0';
        char next = i + 1 < input_len ? input[i + 1] : '\0';
        if (i > 0 && curr >= 'A' && curr <= 'Z' &&
            ((prev >= 'a' && prev <= 'z') ||
             (prev >= 'A' && prev <= 'Z' && next >= 'a' && next <= 'z')))
            result[len++] = ' ';
        result[len++] = curr;
    }
    result[len] = '\0';
    *out = result;
    return 0;
}

static void v5_workspace_free(v5_workspace_t *workspace) {
    if (!workspace) return;
    for (int i = 0; i < workspace->input.node_count; i++) {
        free((char *)workspace->nodes[i].label);
        free((char *)workspace->nodes[i].name);
        free((char *)workspace->nodes[i].qualified_name);
        free((char *)workspace->nodes[i].file_path);
        free((char *)workspace->nodes[i].properties);
        free(workspace->stable_node_ids ? workspace->stable_node_ids[i] : NULL);
    }
    for (int i = 0; i < workspace->input.edge_count; i++) {
        free((char *)workspace->edges[i].type);
        free((char *)workspace->edges[i].properties);
        free((char *)workspace->edges[i].url_path);
        free((char *)workspace->edges[i].local_name);
    }
    for (int i = 0; i < workspace->input.node_vector_count; i++)
        free((void *)workspace->node_vectors[i].vector);
    for (int i = 0; i < workspace->input.token_vector_count; i++) {
        free((char *)workspace->token_vectors[i].token);
        free((void *)workspace->token_vectors[i].vector);
    }
    for (int i = 0; i < workspace->input.file_hash_count; i++) {
        free((char *)workspace->file_hashes[i].file_path);
        free((char *)workspace->file_hashes[i].content_hash);
    }
    free(workspace->nodes);
    free(workspace->stable_node_ids);
    free(workspace->edges);
    free(workspace->node_vectors);
    free(workspace->token_vectors);
    free(workspace->file_hashes);
    free(workspace->workspace_id);
    free(workspace->root_path);
    free(workspace->project);
    free(workspace->indexed_at);
    free(workspace->model_fingerprint);
    free(workspace->summary);
    free(workspace->summary_source_hash);
    free(workspace->summary_created_at);
    free(workspace->summary_updated_at);
    memset(workspace, 0, sizeof(*workspace));
}

void cbm_zova_v5_snapshot_close(cbm_zova_v5_snapshot_t *snapshot) {
    if (!snapshot) return;
    for (int i = 0; i < snapshot->workspace_count; i++)
        v5_workspace_free(&snapshot->workspaces[i]);
    free(snapshot->workspaces);
    if (snapshot->db) {
        (void)sqlite3_exec(snapshot->db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(snapshot->db);
    }
    free(snapshot);
}

static int v5_validate_source(sqlite3 *db) {
    static const char *required[] = {
        "_zova_meta", "cbm_database_schema_v1", "cbm_workspace_registry",
        "cbm_database_generation_v1", "cbm_projects_v1", "cbm_nodes_v1",
        "cbm_edges_v1", "cbm_file_hashes_v1", "cbm_project_summaries_v2",
        "cbm_generation_integrity_v2", "cbm_node_vectors_compat_v1",
        "cbm_token_vectors_compat_v1", "cbm_nodes_fts_v1", "cbm_fts_rowmap_v1",
        "cbm_workspace_index_state_v1",
    };
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') "
                           "AND name=?1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < sizeof(required) / sizeof(required[0]); i++) {
        if (sqlite3_bind_text(stmt, 1, required[i], -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_int64(stmt, 0) != 1) {
            rc = -1;
        }
        if (sqlite3_reset(stmt) != SQLITE_OK || sqlite3_clear_bindings(stmt) != SQLITE_OK) rc = -1;
    }
    sqlite3_finalize(stmt);
    if (rc != 0) return -1;
    int64_t schema_version = 0;
    int64_t format_matches = 0;
    if (v5_scalar(db, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1", NULL,
                  &schema_version) != 0 ||
        schema_version != 5 ||
        v5_scalar(db,
                  "SELECT count(*) FROM _zova_meta WHERE key='format_version' AND value='7'",
                  NULL, &format_matches) != 0 ||
        format_matches != 1)
        return -1;
    return 0;
}

static int v5_read_nodes(sqlite3 *db, v5_workspace_t *workspace) {
    int count = 0;
    if (v5_count(db, "cbm_nodes_v1", workspace->workspace_id, &count) != 0) return -1;
    workspace->input.node_count = count;
    if (count == 0) return 0;
    workspace->nodes = calloc((size_t)count, sizeof(*workspace->nodes));
    workspace->stable_node_ids = calloc((size_t)count, sizeof(*workspace->stable_node_ids));
    if (!workspace->nodes || !workspace->stable_node_ids) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT n.node_id,n.label,n.name,n.qualified_name,n.file_path,n.start_line,n.end_line,"
        "n.properties,m.fts_rowid FROM cbm_nodes_v1 n JOIN cbm_fts_rowmap_v1 m ON "
        "m.workspace_id=n.workspace_id AND m.node_id=n.node_id WHERE n.workspace_id=?1 "
        "ORDER BY m.fts_rowid";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int row = 0;
    while (row < count && sqlite3_step(stmt) == SQLITE_ROW) {
        CBMDumpNode *node = &workspace->nodes[row];
        workspace->stable_node_ids[row] = v5_text(stmt, 0);
        node->id = sqlite3_column_int64(stmt, 8);
        node->project = workspace->project;
        node->label = v5_text(stmt, 1);
        node->name = v5_text(stmt, 2);
        node->qualified_name = v5_text(stmt, 3);
        node->file_path = v5_text(stmt, 4);
        node->start_line = sqlite3_column_int(stmt, 5);
        node->end_line = sqlite3_column_int(stmt, 6);
        node->properties = v5_text(stmt, 7);
        if (!workspace->stable_node_ids[row] || !node->label || !node->name ||
            !node->qualified_name || !node->file_path || !node->properties || node->id <= 0 ||
            (row > 0 && workspace->nodes[row - 1].id >= node->id) ||
            v5_node_stable_id_matches(workspace->workspace_id, node,
                                      workspace->stable_node_ids[row]) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
        row++;
    }
    int rc = row == count && sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    workspace->input.nodes = workspace->nodes;
    return rc;
}

static int v5_read_edges(sqlite3 *db, v5_workspace_t *workspace) {
    int count = 0;
    if (v5_count(db, "cbm_edges_v1", workspace->workspace_id, &count) != 0) return -1;
    workspace->input.edge_count = count;
    if (count == 0) return 0;
    workspace->edges = calloc((size_t)count, sizeof(*workspace->edges));
    if (!workspace->edges) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT source_node_id,target_node_id,edge_type,properties,url_path,local_name "
        "FROM cbm_edges_v1 WHERE workspace_id=?1 ORDER BY edge_id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int row = 0;
    while (row < count && sqlite3_step(stmt) == SQLITE_ROW) {
        char *source = v5_text(stmt, 0);
        char *target = v5_text(stmt, 1);
        int source_index = v5_node_index(workspace, source);
        int target_index = v5_node_index(workspace, target);
        CBMDumpEdge *edge = &workspace->edges[row];
        edge->id = row + 1;
        edge->project = workspace->project;
        edge->source_id = source_index < 0 ? 0 : workspace->nodes[source_index].id;
        edge->target_id = target_index < 0 ? 0 : workspace->nodes[target_index].id;
        edge->type = v5_text(stmt, 2);
        edge->properties = v5_text(stmt, 3);
        edge->url_path = v5_text(stmt, 4);
        edge->local_name = v5_text(stmt, 5);
        free(source);
        free(target);
        if (source_index < 0 || target_index < 0 || !edge->type || !edge->type[0] ||
            !edge->properties || !edge->url_path || !edge->local_name) {
            sqlite3_finalize(stmt);
            return -1;
        }
        row++;
    }
    int rc = row == count && sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    workspace->input.edges = workspace->edges;
    return rc;
}

static int v5_read_node_vectors(sqlite3 *db, v5_workspace_t *workspace) {
    int count = 0;
    if (v5_count(db, "cbm_node_vectors_compat_v1", workspace->workspace_id, &count) != 0)
        return -1;
    workspace->input.node_vector_count = count;
    if (count == 0) return 0;
    workspace->node_vectors = calloc((size_t)count, sizeof(*workspace->node_vectors));
    if (!workspace->node_vectors) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT node_id,vector FROM cbm_node_vectors_compat_v1 "
                           "WHERE workspace_id=?1 ORDER BY node_id",
                           -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int row = 0;
    while (row < count && sqlite3_step(stmt) == SQLITE_ROW) {
        char *node_id = v5_text(stmt, 0);
        int node_index = v5_node_index(workspace, node_id);
        CBMDumpVector *vector = &workspace->node_vectors[row];
        vector->node_id = node_index < 0 ? 0 : workspace->nodes[node_index].id;
        vector->project = workspace->project;
        vector->vector_len = workspace->input.vector_dimensions;
        vector->vector = v5_blob(stmt, 1, vector->vector_len);
        free(node_id);
        if (node_index < 0 || !vector->vector) {
            sqlite3_finalize(stmt);
            return -1;
        }
        row++;
    }
    int rc = row == count && sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    workspace->input.node_vectors = workspace->node_vectors;
    return rc;
}

static int v5_read_token_vectors(sqlite3 *db, v5_workspace_t *workspace) {
    int count = 0;
    if (v5_count(db, "cbm_token_vectors_compat_v1", workspace->workspace_id, &count) != 0)
        return -1;
    workspace->input.token_vector_count = count;
    if (count == 0) return 0;
    workspace->token_vectors = calloc((size_t)count, sizeof(*workspace->token_vectors));
    if (!workspace->token_vectors) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT token_id,token,vector,idf FROM cbm_token_vectors_compat_v1 "
                           "WHERE workspace_id=?1 ORDER BY token_id",
                           -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int row = 0;
    int64_t previous_id = 0;
    while (row < count && sqlite3_step(stmt) == SQLITE_ROW) {
        CBMDumpTokenVec *vector = &workspace->token_vectors[row];
        vector->id = sqlite3_column_int64(stmt, 0);
        vector->project = workspace->project;
        vector->token = v5_text(stmt, 1);
        vector->vector_len = workspace->input.vector_dimensions;
        vector->vector = v5_blob(stmt, 2, vector->vector_len);
        vector->idf = (float)sqlite3_column_double(stmt, 3);
        if (vector->id <= previous_id || !vector->token || !vector->vector) {
            sqlite3_finalize(stmt);
            return -1;
        }
        previous_id = vector->id;
        row++;
    }
    int rc = row == count && sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    workspace->input.token_vectors = workspace->token_vectors;
    return rc;
}

static int v5_read_hashes(sqlite3 *db, v5_workspace_t *workspace) {
    int count = 0;
    if (v5_count(db, "cbm_file_hashes_v1", workspace->workspace_id, &count) != 0) return -1;
    workspace->input.file_hash_count = count;
    if (count == 0) return 0;
    workspace->file_hashes = calloc((size_t)count, sizeof(*workspace->file_hashes));
    if (!workspace->file_hashes) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT file_path,content_hash,mtime_ns,size_bytes FROM "
                           "cbm_file_hashes_v1 WHERE workspace_id=?1 ORDER BY file_path",
                           -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int row = 0;
    while (row < count && sqlite3_step(stmt) == SQLITE_ROW) {
        cbm_zova_file_hash_input_t *hash = &workspace->file_hashes[row];
        hash->file_path = v5_text(stmt, 0);
        hash->content_hash = v5_text(stmt, 1);
        hash->mtime_ns = sqlite3_column_int64(stmt, 2);
        hash->size_bytes = sqlite3_column_int64(stmt, 3);
        if (!hash->file_path || !hash->content_hash ||
            (row > 0 && strcmp(workspace->file_hashes[row - 1].file_path, hash->file_path) >= 0)) {
            sqlite3_finalize(stmt);
            return -1;
        }
        row++;
    }
    int rc = row == count && sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    workspace->input.file_hashes = workspace->file_hashes;
    return rc;
}

static int v5_read_summary(sqlite3 *db, v5_workspace_t *workspace) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT summary,source_hash,created_at,updated_at FROM "
                           "cbm_project_summaries_v2 WHERE workspace_id=?1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0;
    }
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    workspace->summary = v5_text(stmt, 0);
    workspace->summary_source_hash = v5_text(stmt, 1);
    workspace->summary_created_at = v5_text(stmt, 2);
    workspace->summary_updated_at = v5_text(stmt, 3);
    if (!workspace->summary || !workspace->summary_source_hash ||
        !workspace->summary_created_at || !workspace->summary_updated_at ||
        sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    workspace->input.project_summary = (cbm_zova_project_summary_input_t){
        .present = true,
        .summary = workspace->summary,
        .source_hash = workspace->summary_source_hash,
        .created_at = workspace->summary_created_at,
        .updated_at = workspace->summary_updated_at,
    };
    return 0;
}

static int v5_validate_fts(sqlite3 *db, const v5_workspace_t *workspace) {
    int fts_count = 0;
    int rowmap_count = 0;
    if (v5_count(db, "cbm_nodes_fts_v1", workspace->workspace_id, &fts_count) != 0 ||
        v5_count(db, "cbm_fts_rowmap_v1", workspace->workspace_id, &rowmap_count) != 0 ||
        fts_count != workspace->input.node_count || rowmap_count != workspace->input.node_count)
        return -1;
    int64_t joined = 0;
    const char *sql =
        "SELECT count(*) FROM cbm_fts_rowmap_v1 m "
        "JOIN cbm_nodes_fts_v1 f ON f.workspace_id=m.workspace_id AND f.node_id=m.node_id "
        "JOIN cbm_nodes_v1 n ON n.workspace_id=m.workspace_id "
        "AND n.node_id=m.node_id WHERE m.workspace_id=?1 AND "
        "f.qualified_name=n.qualified_name AND f.file_path=n.file_path AND f.label=n.label";
    if (v5_scalar(db, sql, workspace->workspace_id, &joined) != 0 ||
        joined != workspace->input.node_count)
        return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT f.name FROM cbm_fts_rowmap_v1 m JOIN cbm_nodes_fts_v1 f "
                           "ON f.workspace_id=m.workspace_id AND f.node_id=m.node_id WHERE "
                           "m.workspace_id=?1 ORDER BY m.fts_rowid",
                           -1, &stmt, NULL) != SQLITE_OK ||
        v5_bind_workspace(stmt, workspace->workspace_id) != 0)
        return sqlite3_finalize(stmt), -1;
    int row = 0;
    int step = SQLITE_ROW;
    while (row < workspace->input.node_count && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        char *expected = NULL;
        const unsigned char *actual = sqlite3_column_text(stmt, 0);
        if (!actual || v5_camel_split(workspace->nodes[row].name, &expected) != 0 ||
            strcmp((const char *)actual, expected) != 0) {
            free(expected);
            sqlite3_finalize(stmt);
            return -1;
        }
        free(expected);
        row++;
    }
    if (row == workspace->input.node_count && step == SQLITE_ROW) step = sqlite3_step(stmt);
    int rc = row == workspace->input.node_count && step == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int v5_validate_integrity(sqlite3 *db, const v5_workspace_t *workspace) {
    cbm_zova_workspace_generation_result_t actual = {0};
    if (cbm_zova_workspace_generation_digest_input(workspace->workspace_id, &workspace->input,
                                                   &actual) != 0)
        return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT graph_nodes,graph_edges,metadata_nodes,metadata_edges,metadata_topology_edges,"
        "fts_rows,node_vector_rows,token_vector_rows,node_vectors,token_vectors,metadata_sha256,"
        "fts_sha256,topology_sha256,node_vector_sha256,token_vector_sha256 FROM "
        "cbm_generation_integrity_v2 WHERE workspace_id=?1 AND generation=?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, workspace->active_generation) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    const uint64_t expected_counts[] = {
        actual.graph_nodes, actual.graph_edges, actual.metadata_nodes, actual.metadata_edges,
        actual.metadata_topology_edges, actual.fts_rows, actual.node_vector_rows,
        actual.token_vector_rows, actual.node_vectors, actual.token_vectors,
    };
    int rc = 0;
    for (int i = 0; rc == 0 && i < 10; i++)
        if (sqlite3_column_int64(stmt, i) < 0 ||
            (uint64_t)sqlite3_column_int64(stmt, i) != expected_counts[i])
            rc = -1;
    const char *expected_digests[] = {actual.metadata_sha256, actual.fts_sha256,
                                      actual.topology_sha256, actual.node_vector_sha256,
                                      actual.token_vector_sha256};
    for (int i = 0; rc == 0 && i < 5; i++) {
        const unsigned char *digest = sqlite3_column_text(stmt, 10 + i);
        if (!digest || strcmp((const char *)digest, expected_digests[i]) != 0) rc = -1;
    }
    if (rc == 0 && sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int v5_read_workspace(sqlite3 *db, sqlite3_stmt *workspace_stmt,
                             v5_workspace_t *workspace) {
    workspace->workspace_id = v5_text(workspace_stmt, 0);
    workspace->active_generation = sqlite3_column_int64(workspace_stmt, 1);
    workspace->project = v5_text(workspace_stmt, 2);
    workspace->root_path = v5_text(workspace_stmt, 3);
    workspace->indexed_at = v5_text(workspace_stmt, 4);
    workspace->model_fingerprint = v5_text(workspace_stmt, 5);
    workspace->input.vector_dimensions = sqlite3_column_int(workspace_stmt, 6);
    if (!workspace->workspace_id || !workspace->project || !workspace->root_path ||
        !workspace->indexed_at || !workspace->model_fingerprint ||
        workspace->active_generation <= 0 || workspace->input.vector_dimensions <= 0)
        return -1;
    workspace->input.root_path = workspace->root_path;
    workspace->input.project = workspace->project;
    workspace->input.indexed_at = workspace->indexed_at;
    workspace->input.model_fingerprint = workspace->model_fingerprint;
    return v5_read_nodes(db, workspace) == 0 && v5_read_edges(db, workspace) == 0 &&
                   v5_read_node_vectors(db, workspace) == 0 &&
                   v5_read_token_vectors(db, workspace) == 0 &&
                   v5_read_hashes(db, workspace) == 0 && v5_read_summary(db, workspace) == 0 &&
                   v5_validate_fts(db, workspace) == 0 &&
                   v5_validate_integrity(db, workspace) == 0
               ? 0
               : -1;
}

int cbm_zova_v5_snapshot_open(const char *source_path,
                              cbm_zova_v5_snapshot_t **out_snapshot) {
    if (out_snapshot) *out_snapshot = NULL;
    if (!source_path || !source_path[0] || !out_snapshot) return -1;
    cbm_zova_v5_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return -1;
    int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(source_path, &snapshot->db, flags, NULL) != SQLITE_OK ||
        sqlite3_exec(snapshot->db, "PRAGMA query_only=ON;BEGIN", NULL, NULL, NULL) != SQLITE_OK ||
        v5_validate_source(snapshot->db) != 0) {
        cbm_zova_v5_snapshot_close(snapshot);
        return -1;
    }
    int64_t count = 0;
    const char *count_sql =
        "SELECT count(*) FROM cbm_workspace_registry r JOIN cbm_projects_v1 p USING(workspace_id) "
        "JOIN cbm_workspace_index_state_v1 s USING(workspace_id) JOIN cbm_database_generation_v1 g "
        "ON g.workspace_id=r.workspace_id AND g.generation=r.active_generation "
        "WHERE r.active_generation>0 AND s.generation=r.active_generation AND g.state='ready'";
    if (v5_scalar(snapshot->db, count_sql, NULL, &count) != 0 || count <= 0 || count > INT_MAX) {
        cbm_zova_v5_snapshot_close(snapshot);
        return -1;
    }
    snapshot->workspace_count = (int)count;
    snapshot->workspaces = calloc((size_t)count, sizeof(*snapshot->workspaces));
    if (!snapshot->workspaces) {
        cbm_zova_v5_snapshot_close(snapshot);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT r.workspace_id,r.active_generation,p.project,p.root_path,p.indexed_at,"
        "s.model_fingerprint,s.vector_dimensions FROM cbm_workspace_registry r "
        "JOIN cbm_projects_v1 p USING(workspace_id) JOIN cbm_workspace_index_state_v1 s "
        "USING(workspace_id) JOIN cbm_database_generation_v1 g ON "
        "g.workspace_id=r.workspace_id AND g.generation=r.active_generation WHERE "
        "r.active_generation>0 AND s.generation=r.active_generation AND g.state='ready' "
        "ORDER BY r.workspace_id";
    if (sqlite3_prepare_v2(snapshot->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        cbm_zova_v5_snapshot_close(snapshot);
        return -1;
    }
    int row = 0;
    int rc = 0;
    while (row < snapshot->workspace_count && sqlite3_step(stmt) == SQLITE_ROW) {
        if (v5_read_workspace(snapshot->db, stmt, &snapshot->workspaces[row]) != 0) {
            rc = -1;
            break;
        }
        row++;
    }
    if (rc == 0 && (row != snapshot->workspace_count || sqlite3_step(stmt) != SQLITE_DONE)) rc = -1;
    sqlite3_finalize(stmt);
    if (rc != 0) {
        cbm_zova_v5_snapshot_close(snapshot);
        return -1;
    }
    *out_snapshot = snapshot;
    return 0;
}

int cbm_zova_v5_snapshot_workspace_count(const cbm_zova_v5_snapshot_t *snapshot) {
    return snapshot ? snapshot->workspace_count : -1;
}

const cbm_zova_workspace_generation_input_t *cbm_zova_v5_snapshot_input_at(
    const cbm_zova_v5_snapshot_t *snapshot, int index, int64_t *out_active_generation) {
    if (out_active_generation) *out_active_generation = 0;
    if (!snapshot || index < 0 || index >= snapshot->workspace_count) return NULL;
    if (out_active_generation)
        *out_active_generation = snapshot->workspaces[index].active_generation;
    return &snapshot->workspaces[index].input;
}
