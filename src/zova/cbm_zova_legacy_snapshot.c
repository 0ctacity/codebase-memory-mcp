#include "zova/cbm_zova_legacy_snapshot.h"

#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

#include <sqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

struct cbm_zova_legacy_snapshot {
    sqlite3 *source_db;
    cbm_zova_workspace_generation_input_t input;
    int64_t source_generation;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char *root_path;
    char *project;
    char *indexed_at;
    char *model_fingerprint;
    CBMDumpNode *nodes;
    CBMDumpEdge *edges;
    CBMDumpVector *node_vectors;
    CBMDumpTokenVec *token_vectors;
    cbm_zova_file_hash_input_t *file_hashes;
    char *summary;
    char *summary_source_hash;
    char *summary_created_at;
    char *summary_updated_at;
    char **source_ids;
    char **target_ids;
    cbm_zova_migration_fts_row_t *fts_rows;
    int fts_row_count;
    cbm_zova_migration_manifest_t manifest;
    char **fts_queries;
    int fts_query_count;
#if CBM_WITH_ZOVA
    zova_database *source_zova;
#endif
};

static char *snapshot_text(sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    return strdup(text ? (const char *)text : "");
}

static int snapshot_string_ptr_compare(const void *left_ptr, const void *right_ptr) {
    const char *const *left = left_ptr;
    const char *const *right = right_ptr;
    return strcmp(*left, *right);
}

static int snapshot_scalar(sqlite3 *db, const char *sql, const char *text_bind,
                           int64_t *out_value) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (text_bind && sqlite3_bind_text(stmt, 1, text_bind, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    if (rc == 0) *out_value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return rc;
}

static void snapshot_free_nodes(cbm_zova_legacy_snapshot_t *snapshot) {
    for (int i = 0; i < snapshot->input.node_count; i++) {
        free((char *)snapshot->nodes[i].project);
        free((char *)snapshot->nodes[i].label);
        free((char *)snapshot->nodes[i].name);
        free((char *)snapshot->nodes[i].qualified_name);
        free((char *)snapshot->nodes[i].file_path);
        free((char *)snapshot->nodes[i].properties);
        free(snapshot->source_ids ? snapshot->source_ids[i] : NULL);
        free(snapshot->target_ids ? snapshot->target_ids[i] : NULL);
    }
    free(snapshot->source_ids);
    free(snapshot->target_ids);
    free(snapshot->nodes);
}

void cbm_zova_legacy_snapshot_close(cbm_zova_legacy_snapshot_t *snapshot) {
    if (!snapshot) return;
    snapshot_free_nodes(snapshot);
    for (int i = 0; i < snapshot->input.edge_count; i++) {
        free((char *)snapshot->edges[i].project);
        free((char *)snapshot->edges[i].type);
        free((char *)snapshot->edges[i].properties);
        free((char *)snapshot->edges[i].url_path);
        free((char *)snapshot->edges[i].local_name);
    }
    free(snapshot->edges);
    for (int i = 0; i < snapshot->input.node_vector_count; i++) {
        free((char *)snapshot->node_vectors[i].project);
        free((void *)snapshot->node_vectors[i].vector);
    }
    free(snapshot->node_vectors);
    for (int i = 0; i < snapshot->input.token_vector_count; i++) {
        free((char *)snapshot->token_vectors[i].project);
        free((char *)snapshot->token_vectors[i].token);
        free((void *)snapshot->token_vectors[i].vector);
    }
    free(snapshot->token_vectors);
    for (int i = 0; i < snapshot->input.file_hash_count; i++) {
        free((char *)snapshot->file_hashes[i].file_path);
        free((char *)snapshot->file_hashes[i].content_hash);
    }
    free(snapshot->file_hashes);
    free(snapshot->root_path);
    free(snapshot->project);
    free(snapshot->indexed_at);
    free(snapshot->model_fingerprint);
    free(snapshot->summary);
    free(snapshot->summary_source_hash);
    free(snapshot->summary_created_at);
    free(snapshot->summary_updated_at);
    for (int i = 0; i < snapshot->fts_row_count; i++) {
        free((char *)snapshot->fts_rows[i].name);
        free((char *)snapshot->fts_rows[i].qualified_name);
        free((char *)snapshot->fts_rows[i].label);
        free((char *)snapshot->fts_rows[i].file_path);
    }
    free(snapshot->fts_rows);
    for (int i = 0; i < snapshot->fts_query_count; i++) free(snapshot->fts_queries[i]);
    free(snapshot->fts_queries);
    if (snapshot->source_db) {
        (void)sqlite3_exec(snapshot->source_db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(snapshot->source_db);
    }
#if CBM_WITH_ZOVA
    if (snapshot->source_zova) (void)zova_database_close(snapshot->source_zova);
#endif
    free(snapshot);
}

static int snapshot_required_tables(sqlite3 *db) {
    int64_t count = 0;
    return snapshot_scalar(
               db,
               "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') AND name IN ("
               "'projects','nodes','edges','file_hashes','project_summaries','node_vectors',"
               "'token_vectors','nodes_fts','cbm_zova_sidecar_generation_v1')",
               NULL, &count) == 0 &&
                   count == 9
               ? 0
               : -1;
}

static int snapshot_read_project(cbm_zova_legacy_snapshot_t *snapshot,
                                 const char *canonical_root) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db,
                        "SELECT count(*) FROM projects WHERE root_path=?1", canonical_root,
                        &count) != 0 ||
        count != 1) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(snapshot->source_db,
                           "SELECT name,indexed_at,root_path FROM projects WHERE root_path=?1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, canonical_root, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    snapshot->project = snapshot_text(stmt, 0);
    snapshot->indexed_at = snapshot_text(stmt, 1);
    snapshot->root_path = snapshot_text(stmt, 2);
    snapshot->model_fingerprint = strdup(CBM_ZOVA_MODEL_FINGERPRINT);
    sqlite3_finalize(stmt);
    return snapshot->project && snapshot->indexed_at && snapshot->root_path &&
                   snapshot->model_fingerprint
               ? 0
               : -1;
}

static int snapshot_read_nodes(cbm_zova_legacy_snapshot_t *snapshot) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db, "SELECT count(*) FROM nodes WHERE project=?1",
                        snapshot->project, &count) != 0 ||
        count < 0 || count > INT32_MAX) {
        return -1;
    }
    snapshot->input.node_count = (int)count;
    if (count == 0) return 0;
    snapshot->nodes = calloc((size_t)count, sizeof(*snapshot->nodes));
    snapshot->source_ids = calloc((size_t)count, sizeof(*snapshot->source_ids));
    snapshot->target_ids = calloc((size_t)count, sizeof(*snapshot->target_ids));
    if (!snapshot->nodes || !snapshot->source_ids || !snapshot->target_ids) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT id,project,label,name,qualified_name,file_path,start_line,end_line,properties "
        "FROM nodes WHERE project=?1 ORDER BY id";
    if (sqlite3_prepare_v2(snapshot->source_db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, snapshot->project, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int i = 0;
    while (i < count && sqlite3_step(stmt) == SQLITE_ROW) {
        CBMDumpNode *node = &snapshot->nodes[i];
        node->id = sqlite3_column_int64(stmt, 0);
        node->project = snapshot_text(stmt, 1);
        node->label = snapshot_text(stmt, 2);
        node->name = snapshot_text(stmt, 3);
        node->qualified_name = snapshot_text(stmt, 4);
        node->file_path = snapshot_text(stmt, 5);
        node->start_line = sqlite3_column_int(stmt, 6);
        node->end_line = sqlite3_column_int(stmt, 7);
        node->properties = snapshot_text(stmt, 8);
        snapshot->source_ids[i] = calloc(CBM_ZOVA_DIGEST_HEX_SIZE + 16, 1);
        snapshot->target_ids[i] = calloc(CBM_ZOVA_DIGEST_HEX_SIZE + 16, 1);
        char *normalized_path = strdup(node->file_path);
        if (normalized_path) cbm_normalize_path_sep(normalized_path);
        char source_discriminator[64];
        int source_discriminator_len =
            node->qualified_name[0]
                ? snprintf(source_discriminator, sizeof(source_discriminator), "named")
                : snprintf(source_discriminator, sizeof(source_discriminator), "anon:%d:%d",
                           node->start_line, node->end_line);
        char discriminator[512];
        int discriminator_len =
            node->qualified_name[0]
                ? snprintf(discriminator, sizeof(discriminator), "named:%s", node->qualified_name)
                : snprintf(discriminator, sizeof(discriminator), "local:%s:span:%d:%d", node->name,
                           node->start_line, node->end_line);
        if (!node->project || !node->label || !node->name || !node->qualified_name ||
            !node->file_path || !node->properties || !normalized_path || !snapshot->source_ids[i] ||
            !snapshot->target_ids[i] || source_discriminator_len < 0 ||
            source_discriminator_len >= (int)sizeof(source_discriminator) || discriminator_len < 0 ||
            discriminator_len >= (int)sizeof(discriminator) ||
            cbm_zova_workspace_node_id_v1(snapshot->workspace_id, node->label, normalized_path,
                                          node->qualified_name, source_discriminator,
                                          snapshot->source_ids[i], CBM_ZOVA_DIGEST_HEX_SIZE + 16) !=
                0 ||
            cbm_zova_workspace_node_id_v2(snapshot->workspace_id, node->label, normalized_path,
                                          node->qualified_name, discriminator,
                                          snapshot->target_ids[i],
                                          CBM_ZOVA_DIGEST_HEX_SIZE + 16) != 0) {
            free(normalized_path);
            sqlite3_finalize(stmt);
            return -1;
        }
        free(normalized_path);
        for (int previous = 0; previous < i; previous++) {
            if (strcmp(snapshot->source_ids[previous], snapshot->source_ids[i]) == 0 ||
                strcmp(snapshot->target_ids[previous], snapshot->target_ids[i]) == 0) {
                sqlite3_finalize(stmt);
                return -1;
            }
        }
        i++;
    }
    sqlite3_finalize(stmt);
    return i == count ? 0 : -1;
}

/* The source schema fixes FTS5 to unicode61 without custom token characters.
 * Pure ASCII punctuation/control/space therefore contributes no searchable
 * token. Any ASCII letter/digit or non-ASCII byte is conservatively probed. */
int cbm_zova_migration_fts_value_may_tokenize(const char *value) {
    if (!value) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') || *p >= 0x80) {
            return 1;
        }
    }
    return 0;
}

static int snapshot_read_fts(cbm_zova_legacy_snapshot_t *snapshot) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db, "SELECT count(*) FROM nodes_fts", NULL, &count) != 0 ||
        count != snapshot->input.node_count || count > INT32_MAX) {
        return -1;
    }
    snapshot->fts_row_count = (int)count;
    if (!count) return 0;
    snapshot->fts_rows = calloc((size_t)count, sizeof(*snapshot->fts_rows));
    if (!snapshot->fts_rows) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(snapshot->source_db, "SELECT rowid FROM nodes_fts ORDER BY rowid",
                           -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int i = 0;
    while (i < count && sqlite3_step(stmt) == SQLITE_ROW) {
        cbm_zova_migration_fts_row_t *row = &snapshot->fts_rows[i];
        row->source_node_id = sqlite3_column_int64(stmt, 0);
        const CBMDumpNode *node = NULL;
        for (int n = 0; n < snapshot->input.node_count; n++) {
            if (snapshot->nodes[n].id == row->source_node_id) node = &snapshot->nodes[n];
        }
        if (!node || !(row->name = strdup(node->name)) ||
            !(row->qualified_name = strdup(node->qualified_name)) ||
            !(row->label = strdup(node->label)) || !(row->file_path = strdup(node->file_path))) {
            sqlite3_finalize(stmt);
            return -1;
        }
        i++;
    }
    sqlite3_finalize(stmt);
    if (i != count) return -1;

    const char *columns[] = {"name", "qualified_name", "label", "file_path"};
    for (i = 0; i < count; i++) {
        const char *values[] = {snapshot->fts_rows[i].name,
                                snapshot->fts_rows[i].qualified_name,
                                snapshot->fts_rows[i].label,
                                snapshot->fts_rows[i].file_path};
        for (int column = 0; column < 4; column++) {
            if (!cbm_zova_migration_fts_value_may_tokenize(values[column])) continue;
            char query[2048];
            size_t offset = (size_t)snprintf(query, sizeof(query), "%s : \"", columns[column]);
            for (const char *p = values[column]; *p && offset + 3 < sizeof(query); p++) {
                if (*p == '"') query[offset++] = '"';
                query[offset++] = *p;
            }
            if (offset + 2 >= sizeof(query)) return -1;
            query[offset++] = '"';
            query[offset] = '\0';
            sqlite3_stmt *match = NULL;
            if (sqlite3_prepare_v2(snapshot->source_db,
                                   "SELECT count(*) FROM nodes_fts WHERE rowid=?1 AND "
                                   "nodes_fts MATCH ?2",
                                   -1, &match, NULL) != SQLITE_OK ||
                sqlite3_bind_int64(match, 1, snapshot->fts_rows[i].source_node_id) != SQLITE_OK ||
                sqlite3_bind_text(match, 2, query, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_step(match) != SQLITE_ROW || sqlite3_column_int64(match, 0) != 1) {
                sqlite3_finalize(match);
                return -1;
            }
            sqlite3_finalize(match);
        }
    }
    return 0;
}

typedef struct {
    char *term;
    int frequency;
    uint8_t hash[CBM_SHA256_DIGEST_LEN];
} snapshot_term_t;

static int snapshot_term_lex_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_term_t *left = left_ptr;
    const snapshot_term_t *right = right_ptr;
    return strcmp(left->term, right->term);
}

static int snapshot_term_most_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_term_t *left = left_ptr;
    const snapshot_term_t *right = right_ptr;
    if (left->frequency != right->frequency) return right->frequency - left->frequency;
    return strcmp(left->term, right->term);
}

static int snapshot_term_least_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_term_t *left = left_ptr;
    const snapshot_term_t *right = right_ptr;
    if (left->frequency != right->frequency) return left->frequency - right->frequency;
    return strcmp(left->term, right->term);
}

static int snapshot_term_hash_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_term_t *left = left_ptr;
    const snapshot_term_t *right = right_ptr;
    int cmp = memcmp(left->hash, right->hash, sizeof(left->hash));
    return cmp != 0 ? cmp : strcmp(left->term, right->term);
}

static int snapshot_add_query(cbm_zova_legacy_snapshot_t *snapshot, const char *value) {
    if (!value || !value[0]) return 0;
    char query[1024];
    size_t offset = 1;
    query[0] = '"';
    for (const char *p = value; *p && offset + 3 < sizeof(query); p++) {
        if (*p == '"') query[offset++] = '"';
        query[offset++] = *p;
    }
    if (offset + 2 >= sizeof(query)) return -1;
    query[offset++] = '"';
    query[offset] = '\0';
    for (int i = 0; i < snapshot->fts_query_count; i++) {
        if (strcmp(snapshot->fts_queries[i], query) == 0) return 0;
    }
    if (snapshot->fts_query_count == 512) return 0;
    char **grown = realloc(snapshot->fts_queries,
                           (size_t)(snapshot->fts_query_count + 1) * sizeof(*grown));
    if (!grown) return -1;
    snapshot->fts_queries = grown;
    snapshot->fts_queries[snapshot->fts_query_count] = strdup(query);
    if (!snapshot->fts_queries[snapshot->fts_query_count]) return -1;
    snapshot->fts_query_count++;
    return 0;
}

static int snapshot_term_add(snapshot_term_t **terms, int *count, const char *start, size_t len) {
    if (len == 0 || len >= 256) return 0;
    char value[256];
    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        value[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    value[len] = '\0';
    for (int i = 0; i < *count; i++) {
        if (strcmp((*terms)[i].term, value) == 0) {
            (*terms)[i].frequency++;
            return 0;
        }
    }
    snapshot_term_t *grown = realloc(*terms, (size_t)(*count + 1) * sizeof(*grown));
    if (!grown) return -1;
    *terms = grown;
    snapshot_term_t *term = &grown[*count];
    memset(term, 0, sizeof(*term));
    term->term = strdup(value);
    term->frequency = 1;
    if (!term->term) return -1;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    cbm_sha256_update(&hash, value, len);
    cbm_sha256_final(&hash, term->hash);
    (*count)++;
    return 0;
}

static int snapshot_collect_terms(snapshot_term_t **terms, int *count, const char *text) {
    const char *start = NULL;
    for (const char *p = text;; p++) {
        int word = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '_';
        if (word && !start) start = p;
        if (!word && start) {
            if (snapshot_term_add(terms, count, start, (size_t)(p - start)) != 0) return -1;
            start = NULL;
        }
        if (!*p) break;
    }
    return 0;
}

typedef struct {
    double score;
    const char *qualified_name;
    const char *stable_id;
} snapshot_fts_result_t;

static int snapshot_fts_result_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_fts_result_t *left = left_ptr;
    const snapshot_fts_result_t *right = right_ptr;
    if (left->score < right->score) return -1;
    if (left->score > right->score) return 1;
    int cmp = strcmp(left->qualified_name, right->qualified_name);
    return cmp != 0 ? cmp : strcmp(left->stable_id, right->stable_id);
}

static int snapshot_build_fts_manifest(cbm_zova_legacy_snapshot_t *snapshot,
                                       const char *row_digest) {
    snapshot_term_t *terms = NULL;
    int term_count = 0;
    int rc = 0;
    for (int i = 0; rc == 0 && i < snapshot->input.node_count; i++) {
        const CBMDumpNode *node = &snapshot->nodes[i];
        rc = snapshot_add_query(snapshot, node->label);
        const char *dot = strrchr(node->file_path, '.');
        if (rc == 0 && dot && dot[1]) rc = snapshot_add_query(snapshot, dot + 1);
        if (rc == 0) rc = snapshot_collect_terms(&terms, &term_count, node->name);
        if (rc == 0) rc = snapshot_collect_terms(&terms, &term_count, node->qualified_name);
        if (rc == 0) rc = snapshot_collect_terms(&terms, &term_count, node->file_path);
    }
    int limits[4] = {128, 128, 128, 128};
    int (*comparators[4])(const void *, const void *) = {
        snapshot_term_most_compare, snapshot_term_least_compare, snapshot_term_lex_compare,
        snapshot_term_hash_compare};
    for (int group = 0; rc == 0 && group < 4; group++) {
        if (term_count > 1) {
            qsort(terms, (size_t)term_count, sizeof(*terms), comparators[group]);
        }
        int take = term_count < limits[group] ? term_count : limits[group];
        for (int i = 0; rc == 0 && i < take; i++) rc = snapshot_add_query(snapshot, terms[i].term);
    }
    for (int i = 0; i < term_count; i++) free(terms[i].term);
    free(terms);
    if (rc != 0) return -1;

    cbm_sha256_ctx results;
    cbm_sha256_init(&results);
    for (int q = 0; q < snapshot->fts_query_count; q++) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(snapshot->source_db,
                               "SELECT rowid,bm25(nodes_fts) FROM nodes_fts WHERE nodes_fts "
                               "MATCH ?1",
                               -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, snapshot->fts_queries[q], -1, SQLITE_STATIC) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
        snapshot_fts_result_t *query_results = NULL;
        int query_result_count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t rowid = sqlite3_column_int64(stmt, 0);
            int node_index = -1;
            for (int i = 0; i < snapshot->input.node_count; i++)
                if (snapshot->nodes[i].id == rowid) node_index = i;
            if (node_index < 0) {
                sqlite3_finalize(stmt);
                free(query_results);
                return -1;
            }
            snapshot_fts_result_t *grown = realloc(
                query_results, (size_t)(query_result_count + 1) * sizeof(*grown));
            if (!grown) {
                sqlite3_finalize(stmt);
                free(query_results);
                return -1;
            }
            query_results = grown;
            query_results[query_result_count++] = (snapshot_fts_result_t){
                .score = sqlite3_column_double(stmt, 1),
                .qualified_name = snapshot->nodes[node_index].qualified_name,
                .stable_id = snapshot->target_ids[node_index],
            };
        }
        sqlite3_finalize(stmt);
        if (query_result_count > 1) {
            qsort(query_results, (size_t)query_result_count, sizeof(*query_results),
                  snapshot_fts_result_compare);
        }
        for (int rank = 0; rank < query_result_count; rank++) {
            char rank_text[32], score_text[64];
            double normalized = nearbyint(query_results[rank].score * 1e12) / 1e12;
            snprintf(rank_text, sizeof(rank_text), "%d", rank);
            snprintf(score_text, sizeof(score_text), "%.12f", normalized);
            cbm_zova_migration_digest_text(&results, snapshot->fts_queries[q]);
            cbm_zova_migration_digest_text(&results, rank_text);
            cbm_zova_migration_digest_text(&results, query_results[rank].stable_id);
            cbm_zova_migration_digest_text(&results, score_text);
        }
        free(query_results);
    }
    char query_digest[CBM_ZOVA_DIGEST_HEX_SIZE];
    cbm_zova_migration_digest_finalize(&results, query_digest);
    cbm_sha256_ctx combined;
    cbm_sha256_init(&combined);
    cbm_zova_migration_digest_text(&combined, row_digest);
    cbm_zova_migration_digest_text(&combined, query_digest);
    cbm_zova_migration_digest_finalize(&combined, snapshot->manifest.fts_sha256);
    snapshot->manifest.fts_query_count = (uint64_t)snapshot->fts_query_count;
    return 0;
}

static int snapshot_build_manifest(cbm_zova_legacy_snapshot_t *snapshot) {
    cbm_zova_workspace_generation_result_t result = {0};
    if (cbm_zova_workspace_generation_digest_input(snapshot->workspace_id, &snapshot->input,
                                                   &result) != 0)
        return -1;
    snapshot->manifest.workspace_count = 1;
    snapshot->manifest.stable_id_count = (uint64_t)snapshot->input.node_count;
    snapshot->manifest.graph_node_count = result.graph_nodes;
    snapshot->manifest.graph_edge_count = result.graph_edges;
    snapshot->manifest.node_vector_count = result.node_vectors;
    snapshot->manifest.token_vector_count = result.token_vectors;
    snprintf(snapshot->manifest.metadata_sha256, CBM_ZOVA_DIGEST_HEX_SIZE, "%s",
             result.metadata_sha256);
    snprintf(snapshot->manifest.topology_sha256, CBM_ZOVA_DIGEST_HEX_SIZE, "%s",
             result.topology_sha256);
    snprintf(snapshot->manifest.node_vector_sha256, CBM_ZOVA_DIGEST_HEX_SIZE, "%s",
             result.node_vector_sha256);
    snprintf(snapshot->manifest.token_vector_sha256, CBM_ZOVA_DIGEST_HEX_SIZE, "%s",
             result.token_vector_sha256);
    return snapshot_build_fts_manifest(snapshot, result.fts_sha256);
}

static int snapshot_validate_generation(cbm_zova_legacy_snapshot_t *snapshot,
                                        const char *source_zova_path) {
    int64_t sidecar_generation = 0;
    int64_t active_generation = 0;
    char registry_path[1024];
    return cbm_zova_sidecar_generation_get(source_zova_path, &sidecar_generation) == 0 &&
                   cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)) == 0 &&
                   cbm_zova_workspace_lookup_at(registry_path, snapshot->root_path,
                                                snapshot->workspace_id,
                                                sizeof(snapshot->workspace_id)) == 0 &&
                   cbm_zova_workspace_active_generation_at(registry_path, snapshot->workspace_id,
                                                            &active_generation) == 0 &&
                   sidecar_generation == snapshot->source_generation &&
                   active_generation == snapshot->source_generation
               ? 0
               : -1;
}

static int snapshot_has_node(const cbm_zova_legacy_snapshot_t *snapshot, int64_t id) {
    for (int i = 0; i < snapshot->input.node_count; i++) {
        if (snapshot->nodes[i].id == id) return 1;
    }
    return 0;
}

static int snapshot_read_edges(cbm_zova_legacy_snapshot_t *snapshot) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db, "SELECT count(*) FROM edges WHERE project=?1",
                        snapshot->project, &count) != 0 ||
        count < 0 || count > INT32_MAX) return -1;
    snapshot->input.edge_count = (int)count;
    if (!count) return 0;
    snapshot->edges = calloc((size_t)count, sizeof(*snapshot->edges));
    if (!snapshot->edges) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT id,project,source_id,target_id,type,properties,"
        "coalesce(url_path_gen,''),coalesce(local_name_gen,'') FROM edges "
        "WHERE project=?1 ORDER BY id";
    if (sqlite3_prepare_v2(snapshot->source_db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, snapshot->project, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int i = 0;
    while (i < count && sqlite3_step(stmt) == SQLITE_ROW) {
        CBMDumpEdge *edge = &snapshot->edges[i];
        edge->id = sqlite3_column_int64(stmt, 0);
        edge->project = snapshot_text(stmt, 1);
        edge->source_id = sqlite3_column_int64(stmt, 2);
        edge->target_id = sqlite3_column_int64(stmt, 3);
        edge->type = snapshot_text(stmt, 4);
        edge->properties = snapshot_text(stmt, 5);
        edge->url_path = snapshot_text(stmt, 6);
        edge->local_name = snapshot_text(stmt, 7);
        if (!snapshot_has_node(snapshot, edge->source_id) ||
            !snapshot_has_node(snapshot, edge->target_id) || !edge->project || !edge->type ||
            !edge->type[0] || !edge->properties || !edge->url_path || !edge->local_name) {
            sqlite3_finalize(stmt);
            return -1;
        }
        i++;
    }
    sqlite3_finalize(stmt);
    return i == count ? 0 : -1;
}

static int snapshot_read_node_vectors(cbm_zova_legacy_snapshot_t *snapshot) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db,
                        "SELECT count(*) FROM node_vectors WHERE project=?1", snapshot->project,
                        &count) != 0 ||
        count < 0 || count > INT32_MAX) return -1;
    snapshot->input.node_vector_count = (int)count;
    if (!count) return -1;
    snapshot->node_vectors = calloc((size_t)count, sizeof(*snapshot->node_vectors));
    if (!snapshot->node_vectors) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(snapshot->source_db,
                           "SELECT node_id,project,vector FROM node_vectors WHERE project=?1 "
                           "ORDER BY node_id",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, snapshot->project, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int i = 0;
    while (i < count && sqlite3_step(stmt) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(stmt, 2);
        const void *blob = sqlite3_column_blob(stmt, 2);
        if (len <= 0 || !blob || (i > 0 && len != snapshot->input.vector_dimensions)) {
            sqlite3_finalize(stmt);
            return -1;
        }
        if (i == 0) snapshot->input.vector_dimensions = len;
        snapshot->node_vectors[i].node_id = sqlite3_column_int64(stmt, 0);
        snapshot->node_vectors[i].project = snapshot_text(stmt, 1);
        snapshot->node_vectors[i].vector = malloc((size_t)len);
        snapshot->node_vectors[i].vector_len = len;
        if (!snapshot_has_node(snapshot, snapshot->node_vectors[i].node_id) ||
            !snapshot->node_vectors[i].project || !snapshot->node_vectors[i].vector) {
            sqlite3_finalize(stmt);
            return -1;
        }
        memcpy((void *)snapshot->node_vectors[i].vector, blob, (size_t)len);
        i++;
    }
    sqlite3_finalize(stmt);
    return i == count ? 0 : -1;
}

static int snapshot_read_token_vectors(cbm_zova_legacy_snapshot_t *snapshot) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db,
                        "SELECT count(*) FROM token_vectors WHERE project=?1", snapshot->project,
                        &count) != 0 ||
        count < 0 || count > INT32_MAX) return -1;
    snapshot->input.token_vector_count = (int)count;
    if (!count) return 0;
    snapshot->token_vectors = calloc((size_t)count, sizeof(*snapshot->token_vectors));
    if (!snapshot->token_vectors) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(snapshot->source_db,
                           "SELECT id,project,token,vector,idf FROM token_vectors WHERE project=?1 "
                           "ORDER BY id",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, snapshot->project, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int i = 0;
    while (i < count && sqlite3_step(stmt) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(stmt, 3);
        const void *blob = sqlite3_column_blob(stmt, 3);
        if (len != snapshot->input.vector_dimensions || !blob) {
            sqlite3_finalize(stmt);
            return -1;
        }
        snapshot->token_vectors[i].id = sqlite3_column_int64(stmt, 0);
        snapshot->token_vectors[i].project = snapshot_text(stmt, 1);
        snapshot->token_vectors[i].token = snapshot_text(stmt, 2);
        snapshot->token_vectors[i].vector = malloc((size_t)len);
        snapshot->token_vectors[i].vector_len = len;
        snapshot->token_vectors[i].idf = (float)sqlite3_column_double(stmt, 4);
        if (!snapshot->token_vectors[i].project || !snapshot->token_vectors[i].token ||
            !snapshot->token_vectors[i].vector) {
            sqlite3_finalize(stmt);
            return -1;
        }
        memcpy((void *)snapshot->token_vectors[i].vector, blob, (size_t)len);
        i++;
    }
    sqlite3_finalize(stmt);
    return i == count ? 0 : -1;
}

static int snapshot_read_hashes_and_summary(cbm_zova_legacy_snapshot_t *snapshot) {
    int64_t count = 0;
    if (snapshot_scalar(snapshot->source_db,
                        "SELECT count(*) FROM file_hashes WHERE project=?1", snapshot->project,
                        &count) != 0 || count < 0 || count > INT32_MAX) return -1;
    snapshot->input.file_hash_count = (int)count;
    if (count) {
        snapshot->file_hashes = calloc((size_t)count, sizeof(*snapshot->file_hashes));
        if (!snapshot->file_hashes) return -1;
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(snapshot->source_db,
                               "SELECT rel_path,sha256,mtime_ns,size FROM file_hashes "
                               "WHERE project=?1 ORDER BY rel_path",
                               -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, snapshot->project, -1, SQLITE_STATIC) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
        int i = 0;
        while (i < count && sqlite3_step(stmt) == SQLITE_ROW) {
            snapshot->file_hashes[i].file_path = snapshot_text(stmt, 0);
            snapshot->file_hashes[i].content_hash = snapshot_text(stmt, 1);
            snapshot->file_hashes[i].mtime_ns = sqlite3_column_int64(stmt, 2);
            snapshot->file_hashes[i].size_bytes = sqlite3_column_int64(stmt, 3);
            i++;
        }
        sqlite3_finalize(stmt);
        if (i != count) return -1;
    }
    sqlite3_stmt *summary = NULL;
    if (sqlite3_prepare_v2(snapshot->source_db,
                           "SELECT summary,source_hash,created_at,updated_at FROM project_summaries "
                           "WHERE project=?1",
                           -1, &summary, NULL) != SQLITE_OK ||
        sqlite3_bind_text(summary, 1, snapshot->project, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(summary);
        return -1;
    }
    if (sqlite3_step(summary) == SQLITE_ROW) {
        snapshot->summary = snapshot_text(summary, 0);
        snapshot->summary_source_hash = snapshot_text(summary, 1);
        snapshot->summary_created_at = snapshot_text(summary, 2);
        snapshot->summary_updated_at = snapshot_text(summary, 3);
        snapshot->input.project_summary = (cbm_zova_project_summary_input_t){
            .present = true,
            .summary = snapshot->summary,
            .source_hash = snapshot->summary_source_hash,
            .created_at = snapshot->summary_created_at,
            .updated_at = snapshot->summary_updated_at,
        };
    }
    sqlite3_finalize(summary);
    return 0;
}

static int snapshot_validate_sidecar(cbm_zova_legacy_snapshot_t *snapshot,
                                     const char *source_zova_path) {
    sqlite3 *sidecar = NULL;
    if (sqlite3_open_v2(source_zova_path, &sidecar, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(sidecar);
        return -1;
    }
    int64_t schema_ok = 0;
    int rc = snapshot_scalar(
        sidecar,
        "SELECT count(*) FROM cbm_zova_schema_v1 WHERE id=1 AND schema_version=1 AND "
        "metadata_projection_version=1 AND edge_metadata_projection_version=1",
        NULL, &schema_ok);
    int64_t trace_count = 0;
    if (rc == 0) {
        rc = snapshot_scalar(sidecar,
                             "SELECT count(*) FROM cbm_zova_trace_nodes_v1 WHERE workspace_id=?1",
                             snapshot->workspace_id, &trace_count);
    }
    if (rc == 0 && schema_ok != 1) rc = -2;
    if (rc == 0 && trace_count != snapshot->input.node_count) rc = -1;
    sqlite3_stmt *stmt = NULL;
    if (rc == 0 &&
        (sqlite3_prepare_v2(sidecar,
                            "SELECT node_id FROM cbm_zova_trace_nodes_v1 WHERE workspace_id=?1 "
                            "ORDER BY node_id",
                            -1, &stmt, NULL) != SQLITE_OK ||
         sqlite3_bind_text(stmt, 1, snapshot->workspace_id, -1, SQLITE_STATIC) != SQLITE_OK)) {
        rc = -1;
    }
    if (rc == 0) {
        char **sorted = calloc((size_t)snapshot->input.node_count, sizeof(*sorted));
        if (!sorted) rc = -1;
        for (int i = 0; rc == 0 && i < snapshot->input.node_count; i++) {
            sorted[i] = snapshot->source_ids[i];
        }
        if (rc == 0 && snapshot->input.node_count > 1)
            qsort(sorted, (size_t)snapshot->input.node_count, sizeof(*sorted),
                  snapshot_string_ptr_compare);
        for (int i = 0; rc == 0 && i < snapshot->input.node_count; i++) {
            if (sqlite3_step(stmt) != SQLITE_ROW ||
                strcmp((const char *)sqlite3_column_text(stmt, 0), sorted[i]) != 0) {
                rc = -1;
            }
        }
        free(sorted);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(sidecar);
    return rc;
}

#if CBM_WITH_ZOVA
static int snapshot_vector_nonzero(const uint8_t *values, int len) {
    for (int i = 0; i < len; i++) {
        if (values[i] != 0) return 1;
    }
    return 0;
}

static int snapshot_collection_present(const zova_vector_collection_list *list,
                                       const char *name) {
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].name && strcmp(list->items[i].name, name) == 0) return 1;
    }
    return 0;
}

static int snapshot_validate_vector(zova_database *db, const char *collection, const char *id,
                                    const uint8_t *expected, int dimensions) {
    zova_vector vector = {0};
    zova_status status = zova_vector_get(&(zova_vector_get_request){
        .db = db, .collection_name = collection, .vector_id = id, .out_vector = &vector});
    if (!snapshot_vector_nonzero(expected, dimensions)) {
        if (status == ZOVA_OK) zova_vector_free(&vector);
        return status == ZOVA_VECTOR_NOT_FOUND ? 0 : -1;
    }
    int rc = status == ZOVA_OK && vector.element_type == ZOVA_VECTOR_ELEMENT_TYPE_I8 &&
                     vector.values_len == (size_t)dimensions && vector.i8_values &&
                     memcmp(vector.i8_values, expected, (size_t)dimensions) == 0
                 ? 0
                 : -1;
    if (status == ZOVA_OK) zova_vector_free(&vector);
    return rc;
}

static int snapshot_validate_native(cbm_zova_legacy_snapshot_t *snapshot,
                                    const char *source_zova_path) {
    zova_message error = {0};
    zova_database_open_options_request open_request = {
        .path = source_zova_path,
        .flags = ZOVA_OPEN_READ_ONLY,
        .busy_timeout_ms = 5000,
        .out_db = &snapshot->source_zova,
        .out_error_message = &error,
    };
    zova_status status = zova_database_open_with_options(&open_request);
    zova_message_free(&error);
    if (status != ZOVA_OK || !snapshot->source_zova) return -1;

    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    if (cbm_zova_workspace_graph_name(snapshot->workspace_id, graph_name, sizeof(graph_name)) != 0)
        return -1;
    zova_graph_info graph = {0};
    status = zova_graph_info_get(&(zova_graph_info_get_request){
        .db = snapshot->source_zova, .name = graph_name, .out_info = &graph});
    if (status != ZOVA_OK || graph.node_count != (uint64_t)snapshot->input.node_count) {
        if (status == ZOVA_OK) zova_graph_info_free(&graph);
        return -1;
    }
    uint64_t native_edge_count = graph.edge_count;
    zova_graph_info_free(&graph);
    uint64_t adjacency_count = 0;
    for (int i = 0; i < snapshot->input.node_count; i++) {
        zova_graph_node node = {0};
        status = zova_graph_node_get(&(zova_graph_node_get_request){
            .db = snapshot->source_zova,
            .graph_name = graph_name,
            .node_id = snapshot->source_ids[i],
            .out_node = &node,
        });
        if (status != ZOVA_OK || !node.node_id || strcmp(node.node_id, snapshot->source_ids[i]) != 0) {
            if (status == ZOVA_OK) zova_graph_node_free(&node);
            return -1;
        }
        zova_graph_node_free(&node);
        zova_graph_neighbor_results neighbors = {0};
        status = zova_graph_neighbors(&(zova_graph_neighbors_request){
            .db = snapshot->source_zova,
            .graph_name = graph_name,
            .node_id = snapshot->source_ids[i],
            .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
            .edge_type = NULL,
            .limit = (size_t)snapshot->input.edge_count + 1,
            .out_results = &neighbors,
        });
        if (status != ZOVA_OK) return -1;
        adjacency_count += neighbors.len;
        for (int e = 0; e < snapshot->input.edge_count; e++) {
            const CBMDumpEdge *edge = &snapshot->edges[e];
            if (edge->source_id != snapshot->nodes[i].id) continue;
            int target_index = -1;
            for (int n = 0; n < snapshot->input.node_count; n++) {
                if (snapshot->nodes[n].id == edge->target_id) target_index = n;
            }
            int found = 0;
            for (size_t n = 0; target_index >= 0 && n < neighbors.len; n++) {
                if (strcmp(neighbors.items[n].node_id, snapshot->source_ids[target_index]) == 0 &&
                    strcmp(neighbors.items[n].edge_type, edge->type) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                zova_graph_neighbor_results_free(&neighbors);
                return -1;
            }
        }
        zova_graph_neighbor_results_free(&neighbors);
    }
    uint64_t unique_sql_edges = 0;
    for (int i = 0; i < snapshot->input.edge_count; i++) {
        int duplicate = 0;
        for (int j = 0; j < i; j++) {
            if (snapshot->edges[i].source_id == snapshot->edges[j].source_id &&
                snapshot->edges[i].target_id == snapshot->edges[j].target_id &&
                strcmp(snapshot->edges[i].type, snapshot->edges[j].type) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) unique_sql_edges++;
    }
    if (adjacency_count != native_edge_count || native_edge_count != unique_sql_edges) return -1;

    zova_vector_collection_list list = {0};
    status = zova_vector_collections_list(&(zova_vector_collections_list_request){
        .db = snapshot->source_zova, .out_list = &list});
    if (status != ZOVA_OK || !snapshot_collection_present(&list, CBM_ZOVA_NODE_COLLECTION) ||
        !snapshot_collection_present(&list, CBM_ZOVA_TOKEN_COLLECTION)) {
        if (status == ZOVA_OK) zova_vector_collection_list_free(&list);
        return -1;
    }
    zova_vector_collection_list_free(&list);
    const char *collections[] = {CBM_ZOVA_NODE_COLLECTION, CBM_ZOVA_TOKEN_COLLECTION};
    uint64_t expected_native_counts[2] = {0, 0};
    for (int i = 0; i < snapshot->input.node_vector_count; i++) {
        if (snapshot_vector_nonzero(snapshot->node_vectors[i].vector,
                                    snapshot->input.vector_dimensions))
            expected_native_counts[0]++;
    }
    for (int i = 0; i < snapshot->input.token_vector_count; i++) {
        if (snapshot_vector_nonzero(snapshot->token_vectors[i].vector,
                                    snapshot->input.vector_dimensions))
            expected_native_counts[1]++;
    }
    for (int i = 0; i < 2; i++) {
        zova_vector_collection_info info = {0};
        status = zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
            .db = snapshot->source_zova, .name = collections[i], .out_info = &info});
        int rc = status == ZOVA_OK &&
                         info.dimensions == (uint32_t)snapshot->input.vector_dimensions &&
                         info.element_type == ZOVA_VECTOR_ELEMENT_TYPE_I8 &&
                         info.vector_count == expected_native_counts[i]
                     ? 0
                     : -1;
        if (status == ZOVA_OK) zova_vector_collection_info_free(&info);
        if (rc != 0) return -1;
    }
    for (int i = 0; i < snapshot->input.node_vector_count; i++) {
        char id[32];
        snprintf(id, sizeof(id), "%lld", (long long)snapshot->node_vectors[i].node_id);
        if (snapshot_validate_vector(snapshot->source_zova, CBM_ZOVA_NODE_COLLECTION, id,
                                     snapshot->node_vectors[i].vector,
                                     snapshot->input.vector_dimensions) != 0)
            return -1;
    }
    for (int i = 0; i < snapshot->input.token_vector_count; i++) {
        char id[32];
        snprintf(id, sizeof(id), "%lld", (long long)snapshot->token_vectors[i].id);
        if (snapshot_validate_vector(snapshot->source_zova, CBM_ZOVA_TOKEN_COLLECTION, id,
                                     snapshot->token_vectors[i].vector,
                                     snapshot->input.vector_dimensions) != 0)
            return -1;
    }
    return 0;
}
#else
static int snapshot_validate_native(cbm_zova_legacy_snapshot_t *snapshot,
                                    const char *source_zova_path) {
    (void)snapshot;
    (void)source_zova_path;
    return -1;
}
#endif

cbm_zova_migration_code_t cbm_zova_legacy_snapshot_open(
    const char *source_db_path, const char *source_zova_path, const char *canonical_root,
    cbm_zova_legacy_snapshot_t **out_snapshot) {
    if (out_snapshot) *out_snapshot = NULL;
    if (!source_db_path || !source_db_path[0] || !source_zova_path || !source_zova_path[0] ||
        !canonical_root || !canonical_root[0] || !out_snapshot) {
        return CBM_ZOVA_MIGRATION_INVALID;
    }
    if (!cbm_file_exists(source_db_path) || !cbm_file_exists(source_zova_path)) {
        return CBM_ZOVA_MIGRATION_SOURCE_MISSING;
    }
    cbm_zova_legacy_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE;
    if (sqlite3_open_v2(source_db_path, &snapshot->source_db, SQLITE_OPEN_READONLY, NULL) !=
            SQLITE_OK ||
        sqlite3_exec(snapshot->source_db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        cbm_zova_legacy_snapshot_close(snapshot);
        return CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE;
    }
    cbm_zova_migration_code_t code = CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE;
    if (snapshot_required_tables(snapshot->source_db) != 0) goto failed;
    if (snapshot_read_project(snapshot, canonical_root) != 0) {
        code = CBM_ZOVA_MIGRATION_SOURCE_NOT_READY;
        goto failed;
    }
    if (snapshot_scalar(snapshot->source_db,
                        "SELECT generation FROM cbm_zova_sidecar_generation_v1 WHERE id=1", NULL,
                        &snapshot->source_generation) != 0 ||
        snapshot->source_generation <= 0) {
        code = CBM_ZOVA_MIGRATION_SOURCE_NOT_READY;
        goto failed;
    }
    if (snapshot_validate_generation(snapshot, source_zova_path) != 0) {
        code = CBM_ZOVA_MIGRATION_SOURCE_NOT_READY;
        goto failed;
    }
    if (snapshot_read_nodes(snapshot) != 0 || snapshot_read_edges(snapshot) != 0 ||
        snapshot_read_node_vectors(snapshot) != 0 || snapshot_read_token_vectors(snapshot) != 0 ||
        snapshot_read_hashes_and_summary(snapshot) != 0 || snapshot_read_fts(snapshot) != 0) {
        goto failed;
    }
    int sidecar_validation = snapshot_validate_sidecar(snapshot, source_zova_path);
    if (sidecar_validation != 0) {
        code = sidecar_validation == -2 ? CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE
                                        : CBM_ZOVA_MIGRATION_SOURCE_NOT_READY;
        goto failed;
    }
    if (snapshot_validate_native(snapshot, source_zova_path) != 0) {
        code = CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE;
        goto failed;
    }
    snapshot->input.root_path = snapshot->root_path;
    snapshot->input.project = snapshot->project;
    snapshot->input.indexed_at = snapshot->indexed_at;
    snapshot->input.model_fingerprint = snapshot->model_fingerprint;
    snapshot->input.nodes = snapshot->nodes;
    snapshot->input.edges = snapshot->edges;
    snapshot->input.node_vectors = snapshot->node_vectors;
    snapshot->input.token_vectors = snapshot->token_vectors;
    snapshot->input.file_hashes = snapshot->file_hashes;
    if (snapshot_build_manifest(snapshot) != 0) goto failed;
    *out_snapshot = snapshot;
    return CBM_ZOVA_MIGRATION_OK;

failed:
    cbm_zova_legacy_snapshot_close(snapshot);
    return code;
}

const cbm_zova_migration_fts_row_t *cbm_zova_legacy_snapshot_fts_rows(
    const cbm_zova_legacy_snapshot_t *snapshot, int *out_count) {
    if (out_count) *out_count = snapshot ? snapshot->fts_row_count : 0;
    return snapshot ? snapshot->fts_rows : NULL;
}

const cbm_zova_migration_manifest_t *cbm_zova_legacy_snapshot_manifest(
    const cbm_zova_legacy_snapshot_t *snapshot) {
    return snapshot ? &snapshot->manifest : NULL;
}

const char *cbm_zova_legacy_snapshot_workspace_id(const cbm_zova_legacy_snapshot_t *snapshot) {
    return snapshot ? snapshot->workspace_id : NULL;
}

const char *cbm_zova_legacy_snapshot_target_id(const cbm_zova_legacy_snapshot_t *snapshot,
                                               int node_index) {
    return snapshot && node_index >= 0 && node_index < snapshot->input.node_count
               ? snapshot->target_ids[node_index]
               : NULL;
}

const char *const *cbm_zova_legacy_snapshot_fts_queries(
    const cbm_zova_legacy_snapshot_t *snapshot, int *out_count) {
    if (out_count) *out_count = snapshot ? snapshot->fts_query_count : 0;
    return snapshot ? (const char *const *)snapshot->fts_queries : NULL;
}

const cbm_zova_workspace_generation_input_t *
cbm_zova_legacy_snapshot_input(const cbm_zova_legacy_snapshot_t *snapshot) {
    return snapshot ? &snapshot->input : NULL;
}

int64_t cbm_zova_legacy_snapshot_source_generation(
    const cbm_zova_legacy_snapshot_t *snapshot) {
    return snapshot ? snapshot->source_generation : 0;
}
