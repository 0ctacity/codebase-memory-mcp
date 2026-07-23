/*
 * pass_complexity.c — Interprocedural complexity propagation (Tier B).
 *
 * Tier A (in the extraction walk) stamps each Function/Method node with local
 * structural metrics: complexity (cyclomatic), cognitive, loop_count, loop_depth.
 * This pass propagates loop_depth along CALLS edges to estimate a worst-case
 * *transitive* nested-loop degree: a function with a depth-1 loop that calls an
 * O(n) helper is effectively O(n^2). The estimate assumes calls may occur inside
 * loops (an upper bound) — it is a queryable bottleneck *candidate* signal, not a
 * proof (true big-O is undecidable; cf. SPEED / Loopus). Cycles in the call graph
 * are broken and flagged via a `recursive` property.
 *
 * Writes two extra node properties: transitive_loop_depth, recursive.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "cbm.h"
#include "yyjson/yyjson.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>

enum { CBM_TLD_MAX_DEPTH = 256 }; /* recursion-depth cap (cycle/stack guard) */

/* Int → string for structured logging (thread-safe ring buffer). */
static const char *itoa_cx(int val) {
    enum { RING = 2, MASK = 1 };
    static CBM_TLS char bufs[RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + 1) & MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* Parse an integer "key":N from a flat JSON object. Returns def if absent. */
static int json_get_int(const char *json, const char *key, int dflt) {
    if (!json) {
        return dflt;
    }
    char pat[CBM_SZ_64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return dflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return (int)strtol(p, NULL, CBM_DECIMAL_BASE);
}

/* Parse a boolean "key":true/false from a flat JSON object. */
static bool json_get_bool(const char *json, const char *key) {
    if (!json) {
        return false;
    }
    char pat[CBM_SZ_64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p == 't';
}

/* Replace the derived complexity suffix when a graph buffer was loaded from a
 * prior index, then append the current values.  Full indexing calls this once;
 * incremental indexing recomputes it over a mixed old/new graph, so blindly
 * appending would leave duplicate JSON keys and stale values. */
static void append_complexity_props(cbm_gbuf_node_t *node, int tld, bool recursive) {
    const char *old = node->properties_json ? node->properties_json : "{}";
    yyjson_doc *doc = yyjson_read(old, strlen(old), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        yyjson_doc_free(doc);
        return;
    }
    yyjson_mut_val *mroot = yyjson_val_mut_copy(mdoc, root);
    if (!mroot) {
        yyjson_mut_doc_free(mdoc);
        yyjson_doc_free(doc);
        return;
    }
    yyjson_mut_doc_set_root(mdoc, mroot);
    /* Loaded incremental nodes may contain these fields before later-derived
     * fields such as decorator_tags, and older generations may contain more
     * than one copy. yyjson removes every matching key before current values
     * are added exactly once. */
    yyjson_mut_obj_remove_key(mroot, "transitive_loop_depth");
    yyjson_mut_obj_remove_key(mroot, "recursive");
    yyjson_mut_obj_add_int(mdoc, mroot, "transitive_loop_depth", tld);
    yyjson_mut_obj_add_bool(mdoc, mroot, "recursive", recursive);
    char *neu = yyjson_mut_write(mdoc, 0, NULL);
    yyjson_mut_doc_free(mdoc);
    yyjson_doc_free(doc);
    if (!neu) return;
    free(node->properties_json);
    node->properties_json = neu;
}

static int compare_node_text(const char *left, const char *right) {
    return strcmp(left ? left : "", right ? right : "");
}

static int compare_complexity_nodes(const void *left, const void *right) {
    const cbm_gbuf_node_t *a = *(const cbm_gbuf_node_t *const *)left;
    const cbm_gbuf_node_t *b = *(const cbm_gbuf_node_t *const *)right;
    int cmp = compare_node_text(a->qualified_name, b->qualified_name);
    if (cmp == 0) cmp = compare_node_text(a->label, b->label);
    if (cmp == 0) cmp = compare_node_text(a->file_path, b->file_path);
    if (cmp == 0 && a->start_line != b->start_line)
        cmp = a->start_line < b->start_line ? -1 : 1;
    if (cmp == 0 && a->end_line != b->end_line)
        cmp = a->end_line < b->end_line ? -1 : 1;
    if (cmp == 0) cmp = compare_node_text(a->name, b->name);
    return cmp;
}

typedef struct {
    int64_t id;
    int rank;
} complexity_callee_t;

static int compare_complexity_callees(const void *left, const void *right) {
    const complexity_callee_t *a = left;
    const complexity_callee_t *b = right;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return 0;
}

/* Memoized DFS: tld(id) = loop_depth(id) + max over CALLS-callees of tld(callee).
 * state: 0=unvisited, 1=in-progress (back-edge → cycle), 2=done. */
static int tld_dfs(const cbm_gbuf_t *gb, int64_t id, const int *loop_depth, int *tld, char *state,
                   bool *recursive, const int *rank, int64_t maxid, int depth) {
    if (id < 1 || id > maxid) {
        return 0;
    }
    if (state[id] == 2) {
        return tld[id];
    }
    if (state[id] == 1) {
        recursive[id] = true; /* back edge → call-graph cycle */
        return 0;
    }
    if (depth > CBM_TLD_MAX_DEPTH) {
        return loop_depth[id];
    }
    state[id] = 1;
    int best = 0;
    const cbm_gbuf_edge_t **edges = NULL;
    int ne = 0;
    cbm_gbuf_find_edges_by_source_type(gb, id, "CALLS", &edges, &ne);
    complexity_callee_t *callees = ne > 0 ? malloc((size_t)ne * sizeof(*callees)) : NULL;
    if (ne > 0 && !callees) {
        state[id] = 2;
        tld[id] = loop_depth[id];
        return tld[id];
    }
    for (int i = 0; i < ne; i++) {
        int64_t target = edges[i]->target_id;
        callees[i] = (complexity_callee_t){
            .id = target,
            .rank = target >= 1 && target <= maxid && rank[target] > 0
                        ? rank[target]
                        : INT_MAX,
        };
    }
    if (ne > 1) {
        qsort(callees, (size_t)ne, sizeof(*callees), compare_complexity_callees);
    }
    for (int i = 0; i < ne; i++) {
        int64_t c = callees[i].id;
        if (c == id) {
            recursive[id] = true; /* direct self-recursion */
            continue;
        }
        int ct = tld_dfs(gb, c, loop_depth, tld, state, recursive, rank, maxid, depth + 1);
        if (ct > best) {
            best = ct;
        }
    }
    free(callees);
    tld[id] = loop_depth[id] + best;
    state[id] = 2;
    return tld[id];
}

/* Seed each Function/Method node's loop_depth and self_recursive flag, and
 * remember the node pointer for write-back. The self_recursive seed (set at
 * extraction) feeds the final recursive flag; tld_dfs additionally ORs in
 * mutual recursion discovered as a call-graph cycle. */
static void seed_loop_depths(const cbm_gbuf_t *gb, const char *label, int *loop_depth,
                             bool *recursive, cbm_gbuf_node_t **nptr, int64_t maxid) {
    const cbm_gbuf_node_t **nodes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        const cbm_gbuf_node_t *n = nodes[i];
        if (n->id >= 1 && n->id <= maxid) {
            loop_depth[n->id] = json_get_int(n->properties_json, "loop_depth", 0);
            recursive[n->id] = json_get_bool(n->properties_json, "self_recursive");
            nptr[n->id] = (cbm_gbuf_node_t *)n;
        }
    }
}

void cbm_pipeline_pass_complexity(cbm_pipeline_ctx_t *ctx) {
    cbm_gbuf_t *gb = ctx->gbuf;
    /* Node and edge IDs are drawn from one shared counter, so node IDs are NOT
     * contiguous 1..node_count — they interleave with edge IDs. Size the lookup
     * arrays by the id ceiling (next_id) so every node id is addressable. */
    int64_t maxid = cbm_gbuf_next_id(gb) - 1;
    if (maxid < 1) {
        return;
    }
    size_t sz = (size_t)maxid + 1;
    int *loop_depth = calloc(sz, sizeof(int));
    int *tld = calloc(sz, sizeof(int));
    char *state = calloc(sz, sizeof(char));
    bool *recursive = calloc(sz, sizeof(bool));
    cbm_gbuf_node_t **nptr = calloc(sz, sizeof(cbm_gbuf_node_t *));
    int *rank = calloc(sz, sizeof(int));
    if (!loop_depth || !tld || !state || !recursive || !nptr || !rank) {
        free(loop_depth);
        free(tld);
        free(state);
        free(recursive);
        free(nptr);
        free(rank);
        return;
    }

    seed_loop_depths(gb, "Function", loop_depth, recursive, nptr, maxid);
    seed_loop_depths(gb, "Method", loop_depth, recursive, nptr, maxid);

    int node_count = 0;
    for (int64_t id = 1; id <= maxid; id++) {
        if (nptr[id]) node_count++;
    }
    cbm_gbuf_node_t **ordered = node_count > 0
        ? malloc((size_t)node_count * sizeof(*ordered))
        : NULL;
    if (node_count > 0 && !ordered) {
        free(loop_depth); free(tld); free(state); free(recursive); free(nptr); free(rank);
        return;
    }
    int ordered_count = 0;
    for (int64_t id = 1; id <= maxid; id++) {
        if (nptr[id]) ordered[ordered_count++] = nptr[id];
    }
    if (ordered_count > 1) {
        qsort(ordered, (size_t)ordered_count, sizeof(*ordered), compare_complexity_nodes);
    }
    for (int i = 0; i < ordered_count; i++) rank[ordered[i]->id] = i + 1;

    for (int i = 0; i < ordered_count; i++) {
        int64_t id = ordered[i]->id;
        if (state[id] != 2)
            tld_dfs(gb, id, loop_depth, tld, state, recursive, rank, maxid, 0);
        append_complexity_props(ordered[i], tld[id], recursive[id]);
    }

    cbm_log_info("pass.complexity", "functions", itoa_cx(ordered_count));

    free(ordered);
    free(loop_depth);
    free(tld);
    free(state);
    free(recursive);
    free(nptr);
    free(rank);
}
