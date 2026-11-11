/*
 * lsp_resolve.h — Shared LSP-override resolver for the call-edge pipeline.
 *
 * Implemented in src/pipeline/lsp_resolve.zig. The call resolver
 * (pass_parallel.zig) and the tests look up an LSP-resolved call for a given
 * (caller, callee) pair here before falling back to the registry's name-based
 * resolver, so every caller admits the same set of LSP overrides.
 */
#ifndef CBM_PIPELINE_LSP_RESOLVE_H
#define CBM_PIPELINE_LSP_RESOLVE_H

#include "cbm.h"
#include "graph_buffer/graph_buffer.h"

#include <stdbool.h>

/* Confidence floor below which LSP-resolved calls are ignored and the
 * registry resolver is consulted instead. Locked at 0.6 per the v1
 * Python-LSP integration plan; revisit when telemetry justifies a knob.
 * Applies to every language whose LSP populates result->resolved_calls
 * (Go, C/C++, Python, PHP). */
#define CBM_LSP_CONFIDENCE_FLOOR 0.6f

/* True for the languages whose LSP joins may fall back to a unique
 * `Class.method` tail match (Java and Kotlin). */
bool cbm_pipeline_lsp_allow_tail_match(CBMLanguage lang);

/* The highest-confidence LSP-resolved call in `arr` for the textual `call`,
 * or NULL. The pointer aliases into `arr`. */
const CBMResolvedCall *cbm_pipeline_find_lsp_resolution(const CBMResolvedCallArray *arr,
                                                        const CBMCall *call,
                                                        bool allow_tail_match);

/* The graph-buffer node an LSP-emitted callee_qn names, or NULL. */
const cbm_gbuf_node_t *cbm_pipeline_lsp_target_node(const cbm_gbuf_t *gbuf,
                                                    const char *project_name,
                                                    const char *callee_qn,
                                                    bool allow_tail_match);

#endif /* CBM_PIPELINE_LSP_RESOLVE_H */
