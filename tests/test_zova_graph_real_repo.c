#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/mcp/mcp.h"
#include "../src/store/store.h"
#include "zova/cbm_zova.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ZGR_MAX_SAMPLES = 20,
    ZGR_WARM_REPETITIONS = 30,
    ZGR_DIRECTION_COUNT = 3,
    ZGR_MAX_TIMINGS = ZGR_MAX_SAMPLES * ZGR_WARM_REPETITIONS * ZGR_DIRECTION_COUNT,
    ZGR_WALK_DEPTH = 2,
    ZGR_WALK_LIMIT = 100000,
    ZGR_HYDRATE_BATCH = 500,
};

typedef struct {
    int64_t sqlite_id;
    char *kind;
    char *file_path;
    char *qualified_name;
    int64_t start_line;
    int64_t end_line;
    char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
} zgr_node_t;

typedef struct {
    char **items;
    int count;
    int capacity;
} zgr_ids_t;

/* The current sidecar intentionally keeps SQLite ids out of Zova graph
 * identities. This map models the translation table that the eventual shared
 * cbm.zova metadata schema will persist. Building it is excluded from query
 * timings; resolving it and the batched rich-node read are included. */
typedef struct {
    int64_t sqlite_id;
    char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
} zgr_node_map_t;

typedef struct {
    CBMDumpNode *nodes;
    int node_count;
    CBMDumpEdge *edges;
    int edge_count;
} zgr_dump_graph_t;

static double zgr_percentile(const double *values, int count, int percentile) {
    double sorted[ZGR_MAX_TIMINGS];
    if (!values || count <= 0 || count > ZGR_MAX_TIMINGS) {
        return 0.0;
    }
    for (int i = 0; i < count; i++) {
        sorted[i] = values[i];
    }
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (sorted[j] < sorted[i]) {
                double temp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temp;
            }
        }
    }
    int rank = (percentile * count + 99) / 100;
    return sorted[rank - 1];
}

static double zgr_elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static char *zgr_strdup(const char *value) {
    if (!value) {
        value = "";
    }
    size_t len = strlen(value);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1);
    return copy;
}

static void zgr_dump_graph_free(zgr_dump_graph_t *graph) {
    if (!graph) {
        return;
    }
    for (int i = 0; i < graph->node_count; i++) {
        free((char *)graph->nodes[i].project);
        free((char *)graph->nodes[i].label);
        free((char *)graph->nodes[i].name);
        free((char *)graph->nodes[i].qualified_name);
        free((char *)graph->nodes[i].file_path);
        free((char *)graph->nodes[i].properties);
    }
    for (int i = 0; i < graph->edge_count; i++) {
        free((char *)graph->edges[i].project);
        free((char *)graph->edges[i].type);
        free((char *)graph->edges[i].properties);
        free((char *)graph->edges[i].url_path);
        free((char *)graph->edges[i].local_name);
    }
    free(graph->nodes);
    free(graph->edges);
    memset(graph, 0, sizeof(*graph));
}

static int zgr_dump_graph_append_node(zgr_dump_graph_t *graph, const CBMDumpNode *node) {
    CBMDumpNode *grown = realloc(graph->nodes, (size_t)(graph->node_count + 1) * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    graph->nodes = grown;
    graph->nodes[graph->node_count++] = *node;
    return 0;
}

static int zgr_dump_graph_append_edge(zgr_dump_graph_t *graph, const CBMDumpEdge *edge) {
    CBMDumpEdge *grown = realloc(graph->edges, (size_t)(graph->edge_count + 1) * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    graph->edges = grown;
    graph->edges[graph->edge_count++] = *edge;
    return 0;
}

static const char *zgr_sql_text_or(sqlite3_stmt *stmt, int column, const char *fallback) {
    const char *value = (const char *)sqlite3_column_text(stmt, column);
    return value ? value : fallback;
}

/* Benchmark setup only: rebuild borrowed CBMDump arrays from the completed
 * authoritative DB. This work is deliberately outside both graph-write timers. */
static int zgr_load_dump_graph(sqlite3 *db, const char *project, zgr_dump_graph_t *out) {
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *stmt = NULL;
    const char *nodes_sql =
        "SELECT id, project, label, name, qualified_name, file_path, start_line, end_line, "
        "properties FROM nodes WHERE project = ?1 ORDER BY id";
    int rc = sqlite3_prepare_v2(db, nodes_sql, -1, &stmt, NULL) == SQLITE_OK ? 0 : -1;
    if (rc == 0) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    }
    while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        CBMDumpNode node = {
            .id = sqlite3_column_int64(stmt, 0),
            .project = zgr_strdup(zgr_sql_text_or(stmt, 1, "")),
            .label = zgr_strdup(zgr_sql_text_or(stmt, 2, "")),
            .name = zgr_strdup(zgr_sql_text_or(stmt, 3, "")),
            .qualified_name = zgr_strdup(zgr_sql_text_or(stmt, 4, "")),
            .file_path = zgr_strdup(zgr_sql_text_or(stmt, 5, "")),
            .start_line = sqlite3_column_int(stmt, 6),
            .end_line = sqlite3_column_int(stmt, 7),
            .properties = zgr_strdup(zgr_sql_text_or(stmt, 8, "{}")),
        };
        if (!node.project || !node.label || !node.name || !node.qualified_name || !node.file_path ||
            !node.properties || zgr_dump_graph_append_node(out, &node) != 0) {
            free((char *)node.project);
            free((char *)node.label);
            free((char *)node.name);
            free((char *)node.qualified_name);
            free((char *)node.file_path);
            free((char *)node.properties);
            rc = -1;
        }
    }
    if (rc == 0 && sqlite3_errcode(db) != SQLITE_OK && sqlite3_errcode(db) != SQLITE_DONE) {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    const char *edges_sql =
        "SELECT id, project, source_id, target_id, type, properties, COALESCE(url_path_gen, ''), "
        "COALESCE(local_name_gen, '') FROM edges WHERE project = ?1 ORDER BY id";
    if (rc == 0 && sqlite3_prepare_v2(db, edges_sql, -1, &stmt, NULL) != SQLITE_OK) {
        rc = -1;
    }
    if (rc == 0) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    }
    while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        CBMDumpEdge edge = {
            .id = sqlite3_column_int64(stmt, 0),
            .project = zgr_strdup(zgr_sql_text_or(stmt, 1, "")),
            .source_id = sqlite3_column_int64(stmt, 2),
            .target_id = sqlite3_column_int64(stmt, 3),
            .type = zgr_strdup(zgr_sql_text_or(stmt, 4, "")),
            .properties = zgr_strdup(zgr_sql_text_or(stmt, 5, "{}")),
            .url_path = zgr_strdup(zgr_sql_text_or(stmt, 6, "")),
            .local_name = zgr_strdup(zgr_sql_text_or(stmt, 7, "")),
        };
        if (!edge.project || !edge.type || !edge.properties || !edge.url_path || !edge.local_name ||
            zgr_dump_graph_append_edge(out, &edge) != 0) {
            free((char *)edge.project);
            free((char *)edge.type);
            free((char *)edge.properties);
            free((char *)edge.url_path);
            free((char *)edge.local_name);
            rc = -1;
        }
    }
    if (rc == 0 && sqlite3_errcode(db) != SQLITE_OK && sqlite3_errcode(db) != SQLITE_DONE) {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    if (rc != 0) {
        zgr_dump_graph_free(out);
    }
    return rc;
}

static void zgr_json_write_string(FILE *out, const char *value) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20) {
                fprintf(out, "\\u%04x", *p);
            } else {
                fputc(*p, out);
            }
        }
    }
    fputc('"', out);
}

static void zgr_write_profile_sample(FILE *out, int sample, const char *root,
                                     const char *direction, int repetition, double sqlite_total_ms,
                                     double zova_total_ms,
                                     const cbm_zova_graph_metrics_t *metrics) {
    if (!out || !metrics || !metrics->native_profiled) {
        return;
    }
    fputs("{\"sample\":", out);
    fprintf(out, "%d,\"root\":", sample);
    zgr_json_write_string(out, root);
    fputs(",\"direction\":", out);
    zgr_json_write_string(out, direction);
    fprintf(out,
            ",\"repetition\":%d,\"sqlite_total_ms\":%.6f,\"zova_total_ms\":%.6f,"
            "\"walk_count\":%d,\"native_result_count\":%llu,"
            "\"mutex_wait_ms\":%.6f,\"root_lookup_ms\":%.6f,"
            "\"adjacency_prepare_ms\":%.6f,\"adjacency_execute_ms\":%.6f,"
            "\"bfs_bookkeeping_allocation_ms\":%.6f,"
            "\"c_abi_result_export_ms\":%.6f,\"total_profiled_ms\":%.6f,"
            "\"frontier_expansions\":%llu,\"adjacency_query_binds\":%llu,"
            "\"adjacency_rows_stepped\":%llu}\n",
            repetition, sqlite_total_ms, zova_total_ms, metrics->walk_count,
            (unsigned long long)metrics->native_result_count, metrics->native_mutex_wait_ms,
            metrics->native_root_lookup_ms, metrics->native_adjacency_prepare_ms,
            metrics->native_adjacency_execute_ms, metrics->native_bfs_bookkeeping_allocation_ms,
            metrics->native_c_abi_result_export_ms, metrics->native_total_profiled_ms,
            (unsigned long long)metrics->frontier_expansions,
            (unsigned long long)metrics->adjacency_query_binds,
            (unsigned long long)metrics->adjacency_rows_stepped);
}

static void zgr_node_free(zgr_node_t *node) {
    if (!node) {
        return;
    }
    free(node->kind);
    free(node->file_path);
    free(node->qualified_name);
    memset(node, 0, sizeof(*node));
}

static void zgr_nodes_free(zgr_node_t *nodes, int count) {
    for (int i = 0; i < count; i++) {
        zgr_node_free(&nodes[i]);
    }
}

static void zgr_ids_free(zgr_ids_t *ids) {
    if (!ids) {
        return;
    }
    for (int i = 0; i < ids->count; i++) {
        free(ids->items[i]);
    }
    free(ids->items);
    memset(ids, 0, sizeof(*ids));
}

static int zgr_ids_append(zgr_ids_t *ids, const char *value) {
    if (ids->count == ids->capacity) {
        int next_capacity = ids->capacity ? ids->capacity * 2 : 16;
        char **items = realloc(ids->items, (size_t)next_capacity * sizeof(*items));
        if (!items) {
            return -1;
        }
        ids->items = items;
        ids->capacity = next_capacity;
    }
    ids->items[ids->count] = zgr_strdup(value);
    if (!ids->items[ids->count]) {
        return -1;
    }
    ids->count++;
    return 0;
}

static int zgr_id_compare(const void *a, const void *b) {
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

static bool zgr_ids_equal(zgr_ids_t *left, zgr_ids_t *right) {
    if (left->count != right->count) {
        return false;
    }
    qsort(left->items, (size_t)left->count, sizeof(*left->items), zgr_id_compare);
    qsort(right->items, (size_t)right->count, sizeof(*right->items), zgr_id_compare);
    for (int i = 0; i < left->count; i++) {
        if (strcmp(left->items[i], right->items[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* Zova's walk is canonical BFS: a node is emitted once, at its shortest hop.
 * cbm_store_bfs() currently uses UNION on (node_id, hop), so a cycle can emit
 * the same node at a later hop. Keep both comparisons in the report: raw
 * compatibility, and normalized topology parity. */
static int zgr_ids_normalize_min_hop(const zgr_ids_t *input, zgr_ids_t *out) {
    for (int i = 0; i < input->count; i++) {
        const char *at = strrchr(input->items[i], '@');
        if (!at || at == input->items[i] || at[1] == '\0') {
            return -1;
        }
        char *end = NULL;
        long hop = strtol(at + 1, &end, 10);
        if (*end != '\0' || hop < 0) {
            return -1;
        }
        size_t stable_len = (size_t)(at - input->items[i]);
        int found = -1;
        for (int j = 0; j < out->count; j++) {
            const char *existing_at = strrchr(out->items[j], '@');
            if (existing_at && (size_t)(existing_at - out->items[j]) == stable_len &&
                memcmp(out->items[j], input->items[i], stable_len) == 0) {
                found = j;
                long existing_hop = strtol(existing_at + 1, NULL, 10);
                if (hop < existing_hop) {
                    char *replacement = zgr_strdup(input->items[i]);
                    if (!replacement) {
                        return -1;
                    }
                    free(out->items[j]);
                    out->items[j] = replacement;
                }
                break;
            }
        }
        if (found < 0 && zgr_ids_append(out, input->items[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int zgr_node_stable_id(const char *workspace_id, const char *kind, const char *file_path,
                              const char *qualified_name, int64_t start_line, int64_t end_line,
                              char *out, size_t out_size) {
    char discriminator[64];
    if (qualified_name && qualified_name[0]) {
        snprintf(discriminator, sizeof(discriminator), "named");
    } else if (snprintf(discriminator, sizeof(discriminator), "anon:%lld:%lld",
                        (long long)start_line, (long long)end_line) >=
               (int)sizeof(discriminator)) {
        return -1;
    }
    return cbm_zova_workspace_node_id_v1(workspace_id, kind ? kind : "", file_path ? file_path : "",
                                         qualified_name ? qualified_name : "", discriminator, out,
                                         out_size);
}

static int zgr_node_from_row(sqlite3_stmt *stmt, int first_column, const char *workspace_id,
                             zgr_node_t *out) {
    memset(out, 0, sizeof(*out));
    out->sqlite_id = sqlite3_column_int64(stmt, first_column);
    out->kind = zgr_strdup((const char *)sqlite3_column_text(stmt, first_column + 1));
    out->file_path = zgr_strdup((const char *)sqlite3_column_text(stmt, first_column + 2));
    out->qualified_name = zgr_strdup((const char *)sqlite3_column_text(stmt, first_column + 3));
    out->start_line = sqlite3_column_int64(stmt, first_column + 4);
    out->end_line = sqlite3_column_int64(stmt, first_column + 5);
    if (!out->kind || !out->file_path || !out->qualified_name ||
        zgr_node_stable_id(workspace_id, out->kind, out->file_path, out->qualified_name,
                           out->start_line, out->end_line, out->stable_id,
                           sizeof(out->stable_id)) != 0) {
        zgr_node_free(out);
        return -1;
    }
    return 0;
}

static int zgr_node_map_compare(const void *left, const void *right) {
    const zgr_node_map_t *a = left;
    const zgr_node_map_t *b = right;
    return strcmp(a->stable_id, b->stable_id);
}

static int zgr_load_project_node_map(sqlite3 *db, const char *project, const char *workspace_id,
                                     zgr_node_map_t **out_items, int *out_count) {
    const char *sql =
        "SELECT id,label,file_path,qualified_name,start_line,end_line FROM nodes "
        "WHERE project = ?1 ORDER BY id";
    *out_items = NULL;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    int capacity = 0;
    int rc = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*out_count == capacity) {
            int next_capacity = capacity ? capacity * 2 : 128;
            zgr_node_map_t *grown = realloc(*out_items, (size_t)next_capacity * sizeof(**out_items));
            if (!grown) {
                rc = -1;
                break;
            }
            *out_items = grown;
            capacity = next_capacity;
        }
        zgr_node_t node;
        if (zgr_node_from_row(stmt, 0, workspace_id, &node) != 0) {
            rc = -1;
            break;
        }
        (*out_items)[*out_count].sqlite_id = node.sqlite_id;
        snprintf((*out_items)[*out_count].stable_id, sizeof((*out_items)[*out_count].stable_id),
                 "%s", node.stable_id);
        (*out_count)++;
        zgr_node_free(&node);
    }
    sqlite3_finalize(stmt);
    if (rc != 0) {
        free(*out_items);
        *out_items = NULL;
        *out_count = 0;
        return -1;
    }
    qsort(*out_items, (size_t)*out_count, sizeof(**out_items), zgr_node_map_compare);
    return 0;
}

static const zgr_node_map_t *zgr_node_map_find(const zgr_node_map_t *items, int count,
                                                const char *stable_id) {
    int low = 0;
    int high = count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        int comparison = strcmp(items[middle].stable_id, stable_id);
        if (comparison < 0) {
            low = middle + 1;
        } else if (comparison > 0) {
            high = middle;
        } else {
            return &items[middle];
        }
    }
    return NULL;
}

static int zgr_identity_stable_id(const char *identity, char *out, size_t out_size) {
    const char *at = strrchr(identity, '@');
    if (!at || at == identity || (size_t)(at - identity) >= out_size) {
        return -1;
    }
    memcpy(out, identity, (size_t)(at - identity));
    out[at - identity] = '\0';
    return 0;
}

static int zgr_sql_open(const char *path, sqlite3 **out_db) {
    *out_db = NULL;
    return sqlite3_open_v2(path, out_db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK ? 0 : -1;
}

static int zgr_sql_project_count(sqlite3 *db, const char *table, const char *project,
                                 int64_t *out_count) {
    char sql[128];
    if (snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s WHERE project = ?1", table) >=
        (int)sizeof(sql)) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    if (rc == 0) {
        *out_count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return rc;
}

/* Zova topology has one directed typed edge per (source, type, target). CBM
 * can retain several IMPORTS rows for that same topology when their rich edge
 * properties (for example local_name) differ. */
static int zgr_sql_project_topology_edge_count(sqlite3 *db, const char *project,
                                               int64_t *out_count) {
    const char *sql =
        "SELECT count(*) FROM (SELECT DISTINCT source_id,type,target_id FROM edges "
        "WHERE project = ?1)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    if (rc == 0) {
        *out_count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int zgr_load_samples(sqlite3 *db, const char *project, const char *workspace_id,
                            zgr_node_t *out_nodes, int *out_count) {
    const char *sql =
        "SELECT n.id,n.label,n.file_path,n.qualified_name,n.start_line,n.end_line "
        "FROM nodes n WHERE n.project = ?1 AND n.label IN ('Function','Method') "
        "AND EXISTS(SELECT 1 FROM edges e WHERE e.project = ?1 AND e.source_id = n.id "
        "AND e.type = 'CALLS') ORDER BY n.qualified_name,n.file_path,n.id LIMIT ?2";
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, ZGR_MAX_SAMPLES);
    int rc = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*out_count == ZGR_MAX_SAMPLES ||
            zgr_node_from_row(stmt, 0, workspace_id, &out_nodes[*out_count]) != 0) {
            rc = -1;
            break;
        }
        (*out_count)++;
    }
    sqlite3_finalize(stmt);
    if (rc != 0) {
        zgr_nodes_free(out_nodes, *out_count);
        *out_count = 0;
    }
    return rc;
}

static int zgr_sql_degree_calls(sqlite3 *db, const char *project, int64_t node_id,
                                uint64_t *out_degree, double *out_ms) {
    const char *sql =
        "SELECT count(*) FROM edges WHERE project = ?1 AND source_id = ?2 AND type = 'CALLS'";
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK ? 0 : -1;
    if (rc == 0) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, node_id);
        rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    }
    if (rc == 0) {
        *out_degree = (uint64_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    return rc;
}

static int zgr_sql_neighbors_calls(sqlite3 *db, const char *project, int64_t node_id,
                                   const char *workspace_id, zgr_ids_t *out_ids,
                                   double *out_ms) {
    const char *sql =
        "SELECT n.id,n.label,n.file_path,n.qualified_name,n.start_line,n.end_line "
        "FROM edges e JOIN nodes n ON n.id = e.target_id "
        "WHERE e.project = ?1 AND e.source_id = ?2 AND e.type = 'CALLS' ORDER BY e.id";
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK ? 0 : -1;
    if (rc == 0) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, node_id);
    }
    while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        zgr_node_t node;
        if (zgr_node_from_row(stmt, 0, workspace_id, &node) != 0 ||
            zgr_ids_append(out_ids, node.stable_id) != 0) {
            zgr_node_free(&node);
            rc = -1;
            break;
        }
        zgr_node_free(&node);
    }
    sqlite3_finalize(stmt);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    return rc;
}

static int zgr_sql_walk_calls(sqlite3 *db, const char *project, int64_t start_node_id,
                              const char *workspace_id, zgr_ids_t *out_ids, double *out_ms) {
    const char *sql =
        "WITH RECURSIVE walk(node_id,depth) AS ("
        "SELECT ?1,0 UNION ALL "
        "SELECT e.target_id,walk.depth+1 FROM edges e JOIN walk ON e.source_id=walk.node_id "
        "WHERE e.project=?2 AND e.type='CALLS' AND walk.depth < ?3),"
        "shortest AS (SELECT node_id,MIN(depth) AS depth FROM walk GROUP BY node_id) "
        "SELECT n.id,n.label,n.file_path,n.qualified_name,n.start_line,n.end_line,shortest.depth "
        "FROM shortest JOIN nodes n ON n.id=shortest.node_id WHERE n.project=?2 "
        "ORDER BY shortest.depth,n.id LIMIT ?4";
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK ? 0 : -1;
    if (rc == 0) {
        sqlite3_bind_int64(stmt, 1, start_node_id);
        sqlite3_bind_text(stmt, 2, project, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, ZGR_WALK_DEPTH);
        sqlite3_bind_int(stmt, 4, ZGR_WALK_LIMIT);
    }
    while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        zgr_node_t node;
        char identity[sizeof(node.stable_id) + 32];
        if (zgr_node_from_row(stmt, 0, workspace_id, &node) != 0 ||
            snprintf(identity, sizeof(identity), "%s@%lld", node.stable_id,
                     (long long)sqlite3_column_int64(stmt, 6)) >= (int)sizeof(identity) ||
            zgr_ids_append(out_ids, identity) != 0) {
            zgr_node_free(&node);
            rc = -1;
            break;
        }
        zgr_node_free(&node);
    }
    sqlite3_finalize(stmt);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    return rc;
}

static int zgr_store_bfs_calls(cbm_store_t *store, const char *workspace_id,
                               const zgr_node_t *sample, zgr_ids_t *out_ids,
                               int *out_edge_count, double *out_ms) {
    const char *edge_types[] = {"CALLS"};
    cbm_traverse_result_t result = {0};
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = cbm_store_bfs(store, sample->sqlite_id, "outbound", edge_types, 1, ZGR_WALK_DEPTH,
                           ZGR_WALK_LIMIT, &result);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    *out_edge_count = result.edge_count;
    if (rc != CBM_STORE_OK) {
        cbm_store_traverse_free(&result);
        return -1;
    }

    char identity[CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    if (snprintf(identity, sizeof(identity), "%s@0", sample->stable_id) >= (int)sizeof(identity) ||
        zgr_ids_append(out_ids, identity) != 0) {
        cbm_store_traverse_free(&result);
        return -1;
    }
    for (int i = 0; i < result.visited_count; i++) {
        cbm_node_t *node = &result.visited[i].node;
        char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (zgr_node_stable_id(workspace_id, node->label, node->file_path, node->qualified_name,
                               node->start_line, node->end_line, stable_id, sizeof(stable_id)) != 0 ||
            snprintf(identity, sizeof(identity), "%s@%d", stable_id, result.visited[i].hop) >=
                (int)sizeof(identity) ||
            zgr_ids_append(out_ids, identity) != 0) {
            cbm_store_traverse_free(&result);
            return -1;
        }
    }
    cbm_store_traverse_free(&result);
    return 0;
}

/* Fetch the same rich node columns cbm_store_bfs() materializes. The stable
 * id map was created during setup, not on the measured request path. */
static int zgr_sql_hydrate_walk_nodes(sqlite3 *db, const char *project,
                                      const zgr_node_map_t *node_map, int node_map_count,
                                      const zgr_ids_t *walk_ids, int *out_count, double *out_ms) {
    int64_t ids[ZGR_HYDRATE_BATCH];
    int id_count = 0;
    int hydrated = 0;
    int rc = 0;
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    for (int offset = 0; rc == 0 && offset < walk_ids->count; offset += ZGR_HYDRATE_BATCH) {
        id_count = walk_ids->count - offset;
        if (id_count > ZGR_HYDRATE_BATCH) {
            id_count = ZGR_HYDRATE_BATCH;
        }
        for (int i = 0; i < id_count; i++) {
            char stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
            if (zgr_identity_stable_id(walk_ids->items[offset + i], stable_id,
                                       sizeof(stable_id)) != 0) {
                rc = -1;
                break;
            }
            const zgr_node_map_t *entry = zgr_node_map_find(node_map, node_map_count, stable_id);
            if (!entry) {
                rc = -1;
                break;
            }
            ids[i] = entry->sqlite_id;
        }
        if (rc != 0) {
            break;
        }
        char sql[8192] =
            "SELECT id,project,label,name,qualified_name,file_path,start_line,end_line,properties "
            "FROM nodes WHERE project = ?1 AND id IN (";
        size_t length = strlen(sql);
        for (int i = 0; i < id_count; i++) {
            int written = snprintf(sql + length, sizeof(sql) - length, "%s?%d", i ? "," : "",
                                   i + 2);
            if (written < 0 || (size_t)written >= sizeof(sql) - length) {
                rc = -1;
                break;
            }
            length += (size_t)written;
        }
        if (rc != 0 || snprintf(sql + length, sizeof(sql) - length, ")") >=
                           (int)(sizeof(sql) - length)) {
            rc = -1;
            break;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            rc = -1;
            break;
        }
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
        for (int i = 0; i < id_count; i++) {
            sqlite3_bind_int64(stmt, i + 2, ids[i]);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            /* Touch every column so this measures rich-node materialization,
             * not only an id-only existence check. */
            (void)sqlite3_column_int64(stmt, 0);
            for (int column = 1; column <= 5; column++) {
                (void)sqlite3_column_text(stmt, column);
            }
            (void)sqlite3_column_int64(stmt, 6);
            (void)sqlite3_column_int64(stmt, 7);
            (void)sqlite3_column_text(stmt, 8);
            hydrated++;
        }
        sqlite3_finalize(stmt);
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    *out_count = hydrated;
    return rc == 0 && hydrated == walk_ids->count ? 0 : -1;
}

#if CBM_WITH_ZOVA

static int zgr_open_zova(const char *path, zova_database **out_db) {
    *out_db = NULL;
    zova_message error = {0};
    zova_database_open_request request = {
        .path = path,
        .out_db = out_db,
        .out_error_message = &error,
    };
    zova_status status = zova_database_open(&request);
    zova_message_free(&error);
    return status == ZOVA_OK ? 0 : -1;
}

static int zgr_zova_degree_calls(zova_database *db, const char *graph_name, const char *node_id,
                                 uint64_t *out_degree, double *out_ms) {
    zova_graph_degree_request request = {
        .db = db,
        .graph_name = graph_name,
        .node_id = node_id,
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = "CALLS",
        .out_degree = out_degree,
    };
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    zova_status status = zova_graph_degree(&request);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    return status == ZOVA_OK ? 0 : -1;
}

static int zgr_zova_neighbors_calls(zova_database *db, const char *graph_name, const char *node_id,
                                    zgr_ids_t *out_ids, double *out_ms) {
    zova_graph_neighbor_results results = {0};
    zova_graph_neighbors_request request = {
        .db = db,
        .graph_name = graph_name,
        .node_id = node_id,
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = "CALLS",
        .limit = ZGR_WALK_LIMIT,
        .out_results = &results,
    };
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    zova_status status = zova_graph_neighbors(&request);
    int rc = status == ZOVA_OK ? 0 : -1;
    if (status == ZOVA_OK) {
        for (size_t i = 0; i < results.len; i++) {
            if (zgr_ids_append(out_ids, results.items[i].node_id) != 0) {
                rc = -1;
                break;
            }
        }
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    zova_graph_neighbor_results_free(&results);
    return rc;
}

static int zgr_zova_walk_calls(zova_database *db, const char *graph_name, const char *node_id,
                               zgr_ids_t *out_ids, double *out_ms) {
    zova_graph_walk_results results = {0};
    zova_graph_walk_request request = {
        .db = db,
        .graph_name = graph_name,
        .start_node_id = node_id,
        .edge_type = "CALLS",
        .max_depth = ZGR_WALK_DEPTH,
        .limit = ZGR_WALK_LIMIT,
        .out_results = &results,
    };
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    zova_status status = zova_graph_walk(&request);
    int rc = status == ZOVA_OK ? 0 : -1;
    if (status == ZOVA_OK) {
        for (size_t i = 0; i < results.len; i++) {
            char identity[CBM_ZOVA_WORKSPACE_ID_MAX + 96];
            if (snprintf(identity, sizeof(identity), "%s@%u", results.items[i].node_id,
                         results.items[i].depth) >= (int)sizeof(identity) ||
                zgr_ids_append(out_ids, identity) != 0) {
                rc = -1;
                break;
            }
        }
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    zova_graph_walk_results_free(&results);
    return rc;
}

static void zgr_json_string(FILE *file, const char *text) {
    fputc('"', file);
    for (const unsigned char *cursor = (const unsigned char *)(text ? text : ""); *cursor;
         cursor++) {
        switch (*cursor) {
        case '"':
            fputs("\\\"", file);
            break;
        case '\\':
            fputs("\\\\", file);
            break;
        case '\n':
            fputs("\\n", file);
            break;
        case '\r':
            fputs("\\r", file);
            break;
        case '\t':
            fputs("\\t", file);
            break;
        default:
            fputc(*cursor, file);
            break;
        }
    }
    fputc('"', file);
}

TEST(zova_graph_real_repo_report_and_parity) {
    const char *db_path = getenv("CBM_ZOVA_REAL_DB");
    const char *zova_path = getenv("CBM_ZOVA_REAL_ZOVA");
    const char *project = getenv("CBM_ZOVA_REAL_PROJECT");
    const char *repo_path = getenv("CBM_ZOVA_REAL_REPO");
    const char *registry_path = getenv("CBM_ZOVA_REAL_GRAPH_REGISTRY");
    const char *report_path = getenv("CBM_ZOVA_REAL_GRAPH_REPORT");
    ASSERT_NOT_NULL(db_path);
    ASSERT_NOT_NULL(zova_path);
    ASSERT_NOT_NULL(project);
    ASSERT_NOT_NULL(repo_path);
    ASSERT_NOT_NULL(registry_path);
    ASSERT_NOT_NULL(report_path);

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32] = {0};
    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, repo_path, workspace_id,
                                                   sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);

    sqlite3 *sqlite_db = NULL;
    ASSERT_EQ(zgr_sql_open(db_path, &sqlite_db), 0);
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(store);
    int64_t sqlite_nodes = 0;
    int64_t sqlite_edge_rows = 0;
    int64_t sqlite_topology_edges = 0;
    ASSERT_EQ(zgr_sql_project_count(sqlite_db, "nodes", project, &sqlite_nodes), 0);
    ASSERT_EQ(zgr_sql_project_count(sqlite_db, "edges", project, &sqlite_edge_rows), 0);
    ASSERT_EQ(zgr_sql_project_topology_edge_count(sqlite_db, project, &sqlite_topology_edges),
              0);

    zgr_dump_graph_t dump_graph = {0};
    ASSERT_EQ(zgr_load_dump_graph(sqlite_db, project, &dump_graph), 0);
    ASSERT_EQ(dump_graph.node_count, sqlite_nodes);
    ASSERT_EQ(dump_graph.edge_count, sqlite_edge_rows);
    char ingestion_direct_path[1024];
    char mirror_path[1024];
    ASSERT_TRUE(snprintf(ingestion_direct_path, sizeof(ingestion_direct_path),
                         "%s.ingestion-direct.zova", zova_path) <
                (int)sizeof(ingestion_direct_path));
    ASSERT_TRUE(snprintf(mirror_path, sizeof(mirror_path), "%s.ingestion-mirror.zova", zova_path) <
                (int)sizeof(mirror_path));
    cbm_unlink(ingestion_direct_path);
    cbm_unlink(mirror_path);
    cbm_zova_graph_ingestion_metrics_t ingestion = {0};
    ASSERT_EQ(cbm_zova_benchmark_workspace_graph_ingestion(
                  db_path, ingestion_direct_path, mirror_path, workspace_id, project,
                  dump_graph.nodes, dump_graph.node_count, dump_graph.edges, dump_graph.edge_count,
                  &ingestion),
              0);

    zova_database *zova_db = NULL;
    ASSERT_EQ(zgr_open_zova(zova_path, &zova_db), 0);
    zova_database *mirror_zova_db = NULL;
    ASSERT_EQ(zgr_open_zova(mirror_path, &mirror_zova_db), 0);
    zova_graph_info graph_info = {0};
    zova_graph_info_get_request graph_info_request = {
        .db = zova_db,
        .name = graph_name,
        .out_info = &graph_info,
    };
    ASSERT_EQ(zova_graph_info_get(&graph_info_request), ZOVA_OK);
    uint64_t zova_nodes = graph_info.node_count;
    uint64_t zova_edges = graph_info.edge_count;
    zova_graph_info_free(&graph_info);
    zova_graph_info mirror_graph_info = {0};
    zova_graph_info_get_request mirror_graph_info_request = {
        .db = mirror_zova_db,
        .name = graph_name,
        .out_info = &mirror_graph_info,
    };
    ASSERT_EQ(zova_graph_info_get(&mirror_graph_info_request), ZOVA_OK);
    uint64_t mirror_nodes = mirror_graph_info.node_count;
    uint64_t mirror_edges = mirror_graph_info.edge_count;
    zova_graph_info_free(&mirror_graph_info);
    ASSERT_EQ(zova_nodes, (uint64_t)sqlite_nodes);
    ASSERT_EQ(zova_edges, (uint64_t)sqlite_topology_edges);
    ASSERT_EQ(zova_nodes, mirror_nodes);
    ASSERT_EQ(zova_edges, mirror_edges);
    ASSERT_EQ(ingestion.direct_node_count, zova_nodes);
    ASSERT_EQ(ingestion.direct_edge_count, zova_edges);
    ASSERT_EQ(ingestion.mirror_node_count, mirror_nodes);
    ASSERT_EQ(ingestion.mirror_edge_count, mirror_edges);

    zgr_node_t samples[ZGR_MAX_SAMPLES];
    memset(samples, 0, sizeof(samples));
    int sample_count = 0;
    ASSERT_EQ(zgr_load_samples(sqlite_db, project, workspace_id, samples, &sample_count), 0);
    ASSERT_GT(sample_count, 0);
    zgr_node_map_t *node_map = NULL;
    int node_map_count = 0;
    ASSERT_EQ(zgr_load_project_node_map(sqlite_db, project, workspace_id, &node_map, &node_map_count),
              0);
    ASSERT_EQ(node_map_count, sqlite_nodes);

    double sqlite_degree_ms[ZGR_MAX_TIMINGS] = {0};
    double zova_degree_ms[ZGR_MAX_TIMINGS] = {0};
    double sqlite_neighbors_ms[ZGR_MAX_TIMINGS] = {0};
    double zova_neighbors_ms[ZGR_MAX_TIMINGS] = {0};
    double sqlite_walk_ms[ZGR_MAX_TIMINGS] = {0};
    double zova_walk_ms[ZGR_MAX_TIMINGS] = {0};
    double store_bfs_ms[ZGR_MAX_TIMINGS] = {0};
    double zova_bfs_walk_ms[ZGR_MAX_TIMINGS] = {0};
    double zova_bfs_hydration_ms[ZGR_MAX_TIMINGS] = {0};
    double zova_bfs_total_ms[ZGR_MAX_TIMINGS] = {0};
    int degree_mismatches = 0;
    int neighbor_mismatches = 0;
    int walk_mismatches = 0;
    int direct_mirror_degree_mismatches = 0;
    int direct_mirror_neighbor_mismatches = 0;
    int direct_mirror_walk_mismatches = 0;
    int store_bfs_raw_mismatches = 0;
    int store_bfs_normalized_mismatches = 0;
    int zova_bfs_hydration_mismatches = 0;
    int store_bfs_edge_count_total = 0;

    for (int i = 0; i < sample_count; i++) {
        uint64_t sqlite_degree = 0;
        uint64_t zova_degree = 0;
        ASSERT_EQ(zgr_sql_degree_calls(sqlite_db, project, samples[i].sqlite_id, &sqlite_degree,
                                       &sqlite_degree_ms[i]),
                  0);
        ASSERT_EQ(zgr_zova_degree_calls(zova_db, graph_name, samples[i].stable_id, &zova_degree,
                                        &zova_degree_ms[i]),
                  0);
        uint64_t mirror_degree = 0;
        double mirror_degree_ms = 0.0;
        ASSERT_EQ(zgr_zova_degree_calls(mirror_zova_db, graph_name, samples[i].stable_id,
                                        &mirror_degree, &mirror_degree_ms),
                  0);
        if (sqlite_degree != zova_degree) {
            degree_mismatches++;
        }
        if (zova_degree != mirror_degree) {
            direct_mirror_degree_mismatches++;
        }

        zgr_ids_t sqlite_neighbors = {0};
        zgr_ids_t zova_neighbors = {0};
        ASSERT_EQ(zgr_sql_neighbors_calls(sqlite_db, project, samples[i].sqlite_id, workspace_id,
                                          &sqlite_neighbors, &sqlite_neighbors_ms[i]),
                  0);
        ASSERT_EQ(zgr_zova_neighbors_calls(zova_db, graph_name, samples[i].stable_id,
                                           &zova_neighbors, &zova_neighbors_ms[i]),
                  0);
        zgr_ids_t mirror_neighbors = {0};
        double mirror_neighbors_ms = 0.0;
        ASSERT_EQ(zgr_zova_neighbors_calls(mirror_zova_db, graph_name, samples[i].stable_id,
                                           &mirror_neighbors, &mirror_neighbors_ms),
                  0);
        if (!zgr_ids_equal(&sqlite_neighbors, &zova_neighbors)) {
            neighbor_mismatches++;
        }
        if (!zgr_ids_equal(&zova_neighbors, &mirror_neighbors)) {
            direct_mirror_neighbor_mismatches++;
        }
        zgr_ids_free(&sqlite_neighbors);
        zgr_ids_free(&zova_neighbors);
        zgr_ids_free(&mirror_neighbors);

        zgr_ids_t sqlite_walk = {0};
        zgr_ids_t zova_walk = {0};
        ASSERT_EQ(zgr_sql_walk_calls(sqlite_db, project, samples[i].sqlite_id, workspace_id,
                                     &sqlite_walk, &sqlite_walk_ms[i]),
                  0);
        ASSERT_EQ(zgr_zova_walk_calls(zova_db, graph_name, samples[i].stable_id, &zova_walk,
                                      &zova_walk_ms[i]),
                  0);
        zgr_ids_t mirror_walk = {0};
        double mirror_walk_ms = 0.0;
        ASSERT_EQ(zgr_zova_walk_calls(mirror_zova_db, graph_name, samples[i].stable_id,
                                      &mirror_walk, &mirror_walk_ms),
                  0);
        if (!zgr_ids_equal(&sqlite_walk, &zova_walk)) {
            walk_mismatches++;
        }
        if (!zgr_ids_equal(&zova_walk, &mirror_walk)) {
            direct_mirror_walk_mismatches++;
        }
        zgr_ids_free(&sqlite_walk);
        zgr_ids_free(&zova_walk);
        zgr_ids_free(&mirror_walk);

        zgr_ids_t store_bfs = {0};
        zgr_ids_t zova_bfs = {0};
        int store_bfs_edge_count = 0;
        int zova_hydrated_count = 0;
        ASSERT_EQ(zgr_store_bfs_calls(store, workspace_id, &samples[i], &store_bfs,
                                      &store_bfs_edge_count, &store_bfs_ms[i]),
                  0);
        ASSERT_EQ(zgr_zova_walk_calls(zova_db, graph_name, samples[i].stable_id, &zova_bfs,
                                      &zova_bfs_walk_ms[i]),
                  0);
        ASSERT_EQ(zgr_sql_hydrate_walk_nodes(sqlite_db, project, node_map, node_map_count,
                                             &zova_bfs, &zova_hydrated_count,
                                             &zova_bfs_hydration_ms[i]),
                  0);
        zova_bfs_total_ms[i] = zova_bfs_walk_ms[i] + zova_bfs_hydration_ms[i];
        zgr_ids_t normalized_store_bfs = {0};
        ASSERT_EQ(zgr_ids_normalize_min_hop(&store_bfs, &normalized_store_bfs), 0);
        if (!zgr_ids_equal(&store_bfs, &zova_bfs)) {
            store_bfs_raw_mismatches++;
        }
        if (!zgr_ids_equal(&normalized_store_bfs, &zova_bfs)) {
            store_bfs_normalized_mismatches++;
        }
        if (zova_hydrated_count != zova_bfs.count) {
            zova_bfs_hydration_mismatches++;
        }
        store_bfs_edge_count_total += store_bfs_edge_count;
        zgr_ids_free(&store_bfs);
        zgr_ids_free(&normalized_store_bfs);
        zgr_ids_free(&zova_bfs);
    }

    ASSERT_EQ(degree_mismatches, 0);
    ASSERT_EQ(neighbor_mismatches, 0);
    ASSERT_EQ(walk_mismatches, 0);
    ASSERT_EQ(direct_mirror_degree_mismatches, 0);
    ASSERT_EQ(direct_mirror_neighbor_mismatches, 0);
    ASSERT_EQ(direct_mirror_walk_mismatches, 0);
    ASSERT_EQ(store_bfs_normalized_mismatches, 0);
    ASSERT_EQ(zova_bfs_hydration_mismatches, 0);

    FILE *report = fopen(report_path, "wb");
    ASSERT_NOT_NULL(report);
    fputs("{\n  \"repo_path\": ", report);
    zgr_json_string(report, repo_path);
    fputs(",\n  \"project\": ", report);
    zgr_json_string(report, project);
    fputs(",\n  \"workspace_id\": ", report);
    zgr_json_string(report, workspace_id);
    fputs(",\n  \"graph_name\": ", report);
    zgr_json_string(report, graph_name);
    fprintf(report,
            ",\n  \"sqlite_node_count\": %lld,\n"
            "  \"direct_node_count\": %llu,\n"
            "  \"mirror_node_count\": %llu,\n"
            "  \"sqlite_edge_row_count\": %lld,\n"
            "  \"sqlite_topology_edge_count\": %lld,\n"
            "  \"direct_edge_count\": %llu,\n"
            "  \"mirror_edge_count\": %llu,\n"
            "  \"sidecar_topology_source\": \"direct_graph_buffer\",\n"
            "  \"ingestion_benchmark\": {\"scope\": \"workspace graph materialization only; "
            "conversion and dump-array loading excluded\", \"direct_graph_write_ms\": %.3f, "
            "\"sqlite_row_mirror_ms\": %.3f, \"direct_node_count\": %llu, "
            "\"direct_edge_count\": %llu, \"mirror_node_count\": %llu, "
            "\"mirror_edge_count\": %llu},\n"
            "  \"sample_count\": %d,\n"
            "  \"parity\": {\"degree_mismatches\": %d, \"neighbor_mismatches\": %d, "
            "\"walk_mismatches\": %d, \"store_bfs_raw_mismatches\": %d, "
            "\"store_bfs_normalized_mismatches\": %d, "
            "\"zova_bfs_hydration_mismatches\": %d, "
            "\"direct_mirror_degree_mismatches\": %d, "
            "\"direct_mirror_neighbor_mismatches\": %d, "
            "\"direct_mirror_walk_mismatches\": %d, "
            "\"topology_parity_passed\": %s, \"raw_store_bfs_compatible\": %s},\n"
            "  \"query_method\": \"raw typed CALLS topology primitives; SQLite includes "
            "stable-ID projection\",\n"
            "  \"actual_bfs_scope\": {\"operation\": \"cbm_store_bfs\", "
            "\"direction\": \"outbound\", \"edge_type\": \"CALLS\", "
            "\"max_depth\": %d, \"max_results\": %d, "
            "\"parity\": \"raw and min-hop-normalized node_id+hop sets; CBM only orders ties by hop\", "
            "\"zova_hydration\": \"batched rich-node SQLite read via setup-time stable-id map\", "
            "\"store_edges_materialized\": %d},\n"
            "  \"performance_ms\": {\n"
            "    \"sqlite_degree_p50\": %.3f, \"sqlite_degree_p95\": %.3f,\n"
            "    \"zova_degree_p50\": %.3f, \"zova_degree_p95\": %.3f,\n"
            "    \"sqlite_neighbors_p50\": %.3f, \"sqlite_neighbors_p95\": %.3f,\n"
            "    \"zova_neighbors_p50\": %.3f, \"zova_neighbors_p95\": %.3f,\n"
            "    \"sqlite_walk_p50\": %.3f, \"sqlite_walk_p95\": %.3f,\n"
            "    \"zova_walk_p50\": %.3f, \"zova_walk_p95\": %.3f,\n"
            "    \"store_bfs_p50\": %.3f, \"store_bfs_p95\": %.3f,\n"
            "    \"zova_bfs_walk_p50\": %.3f, \"zova_bfs_walk_p95\": %.3f,\n"
            "    \"zova_bfs_hydration_p50\": %.3f, \"zova_bfs_hydration_p95\": %.3f,\n"
            "    \"zova_bfs_walk_plus_hydration_p50\": %.3f, "
            "\"zova_bfs_walk_plus_hydration_p95\": %.3f\n"
            "  }\n}\n",
            (long long)sqlite_nodes, (unsigned long long)zova_nodes,
            (unsigned long long)mirror_nodes,
            (long long)sqlite_edge_rows, (long long)sqlite_topology_edges,
            (unsigned long long)zova_edges, (unsigned long long)mirror_edges,
            ingestion.direct_graph_write_ms, ingestion.sqlite_row_mirror_ms,
            (unsigned long long)ingestion.direct_node_count,
            (unsigned long long)ingestion.direct_edge_count,
            (unsigned long long)ingestion.mirror_node_count,
            (unsigned long long)ingestion.mirror_edge_count,
            sample_count, degree_mismatches,
            neighbor_mismatches, walk_mismatches, store_bfs_raw_mismatches,
            store_bfs_normalized_mismatches,
            zova_bfs_hydration_mismatches, direct_mirror_degree_mismatches,
            direct_mirror_neighbor_mismatches, direct_mirror_walk_mismatches,
            degree_mismatches == 0 && neighbor_mismatches == 0 && walk_mismatches == 0 &&
                    store_bfs_normalized_mismatches == 0 && zova_bfs_hydration_mismatches == 0 &&
                    direct_mirror_degree_mismatches == 0 &&
                    direct_mirror_neighbor_mismatches == 0 &&
                    direct_mirror_walk_mismatches == 0
                ? "true"
                : "false",
            store_bfs_raw_mismatches == 0 ? "true" : "false",
            ZGR_WALK_DEPTH, ZGR_WALK_LIMIT, store_bfs_edge_count_total,
            zgr_percentile(sqlite_degree_ms, sample_count, 50),
            zgr_percentile(sqlite_degree_ms, sample_count, 95),
            zgr_percentile(zova_degree_ms, sample_count, 50),
            zgr_percentile(zova_degree_ms, sample_count, 95),
            zgr_percentile(sqlite_neighbors_ms, sample_count, 50),
            zgr_percentile(sqlite_neighbors_ms, sample_count, 95),
            zgr_percentile(zova_neighbors_ms, sample_count, 50),
            zgr_percentile(zova_neighbors_ms, sample_count, 95),
            zgr_percentile(sqlite_walk_ms, sample_count, 50),
            zgr_percentile(sqlite_walk_ms, sample_count, 95),
            zgr_percentile(zova_walk_ms, sample_count, 50),
            zgr_percentile(zova_walk_ms, sample_count, 95),
            zgr_percentile(store_bfs_ms, sample_count, 50),
            zgr_percentile(store_bfs_ms, sample_count, 95),
            zgr_percentile(zova_bfs_walk_ms, sample_count, 50),
            zgr_percentile(zova_bfs_walk_ms, sample_count, 95),
            zgr_percentile(zova_bfs_hydration_ms, sample_count, 50),
            zgr_percentile(zova_bfs_hydration_ms, sample_count, 95),
            zgr_percentile(zova_bfs_total_ms, sample_count, 50),
            zgr_percentile(zova_bfs_total_ms, sample_count, 95));
    ASSERT_EQ(fclose(report), 0);

    zgr_nodes_free(samples, sample_count);
    free(node_map);
    zgr_dump_graph_free(&dump_graph);
    cbm_store_close(store);
    sqlite3_close(sqlite_db);
    ASSERT_EQ(zova_database_close(zova_db), ZOVA_OK);
    ASSERT_EQ(zova_database_close(mirror_zova_db), ZOVA_OK);
    cbm_unlink(ingestion_direct_path);
    cbm_unlink(mirror_path);
    PASS();
}

static char *zgr_mcp_trace(cbm_mcp_server_t *server, const char *project, const char *qualified_name,
                           const char *direction, double *out_ms) {
    char args[2048];
    if (snprintf(args, sizeof(args),
                 "{\"function_name\":\"%s\",\"project\":\"%s\","
                 "\"direction\":\"%s\",\"mode\":\"calls\",\"depth\":%d,"
                 "\"risk_labels\":true,\"include_tests\":true}",
                 qualified_name, project, direction, ZGR_WALK_DEPTH) >= (int)sizeof(args)) {
        return NULL;
    }
    struct timespec start;
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    char *result = cbm_mcp_handle_tool(server, "trace_path", args);
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = zgr_elapsed_ms(&start, &end);
    return result;
}

TEST(zova_graph_mcp_real_repo_report_and_parity) {
    const char *db_path = getenv("CBM_ZOVA_REAL_DB");
    const char *project = getenv("CBM_ZOVA_REAL_PROJECT");
    const char *report_path = getenv("CBM_ZOVA_REAL_GRAPH_MCP_REPORT");
    const char *samples_path = getenv("CBM_ZOVA_REAL_GRAPH_MCP_SAMPLES");
    ASSERT_NOT_NULL(db_path);
    ASSERT_NOT_NULL(project);
    ASSERT_NOT_NULL(report_path);
    FILE *profile_samples = NULL;
    if (samples_path && samples_path[0]) {
        profile_samples = fopen(samples_path, "wb");
        ASSERT_NOT_NULL(profile_samples);
    }

    sqlite3 *sqlite_db = NULL;
    ASSERT_EQ(zgr_sql_open(db_path, &sqlite_db), 0);
    char registry_path[1024];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    ASSERT_EQ(cbm_zova_workspace_registry_path(registry_path, sizeof(registry_path)), 0);
    cbm_project_t project_row = {0};
    cbm_store_t *source_store = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(source_store);
    ASSERT_EQ(cbm_store_get_project(source_store, project, &project_row), CBM_STORE_OK);
    if (cbm_zova_workspace_lookup_at(registry_path, project_row.root_path, workspace_id,
                                     sizeof(workspace_id)) != 0) {
        /* The normal graph_read index path creates this registry row. Existing
         * sidecar artifacts from before graph_read need it only for this
         * standalone read-only benchmark setup. */
        ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, project_row.root_path,
                                                       workspace_id, sizeof(workspace_id)),
                  0);
    }

    zgr_node_t samples[ZGR_MAX_SAMPLES];
    memset(samples, 0, sizeof(samples));
    int sample_count = 0;
    ASSERT_EQ(zgr_load_samples(sqlite_db, project, workspace_id, samples, &sample_count), 0);
    ASSERT_GT(sample_count, 0);
    cbm_project_free_fields(&project_row);
    cbm_store_close(source_store);

    cbm_mcp_server_t *server = cbm_mcp_server_new(project);
    ASSERT_NOT_NULL(server);
    const char *directions[] = {"outbound", "inbound", "both"};
    double sqlite_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_total_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_walk_count_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_walk_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_hydrate_prepare_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_hydrate_step_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_result_build_warm[ZGR_MAX_TIMINGS] = {0};
    double zova_hydration_warm[ZGR_MAX_TIMINGS] = {0};
    double sqlite_cold[ZGR_MAX_SAMPLES * ZGR_DIRECTION_COUNT] = {0};
    double zova_cold[ZGR_MAX_SAMPLES * ZGR_DIRECTION_COUNT] = {0};
    int warm_count = 0;
    int cold_count = 0;
    int mismatches = 0;
    int warm_fallback_count = 0;
    int cold_fallback_count = 0;
    int fallback_sample = -1;
    int fallback_direction = -1;
    int fallback_repetition = -2;

    for (int sample = 0; sample < sample_count; sample++) {
        for (int direction = 0; direction < ZGR_DIRECTION_COUNT; direction++) {
            cbm_setenv("CBM_ZOVA_MODE", "off", 1);
            char *warmup_sqlite = zgr_mcp_trace(server, project, samples[sample].qualified_name,
                                                directions[direction], &sqlite_cold[cold_count]);
            ASSERT_NOT_NULL(warmup_sqlite);
            free(warmup_sqlite);
            cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
            char *warmup_zova = zgr_mcp_trace(server, project, samples[sample].qualified_name,
                                              directions[direction], &zova_cold[cold_count]);
            ASSERT_NOT_NULL(warmup_zova);
            cbm_zova_graph_metrics_t warmup_metrics = {0};
            cbm_store_zova_graph_last_metrics(cbm_mcp_server_store(server), &warmup_metrics);
            if (warmup_metrics.fallback) {
                cold_fallback_count++;
                fallback_sample = sample;
                fallback_direction = direction;
                fallback_repetition = -1;
            }
            free(warmup_zova);
            cold_count++;

            for (int repetition = 0; repetition < ZGR_WARM_REPETITIONS; repetition++) {
                cbm_setenv("CBM_ZOVA_MODE", "off", 1);
                char *sqlite_result = zgr_mcp_trace(server, project, samples[sample].qualified_name,
                                                     directions[direction], &sqlite_warm[warm_count]);
                ASSERT_NOT_NULL(sqlite_result);
                cbm_setenv("CBM_ZOVA_MODE", "graph_read", 1);
                char *zova_result = zgr_mcp_trace(server, project, samples[sample].qualified_name,
                                                   directions[direction], &zova_total_warm[warm_count]);
                ASSERT_NOT_NULL(zova_result);
                if (strcmp(sqlite_result, zova_result) != 0) {
                    mismatches++;
                }
                cbm_zova_graph_metrics_t metrics = {0};
                cbm_store_zova_graph_last_metrics(cbm_mcp_server_store(server), &metrics);
                if (metrics.fallback) {
                    warm_fallback_count++;
                    fallback_sample = sample;
                    fallback_direction = direction;
                    fallback_repetition = repetition;
                }
                zova_walk_warm[warm_count] = metrics.walk_ms;
                zova_walk_count_warm[warm_count] = metrics.walk_count;
                zova_hydrate_prepare_warm[warm_count] = metrics.hydrate_prepare_ms;
                zova_hydrate_step_warm[warm_count] = metrics.hydrate_step_ms;
                zova_result_build_warm[warm_count] = metrics.result_build_ms;
                zova_hydration_warm[warm_count] = metrics.hydrate_prepare_ms + metrics.hydrate_step_ms;
                zgr_write_profile_sample(profile_samples, sample, samples[sample].qualified_name,
                                         directions[direction], repetition, sqlite_warm[warm_count],
                                         zova_total_warm[warm_count], &metrics);
                free(sqlite_result);
                free(zova_result);
                warm_count++;
            }
        }
    }

    FILE *report = fopen(report_path, "wb");
    ASSERT_NOT_NULL(report);
    fprintf(report,
            "{\n"
            "  \"scope\": \"public cbm_mcp_handle_tool trace_path calls (outbound/inbound/both)\",\n"
            "  \"sample_count\": %d,\n"
            "  \"warm_repetitions\": %d,\n"
            "  \"trace_parity\": {\"mismatches\": %d, \"fallback_count\": %d, "
            "\"cold_fallback_count\": %d, "
            "\"fallback_sample\": %d, \"fallback_direction\": \"%s\", "
            "\"fallback_repetition\": %d},\n"
            "  \"cold_ms\": {\"sqlite_p50\": %.3f, \"zova_total_p50\": %.3f},\n"
            "  \"warm_ms\": {\"sqlite_p50\": %.3f, \"sqlite_p95\": %.3f, "
            "\"zova_walk_count_p50\": %.3f, \"zova_walk_count_p95\": %.3f, "
            "\"zova_walk_p50\": %.3f, \"zova_walk_p95\": %.3f, "
            "\"zova_hydrate_prepare_p50\": %.3f, \"zova_hydrate_prepare_p95\": %.3f, "
            "\"zova_hydrate_step_p50\": %.3f, \"zova_hydrate_step_p95\": %.3f, "
            "\"zova_result_build_p50\": %.3f, \"zova_result_build_p95\": %.3f, "
            "\"zova_hydration_p50\": %.3f, \"zova_hydration_p95\": %.3f, "
            "\"zova_total_p50\": %.3f, \"zova_total_p95\": %.3f}\n"
            "}\n",
            sample_count, ZGR_WARM_REPETITIONS, mismatches, warm_fallback_count,
            cold_fallback_count, fallback_sample,
            fallback_direction >= 0 ? directions[fallback_direction] : "", fallback_repetition,
            zgr_percentile(sqlite_cold, cold_count, 50), zgr_percentile(zova_cold, cold_count, 50),
            zgr_percentile(sqlite_warm, warm_count, 50), zgr_percentile(sqlite_warm, warm_count, 95),
            zgr_percentile(zova_walk_count_warm, warm_count, 50),
            zgr_percentile(zova_walk_count_warm, warm_count, 95),
            zgr_percentile(zova_walk_warm, warm_count, 50),
            zgr_percentile(zova_walk_warm, warm_count, 95),
            zgr_percentile(zova_hydrate_prepare_warm, warm_count, 50),
            zgr_percentile(zova_hydrate_prepare_warm, warm_count, 95),
            zgr_percentile(zova_hydrate_step_warm, warm_count, 50),
            zgr_percentile(zova_hydrate_step_warm, warm_count, 95),
            zgr_percentile(zova_result_build_warm, warm_count, 50),
            zgr_percentile(zova_result_build_warm, warm_count, 95),
            zgr_percentile(zova_hydration_warm, warm_count, 50),
            zgr_percentile(zova_hydration_warm, warm_count, 95),
            zgr_percentile(zova_total_warm, warm_count, 50),
            zgr_percentile(zova_total_warm, warm_count, 95));
    ASSERT_EQ(fclose(report), 0);
    if (profile_samples) {
        ASSERT_EQ(fclose(profile_samples), 0);
    }
    cbm_mcp_server_free(server);
    zgr_nodes_free(samples, sample_count);
    sqlite3_close(sqlite_db);
    ASSERT_EQ(mismatches, 0);
    /* This suite exercises the transitional two-file sidecar. Projection-free
     * sidecars retain native topology but fall back when a result needs
     * canonical metadata from the separate project database. Empty or
     * topology-only results can still finish natively. Flagged single-file
     * zero-fallback coverage lives in zova_single_file_promotion_real_repo. */
    ASSERT_GT(warm_fallback_count, 0);
    ASSERT_GT(cold_fallback_count, 0);
    ASSERT_TRUE(warm_fallback_count <= warm_count);
    ASSERT_TRUE(cold_fallback_count <= cold_count);
    PASS();
}

#else

TEST(zova_graph_real_repo_report_and_parity) {
    PASS();
}

TEST(zova_graph_mcp_real_repo_report_and_parity) {
    PASS();
}

#endif

TEST(zova_graph_real_repo_percentile_uses_nearest_rank) {
    const double values[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    ASSERT_FLOAT_EQ(zgr_percentile(values, 5, 50), 3.0, 0.000001);
    ASSERT_FLOAT_EQ(zgr_percentile(values, 5, 95), 5.0, 0.000001);
    PASS();
}

SUITE(zova_graph_real_repo) {
    RUN_TEST(zova_graph_real_repo_percentile_uses_nearest_rank);
    RUN_TEST(zova_graph_real_repo_report_and_parity);
    RUN_TEST(zova_graph_mcp_real_repo_report_and_parity);
}
