#include "zova/cbm_zova_repository.h"

#include "zova/cbm_zova.h"
#include "zova/cbm_zova_edge_payload.h"
#include "foundation/compat.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/sha256.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pipeline.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CBM_WITH_ZOVA
#include "zova.h"

struct cbm_zova_repository {
    zova_database *db;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char project[CBM_SZ_4K];
    int64_t workspace_key;
    int64_t generation;
};

struct cbm_zova_catalog {
    zova_database *db;
    cbm_zova_catalog_project_t *projects;
    int count;
    bool transaction_open;
    char error[512];
};

enum {
    REPO_GRAPH_SCAN_PAGE = 1024,
    REPO_EDGE_SCAN_PAGE = 4096,
    REPO_GRAPH_NAME_CAP = CBM_ZOVA_WORKSPACE_ID_MAX + 64,
    REPO_STRING_ARENA_CHUNK = 64 * 1024,
};

typedef struct repo_string_arena_chunk {
    struct repo_string_arena_chunk *next;
    size_t used;
    size_t capacity;
    char data[];
} repo_string_arena_chunk_t;

typedef struct {
    repo_string_arena_chunk_t *head;
    uint64_t chunk_count;
    uint64_t bytes_used;
} repo_string_arena_t;

static void repo_string_arena_free(repo_string_arena_t *arena) {
    if (!arena) return;
    repo_string_arena_chunk_t *chunk = arena->head;
    while (chunk) {
        repo_string_arena_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    free(arena);
}

static char *repo_string_arena_dup(repo_string_arena_t *arena,
                                   const uint8_t *text, size_t len) {
    if (!arena) return NULL;
    size_t needed = len + 1;
    repo_string_arena_chunk_t *chunk = arena->head;
    if (!chunk || chunk->capacity - chunk->used < needed) {
        size_t capacity = needed > REPO_STRING_ARENA_CHUNK
                              ? needed : REPO_STRING_ARENA_CHUNK;
        chunk = malloc(sizeof(*chunk) + capacity);
        if (!chunk) return NULL;
        *chunk = (repo_string_arena_chunk_t){
            .next = arena->head, .used = 0, .capacity = capacity};
        arena->head = chunk;
        arena->chunk_count++;
    }
    char *copy = chunk->data + chunk->used;
    if (len) memcpy(copy, text, len);
    copy[len] = '\0';
    chunk->used += needed;
    arena->bytes_used += needed;
    return copy;
}

static char *repo_dup(const uint8_t *text, size_t len) {
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    if (len) memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static int repo_prepare(cbm_zova_repository_t *repo, const char *sql, zova_statement **out) {
    return zova_database_prepare(&(zova_database_prepare_request){
               .db = repo->db, .sql = sql, .out_statement = out}) == ZOVA_OK && *out
               ? 0 : -1;
}

static int repo_bind_text(zova_statement *stmt, int index, const char *value) {
    return zova_statement_bind_text(&(zova_statement_bind_text_request){
               .statement = stmt, .index = index, .data = (const uint8_t *)value,
               .len = strlen(value)}) == ZOVA_OK ? 0 : -1;
}

static int repo_bind_i64(zova_statement *stmt, int index, int64_t value) {
    return zova_statement_bind_int64(&(zova_statement_bind_int64_request){
               .statement = stmt, .index = index, .value = value}) == ZOVA_OK ? 0 : -1;
}

static int repo_step(zova_statement *stmt, zova_step_result *out) {
    return zova_statement_step(&(zova_statement_step_request){
               .statement = stmt, .out_result = out}) == ZOVA_OK ? 0 : -1;
}

static char *repo_column_text(zova_statement *stmt, int index) {
    zova_text text = {0};
    if (zova_statement_column_text(&(zova_statement_column_text_request){
            .statement = stmt, .index = index, .out_text = &text}) != ZOVA_OK)
        return NULL;
    char *copy = repo_dup((const uint8_t *)(text.data ? text.data : ""),
                          text.data ? text.len : 0);
    zova_text_free(&text);
    return copy;
}

static int64_t repo_column_i64(zova_statement *stmt, int index) {
    int64_t value = 0;
    (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
        .statement = stmt, .index = index, .out_value = &value});
    return value;
}

static double repo_column_double(zova_statement *stmt, int index) {
    double value = 0.0;
    (void)zova_statement_column_double(&(zova_statement_column_double_request){
        .statement = stmt, .index = index, .out_value = &value});
    return value;
}

static int64_t stable_numeric_id(const char *value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)value; p && *p; ++p) {
        hash = (hash ^ *p) * 1099511628211ULL;
    }
    return (int64_t)(hash & INT64_MAX);
}

static int repo_edge_id(const char *workspace_id, const char *source, const char *type,
                        const char *target, const char *local_name, char *out,
                        size_t out_size) {
    const char *parts[] = {source, type, target, local_name};
    cbm_sha256_ctx hash;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    static const char digits[] = "0123456789abcdef";
    char hex[33];
    if (!workspace_id || !source || !type || !target || !local_name || !out) return -1;
    cbm_sha256_init(&hash);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        cbm_sha256_update(&hash, parts[i], strlen(parts[i]));
        cbm_sha256_update(&hash, "\0", 1);
    }
    cbm_sha256_final(&hash, digest);
    for (size_t i = 0; i < 16; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0xf];
    }
    hex[32] = '\0';
    return snprintf(out, out_size, "e:v1:%s:%s", workspace_id, hex) < (int)out_size ? 0 : -1;
}

static int repo_graph_name(const cbm_zova_repository_t *repo, char *out, size_t out_size) {
    return repo && cbm_zova_workspace_graph_name(repo->workspace_id, out, out_size) == 0
        ? 0 : -1;
}

static int repo_node_key_for_stable_id(cbm_zova_repository_t *repo, const char *stable_id,
                                       int64_t *out_key) {
    if (!repo || !stable_id || !stable_id[0] || !out_key) return -1;
    *out_key = 0;
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) return -1;
    zova_graph_scan_cursor cursor = {0};
    for (;;) {
        zova_graph_scan_results page = {0};
        zova_status status = zova_graph_scan(&(zova_graph_scan_request){
            .db = repo->db, .graph_name = graph_name, .node_after = cursor,
            .node_limit = REPO_GRAPH_SCAN_PAGE, .edge_limit = 0, .out_results = &page});
        if (status != ZOVA_OK) return -1;
        for (size_t i = 0; i < page.nodes_len; i++) {
            zova_graph_scan_node *node = &page.nodes[i];
            if (node->node_id && strcmp(node->node_id, stable_id) == 0) {
                *out_key = node->node_key;
                zova_graph_scan_results_free(&page);
                return *out_key > 0 ? 0 : -1;
            }
        }
        if (page.nodes_len) {
            zova_graph_scan_node *last = &page.nodes[page.nodes_len - 1];
            cursor = (zova_graph_scan_cursor){.created_order = last->created_order,
                                              .key = last->node_key};
        }
        uint8_t more = page.has_more_nodes;
        zova_graph_scan_results_free(&page);
        if (!more) return 1;
    }
}

static int repo_native_nodes_by_key(cbm_zova_repository_t *repo, const int64_t *keys,
                                    size_t count, zova_graph_keyed_node_results *out) {
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (!out || repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) return -1;
    memset(out, 0, sizeof(*out));
    zova_status status = zova_graph_nodes_get_many_keyed(&(zova_graph_nodes_get_many_keyed_request){
               .db = repo->db, .graph_name = graph_name, .node_keys = keys,
               .key_count = count, .out_results = out});
    if (status != ZOVA_OK || out->len != count) {
        fprintf(stderr, "zova keyed node read failed status=%d requested=%zu returned=%zu\n",
                (int)status, count, out->len);
        return -1;
    }
    return 0;
}

static int repo_degree_many(cbm_zova_repository_t *repo, const int64_t *keys, size_t count,
                            int direction, const char *edge_type, uint64_t *out) {
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (!out || repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) return -1;
    return zova_graph_degree_many_keyed(&(zova_graph_degree_many_keyed_request){
        .db=repo->db,.graph_name=graph_name,.node_keys=keys,.node_count=count,
        .direction=direction,.edge_type=edge_type,.out_degrees=out,
        .out_degrees_capacity=count}) == ZOVA_OK ? 0 : -1;
}

static double repo_elapsed_ms(struct timespec started) {
    struct timespec finished;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    return (double)(finished.tv_sec - started.tv_sec) * 1000.0 +
           (double)(finished.tv_nsec - started.tv_nsec) / 1000000.0;
}

cbm_zova_repository_t *cbm_zova_repository_open(const char *path, const char *project) {
    if (!path || !path[0] || !project || !project[0]) return NULL;
    char *stored_project = strdup(project);
    if (!stored_project) return NULL;
    cbm_zova_catalog_t *catalog = cbm_zova_catalog_open(path);
    if (catalog) {
        const char *selectors[] = {project};
        cbm_zova_catalog_scope_t scope = {0};
        if (cbm_zova_catalog_resolve(catalog, selectors, 1, false, &scope) == CBM_STORE_OK &&
            scope.count == 1) {
            char *resolved = strdup(scope.projects[0].project);
            if (!resolved) {
                cbm_zova_catalog_scope_free(&scope);
                cbm_zova_catalog_close(catalog);
                free(stored_project);
                return NULL;
            }
            free(stored_project);
            stored_project = resolved;
        }
        cbm_zova_catalog_scope_free(&scope);
        cbm_zova_catalog_close(catalog);
    }
    cbm_zova_repository_t *repo = calloc(1, sizeof(*repo));
    if (!repo) {
        free(stored_project);
        return NULL;
    }
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &repo->db, .out_error_message = &error}) != ZOVA_OK || !repo->db) {
        zova_message_free(&error);
        free(repo);
        free(stored_project);
        return NULL;
    }
    zova_message_free(&error);
    if (cbm_zova_register_sql_functions(repo->db) != 0) {
        cbm_zova_repository_close(repo);
        free(stored_project);
        return NULL;
    }
    zova_statement *stmt = NULL;
    const char *sql =
        "SELECT r.workspace_id,p.workspace_key,g.generation FROM cbm_projects_v1 p "
        "JOIN cbm_workspace_registry r ON r.workspace_key=p.workspace_key "
        "JOIN cbm_database_generation_v1 g ON g.workspace_key=p.workspace_key "
        "WHERE p.project=?1 AND g.state='ready' AND g.generation=(SELECT MAX(g2.generation) "
        "FROM cbm_database_generation_v1 g2 WHERE g2.workspace_key=p.workspace_key "
        "AND g2.state='ready') AND NOT EXISTS (SELECT 1 FROM cbm_workspace_health_v1 h "
        "WHERE h.workspace_key=p.workspace_key AND h.state='rebuild_required')";
    if (repo_prepare(repo, sql, &stmt) != 0 || repo_bind_text(stmt, 1, stored_project) != 0) {
        if (stmt) (void)zova_statement_finalize(stmt);
        cbm_zova_repository_close(repo);
        free(stored_project);
        return NULL;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int ok = repo_step(stmt, &step) == 0 && step == ZOVA_STEP_ROW;
    char *workspace = ok ? repo_column_text(stmt, 0) : NULL;
    if (ok) {
        repo->workspace_key = repo_column_i64(stmt, 1);
        repo->generation = repo_column_i64(stmt, 2);
    }
    zova_step_result second = ZOVA_STEP_DONE;
    if (ok && repo_step(stmt, &second) != 0) ok = 0;
    if (second == ZOVA_STEP_ROW) ok = 0;
    (void)zova_statement_finalize(stmt);
    if (!ok || !workspace || !workspace[0] || strlen(workspace) >= sizeof(repo->workspace_id) ||
        strlen(project) >= sizeof(repo->project)) {
        free(workspace);
        cbm_zova_repository_close(repo);
        free(stored_project);
        return NULL;
    }
    strcpy(repo->workspace_id, workspace);
    strcpy(repo->project, project);
    free(workspace);
    free(stored_project);
    return repo;
}

void cbm_zova_repository_close(cbm_zova_repository_t *repo) {
    if (!repo) return;
    if (repo->db) (void)zova_database_close(repo->db);
    free(repo);
}

static void catalog_project_free(cbm_zova_catalog_project_t *project) {
    if (!project) return;
    free(project->selector);
    free(project->project);
    free(project->root_path);
    free(project->indexed_at);
    free(project->model_fingerprint);
    free(project->health_reason);
    memset(project, 0, sizeof(*project));
}

static int catalog_project_copy(cbm_zova_catalog_project_t *destination,
                                const cbm_zova_catalog_project_t *source) {
    if (!destination || !source) return -1;
    *destination = (cbm_zova_catalog_project_t){
        .workspace_key = source->workspace_key,
        .vector_dimensions = source->vector_dimensions,
        .generation = source->generation,
        .ready = source->ready,
        .healthy = source->healthy,
    };
    memcpy(destination->workspace_id, source->workspace_id,
           sizeof(destination->workspace_id));
#define CATALOG_COPY(field) \
    do { \
        destination->field = strdup(source->field ? source->field : ""); \
        if (!destination->field) goto error; \
    } while (0)
    CATALOG_COPY(selector);
    CATALOG_COPY(project);
    CATALOG_COPY(root_path);
    CATALOG_COPY(indexed_at);
    CATALOG_COPY(model_fingerprint);
    CATALOG_COPY(health_reason);
#undef CATALOG_COPY
    return 0;
error:
    catalog_project_free(destination);
    return -1;
}

static bool catalog_selector_used(const cbm_zova_catalog_t *catalog, int before,
                                  const char *selector) {
    for (int i = 0; i < before; i++) {
        if (catalog->projects[i].selector &&
            strcmp(catalog->projects[i].selector, selector) == 0) {
            return true;
        }
    }
    return false;
}

static char *catalog_selector_disambiguate(const char *selector, const char *root_path,
                                           int64_t workspace_key) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)root_path; p && *p; p++)
        hash = (hash ^ *p) * 1099511628211ULL;
    size_t needed = strlen(selector) + 1 + 16 + 1 + 20 + 1;
    char *result = malloc(needed);
    if (!result) return NULL;
    snprintf(result, needed, "%s-%016llx-%lld", selector, (unsigned long long)hash,
             (long long)workspace_key);
    return result;
}

static int catalog_assign_selectors(cbm_zova_catalog_t *catalog) {
    for (int i = 0; i < catalog->count; i++) {
        size_t parent_levels = 0;
        char *selector = cbm_project_selector_from_path(catalog->projects[i].root_path,
                                                        parent_levels);
        if (!selector) return -1;
        while (catalog_selector_used(catalog, i, selector)) {
            char *next = cbm_project_selector_from_path(catalog->projects[i].root_path,
                                                        ++parent_levels);
            if (!next) {
                free(selector);
                return -1;
            }
            if (strcmp(next, selector) == 0) {
                free(next);
                next = catalog_selector_disambiguate(
                    selector, catalog->projects[i].root_path,
                    catalog->projects[i].workspace_key);
                if (!next) {
                    free(selector);
                    return -1;
                }
            }
            free(selector);
            selector = next;
        }
        catalog->projects[i].selector = selector;
    }
    return 0;
}

static int catalog_project_selector_compare(const void *left, const void *right) {
    const cbm_zova_catalog_project_t *a = left;
    const cbm_zova_catalog_project_t *b = right;
    return strcmp(a->selector, b->selector);
}

cbm_zova_catalog_t *cbm_zova_catalog_open(const char *path) {
    if (!path || !path[0]) return NULL;
    cbm_zova_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) return NULL;
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &catalog->db, .out_error_message = &error}) != ZOVA_OK || !catalog->db) {
        zova_message_free(&error);
        cbm_zova_catalog_close(catalog);
        return NULL;
    }
    zova_message_free(&error);
    if (cbm_zova_register_sql_functions(catalog->db) != 0 ||
        zova_database_exec(&(zova_database_exec_request){
            .db = catalog->db, .sql = "BEGIN"}) != ZOVA_OK) {
        cbm_zova_catalog_close(catalog);
        return NULL;
    }
    catalog->transaction_open = true;

    cbm_zova_repository_t reader = {.db = catalog->db};
    zova_statement *statement = NULL;
    const char *sql =
        "SELECT r.workspace_id,r.workspace_key,r.canonical_root,COALESCE(p.project,''),"
        "COALESCE(p.indexed_at,''),COALESCE(s.model_fingerprint,''),"
        "COALESCE(s.vector_dimensions,0),r.active_generation,"
        "CASE WHEN g.state='ready' THEN 1 ELSE 0 END,"
        "COALESCE(h.state,'healthy'),COALESCE(h.reason,'') "
        "FROM cbm_workspace_registry r "
        "LEFT JOIN cbm_projects_v1 p ON p.workspace_key=r.workspace_key "
        "LEFT JOIN cbm_workspace_index_state_v1 s ON s.workspace_key=r.workspace_key "
        "LEFT JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key "
        "AND g.generation=r.active_generation "
        "LEFT JOIN cbm_workspace_health_v1 h ON h.workspace_key=r.workspace_key "
        "ORDER BY r.workspace_key";
    if (repo_prepare(&reader, sql, &statement) != 0) {
        cbm_zova_catalog_close(catalog);
        return NULL;
    }
    int capacity = 8;
    catalog->projects = calloc((size_t)capacity, sizeof(*catalog->projects));
    if (!catalog->projects) {
        (void)zova_statement_finalize(statement);
        cbm_zova_catalog_close(catalog);
        return NULL;
    }
    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0) goto error;
        if (step == ZOVA_STEP_DONE) break;
        if (catalog->count == capacity) {
            if (capacity > INT_MAX / 2) goto error;
            int old_capacity = capacity;
            capacity *= 2;
            cbm_zova_catalog_project_t *grown =
                realloc(catalog->projects, (size_t)capacity * sizeof(*grown));
            if (!grown) goto error;
            catalog->projects = grown;
            memset(catalog->projects + old_capacity, 0,
                   (size_t)(capacity - old_capacity) * sizeof(*catalog->projects));
        }
        cbm_zova_catalog_project_t *project = &catalog->projects[catalog->count];
        char *workspace_id = repo_column_text(statement, 0);
        project->workspace_key = repo_column_i64(statement, 1);
        project->root_path = repo_column_text(statement, 2);
        project->project = repo_column_text(statement, 3);
        project->indexed_at = repo_column_text(statement, 4);
        project->model_fingerprint = repo_column_text(statement, 5);
        project->vector_dimensions = (int)repo_column_i64(statement, 6);
        project->generation = repo_column_i64(statement, 7);
        project->ready = repo_column_i64(statement, 8) != 0;
        char *health = repo_column_text(statement, 9);
        project->health_reason = repo_column_text(statement, 10);
        project->healthy = health && strcmp(health, "rebuild_required") != 0;
        if (!workspace_id || !workspace_id[0] ||
            strlen(workspace_id) >= sizeof(project->workspace_id) ||
            !project->root_path || !project->project || !project->indexed_at ||
            !project->model_fingerprint || !health || !project->health_reason) {
            free(workspace_id);
            free(health);
            catalog_project_free(project);
            goto error;
        }
        strcpy(project->workspace_id, workspace_id);
        free(workspace_id);
        free(health);
        catalog->count++;
    }
    (void)zova_statement_finalize(statement);
    if (catalog_assign_selectors(catalog) != 0) {
        cbm_zova_catalog_close(catalog);
        return NULL;
    }
    qsort(catalog->projects, (size_t)catalog->count, sizeof(*catalog->projects),
          catalog_project_selector_compare);
    return catalog;
error:
    (void)zova_statement_finalize(statement);
    cbm_zova_catalog_close(catalog);
    return NULL;
}

void cbm_zova_catalog_close(cbm_zova_catalog_t *catalog) {
    if (!catalog) return;
    if (catalog->db && catalog->transaction_open) {
        (void)zova_database_exec(&(zova_database_exec_request){
            .db = catalog->db, .sql = "ROLLBACK"});
    }
    for (int i = 0; i < catalog->count; i++) catalog_project_free(&catalog->projects[i]);
    free(catalog->projects);
    if (catalog->db) (void)zova_database_close(catalog->db);
    free(catalog);
}

void cbm_zova_catalog_scope_free(cbm_zova_catalog_scope_t *scope) {
    if (!scope) return;
    for (int i = 0; i < scope->count; i++) catalog_project_free(&scope->projects[i]);
    for (int i = 0; i < scope->excluded_count; i++)
        catalog_project_free(&scope->excluded_projects[i]);
    free(scope->projects);
    free(scope->excluded_projects);
    memset(scope, 0, sizeof(*scope));
}

int cbm_zova_catalog_list(cbm_zova_catalog_t *catalog, cbm_zova_catalog_scope_t *out) {
    if (!catalog || !out) return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    if (catalog->count == 0) return CBM_STORE_OK;
    out->projects = calloc((size_t)catalog->count, sizeof(*out->projects));
    if (!out->projects) return CBM_STORE_ERR;
    for (int i = 0; i < catalog->count; i++) {
        if (catalog_project_copy(&out->projects[out->count], &catalog->projects[i]) != 0) {
            cbm_zova_catalog_scope_free(out);
            return CBM_STORE_ERR;
        }
        out->count++;
    }
    return CBM_STORE_OK;
}

const char *cbm_zova_catalog_error(const cbm_zova_catalog_t *catalog) {
    return catalog && catalog->error[0] ? catalog->error : "";
}

int cbm_zova_catalog_resolve(cbm_zova_catalog_t *catalog, const char *const *selectors,
                             int selector_count, bool wildcard,
                             cbm_zova_catalog_scope_t *out) {
    if (!catalog || !out || (!wildcard && (!selectors || selector_count <= 0)))
        return CBM_STORE_ERR;
    catalog->error[0] = '\0';
    memset(out, 0, sizeof(*out));
    int wanted = wildcard ? catalog->count : selector_count;
    out->projects = wanted ? calloc((size_t)wanted, sizeof(*out->projects)) : NULL;
    if (wanted && !out->projects) return CBM_STORE_ERR;

    if (wildcard) {
        out->excluded_projects = wanted ? calloc((size_t)wanted, sizeof(*out->excluded_projects))
                                        : NULL;
        if (wanted && !out->excluded_projects) goto error;
        for (int i = 0; i < catalog->count; i++) {
            if (!catalog->projects[i].ready || !catalog->projects[i].healthy) {
                if (catalog_project_copy(&out->excluded_projects[out->excluded_count],
                                         &catalog->projects[i]) != 0)
                    goto error;
                out->excluded_count++;
            } else {
                if (catalog_project_copy(&out->projects[out->count], &catalog->projects[i]) != 0)
                    goto error;
                out->count++;
            }
        }
    } else {
        for (int i = 0; i < selector_count; i++) {
            const cbm_zova_catalog_project_t *match = NULL;
            for (int j = 0; j < catalog->count; j++) {
                if (strcmp(catalog->projects[j].selector, selectors[i]) == 0) {
                    match = &catalog->projects[j];
                    break;
                }
            }
            if (!match) {
                snprintf(catalog->error, sizeof(catalog->error),
                         "project '%s' was not found", selectors[i]);
                cbm_zova_catalog_scope_free(out);
                return CBM_STORE_NOT_FOUND;
            }
            if (!match->ready || !match->healthy) {
                snprintf(catalog->error, sizeof(catalog->error), "project '%s' is unavailable: %s",
                         selectors[i], !match->ready ? "no ready generation"
                                                    : (match->health_reason[0]
                                                           ? match->health_reason
                                                           : "workspace unhealthy"));
                goto error;
            }
            if (catalog_project_copy(&out->projects[out->count], match) != 0) goto error;
            out->count++;
        }
    }
    return CBM_STORE_OK;
error:
    cbm_zova_catalog_scope_free(out);
    return CBM_STORE_ERR;
}

static char *catalog_scope_cte(const cbm_zova_catalog_scope_t *scope, int *next_bind) {
    if (!scope || scope->count <= 0 || !next_bind) return NULL;
    size_t capacity = 64 + (size_t)scope->count * 32;
    char *sql = malloc(capacity);
    if (!sql) return NULL;
    size_t used = (size_t)snprintf(sql, capacity,
                                  "WITH scope(workspace_key,selector) AS (VALUES ");
    int bind = 3;
    for (int i = 0; i < scope->count; i++) {
        int written = snprintf(sql + used, capacity - used, "%s(?%d,?%d)",
                               i ? "," : "", bind, bind + 1);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(sql);
            return NULL;
        }
        used += (size_t)written;
        bind += 2;
    }
    if (used + 3 > capacity) {
        free(sql);
        return NULL;
    }
    memcpy(sql + used, ") ", 3);
    *next_bind = bind;
    return sql;
}

static int catalog_bind_fts_scope(zova_statement *statement,
                                  const cbm_zova_catalog_scope_t *scope,
                                  const char *query, const char *file_pattern) {
    if (repo_bind_text(statement, 1, query) != 0) return -1;
    zova_status status = file_pattern && file_pattern[0]
                             ? zova_statement_bind_text(&(zova_statement_bind_text_request){
                                   .statement = statement, .index = 2,
                                   .data = (const uint8_t *)file_pattern,
                                   .len = strlen(file_pattern)})
                             : zova_statement_bind_null(&(zova_statement_bind_null_request){
                                   .statement = statement, .index = 2});
    if (status != ZOVA_OK) return -1;
    int bind = 3;
    for (int i = 0; i < scope->count; i++) {
        if (repo_bind_i64(statement, bind++, scope->projects[i].workspace_key) != 0 ||
            repo_bind_text(statement, bind++, scope->projects[i].selector) != 0) {
            return -1;
        }
    }
    return 0;
}

int cbm_zova_catalog_search_fts(cbm_zova_catalog_t *catalog,
                                const cbm_zova_catalog_scope_t *scope,
                                const char *query, const char *file_pattern,
                                int limit, int offset, cbm_search_output_t *out) {
    if (!catalog || !scope || scope->count <= 0 || !query || !query[0] || !out || offset < 0)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    if (limit <= 0) limit = 10;

    int next_bind = 0;
    char *cte = catalog_scope_cte(scope, &next_bind);
    if (!cte) return CBM_STORE_ERR;
    const char *count_tail =
        "SELECT count(*) FROM scope sc JOIN cbm_nodes_v1 n "
        "ON n.workspace_key=sc.workspace_key "
        "JOIN cbm_nodes_fts_v1 f ON f.rowid=n.zova_node_key "
        "JOIN cbm_files_v1 p ON p.file_key=n.file_key "
        "WHERE cbm_nodes_fts_v1 MATCH ?1 "
        "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') "
        "AND (?2 IS NULL OR p.file_path GLOB ?2)";
    size_t count_len = strlen(cte) + strlen(count_tail) + 1;
    char *count_sql = malloc(count_len);
    if (!count_sql) {
        free(cte);
        return CBM_STORE_ERR;
    }
    snprintf(count_sql, count_len, "%s%s", cte, count_tail);
    cbm_zova_repository_t reader = {.db = catalog->db};
    zova_statement *statement = NULL;
    int rc = repo_prepare(&reader, count_sql, &statement);
    if (rc == 0) rc = catalog_bind_fts_scope(statement, scope, query, file_pattern);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW)) rc = -1;
    int64_t total = rc == 0 ? repo_column_i64(statement, 0) : 0;
    if (statement) (void)zova_statement_finalize(statement);
    statement = NULL;
    free(count_sql);
    if (rc != 0 || total < 0 || total > INT_MAX) {
        free(cte);
        return CBM_STORE_ERR;
    }

    const char *data_tail =
        "SELECT n.workspace_key,n.zova_node_key,sc.selector,n.label,n.name,"
        "n.qualified_name,p.file_path,n.start_line,n.end_line,n.properties,"
        "(bm25(cbm_nodes_fts_v1)-CASE WHEN n.label IN ('Function','Method') THEN 10.0 "
        "WHEN n.label='Route' THEN 8.0 WHEN n.label IN "
        "('Class','Interface','Type','Enum') THEN 5.0 ELSE 0.0 END) AS rank "
        "FROM scope sc JOIN cbm_nodes_v1 n ON n.workspace_key=sc.workspace_key "
        "JOIN cbm_nodes_fts_v1 f ON f.rowid=n.zova_node_key "
        "JOIN cbm_files_v1 p ON p.file_key=n.file_key "
        "WHERE cbm_nodes_fts_v1 MATCH ?1 "
        "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') "
        "AND (?2 IS NULL OR p.file_path GLOB ?2) "
        "ORDER BY rank,sc.selector,n.qualified_name,n.zova_node_key ";
    size_t data_capacity = strlen(cte) + strlen(data_tail) + 80;
    char *data_sql = malloc(data_capacity);
    if (!data_sql) {
        free(cte);
        return CBM_STORE_ERR;
    }
    snprintf(data_sql, data_capacity, "%s%sLIMIT ?%d OFFSET ?%d", cte, data_tail,
             next_bind, next_bind + 1);
    free(cte);
    if (repo_prepare(&reader, data_sql, &statement) != 0 ||
        catalog_bind_fts_scope(statement, scope, query, file_pattern) != 0 ||
        repo_bind_i64(statement, next_bind, limit) != 0 ||
        repo_bind_i64(statement, next_bind + 1, offset) != 0) {
        free(data_sql);
        if (statement) (void)zova_statement_finalize(statement);
        return CBM_STORE_ERR;
    }
    free(data_sql);

    int capacity = limit < 64 ? limit : 64;
    if (capacity < 1) capacity = 1;
    cbm_search_result_t *results = calloc((size_t)capacity, sizeof(*results));
    int64_t *keys = calloc((size_t)capacity, sizeof(*keys));
    int64_t *workspace_keys = calloc((size_t)capacity, sizeof(*workspace_keys));
    int count = 0;
    if (!results || !keys || !workspace_keys) goto search_error;
    for (;;) {
        step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0) goto search_error;
        if (step == ZOVA_STEP_DONE) break;
        if (count == capacity) {
            int old_capacity = capacity;
            if (capacity > INT_MAX / 2) goto search_error;
            capacity *= 2;
            cbm_search_result_t *grown_results =
                realloc(results, (size_t)capacity * sizeof(*grown_results));
            int64_t *grown_keys = realloc(keys, (size_t)capacity * sizeof(*grown_keys));
            int64_t *grown_workspace_keys =
                realloc(workspace_keys, (size_t)capacity * sizeof(*grown_workspace_keys));
            if (!grown_results || !grown_keys || !grown_workspace_keys) {
                if (grown_results) results = grown_results;
                if (grown_keys) keys = grown_keys;
                if (grown_workspace_keys) workspace_keys = grown_workspace_keys;
                goto search_error;
            }
            results = grown_results;
            keys = grown_keys;
            workspace_keys = grown_workspace_keys;
            memset(results + old_capacity, 0,
                   (size_t)(capacity - old_capacity) * sizeof(*results));
        }
        workspace_keys[count] = repo_column_i64(statement, 0);
        keys[count] = repo_column_i64(statement, 1);
        cbm_node_t *node = &results[count].node;
        node->project = repo_column_text(statement, 2);
        node->label = repo_column_text(statement, 3);
        node->name = repo_column_text(statement, 4);
        node->qualified_name = repo_column_text(statement, 5);
        node->file_path = repo_column_text(statement, 6);
        node->start_line = (int)repo_column_i64(statement, 7);
        node->end_line = (int)repo_column_i64(statement, 8);
        node->properties_json = repo_column_text(statement, 9);
        results[count].rank = repo_column_double(statement, 10);
        if (!node->project || !node->label || !node->name || !node->qualified_name ||
            !node->file_path || !node->properties_json) goto search_error;
        count++;
    }
    (void)zova_statement_finalize(statement);
    statement = NULL;

    for (int p = 0; p < scope->count; p++) {
        int project_count = 0;
        for (int i = 0; i < count; i++) {
            if (workspace_keys[i] == scope->projects[p].workspace_key) project_count++;
        }
        if (!project_count) continue;
        int64_t *project_keys = malloc((size_t)project_count * sizeof(*project_keys));
        int *positions = malloc((size_t)project_count * sizeof(*positions));
        if (!project_keys || !positions) {
            free(project_keys);
            free(positions);
            goto search_error;
        }
        int written = 0;
        for (int i = 0; i < count; i++) {
            if (workspace_keys[i] != scope->projects[p].workspace_key) continue;
            project_keys[written] = keys[i];
            positions[written++] = i;
        }
        cbm_zova_repository_t workspace = {.db = catalog->db};
        workspace.workspace_key = scope->projects[p].workspace_key;
        strcpy(workspace.workspace_id, scope->projects[p].workspace_id);
        zova_graph_keyed_node_results native = {0};
        if (repo_native_nodes_by_key(&workspace, project_keys, (size_t)project_count,
                                     &native) != 0) {
            free(project_keys);
            free(positions);
            goto search_error;
        }
        for (int i = 0; i < project_count; i++) {
            int position = positions[i];
            if (!native.items[i].found || !native.items[i].node_id || !native.items[i].kind) {
                zova_graph_keyed_node_results_free(&native);
                free(project_keys);
                free(positions);
                goto search_error;
            }
            results[position].node.id = stable_numeric_id(native.items[i].node_id);
            free((char *)results[position].node.label);
            results[position].node.label =
                repo_dup((const uint8_t *)native.items[i].kind, native.items[i].kind_len);
            if (!results[position].node.label) {
                zova_graph_keyed_node_results_free(&native);
                free(project_keys);
                free(positions);
                goto search_error;
            }
        }
        zova_graph_keyed_node_results_free(&native);
        free(project_keys);
        free(positions);
    }
    free(keys);
    free(workspace_keys);
    out->results = results;
    out->count = count;
    out->total = (int)total;
    return CBM_STORE_OK;

search_error:
    if (statement) (void)zova_statement_finalize(statement);
    free(keys);
    free(workspace_keys);
    cbm_store_search_free(&(cbm_search_output_t){.results = results, .count = count});
    return CBM_STORE_ERR;
}

static int catalog_search_result_compare(const void *left_ptr, const void *right_ptr) {
    const cbm_search_result_t *left = left_ptr;
    const cbm_search_result_t *right = right_ptr;
    int compared = strcmp(left->node.name ? left->node.name : "",
                          right->node.name ? right->node.name : "");
    if (compared != 0) return compared;
    compared = strcmp(left->node.qualified_name ? left->node.qualified_name : "",
                      right->node.qualified_name ? right->node.qualified_name : "");
    if (compared != 0) return compared;
    compared = strcmp(left->node.project ? left->node.project : "",
                      right->node.project ? right->node.project : "");
    if (compared != 0) return compared;
    return (left->node.id > right->node.id) - (left->node.id < right->node.id);
}

int cbm_zova_catalog_search(cbm_zova_catalog_t *catalog,
                            const cbm_zova_catalog_scope_t *scope,
                            const cbm_search_params_t *params, cbm_search_output_t *out) {
    if (!catalog || !scope || scope->count <= 0 || !params || !out || params->offset < 0)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    cbm_search_result_t *combined = NULL;
    int combined_count = 0;
    int combined_capacity = 0;

    for (int p = 0; p < scope->count; p++) {
        cbm_zova_repository_t repository = {.db = catalog->db};
        repository.workspace_key = scope->projects[p].workspace_key;
        repository.generation = scope->projects[p].generation;
        strcpy(repository.workspace_id, scope->projects[p].workspace_id);
        if (strlen(scope->projects[p].project) >= sizeof(repository.project)) goto error;
        strcpy(repository.project, scope->projects[p].project);

        cbm_search_params_t local_params = *params;
        local_params.project = scope->projects[p].project;
        local_params.limit = INT_MAX;
        local_params.offset = 0;
        local_params.include_connected = false;
        cbm_search_output_t local = {0};
        if (cbm_zova_repository_search(&repository, repository.workspace_id,
                                       &local_params, &local) != CBM_STORE_OK) {
            cbm_store_search_free(&local);
            goto error;
        }
        if (local.count > INT_MAX - combined_count) {
            cbm_store_search_free(&local);
            goto error;
        }
        int needed = combined_count + local.count;
        if (needed > combined_capacity) {
            int grown_capacity = combined_capacity ? combined_capacity : 64;
            while (grown_capacity < needed) {
                if (grown_capacity > INT_MAX / 2) {
                    grown_capacity = needed;
                    break;
                }
                grown_capacity *= 2;
            }
            cbm_search_result_t *grown =
                realloc(combined, (size_t)grown_capacity * sizeof(*grown));
            if (!grown) {
                cbm_store_search_free(&local);
                goto error;
            }
            combined = grown;
            memset(combined + combined_capacity, 0,
                   (size_t)(grown_capacity - combined_capacity) * sizeof(*combined));
            combined_capacity = grown_capacity;
        }
        for (int i = 0; i < local.count; i++) {
            char *selector = strdup(scope->projects[p].selector);
            if (!selector) {
                cbm_store_search_free(&local);
                goto error;
            }
            free((char *)local.results[i].node.project);
            local.results[i].node.project = selector;
            combined[combined_count++] = local.results[i];
            memset(&local.results[i], 0, sizeof(local.results[i]));
        }
        cbm_store_search_free(&local);
    }

    qsort(combined, (size_t)combined_count, sizeof(*combined), catalog_search_result_compare);
    int limit = params->limit > 0 ? params->limit : 10;
    int available = params->offset < combined_count ? combined_count - params->offset : 0;
    int page_count = available < limit ? available : limit;
    cbm_search_result_t *page =
        page_count ? calloc((size_t)page_count, sizeof(*page)) : NULL;
    if (page_count && !page) goto error;
    for (int i = 0; i < page_count; i++) {
        int source = params->offset + i;
        page[i] = combined[source];
        memset(&combined[source], 0, sizeof(combined[source]));
    }
    cbm_store_search_free(
        &(cbm_search_output_t){.results = combined, .count = combined_count});
    out->results = page;
    out->count = page_count;
    out->total = combined_count;
    return CBM_STORE_OK;
error:
    cbm_store_search_free(
        &(cbm_search_output_t){.results = combined, .count = combined_count});
    return CBM_STORE_ERR;
}

int cbm_zova_catalog_search_semantic(
    cbm_zova_catalog_t *catalog, const char *path, const cbm_zova_catalog_scope_t *scope,
    const char **keywords, int keyword_count, int limit, int offset,
    cbm_vector_result_t **out, int *out_count, int *out_total) {
    if (!catalog || !path || !scope || scope->count <= 0 || !keywords || keyword_count <= 0 ||
        !out || !out_count || !out_total) {
        return CBM_STORE_ERR;
    }
    *out_total = 0;
    const char *model = scope->projects[0].model_fingerprint;
    int dimensions = scope->projects[0].vector_dimensions;
    if (!model || !model[0] || dimensions <= 0) return CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS;
    for (int i = 0; i < scope->count; i++) {
        const cbm_zova_catalog_project_t *project = &scope->projects[i];
        if (!project->model_fingerprint || strcmp(project->model_fingerprint, model) != 0 ||
            project->vector_dimensions != dimensions) {
            return CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS;
        }
    }

    zova_statement *count_statement = NULL;
    int rc = CBM_STORE_OK;
    if (repo_prepare(&(cbm_zova_repository_t){.db = catalog->db},
                     "SELECT node_vector_rows FROM cbm_generation_integrity_v2 "
                     "WHERE workspace_key=?1 AND generation=?2", &count_statement) != 0) {
        rc = CBM_STORE_ERR;
    }
    int *vector_counts = calloc((size_t)scope->count, sizeof(*vector_counts));
    if (!vector_counts) rc = CBM_STORE_ERR;
    int64_t total = 0;
    for (int i = 0; rc == CBM_STORE_OK && i < scope->count; i++) {
        if (zova_statement_reset(count_statement) != ZOVA_OK ||
            zova_statement_clear_bindings(count_statement) != ZOVA_OK ||
            repo_bind_i64(count_statement, 1, scope->projects[i].workspace_key) != 0 ||
            repo_bind_i64(count_statement, 2, scope->projects[i].generation) != 0) {
            rc = CBM_STORE_ERR;
            break;
        }
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(count_statement, &step) != 0 || step != ZOVA_STEP_ROW) {
            rc = CBM_STORE_ERR;
            break;
        }
        int64_t count = repo_column_i64(count_statement, 0);
        if (count < 0 || total > INT_MAX - count) {
            rc = CBM_STORE_ERR;
            break;
        }
        vector_counts[i] = (int)count;
        total += count;
    }
    if (count_statement) (void)zova_statement_finalize(count_statement);
    if (rc != CBM_STORE_OK) {
        free(vector_counts);
        return rc;
    }
    *out_total = (int)total;

    cbm_zova_vector_workspace_t *workspaces =
        calloc((size_t)scope->count, sizeof(*workspaces));
    if (!workspaces) {
        free(vector_counts);
        return CBM_STORE_ERR;
    }
    int workspace_count = 0;
    for (int i = 0; i < scope->count; i++) {
        if (vector_counts[i] == 0) continue;
        const cbm_zova_catalog_project_t *project = &scope->projects[i];
        workspaces[workspace_count++] = (cbm_zova_vector_workspace_t){
            .workspace_key = project->workspace_key,
            .workspace_id = project->workspace_id,
            .project = project->project,
            .selector = project->selector,
            .model_fingerprint = project->model_fingerprint,
            .vector_dimensions = project->vector_dimensions,
            .generation = project->generation,
        };
    }
    free(vector_counts);
    if (workspace_count > 0) {
        rc = cbm_store_zova_multi_vector_search(
            path, workspaces, workspace_count, keywords, keyword_count,
            limit, offset, out, out_count);
    } else {
        *out = NULL;
        *out_count = 0;
    }
    free(workspaces);

    cbm_zova_catalog_t *current = rc == CBM_STORE_OK ? cbm_zova_catalog_open(path) : NULL;
    const char **selectors = calloc((size_t)scope->count, sizeof(*selectors));
    cbm_zova_catalog_scope_t verified = {0};
    if (!current || !selectors) rc = CBM_STORE_ERR;
    for (int i = 0; rc == CBM_STORE_OK && i < scope->count; i++)
        selectors[i] = scope->projects[i].selector;
    if (rc == CBM_STORE_OK &&
        cbm_zova_catalog_resolve(current, selectors, scope->count, false, &verified) !=
            CBM_STORE_OK) {
        rc = CBM_ZOVA_CATALOG_STALE_SCOPE;
    }
    for (int i = 0; rc == CBM_STORE_OK && i < scope->count; i++) {
        if (verified.projects[i].workspace_key != scope->projects[i].workspace_key ||
            verified.projects[i].generation != scope->projects[i].generation ||
            verified.projects[i].vector_dimensions != scope->projects[i].vector_dimensions ||
            strcmp(verified.projects[i].model_fingerprint,
                   scope->projects[i].model_fingerprint) != 0) {
            rc = CBM_ZOVA_CATALOG_STALE_SCOPE;
        }
    }
    free(selectors);
    cbm_zova_catalog_scope_free(&verified);
    cbm_zova_catalog_close(current);
    if (rc != CBM_STORE_OK) {
        cbm_store_free_vector_results(*out, *out_count);
        *out = NULL;
        *out_count = 0;
    }
    return rc;
}

const char *cbm_zova_repository_workspace_id(const cbm_zova_repository_t *repo) {
    return repo ? repo->workspace_id : "";
}

static int repo_workspace_required(const cbm_zova_repository_t *repo,
                                   const char *workspace_id) {
    return repo && cbm_zova_workspace_id_validate(workspace_id) == 0 &&
           strcmp(repo->workspace_id, workspace_id) == 0
               ? CBM_STORE_OK
               : CBM_STORE_ERR;
}

static int hydrate_node_key(cbm_zova_repository_t *repo, const char *workspace_id,
                            int64_t node_key, cbm_node_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || node_key <= 0 || !out)
        return CBM_STORE_ERR;
    zova_statement *stmt = NULL;
    const char *sql = "SELECT n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,"
        "n.properties FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key "
        "WHERE n.workspace_key=?1 AND n.zova_node_key=?2";
    if (repo_prepare(repo, sql, &stmt) != 0 ||
        repo_bind_i64(stmt, 1, repo->workspace_key) != 0 ||
        repo_bind_i64(stmt, 2, node_key) != 0) goto error;
    zova_step_result step = ZOVA_STEP_DONE;
    if (repo_step(stmt, &step) != 0 || step != ZOVA_STEP_ROW) {
        (void)zova_statement_finalize(stmt);
        return CBM_STORE_NOT_FOUND;
    }
    zova_graph_keyed_node_results native = {0};
    if (repo_native_nodes_by_key(repo, &node_key, 1, &native) != 0 ||
        !native.items[0].found || !native.items[0].node_id || !native.items[0].kind) {
        zova_graph_keyed_node_results_free(&native);
        goto error;
    }
    memset(out, 0, sizeof(*out));
    out->id = stable_numeric_id(native.items[0].node_id);
    out->project = repo_dup((const uint8_t *)repo->project, strlen(repo->project));
    out->label = repo_dup((const uint8_t *)native.items[0].kind,
                          native.items[0].kind_len);
    out->name = repo_column_text(stmt, 0);
    out->qualified_name = repo_column_text(stmt, 1);
    out->file_path = repo_column_text(stmt, 2);
    out->start_line = (int)repo_column_i64(stmt, 3);
    out->end_line = (int)repo_column_i64(stmt, 4);
    out->properties_json = repo_column_text(stmt, 5);
    zova_graph_keyed_node_results_free(&native);
    (void)zova_statement_finalize(stmt);
    return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_find_node_by_qn(cbm_zova_repository_t *repo, const char *workspace_id,
                                         const char *qualified_name, cbm_node_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !qualified_name || !out)
        return CBM_STORE_ERR;
    zova_statement *stmt = NULL;
    int64_t key = 0;
    int rc = repo_prepare(repo, "SELECT zova_node_key FROM cbm_nodes_v1 "
                                "WHERE workspace_key=?1 AND qualified_name=?2", &stmt);
    if (rc == 0) rc = repo_bind_i64(stmt, 1, repo->workspace_key);
    if (rc == 0) rc = repo_bind_text(stmt, 2, qualified_name);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && repo_step(stmt, &step) == 0 && step == ZOVA_STEP_ROW)
        key = repo_column_i64(stmt, 0);
    if (stmt) (void)zova_statement_finalize(stmt);
    return key > 0 ? hydrate_node_key(repo, workspace_id, key, out) : CBM_STORE_NOT_FOUND;
}

int cbm_zova_repository_find_node_by_stable_id(cbm_zova_repository_t *repo,
                                                const char *workspace_id,
                                                const char *stable_id, cbm_node_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !stable_id || !out)
        return CBM_STORE_ERR;
    int64_t key = 0;
    int rc = repo_node_key_for_stable_id(repo, stable_id, &key);
    return rc == 0 ? hydrate_node_key(repo, workspace_id, key, out)
                   : rc == 1 ? CBM_STORE_NOT_FOUND : CBM_STORE_ERR;
}

typedef struct {
    const char *stable_id;
    int input_index;
    int64_t node_key;
} repo_stable_node_request_t;

static int repo_stable_node_request_compare(const void *left_ptr, const void *right_ptr) {
    const repo_stable_node_request_t *left = left_ptr;
    const repo_stable_node_request_t *right = right_ptr;
    return strcmp(left->stable_id, right->stable_id);
}

static repo_stable_node_request_t *repo_stable_node_request_find(
    repo_stable_node_request_t *requests, int count, const char *stable_id) {
    repo_stable_node_request_t key = {.stable_id = stable_id};
    return bsearch(&key, requests, (size_t)count, sizeof(*requests),
                   repo_stable_node_request_compare);
}

int cbm_zova_repository_find_nodes_by_stable_ids(cbm_zova_repository_t *repo,
                                                  const char *workspace_id,
                                                  const char *const *stable_ids,
                                                  int count, cbm_node_t **out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || count < 0 || !out ||
        (count > 0 && !stable_ids)) return CBM_STORE_ERR;
    *out = NULL;
    if (count == 0) return CBM_STORE_OK;
    repo_stable_node_request_t *requests = calloc((size_t)count, sizeof(*requests));
    int64_t *keys = calloc((size_t)count, sizeof(*keys));
    cbm_node_t *nodes = calloc((size_t)count, sizeof(*nodes));
    if (!requests || !keys || !nodes) goto error;
    for (int i = 0; i < count; i++) {
        if (!stable_ids[i] || !stable_ids[i][0]) goto error;
        requests[i] = (repo_stable_node_request_t){
            .stable_id = stable_ids[i], .input_index = i};
    }
    if (count > 1) qsort(requests, (size_t)count, sizeof(*requests),
                         repo_stable_node_request_compare);
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) goto error;
    zova_graph_scan_cursor cursor = {0};
    for (;;) {
        zova_graph_scan_results page = {0};
        zova_status status = zova_graph_scan(&(zova_graph_scan_request){
            .db = repo->db, .graph_name = graph_name, .node_after = cursor,
            .node_limit = REPO_GRAPH_SCAN_PAGE, .edge_limit = 0, .out_results = &page});
        if (status != ZOVA_OK) { zova_graph_scan_results_free(&page); goto error; }
        for (size_t i = 0; i < page.nodes_len; i++) {
            repo_stable_node_request_t *request = repo_stable_node_request_find(
                requests, count, page.nodes[i].node_id);
            if (request) request->node_key = page.nodes[i].node_key;
        }
        if (page.nodes_len) {
            const zova_graph_scan_node *last = &page.nodes[page.nodes_len - 1];
            cursor = (zova_graph_scan_cursor){.created_order = last->created_order,
                                              .key = last->node_key};
        }
        uint8_t more = page.has_more_nodes;
        zova_graph_scan_results_free(&page);
        if (!more) break;
    }
    for (int i = 0; i < count; i++) {
        if (requests[i].node_key <= 0) goto not_found;
        keys[requests[i].input_index] = requests[i].node_key;
    }
    zova_graph_keyed_node_results native = {0};
    if (repo_native_nodes_by_key(repo, keys, (size_t)count, &native) != 0) goto error;
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,n.properties "
        "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key "
        "WHERE n.workspace_key=?1 AND n.zova_node_key=?2", &statement);
    for (int i = 0; rc == 0 && i < count; i++) {
        if (!native.items[i].found || native.items[i].node_key != keys[i] ||
            !native.items[i].node_id || !native.items[i].kind ||
            strcmp(native.items[i].node_id, stable_ids[i]) != 0) { rc = -1; break; }
        if (zova_statement_reset(statement) != ZOVA_OK ||
            zova_statement_clear_bindings(statement) != ZOVA_OK ||
            repo_bind_i64(statement, 1, repo->workspace_key) != 0 ||
            repo_bind_i64(statement, 2, keys[i]) != 0) { rc = -1; break; }
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        cbm_node_t *node = &nodes[i];
        node->id = stable_numeric_id(native.items[i].node_id);
        node->project = repo_dup((const uint8_t *)repo->project, strlen(repo->project));
        node->label = repo_dup((const uint8_t *)native.items[i].kind,
                               native.items[i].kind_len);
        node->name = repo_column_text(statement, 0);
        node->qualified_name = repo_column_text(statement, 1);
        node->file_path = repo_column_text(statement, 2);
        node->start_line = (int)repo_column_i64(statement, 3);
        node->end_line = (int)repo_column_i64(statement, 4);
        node->properties_json = repo_column_text(statement, 5);
        if (!node->project || !node->label || !node->name || !node->qualified_name ||
            !node->file_path || !node->properties_json) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    zova_graph_keyed_node_results_free(&native);
    if (rc != 0) goto error;
    free(requests); free(keys);
    *out = nodes;
    return CBM_STORE_OK;
not_found:
    free(requests); free(keys); free(nodes);
    return CBM_STORE_NOT_FOUND;
error:
    free(requests); free(keys);
    cbm_store_free_nodes(nodes, count);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_find_nodes_by_keys(cbm_zova_repository_t *repo,
                                           const char *workspace_id,
                                           const int64_t *node_keys,
                                           int count, cbm_node_t **out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || count < 0 || !out ||
        (count > 0 && !node_keys)) return CBM_STORE_ERR;
    *out = NULL;
    if (count == 0) return CBM_STORE_OK;
    cbm_node_t *nodes = calloc((size_t)count, sizeof(*nodes));
    if (!nodes) return CBM_STORE_ERR;
    for (int i = 0; i < count; i++) if (node_keys[i] <= 0) goto error;
    zova_graph_keyed_node_results native = {0};
    if (repo_native_nodes_by_key(repo, node_keys, (size_t)count, &native) != 0) goto error;
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,n.properties "
        "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key "
        "WHERE n.workspace_key=?1 AND n.zova_node_key=?2", &statement);
    for (int i = 0; rc == 0 && i < count; i++) {
        if (!native.items[i].found || native.items[i].node_key != node_keys[i] ||
            !native.items[i].node_id || !native.items[i].kind ||
            zova_statement_reset(statement) != ZOVA_OK ||
            zova_statement_clear_bindings(statement) != ZOVA_OK ||
            repo_bind_i64(statement, 1, repo->workspace_key) != 0 ||
            repo_bind_i64(statement, 2, node_keys[i]) != 0) { rc = -1; break; }
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        cbm_node_t *node = &nodes[i];
        node->id = stable_numeric_id(native.items[i].node_id);
        node->project = repo_dup((const uint8_t *)repo->project, strlen(repo->project));
        node->label = repo_dup((const uint8_t *)native.items[i].kind,
                               native.items[i].kind_len);
        node->name = repo_column_text(statement, 0);
        node->qualified_name = repo_column_text(statement, 1);
        node->file_path = repo_column_text(statement, 2);
        node->start_line = (int)repo_column_i64(statement, 3);
        node->end_line = (int)repo_column_i64(statement, 4);
        node->properties_json = repo_column_text(statement, 5);
        if (!node->project || !node->label || !node->name || !node->qualified_name ||
            !node->file_path || !node->properties_json) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    zova_graph_keyed_node_results_free(&native);
    if (rc != 0) goto error;
    *out = nodes;
    return CBM_STORE_OK;
error:
    cbm_store_free_nodes(nodes, count);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_stable_id_for_numeric_id(cbm_zova_repository_t *repo,
                                                  const char *workspace_id,
                                                  int64_t numeric_id, char *out,
                                                  size_t out_size) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !out || out_size == 0)
        return CBM_STORE_ERR;
    out[0] = '\0';
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) return CBM_STORE_ERR;
    zova_graph_scan_cursor cursor = {0};
    for (;;) {
        zova_graph_scan_results page = {0};
        zova_status status = zova_graph_scan(&(zova_graph_scan_request){
            .db = repo->db, .graph_name = graph_name, .node_after = cursor,
            .node_limit = REPO_GRAPH_SCAN_PAGE, .edge_limit = 0, .out_results = &page});
        if (status != ZOVA_OK) return CBM_STORE_ERR;
        for (size_t i = 0; i < page.nodes_len; i++) {
            const char *stable = page.nodes[i].node_id;
            if (stable && stable_numeric_id(stable) == numeric_id) {
                int written = snprintf(out, out_size, "%s", stable);
                zova_graph_scan_results_free(&page);
                return written >= 0 && (size_t)written < out_size
                           ? CBM_STORE_OK : CBM_STORE_ERR;
            }
        }
        if (page.nodes_len) {
            const zova_graph_scan_node *last = &page.nodes[page.nodes_len - 1];
            cursor = (zova_graph_scan_cursor){.created_order = last->created_order,
                                              .key = last->node_key};
        }
        uint8_t more = page.has_more_nodes;
        zova_graph_scan_results_free(&page);
        if (!more) return CBM_STORE_NOT_FOUND;
    }
}

int cbm_zova_repository_find_node_by_numeric_id(cbm_zova_repository_t *repo,
                                                 const char *workspace_id,
                                                 int64_t numeric_id, cbm_node_t *out) {
    char stable[CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    int rc = cbm_zova_repository_stable_id_for_numeric_id(
        repo, workspace_id, numeric_id, stable, sizeof(stable));
    return rc == CBM_STORE_OK
               ? cbm_zova_repository_find_node_by_stable_id(repo, workspace_id, stable, out)
               : rc;
}

int cbm_zova_repository_find_nodes_by_file_overlap(cbm_zova_repository_t *repo,
                                                    const char *workspace_id,
                                                    const char *file_path,
                                                    int start_line, int end_line,
                                                    cbm_node_t **out, int *out_count) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !file_path ||
        !out || !out_count) return CBM_STORE_ERR;
    *out = NULL;
    *out_count = 0;
    zova_statement *stmt = NULL;
    const char *sql =
        "SELECT n.zova_node_key,n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,"
        "n.properties FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key "
        "WHERE n.workspace_key=?1 AND f.file_path=?2 "
        "AND n.label NOT IN ('Module','Package','File','Folder') "
        "AND n.start_line<=?3 AND n.end_line>=?4 "
        "ORDER BY n.start_line,n.qualified_name,n.zova_node_key";
    int rc = repo_prepare(repo, sql, &stmt);
    if (rc == 0) rc = repo_bind_i64(stmt, 1, repo->workspace_key);
    if (rc == 0) rc = repo_bind_text(stmt, 2, file_path);
    if (rc == 0) rc = repo_bind_i64(stmt, 3, end_line);
    if (rc == 0) rc = repo_bind_i64(stmt, 4, start_line);
    int cap = 8, count = 0;
    cbm_node_t *nodes = rc == 0 ? calloc((size_t)cap, sizeof(*nodes)) : NULL;
    int64_t *keys = rc == 0 ? calloc((size_t)cap, sizeof(*keys)) : NULL;
    if (rc != 0 || !nodes || !keys) goto error;
    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(stmt, &step) != 0) goto error;
        if (step == ZOVA_STEP_DONE) break;
        if (count == cap) {
            int old_cap = cap;
            cap *= 2;
            cbm_node_t *grown_nodes = realloc(nodes, (size_t)cap * sizeof(*grown_nodes));
            int64_t *grown_keys = realloc(keys, (size_t)cap * sizeof(*grown_keys));
            if (!grown_nodes || !grown_keys) {
                if (grown_nodes) nodes = grown_nodes;
                if (grown_keys) keys = grown_keys;
                goto error;
            }
            nodes = grown_nodes;
            keys = grown_keys;
            memset(nodes + old_cap, 0, (size_t)(cap - old_cap) * sizeof(*nodes));
        }
        keys[count] = repo_column_i64(stmt, 0);
        nodes[count].project = repo_dup((const uint8_t *)repo->project, strlen(repo->project));
        nodes[count].name = repo_column_text(stmt, 1);
        nodes[count].qualified_name = repo_column_text(stmt, 2);
        nodes[count].file_path = repo_column_text(stmt, 3);
        nodes[count].start_line = (int)repo_column_i64(stmt, 4);
        nodes[count].end_line = (int)repo_column_i64(stmt, 5);
        nodes[count].properties_json = repo_column_text(stmt, 6);
        count++;
    }
    (void)zova_statement_finalize(stmt);
    stmt = NULL;
    zova_graph_keyed_node_results native = {0};
    if (count && repo_native_nodes_by_key(repo, keys, (size_t)count, &native) != 0)
        goto error;
    for (int i = 0; i < count; i++) {
        if (!native.items[i].found || !native.items[i].node_id || !native.items[i].kind) {
            zova_graph_keyed_node_results_free(&native);
            goto error;
        }
        nodes[i].id = stable_numeric_id(native.items[i].node_id);
        nodes[i].label = repo_dup((const uint8_t *)native.items[i].kind,
                                  native.items[i].kind_len);
    }
    zova_graph_keyed_node_results_free(&native);
    free(keys);
    *out = nodes;
    *out_count = count;
    return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    free(keys);
    cbm_store_free_nodes(nodes, count);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_get_project(cbm_zova_repository_t *repo, const char *workspace_id,
                                    cbm_project_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !out)
        return CBM_STORE_ERR;
    zova_statement *stmt = NULL;
    if (repo_prepare(repo, "SELECT project,indexed_at,root_path FROM cbm_projects_v1 "
                           "WHERE workspace_key=?1", &stmt) != 0 ||
        repo_bind_i64(stmt, 1, repo->workspace_key) != 0) goto error;
    zova_step_result step = ZOVA_STEP_DONE;
    if (repo_step(stmt, &step) != 0 || step != ZOVA_STEP_ROW) goto error;
    memset(out, 0, sizeof(*out));
    out->name = repo_column_text(stmt, 0);
    out->indexed_at = repo_column_text(stmt, 1);
    out->root_path = repo_column_text(stmt, 2);
    (void)zova_statement_finalize(stmt);
    return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_index_status(cbm_zova_repository_t *repo, const char *workspace_id,
                                      int64_t *generation,
                                      char **indexed_at) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !generation || !indexed_at)
        return CBM_STORE_ERR;
    cbm_project_t project = {0};
    if (cbm_zova_repository_get_project(repo, workspace_id, &project) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    *generation = repo->generation;
    *indexed_at = (char *)project.indexed_at;
    free((void *)project.name);
    free((void *)project.root_path);
    return CBM_STORE_OK;
}

int cbm_zova_repository_counts(cbm_zova_repository_t *repo, const char *workspace_id,
                               int *nodes, int *edges) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !nodes || !edges)
        return CBM_STORE_ERR;
    zova_statement *stmt = NULL;
    if (repo_prepare(repo,
                     "SELECT (SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=?1),"
                     "metadata_edges FROM cbm_generation_integrity_v2 "
                     "WHERE workspace_key=?1 AND generation=?2",
                     &stmt) != 0 ||
        repo_bind_i64(stmt, 1, repo->workspace_key) != 0 ||
        repo_bind_i64(stmt, 2, repo->generation) != 0) goto error;
    zova_step_result step = ZOVA_STEP_DONE;
    if (repo_step(stmt, &step) != 0 || step != ZOVA_STEP_ROW) goto error;
    *nodes = (int)repo_column_i64(stmt, 0);
    *edges = (int)repo_column_i64(stmt, 1);
    (void)zova_statement_finalize(stmt);
    return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_project_summary(cbm_zova_repository_t *repo,
                                        const char *workspace_id,
                                        cbm_project_summary_export_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    zova_statement *stmt = NULL;
    if (repo_prepare(repo, "SELECT summary,source_hash,created_at,updated_at "
                           "FROM cbm_project_summaries_v2 WHERE workspace_key=?1", &stmt) != 0 ||
        repo_bind_i64(stmt, 1, repo->workspace_key) != 0) goto error;
    zova_step_result step = ZOVA_STEP_DONE;
    if (repo_step(stmt, &step) != 0) goto error;
    if (step == ZOVA_STEP_DONE) { (void)zova_statement_finalize(stmt); return CBM_STORE_NOT_FOUND; }
    out->summary=repo_column_text(stmt,0); out->source_hash=repo_column_text(stmt,1);
    out->created_at=repo_column_text(stmt,2); out->updated_at=repo_column_text(stmt,3);
    (void)zova_statement_finalize(stmt);
    return CBM_STORE_OK;
error:
    if(stmt)(void)zova_statement_finalize(stmt);
    cbm_store_project_summary_export_free(out);
    return CBM_STORE_ERR;
}

typedef struct {
    cbm_zova_repository_t *repo;
    const char *source;
    const char *target;
    const char *edge_type;
    size_t edge_type_len;
    cbm_edge_t **edges;
    int *count;
    int *capacity;
} repo_payload_edge_context_t;

static int repo_append_payload_edge(
    const cbm_zova_edge_payload_record_t *record, void *context_ptr) {
    repo_payload_edge_context_t *context = context_ptr;
    if (!record || !context || !context->repo || !context->edges ||
        !context->count || !context->capacity) return -1;
    if (*context->count == *context->capacity) {
        if (*context->capacity > INT_MAX / 2) return -1;
        int grown_capacity = *context->capacity * 2;
        cbm_edge_t *grown = realloc(
            *context->edges, (size_t)grown_capacity * sizeof(*grown));
        if (!grown) return -1;
        memset(grown + *context->capacity, 0,
               (size_t)(grown_capacity - *context->capacity) * sizeof(*grown));
        *context->edges = grown;
        *context->capacity = grown_capacity;
    }
    char local_name[CBM_ZOVA_WORKSPACE_ID_MAX + 80];
    if (record->local_name.len >= sizeof(local_name)) return -1;
    memcpy(local_name, record->local_name.data, record->local_name.len);
    local_name[record->local_name.len] = '\0';
    char edge_id[CBM_ZOVA_WORKSPACE_ID_MAX + 80];
    if (repo_edge_id(context->repo->workspace_id, context->source,
                     context->edge_type, context->target, local_name,
                     edge_id, sizeof(edge_id)) != 0) return -1;
    cbm_edge_t *edge = &(*context->edges)[*context->count];
    char *project = repo_dup((const uint8_t *)context->repo->project,
                             strlen(context->repo->project));
    char *type = repo_dup((const uint8_t *)context->edge_type,
                          context->edge_type_len);
    char *properties = repo_dup(record->properties.data,
                                record->properties.len);
    if (!project || !type || !properties) {
        free(project);
        free(type);
        free(properties);
        return -1;
    }
    *edge = (cbm_edge_t){
        .id = stable_numeric_id(edge_id),
        .project = project,
        .source_id = stable_numeric_id(context->source),
        .target_id = stable_numeric_id(context->target),
        .type = type,
        .properties_json = properties,
    };
    (*context->count)++;
    return 0;
}

int cbm_zova_repository_find_edges(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *stable_id,
                                   const char *direction, cbm_edge_t **out, int *out_count) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !stable_id || !out ||
        !out_count) return CBM_STORE_ERR;
    *out = NULL; *out_count = 0;
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) return CBM_STORE_ERR;
    int directions[2];
    int direction_count = 1;
    if (!direction || strcmp(direction, "any") == 0) {
        directions[0] = ZOVA_GRAPH_NEIGHBOR_OUTGOING;
        directions[1] = ZOVA_GRAPH_NEIGHBOR_INCOMING;
        direction_count = 2;
    } else directions[0] = strcmp(direction, "inbound") == 0
        ? ZOVA_GRAPH_NEIGHBOR_INCOMING : ZOVA_GRAPH_NEIGHBOR_OUTGOING;
    int cap = 8, count = 0;
    cbm_edge_t *edges = calloc((size_t)cap, sizeof(*edges));
    if (!edges) return CBM_STORE_ERR;
    for (int d = 0; d < direction_count; d++) {
        zova_graph_keyed_neighbor_results neighbors = {0};
        if (zova_graph_neighbors_keyed(&(zova_graph_neighbors_keyed_request){
                .db=repo->db,.graph_name=graph_name,.node_id=stable_id,
                .direction=directions[d],.edge_type=NULL,.limit=(size_t)INT64_MAX,
                .out_results=&neighbors}) != ZOVA_OK) {
            zova_graph_keyed_neighbor_results_free(&neighbors);
            cbm_store_free_edges(edges,count); return CBM_STORE_ERR;
        }
        int64_t *edge_keys = neighbors.len
            ? calloc(neighbors.len, sizeof(*edge_keys)) : NULL;
        zova_graph_edge_payload_results payloads = {0};
        if (neighbors.len && !edge_keys) {
            zova_graph_keyed_neighbor_results_free(&neighbors);
            cbm_store_free_edges(edges, count);
            return CBM_STORE_ERR;
        }
        for (size_t i = 0; i < neighbors.len; i++)
            edge_keys[i] = neighbors.items[i].edge_key;
        if (neighbors.len &&
            zova_graph_edge_payload_get_many(
                &(zova_graph_edge_payload_get_many_request){
                    .db = repo->db,
                    .graph_name = graph_name,
                    .edge_keys = edge_keys,
                    .key_count = neighbors.len,
                    .out_results = &payloads,
                }) != ZOVA_OK) {
            free(edge_keys);
            zova_graph_keyed_neighbor_results_free(&neighbors);
            cbm_store_free_edges(edges, count);
            return CBM_STORE_ERR;
        }
        free(edge_keys);
        if (payloads.len != neighbors.len) {
            zova_graph_edge_payload_results_free(&payloads);
            zova_graph_keyed_neighbor_results_free(&neighbors);
            cbm_store_free_edges(edges, count);
            return CBM_STORE_ERR;
        }
        for (size_t i = 0; i < neighbors.len; i++) {
            if (direction_count == 2 && d == 1 &&
                strcmp(neighbors.items[i].node_id, stable_id) == 0) continue;
            if (!payloads.items[i].found ||
                payloads.items[i].edge_key != neighbors.items[i].edge_key) {
                zova_graph_edge_payload_results_free(&payloads);
                zova_graph_keyed_neighbor_results_free(&neighbors);
                cbm_store_free_edges(edges, count);
                return CBM_STORE_ERR;
            }
            repo_payload_edge_context_t context = {
                .repo = repo,
                .source = directions[d] == ZOVA_GRAPH_NEIGHBOR_OUTGOING
                              ? stable_id : neighbors.items[i].node_id,
                .target = directions[d] == ZOVA_GRAPH_NEIGHBOR_OUTGOING
                              ? neighbors.items[i].node_id : stable_id,
                .edge_type = neighbors.items[i].edge_type,
                .edge_type_len = neighbors.items[i].edge_type_len,
                .edges = &edges,
                .count = &count,
                .capacity = &cap,
            };
            if (cbm_zova_edge_payload_visit(
                    payloads.items[i].payload, payloads.items[i].payload_len,
                    repo_append_payload_edge, &context, NULL) != 0) {
                zova_graph_edge_payload_results_free(&payloads);
                zova_graph_keyed_neighbor_results_free(&neighbors);
                cbm_store_free_edges(edges, count);
                return CBM_STORE_ERR;
            }
        }
        zova_graph_edge_payload_results_free(&payloads);
        zova_graph_keyed_neighbor_results_free(&neighbors);
    }
    *out=edges;*out_count=count;return CBM_STORE_OK;
}

int cbm_zova_repository_search_fts(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *query,
                                   const char *file_pattern, int limit, int offset,
                                   cbm_search_output_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !query || !query[0] ||
        !out) return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    if (limit <= 0) limit = 10;
    zova_statement *stmt = NULL;
    const char *sql = "SELECT n.zova_node_key,n.label,n.name,n.qualified_name,p.file_path,"
        "n.start_line,n.end_line,n.properties,(bm25(cbm_nodes_fts_v1)-CASE "
        "WHEN n.label IN ('Function','Method') THEN 10.0 WHEN n.label='Route' THEN 8.0 "
        "WHEN n.label IN ('Class','Interface','Type','Enum') THEN 5.0 ELSE 0.0 END) AS rank "
        "FROM cbm_nodes_fts_v1 f "
        "JOIN cbm_nodes_v1 n ON n.zova_node_key=f.rowid "
        "JOIN cbm_files_v1 p ON p.file_key=n.file_key "
        "WHERE n.workspace_key=?1 AND cbm_nodes_fts_v1 MATCH ?2 "
        "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') "
        "AND (?3 IS NULL OR p.file_path GLOB ?3) "
        "ORDER BY rank,n.qualified_name,n.zova_node_key LIMIT ?4 OFFSET ?5";
    if (repo_prepare(repo, sql, &stmt) != 0 || repo_bind_i64(stmt, 1, repo->workspace_key) != 0 ||
        repo_bind_text(stmt, 2, query) != 0) goto error;
    zova_status bst = file_pattern && file_pattern[0]
                          ? zova_statement_bind_text(&(zova_statement_bind_text_request){
                                .statement=stmt,.index=3,.data=(const uint8_t *)file_pattern,
                                .len=strlen(file_pattern)})
                          : zova_statement_bind_null(&(zova_statement_bind_null_request){
                                .statement=stmt,.index=3});
    if (bst != ZOVA_OK || repo_bind_i64(stmt, 4, limit) != 0 ||
        repo_bind_i64(stmt, 5, offset) != 0) goto error;
    int cap = 8, count = 0;
    cbm_search_result_t *results = calloc((size_t)cap, sizeof(*results));
    int64_t *keys = calloc((size_t)cap, sizeof(*keys));
    if (!results || !keys) { free(results); free(keys); goto error; }
    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(stmt, &step) != 0) { cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count}); goto error; }
        if (step == ZOVA_STEP_DONE) break;
        if (count == cap) { int old=cap; cap *= 2; cbm_search_result_t *g = realloc(results,(size_t)cap*sizeof(*g));
            int64_t *kg=realloc(keys,(size_t)cap*sizeof(*kg));
            if (!g || !kg) { free(kg); cbm_store_search_free(&(cbm_search_output_t){.results=g?g:results,.count=count}); goto error; }
            results=g;keys=kg;memset(results+count,0,(size_t)(cap-old)*sizeof(*results)); }
        cbm_node_t *n=&results[count].node; keys[count]=repo_column_i64(stmt,0);
        n->project=repo_dup((const uint8_t *)repo->project,strlen(repo->project));
        n->label=repo_column_text(stmt,1); n->name=repo_column_text(stmt,2);
        n->qualified_name=repo_column_text(stmt,3); n->file_path=repo_column_text(stmt,4);
        n->start_line=(int)repo_column_i64(stmt,5); n->end_line=(int)repo_column_i64(stmt,6);
        n->properties_json=repo_column_text(stmt,7);
        results[count].rank=repo_column_double(stmt,8);
        count++;
    }
    (void)zova_statement_finalize(stmt);stmt=NULL;
    zova_graph_keyed_node_results native={0};
    if(repo_native_nodes_by_key(repo,keys,(size_t)count,&native)!=0){free(keys);cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});goto error;}
    for(int i=0;i<count;i++){if(!native.items[i].found){zova_graph_keyed_node_results_free(&native);free(keys);cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});goto error;}
        results[i].node.id=stable_numeric_id(native.items[i].node_id);
        free((char*)results[i].node.label);results[i].node.label=repo_dup((const uint8_t*)native.items[i].kind,native.items[i].kind_len);}
    zova_graph_keyed_node_results_free(&native);free(keys);
    out->results=results; out->count=count; out->total=count; return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_search(cbm_zova_repository_t *repo, const char *workspace_id,
                               const cbm_search_params_t *params, cbm_search_output_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !params || !out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    char sql[4096] = "SELECT n.zova_node_key,n.label,n.name,n.qualified_name,f.file_path,"
        "n.start_line,n.end_line,n.properties FROM cbm_nodes_v1 n "
        "JOIN cbm_files_v1 f ON f.file_key=n.file_key WHERE n.workspace_key=?1";
    const char *binds[64] = {0}; int bind_count = 0;
#define REPO_FILTER(clause, value) do { if ((value) && (value)[0]) { strncat(sql, (clause), sizeof(sql)-strlen(sql)-1); binds[bind_count++]=(value); } } while (0)
    REPO_FILTER(" AND n.label=?", params->label);
    REPO_FILTER(params->case_sensitive?" AND regexp(?,n.name)":" AND iregexp(?,n.name)", params->name_pattern);
    REPO_FILTER(params->case_sensitive?" AND regexp(?,n.qualified_name)":" AND iregexp(?,n.qualified_name)", params->qn_pattern);
    REPO_FILTER(" AND f.file_path GLOB ?", params->file_pattern);
#undef REPO_FILTER
    if(params->exclude_labels){for(int i=0;params->exclude_labels[i]&&bind_count<63;i++){
        strncat(sql," AND n.label<>?",sizeof(sql)-strlen(sql)-1);binds[bind_count++]=params->exclude_labels[i];}}
    strncat(sql, " ORDER BY n.name,n.qualified_name,n.zova_node_key", sizeof(sql)-strlen(sql)-1);
    zova_statement *stmt=NULL;
    if (repo_prepare(repo,sql,&stmt)!=0 || repo_bind_i64(stmt,1,repo->workspace_key)!=0) goto regular_error;
    int idx=2;
    for(int i=0;i<bind_count;i++) if(repo_bind_text(stmt,idx++,binds[i])!=0) goto regular_error;
    int cap=64,count=0; cbm_search_result_t *results=calloc((size_t)cap,sizeof(*results));int64_t*keys=calloc((size_t)cap,sizeof(*keys)); if(!results||!keys){free(results);free(keys);goto regular_error;}
    for(;;){zova_step_result step=ZOVA_STEP_DONE; if(repo_step(stmt,&step)!=0){cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});goto regular_error;} if(step==ZOVA_STEP_DONE)break;
        if(count==cap){int old=cap;cap*=2;cbm_search_result_t*g=realloc(results,(size_t)cap*sizeof(*g));int64_t*kg=realloc(keys,(size_t)cap*sizeof(*kg));if(!g||!kg){free(kg);cbm_store_search_free(&(cbm_search_output_t){.results=g?g:results,.count=count});goto regular_error;}results=g;keys=kg;memset(results+count,0,(size_t)(cap-old)*sizeof(*results));}
        cbm_node_t*n=&results[count].node;keys[count]=repo_column_i64(stmt,0);n->project=repo_dup((const uint8_t *)repo->project,strlen(repo->project));n->label=repo_column_text(stmt,1);n->name=repo_column_text(stmt,2);n->qualified_name=repo_column_text(stmt,3);n->file_path=repo_column_text(stmt,4);n->start_line=(int)repo_column_i64(stmt,5);n->end_line=(int)repo_column_i64(stmt,6);n->properties_json=repo_column_text(stmt,7);count++;}
    (void)zova_statement_finalize(stmt);stmt=NULL;
    zova_graph_keyed_node_results native={0};uint64_t*in=calloc((size_t)count,sizeof(*in));uint64_t*outd=calloc((size_t)count,sizeof(*outd));uint64_t*tmp=calloc((size_t)count,sizeof(*tmp));uint64_t*calls_in=calloc((size_t)count,sizeof(*calls_in));uint64_t*calls_out=calloc((size_t)count,sizeof(*calls_out));uint64_t*rel_in=calloc((size_t)count,sizeof(*rel_in));uint64_t*rel_out=calloc((size_t)count,sizeof(*rel_out));
    if((count&&(!in||!outd||!tmp||!calls_in||!calls_out||!rel_in||!rel_out))||repo_native_nodes_by_key(repo,keys,(size_t)count,&native)!=0)goto search_data_error;
    const char*degree_types[]={"CALLS","USAGE","INHERITS","IMPLEMENTS"};
    for(int t=0;t<4;t++){memset(tmp,0,(size_t)count*sizeof(*tmp));if(repo_degree_many(repo,keys,(size_t)count,ZOVA_GRAPH_NEIGHBOR_INCOMING,degree_types[t],tmp)!=0)goto search_data_error;for(int i=0;i<count;i++)in[i]+=tmp[i];if(t==0)memcpy(calls_in,tmp,(size_t)count*sizeof(*tmp));memset(tmp,0,(size_t)count*sizeof(*tmp));if(repo_degree_many(repo,keys,(size_t)count,ZOVA_GRAPH_NEIGHBOR_OUTGOING,degree_types[t],tmp)!=0)goto search_data_error;for(int i=0;i<count;i++)outd[i]+=tmp[i];if(t==0)memcpy(calls_out,tmp,(size_t)count*sizeof(*tmp));}
    if(params->relationship){if(repo_degree_many(repo,keys,(size_t)count,ZOVA_GRAPH_NEIGHBOR_INCOMING,params->relationship,rel_in)!=0||repo_degree_many(repo,keys,(size_t)count,ZOVA_GRAPH_NEIGHBOR_OUTGOING,params->relationship,rel_out)!=0)goto search_data_error;}
    int limit=params->limit>0?params->limit:10;int page_capacity=limit<count?limit:count;
    cbm_search_result_t*page=calloc((size_t)page_capacity,sizeof(*page));if(page_capacity&&!page)goto search_data_error;int accepted=0,written=0;
    for(int i=0;i<count;i++){if(!native.items[i].found)goto search_data_error;int degree=(int)(in[i]+outd[i]);bool keep=true;if(params->relationship){keep=!params->direction||strcmp(params->direction,"any")==0?rel_in[i]+rel_out[i]>0:strcmp(params->direction,"inbound")==0?rel_in[i]>0:rel_out[i]>0;}if(keep&&params->exclude_entry_points&&calls_in[i]==0&&calls_out[i]>0)keep=false;if(keep&&params->min_degree>=0&&degree<params->min_degree)keep=false;if(keep&&params->max_degree>=0&&degree>params->max_degree)keep=false;if(!keep)continue;if(accepted>=params->offset&&written<limit){results[i].node.id=stable_numeric_id(native.items[i].node_id);free((char*)results[i].node.label);results[i].node.label=repo_dup((const uint8_t*)native.items[i].kind,native.items[i].kind_len);results[i].in_degree=(int)in[i];results[i].out_degree=(int)outd[i];page[written++]=results[i];memset(&results[i],0,sizeof(results[i]));}accepted++;}
    cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});zova_graph_keyed_node_results_free(&native);free(keys);free(in);free(outd);free(tmp);free(calls_in);free(calls_out);free(rel_in);free(rel_out);out->results=page;out->count=written;out->total=accepted;return CBM_STORE_OK;
search_data_error: zova_graph_keyed_node_results_free(&native);free(keys);free(in);free(outd);free(tmp);free(calls_in);free(calls_out);free(rel_in);free(rel_out);cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});
regular_error: if(stmt)(void)zova_statement_finalize(stmt);return CBM_STORE_ERR;
}

void cbm_zova_workspace_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot) {
    if (!snapshot) return;
    free(snapshot->root_path);
    free(snapshot->project);
    free(snapshot->indexed_at);
    free(snapshot->model_fingerprint);
    for (int i = 0; i < snapshot->node_count; i++) {
        free(snapshot->node_stable_ids ? snapshot->node_stable_ids[i] : NULL);
        free((char *)snapshot->nodes[i].project);
        free((char *)snapshot->nodes[i].label);
        free((char *)snapshot->nodes[i].name);
        free((char *)snapshot->nodes[i].qualified_name);
        free((char *)snapshot->nodes[i].file_path);
        free((char *)snapshot->nodes[i].properties);
    }
    free(snapshot->node_stable_ids);
    free(snapshot->zova_node_keys);
    free(snapshot->node_source_ordinals);
    free(snapshot->nodes);
    for (int i = 0; i < snapshot->edge_count; i++) {
        free(snapshot->edge_ids ? snapshot->edge_ids[i] : NULL);
        if (snapshot->edges[i].project != snapshot->project)
            free((char *)snapshot->edges[i].project);
        if (!snapshot->edge_string_arena) {
            free((char *)snapshot->edges[i].type);
            free((char *)snapshot->edges[i].properties);
            free((char *)snapshot->edges[i].url_path);
            free((char *)snapshot->edges[i].local_name);
        }
    }
    free(snapshot->edge_ids);
    free(snapshot->edges);
    repo_string_arena_free(snapshot->edge_string_arena);
    for (int i = 0; i < snapshot->topology_edge_count; i++) {
        free(snapshot->topology_edges[i].source_stable_id);
        free(snapshot->topology_edges[i].edge_type);
        free(snapshot->topology_edges[i].target_stable_id);
    }
    free(snapshot->topology_edges);
    for (int i = 0; i < snapshot->node_vector_count; i++) {
        free(snapshot->node_vector_ids ? snapshot->node_vector_ids[i] : NULL);
        free((char *)snapshot->node_vectors[i].project);
        free((void *)snapshot->node_vectors[i].vector);
    }
    free(snapshot->node_vector_ids);
    free(snapshot->node_vectors);
    for (int i = 0; i < snapshot->token_vector_count; i++) {
        free(snapshot->token_vector_ids ? snapshot->token_vector_ids[i] : NULL);
        free((char *)snapshot->token_vectors[i].project);
        free((char *)snapshot->token_vectors[i].token);
        free((void *)snapshot->token_vectors[i].vector);
    }
    free(snapshot->token_vector_ids);
    free(snapshot->token_vectors);
    for (int i = 0; i < snapshot->file_hash_count; i++) {
        free((char *)snapshot->file_hashes[i].file_path);
        free((char *)snapshot->file_hashes[i].content_hash);
    }
    free(snapshot->file_hashes);
    free((char *)snapshot->project_summary.summary);
    free((char *)snapshot->project_summary.source_hash);
    free((char *)snapshot->project_summary.created_at);
    free((char *)snapshot->project_summary.updated_at);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int snapshot_count(cbm_zova_repository_t *repo, const char *sql) {
    zova_statement *statement = NULL;
    if (repo_prepare(repo, sql, &statement) != 0 ||
        repo_bind_i64(statement, 1, repo->workspace_key) != 0) {
        if (statement) (void)zova_statement_finalize(statement);
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int count = repo_step(statement, &step) == 0 && step == ZOVA_STEP_ROW
                    ? (int)repo_column_i64(statement, 0)
                    : -1;
    (void)zova_statement_finalize(statement);
    return count;
}

typedef struct {
    int64_t node_key;
    int64_t snapshot_id;
} snapshot_node_key_ref_t;

static int snapshot_node_key_ref_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_node_key_ref_t *left = left_ptr;
    const snapshot_node_key_ref_t *right = right_ptr;
    return (left->node_key > right->node_key) - (left->node_key < right->node_key);
}

static int64_t snapshot_map_node_key(const snapshot_node_key_ref_t *refs, int count,
                                     int64_t node_key) {
    int low = 0, high = count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (refs[mid].node_key == node_key) return refs[mid].snapshot_id;
        if (refs[mid].node_key < node_key) low = mid + 1;
        else high = mid;
    }
    return -1;
}

typedef struct {
    int64_t node_key;
    char *stable_id;
    CBMDumpNode node;
    uint64_t source_ordinal;
} snapshot_node_row_t;

static int snapshot_node_row_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_node_row_t *left = left_ptr;
    const snapshot_node_row_t *right = right_ptr;
    return strcmp(left->stable_id, right->stable_id);
}

static int snapshot_read_nodes(cbm_zova_repository_t *repo,
                               cbm_zova_workspace_snapshot_t *snapshot,
                               char ***out_stable_ids,
                               snapshot_node_key_ref_t **out_node_keys) {
    struct timespec phase_started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    int count = snapshot_count(repo,
        "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=?1");
    if (count < 0) return -1;
    snapshot->node_count = count;
    snapshot->nodes = count ? calloc((size_t)count, sizeof(*snapshot->nodes)) : NULL;
    char **stable_ids = count ? calloc((size_t)count, sizeof(*stable_ids)) : NULL;
    snapshot_node_key_ref_t *node_keys =
        count ? calloc((size_t)count, sizeof(*node_keys)) : NULL;
    snapshot_node_row_t *rows = count ? calloc((size_t)count, sizeof(*rows)) : NULL;
    int64_t *keys = count ? calloc((size_t)count, sizeof(*keys)) : NULL;
    snapshot->node_source_ordinals =
        count ? calloc((size_t)count, sizeof(*snapshot->node_source_ordinals)) : NULL;
    snapshot->zova_node_keys =
        count ? calloc((size_t)count, sizeof(*snapshot->zova_node_keys)) : NULL;
    if (count && (!snapshot->nodes || !stable_ids || !node_keys || !rows || !keys ||
                  !snapshot->node_source_ordinals || !snapshot->zova_node_keys)) {
        free(stable_ids);
        free(node_keys);
        free(rows);
        free(keys);
        return -1;
    }
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT n.zova_node_key,n.name,n.qualified_name,f.file_path,n.start_line,"
        "n.end_line,n.properties,n.source_ordinal FROM cbm_nodes_v1 n "
        "JOIN cbm_files_v1 f ON f.file_key=n.file_key "
        "WHERE n.workspace_key=?1 ORDER BY n.zova_node_key", &statement);
    if (rc == 0) rc = repo_bind_i64(statement, 1, repo->workspace_key);
    for (int i = 0; rc == 0 && i < count; i++) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        snapshot_node_row_t *row = &rows[i];
        CBMDumpNode *node = &row->node;
        row->node_key = keys[i] = repo_column_i64(statement, 0);
        node->project = repo_dup((const uint8_t *)snapshot->project,
                                 strlen(snapshot->project));
        node->name = repo_column_text(statement, 1);
        node->qualified_name = repo_column_text(statement, 2);
        node->file_path = repo_column_text(statement, 3);
        node->start_line = (int)repo_column_i64(statement, 4);
        node->end_line = (int)repo_column_i64(statement, 5);
        node->properties = repo_column_text(statement, 6);
        row->source_ordinal = (uint64_t)repo_column_i64(statement, 7);
        if (!node->project || !node->name || !node->qualified_name || !node->file_path ||
            !node->properties || row->node_key <= 0) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    snapshot->metrics.nodes_sql_ms = repo_elapsed_ms(phase_started);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    zova_graph_keyed_node_results native = {0};
    if (rc == 0 && repo_native_nodes_by_key(repo, keys, (size_t)count, &native) != 0) rc = -1;
    snapshot->metrics.nodes_native_ms = repo_elapsed_ms(phase_started);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    for (int i = 0; rc == 0 && i < count; i++) {
        if (!native.items[i].found || native.items[i].node_key != rows[i].node_key ||
            !native.items[i].node_id || !native.items[i].kind) {
            fprintf(stderr, "zova keyed node mismatch index=%d requested=%lld found=%u returned=%lld\n",
                    i, (long long)rows[i].node_key, native.items[i].found,
                    (long long)native.items[i].node_key);
            rc = -1; break;
        }
        rows[i].stable_id = repo_dup((const uint8_t *)native.items[i].node_id,
                                     native.items[i].node_id_len);
        rows[i].node.label = repo_dup((const uint8_t *)native.items[i].kind,
                                      native.items[i].kind_len);
        if (!rows[i].stable_id || !rows[i].node.label) rc = -1;
    }
    zova_graph_keyed_node_results_free(&native);
    free(keys);
    if (rc != 0) {
        for (int i = 0; i < count; i++) {
            free(rows[i].stable_id);
            free((char *)rows[i].node.project); free((char *)rows[i].node.label);
            free((char *)rows[i].node.name); free((char *)rows[i].node.qualified_name);
            free((char *)rows[i].node.file_path); free((char *)rows[i].node.properties);
        }
        free(stable_ids);
        free(node_keys);
        free(rows);
        return -1;
    }
    if (count > 1) qsort(rows, (size_t)count, sizeof(*rows), snapshot_node_row_compare);
    for (int i = 0; i < count; i++) {
        rows[i].node.id = i + 1;
        snapshot->nodes[i] = rows[i].node;
        stable_ids[i] = rows[i].stable_id;
        snapshot->zova_node_keys[i] = rows[i].node_key;
        snapshot->node_source_ordinals[i] = rows[i].source_ordinal;
        node_keys[i] = (snapshot_node_key_ref_t){.node_key = rows[i].node_key,
                                                 .snapshot_id = i + 1};
    }
    free(rows);
    if (count > 1)
        qsort(node_keys, (size_t)count, sizeof(*node_keys), snapshot_node_key_ref_compare);
    snapshot->node_stable_ids = stable_ids;
    *out_stable_ids = stable_ids;
    *out_node_keys = node_keys;
    snapshot->metrics.nodes_finalize_ms = repo_elapsed_ms(phase_started);
    snapshot->metrics.node_rows = (uint64_t)count;
    snapshot->metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_NODES;
    return 0;
}

typedef struct {
    CBMDumpEdge edge;
    char *edge_id;
} snapshot_edge_sort_row_t;

static int snapshot_edge_sort_row_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_edge_sort_row_t *left = left_ptr;
    const snapshot_edge_sort_row_t *right = right_ptr;
    return strcmp(left->edge_id, right->edge_id);
}

typedef struct {
    cbm_zova_workspace_snapshot_t *snapshot;
    repo_string_arena_t *arena;
    const snapshot_node_key_ref_t *node_keys;
    const zova_graph_scan_edge *native;
    int expected_count;
    int *written;
} snapshot_payload_context_t;

static int snapshot_append_payload_edge(
    const cbm_zova_edge_payload_record_t *record, void *context_ptr) {
    snapshot_payload_context_t *context = context_ptr;
    if (!record || !context || !context->snapshot || !context->written ||
        *context->written >= context->expected_count) return -1;
    int64_t source_id = snapshot_map_node_key(
        context->node_keys, context->snapshot->node_count,
        context->native->source_node_key);
    int64_t target_id = snapshot_map_node_key(
        context->node_keys, context->snapshot->node_count,
        context->native->target_node_key);
    if (source_id <= 0 || target_id <= 0) return -1;
    CBMDumpEdge *edge = &context->snapshot->edges[*context->written];
    *edge = (CBMDumpEdge){
        .id = *context->written + 1,
        .project = context->snapshot->project,
        .source_id = source_id,
        .target_id = target_id,
        .type = repo_string_arena_dup(
            context->arena, (const uint8_t *)context->native->edge_type,
            context->native->edge_type_len),
        .properties = repo_string_arena_dup(
            context->arena, record->properties.data, record->properties.len),
        .url_path = repo_string_arena_dup(
            context->arena, record->url_path.data, record->url_path.len),
        .local_name = repo_string_arena_dup(
            context->arena, record->local_name.data, record->local_name.len),
    };
    if (!edge->type || !edge->properties || !edge->url_path || !edge->local_name)
        return -1;
    (*context->written)++;
    return 0;
}

static int snapshot_read_edges(cbm_zova_repository_t *repo,
                               cbm_zova_workspace_snapshot_t *snapshot,
                               const snapshot_node_key_ref_t *node_keys) {
    const char *failure = NULL;
    struct timespec phase_started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    if (snapshot->integrity.metadata_edges > INT_MAX) return -1;
    int count = (int)snapshot->integrity.metadata_edges;
    snapshot->edge_count = count;
    snapshot->edges = count ? calloc((size_t)count, sizeof(*snapshot->edges)) : NULL;
    snapshot->edge_ids = count ? calloc((size_t)count, sizeof(*snapshot->edge_ids)) : NULL;
    snapshot->edge_string_arena = count ? calloc(1, sizeof(repo_string_arena_t)) : NULL;
    if (count && (!snapshot->edges || !snapshot->edge_ids ||
                  !snapshot->edge_string_arena)) return -1;
    repo_string_arena_t *arena = snapshot->edge_string_arena;
    snapshot->metrics.edges_sql_ms = repo_elapsed_ms(phase_started);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    int rc = 0;
    char graph_name[REPO_GRAPH_NAME_CAP];
    if (rc == 0 && repo_graph_name(repo, graph_name, sizeof(graph_name)) != 0) {
        failure = "graph_name";
        rc = -1;
    }
    zova_graph_scan_cursor cursor = {0};
    int written = 0;
    while (rc == 0) {
        zova_graph_scan_results page = {0};
        zova_status status = zova_graph_scan(&(zova_graph_scan_request){
            .db = repo->db, .graph_name = graph_name, .edge_after = cursor,
            .node_limit = 0, .edge_limit = REPO_EDGE_SCAN_PAGE, .out_results = &page});
        if (status != ZOVA_OK) {
            fprintf(stderr, "zova snapshot graph scan failed status=%s\n",
                    zova_status_name(status));
            failure = "graph_scan";
            zova_graph_scan_results_free(&page);
            rc = -1;
            break;
        }
        if (page.edges_len) snapshot->metrics.edge_scan_pages++;
        snapshot->metrics.edge_native_rows += page.edges_len;
        int64_t *edge_keys = page.edges_len
            ? calloc(page.edges_len, sizeof(*edge_keys)) : NULL;
        zova_graph_edge_payload_results payloads = {0};
        if (page.edges_len && !edge_keys) rc = -1;
        for (size_t i = 0; rc == 0 && i < page.edges_len; i++)
            edge_keys[i] = page.edges[i].edge_key;
        if (rc == 0 && page.edges_len &&
            zova_graph_edge_payload_get_many(
                &(zova_graph_edge_payload_get_many_request){
                    .db = repo->db,
                    .graph_name = graph_name,
                    .edge_keys = edge_keys,
                    .key_count = page.edges_len,
                    .out_results = &payloads,
                }) != ZOVA_OK) rc = -1;
        if (rc == 0 && payloads.len != page.edges_len) rc = -1;
        snapshot->metrics.edge_keyed_read_count += payloads.len;
        for (size_t i = 0; rc == 0 && i < page.edges_len; i++) {
            if (!payloads.items[i].found ||
                payloads.items[i].edge_key != page.edges[i].edge_key) {
                failure = "payload_identity";
                rc = -1;
                break;
            }
            snapshot_payload_context_t context = {
                .snapshot = snapshot,
                .arena = arena,
                .node_keys = node_keys,
                .native = &page.edges[i],
                .expected_count = count,
                .written = &written,
            };
            if (cbm_zova_edge_payload_visit(
                    payloads.items[i].payload, payloads.items[i].payload_len,
                    snapshot_append_payload_edge, &context, NULL) != 0) {
                failure = "payload_decode";
                rc = -1;
            }
        }
        zova_graph_edge_payload_results_free(&payloads);
        free(edge_keys);
        if (page.edges_len) {
            const zova_graph_scan_edge *last = &page.edges[page.edges_len - 1];
            cursor = (zova_graph_scan_cursor){
                .created_order = last->created_order,
                .key = last->edge_key,
            };
        }
        uint8_t more = page.has_more_edges;
        zova_graph_scan_results_free(&page);
        if (!more) break;
    }
    snapshot->metrics.edge_logical_rows = (uint64_t)written;
    if (rc == 0 && written != count) rc = -1;
    if (rc != 0 && failure)
        fprintf(stderr, "zova snapshot edge read failed stage=%s\n", failure);
    snapshot->metrics.edges_native_ms = repo_elapsed_ms(phase_started);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    snapshot_edge_sort_row_t *rows = count
        ? calloc((size_t)count, sizeof(*rows)) : NULL;
    if (rc == 0 && count && !rows) rc = -1;
    for (int i = 0; rc == 0 && i < count; i++) {
        char edge_id[CBM_ZOVA_WORKSPACE_ID_MAX + 80];
        CBMDumpEdge *edge = &snapshot->edges[i];
        if (repo_edge_id(snapshot->workspace_id,
                         snapshot->node_stable_ids[edge->source_id - 1], edge->type,
                         snapshot->node_stable_ids[edge->target_id - 1],
                         edge->local_name, edge_id, sizeof(edge_id)) != 0 ||
            !(rows[i].edge_id = repo_dup((const uint8_t *)edge_id, strlen(edge_id)))) {
            rc = -1;
            break;
        }
        rows[i].edge = *edge;
    }
    if (rc == 0 && count > 1)
        qsort(rows, (size_t)count, sizeof(*rows), snapshot_edge_sort_row_compare);
    for (int i = 0; rc == 0 && i < count; i++) {
        snapshot->edges[i] = rows[i].edge;
        snapshot->edges[i].id = i + 1;
        snapshot->edge_ids[i] = rows[i].edge_id;
        rows[i].edge_id = NULL;
    }
    for (int i = 0; rows && i < count; i++) free(rows[i].edge_id);
    free(rows);
    snapshot->metrics.edges_finalize_ms = repo_elapsed_ms(phase_started);
    snapshot->metrics.edge_rows = (uint64_t)count;
    if (arena) {
        snapshot->metrics.edge_string_arena_chunks = arena->chunk_count;
        snapshot->metrics.edge_string_arena_bytes = arena->bytes_used;
    }
    snapshot->metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_EDGES;
    return rc;
}

int cbm_zova_workspace_snapshot_format_edge_id(
    const cbm_zova_workspace_snapshot_t *snapshot, int edge_index,
    char *out, size_t out_size) {
    if (!snapshot || edge_index < 0 || edge_index >= snapshot->edge_count ||
        !out || out_size == 0) return -1;
    if (snapshot->edge_ids && snapshot->edge_ids[edge_index]) {
        int written = snprintf(out, out_size, "%s", snapshot->edge_ids[edge_index]);
        return written >= 0 && (size_t)written < out_size ? 0 : -1;
    }
    const CBMDumpEdge *edge = &snapshot->edges[edge_index];
    if (edge->source_id <= 0 || edge->source_id > snapshot->node_count ||
        edge->target_id <= 0 || edge->target_id > snapshot->node_count || !edge->type)
        return -1;
    return repo_edge_id(snapshot->workspace_id,
                        snapshot->node_stable_ids[edge->source_id - 1], edge->type,
                        snapshot->node_stable_ids[edge->target_id - 1],
                        edge->local_name ? edge->local_name : "", out, out_size);
}

typedef struct {
    const char *source_stable_id;
    const char *edge_type;
    const char *target_stable_id;
} snapshot_topology_ref_t;

static int snapshot_topology_ref_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_topology_ref_t *left = left_ptr;
    const snapshot_topology_ref_t *right = right_ptr;
    int order = strcmp(left->source_stable_id, right->source_stable_id);
    if (order == 0) order = strcmp(left->edge_type, right->edge_type);
    if (order == 0) order = strcmp(left->target_stable_id, right->target_stable_id);
    return order;
}

static int snapshot_build_topology(const cbm_zova_workspace_snapshot_t *snapshot,
                                   cbm_zova_workspace_snapshot_t *out) {
    if (!snapshot || !out || snapshot->edge_count < 0) return -1;
    snapshot_topology_ref_t *refs = snapshot->edge_count
        ? calloc((size_t)snapshot->edge_count, sizeof(*refs)) : NULL;
    if (snapshot->edge_count && !refs) return -1;
    for (int i = 0; i < snapshot->edge_count; i++) {
        const CBMDumpEdge *edge = &snapshot->edges[i];
        if (edge->source_id <= 0 || edge->source_id > snapshot->node_count ||
            edge->target_id <= 0 || edge->target_id > snapshot->node_count || !edge->type) {
            free(refs);
            return -1;
        }
        refs[i] = (snapshot_topology_ref_t){
            .source_stable_id = snapshot->node_stable_ids[edge->source_id - 1],
            .edge_type = edge->type,
            .target_stable_id = snapshot->node_stable_ids[edge->target_id - 1],
        };
        if (!refs[i].source_stable_id || !refs[i].target_stable_id) {
            free(refs);
            return -1;
        }
    }
    if (snapshot->edge_count > 1)
        qsort(refs, (size_t)snapshot->edge_count, sizeof(*refs), snapshot_topology_ref_compare);
    int unique_count = 0;
    for (int i = 0; i < snapshot->edge_count; i++) {
        if (i == 0 || snapshot_topology_ref_compare(&refs[i - 1], &refs[i]) != 0)
            unique_count++;
    }
    if ((uint64_t)unique_count != snapshot->integrity.metadata_topology_edges) {
        free(refs);
        return -1;
    }
    out->topology_edges = unique_count
        ? calloc((size_t)unique_count, sizeof(*out->topology_edges)) : NULL;
    if (unique_count && !out->topology_edges) {
        free(refs);
        return -1;
    }
    out->topology_edge_count = unique_count;
    int written = 0;
    for (int i = 0; i < snapshot->edge_count; i++) {
        if (i > 0 && snapshot_topology_ref_compare(&refs[i - 1], &refs[i]) == 0) continue;
        cbm_zova_snapshot_topology_edge_t *row = &out->topology_edges[written++];
        row->source_stable_id = repo_dup((const uint8_t *)refs[i].source_stable_id,
                                         strlen(refs[i].source_stable_id));
        row->edge_type = repo_dup((const uint8_t *)refs[i].edge_type,
                                  strlen(refs[i].edge_type));
        row->target_stable_id = repo_dup((const uint8_t *)refs[i].target_stable_id,
                                         strlen(refs[i].target_stable_id));
        if (!row->source_stable_id || !row->edge_type || !row->target_stable_id) {
            free(refs);
            return -1;
        }
    }
    free(refs);
    return 0;
}

static int snapshot_read_node_vectors(cbm_zova_repository_t *repo,
                                      const cbm_zova_workspace_snapshot_t *snapshot,
                                      cbm_zova_workspace_snapshot_t *out) {
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    if (cbm_zova_workspace_node_vector_collection_name(
            repo->workspace_id, snapshot->model_fingerprint, snapshot->vector_dimensions,
            node_collection, sizeof(node_collection)) != 0)
        return -1;
    zova_vector_collection_info node_info = {0};
    zova_status node_status = zova_vector_collection_info_get(
        &(zova_vector_collection_info_get_request){
            .db = repo->db, .name = node_collection, .out_info = &node_info});
    int rc = node_status == ZOVA_OK &&
                     node_info.element_type == ZOVA_VECTOR_ELEMENT_TYPE_I8 &&
                     node_info.dimensions == (uint32_t)snapshot->vector_dimensions &&
                     node_info.vector_count <= INT32_MAX &&
                     node_info.vector_count == snapshot->integrity.node_vectors
                 ? 0
                 : -1;
    int node_count = rc == 0 ? (int)node_info.vector_count : 0;
    if (node_status == ZOVA_OK) zova_vector_collection_info_free(&node_info);
    out->node_vectors = node_count ? calloc((size_t)node_count, sizeof(*out->node_vectors)) : NULL;
    out->node_vector_ids = node_count ? calloc((size_t)node_count, sizeof(*out->node_vector_ids)) : NULL;
    out->node_vector_count = node_count;
    if (rc == 0 && node_count && (!out->node_vectors || !out->node_vector_ids))
        rc = -1;
    int node_written = 0;
    for (int i = 0; rc == 0 && i < snapshot->node_count; i++) {
        char vector_id[32];
        if (!snapshot->zova_node_keys ||
            snprintf(vector_id, sizeof(vector_id), "%lld",
                     (long long)snapshot->zova_node_keys[i]) >= (int)sizeof(vector_id)) {
            rc = -1;
            break;
        }
        uint8_t exists = 0;
        if (zova_vector_exists(&(zova_vector_exists_request){
                .db = repo->db, .collection_name = node_collection,
                .vector_id = vector_id, .out_exists = &exists}) != ZOVA_OK) {
            rc = -1;
            break;
        }
        if (!exists) continue;
        if (node_written >= node_count) { rc = -1; break; }
        zova_vector native = {0};
        if (zova_vector_get(&(zova_vector_get_request){
                .db = repo->db, .collection_name = node_collection,
                .vector_id = vector_id, .out_vector = &native}) != ZOVA_OK ||
            native.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
            native.values_len != (size_t)snapshot->vector_dimensions) {
            zova_vector_free(&native);
            rc = -1;
            break;
        }
        CBMDumpVector *vector = &out->node_vectors[node_written++];
        out->node_vector_ids[node_written - 1] =
            repo_dup((const uint8_t *)snapshot->node_stable_ids[i],
                     strlen(snapshot->node_stable_ids[i]));
        vector->node_id = snapshot->nodes[i].id;
        vector->project = repo_dup((const uint8_t *)snapshot->project, strlen(snapshot->project));
        uint8_t *bytes = malloc(native.values_len);
        if (!out->node_vector_ids[node_written - 1] || !vector->project || !bytes) rc = -1;
        else {
            memcpy(bytes, native.i8_values, native.values_len);
            vector->vector = bytes;
            vector->vector_len = (int)native.values_len;
        }
        zova_vector_free(&native);
    }
    if (rc == 0 && node_written != node_count) rc = -1;
    return rc;
}

typedef struct {
    int64_t token_key;
    char *public_id;
    char *token;
    double idf;
} snapshot_token_row_t;

static int snapshot_token_row_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_token_row_t *left = left_ptr;
    const snapshot_token_row_t *right = right_ptr;
    return strcmp(left->public_id, right->public_id);
}

static int snapshot_read_token_vectors(cbm_zova_repository_t *repo,
                                       const cbm_zova_workspace_snapshot_t *snapshot,
                                       cbm_zova_workspace_snapshot_t *out) {
    char token_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    if (cbm_zova_workspace_token_vector_collection_name(
            repo->workspace_id, snapshot->model_fingerprint, snapshot->vector_dimensions,
            token_collection, sizeof(token_collection)) != 0)
        return -1;
    zova_vector_collection_info token_info = {0};
    zova_status token_status = zova_vector_collection_info_get(
        &(zova_vector_collection_info_get_request){
            .db = repo->db, .name = token_collection, .out_info = &token_info});
    int rc = token_status == ZOVA_OK &&
                     token_info.element_type == ZOVA_VECTOR_ELEMENT_TYPE_I8 &&
                     token_info.dimensions == (uint32_t)snapshot->vector_dimensions &&
                     token_info.vector_count <= INT32_MAX &&
                     token_info.vector_count == snapshot->integrity.token_vectors
                 ? 0
                 : -1;
    int token_count = rc == 0 ? (int)token_info.vector_count : 0;
    if (token_status == ZOVA_OK) zova_vector_collection_info_free(&token_info);
    out->token_vectors = token_count ? calloc((size_t)token_count, sizeof(*out->token_vectors)) : NULL;
    out->token_vector_ids = token_count ? calloc((size_t)token_count, sizeof(*out->token_vector_ids)) : NULL;
    out->token_vector_count = token_count;
    if (rc == 0 && token_count && (!out->token_vectors || !out->token_vector_ids)) rc = -1;
    snapshot_token_row_t *rows = token_count
        ? calloc((size_t)token_count, sizeof(*rows)) : NULL;
    if (rc == 0 && token_count && !rows) rc = -1;
    zova_statement *statement = NULL;
    if (rc == 0) rc = repo_prepare(repo,
        "SELECT token_key,token,idf FROM cbm_token_vector_metadata_v1 "
        "WHERE workspace_key=?1 ORDER BY token_key", &statement);
    if (rc == 0) rc = repo_bind_i64(statement, 1, repo->workspace_key);
    for (int i = 0; rc == 0 && i < token_count; i++) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        rows[i].token_key = repo_column_i64(statement, 0);
        rows[i].token = repo_column_text(statement, 1);
        rows[i].idf = repo_column_double(statement, 2);
        char public_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
        if (rows[i].token_key <= 0 || !rows[i].token ||
            cbm_zova_workspace_token_id_v1(repo->workspace_id, rows[i].token,
                                           public_id, sizeof(public_id)) != 0) {
            rc = -1;
            break;
        }
        rows[i].public_id = repo_dup((const uint8_t *)public_id, strlen(public_id));
        if (!rows[i].public_id) rc = -1;
    }
    if (statement) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_DONE)) rc = -1;
        (void)zova_statement_finalize(statement);
        statement = NULL;
    }
    if (rc == 0 && token_count > 1)
        qsort(rows, (size_t)token_count, sizeof(*rows), snapshot_token_row_compare);
    int token_written = 0;
    for (; rc == 0 && token_written < token_count; token_written++) {
        char vector_id[32];
        if (snprintf(vector_id, sizeof(vector_id), "%lld",
                     (long long)rows[token_written].token_key) >= (int)sizeof(vector_id)) {
            rc = -1;
            break;
        }
        zova_vector native = {0};
        if (zova_vector_get(&(zova_vector_get_request){
                .db = repo->db, .collection_name = token_collection,
                .vector_id = vector_id, .out_vector = &native}) != ZOVA_OK ||
            native.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
            native.values_len != (size_t)snapshot->vector_dimensions) {
            zova_vector_free(&native); rc = -1; break;
        }
        CBMDumpTokenVec *vector = &out->token_vectors[token_written];
        out->token_vector_ids[token_written] = rows[token_written].public_id;
        rows[token_written].public_id = NULL;
        vector->id = token_written + 1;
        vector->project = repo_dup((const uint8_t *)snapshot->project, strlen(snapshot->project));
        vector->token = rows[token_written].token;
        rows[token_written].token = NULL;
        vector->idf = (float)rows[token_written].idf;
        uint8_t *bytes = malloc(native.values_len);
        if (vector->id <= 0 || !vector->project || !bytes) rc = -1;
        else {
            memcpy(bytes, native.i8_values, native.values_len);
            vector->vector = bytes;
            vector->vector_len = (int)native.values_len;
        }
        zova_vector_free(&native);
    }
    for (int i = 0; i < token_count; i++) {
        free(rows ? rows[i].public_id : NULL);
        free(rows ? rows[i].token : NULL);
    }
    free(rows);
    if (rc == 0 && token_written != token_count) rc = -1;
    return rc;
}

static int snapshot_read_hashes_summary(cbm_zova_repository_t *repo,
                                        cbm_zova_workspace_snapshot_t *snapshot) {
    struct timespec phase_started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    int count = snapshot_count(repo,
        "SELECT count(*) FROM cbm_file_hashes_v1 h JOIN cbm_files_v1 f "
        "ON f.file_key=h.file_key WHERE f.workspace_key=?1");
    if (count < 0) return -1;
    snapshot->file_hash_count = count;
    snapshot->file_hashes = count ? calloc((size_t)count, sizeof(*snapshot->file_hashes)) : NULL;
    if (count && !snapshot->file_hashes) return -1;
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT f.file_path,h.content_hash,h.mtime_ns,h.size_bytes FROM cbm_file_hashes_v1 h "
        "JOIN cbm_files_v1 f ON f.file_key=h.file_key "
        "WHERE f.workspace_key=?1 ORDER BY f.file_path", &statement);
    if (rc == 0) rc = repo_bind_i64(statement, 1, repo->workspace_key);
    for (int i = 0; rc == 0 && i < count; i++) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        snapshot->file_hashes[i].file_path = repo_column_text(statement, 0);
        snapshot->file_hashes[i].content_hash = repo_column_text(statement, 1);
        snapshot->file_hashes[i].mtime_ns = repo_column_i64(statement, 2);
        snapshot->file_hashes[i].size_bytes = repo_column_i64(statement, 3);
        if (!snapshot->file_hashes[i].file_path || !snapshot->file_hashes[i].content_hash) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    statement = NULL;
    if (rc == 0) rc = repo_prepare(repo,
        "SELECT summary,source_hash,created_at,updated_at FROM cbm_project_summaries_v2 "
        "WHERE workspace_key=?1", &statement);
    if (rc == 0) rc = repo_bind_i64(statement, 1, repo->workspace_key);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && repo_step(statement, &step) != 0) rc = -1;
    if (rc == 0 && step == ZOVA_STEP_ROW) {
        snapshot->project_summary.present = true;
        snapshot->project_summary.summary = repo_column_text(statement, 0);
        snapshot->project_summary.source_hash = repo_column_text(statement, 1);
        snapshot->project_summary.created_at = repo_column_text(statement, 2);
        snapshot->project_summary.updated_at = repo_column_text(statement, 3);
        if (!snapshot->project_summary.summary || !snapshot->project_summary.source_hash ||
            !snapshot->project_summary.created_at || !snapshot->project_summary.updated_at) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    snapshot->metrics.hashes_summary_ms = repo_elapsed_ms(phase_started);
    snapshot->metrics.file_hash_rows = (uint64_t)count;
    snapshot->metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_HASHES_SUMMARY;
    return rc;
}

static int snapshot_read_header(cbm_zova_repository_t *repo,
                                cbm_zova_workspace_snapshot_t *snapshot) {
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT r.workspace_key,r.canonical_root,p.root_path,p.project,p.indexed_at,"
        "s.model_fingerprint,s.vector_dimensions,r.active_generation "
        "FROM cbm_workspace_registry r "
        "JOIN cbm_projects_v1 p ON p.workspace_key=r.workspace_key "
        "JOIN cbm_workspace_index_state_v1 s ON s.workspace_key=r.workspace_key "
        "JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key "
        "AND g.generation=r.active_generation AND g.state='ready' "
        "WHERE r.workspace_id=?1 AND s.generation=r.active_generation", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW)) rc = -1;
    if (rc == 0) {
        repo->workspace_key = repo_column_i64(statement, 0);
        snapshot->root_path = repo_column_text(statement, 1);
        char *project_root = repo_column_text(statement, 2);
        snapshot->project = repo_column_text(statement, 3);
        snapshot->indexed_at = repo_column_text(statement, 4);
        snapshot->model_fingerprint = repo_column_text(statement, 5);
        snapshot->vector_dimensions = (int)repo_column_i64(statement, 6);
        snapshot->generation = repo_column_i64(statement, 7);
        if (!snapshot->root_path || !snapshot->project || !snapshot->indexed_at ||
            !snapshot->model_fingerprint || snapshot->vector_dimensions <= 0 ||
            snapshot->generation <= 0) rc = -1;
        char expected[CBM_ZOVA_WORKSPACE_ID_MAX];
        if (rc == 0 &&
            (!project_root || strcmp(project_root, snapshot->root_path) != 0 ||
             cbm_zova_workspace_id_for_root(snapshot->root_path, expected, sizeof(expected)) != 0 ||
             strcmp(expected, repo->workspace_id) != 0)) rc = -1;
        free(project_root);
    }
    if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_DONE)) rc = -1;
    if (statement) (void)zova_statement_finalize(statement);
    return rc;
}

static int snapshot_copy_digest(zova_statement *statement, int column,
                                char out[CBM_ZOVA_DIGEST_HEX_SIZE]) {
    char *digest = repo_column_text(statement, column);
    if (!digest || strlen(digest) != CBM_ZOVA_DIGEST_HEX_SIZE - 1) {
        free(digest);
        return -1;
    }
    memcpy(out, digest, CBM_ZOVA_DIGEST_HEX_SIZE);
    free(digest);
    return 0;
}

static int snapshot_read_integrity(cbm_zova_repository_t *repo,
                                   cbm_zova_workspace_snapshot_t *snapshot) {
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT graph_nodes,graph_edges,metadata_nodes,metadata_edges,"
        "metadata_topology_edges,fts_rows,node_vector_rows,token_vector_rows,"
        "node_vectors,token_vectors,metadata_sha256,fts_sha256,topology_sha256,"
        "node_vector_sha256,token_vector_sha256 FROM cbm_generation_integrity_v2 "
        "WHERE workspace_key=?1 AND generation=?2", &statement);
    if (rc == 0) rc = repo_bind_i64(statement, 1, repo->workspace_key);
    if (rc == 0) rc = repo_bind_i64(statement, 2, snapshot->generation);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW)) rc = -1;
    cbm_zova_workspace_generation_result_t *integrity = &snapshot->integrity;
    if (rc == 0) {
        snprintf(integrity->workspace_id, sizeof(integrity->workspace_id), "%s",
                 repo->workspace_id);
        integrity->generation = snapshot->generation;
        integrity->graph_nodes = (uint64_t)repo_column_i64(statement, 0);
        integrity->graph_edges = (uint64_t)repo_column_i64(statement, 1);
        integrity->metadata_nodes = (uint64_t)repo_column_i64(statement, 2);
        integrity->metadata_edges = (uint64_t)repo_column_i64(statement, 3);
        integrity->metadata_topology_edges = (uint64_t)repo_column_i64(statement, 4);
        integrity->fts_rows = (uint64_t)repo_column_i64(statement, 5);
        integrity->node_vector_rows = (uint64_t)repo_column_i64(statement, 6);
        integrity->token_vector_rows = (uint64_t)repo_column_i64(statement, 7);
        integrity->node_vectors = (uint64_t)repo_column_i64(statement, 8);
        integrity->token_vectors = (uint64_t)repo_column_i64(statement, 9);
        if (snapshot_copy_digest(statement, 10, integrity->metadata_sha256) != 0 ||
            snapshot_copy_digest(statement, 11, integrity->fts_sha256) != 0 ||
            snapshot_copy_digest(statement, 12, integrity->topology_sha256) != 0 ||
            snapshot_copy_digest(statement, 13, integrity->node_vector_sha256) != 0 ||
            snapshot_copy_digest(statement, 14, integrity->token_vector_sha256) != 0) rc = -1;
    }
    if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_DONE)) rc = -1;
    if (statement) (void)zova_statement_finalize(statement);
    return rc;
}

static int snapshot_verify_native(cbm_zova_repository_t *repo,
                                  const cbm_zova_workspace_snapshot_t *snapshot) {
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    char token_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    if (cbm_zova_workspace_graph_name(repo->workspace_id, graph_name, sizeof(graph_name)) != 0 ||
        cbm_zova_workspace_node_vector_collection_name(
            repo->workspace_id, snapshot->model_fingerprint, snapshot->vector_dimensions,
            node_collection, sizeof(node_collection)) != 0 ||
        cbm_zova_workspace_token_vector_collection_name(
            repo->workspace_id, snapshot->model_fingerprint, snapshot->vector_dimensions,
            token_collection, sizeof(token_collection)) != 0) return -1;
    zova_graph_info graph = {0};
    zova_vector_collection_info nodes = {0};
    zova_vector_collection_info tokens = {0};
    int rc = zova_graph_info_get(&(zova_graph_info_get_request){
                 .db = repo->db, .name = graph_name, .out_info = &graph}) == ZOVA_OK &&
                     zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
                         .db = repo->db, .name = node_collection, .out_info = &nodes}) == ZOVA_OK &&
                     zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
                         .db = repo->db, .name = token_collection, .out_info = &tokens}) == ZOVA_OK &&
                     graph.node_count == snapshot->integrity.graph_nodes &&
                     graph.edge_count == snapshot->integrity.graph_edges &&
                     nodes.vector_count == snapshot->integrity.node_vectors &&
                     tokens.vector_count == snapshot->integrity.token_vectors
                 ? 0 : -1;
    zova_graph_info_free(&graph);
    zova_vector_collection_info_free(&nodes);
    zova_vector_collection_info_free(&tokens);
    return rc;
}

static int repository_export_snapshot(const char *path, const char *workspace_id,
                                      cbm_zova_workspace_snapshot_t *out,
                                      cbm_zova_snapshot_components_t components) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0] || cbm_zova_workspace_id_validate(workspace_id) != 0) return -1;
    cbm_zova_workspace_snapshot_t snapshot = {0};
    cbm_zova_repository_t repo = {0};
    snprintf(repo.workspace_id, sizeof(repo.workspace_id), "%s", workspace_id);
    snprintf(snapshot.workspace_id, sizeof(snapshot.workspace_id), "%s", workspace_id);
    zova_message error = {0};
    struct timespec base_started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &base_started);
    int rc = zova_database_open_with_options(&(zova_database_open_options_request){
                 .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                 .out_db = &repo.db, .out_error_message = &error}) == ZOVA_OK && repo.db
                 ? 0 : -1;
    zova_message_free(&error);
    snapshot.metrics.open_ms = repo_elapsed_ms(base_started);
    if (rc == 0) snapshot.metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_OPEN;
    bool transaction_started = false;
    const char *phase = "open";
    if (rc == 0 && (cbm_zova_register_sql_functions(repo.db) != 0 ||
                    zova_database_exec(&(zova_database_exec_request){
                        .db = repo.db, .sql = "BEGIN"}) != ZOVA_OK)) rc = -1;
    else if (rc == 0) transaction_started = true;
    char **stable_ids = NULL;
    snapshot_node_key_ref_t *node_keys = NULL;
    struct timespec phase_started = {0};
    if (rc == 0) {
        phase = "header";
        cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
        rc = snapshot_read_header(&repo, &snapshot);
        snapshot.metrics.header_ms = repo_elapsed_ms(phase_started);
        if (rc == 0) snapshot.metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_HEADER;
    }
    if (rc == 0) {
        phase = "integrity";
        cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
        rc = snapshot_read_integrity(&repo, &snapshot);
        snapshot.metrics.integrity_ms = repo_elapsed_ms(phase_started);
        if (rc == 0) snapshot.metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_INTEGRITY;
    }
    if (rc == 0) { phase = "nodes"; rc = snapshot_read_nodes(&repo, &snapshot, &stable_ids, &node_keys); }
    if (rc == 0) { phase = "edges"; rc = snapshot_read_edges(&repo, &snapshot, node_keys); }
    if (rc == 0) { phase = "hashes_summary"; rc = snapshot_read_hashes_summary(&repo, &snapshot); }
    struct timespec optional_started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &optional_started);
    if (rc == 0 && (components & CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY))
        rc = snapshot_build_topology(&snapshot, &snapshot);
    if (rc == 0 && (components & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS))
        rc = snapshot_read_node_vectors(&repo, &snapshot, &snapshot);
    if (rc == 0 && (components & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS))
        rc = snapshot_read_token_vectors(&repo, &snapshot, &snapshot);
    snapshot.metrics.optional_ms = components == CBM_ZOVA_SNAPSHOT_COMPONENT_NONE
        ? 0.0 : repo_elapsed_ms(optional_started);
    if (rc == 0 && (snapshot.node_count != (int)snapshot.integrity.metadata_nodes ||
                    snapshot.edge_count != (int)snapshot.integrity.metadata_edges))
        rc = -1;
    if (rc == 0 && (components & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) &&
        snapshot.node_vector_count != (int)snapshot.integrity.node_vectors)
        rc = -1;
    if (rc == 0 && (components & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) &&
        snapshot.token_vector_count != (int)snapshot.integrity.token_vectors)
        rc = -1;
    if (rc == 0 && components == CBM_ZOVA_SNAPSHOT_COMPONENT_ALL)
        rc = snapshot_verify_native(&repo, &snapshot);
    if (rc == 0) {
        snapshot.hydrated_components = components;
        snapshot.metrics.hydrated_components = snapshot.hydrated_components;
        snapshot.metrics.topology_rows = (uint64_t)snapshot.topology_edge_count;
        snapshot.metrics.node_vector_rows = (uint64_t)snapshot.node_vector_count;
        snapshot.metrics.token_vector_rows = (uint64_t)snapshot.token_vector_count;
    }
    free(node_keys);
    cbm_clock_gettime(CLOCK_MONOTONIC, &phase_started);
    if (transaction_started) {
        zova_status tx_status = zova_database_exec(&(zova_database_exec_request){
            .db = repo.db, .sql = rc == 0 ? "COMMIT" : "ROLLBACK"});
        if (rc == 0 && tx_status != ZOVA_OK) rc = -1;
    }
    if (repo.db) (void)zova_database_close(repo.db);
    snapshot.metrics.close_ms = repo_elapsed_ms(phase_started);
    if (rc == 0) snapshot.metrics.base_phase_mask |= CBM_ZOVA_SNAPSHOT_BASE_PHASE_CLOSE;
    snapshot.metrics.base_ms = repo_elapsed_ms(base_started);
    if (rc != 0) {
        fprintf(stderr, "zova snapshot export failed phase=%s\n", phase);
        cbm_zova_workspace_snapshot_free(&snapshot);
        return -1;
    }
    *out = snapshot;
    return 0;
}

int cbm_zova_repository_export_snapshot(const char *path, const char *workspace_id,
                                        cbm_zova_workspace_snapshot_t *out) {
    return repository_export_snapshot(path, workspace_id, out,
                                      CBM_ZOVA_SNAPSHOT_COMPONENT_ALL);
}

int cbm_zova_repository_export_incremental_snapshot(
    const char *path, const char *workspace_id, cbm_zova_workspace_snapshot_t *out) {
    return repository_export_snapshot(path, workspace_id, out,
                                      CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);
}

typedef struct {
    uint64_t source_ordinal;
    int index;
} snapshot_graph_node_order_t;

static int snapshot_graph_node_order_compare(const void *left_ptr, const void *right_ptr) {
    const snapshot_graph_node_order_t *left = left_ptr;
    const snapshot_graph_node_order_t *right = right_ptr;
    if (left->source_ordinal != right->source_ordinal)
        return left->source_ordinal < right->source_ordinal ? -1 : 1;
    return (left->index > right->index) - (left->index < right->index);
}

static int snapshot_load_graph(const cbm_zova_workspace_snapshot_t *snapshot,
                               cbm_gbuf_t *graph) {
    if (!snapshot || !graph ||
        cbm_gbuf_reserve(graph, snapshot->node_count, snapshot->edge_count) != 0)
        return -1;
    int64_t *ids = snapshot->node_count
                       ? calloc((size_t)snapshot->node_count + 1, sizeof(*ids)) : NULL;
    snapshot_graph_node_order_t *order = snapshot->node_count
                       ? calloc((size_t)snapshot->node_count, sizeof(*order)) : NULL;
    if (snapshot->node_count && (!ids || !order || !snapshot->node_source_ordinals)) {
        free(ids); free(order); return -1;
    }
    for (int i = 0; i < snapshot->node_count; i++)
        order[i] = (snapshot_graph_node_order_t){
            .source_ordinal = snapshot->node_source_ordinals[i], .index = i};
    if (snapshot->node_count > 1)
        qsort(order, (size_t)snapshot->node_count, sizeof(*order),
              snapshot_graph_node_order_compare);
    int rc = 0;
    for (int i = 0; rc == 0 && i < snapshot->node_count; i++) {
        const CBMDumpNode *node = &snapshot->nodes[order[i].index];
        if (node->id <= 0 || node->id > snapshot->node_count || ids[node->id] != 0) {
            rc = -1; break;
        }
        ids[node->id] = cbm_gbuf_upsert_node(
            graph, node->label, node->name, node->qualified_name, node->file_path,
            node->start_line, node->end_line, node->properties);
        if (ids[node->id] <= 0) rc = -1;
    }
    for (int i = 0; rc == 0 && i < snapshot->edge_count; i++) {
        const CBMDumpEdge *edge = &snapshot->edges[i];
        int64_t source = edge->source_id > 0 && edge->source_id <= snapshot->node_count
                             ? ids[edge->source_id] : 0;
        int64_t target = edge->target_id > 0 && edge->target_id <= snapshot->node_count
                             ? ids[edge->target_id] : 0;
        if (source <= 0 || target <= 0 ||
            cbm_gbuf_insert_edge(graph, source, target, edge->type, edge->properties) <= 0)
            rc = -1;
    }
    free(ids);
    free(order);
    return rc;
}

int cbm_zova_repository_export_incremental_snapshot_to_graph(
    const char *path, const char *workspace_id, cbm_zova_workspace_snapshot_t *out,
    cbm_gbuf_t *graph) {
    if (!graph || cbm_zova_repository_export_incremental_snapshot(path, workspace_id, out) != 0)
        return -1;
    struct timespec started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    if (snapshot_load_graph(out, graph) != 0) {
        cbm_zova_workspace_snapshot_free(out);
        return -1;
    }
    out->metrics.graph_buffer_ms = repo_elapsed_ms(started);
    return 0;
}

static int snapshot_integrity_matches(const cbm_zova_workspace_snapshot_t *left,
                                      const cbm_zova_workspace_snapshot_t *right) {
    const cbm_zova_workspace_generation_result_t *a = &left->integrity;
    const cbm_zova_workspace_generation_result_t *b = &right->integrity;
    return left->generation == right->generation &&
           strcmp(left->workspace_id, right->workspace_id) == 0 &&
           a->metadata_nodes == b->metadata_nodes && a->metadata_edges == b->metadata_edges &&
           a->metadata_topology_edges == b->metadata_topology_edges &&
           a->node_vectors == b->node_vectors && a->token_vectors == b->token_vectors &&
           strcmp(a->metadata_sha256, b->metadata_sha256) == 0 &&
           strcmp(a->fts_sha256, b->fts_sha256) == 0 &&
           strcmp(a->topology_sha256, b->topology_sha256) == 0 &&
           strcmp(a->node_vector_sha256, b->node_vector_sha256) == 0 &&
           strcmp(a->token_vector_sha256, b->token_vector_sha256) == 0;
}

int cbm_zova_repository_hydrate_incremental_components(
    const char *path, const char *workspace_id, int64_t expected_generation,
    cbm_zova_snapshot_components_t requested, cbm_zova_workspace_snapshot_t *snapshot) {
    if (!path || !path[0] || !snapshot ||
        cbm_zova_workspace_id_validate(workspace_id) != 0 ||
        strcmp(workspace_id, snapshot->workspace_id) != 0 ||
        requested == CBM_ZOVA_SNAPSHOT_COMPONENT_NONE ||
        (requested & ~CBM_ZOVA_SNAPSHOT_COMPONENT_ALL) != 0)
        return CBM_ZOVA_SNAPSHOT_ERROR;
    if (expected_generation <= 0 || expected_generation != snapshot->generation)
        return CBM_ZOVA_SNAPSHOT_STALE;
    cbm_zova_snapshot_components_t pending = requested & ~snapshot->hydrated_components;
    if (pending == CBM_ZOVA_SNAPSHOT_COMPONENT_NONE) return CBM_ZOVA_SNAPSHOT_OK;

    struct timespec started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_workspace_snapshot_t hydrated = {0};
    int rc = 0;
    if (pending & CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY)
        rc = snapshot_build_topology(snapshot, &hydrated);

    cbm_zova_repository_t repo = {0};
    cbm_zova_workspace_snapshot_t current = {0};
    bool transaction_started = false;
    bool needs_database = (pending & (CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
                                      CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS)) != 0;
    if (rc == 0 && needs_database) {
        snprintf(repo.workspace_id, sizeof(repo.workspace_id), "%s", workspace_id);
        snprintf(current.workspace_id, sizeof(current.workspace_id), "%s", workspace_id);
        zova_message error = {0};
        rc = zova_database_open_with_options(&(zova_database_open_options_request){
                 .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
                 .out_db = &repo.db, .out_error_message = &error}) == ZOVA_OK && repo.db
                 ? 0 : -1;
        zova_message_free(&error);
        if (rc == 0 && (cbm_zova_register_sql_functions(repo.db) != 0 ||
                        zova_database_exec(&(zova_database_exec_request){
                            .db = repo.db, .sql = "PRAGMA query_only=ON;BEGIN"}) != ZOVA_OK))
            rc = -1;
        else if (rc == 0) transaction_started = true;
        if (rc == 0) rc = snapshot_read_header(&repo, &current);
        if (rc == 0) rc = snapshot_read_integrity(&repo, &current);
        if (rc == 0 && !snapshot_integrity_matches(snapshot, &current)) rc = -2;
        if (rc == 0 && (pending & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS))
            rc = snapshot_read_node_vectors(&repo, snapshot, &hydrated);
        if (rc == 0 && (pending & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS))
            rc = snapshot_read_token_vectors(&repo, snapshot, &hydrated);
        if (transaction_started) {
            zova_status tx_status = zova_database_exec(&(zova_database_exec_request){
                .db = repo.db, .sql = rc == 0 ? "COMMIT" : "ROLLBACK"});
            if (rc == 0 && tx_status != ZOVA_OK) rc = -1;
        }
        if (repo.db) (void)zova_database_close(repo.db);
    }
    cbm_zova_workspace_snapshot_free(&current);
    if (rc != 0) {
        cbm_zova_workspace_snapshot_free(&hydrated);
        return rc == -2 ? CBM_ZOVA_SNAPSHOT_STALE : CBM_ZOVA_SNAPSHOT_ERROR;
    }

    if (pending & CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY) {
        snapshot->topology_edges = hydrated.topology_edges;
        snapshot->topology_edge_count = hydrated.topology_edge_count;
        hydrated.topology_edges = NULL;
        hydrated.topology_edge_count = 0;
    }
    if (pending & CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS) {
        snapshot->node_vectors = hydrated.node_vectors;
        snapshot->node_vector_ids = hydrated.node_vector_ids;
        snapshot->node_vector_count = hydrated.node_vector_count;
        hydrated.node_vectors = NULL;
        hydrated.node_vector_ids = NULL;
        hydrated.node_vector_count = 0;
    }
    if (pending & CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS) {
        snapshot->token_vectors = hydrated.token_vectors;
        snapshot->token_vector_ids = hydrated.token_vector_ids;
        snapshot->token_vector_count = hydrated.token_vector_count;
        hydrated.token_vectors = NULL;
        hydrated.token_vector_ids = NULL;
        hydrated.token_vector_count = 0;
    }
    cbm_zova_workspace_snapshot_free(&hydrated);
    snapshot->hydrated_components |= pending;
    snapshot->metrics.hydrated_components = snapshot->hydrated_components;
    snapshot->metrics.topology_rows = (uint64_t)snapshot->topology_edge_count;
    snapshot->metrics.node_vector_rows = (uint64_t)snapshot->node_vector_count;
    snapshot->metrics.token_vector_rows = (uint64_t)snapshot->token_vector_count;
    snapshot->metrics.optional_ms += repo_elapsed_ms(started);
    return CBM_ZOVA_SNAPSHOT_OK;
}

#else
struct cbm_zova_repository { int unused; };
struct cbm_zova_catalog { int unused; };
cbm_zova_repository_t *cbm_zova_repository_open(const char *p,const char *n){(void)p;(void)n;return NULL;}
void cbm_zova_repository_close(cbm_zova_repository_t *r){(void)r;}
const char *cbm_zova_repository_workspace_id(const cbm_zova_repository_t *r){(void)r;return "";}
int cbm_zova_repository_get_project(cbm_zova_repository_t*r,const char*w,cbm_project_t*o){(void)r;(void)w;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_node_by_qn(cbm_zova_repository_t*r,const char*w,const char*q,cbm_node_t*o){(void)r;(void)w;(void)q;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_node_by_stable_id(cbm_zova_repository_t*r,const char*w,const char*s,cbm_node_t*o){(void)r;(void)w;(void)s;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_nodes_by_stable_ids(cbm_zova_repository_t*r,const char*w,const char*const*s,int c,cbm_node_t**o){(void)r;(void)w;(void)s;(void)c;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_nodes_by_keys(cbm_zova_repository_t*r,const char*w,const int64_t*k,int c,cbm_node_t**o){(void)r;(void)w;(void)k;(void)c;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_node_by_numeric_id(cbm_zova_repository_t*r,const char*w,int64_t n,cbm_node_t*o){(void)r;(void)w;(void)n;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_nodes_by_file_overlap(cbm_zova_repository_t*r,const char*w,const char*f,int s,int e,cbm_node_t**o,int*c){(void)r;(void)w;(void)f;(void)s;(void)e;(void)o;(void)c;return CBM_STORE_ERR;}
int cbm_zova_repository_stable_id_for_numeric_id(cbm_zova_repository_t*r,const char*w,int64_t n,char*o,size_t z){(void)r;(void)w;(void)n;if(o&&z)o[0]='\0';return CBM_STORE_ERR;}
int cbm_zova_repository_find_edges(cbm_zova_repository_t*r,const char*w,const char*s,const char*d,cbm_edge_t**o,int*c){(void)r;(void)w;(void)s;(void)d;(void)o;(void)c;return CBM_STORE_ERR;}
int cbm_zova_repository_search(cbm_zova_repository_t*r,const char*w,const cbm_search_params_t*p,cbm_search_output_t*o){(void)r;(void)w;(void)p;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_search_fts(cbm_zova_repository_t*r,const char*w,const char*q,const char*f,int l,int x,cbm_search_output_t*o){(void)r;(void)w;(void)q;(void)f;(void)l;(void)x;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_index_status(cbm_zova_repository_t*r,const char*w,int64_t*g,char**i){(void)r;(void)w;(void)g;(void)i;return CBM_STORE_ERR;}
int cbm_zova_repository_counts(cbm_zova_repository_t*r,const char*w,int*n,int*e){(void)r;(void)w;(void)n;(void)e;return CBM_STORE_ERR;}
int cbm_zova_repository_project_summary(cbm_zova_repository_t*r,const char*w,cbm_project_summary_export_t*o){(void)r;(void)w;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_export_snapshot(const char*p,const char*w,cbm_zova_workspace_snapshot_t*o){(void)p;(void)w;if(o)memset(o,0,sizeof(*o));return -1;}
int cbm_zova_repository_export_incremental_snapshot(const char*p,const char*w,cbm_zova_workspace_snapshot_t*o){(void)p;(void)w;if(o)memset(o,0,sizeof(*o));return -1;}
int cbm_zova_repository_hydrate_incremental_components(const char*p,const char*w,int64_t g,cbm_zova_snapshot_components_t c,cbm_zova_workspace_snapshot_t*s){(void)p;(void)w;(void)g;(void)c;(void)s;return CBM_ZOVA_SNAPSHOT_ERROR;}
int cbm_zova_workspace_snapshot_format_edge_id(const cbm_zova_workspace_snapshot_t*s,int i,char*o,size_t z){(void)s;(void)i;if(o&&z)o[0]='\0';return -1;}
void cbm_zova_workspace_snapshot_free(cbm_zova_workspace_snapshot_t*s){if(s)memset(s,0,sizeof(*s));}
cbm_zova_catalog_t *cbm_zova_catalog_open(const char*p){(void)p;return NULL;}
void cbm_zova_catalog_close(cbm_zova_catalog_t*c){(void)c;}
int cbm_zova_catalog_list(cbm_zova_catalog_t*c,cbm_zova_catalog_scope_t*o){(void)c;if(o)memset(o,0,sizeof(*o));return CBM_STORE_ERR;}
const char *cbm_zova_catalog_error(const cbm_zova_catalog_t*c){(void)c;return "";}
int cbm_zova_catalog_resolve(cbm_zova_catalog_t*c,const char*const*s,int n,bool w,cbm_zova_catalog_scope_t*o){(void)c;(void)s;(void)n;(void)w;if(o)memset(o,0,sizeof(*o));return CBM_STORE_ERR;}
int cbm_zova_catalog_search_fts(cbm_zova_catalog_t*c,const cbm_zova_catalog_scope_t*s,const char*q,const char*f,int l,int x,cbm_search_output_t*o){(void)c;(void)s;(void)q;(void)f;(void)l;(void)x;if(o)memset(o,0,sizeof(*o));return CBM_STORE_ERR;}
int cbm_zova_catalog_search(cbm_zova_catalog_t*c,const cbm_zova_catalog_scope_t*s,const cbm_search_params_t*p,cbm_search_output_t*o){(void)c;(void)s;(void)p;if(o)memset(o,0,sizeof(*o));return CBM_STORE_ERR;}
int cbm_zova_catalog_search_semantic(cbm_zova_catalog_t*c,const char*p,const cbm_zova_catalog_scope_t*s,const char**k,int n,int l,int x,cbm_vector_result_t**o,int*z,int*t){(void)c;(void)p;(void)s;(void)k;(void)n;(void)l;(void)x;if(o)*o=NULL;if(z)*z=0;if(t)*t=0;return CBM_STORE_ERR;}
void cbm_zova_catalog_scope_free(cbm_zova_catalog_scope_t*s){if(s)memset(s,0,sizeof(*s));}
#endif
