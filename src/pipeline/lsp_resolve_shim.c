/*
 * lsp_resolve_shim.c — cbm_pipeline_lsp_target_node as a linkable function.
 *
 * lsp_resolve.h defines its helpers as static inline, and the Zig modules
 * read them through translate-c. This one counts its tail-match lookups with
 * C11 atomics, which translate-c cannot express, so the Zig side sees only its
 * declaration. This file gives that declaration a definition by compiling the
 * header's own body, so lsp_resolve.h stays the single source of the logic
 * for the C code that includes it (internal/cbm/lsp, the tests).
 */
#define cbm_pipeline_lsp_target_node cbm_pipeline_lsp_target_node_inline
#include "pipeline/lsp_resolve.h"
#undef cbm_pipeline_lsp_target_node

const cbm_gbuf_node_t *cbm_pipeline_lsp_target_node(const cbm_gbuf_t *gbuf, const char *project,
                                                    const char *callee_qn, bool allow_tail_match);

const cbm_gbuf_node_t *cbm_pipeline_lsp_target_node(const cbm_gbuf_t *gbuf, const char *project,
                                                    const char *callee_qn, bool allow_tail_match) {
    return cbm_pipeline_lsp_target_node_inline(gbuf, project, callee_qn, allow_tail_match);
}
