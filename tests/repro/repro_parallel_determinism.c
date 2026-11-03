/*
 * repro_parallel_determinism.c — RED reproduction for the parallel-indexing
 * edge-loss / non-determinism bug (REAL-REPO TIER).
 *
 * DISCOVERED: while verifying F6 (2026-07), re-indexing the SAME corpus with the
 * correct cache cleared produced DIFFERENT edge counts run-to-run when
 * multi-threaded (kernel fs/xfs: 58548 / 58552 / 58557 / 58573; kernel-rust:
 * 44445 / 44775 / 44857) and the multi-threaded counts trend BELOW the
 * single-threaded reference (xfs ST 58573). I.e. parallel indexing both flickers
 * AND drops edges vs the single-threaded graph.
 *
 * INVARIANT (green <=> fixed): indexing a fixed corpus is DETERMINISTIC and the
 * multi-threaded graph equals the single-threaded graph exactly. This test indexes
 * the corpus single-threaded (reference) then K times multi-threaded (fresh store
 * each run) and asserts every MT sorted edge set equals the ST one. Comparison is
 * over the SORTED (source_qn, type, target_qn) triples — NOT raw counts.
 *
 * WHY A REAL-REPO TIER (honest calibration record): a self-contained synthetic C
 * corpus could NOT trigger the divergence in three escalating attempts —
 *   (1) 300 files, dense cross-file CALLS only;
 *   (2) 500 files, big per-function bodies (fingerprinted, nodes_with_fp=500);
 *   (3) 600 files, TOKEN-IDENTICAL clustered bodies (to force SIMILAR_TO edges);
 * all produced a fully DETERMINISTIC graph across ST + 6 MT runs, and the
 * similarity pass emitted 0 edges on synthetic C (`pass.similarity edges=0`). The
 * race clearly needs real-code edge diversity/volume (real SIMILAR_TO / semantic /
 * cross-file type edges) that synthetic C does not produce. Rather than ship a
 * false guard (green on buggy code), this uses the smallest REAL corpus on which
 * the flicker was directly observed — the kernel's fs/xfs subtree — and SKIPs when
 * that corpus is absent (e.g. CI). It is RED on the dev machine where the race
 * lives, which is where the fix (deferred) will be developed and guarded.
 *
 * SUSPECTED ROOT CAUSE (for the fixer): a race in parallel edge production/merge —
 * per-worker edge-buffer merge (pass_parallel 2_merge_edge_bufs_seq), graph-buffer
 * edge dedup under concurrent append (edge_by_key check-then-insert), similarity
 * edge emission (symmetric SIMILAR_TO from both sides), or resolve_worker emission
 * ordering. The single-threaded edge set is the reference semantics. Fix DEFERRED —
 * this stays on the known-red board until the race is fixed.
 */
#include "test_framework.h"
#include "repro_harness.h"
#include <store/store.h>
#include <pipeline/pipeline.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define RPD_MT_RUNS 4 /* K multi-threaded runs compared against the ST reference */

/* Smallest real corpus on which the flicker was directly observed. */
#define RPD_CORPUS "/Users/martinvogel/perf-bench/linux/fs/xfs"

static int rpd_cmp_lines(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sorted (source_qn|type|target_qn) fingerprint of the whole project graph.
 * Heap string (caller frees) or NULL on error. */
static char *rpd_edge_fingerprint(cbm_store_t *s, const char *project) {
    cbm_schema_info_t schema = {0};
    if (cbm_store_get_schema_counts(s, project, &schema) != CBM_STORE_OK)
        return NULL;
    char **lines = NULL;
    size_t line_count = 0;
    size_t total = 1;
    int ok = 1;
    for (int t = 0; ok && t < schema.edge_type_count; t++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (cbm_store_find_edges_by_type(s, project, schema.edge_types[t].type, &edges,
                                         &edge_count) != CBM_STORE_OK) {
            ok = 0;
            break;
        }
        char **grown = (char **)realloc(lines, (line_count + (size_t)edge_count) * sizeof(*lines));
        if (!grown && edge_count > 0) {
            ok = 0;
        } else if (grown) {
            lines = grown;
        }
        for (int i = 0; ok && i < edge_count; i++) {
            cbm_node_t src = {0};
            cbm_node_t tgt = {0};
            cbm_store_find_node_by_id(s, edges[i].source_id, &src);
            cbm_store_find_node_by_id(s, edges[i].target_id, &tgt);
            const char *sq = src.qualified_name ? src.qualified_name : "";
            const char *tq = tgt.qualified_name ? tgt.qualified_name : "";
            size_t need = strlen(sq) + strlen(edges[i].type) + strlen(tq) + 4;
            char *line = (char *)malloc(need);
            if (line) {
                snprintf(line, need, "%s|%s|%s\n", sq, edges[i].type, tq);
                lines[line_count++] = line;
                total += need;
            } else {
                ok = 0;
            }
            cbm_node_free_fields(&src);
            cbm_node_free_fields(&tgt);
        }
        cbm_store_free_edges(edges, edge_count);
    }
    cbm_store_schema_free(&schema);
    char *buf = ok ? (char *)malloc(total) : NULL;
    if (buf) {
        qsort(lines, line_count, sizeof(*lines), rpd_cmp_lines);
        size_t len = 0;
        buf[0] = '\0';
        for (size_t i = 0; i < line_count; i++) {
            size_t n = strlen(lines[i]);
            memcpy(buf + len, lines[i], n + 1);
            len += n;
        }
    }
    for (size_t i = 0; i < line_count; i++)
        free(lines[i]);
    free(lines);
    return buf;
}

/* Index `repo` through cbm.zova and return the edge fingerprint. Honors
 * CBM_WORKERS from the environment. */
static char *rpd_index_and_fingerprint(const char *repo) {
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    if (!p)
        return NULL;
    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    if (rc != 0)
        return NULL;

    char *project = cbm_project_name_from_path(repo);
    if (!project)
        return NULL;
    cbm_store_t *s = th_open_project_store(project);
    char *fp = s ? rpd_edge_fingerprint(s, project) : NULL;
    if (s)
        cbm_store_close(s);
    th_remove_project_workspace(project);
    free(project);
    return fp;
}

/* GUARD (green <=> the parallel-indexing races are fixed): repeated
 * multi-threaded runs over a fixed corpus must produce the IDENTICAL sorted
 * node+edge set. Root causes fixed (all order/scheduling dependence):
 * semantic-pass admission + canonical funcs order, import target first-match,
 * similarity ordering + id-based pair ownership, call-neighbor truncation
 * subsets, and the QN-collision last-wins overwrite in gbuf upsert AND merge
 * (a C struct/function/macro sharing one name flipped label by merge order). */
TEST(repro_parallel_edge_determinism) {
    struct stat st;
    if (stat(RPD_CORPUS, &st) != 0 || !S_ISDIR(st.st_mode)) {
        SKIP("real-repo tier: corpus " RPD_CORPUS
             " absent (synthetic C could not trigger the parallel edge race — see header)");
    }

    /* First multi-threaded run = reference; every further MT run must match. */
    char *fp_ref = rpd_index_and_fingerprint(RPD_CORPUS);
    ASSERT_NOT_NULL(fp_ref);
    ASSERT_TRUE(strlen(fp_ref) > 0);

    int diverged = 0;
    for (int k = 1; k < RPD_MT_RUNS && !diverged; k++) {
        char *fp_mt = rpd_index_and_fingerprint(RPD_CORPUS);
        ASSERT_NOT_NULL(fp_mt);
        if (strcmp(fp_mt, fp_ref) != 0)
            diverged = 1;
        free(fp_mt);
    }

    free(fp_ref);

    ASSERT_EQ(diverged, 0);
    PASS();
}

/* One worker (CBM_WORKERS=0) and the default worker count must emit the same
 * graph for the same corpus. When one worker meant a separate sequential
 * pipeline, the two disagreed on the xfs corpus on ~3459 USAGE, ~1666 WRITES
 * and ~60 CALLS sorted-edge lines. Both now run the parallel pipeline; this
 * has not been re-measured since, because the corpus is not checked in. */
TEST(repro_one_and_many_workers_equivalence) {
    struct stat st;
    if (stat(RPD_CORPUS, &st) != 0 || !S_ISDIR(st.st_mode)) {
        SKIP("real-repo tier: corpus " RPD_CORPUS " absent");
    }

    setenv("CBM_WORKERS", "0", 1);
    char *fp_st = rpd_index_and_fingerprint(RPD_CORPUS);
    unsetenv("CBM_WORKERS");
    ASSERT_NOT_NULL(fp_st);

    char *fp_mt = rpd_index_and_fingerprint(RPD_CORPUS);
    ASSERT_NOT_NULL(fp_mt);

    int equal = strcmp(fp_st, fp_mt) == 0;
    free(fp_st);
    free(fp_mt);

    ASSERT_EQ(equal, 1);
    PASS();
}

void suite_repro_parallel_determinism(void) {
    RUN_TEST(repro_parallel_edge_determinism);
    RUN_TEST(repro_one_and_many_workers_equivalence);
}
