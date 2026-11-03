/*
 * pass_semantic.c — Go implicit interface satisfaction.
 *
 * A Go type implements an interface by having its method set; nothing in the
 * source names the relation. After all definitions are in the graph buffer,
 * this pass matches each Go Struct/Class against each Go Interface by method
 * name and emits IMPLEMENTS (type → interface) plus OVERRIDE (method →
 * interface method). The explicit relations (INHERITS, DECORATES, declared
 * IMPLEMENTS) come from the resolve workers in pass_parallel.c.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/limits.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Check if file_path ends with a suffix. */
static bool fp_ends_with(const char *fp, const char *suffix) {
    if (!fp || !suffix) {
        return false;
    }
    size_t fplen = strlen(fp);
    size_t sflen = strlen(suffix);
    return fplen >= sflen && strcmp(fp + fplen - sflen, suffix) == 0;
}

/* Info about one interface method (name + node ID). */
typedef struct {
    const char *name;
    int64_t id;
} go_imethod_t;

/* Check if class has all interface methods and create IMPLEMENTS + OVERRIDE edges. */
static int check_go_class_implements(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *cls,
                                     const cbm_gbuf_node_t *iface, const go_imethod_t *imethods,
                                     int im_count) {
    if (!cls->file_path || !cls->qualified_name) {
        return 0;
    }
    if (!fp_ends_with(cls->file_path, ".go")) {
        return 0;
    }
    /* Resolve the struct's methods two ways and use whichever finds them:
     *   (a) the struct's DEFINES_METHOD edges, matched by method NAME — the real
     *       pipeline path, where Go receiver methods carry a flat QN (e.g.
     *       "pkg.Area" rather than "pkg.Circle.Area"), so QN-string
     *       reconstruction would miss; and
     *   (b) the reconstructed QN "<ClassQN>.<methodName>" — used by tests and any
     *       extractor that does emit class-qualified method QNs without
     *       DEFINES_METHOD edges from the class. */
    const cbm_gbuf_edge_t **cls_dm = NULL;
    int cls_dm_count = 0;
    cbm_gbuf_find_edges_by_source_type(ctx->gbuf, cls->id, "DEFINES_METHOD", &cls_dm,
                                       &cls_dm_count);

    char prefix[CBM_SZ_512];
    snprintf(prefix, sizeof(prefix), "%s.", cls->qualified_name);

    /* For each interface method, find the matching struct method node. */
    const cbm_gbuf_node_t *matched[CBM_SZ_128];
    for (int m = 0; m < im_count; m++) {
        const cbm_gbuf_node_t *found = NULL;
        /* (a) DEFINES_METHOD edge by name */
        for (int d = 0; d < cls_dm_count; d++) {
            const cbm_gbuf_node_t *cm = cbm_gbuf_find_by_id(ctx->gbuf, cls_dm[d]->target_id);
            if (cm && cm->name && strcmp(cm->name, imethods[m].name) == 0) {
                found = cm;
                break;
            }
        }
        /* (b) reconstructed "<ClassQN>.<methodName>" */
        if (!found) {
            char method_qn[CBM_SZ_512];
            snprintf(method_qn, sizeof(method_qn), "%s%s", prefix, imethods[m].name);
            found = cbm_gbuf_find_by_qn(ctx->gbuf, method_qn);
        }
        if (!found) {
            return 0; /* struct does not satisfy the interface */
        }
        matched[m] = found;
    }
    cbm_gbuf_insert_edge(ctx->gbuf, cls->id, iface->id, "IMPLEMENTS", "{}");
    int edges = SKIP_ONE;
    for (int m = 0; m < im_count; m++) {
        cbm_gbuf_insert_edge(ctx->gbuf, matched[m]->id, imethods[m].id, "OVERRIDE", "{}");
        edges++;
    }
    return edges;
}

int cbm_pipeline_implements_go(cbm_pipeline_ctx_t *ctx) {
    int edge_count = 0;

    /* Find all Interface nodes */
    const cbm_gbuf_node_t **ifaces = NULL;
    int iface_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "Interface", &ifaces, &iface_count) != 0) {
        return 0;
    }

    /* Find candidate concrete types. In Go the type that satisfies an interface
     * is a struct (now labelled "Struct") or a named type (labelled "Class"); both
     * sets are checked. Each call returns a borrowed internal array (no free). */
    const cbm_gbuf_node_t **classes = NULL;
    int class_count = 0;
    cbm_gbuf_find_by_label(ctx->gbuf, "Class", &classes, &class_count);
    const cbm_gbuf_node_t **structs = NULL;
    int struct_count = 0;
    cbm_gbuf_find_by_label(ctx->gbuf, "Struct", &structs, &struct_count);
    if (class_count == 0 && struct_count == 0) {
        return 0;
    }

    for (int i = 0; i < iface_count; i++) {
        const cbm_gbuf_node_t *iface = ifaces[i];
        if (!iface->file_path || !fp_ends_with(iface->file_path, ".go")) {
            continue;
        }

        /* Get interface methods via DEFINES_METHOD edges */
        const cbm_gbuf_edge_t **dm_edges = NULL;
        int dm_count = 0;
        if (cbm_gbuf_find_edges_by_source_type(ctx->gbuf, iface->id, "DEFINES_METHOD", &dm_edges,
                                               &dm_count) != 0 ||
            dm_count == 0) {
            continue;
        }

        /* Collect interface method info */
        go_imethod_t imethods[CBM_SZ_128];
        int im_count = 0;
        for (int j = 0; j < dm_count && im_count < CBM_SZ_128; j++) {
            const cbm_gbuf_node_t *m = cbm_gbuf_find_by_id(ctx->gbuf, dm_edges[j]->target_id);
            if (m && m->name) {
                imethods[im_count++] = (go_imethod_t){m->name, m->id};
            }
        }
        if (im_count == 0) {
            continue;
        }

        /* Check each concrete-type node (Struct + Class) for method-set
         * satisfaction. */
        for (int c = 0; c < struct_count; c++) {
            edge_count += check_go_class_implements(ctx, structs[c], iface, imethods, im_count);
        }
        for (int c = 0; c < class_count; c++) {
            edge_count += check_go_class_implements(ctx, classes[c], iface, imethods, im_count);
        }
    }
    return edge_count;
}
