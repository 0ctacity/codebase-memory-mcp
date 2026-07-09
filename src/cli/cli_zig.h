/*
 * cli_zig.h — small Zig-backed helpers for CLI reporting.
 *
 * Keep this ABI narrow: the C CLI owns behavior and filesystem portability;
 * Zig formats the first migration-friendly support reports.
 */
#ifndef CBM_CLI_ZIG_H
#define CBM_CLI_ZIG_H

#ifdef __cplusplus
extern "C" {
#endif

char *cbm_cli_zig_doctor_report(const char *home, const char *running_binary,
                                const char *installed_binary, const char *cache_dir,
                                const char *project_dbs, const char *config_paths,
                                const char *detected_agents, int path_ready,
                                int ui_capable, int ui_enabled, int ui_port);

char *cbm_cli_zig_where_report(const char *running_binary, const char *installed_binary,
                               const char *cache_dir, const char *project_dbs,
                               const char *config_paths);

char *cbm_cli_zig_install_plan_overview(const char *binary_target, const char *shell_rc,
                                        const char *planned_paths);

void cbm_cli_zig_free(char *ptr);

#ifdef __cplusplus
}
#endif

#endif /* CBM_CLI_ZIG_H */
