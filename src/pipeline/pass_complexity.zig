//! pass_complexity.zig — Interprocedural complexity propagation (Tier B).
//!
//! Tier A (in the extraction walk) stamps each Function/Method node with local
//! structural metrics: complexity (cyclomatic), cognitive, loop_count, loop_depth.
//! This pass propagates loop_depth along CALLS edges to estimate a worst-case
//! *transitive* nested-loop degree: a function with a depth-1 loop that calls an
//! O(n) helper is effectively O(n^2). The estimate assumes calls may occur inside
//! loops (an upper bound) — it is a queryable bottleneck *candidate* signal, not a
//! proof (true big-O is undecidable; cf. SPEED / Loopus). Cycles in the call graph
//! are broken and flagged via a `recursive` property.
//!
//! Writes two extra node properties: transitive_loop_depth, recursive.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;

const CBM_TLD_MAX_DEPTH = 256; // recursion-depth cap (cycle/stack guard)

/// The text after "key": in a flat JSON object, past spaces and tabs; NULL
/// when the key is absent.
fn jsonValueOf(json: CStr, key: CStr) CStr {
    if (json == null) return null;
    var pat: [c.CBM_SZ_64]u8 = undefined;
    _ = c.snprintf(&pat, pat.len, "\"%s\":", key);
    var p: CStr = c.strstr(json, &pat);
    if (p == null) return null;
    p += c.strlen(&pat);
    while (p[0] == ' ' or p[0] == '\t') p += 1;
    return p;
}

/// Parse an integer "key":N from a flat JSON object. Returns dflt if absent.
fn jsonGetInt(json: CStr, key: CStr, dflt: c_int) c_int {
    const p = jsonValueOf(json, key);
    if (p == null) return dflt;
    return @truncate(c.strtol(p, null, c.CBM_DECIMAL_BASE));
}

/// Parse a boolean "key":true/false from a flat JSON object.
fn jsonGetBool(json: CStr, key: CStr) bool {
    const p = jsonValueOf(json, key);
    if (p == null) return false;
    return p[0] == 't';
}

/// Replace the derived complexity suffix when a graph buffer was loaded from a
/// prior index, then append the current values.  Full indexing calls this once;
/// incremental indexing recomputes it over a mixed old/new graph, so blindly
/// appending would leave duplicate JSON keys and stale values.
fn appendComplexityProps(node: *Node, tld: c_int, recursive: bool) void {
    @setEvalBranchQuota(10000);
    const old: CStr = if (node.properties_json != null) node.properties_json else "{}";
    const doc = c.yyjson_read(old, c.strlen(old), 0);
    const root = if (doc != null) c.yyjson_doc_get_root(doc) else null;
    if (root == null or !c.yyjson_is_obj(root)) {
        c.yyjson_doc_free(doc);
        return;
    }
    const mdoc = c.yyjson_mut_doc_new(null);
    if (mdoc == null) {
        c.yyjson_doc_free(doc);
        return;
    }
    const mroot = c.yyjson_val_mut_copy(mdoc, root);
    if (mroot == null) {
        c.yyjson_mut_doc_free(mdoc);
        c.yyjson_doc_free(doc);
        return;
    }
    c.yyjson_mut_doc_set_root(mdoc, mroot);
    // Loaded incremental nodes may contain these fields before later-derived
    // fields such as decorator_tags, and older generations may contain more
    // than one copy. yyjson removes every matching key before current values
    // are added exactly once.
    _ = c.yyjson_mut_obj_remove_key(mroot, "transitive_loop_depth");
    _ = c.yyjson_mut_obj_remove_key(mroot, "recursive");
    _ = c.yyjson_mut_obj_add_int(mdoc, mroot, "transitive_loop_depth", tld);
    _ = c.yyjson_mut_obj_add_bool(mdoc, mroot, "recursive", recursive);
    const neu = c.yyjson_mut_write(mdoc, 0, null);
    c.yyjson_mut_doc_free(mdoc);
    c.yyjson_doc_free(doc);
    if (neu == null) return;
    c.free(node.properties_json);
    node.properties_json = neu;
}

fn compareNodeText(left: CStr, right: CStr) c_int {
    return c.strcmp(if (left != null) left else "", if (right != null) right else "");
}

fn compareComplexityNodes(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    const a: *const Node = @as(*const *const Node, @ptrCast(@alignCast(left))).*;
    const b: *const Node = @as(*const *const Node, @ptrCast(@alignCast(right))).*;
    var cmp = compareNodeText(a.qualified_name, b.qualified_name);
    if (cmp == 0) cmp = compareNodeText(a.label, b.label);
    if (cmp == 0) cmp = compareNodeText(a.file_path, b.file_path);
    if (cmp == 0 and a.start_line != b.start_line) cmp = if (a.start_line < b.start_line) -1 else 1;
    if (cmp == 0 and a.end_line != b.end_line) cmp = if (a.end_line < b.end_line) -1 else 1;
    if (cmp == 0) cmp = compareNodeText(a.name, b.name);
    return cmp;
}

const ComplexityCallee = extern struct {
    id: i64,
    rank: c_int,
};

fn compareComplexityCallees(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    const a: *const ComplexityCallee = @ptrCast(@alignCast(left));
    const b: *const ComplexityCallee = @ptrCast(@alignCast(right));
    if (a.rank != b.rank) return if (a.rank < b.rank) -1 else 1;
    if (a.id != b.id) return if (a.id < b.id) -1 else 1;
    return 0;
}

/// The per-node tables of the pass, indexed by node id.
const Tables = struct {
    loop_depth: [*]c_int,
    tld: [*]c_int,
    state: [*]u8,
    recursive: [*]bool,
    rank: [*]c_int,
    maxid: i64,
};

/// Memoized DFS: tld(id) = loop_depth(id) + max over CALLS-callees of tld(callee).
/// state: 0=unvisited, 1=in-progress (back-edge → cycle), 2=done.
fn tldDfs(gb: ?*const c.cbm_gbuf_t, id: i64, t: *const Tables, depth: c_int) c_int {
    if (id < 1 or id > t.maxid) return 0;
    const n: usize = @intCast(id);
    if (t.state[n] == 2) return t.tld[n];
    if (t.state[n] == 1) {
        t.recursive[n] = true; // back edge → call-graph cycle
        return 0;
    }
    if (depth > CBM_TLD_MAX_DEPTH) return t.loop_depth[n];
    t.state[n] = 1;
    var best: c_int = 0;
    var edges: [*c][*c]const Edge = null;
    var ne: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_source_type(gb, id, "CALLS", &edges, &ne);
    const callees: [*c]ComplexityCallee = if (ne > 0) interop.mallocN(ComplexityCallee, util.ix(ne)) else null;
    if (ne > 0 and callees == null) {
        t.state[n] = 2;
        t.tld[n] = t.loop_depth[n];
        return t.tld[n];
    }
    var i: usize = 0;
    while (i < ne) : (i += 1) {
        const target = edges[i].*.target_id;
        callees[i] = .{
            .id = target,
            .rank = if (target >= 1 and target <= t.maxid and t.rank[@intCast(target)] > 0)
                t.rank[@intCast(target)]
            else
                std.math.maxInt(c_int),
        };
    }
    if (ne > 1) {
        c.qsort(callees, util.ix(ne), @sizeOf(ComplexityCallee), compareComplexityCallees);
    }
    i = 0;
    while (i < ne) : (i += 1) {
        const callee = callees[i].id;
        if (callee == id) {
            t.recursive[n] = true; // direct self-recursion
            continue;
        }
        const ct = tldDfs(gb, callee, t, depth + 1);
        if (ct > best) best = ct;
    }
    c.free(callees);
    t.tld[n] = t.loop_depth[n] + best;
    t.state[n] = 2;
    return t.tld[n];
}

/// Seed each Function/Method node's loop_depth and self_recursive flag, and
/// remember the node pointer for write-back. The self_recursive seed (set at
/// extraction) feeds the final recursive flag; tldDfs additionally ORs in
/// mutual recursion discovered as a call-graph cycle.
fn seedLoopDepths(gb: ?*const c.cbm_gbuf_t, label: CStr, t: *const Tables, nptr: [*]?*Node) void {
    var nodes: [*c][*c]const Node = null;
    var count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) return;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const n: *const Node = nodes[i];
        if (n.id >= 1 and n.id <= t.maxid) {
            const id: usize = @intCast(n.id);
            // CBM_WALK_METRIC_UNAVAILABLE (-1) marks a body whose metric walk
            // could not grow its stack; propagate it as "no loops of its own"
            // rather than letting it subtract from its callers' depth.
            const ld = jsonGetInt(n.properties_json, "loop_depth", 0);
            t.loop_depth[id] = if (ld < 0) 0 else ld;
            t.recursive[id] = jsonGetBool(n.properties_json, "self_recursive");
            nptr[id] = @constCast(n);
        }
    }
}

pub export fn cbm_pipeline_pass_complexity(ctx: *c.cbm_pipeline_ctx_t) void {
    const gb = ctx.gbuf;
    // Node and edge IDs are drawn from one shared counter, so node IDs are NOT
    // contiguous 1..node_count — they interleave with edge IDs. Size the lookup
    // arrays by the id ceiling (next_id) so every node id is addressable.
    const maxid = c.cbm_gbuf_next_id(gb) - 1;
    if (maxid < 1) return;
    const sz: usize = @as(usize, @intCast(maxid)) + 1;
    const loop_depth = interop.callocN(c_int, sz);
    const tld = interop.callocN(c_int, sz);
    const state = interop.callocN(u8, sz);
    const recursive = interop.callocN(bool, sz);
    const nptr = interop.callocN(?*Node, sz);
    const rank = interop.callocN(c_int, sz);
    if (loop_depth == null or tld == null or state == null or recursive == null or nptr == null or
        rank == null)
    {
        c.free(loop_depth);
        c.free(tld);
        c.free(state);
        c.free(recursive);
        c.free(@ptrCast(nptr));
        c.free(rank);
        return;
    }
    const t: Tables = .{
        .loop_depth = loop_depth,
        .tld = tld,
        .state = state,
        .recursive = recursive,
        .rank = rank,
        .maxid = maxid,
    };

    seedLoopDepths(gb, "Function", &t, nptr);
    seedLoopDepths(gb, "Method", &t, nptr);

    var node_count: usize = 0;
    for (nptr[1..sz]) |node| {
        if (node != null) node_count += 1;
    }
    const ordered: [*c]*Node = if (node_count > 0) interop.mallocN(*Node, node_count) else null;
    if (node_count > 0 and ordered == null) {
        c.free(loop_depth);
        c.free(tld);
        c.free(state);
        c.free(recursive);
        c.free(@ptrCast(nptr));
        c.free(rank);
        return;
    }
    var ordered_count: usize = 0;
    for (nptr[1..sz]) |node| {
        if (node) |found| {
            ordered[ordered_count] = found;
            ordered_count += 1;
        }
    }
    if (ordered_count > 1) {
        c.qsort(@ptrCast(ordered), ordered_count, @sizeOf(*Node), compareComplexityNodes);
    }
    for (0..ordered_count) |i| {
        rank[@intCast(ordered[i].id)] = @intCast(i + 1);
    }

    for (0..ordered_count) |i| {
        const id = ordered[i].id;
        const n: usize = @intCast(id);
        if (state[n] != 2) _ = tldDfs(gb, id, &t, 0);
        appendComplexityProps(ordered[i], tld[n], recursive[n]);
    }

    interop.logInfo("pass.complexity", .{
        @as(CStr, "functions"), @as(CStr, c.cbm_log_itoa(@intCast(ordered_count))),
    });

    c.free(@ptrCast(ordered));
    c.free(loop_depth);
    c.free(tld);
    c.free(state);
    c.free(recursive);
    c.free(@ptrCast(nptr));
    c.free(rank);
}
