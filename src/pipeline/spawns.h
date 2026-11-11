/*
 * spawns.h — SPAWNS edges: a call that starts another program, to the
 * Process node of that program. Implemented in src/pipeline/spawns.zig.
 */
#ifndef CBM_PIPELINE_SPAWNS_H
#define CBM_PIPELINE_SPAWNS_H

#include "cbm.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pipeline.h"
#include "spawn_patterns.h"

#include <stdbool.h>

typedef struct {
    const char *api;
    char program[CBM_SPAWN_PROGRAM_MAX];
} cbm_pipeline_spawn_t;

bool cbm_pipeline_spawn_site(const cbm_gbuf_t *gbuf, CBMLanguage lang, const CBMCall *call,
                             const CBMImportArray *imports, const cbm_resolution_t *res,
                             cbm_pipeline_spawn_t *out);

void cbm_pipeline_emit_spawn(cbm_gbuf_t *gbuf, const cbm_gbuf_node_t *source, const CBMCall *call,
                             const cbm_pipeline_spawn_t *spawn);

#endif /* CBM_PIPELINE_SPAWNS_H */
