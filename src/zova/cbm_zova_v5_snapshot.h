#ifndef CBM_ZOVA_V5_SNAPSHOT_H
#define CBM_ZOVA_V5_SNAPSHOT_H

#include "zova/cbm_zova.h"

#include <stdint.h>

typedef struct cbm_zova_v5_snapshot cbm_zova_v5_snapshot_t;

int cbm_zova_v5_snapshot_open(const char *source_path,
                              cbm_zova_v5_snapshot_t **out_snapshot);
int cbm_zova_v5_snapshot_workspace_count(const cbm_zova_v5_snapshot_t *snapshot);
const cbm_zova_workspace_generation_input_t *cbm_zova_v5_snapshot_input_at(
    const cbm_zova_v5_snapshot_t *snapshot, int index, int64_t *out_active_generation);
void cbm_zova_v5_snapshot_close(cbm_zova_v5_snapshot_t *snapshot);

#endif
