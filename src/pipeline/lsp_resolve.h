/*
 * lsp_resolve.h — Shared LSP-override resolver for the call-edge pipeline.
 *
 * Both pipeline paths (sequential cbm_pipeline_pass_calls and parallel
 * cbm_parallel_extract → resolve_file_calls) need to look up an
 * LSP-resolved call for a given (caller, callee) pair before falling back
 * to the registry's name-based resolver. Before this header existed, each
 * pipeline carried its own copy of that lookup with divergent confidence
 * floors and slightly different match semantics — most production
 * indexing went through the parallel path with a 0.5 floor while the
 * sequential path used 0.6, so the same project produced different
 * CALLS-edge attributions depending on which pipeline mode kicked in.
 *
 * Centralising the lookup here means both pipelines admit exactly the
 * same set of LSP overrides. Each pipeline still owns its own edge
 * emission (sequential uses emit_classified_edge, parallel uses
 * emit_service_edge) — this header only does the matching.
 *
 * Inline-only: no .c file needed.
 */
#ifndef CBM_PIPELINE_LSP_RESOLVE_H
#define CBM_PIPELINE_LSP_RESOLVE_H

#include "cbm.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Confidence floor below which LSP-resolved calls are ignored and the
 * registry resolver is consulted instead. Locked at 0.6 per the v1
 * Python-LSP integration plan; revisit when telemetry justifies a knob.
 * Applies to every language whose LSP populates result->resolved_calls
 * (Go, C/C++, Python, PHP). */
#define CBM_LSP_CONFIDENCE_FLOOR 0.6f

/* Bare last segment of a (possibly qualified) name, splitting on the LAST
 * member/scope separator. C++ textual callees carry `::` (Class::method,
 * Ns::f) and `->` (p->run), while the LSP records dotted internal QNs
 * (Class.method). Splitting only on '.' (strrchr) leaves `Math::square`
 * and `p->run` intact, so they never match the LSP's `square`/`run` short
 * name and the type-aware strategy is silently dropped to the textual
 * registry. Treat '.', ':' and '>' as terminal separators so the bare
 * method name is recovered on BOTH the QN side (dotted, occasionally `::`
 * for template/alias scopes) and the textual side (`.`/`::`/`->`). Other
 * languages' callee names contain none of `::`/`->`, so this is a no-op
 * for them. */
static inline const char *cbm_lsp_bare_segment(const char *name) {
    if (!name) {
        return name;
    }
    const char *seg = name;
    for (const char *p = name; *p; p++) {
        /* '.' (dotted QN / Java-style member) and ':' (C++ `::`, last colon
         * wins) are member/scope separators. '>' is only a separator when it
         * closes the `->` arrow (preceded by '-'); a bare '>' closes a template
         * argument list ("identity<int>") and must NOT split, else the segment
         * would be the empty string after the trailing '>'. */
        if (*p == '.' || *p == ':' || (*p == '>' && p != name && p[-1] == '-')) {
            seg = p + SKIP_ONE;
        }
    }
    return seg;
}

/* Tail helper: return the start of the final two dot-separated segments
 * ("Class.method") or NULL when the QN is too short. */
static inline const char *cbm_pipeline_qn_class_method_tail(const char *qn) {
    if (!qn) {
        return NULL;
    }
    const char *last = strrchr(qn, '.');
    if (!last || last == qn) {
        return NULL;
    }
    const char *second = last;
    while (second > qn) {
        second--;
        if (*second == '.') {
            if (second == qn) {
                return qn;
            }
            return second + 1;
        }
    }
    return qn;
}

static inline const char *cbm_pipeline_call_callee_leaf(const char *callee_name) {
    return cbm_lsp_bare_segment(callee_name);
}

/* Gate for the unique-`Class.method`-tail fallbacks below. Tail-matching by
 * leaf is safe where class-per-file package semantics hold — the JVM
 * languages (Java/Kotlin): the declared `package` is ground truth, a class
 * name is unique within a package, and mixed Gradle/Maven source roots
 * (`src/main/java` + `src/main/kotlin`) legitimately produce path-derived
 * module QNs that disagree with the package-shaped QNs the LSP emits, so
 * the tail is the only reliable join key. In other languages the same-name
 * guarantee does not exist (Python/TS re-export shims, Go internal clones,
 * C++ template instantiations), and a single wrong-module coincidence
 * would fabricate a CALLS edge — so the fallbacks stay off there. */
static inline bool cbm_pipeline_lsp_allow_tail_match(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_KOTLIN;
}

static inline int cbm_pipeline_qn_class_method_tail_eq(const char *qn, const char *tail) {
    const char *qt = cbm_pipeline_qn_class_method_tail(qn);
    return qt && tail && strcmp(qt, tail) == 0;
}

/* An invocation row that carries a site span (end > start) proves ONE call: it
 * joins only the call whose node starts at that byte, so `db.deinit()` and
 * `st.deinit()` in one function each keep their own target. Rows without a span
 * join every same-named call in the caller, as before. */
static inline bool cbm_pipeline_lsp_site_admits(const CBMResolvedCall *rc, const CBMCall *call) {
    return rc->site_end_byte <= rc->site_start_byte ||
           rc->site_start_byte == call->site_start_byte;
}

/* Look up the highest-confidence LSP-resolved call entry whose caller QN
 * matches the textual call's enclosing function and whose callee QN
 * short-name matches the textual callee. Returns a pointer into `arr`
 * or NULL if no qualifying entry exists.
 *
 * Match rule:
 *   1. exact caller_qn + callee short-name match wins first;
 *   2. if no exact caller match exists AND allow_tail_match is set
 *      (JVM callers only, see cbm_pipeline_lsp_allow_tail_match), a
 *      unique Class.method tail match between rc->caller_qn and
 *      call->enclosing_func_qn may win;
 *   3. ambiguous tails return NULL so the registry fallback stays in
 *      control.
 *
 * The pointer returned aliases into `arr` and stays valid as long as the
 * underlying CBMFileResult is alive. */
static inline const CBMResolvedCall *cbm_pipeline_find_lsp_resolution(
    const CBMResolvedCallArray *arr, const CBMCall *call, bool allow_tail_match) {
    if (!arr || arr->count == 0 || !call) {
        return NULL;
    }
    if (!call->enclosing_func_qn || !call->callee_name) {
        return NULL;
    }

    const CBMResolvedCall *best_exact = NULL;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn || rc->kind != CBM_RESOLVED_INVOCATION) {
            continue;
        }
        if (rc->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        if (strcmp(rc->caller_qn, call->enclosing_func_qn) != 0) {
            continue;
        }
        if (!cbm_pipeline_lsp_site_admits(rc, call)) {
            continue;
        }
        const char *short_name = cbm_lsp_bare_segment(rc->callee_qn);
        /* The call's callee_name is receiver-qualified for method/qualified
         * calls ("c.inc", "A.Helper", "Math::square", "p->run"); the LSP
         * records the resolved class-qualified callee_qn ("Class.inc"). Compare
         * the bare last segment on BOTH sides so method-dispatch resolutions
         * join — the LSP already did the receiver->type resolution, and matching
         * the full "c.inc" against "inc" would always miss, silently dropping the
         * type-aware LSP strategy to the weaker textual registry. Free-function
         * calls (bare callee_name) are unaffected. */
        const char *call_short = cbm_lsp_bare_segment(call->callee_name);
        if (strcmp(short_name, call_short) != 0) {
            /* Indirect/implicit resolution: the textual callee differs from the
             * resolved callee_qn's short name. A function-pointer / DLL call's
             * callee is the pointer name (`fp`); a C++ destructor's only textual
             * anchor is the deleted operand (`p`, vs. the `T.~T` callee QN). In
             * both the LSP stashed the original textual name in `reason`. Match
             * the call site on that name, gated to those strategies so `reason`
             * is never misread as an unresolved-call diagnostic. */
            if (!(rc->reason && rc->strategy &&
                  (strcmp(rc->strategy, "lsp_func_ptr") == 0 ||
                   strcmp(rc->strategy, "lsp_dll_resolve") == 0 ||
                   strcmp(rc->strategy, "lsp_dict_dispatch") == 0 ||
                   strcmp(rc->strategy, "lsp_destructor") == 0 ||
                   strcmp(rc->strategy, "php_method_dynamic") == 0) &&
                  strcmp(cbm_lsp_bare_segment(rc->reason), call_short) == 0)) {
                continue;
            }
        }
        if (!best_exact || rc->confidence > best_exact->confidence) {
            best_exact = rc;
        }
    }
    if (best_exact) {
        return best_exact;
    }
    if (!allow_tail_match) {
        return NULL;
    }

    const char *call_tail = cbm_pipeline_qn_class_method_tail(call->enclosing_func_qn);
    if (!call_tail) {
        return NULL;
    }

    const CBMResolvedCall *best_tail = NULL;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn || rc->kind != CBM_RESOLVED_INVOCATION) {
            continue;
        }
        if (rc->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        const char *short_name = strrchr(rc->callee_qn, '.');
        short_name = short_name ? short_name + SKIP_ONE : rc->callee_qn;
        const char *call_leaf = cbm_pipeline_call_callee_leaf(call->callee_name);
        if (!call_leaf || strcmp(short_name, call_leaf) != 0) {
            continue;
        }
        if (!cbm_pipeline_qn_class_method_tail_eq(rc->caller_qn, call_tail)) {
            continue;
        }
        if (!cbm_pipeline_lsp_site_admits(rc, call)) {
            continue;
        }
        if (best_tail) {
            return NULL;
        }
        best_tail = rc;
    }
    return best_tail;
}

/* Tail-match cost counters (upstream f6e3af43). The tail scan in
 * cbm_pipeline_lsp_target_node has no candidate cap and its candidate set
 * grows with the corpus, so candidates per lookup is what would turn resolve
 * superlinear. Counted always (two relaxed adds), defined in pass_parallel.c
 * and logged at the end of parallel resolve as parallel.resolve.scan_cost. */
extern _Atomic uint64_t g_lsp_tail_lookups;
extern _Atomic uint64_t g_lsp_tail_candidates;

/* Resolve an LSP-emitted callee_qn to a graph-buffer node.
 *
 * Per-file LSPs sometimes emit `callee_qn` as the raw package-shaped
 * import path the source code uses rather than the project-qualified QN
 * the gbuf actually stores. The fallback rule is:
 *   1. try the LSP-emitted QN as-is;
 *   2. retry with `<project>.<callee_qn>` when needed;
 *   3. if both fail AND allow_tail_match is set (JVM callers only, see
 *      cbm_pipeline_lsp_allow_tail_match), use the exact node-name index
 *      to narrow candidates by short method name and accept exactly one
 *      Function/Method whose qualified_name has the same Class.method
 *      tail.
 *
 * Returns the matching node, or NULL if neither lookup hits. */
static inline const cbm_gbuf_node_t *cbm_pipeline_lsp_target_node(const cbm_gbuf_t *gbuf,
                                                                  const char *project_name,
                                                                  const char *callee_qn,
                                                                  bool allow_tail_match) {
    if (!gbuf || !callee_qn) {
        return NULL;
    }
    const cbm_gbuf_node_t *direct = cbm_gbuf_find_by_qn(gbuf, callee_qn);
    if (direct) {
        return direct;
    }
    /* Exact retries, in tie-rule order (CBM_MACRO_QN_SUFFIX): the definition
     * first -- the QN as given, then with the project prefix -- and only when
     * neither names a node the C-preprocessor macro of that name, in the same
     * two spellings. The macro case is a prototype the C resolver knows whose
     * only node in the graph is its rename macro (`extern T f(...);` plus
     * `#define f f_v2`). One buffer, "<project>.<callee_qn>#macro", serves all
     * three retries. */
    size_t proj_len = (project_name && project_name[0]) ? strlen(project_name) : 0;
    size_t callee_len = strlen(callee_qn);
    size_t suffix_len = sizeof(CBM_MACRO_QN_SUFFIX) - 1U;
    bool add_prefix = proj_len > 0 && !(strncmp(callee_qn, project_name, proj_len) == 0 &&
                                        callee_qn[proj_len] == '.');
    if (proj_len <= SIZE_MAX - callee_len - suffix_len - 2U) {
        char *buf = (char *)malloc(proj_len + 1U + callee_len + suffix_len + 1U);
        if (buf) {
            char *own = buf + proj_len + 1U; /* callee_qn inside the buffer */
            if (proj_len > 0) {
                memcpy(buf, project_name, proj_len);
            }
            buf[proj_len] = '.';
            memcpy(own, callee_qn, callee_len + 1U);
            const cbm_gbuf_node_t *hit = add_prefix ? cbm_gbuf_find_by_qn(gbuf, buf) : NULL;
            if (!hit) {
                memcpy(own + callee_len, CBM_MACRO_QN_SUFFIX, suffix_len + 1U);
                hit = cbm_gbuf_find_by_qn(gbuf, own);
                if (!hit && add_prefix) {
                    hit = cbm_gbuf_find_by_qn(gbuf, buf);
                }
            }
            free(buf);
            if (hit) {
                return hit;
            }
        }
    }
    if (!allow_tail_match) {
        return NULL;
    }

    const char *short_name = strrchr(callee_qn, '.');
    short_name = short_name ? short_name + SKIP_ONE : callee_qn;
    const char *callee_tail = cbm_pipeline_qn_class_method_tail(callee_qn);
    if (!callee_tail) {
        return NULL;
    }
    const cbm_gbuf_node_t **hits = NULL;
    int hit_count = 0;
    if (cbm_gbuf_find_by_name(gbuf, short_name, &hits, &hit_count) != 0 || hit_count == 0) {
        return NULL;
    }
    atomic_fetch_add_explicit(&g_lsp_tail_lookups, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lsp_tail_candidates, (uint64_t)hit_count, memory_order_relaxed);

    const cbm_gbuf_node_t *match = NULL;
    for (int i = 0; i < hit_count; i++) {
        const cbm_gbuf_node_t *cand = hits[i];
        if (!cand || !cand->label || !cand->qualified_name) {
            continue;
        }
        if (strcmp(cand->label, "Function") != 0 && strcmp(cand->label, "Method") != 0) {
            continue;
        }
        if (!cbm_pipeline_qn_class_method_tail_eq(cand->qualified_name, callee_tail)) {
            continue;
        }
        if (match) {
            return NULL;
        }
        match = cand;
    }
    return match;
}

/* ── Callable references (CALL_REFERENCE) ─────────────────────────────────
 *
 * A function or method named as a value is a CALL_REFERENCE only when the
 * language LSP proved one exact callable at that source occurrence. The
 * semantic join is by (enclosing function, exact byte span of the reference
 * token); the textual registry never earns CALL_REFERENCE, it only yields an
 * ordinary USAGE. Reference rows never take part in the CALLS join. */

/* Extraction marked this usage as a callable-reference candidate: explicit
 * reference syntax, or a bare value in a narrow position (direct argument,
 * assignment right-hand side) the language LSP knows how to prove. */
static inline bool cbm_pipeline_usage_is_reference_candidate(const CBMUsage *usage) {
    return usage && (usage->kind == CBM_USAGE_CALL_REFERENCE || usage->may_be_call_reference) &&
           usage->site_end_byte > usage->site_start_byte;
}

/* Graph labels a CALL_REFERENCE may point at (a Class covers constructor
 * references such as Java `Foo::new`). Anything else (a field, a variable, an
 * interface) proved by the LSP still lands as USAGE. */
static inline bool cbm_pipeline_node_is_callable_target(const cbm_gbuf_node_t *node) {
    if (!node || !node->label) {
        return false;
    }
    return strcmp(node->label, "Function") == 0 || strcmp(node->label, "Method") == 0 ||
           strcmp(node->label, "Constructor") == 0 || strcmp(node->label, "Class") == 0;
}

/* Per-file index of CALL_REFERENCE rows sorted by site, so each candidate
 * usage costs one binary search instead of a scan of every resolved row. */
typedef struct {
    const CBMResolvedCall **rows;
    int count;
} cbm_pipeline_ref_index_t;

static inline int cbm_pipeline_ref_row_cmp(const void *a, const void *b) {
    const CBMResolvedCall *l = *(const CBMResolvedCall *const *)a;
    const CBMResolvedCall *r = *(const CBMResolvedCall *const *)b;
    if (l->site_start_byte != r->site_start_byte) {
        return l->site_start_byte < r->site_start_byte ? -1 : 1;
    }
    if (l->site_end_byte != r->site_end_byte) {
        return l->site_end_byte < r->site_end_byte ? -1 : 1;
    }
    /* Stable order within one site: confidence ties keep the first row. */
    return l < r ? -1 : (l > r ? 1 : 0);
}

/* Returns false only on allocation failure; the caller then finds no semantic
 * row and every candidate stays USAGE (fail closed). */
static inline bool cbm_pipeline_ref_index_build(const CBMResolvedCallArray *arr,
                                                cbm_pipeline_ref_index_t *index) {
    index->rows = NULL;
    index->count = 0;
    if (!arr || arr->count <= 0) {
        return true;
    }
    int n = 0;
    for (int i = 0; i < arr->count; i++) {
        if (arr->items[i].kind == CBM_RESOLVED_CALL_REFERENCE) {
            n++;
        }
    }
    if (n == 0) {
        return true;
    }
    const CBMResolvedCall **rows = (const CBMResolvedCall **)malloc((size_t)n * sizeof(*rows));
    if (!rows) {
        return false;
    }
    int k = 0;
    for (int i = 0; i < arr->count; i++) {
        if (arr->items[i].kind == CBM_RESOLVED_CALL_REFERENCE) {
            rows[k++] = &arr->items[i];
        }
    }
    qsort(rows, (size_t)n, sizeof(*rows), cbm_pipeline_ref_row_cmp);
    index->rows = rows;
    index->count = n;
    return true;
}

static inline void cbm_pipeline_ref_index_free(cbm_pipeline_ref_index_t *index) {
    free(index->rows);
    index->rows = NULL;
    index->count = 0;
}

/* Scan the rows sharing the usage's byte span. `tail` NULL requires the exact
 * enclosing function; otherwise the caller's Class.method tail must match. */
static inline const CBMResolvedCall *cbm_pipeline_ref_scan(const cbm_pipeline_ref_index_t *index,
                                                           int first, const CBMUsage *usage,
                                                           const char *tail) {
    const char *ref_leaf = cbm_lsp_bare_segment(usage->ref_name);
    const CBMResolvedCall *best = NULL;
    for (int i = first; i < index->count; i++) {
        const CBMResolvedCall *r = index->rows[i];
        if (r->site_start_byte != usage->site_start_byte ||
            r->site_end_byte != usage->site_end_byte) {
            break;
        }
        if (!r->caller_qn || !r->callee_qn || r->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        if (tail ? !cbm_pipeline_qn_class_method_tail_eq(r->caller_qn, tail)
                 : strcmp(r->caller_qn, usage->enclosing_func_qn) != 0) {
            continue;
        }
        /* The row must name this reference: the callee leaf, or `reason` for an
         * alias (`cb` proven to hold `handler`). */
        if (strcmp(cbm_lsp_bare_segment(r->callee_qn), ref_leaf) != 0 &&
            !(r->reason && strcmp(cbm_lsp_bare_segment(r->reason), ref_leaf) == 0)) {
            continue;
        }
        if (!best) {
            best = r;
        } else if (strcmp(best->callee_qn, r->callee_qn) != 0) {
            return NULL; /* two targets at one occurrence: ambiguous, fail closed */
        } else if (r->confidence > best->confidence) {
            best = r;
        }
    }
    return best;
}

/* The exact semantic row for one candidate usage, or NULL. Rows must share the
 * usage's byte span and enclosing function and clear the confidence floor.
 * JVM callers (allow_tail_match) may fall back to a Class.method tail match on
 * the caller, the same rule the CALLS join uses for package-shaped LSP QNs. */
static inline const CBMResolvedCall *cbm_pipeline_find_lsp_reference(
    const cbm_pipeline_ref_index_t *index, const CBMUsage *usage, bool allow_tail_match) {
    if (!index || index->count == 0 || !cbm_pipeline_usage_is_reference_candidate(usage) ||
        !usage->enclosing_func_qn || !usage->ref_name) {
        return NULL;
    }
    int lo = 0;
    int hi = index->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        const CBMResolvedCall *r = index->rows[mid];
        if (r->site_start_byte < usage->site_start_byte ||
            (r->site_start_byte == usage->site_start_byte &&
             r->site_end_byte < usage->site_end_byte)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const CBMResolvedCall *exact = cbm_pipeline_ref_scan(index, lo, usage, NULL);
    if (exact || !allow_tail_match) {
        return exact;
    }
    const char *tail = cbm_pipeline_qn_class_method_tail(usage->enclosing_func_qn);
    return tail ? cbm_pipeline_ref_scan(index, lo, usage, tail) : NULL;
}

/* Rows that take part in the CALLS join. Count-based heuristics that compare
 * LSP output with textual calls must not count reference rows, nor the
 * confidence-0 lsp_unresolved diagnostics the join ignores. */
static inline int cbm_pipeline_invocation_row_count(const CBMResolvedCallArray *arr) {
    int n = 0;
    for (int i = 0; arr && i < arr->count; i++) {
        if (arr->items[i].kind == CBM_RESOLVED_INVOCATION &&
            arr->items[i].confidence >= CBM_LSP_CONFIDENCE_FLOOR) {
            n++;
        }
    }
    return n;
}

/* Rows that take part in the CALL_REFERENCE join. */
static inline int cbm_pipeline_reference_row_count(const CBMResolvedCallArray *arr) {
    int n = 0;
    for (int i = 0; arr && i < arr->count; i++) {
        n += arr->items[i].kind == CBM_RESOLVED_CALL_REFERENCE ? 1 : 0;
    }
    return n;
}

/* Outcome of the exact semantic join for one usage. */
typedef enum {
    CBM_REF_JOIN_NONE = 0, /* no LSP row at this occurrence: registry fallback decides */
    CBM_REF_JOIN_CALLABLE, /* proven callable target: emit CALL_REFERENCE */
    CBM_REF_JOIN_VALUE,    /* proven non-callable target: at most a USAGE */
    CBM_REF_JOIN_SKIP,     /* a row owns the occurrence but its target is not in the
                            * graph: emit nothing rather than guess by name */
} cbm_ref_join_t;

static inline cbm_ref_join_t cbm_pipeline_join_lsp_reference(
    const cbm_gbuf_t *gbuf, const char *project_name, const cbm_pipeline_ref_index_t *index,
    const CBMUsage *usage, bool allow_tail_match, const cbm_gbuf_node_t **out_target,
    const CBMResolvedCall **out_row) {
    *out_target = NULL;
    *out_row = NULL;
    const CBMResolvedCall *row = cbm_pipeline_find_lsp_reference(index, usage, allow_tail_match);
    if (!row) {
        return CBM_REF_JOIN_NONE;
    }
    *out_row = row;
    const cbm_gbuf_node_t *tgt =
        cbm_pipeline_lsp_target_node(gbuf, project_name, row->callee_qn, allow_tail_match);
    if (!tgt) {
        return CBM_REF_JOIN_SKIP;
    }
    *out_target = tgt;
    return cbm_pipeline_node_is_callable_target(tgt) ? CBM_REF_JOIN_CALLABLE : CBM_REF_JOIN_VALUE;
}

/* Edge properties for a USAGE / CALL_REFERENCE edge. A CALL_REFERENCE always
 * comes from an LSP row (row non-NULL), whose strategy and confidence are the
 * edge's provenance, in the same keys CALLS edges use; USAGE stays
 * {"callee":...}. esc_ref is already JSON-escaped. */
static inline void cbm_pipeline_reference_props(char *buf, size_t size, const char *esc_ref,
                                                const CBMResolvedCall *row) {
    if (row && row->strategy && row->strategy[0]) {
        snprintf(buf, size, "{\"callee\":\"%s\",\"confidence\":%.2f,\"strategy\":\"%s\"}",
                 esc_ref, row->confidence, row->strategy);
    } else {
        snprintf(buf, size, "{\"callee\":\"%s\"}", esc_ref);
    }
}

/* Registry fallback for a usage the LSP did not prove. A candidate emitted
 * only for the join (reference_only) never falls back. Explicit JVM reference
 * syntax may not bind by a bare-name guess (unique_name / suffix_match),
 * except a Kotlin `::name` whose spelling the file imports. */
static inline bool cbm_pipeline_reference_fallback_allowed(CBMLanguage lang, const CBMUsage *usage,
                                                           const char *strategy,
                                                           const char *const *import_names,
                                                           int import_count) {
    if (usage->reference_only) {
        return false;
    }
    if (usage->kind != CBM_USAGE_CALL_REFERENCE ||
        (lang != CBM_LANG_JAVA && lang != CBM_LANG_KOTLIN)) {
        return true;
    }
    if (strategy && strategy[0] && strcmp(strategy, "unique_name") != 0 &&
        strcmp(strategy, "suffix_match") != 0) {
        return true;
    }
    if (lang == CBM_LANG_KOTLIN && usage->ref_name && import_names) {
        for (int i = 0; i < import_count; i++) {
            if (import_names[i] && strcmp(import_names[i], usage->ref_name) == 0) {
                return true;
            }
        }
    }
    return false;
}

#endif /* CBM_PIPELINE_LSP_RESOLVE_H */
