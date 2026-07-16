#include "zova/cbm_zova_route.h"

#include "foundation/platform.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "zova/cbm_zova.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

cbm_zova_route_t cbm_zova_route_from_env(void) {
    return cbm_zova_single_file_experimental_enabled()
               ? CBM_ZOVA_ROUTE_FULL_AUTHORITY
               : CBM_ZOVA_ROUTE_COMPATIBILITY;
}

cbm_zova_route_t cbm_zova_route_for_project(const char *project) {
    if (!cbm_zova_single_file_experimental_enabled()) return CBM_ZOVA_ROUTE_COMPATIBILITY;
    if (!project || !project[0]) return CBM_ZOVA_ROUTE_FULL_AUTHORITY;
    char path[1024];
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0 || !cbm_file_exists(path))
        return CBM_ZOVA_ROUTE_FULL_AUTHORITY;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    cbm_zova_route_t route = CBM_ZOVA_ROUTE_FULL_AUTHORITY;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
                           "SELECT state FROM cbm_workspace_migrations_v1 WHERE project=?1 "
                           "ORDER BY prepared_at DESC LIMIT 2",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return route;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *state = (const char *)sqlite3_column_text(stmt, 0);
        if (state && (strcmp(state, "prepared") == 0 || strcmp(state, "copying") == 0 ||
                      strcmp(state, "failed") == 0 || strcmp(state, "rolled_back") == 0))
            route = CBM_ZOVA_ROUTE_MIGRATION_LEGACY;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (route == CBM_ZOVA_ROUTE_MIGRATION_LEGACY)
        cbm_log_info("zova.route", "route", "migration_legacy", "project", project);
    return route;
}

int cbm_zova_user_database_path(char *out, size_t out_size) {
    const char *cache = cbm_resolve_cache_dir();
    if (!out || out_size == 0 || !cache || !cache[0]) return -1;
    int n = snprintf(out, out_size, "%s/cbm.zova", cache);
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}
