#ifndef CBM_ZOVA_LEGACY_SNAPSHOT_H
#define CBM_ZOVA_LEGACY_SNAPSHOT_H

#include "zova/cbm_zova.h"
#include "zova/cbm_zova_migration.h"

#include <stdint.h>

typedef struct cbm_zova_legacy_snapshot cbm_zova_legacy_snapshot_t;

cbm_zova_migration_code_t cbm_zova_legacy_snapshot_open(
    const char *source_db_path, const char *source_zova_path, const char *canonical_root,
    cbm_zova_legacy_snapshot_t **out_snapshot);

const cbm_zova_workspace_generation_input_t *
cbm_zova_legacy_snapshot_input(const cbm_zova_legacy_snapshot_t *snapshot);
int64_t cbm_zova_legacy_snapshot_source_generation(
    const cbm_zova_legacy_snapshot_t *snapshot);
const cbm_zova_migration_fts_row_t *cbm_zova_legacy_snapshot_fts_rows(
    const cbm_zova_legacy_snapshot_t *snapshot, int *out_count);
const cbm_zova_migration_manifest_t *cbm_zova_legacy_snapshot_manifest(
    const cbm_zova_legacy_snapshot_t *snapshot);
const char *cbm_zova_legacy_snapshot_workspace_id(const cbm_zova_legacy_snapshot_t *snapshot);
const char *cbm_zova_legacy_snapshot_target_id(const cbm_zova_legacy_snapshot_t *snapshot,
                                               int node_index);
const char *const *cbm_zova_legacy_snapshot_fts_queries(
    const cbm_zova_legacy_snapshot_t *snapshot, int *out_count);

/* Shared by source and target validation for the fixed unicode61 schema. */
int cbm_zova_migration_fts_value_may_tokenize(const char *value);

void cbm_zova_legacy_snapshot_close(cbm_zova_legacy_snapshot_t *snapshot);

#endif
