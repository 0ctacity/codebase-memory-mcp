/*
 * test_helpers.h — Cross-platform test fixture helpers.
 *
 * Replaces system("rm -rf"), system("mkdir -p && echo >"), chmod(),
 * and hardcoded /tmp/ paths with portable C implementations.
 * Uses compat.h/compat_fs.h underneath — same abstraction layer
 * as production code.
 *
 * All functions are static inline to avoid linker issues when
 * included from multiple test files.
 */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/git_env.h"
#include "../src/store/store.h"
#include "../src/zova/cbm_zova.h"
#include "../src/zova/cbm_zova_route.h"
#include "../src/zova/cbm_zova_repository.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Path building ────────────────────────────────────────────── */

/* Build a path from base + relative. Uses a static buffer — not reentrant.
 * Usage: th_write_file(TH_PATH(base, "src/main.go"), "content"); */
#define TH_PATH(base, rel) th_path_join(base, rel)

static inline const char *th_path_join(const char *base, const char *rel) {
    static char buf[4][1024];
    static int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

/* ── File writing ─────────────────────────────────────────────── */

/* Write content to a file, creating parent directories as needed. */
static inline int th_write_file(const char *path, const char *content) {
    /* Create parent directories */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
#ifdef _WIN32
    char *last_bslash = strrchr(dir, '\\');
    if (last_bslash && (!last_slash || last_bslash > last_slash)) {
        last_slash = last_bslash;
    }
#endif
    if (last_slash) {
        *last_slash = '\0';
        cbm_mkdir_p(dir, 0755);
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}

/* Append content to a file. */
static inline int th_append_file(const char *path, const char *content) {
    FILE *f = fopen(path, "a");
    if (!f) {
        return -1;
    }
    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}

/* ── Directory creation ───────────────────────────────────────── */

/* Create a directory and all parents. Returns 0 on success. */
static inline int th_mkdir_p(const char *path) {
    return cbm_mkdir_p(path, 0755) ? 0 : -1;
}

/* ── Recursive directory removal ──────────────────────────────── */

/* Remove a file or directory tree recursively. Cross-platform rm -rf. */
static inline int th_rmtree(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0; /* doesn't exist — success */
    }

    if (!S_ISDIR(st.st_mode)) {
        return cbm_unlink(path);
    }

    /* Directory — recurse into children */
    cbm_dir_t *d = cbm_opendir(path);
    if (!d) {
        return -1;
    }

    cbm_dirent_t *entry;
    int rc = 0;
    while ((entry = cbm_readdir(d)) != NULL) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            continue;
        }
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, entry->name);
        if (entry->is_dir) {
            if (th_rmtree(child) != 0) {
                rc = -1;
            }
        } else {
            if (cbm_unlink(child) != 0) {
                rc = -1;
            }
        }
    }
    cbm_closedir(d);
    cbm_rmdir(path);
    return rc;
}

/* ── Temp directory creation ──────────────────────────────────── */

/* Create a temporary directory. Returns static buffer with path.
 * Pattern: prefix is used as part of the dirname. */
static inline char *th_mktempdir(const char *prefix) {
    static char buf[256];
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = "C:\\Temp";
    snprintf(buf, sizeof(buf), "%s\\%s_XXXXXX", tmp, prefix);
#else
    /* Honour TMPDIR so a runner pointed at another volume keeps its temp dirs there. */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t tmp_len = strlen(tmp);
    while (tmp_len > 1 && tmp[tmp_len - 1] == '/') tmp_len--;
    snprintf(buf, sizeof(buf), "%.*s/%s_XXXXXX", (int)tmp_len, tmp, prefix);
#endif
    if (!cbm_mkdtemp(buf)) {
        return NULL;
    }
    return buf;
}

/* ── File permissions (no-op on Windows) ──────────────────────── */

/* Make a file executable. On Windows this is a no-op (all files are
 * "executable" if they have the right extension). */
static inline void th_make_executable(const char *path) {
#ifndef _WIN32
    chmod(path, 0755);
#else
    (void)path;
#endif
}

/* ── Cleanup helper ───────────────────────────────────────────── */

/* Remove a temp directory tree. Safe to call with NULL. */
static inline void th_cleanup(const char *path) {
    if (path && path[0]) {
        th_rmtree(path);
    }
}

/* ── Indexed project access ───────────────────────────────────── */

/* Open the read-only store view of an indexed project in the shared cbm.zova.
 * Returns NULL when the project is not indexed. Caller closes the store. */
static inline cbm_store_t *th_open_project_store(const char *project) {
    char path[1024];
    if (!project || cbm_zova_user_database_path(path, sizeof(path)) != 0) {
        return NULL;
    }
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) {
        return NULL;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_id, sizeof(workspace_id), "%s", cbm_zova_repository_workspace_id(repo));
    cbm_zova_repository_close(repo);
    return cbm_store_open_zova_workspace_query(path, workspace_id);
}

/* From pipeline/pipeline.h, which is not included here: it pulls in
 * internal/cbm/arena.h, whose include guard hides src/foundation/arena.h. */
char *cbm_project_name_from_path(const char *abs_path);

/* Open the store of the project indexed from repo_path. Caller closes it. */
static inline cbm_store_t *th_open_repo_store(const char *repo_path) {
    char *project = cbm_project_name_from_path(repo_path);
    cbm_store_t *store = th_open_project_store(project);
    free(project);
    return store;
}

/* Remove the shared cbm.zova so a fixture starts from an empty catalog. */
static inline void th_remove_zova_user_database(void) {
    char path[1024];
    char side[1040];
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0) {
        return;
    }
    cbm_unlink(path);
    snprintf(side, sizeof(side), "%s-wal", path);
    cbm_unlink(side);
    snprintf(side, sizeof(side), "%s-shm", path);
    cbm_unlink(side);
}

/* Remove one project's workspace from the shared cbm.zova, if it is there. */
static inline void th_remove_project_workspace(const char *project) {
    char path[1024];
    if (!project || cbm_zova_user_database_path(path, sizeof(path)) != 0) {
        return;
    }
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) {
        return;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_id, sizeof(workspace_id), "%s", cbm_zova_repository_workspace_id(repo));
    cbm_zova_repository_close(repo);
    (void)cbm_zova_user_database_delete_workspace(path, workspace_id);
}

static inline char *th_strdup_column(sqlite3_stmt *stmt, int column) {
    const char *text = (const char *)sqlite3_column_text(stmt, column);
    return strdup(text ? text : "");
}

/* Publish one project of a fixture store (nodes, edges, root path) as a
 * workspace generation in the shared cbm.zova, so MCP handlers can serve it.
 * Returns 0 on success. */
static inline int th_publish_store_project(cbm_store_t *store, const char *project) {
    char path[1024];
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !project || cbm_zova_user_database_path(path, sizeof(path)) != 0) {
        return -1;
    }
    cbm_project_t row = {0};
    if (cbm_store_get_project(store, project, &row) != CBM_STORE_OK) {
        return -1;
    }
    int rc = -1;
    int node_count = 0;
    int edge_count = 0;
    CBMDumpNode *nodes = NULL;
    CBMDumpEdge *edges = NULL;
    sqlite3_stmt *node_stmt = NULL;
    sqlite3_stmt *edge_stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT id,label,name,qualified_name,file_path,start_line,end_line,"
                           "properties FROM nodes WHERE project=?1 ORDER BY id",
                           -1, &node_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
                           "SELECT id,source_id,target_id,type,properties FROM edges "
                           "WHERE project=?1 ORDER BY id",
                           -1, &edge_stmt, NULL) != SQLITE_OK) {
        goto done;
    }
    sqlite3_bind_text(node_stmt, 1, project, -1, SQLITE_STATIC);
    sqlite3_bind_text(edge_stmt, 1, project, -1, SQLITE_STATIC);
    while (sqlite3_step(node_stmt) == SQLITE_ROW) {
        nodes = realloc(nodes, (size_t)(node_count + 1) * sizeof(*nodes));
        nodes[node_count++] = (CBMDumpNode){
            .id = sqlite3_column_int64(node_stmt, 0),
            .project = project,
            .label = th_strdup_column(node_stmt, 1),
            .name = th_strdup_column(node_stmt, 2),
            .qualified_name = th_strdup_column(node_stmt, 3),
            .file_path = th_strdup_column(node_stmt, 4),
            .start_line = sqlite3_column_int(node_stmt, 5),
            .end_line = sqlite3_column_int(node_stmt, 6),
            .properties = th_strdup_column(node_stmt, 7),
        };
    }
    while (sqlite3_step(edge_stmt) == SQLITE_ROW) {
        edges = realloc(edges, (size_t)(edge_count + 1) * sizeof(*edges));
        edges[edge_count++] = (CBMDumpEdge){
            .id = sqlite3_column_int64(edge_stmt, 0),
            .project = project,
            .source_id = sqlite3_column_int64(edge_stmt, 1),
            .target_id = sqlite3_column_int64(edge_stmt, 2),
            .type = th_strdup_column(edge_stmt, 3),
            .properties = th_strdup_column(edge_stmt, 4),
        };
    }
    cbm_zova_workspace_generation_input_t input = {
        .root_path = row.root_path,
        .project = project,
        .indexed_at = "2026-01-01T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 768,
        .nodes = nodes,
        .node_count = node_count,
        .edges = edges,
        .edge_count = edge_count,
    };
    cbm_zova_workspace_generation_result_t result = {0};
    rc = cbm_zova_user_database_publish_workspace(path, &input, &result);
done:
    sqlite3_finalize(node_stmt);
    sqlite3_finalize(edge_stmt);
    for (int i = 0; i < node_count; i++) {
        free((char *)nodes[i].label);
        free((char *)nodes[i].name);
        free((char *)nodes[i].qualified_name);
        free((char *)nodes[i].file_path);
        free((char *)nodes[i].properties);
    }
    for (int i = 0; i < edge_count; i++) {
        free((char *)edges[i].type);
        free((char *)edges[i].properties);
    }
    free(nodes);
    free(edges);
    cbm_project_free_fields(&row);
    return rc;
}

/* ── Inherited git repository environment (#56) ─────────────── */

/* Git exports repository-local variables into hooks (GIT_DIR, GIT_INDEX_FILE,
 * ...), and they take precedence over `git -C <dir>`. A test runner started
 * from a hook, or any shell with GIT_DIR set, would point every fixture's git
 * command at the caller's real repository: commits, branches and worktrees get
 * created there and fixture assertions fail. Runners clear the list once at
 * startup so every spawned git, and every re-executed child, scopes to its
 * fixture. */
static inline void th_clear_git_repo_env(void) {
    for (size_t i = 0U; i < CBM_GIT_REPO_ENV_VAR_COUNT; i++) {
        (void)cbm_unsetenv(cbm_git_repo_env_vars[i]);
    }
}

#endif /* TEST_HELPERS_H */
