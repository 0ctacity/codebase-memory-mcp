#ifndef CBM_ZOVA_OPERATIONS_H
#define CBM_ZOVA_OPERATIONS_H

#include "zova/cbm_zova.h"

#include <stdbool.h>
#include <stdint.h>

#define CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION 1

typedef enum {
    CBM_ZOVA_OPERATION_OK = 0,
    CBM_ZOVA_OPERATION_NOOP = 1,
    CBM_ZOVA_OPERATION_INVALID = -1,
    CBM_ZOVA_OPERATION_BUSY = -2,
    CBM_ZOVA_OPERATION_INCOMPATIBLE = -3,
    CBM_ZOVA_OPERATION_VERIFY_FAILED = -4,
    CBM_ZOVA_OPERATION_DISK_REFUSED = -5,
    CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED = -6,
    CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED = -7,
    CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED = -8,
} cbm_zova_operation_code_t;

typedef enum {
    CBM_ZOVA_HEALTH_OK = 0,
    CBM_ZOVA_HEALTH_WORKSPACE_REBUILD = 1,
    CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY = 2,
} cbm_zova_health_class_t;

typedef struct {
    cbm_zova_operation_code_t code;
    char operation[32];
    char reason[64];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    int schema_version;
    int archive_version;
    int64_t generation;
    uint64_t database_bytes;
    uint64_t wal_bytes;
    uint64_t free_bytes;
    uint64_t reclaimable_bytes;
    uint64_t page_size;
    uint64_t page_count;
    uint64_t freelist_count;
    double elapsed_ms;
} cbm_zova_operation_report_t;

typedef struct {
    const char *live_path;
    bool keep_recovery;
} cbm_zova_repack_request_t;

typedef struct {
    cbm_zova_operation_code_t code;
    int source_cbm_schema;
    int target_cbm_schema;
    int workspace_count;
    char source_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char target_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char recovery_path[4096];
} cbm_zova_repack_report_t;

const char *cbm_zova_operation_code_name(cbm_zova_operation_code_t code);
cbm_zova_operation_code_t cbm_zova_database_status(
    const char *path, cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_workspace_delete(
    const char *path, const char *workspace_id, const char *confirmation,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_compact(
    const char *path, cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_backup(
    const char *source_path, const char *destination_path,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_restore(
    const char *live_path, const char *backup_path, bool confirm_replace,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_export(
    const char *source_path, const char *archive_directory,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_import(
    const char *live_path, const char *archive_directory, bool confirm_replace,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_workspace_export(
    const char *source_path, const char *workspace_id,
    const char *archive_directory, cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_workspace_import(
    const char *target_path, const char *archive_directory, bool replace,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_health(
    const char *path, const char *workspace_id,
    cbm_zova_health_class_t *out_class,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_workspace_recover(
    const char *path, const char *workspace_id, const char *repo_path,
    cbm_zova_operation_report_t *out);
cbm_zova_operation_code_t cbm_zova_database_repack(
    const cbm_zova_repack_request_t *request,
    cbm_zova_repack_report_t *out_report);

#endif
