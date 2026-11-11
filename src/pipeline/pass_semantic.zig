//! pass_semantic.zig — Go implicit interface satisfaction.
//!
//! A Go type implements an interface by having its method set; nothing in the
//! source names the relation. After all definitions are in the graph buffer,
//! this pass matches each Go Struct/Class against each Go Interface by method
//! name and emits IMPLEMENTS (type → interface) plus OVERRIDE (method →
//! interface method). The explicit relations (INHERITS, DECORATES, declared
//! IMPLEMENTS) come from the resolve workers in pass_parallel.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const ix = util.ix;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;

/// Check if file_path ends with a suffix.
fn fpEndsWith(fp: CStr, suffix: CStr) bool {
    if (fp == null or suffix == null) return false;
    const fplen = c.strlen(fp);
    const sflen = c.strlen(suffix);
    return fplen >= sflen and util.streq(fp + fplen - sflen, suffix);
}

/// Info about one interface method (name + node ID).
const GoImethod = struct {
    name: CStr,
    id: i64,
};

/// Check if class has all interface methods and create IMPLEMENTS + OVERRIDE edges.
fn checkGoClassImplements(
    ctx: *c.cbm_pipeline_ctx_t,
    cls: *const Node,
    iface: *const Node,
    imethods: []const GoImethod,
) c_int {
    if (cls.file_path == null or cls.qualified_name == null) return 0;
    if (!fpEndsWith(cls.file_path, ".go")) return 0;
    // Resolve the struct's methods two ways and use whichever finds them:
    //   (a) the struct's DEFINES_METHOD edges, matched by method NAME — the real
    //       pipeline path, where Go receiver methods carry a flat QN (e.g.
    //       "pkg.Area" rather than "pkg.Circle.Area"), so QN-string
    //       reconstruction would miss; and
    //   (b) the reconstructed QN "<ClassQN>.<methodName>" — used by tests and any
    //       extractor that does emit class-qualified method QNs without
    //       DEFINES_METHOD edges from the class.
    var cls_dm: [*c][*c]const Edge = null;
    var cls_dm_count: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_source_type(ctx.gbuf, cls.id, "DEFINES_METHOD", &cls_dm, &cls_dm_count);

    var prefix: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(&prefix, prefix.len, "%s.", cls.qualified_name);

    // For each interface method, find the matching struct method node.
    var matched: [c.CBM_SZ_128]*const Node = undefined;
    for (imethods, 0..) |imethod, m| {
        var found: ?*const Node = null;
        // (a) DEFINES_METHOD edge by name
        var d: usize = 0;
        while (d < cls_dm_count) : (d += 1) {
            const cm: ?*const Node = c.cbm_gbuf_find_by_id(ctx.gbuf, cls_dm[d].*.target_id);
            if (cm) |node| {
                if (node.name != null and util.streq(node.name, imethod.name)) {
                    found = node;
                    break;
                }
            }
        }
        // (b) reconstructed "<ClassQN>.<methodName>"
        if (found == null) {
            var method_qn: [c.CBM_SZ_512]u8 = undefined;
            _ = c.snprintf(&method_qn, method_qn.len, "%s%s", @as(CStr, &prefix), imethod.name);
            found = c.cbm_gbuf_find_by_qn(ctx.gbuf, &method_qn);
        }
        matched[m] = found orelse return 0; // struct does not satisfy the interface
    }
    _ = c.cbm_gbuf_insert_edge(ctx.gbuf, cls.id, iface.id, "IMPLEMENTS", "{}");
    var edges: c_int = 1;
    for (imethods, 0..) |imethod, m| {
        _ = c.cbm_gbuf_insert_edge(ctx.gbuf, matched[m].id, imethod.id, "OVERRIDE", "{}");
        edges += 1;
    }
    return edges;
}

pub export fn cbm_pipeline_implements_go(ctx: *c.cbm_pipeline_ctx_t) c_int {
    var edge_count: c_int = 0;

    // Find all Interface nodes
    var ifaces: [*c][*c]const Node = null;
    var iface_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(ctx.gbuf, "Interface", &ifaces, &iface_count) != 0) return 0;

    // Find candidate concrete types. In Go the type that satisfies an interface
    // is a struct (now labelled "Struct") or a named type (labelled "Class"); both
    // sets are checked. Each call returns a borrowed internal array (no free).
    var classes: [*c][*c]const Node = null;
    var class_count: c_int = 0;
    _ = c.cbm_gbuf_find_by_label(ctx.gbuf, "Class", &classes, &class_count);
    var structs: [*c][*c]const Node = null;
    var struct_count: c_int = 0;
    _ = c.cbm_gbuf_find_by_label(ctx.gbuf, "Struct", &structs, &struct_count);
    if (class_count == 0 and struct_count == 0) return 0;

    var i: usize = 0;
    while (i < iface_count) : (i += 1) {
        const iface: *const Node = ifaces[i];
        if (iface.file_path == null or !fpEndsWith(iface.file_path, ".go")) continue;

        // Get interface methods via DEFINES_METHOD edges
        var dm_edges: [*c][*c]const Edge = null;
        var dm_count: c_int = 0;
        if (c.cbm_gbuf_find_edges_by_source_type(ctx.gbuf, iface.id, "DEFINES_METHOD", &dm_edges, &dm_count) != 0 or
            dm_count == 0)
        {
            continue;
        }

        // Collect interface method info
        var imethods: [c.CBM_SZ_128]GoImethod = undefined;
        var im_count: usize = 0;
        var j: usize = 0;
        while (j < dm_count and im_count < imethods.len) : (j += 1) {
            const m: ?*const Node = c.cbm_gbuf_find_by_id(ctx.gbuf, dm_edges[j].*.target_id);
            if (m) |node| {
                if (node.name != null) {
                    imethods[im_count] = .{ .name = node.name, .id = node.id };
                    im_count += 1;
                }
            }
        }
        if (im_count == 0) continue;

        // Check each concrete-type node (Struct + Class) for method-set
        // satisfaction.
        var k: usize = 0;
        while (k < struct_count) : (k += 1) {
            edge_count += checkGoClassImplements(ctx, structs[k], iface, imethods[0..im_count]);
        }
        k = 0;
        while (k < class_count) : (k += 1) {
            edge_count += checkGoClassImplements(ctx, classes[k], iface, imethods[0..im_count]);
        }
    }
    return edge_count;
}
