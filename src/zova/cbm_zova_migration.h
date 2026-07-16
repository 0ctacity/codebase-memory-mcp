#ifndef CBM_ZOVA_MIGRATION_H
#define CBM_ZOVA_MIGRATION_H

#include "zova/cbm_zova.h"
#include "foundation/sha256.h"

#include <stdbool.h>
#include <stdint.h>

#define CBM_ZOVA_MIGRATION_VERSION 1

typedef struct zova_database zova_database;
typedef struct cbm_zova_legacy_snapshot cbm_zova_legacy_snapshot_t;

typedef enum {
    CBM_ZOVA_MIGRATION_PREPARED,
    CBM_ZOVA_MIGRATION_COPYING,
    CBM_ZOVA_MIGRATION_ACTIVE,
    CBM_ZOVA_MIGRATION_FAILED,
    CBM_ZOVA_MIGRATION_ROLLED_BACK,
    CBM_ZOVA_MIGRATION_CLEANUP_PENDING,
    CBM_ZOVA_MIGRATION_RETIRED,
} cbm_zova_migration_state_t;

typedef enum {
    CBM_ZOVA_MIGRATION_OK = 0,
    CBM_ZOVA_MIGRATION_NOOP = 1,
    CBM_ZOVA_MIGRATION_INVALID = -1,
    CBM_ZOVA_MIGRATION_SOURCE_MISSING = -2,
    CBM_ZOVA_MIGRATION_SOURCE_NOT_READY = -3,
    CBM_ZOVA_MIGRATION_SOURCE_INCOMPATIBLE = -4,
    CBM_ZOVA_MIGRATION_TARGET_INCOMPATIBLE = -5,
    CBM_ZOVA_MIGRATION_VERIFY_FAILED = -6,
    CBM_ZOVA_MIGRATION_ROLLBACK_UNAVAILABLE = -7,
    CBM_ZOVA_MIGRATION_CLEANUP_REFUSED = -8,
} cbm_zova_migration_code_t;

typedef struct {
    int64_t source_node_id;
    const char *name;
    const char *qualified_name;
    const char *label;
    const char *file_path;
} cbm_zova_migration_fts_row_t;

typedef struct {
    uint64_t workspace_count;
    uint64_t stable_id_count;
    uint64_t graph_node_count;
    uint64_t graph_edge_count;
    uint64_t node_vector_count;
    uint64_t token_vector_count;
    uint64_t fts_query_count;
    char metadata_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char fts_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char topology_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char node_vector_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
    char token_vector_sha256[CBM_ZOVA_DIGEST_HEX_SIZE];
} cbm_zova_migration_manifest_t;

typedef struct {
    const char *source_db_path;
    const char *source_zova_path;
    const char *target_zova_path;
    const char *canonical_root;
} cbm_zova_migration_request_t;

typedef struct {
    cbm_zova_migration_code_t code;
    cbm_zova_migration_state_t state;
    bool no_op;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    int64_t source_generation;
    int64_t target_generation;
    cbm_zova_migration_manifest_t source;
    cbm_zova_migration_manifest_t target;
} cbm_zova_migration_report_t;

cbm_zova_migration_code_t cbm_zova_migration_run(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report);
cbm_zova_migration_code_t cbm_zova_migration_status(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report);
cbm_zova_migration_code_t cbm_zova_migration_rollback(
    const cbm_zova_migration_request_t *request, cbm_zova_migration_report_t *out_report);
cbm_zova_migration_code_t cbm_zova_migration_cleanup(
    const cbm_zova_migration_request_t *request, const char *confirmed_workspace_id,
    cbm_zova_migration_report_t *out_report);

int cbm_zova_migration_manifest_target_tx(zova_database *db, const char *workspace_id,
                                          int64_t generation,
                                          const cbm_zova_legacy_snapshot_t *source,
                                          cbm_zova_migration_manifest_t *out_manifest);
void cbm_zova_migration_digest_text(cbm_sha256_ctx *hash, const char *text);
void cbm_zova_migration_digest_finalize(cbm_sha256_ctx *hash,
                                        char out[CBM_ZOVA_DIGEST_HEX_SIZE]);
bool cbm_zova_migration_test_fault(const char *phase);

bool cbm_zova_migration_transition_allowed(cbm_zova_migration_state_t from,
                                           cbm_zova_migration_state_t to);
const char *cbm_zova_migration_state_name(cbm_zova_migration_state_t state);

#endif
