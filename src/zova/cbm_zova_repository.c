#include "zova/cbm_zova_repository.h"

#include "zova/cbm_zova.h"
#include "foundation/compat.h"
#include "foundation/hash_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CBM_WITH_ZOVA
#include "zova.h"

struct cbm_zova_repository {
    zova_database *db;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char project[256];
    int64_t generation;
};

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

static double repo_elapsed_ms(struct timespec started) {
    struct timespec finished;
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    return (double)(finished.tv_sec - started.tv_sec) * 1000.0 +
           (double)(finished.tv_nsec - started.tv_nsec) / 1000000.0;
}

cbm_zova_repository_t *cbm_zova_repository_open(const char *path, const char *project) {
    if (!path || !path[0] || !project || !project[0]) return NULL;
    cbm_zova_repository_t *repo = calloc(1, sizeof(*repo));
    if (!repo) return NULL;
    zova_message error = {0};
    if (zova_database_open_with_options(&(zova_database_open_options_request){
            .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
            .out_db = &repo->db, .out_error_message = &error}) != ZOVA_OK || !repo->db) {
        zova_message_free(&error);
        free(repo);
        return NULL;
    }
    zova_message_free(&error);
    if (cbm_zova_register_sql_functions(repo->db) != 0 ||
        zova_database_exec(&(zova_database_exec_request){.db = repo->db,
                                                        .sql = "PRAGMA query_only=ON"}) != ZOVA_OK) {
        cbm_zova_repository_close(repo);
        return NULL;
    }
    zova_statement *stmt = NULL;
    const char *sql =
        "SELECT p.workspace_id,g.generation FROM cbm_projects_v1 p "
        "JOIN cbm_database_generation_v1 g ON g.workspace_id=p.workspace_id "
        "WHERE p.project=?1 AND g.state='ready' AND g.generation=(SELECT MAX(g2.generation) "
        "FROM cbm_database_generation_v1 g2 WHERE g2.workspace_id=p.workspace_id "
        "AND g2.state='ready') AND NOT EXISTS (SELECT 1 FROM cbm_workspace_health_v1 h "
        "WHERE h.workspace_id=p.workspace_id AND h.state='rebuild_required')";
    if (repo_prepare(repo, sql, &stmt) != 0 || repo_bind_text(stmt, 1, project) != 0) {
        if (stmt) (void)zova_statement_finalize(stmt);
        cbm_zova_repository_close(repo);
        return NULL;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int ok = repo_step(stmt, &step) == 0 && step == ZOVA_STEP_ROW;
    char *workspace = ok ? repo_column_text(stmt, 0) : NULL;
    if (ok) repo->generation = repo_column_i64(stmt, 1);
    zova_step_result second = ZOVA_STEP_DONE;
    if (ok && repo_step(stmt, &second) != 0) ok = 0;
    if (second == ZOVA_STEP_ROW) ok = 0;
    (void)zova_statement_finalize(stmt);
    if (!ok || !workspace || !workspace[0] || strlen(workspace) >= sizeof(repo->workspace_id) ||
        strlen(project) >= sizeof(repo->project)) {
        free(workspace);
        cbm_zova_repository_close(repo);
        return NULL;
    }
    strcpy(repo->workspace_id, workspace);
    strcpy(repo->project, project);
    free(workspace);
    return repo;
}

void cbm_zova_repository_close(cbm_zova_repository_t *repo) {
    if (!repo) return;
    if (repo->db) (void)zova_database_close(repo->db);
    free(repo);
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

static int hydrate_node(cbm_zova_repository_t *repo, const char *workspace_id,
                        const char *column, const char *value,
                        cbm_node_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !value || !out)
        return CBM_STORE_ERR;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT node_id,project,label,name,qualified_name,file_path,"
             "start_line,end_line,properties FROM cbm_nodes_v1 WHERE workspace_id=?1 AND %s=?2",
             column);
    zova_statement *stmt = NULL;
    if (repo_prepare(repo, sql, &stmt) != 0 || repo_bind_text(stmt, 1, repo->workspace_id) != 0 ||
        repo_bind_text(stmt, 2, value) != 0) goto error;
    zova_step_result step = ZOVA_STEP_DONE;
    if (repo_step(stmt, &step) != 0 || step != ZOVA_STEP_ROW) {
        (void)zova_statement_finalize(stmt);
        return CBM_STORE_NOT_FOUND;
    }
    char *stable = repo_column_text(stmt, 0);
    memset(out, 0, sizeof(*out));
    out->id = stable_numeric_id(stable);
    free(stable);
    out->project = repo_column_text(stmt, 1);
    out->label = repo_column_text(stmt, 2);
    out->name = repo_column_text(stmt, 3);
    out->qualified_name = repo_column_text(stmt, 4);
    out->file_path = repo_column_text(stmt, 5);
    out->start_line = (int)repo_column_i64(stmt, 6);
    out->end_line = (int)repo_column_i64(stmt, 7);
    out->properties_json = repo_column_text(stmt, 8);
    (void)zova_statement_finalize(stmt);
    return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_find_node_by_qn(cbm_zova_repository_t *repo, const char *workspace_id,
                                         const char *qualified_name, cbm_node_t *out) {
    return hydrate_node(repo, workspace_id, "qualified_name", qualified_name, out);
}

int cbm_zova_repository_find_node_by_stable_id(cbm_zova_repository_t *repo,
                                                const char *workspace_id,
                                                const char *stable_id, cbm_node_t *out) {
    return hydrate_node(repo, workspace_id, "node_id", stable_id, out);
}

int cbm_zova_repository_get_project(cbm_zova_repository_t *repo, const char *workspace_id,
                                    cbm_project_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !out)
        return CBM_STORE_ERR;
    zova_statement *stmt = NULL;
    if (repo_prepare(repo, "SELECT project,indexed_at,root_path FROM cbm_projects_v1 "
                           "WHERE workspace_id=?1", &stmt) != 0 ||
        repo_bind_text(stmt, 1, repo->workspace_id) != 0) goto error;
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
                     "SELECT (SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_id=?1),"
                     "(SELECT count(*) FROM cbm_edges_v1 WHERE workspace_id=?1)",
                     &stmt) != 0 ||
        repo_bind_text(stmt, 1, repo->workspace_id) != 0) goto error;
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
                           "FROM cbm_project_summaries_v2 WHERE workspace_id=?1", &stmt) != 0 ||
        repo_bind_text(stmt, 1, repo->workspace_id) != 0) goto error;
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

int cbm_zova_repository_find_edges(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *stable_id,
                                   const char *direction, cbm_edge_t **out, int *out_count) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !stable_id || !out ||
        !out_count) return CBM_STORE_ERR;
    *out = NULL; *out_count = 0;
    const char *predicate = !direction || strcmp(direction, "any") == 0
                                ? "(source_node_id=?2 OR target_node_id=?2)"
                                : strcmp(direction, "inbound") == 0 ? "target_node_id=?2"
                                                                    : "source_node_id=?2";
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT edge_id,source_node_id,target_node_id,edge_type,properties "
             "FROM cbm_edges_v1 WHERE workspace_id=?1 AND %s ORDER BY edge_id", predicate);
    zova_statement *stmt = NULL;
    if (repo_prepare(repo, sql, &stmt) != 0 || repo_bind_text(stmt, 1, repo->workspace_id) != 0 ||
        repo_bind_text(stmt, 2, stable_id) != 0) goto error;
    int cap = 8, count = 0;
    cbm_edge_t *edges = calloc((size_t)cap, sizeof(*edges));
    if (!edges) goto error;
    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(stmt, &step) != 0) { cbm_store_free_edges(edges, count); goto error; }
        if (step == ZOVA_STEP_DONE) break;
        if (count == cap) {
            cap *= 2; cbm_edge_t *grown = realloc(edges, (size_t)cap * sizeof(*edges));
            if (!grown) { cbm_store_free_edges(edges, count); goto error; }
            edges = grown; memset(edges + count, 0, (size_t)(cap-count) * sizeof(*edges));
        }
        char *eid = repo_column_text(stmt, 0), *src = repo_column_text(stmt, 1),
             *dst = repo_column_text(stmt, 2);
        edges[count].id = stable_numeric_id(eid);
        edges[count].project = repo_dup((const uint8_t *)repo->project, strlen(repo->project));
        edges[count].source_id = stable_numeric_id(src); edges[count].target_id = stable_numeric_id(dst);
        edges[count].type = repo_column_text(stmt, 3);
        edges[count].properties_json = repo_column_text(stmt, 4);
        free(eid); free(src); free(dst); count++;
    }
    (void)zova_statement_finalize(stmt); *out = edges; *out_count = count; return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
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
    const char *sql = "SELECT n.node_id,n.project,n.label,n.name,n.qualified_name,n.file_path,"
        "n.start_line,n.end_line,n.properties,(bm25(cbm_nodes_fts_v1)-CASE "
        "WHEN n.label IN ('Function','Method') THEN 10.0 WHEN n.label='Route' THEN 8.0 "
        "WHEN n.label IN ('Class','Interface','Type','Enum') THEN 5.0 ELSE 0.0 END) AS rank "
        "FROM cbm_nodes_fts_v1 f "
        "JOIN cbm_nodes_v1 n ON n.workspace_id=f.workspace_id AND n.node_id=f.node_id "
        "WHERE f.workspace_id=?1 AND cbm_nodes_fts_v1 MATCH ?2 "
        "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') "
        "AND (?3 IS NULL OR n.file_path GLOB ?3) "
        "ORDER BY rank,n.qualified_name,n.node_id LIMIT ?4 OFFSET ?5";
    if (repo_prepare(repo, sql, &stmt) != 0 || repo_bind_text(stmt, 1, repo->workspace_id) != 0 ||
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
    if (!results) goto error;
    for (;;) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(stmt, &step) != 0) { cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count}); goto error; }
        if (step == ZOVA_STEP_DONE) break;
        if (count == cap) { cap *= 2; cbm_search_result_t *g = realloc(results,(size_t)cap*sizeof(*g));
            if (!g) { cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count}); goto error; }
            results=g; memset(results+count,0,(size_t)(cap-count)*sizeof(*results)); }
        cbm_node_t *n=&results[count].node; char *sid=repo_column_text(stmt,0); n->id=stable_numeric_id(sid); free(sid);
        n->project=repo_column_text(stmt,1); n->label=repo_column_text(stmt,2); n->name=repo_column_text(stmt,3);
        n->qualified_name=repo_column_text(stmt,4); n->file_path=repo_column_text(stmt,5);
        n->start_line=(int)repo_column_i64(stmt,6); n->end_line=(int)repo_column_i64(stmt,7);
        n->properties_json=repo_column_text(stmt,8);
        results[count].rank=repo_column_double(stmt,9);
        count++;
    }
    (void)zova_statement_finalize(stmt); out->results=results; out->count=count; out->total=count; return CBM_STORE_OK;
error:
    if (stmt) (void)zova_statement_finalize(stmt);
    return CBM_STORE_ERR;
}

int cbm_zova_repository_search(cbm_zova_repository_t *repo, const char *workspace_id,
                               const cbm_search_params_t *params, cbm_search_output_t *out) {
    if (repo_workspace_required(repo, workspace_id) != CBM_STORE_OK || !params || !out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    char sql[4096] =
        "SELECT n.node_id,n.project,n.label,n.name,n.qualified_name,n.file_path,"
        "n.start_line,n.end_line,n.properties,"
        "(SELECT count(*) FROM cbm_edges_v1 e WHERE e.workspace_id=n.workspace_id AND e.target_node_id=n.node_id AND e.edge_type IN ('CALLS','USAGE','INHERITS','IMPLEMENTS')),"
        "(SELECT count(*) FROM cbm_edges_v1 e WHERE e.workspace_id=n.workspace_id AND e.source_node_id=n.node_id AND e.edge_type IN ('CALLS','USAGE','INHERITS','IMPLEMENTS')),"
        "count(*) OVER() FROM cbm_nodes_v1 n WHERE n.workspace_id=?1";
    const char *binds[6] = {0}; int bind_count = 0;
#define REPO_FILTER(clause, value) do { if ((value) && (value)[0]) { strncat(sql, (clause), sizeof(sql)-strlen(sql)-1); binds[bind_count++]=(value); } } while (0)
    REPO_FILTER(" AND n.label=?", params->label);
    REPO_FILTER(" AND regexp(?,n.name)", params->name_pattern);
    REPO_FILTER(" AND regexp(?,n.qualified_name)", params->qn_pattern);
    REPO_FILTER(" AND n.file_path GLOB ?", params->file_pattern);
    REPO_FILTER(" AND EXISTS(SELECT 1 FROM cbm_edges_v1 er WHERE er.workspace_id=n.workspace_id AND (er.source_node_id=n.node_id OR er.target_node_id=n.node_id) AND er.edge_type=?)", params->relationship);
#undef REPO_FILTER
    if (params->exclude_entry_points) strncat(sql, " AND NOT (NOT EXISTS(SELECT 1 FROM cbm_edges_v1 ei WHERE ei.workspace_id=n.workspace_id AND ei.target_node_id=n.node_id AND ei.edge_type='CALLS') AND EXISTS(SELECT 1 FROM cbm_edges_v1 eo WHERE eo.workspace_id=n.workspace_id AND eo.source_node_id=n.node_id AND eo.edge_type='CALLS'))", sizeof(sql)-strlen(sql)-1);
    strncat(sql, " ORDER BY n.name,n.qualified_name,n.node_id LIMIT ? OFFSET ?", sizeof(sql)-strlen(sql)-1);
    zova_statement *stmt=NULL;
    if (repo_prepare(repo,sql,&stmt)!=0 || repo_bind_text(stmt,1,repo->workspace_id)!=0) goto regular_error;
    int idx=2;
    for(int i=0;i<bind_count;i++) if(repo_bind_text(stmt,idx++,binds[i])!=0) goto regular_error;
    if(repo_bind_i64(stmt,idx++,params->limit>0?params->limit:10)!=0 || repo_bind_i64(stmt,idx,params->offset)!=0) goto regular_error;
    int cap=8,count=0; cbm_search_result_t *results=calloc((size_t)cap,sizeof(*results)); if(!results) goto regular_error;
    for(;;){zova_step_result step=ZOVA_STEP_DONE; if(repo_step(stmt,&step)!=0){cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});goto regular_error;} if(step==ZOVA_STEP_DONE)break;
        if(count==cap){cap*=2;cbm_search_result_t*g=realloc(results,(size_t)cap*sizeof(*g));if(!g){cbm_store_search_free(&(cbm_search_output_t){.results=results,.count=count});goto regular_error;}results=g;memset(results+count,0,(size_t)(cap-count)*sizeof(*results));}
        cbm_node_t*n=&results[count].node;char*sid=repo_column_text(stmt,0);n->id=stable_numeric_id(sid);free(sid);n->project=repo_column_text(stmt,1);n->label=repo_column_text(stmt,2);n->name=repo_column_text(stmt,3);n->qualified_name=repo_column_text(stmt,4);n->file_path=repo_column_text(stmt,5);n->start_line=(int)repo_column_i64(stmt,6);n->end_line=(int)repo_column_i64(stmt,7);n->properties_json=repo_column_text(stmt,8);results[count].in_degree=(int)repo_column_i64(stmt,9);results[count].out_degree=(int)repo_column_i64(stmt,10);if(count==0)out->total=(int)repo_column_i64(stmt,11);count++;}
    (void)zova_statement_finalize(stmt);out->results=results;out->count=count;return CBM_STORE_OK;
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
    free(snapshot->node_source_ordinals);
    free(snapshot->nodes);
    for (int i = 0; i < snapshot->edge_count; i++) {
        free(snapshot->edge_ids ? snapshot->edge_ids[i] : NULL);
        free((char *)snapshot->edges[i].project);
        free((char *)snapshot->edges[i].type);
        free((char *)snapshot->edges[i].properties);
        free((char *)snapshot->edges[i].url_path);
        free((char *)snapshot->edges[i].local_name);
    }
    free(snapshot->edge_ids);
    free(snapshot->edges);
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
        repo_bind_text(statement, 1, repo->workspace_id) != 0) {
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

static int64_t snapshot_map_node(const CBMHashTable *node_ids, const char *stable_id) {
    return (int64_t)(intptr_t)cbm_ht_get(node_ids, stable_id);
}

static int snapshot_read_nodes(cbm_zova_repository_t *repo,
                               cbm_zova_workspace_snapshot_t *snapshot,
                               char ***out_stable_ids) {
    int count = snapshot_count(repo,
        "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_id=?1");
    if (count < 0) return -1;
    snapshot->node_count = count;
    snapshot->nodes = count ? calloc((size_t)count, sizeof(*snapshot->nodes)) : NULL;
    char **stable_ids = count ? calloc((size_t)count, sizeof(*stable_ids)) : NULL;
    snapshot->node_source_ordinals =
        count ? calloc((size_t)count, sizeof(*snapshot->node_source_ordinals)) : NULL;
    if (count && (!snapshot->nodes || !stable_ids || !snapshot->node_source_ordinals)) {
        free(stable_ids);
        return -1;
    }
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT n.node_id,n.project,n.label,n.name,n.qualified_name,n.file_path,n.start_line,"
        "n.end_line,n.properties,n.source_ordinal FROM cbm_nodes_v1 n "
        "WHERE n.workspace_id=?1 ORDER BY n.node_id", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
    for (int i = 0; rc == 0 && i < count; i++) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        CBMDumpNode *node = &snapshot->nodes[i];
        node->id = i + 1;
        stable_ids[i] = repo_column_text(statement, 0);
        node->project = repo_column_text(statement, 1);
        node->label = repo_column_text(statement, 2);
        node->name = repo_column_text(statement, 3);
        node->qualified_name = repo_column_text(statement, 4);
        node->file_path = repo_column_text(statement, 5);
        node->start_line = (int)repo_column_i64(statement, 6);
        node->end_line = (int)repo_column_i64(statement, 7);
        node->properties = repo_column_text(statement, 8);
        snapshot->node_source_ordinals[i] = (uint64_t)repo_column_i64(statement, 9);
        if (!stable_ids[i] || !node->project || !node->label || !node->name ||
            !node->qualified_name || !node->file_path || !node->properties) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    if (rc != 0) {
        for (int i = 0; i < count; i++) free(stable_ids[i]);
        free(stable_ids);
        return -1;
    }
    snapshot->node_stable_ids = stable_ids;
    *out_stable_ids = stable_ids;
    return 0;
}

static int snapshot_read_edges(cbm_zova_repository_t *repo,
                               cbm_zova_workspace_snapshot_t *snapshot,
                               const CBMHashTable *node_ids) {
    int count = snapshot_count(repo,
        "SELECT count(*) FROM cbm_edges_v1 WHERE workspace_id=?1");
    if (count < 0) return -1;
    snapshot->edge_count = count;
    snapshot->edges = count ? calloc((size_t)count, sizeof(*snapshot->edges)) : NULL;
    snapshot->edge_ids = count ? calloc((size_t)count, sizeof(*snapshot->edge_ids)) : NULL;
    if (count && (!snapshot->edges || !snapshot->edge_ids)) return -1;
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT edge_id,source_node_id,target_node_id,edge_type,properties,url_path,local_name "
        "FROM cbm_edges_v1 WHERE workspace_id=?1 ORDER BY edge_id", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
    for (int i = 0; rc == 0 && i < count; i++) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        snapshot->edge_ids[i] = repo_column_text(statement, 0);
        char *source = repo_column_text(statement, 1);
        char *target = repo_column_text(statement, 2);
        CBMDumpEdge *edge = &snapshot->edges[i];
        edge->id = i + 1;
        edge->project = repo_dup((const uint8_t *)snapshot->project, strlen(snapshot->project));
        edge->source_id = source ? snapshot_map_node(node_ids, source) : -1;
        edge->target_id = target ? snapshot_map_node(node_ids, target) : -1;
        edge->type = repo_column_text(statement, 3);
        edge->properties = repo_column_text(statement, 4);
        edge->url_path = repo_column_text(statement, 5);
        edge->local_name = repo_column_text(statement, 6);
        free(source); free(target);
        if (!snapshot->edge_ids[i] || !edge->project || edge->source_id <= 0 ||
            edge->target_id <= 0 || !edge->type ||
            !edge->properties || !edge->url_path || !edge->local_name) rc = -1;
    }
    if (statement) (void)zova_statement_finalize(statement);
    return rc;
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
        uint8_t exists = 0;
        if (zova_vector_exists(&(zova_vector_exists_request){
                .db = repo->db, .collection_name = node_collection,
                .vector_id = snapshot->node_stable_ids[i], .out_exists = &exists}) != ZOVA_OK) {
            rc = -1;
            break;
        }
        if (!exists) continue;
        if (node_written >= node_count) { rc = -1; break; }
        zova_vector native = {0};
        if (zova_vector_get(&(zova_vector_get_request){
                .db = repo->db, .collection_name = node_collection,
                .vector_id = snapshot->node_stable_ids[i], .out_vector = &native}) != ZOVA_OK ||
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
    zova_statement *statement = NULL;
    if (rc == 0) rc = repo_prepare(repo,
        "SELECT token_id,token,idf FROM cbm_token_vector_metadata_v1 "
        "WHERE workspace_id=?1 ORDER BY token_id", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
    int token_written = 0;
    for (; rc == 0 && token_written < token_count; token_written++) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW) { rc = -1; break; }
        char *vector_id = repo_column_text(statement, 0);
        char *token = repo_column_text(statement, 1);
        zova_vector native = {0};
        if (!vector_id || !token || zova_vector_get(&(zova_vector_get_request){
                .db = repo->db, .collection_name = token_collection,
                .vector_id = vector_id, .out_vector = &native}) != ZOVA_OK ||
            native.element_type != ZOVA_VECTOR_ELEMENT_TYPE_I8 ||
            native.values_len != (size_t)snapshot->vector_dimensions) {
            free(vector_id); free(token); zova_vector_free(&native); rc = -1; break;
        }
        CBMDumpTokenVec *vector = &out->token_vectors[token_written];
        out->token_vector_ids[token_written] = vector_id;
        vector_id = NULL;
        vector->id = token_written + 1;
        vector->project = repo_dup((const uint8_t *)snapshot->project, strlen(snapshot->project));
        vector->token = token;
        vector->idf = (float)repo_column_double(statement, 2);
        uint8_t *bytes = malloc(native.values_len);
        if (vector->id <= 0 || !vector->project || !bytes) rc = -1;
        else {
            memcpy(bytes, native.i8_values, native.values_len);
            vector->vector = bytes;
            vector->vector_len = (int)native.values_len;
        }
        free(vector_id);
        zova_vector_free(&native);
    }
    if (statement) {
        zova_step_result step = ZOVA_STEP_DONE;
        if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_DONE)) rc = -1;
        (void)zova_statement_finalize(statement);
    }
    if (rc == 0 && token_written != token_count) rc = -1;
    return rc;
}

static int snapshot_read_hashes_summary(cbm_zova_repository_t *repo,
                                        cbm_zova_workspace_snapshot_t *snapshot) {
    int count = snapshot_count(repo,
        "SELECT count(*) FROM cbm_file_hashes_v1 WHERE workspace_id=?1");
    if (count < 0) return -1;
    snapshot->file_hash_count = count;
    snapshot->file_hashes = count ? calloc((size_t)count, sizeof(*snapshot->file_hashes)) : NULL;
    if (count && !snapshot->file_hashes) return -1;
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT file_path,content_hash,mtime_ns,size_bytes FROM cbm_file_hashes_v1 "
        "WHERE workspace_id=?1 ORDER BY file_path", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
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
        "WHERE workspace_id=?1", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
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
    return rc;
}

static int snapshot_read_header(cbm_zova_repository_t *repo,
                                cbm_zova_workspace_snapshot_t *snapshot) {
    zova_statement *statement = NULL;
    int rc = repo_prepare(repo,
        "SELECT r.canonical_root,p.root_path,p.project,p.indexed_at,s.model_fingerprint,"
        "s.vector_dimensions,r.active_generation FROM cbm_workspace_registry r "
        "JOIN cbm_projects_v1 p ON p.workspace_id=r.workspace_id "
        "JOIN cbm_workspace_index_state_v1 s ON s.workspace_id=r.workspace_id "
        "JOIN cbm_database_generation_v1 g ON g.workspace_id=r.workspace_id "
        "AND g.generation=r.active_generation AND g.state='ready' "
        "WHERE r.workspace_id=?1 AND s.generation=r.active_generation", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0 && (repo_step(statement, &step) != 0 || step != ZOVA_STEP_ROW)) rc = -1;
    if (rc == 0) {
        snapshot->root_path = repo_column_text(statement, 0);
        char *project_root = repo_column_text(statement, 1);
        snapshot->project = repo_column_text(statement, 2);
        snapshot->indexed_at = repo_column_text(statement, 3);
        snapshot->model_fingerprint = repo_column_text(statement, 4);
        snapshot->vector_dimensions = (int)repo_column_i64(statement, 5);
        snapshot->generation = repo_column_i64(statement, 6);
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
        "WHERE workspace_id=?1 AND generation=?2", &statement);
    if (rc == 0) rc = repo_bind_text(statement, 1, repo->workspace_id);
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
    bool transaction_started = false;
    if (rc == 0 && (cbm_zova_register_sql_functions(repo.db) != 0 ||
                    zova_database_exec(&(zova_database_exec_request){
                        .db = repo.db, .sql = "PRAGMA query_only=ON;BEGIN"}) != ZOVA_OK)) rc = -1;
    else if (rc == 0) transaction_started = true;
    char **stable_ids = NULL;
    CBMHashTable *node_ids = NULL;
    if (rc == 0) rc = snapshot_read_header(&repo, &snapshot);
    if (rc == 0) rc = snapshot_read_integrity(&repo, &snapshot);
    if (rc == 0) rc = snapshot_read_nodes(&repo, &snapshot, &stable_ids);
    if (rc == 0) {
        node_ids = cbm_ht_create(snapshot.node_count > 0 ? (uint32_t)snapshot.node_count * 2 : 16);
        if (!node_ids) {
            rc = -1;
        } else {
            for (int i = 0; i < snapshot.node_count; i++)
                cbm_ht_set(node_ids, stable_ids[i], (void *)(intptr_t)snapshot.nodes[i].id);
        }
    }
    if (rc == 0) rc = snapshot_read_edges(&repo, &snapshot, node_ids);
    if (rc == 0) rc = snapshot_read_hashes_summary(&repo, &snapshot);
    snapshot.metrics.base_ms = repo_elapsed_ms(base_started);
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
    cbm_ht_free(node_ids);
    if (transaction_started) {
        zova_status tx_status = zova_database_exec(&(zova_database_exec_request){
            .db = repo.db, .sql = rc == 0 ? "COMMIT" : "ROLLBACK"});
        if (rc == 0 && tx_status != ZOVA_OK) rc = -1;
    }
    if (repo.db) (void)zova_database_close(repo.db);
    if (rc != 0) {
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
cbm_zova_repository_t *cbm_zova_repository_open(const char *p,const char *n){(void)p;(void)n;return NULL;}
void cbm_zova_repository_close(cbm_zova_repository_t *r){(void)r;}
const char *cbm_zova_repository_workspace_id(const cbm_zova_repository_t *r){(void)r;return "";}
int cbm_zova_repository_get_project(cbm_zova_repository_t*r,const char*w,cbm_project_t*o){(void)r;(void)w;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_node_by_qn(cbm_zova_repository_t*r,const char*w,const char*q,cbm_node_t*o){(void)r;(void)w;(void)q;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_node_by_stable_id(cbm_zova_repository_t*r,const char*w,const char*s,cbm_node_t*o){(void)r;(void)w;(void)s;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_find_edges(cbm_zova_repository_t*r,const char*w,const char*s,const char*d,cbm_edge_t**o,int*c){(void)r;(void)w;(void)s;(void)d;(void)o;(void)c;return CBM_STORE_ERR;}
int cbm_zova_repository_search(cbm_zova_repository_t*r,const char*w,const cbm_search_params_t*p,cbm_search_output_t*o){(void)r;(void)w;(void)p;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_search_fts(cbm_zova_repository_t*r,const char*w,const char*q,const char*f,int l,int x,cbm_search_output_t*o){(void)r;(void)w;(void)q;(void)f;(void)l;(void)x;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_index_status(cbm_zova_repository_t*r,const char*w,int64_t*g,char**i){(void)r;(void)w;(void)g;(void)i;return CBM_STORE_ERR;}
int cbm_zova_repository_counts(cbm_zova_repository_t*r,const char*w,int*n,int*e){(void)r;(void)w;(void)n;(void)e;return CBM_STORE_ERR;}
int cbm_zova_repository_project_summary(cbm_zova_repository_t*r,const char*w,cbm_project_summary_export_t*o){(void)r;(void)w;(void)o;return CBM_STORE_ERR;}
int cbm_zova_repository_export_snapshot(const char*p,const char*w,cbm_zova_workspace_snapshot_t*o){(void)p;(void)w;if(o)memset(o,0,sizeof(*o));return -1;}
int cbm_zova_repository_export_incremental_snapshot(const char*p,const char*w,cbm_zova_workspace_snapshot_t*o){(void)p;(void)w;if(o)memset(o,0,sizeof(*o));return -1;}
int cbm_zova_repository_hydrate_incremental_components(const char*p,const char*w,int64_t g,cbm_zova_snapshot_components_t c,cbm_zova_workspace_snapshot_t*s){(void)p;(void)w;(void)g;(void)c;(void)s;return CBM_ZOVA_SNAPSHOT_ERROR;}
void cbm_zova_workspace_snapshot_free(cbm_zova_workspace_snapshot_t*s){if(s)memset(s,0,sizeof(*s));}
#endif
