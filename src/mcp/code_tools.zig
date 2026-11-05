//! code_tools.zig — the trace_path, get_code_snippet and get_file_outline MCP
//! tool handlers, and the same-name node resolution they share.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const mcp = @import("mcp.zig");
const project_tools = @import("project_tools.zig");

const CStr = mcp.CStr;
const HeapStr = mcp.HeapStr;
const Str = mcp.Str;
const Server = mcp.Server;
const MutDoc = mcp.MutDoc;
const MutVal = mcp.MutVal;
const Val = mcp.Val;
const textResult = mcp.textResult;
const orEmpty = mcp.orEmpty;
const sp = mcp.sp;
const eql = mcp.eql;

const MCP_DEFAULT_DEPTH = 3;
const MCP_DEFAULT_LIMIT = 10;
const MCP_BFS_LIMIT = 100;
const MAX_EDGE_TYPES = 16;
const SNIPPET_DEFAULT_LINES = 50;
/// The QN suffix of a C macro node (CBM_MACRO_QN_SUFFIX in internal/cbm/cbm.h).
const MACRO_QN_SUFFIX = "#macro";

fn span0(s: CStr) []const u8 {
    return std.mem.span(orEmpty(s));
}

fn nodeSlice(nodes: [*c]c.cbm_node_t, n: c_int) []c.cbm_node_t {
    return if (nodes != null and n > 0) nodes[0..@intCast(n)] else &.{};
}

/// Free a node's heap fields and zero it.
fn freeNodeContents(n: *c.cbm_node_t) void {
    c.cbm_node_free_fields(n);
    n.* = std.mem.zeroes(c.cbm_node_t);
}

// ── Shared file/project helpers ──────────────────────────────────

/// Lines [start, end] of a file, or NULL when unreadable or empty. Caller frees.
pub fn readFileLines(path: CStr, start: c_int, end: c_int) HeapStr {
    const fp = c.cbm_fopen(path, "r");
    if (fp == null) return null;
    defer _ = c.fclose(fp);

    var out: std.ArrayList(u8) = .empty;
    const gpa = std.heap.c_allocator;
    defer out.deinit(gpa);
    var line: [2048]u8 = undefined;
    var lineno: c_int = 0;
    // A line longer than the buffer arrives in several fgets() chunks; only
    // the first chunk starts a new line, so count lines by newline.
    var at_line_start = true;
    while (c.fgets(&line, line.len, fp) != null) {
        const chunk = mcp.bufStr(&line);
        if (at_line_start) lineno += 1;
        at_line_start = chunk.len > 0 and chunk[chunk.len - 1] == '\n';
        if (lineno < start) continue;
        if (lineno > end) break;
        out.appendSlice(gpa, chunk) catch return null;
    }
    if (out.items.len == 0) return null;
    const buf = interop.mallocN(u8, out.items.len + 1);
    if (buf == null) return null;
    @memcpy(buf[0..out.items.len], out.items);
    buf[out.items.len] = 0;
    return buf;
}

/// The project's root_path from the shared cbm.zova. Caller frees.
pub fn getProjectRoot(project: CStr) HeapStr {
    if (project == null) return null;
    var path: [1024]u8 = undefined;
    const repo = mcp.openRepository(&path, project) orelse return null;
    defer c.cbm_zova_repository_close(repo);
    var proj = std.mem.zeroes(c.cbm_project_t);
    defer c.cbm_project_free_fields(&proj);
    if (c.cbm_zova_repository_get_project(repo, c.cbm_zova_repository_workspace_id(repo), &proj) != c.CBM_STORE_OK) return null;
    return if (proj.root_path != null) mcp.dup(proj.root_path) else null;
}

// ── Node resolution ──────────────────────────────────────────────

/// Rank a candidate for name resolution. The label tier (callable >
/// class-like > module/file) is the primary key; WITHIN a tier the larger
/// definition by line span wins, so the .c-over-.h and C-main-over-shell-main
/// preferences come from span. Two same-tier candidates with equal span tie
/// and are reported ambiguous (see pickResolvedNode).
const RES_RANK_CALLABLE = 2; // Function / Method
const RES_RANK_OTHER = 1; // Class / Struct / etc.
const RES_RANK_MODULE = 0; // Module / File
const RES_LABEL_WEIGHT = 1000000; // label tier dominates span

fn nodeResolutionScore(n: *const c.cbm_node_t) c_long {
    var label_rank: c_long = RES_RANK_MODULE;
    if (n.label != null) {
        const label = std.mem.span(n.label);
        if (eql(label, "Function") or eql(label, "Method")) {
            label_rank = RES_RANK_CALLABLE;
        } else if (!eql(label, "Module") and !eql(label, "File")) {
            label_rank = RES_RANK_OTHER;
        }
    }
    // Tie rule: a name that is both a definition and a C macro -- a typedef
    // or enumerator next to its rename macro -- resolves to the definition.
    // The macro scores just under every other definition, and still above
    // Module/File; without this a one-line typedef and its macro tie on span.
    const qn = span0(n.qualified_name);
    if (label_rank == RES_RANK_OTHER and qn.len > MACRO_QN_SUFFIX.len and std.mem.endsWith(u8, qn, MACRO_QN_SUFFIX)) {
        return RES_RANK_OTHER * RES_LABEL_WEIGHT - 1;
    }
    const span = @max(@as(c_long, n.end_line) - @as(c_long, n.start_line), 0);
    return label_rank * RES_LABEL_WEIGHT + span;
}

/// A "real" callable definition: a Function/Method node with a non-empty body
/// span. A body-less node is an ambient declaration / signature stub (e.g. a
/// TypeScript `.d.ts` declaration) — a fragment of one logical symbol, which
/// pickResolvedNode unions with its real implementation (#546).
fn nodeIsRealCallableDef(n: *const c.cbm_node_t) bool {
    if (n.label == null) return false;
    const label = std.mem.span(n.label);
    if (!eql(label, "Function") and !eql(label, "Method")) return false;
    return @as(c_long, n.end_line) - @as(c_long, n.start_line) > 0;
}

/// Keep the nodes defined in `file`: its exact repository-relative path, or
/// a trailing path suffix of one ("db.zig" matches "src/db.zig"). Dropped
/// nodes are freed; returns the new count.
fn filterNodesByFile(nodes: [*c]c.cbm_node_t, n: c_int, file_c: CStr) c_int {
    if (nodes == null or file_c == null or file_c[0] == 0) return n;
    const file = std.mem.span(file_c);
    var kept: usize = 0;
    for (nodeSlice(nodes, n), 0..) |*node, i| {
        const fp = span0(node.file_path);
        const match = eql(fp, file) or
            (fp.len > file.len and fp[fp.len - file.len - 1] == '/' and std.mem.endsWith(u8, fp, file));
        if (!match) {
            freeNodeContents(node);
        } else {
            if (kept != i) {
                nodes[kept] = node.*;
                node.* = std.mem.zeroes(c.cbm_node_t);
            }
            kept += 1;
        }
    }
    return @intCast(kept);
}

/// Pick the best-resolving node among name matches. Sets ambiguous when the
/// matches can't be reduced to one logical symbol:
///   1. the top score is shared by >1 candidate (a genuine rank/span tie), or
///   2. two or more *real* callable definitions share the name — distinct
///      implementations, not a definition plus its body-less stub(s).
/// Body-less .d.ts stubs still union with their implementation (#650).
fn pickResolvedNode(nodes: []const c.cbm_node_t, ambiguous: *bool) usize {
    ambiguous.* = false;
    if (nodes.len <= 1) return 0;
    var best: usize = 0;
    var best_score = nodeResolutionScore(&nodes[0]);
    for (nodes[1..], 1..) |*n, i| {
        const s = nodeResolutionScore(n);
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }
    var top_count: usize = 0;
    var real_def_count: usize = 0;
    for (nodes) |*n| {
        if (nodeResolutionScore(n) == best_score) top_count += 1;
        if (nodeIsRealCallableDef(n)) real_def_count += 1;
    }
    ambiguous.* = real_def_count > 1 or top_count > 1;
    return best;
}

fn suggestionLess(_: void, a: c.cbm_node_t, b: c.cbm_node_t) bool {
    var o = std.mem.order(u8, span0(a.qualified_name), span0(b.qualified_name));
    if (o == .eq) o = std.mem.order(u8, span0(a.file_path), span0(b.file_path));
    if (o == .eq) o = std.mem.order(u8, span0(a.label), span0(b.label));
    return o == .lt;
}

/// A JSON suggestions response for ambiguous or fuzzy results.
fn snippetSuggestions(input: CStr, nodes: []c.cbm_node_t) HeapStr {
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "status", "ambiguous");

    var msg: [512]u8 = undefined;
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "message", mcp.fmtZ(&msg, "{d} matches for \"{s}\". Pick a qualified_name from suggestions below, " ++
        "pass file_path to choose by file, or use search_graph(name_pattern=\"...\") " ++
        "to narrow results.", .{ nodes.len, span0(input) }));

    std.sort.pdq(c.cbm_node_t, nodes, {}, suggestionLess);
    const arr = c.yyjson_mut_arr(doc);
    for (nodes) |n| {
        const s = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, s, "qualified_name", orEmpty(n.qualified_name));
        _ = c.yyjson_mut_obj_add_str(doc, s, "name", orEmpty(n.name));
        _ = c.yyjson_mut_obj_add_str(doc, s, "label", orEmpty(n.label));
        _ = c.yyjson_mut_obj_add_str(doc, s, "file_path", orEmpty(n.file_path));
        _ = c.yyjson_mut_arr_append(arr, s);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "suggestions", arr);
    return mcp.docResult(doc);
}

// ── trace_path ───────────────────────────────────────────────────

/// Edge types from args: explicit array > mode-based > default ("CALLS").
/// Returns the parsed args doc when explicit edge_types were found (the
/// caller keeps it alive while the types are used), or NULL.
fn resolveTraceEdgeTypes(args: CStr, mode: CStr, out_types: *[MAX_EDGE_TYPES]CStr, out_count: *c_int) ?*c.yyjson_doc {
    out_count.* = 0;
    if (mcp.readJson(args)) |et_doc| {
        const et_arr = c.yyjson_obj_get(c.yyjson_doc_get_root(et_doc), "edge_types");
        if (c.yyjson_is_arr(et_arr)) {
            var it: c.yyjson_arr_iter = undefined;
            _ = c.yyjson_arr_iter_init(et_arr, &it);
            while (c.yyjson_arr_iter_next(&it)) |v| {
                if (c.yyjson_is_str(v) and out_count.* < MAX_EDGE_TYPES) {
                    out_types[@intCast(out_count.*)] = c.yyjson_get_str(v);
                    out_count.* += 1;
                }
            }
        }
        if (out_count.* > 0) return et_doc; // pointers reference doc memory
        c.yyjson_doc_free(et_doc);
    }

    const m = span0(mode);
    const defaults: []const CStr = if (eql(m, "data_flow"))
        &.{ "CALLS", "DATA_FLOWS" }
    else if (eql(m, "cross_service"))
        &.{ "HTTP_CALLS", "ASYNC_CALLS", "DATA_FLOWS", "CALLS" }
    else
        &.{"CALLS"};
    for (defaults, 0..) |t, i| out_types[i] = t;
    out_count.* = @intCast(defaults.len);
    return null;
}

/// Whether a file path looks like a test file.
fn isTestFile(path_c: CStr) bool {
    if (path_c == null) return false;
    const path = std.mem.span(path_c);
    const contains = [_][]const u8{ "/test", "test_", "_test.", "/tests/", "/spec/", ".test." };
    for (contains) |needle| {
        if (std.mem.indexOf(u8, path, needle) != null) return true;
    }
    // project-root-relative test trees have no leading slash (#1294)
    const prefixes = [_][]const u8{ "tests/", "test/", "spec/", "__tests__/" };
    for (prefixes) |prefix| {
        if (std.mem.startsWith(u8, path, prefix)) return true;
    }
    return false;
}

fn traceJsonAppend(doc: ?*MutDoc, arr: ?*MutVal, node: *const c.cbm_node_t, hop: c_int, risk_labels: bool, include_tests: bool) ?*MutVal {
    const is_test = isTestFile(node.file_path);
    if (!include_tests and is_test) return null;
    const item = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_str(doc, item, "name", orEmpty(node.name));
    _ = c.yyjson_mut_obj_add_str(doc, item, "qualified_name", orEmpty(node.qualified_name));
    _ = c.yyjson_mut_obj_add_int(doc, item, "hop", hop);
    if (risk_labels) _ = c.yyjson_mut_obj_add_str(doc, item, "risk", c.cbm_risk_label(c.cbm_hop_to_risk(hop)));
    if (is_test) _ = c.yyjson_mut_obj_add_bool(doc, item, "is_test", true);
    _ = c.yyjson_mut_arr_add_val(arr, item);
    return item;
}

fn edgeSlice(tr: *const c.cbm_traverse_result_t) []const c.cbm_edge_info_t {
    return if (tr.edges != null and tr.edge_count > 0) tr.edges[0..@intCast(tr.edge_count)] else &.{};
}

/// The CALLS-edge "args" JSON (the serialized arg expressions) on the edge
/// that leads to the given hop node, so data_flow mode can surface argument
/// expressions (#514). Borrowed "[...]" slice of the edge's properties_json,
/// or null when no args are recorded.
fn bfsEdgeArgsForHop(tr: *const c.cbm_traverse_result_t, hop_node_id: i64) ?[]const u8 {
    for (edgeSlice(tr)) |e| {
        // The hop node is the edge endpoint reached from the root side: for
        // an outbound trace it is the target, for inbound the source.
        if (e.target_id != hop_node_id and e.source_id != hop_node_id) continue;
        if (e.properties_json == null) continue;
        const pj = std.mem.span(e.properties_json);
        const args = std.mem.indexOf(u8, pj, "\"args\"") orelse continue;
        const open = std.mem.indexOfScalarPos(u8, pj, args, '[') orelse continue;
        var depth: usize = 0;
        var p = open;
        while (p < pj.len) : (p += 1) {
            if (pj[p] == '[') {
                depth += 1;
            } else if (pj[p] == ']') {
                depth -= 1;
                if (depth == 0) {
                    p += 1;
                    break;
                }
            }
        }
        return pj[open..p];
    }
    return null;
}

/// Map an internal resolver strategy (recorded on CALLS and CALL_REFERENCE
/// edges) to the closed public class trace_path publishes with
/// include_evidence (upstream 73cfb61d): lsp | language_rule | heuristic |
/// unresolved. Publishing the raw internal names would make each resolver
/// name public API by accident. NULL only for a NULL/empty strategy.
pub export fn cbm_mcp_edge_strategy_class(strategy: [*c]const u8) [*c]const u8 {
    if (strategy == null or strategy[0] == 0) return null;
    const s = std.mem.span(strategy);
    // lsp_unresolved is an LSP attempt that did NOT resolve.
    if (eql(s, "lsp_unresolved") or eql(s, "unknown")) return "unresolved";
    if (std.mem.startsWith(u8, s, "lsp_")) return "lsp";
    if (std.mem.startsWith(u8, s, "php_") or std.mem.startsWith(u8, s, "perl_")) return "language_rule";
    return "heuristic"; // callee_suffix, unique_name, field_type_hint, ...
}

const Evidence = struct { class: CStr, confidence: f64 };

/// Resolution evidence on the edge leading to the given hop node, matched
/// like bfsEdgeArgsForHop. Null when the edge records no strategy.
fn bfsEdgeEvidenceForHop(tr: *const c.cbm_traverse_result_t, hop_node_id: i64) ?Evidence {
    for (edgeSlice(tr)) |e| {
        if (e.target_id != hop_node_id and e.source_id != hop_node_id) continue;
        const pdoc = if (e.properties_json != null) mcp.readJson(e.properties_json) else null;
        defer c.yyjson_doc_free(pdoc);
        const proot: ?*Val = if (pdoc) |d| c.yyjson_doc_get_root(d) else null;
        const cls = cbm_mcp_edge_strategy_class(c.yyjson_get_str(c.yyjson_obj_get(proot, "strategy")));
        const conf = c.yyjson_obj_get(proot, "confidence");
        if (cls != null) return .{ .class = cls, .confidence = if (c.yyjson_is_num(conf)) c.yyjson_get_num(conf) else -1.0 };
    }
    return null;
}

/// Traversal results as a JSON array.
fn bfsToJsonArray(doc: ?*MutDoc, tr: *const c.cbm_traverse_result_t, risk_labels: bool, include_tests: bool, data_flow: bool, include_evidence: bool) ?*MutVal {
    const arr = c.yyjson_mut_arr(doc);
    const visited = if (tr.visited != null and tr.visited_count > 0) tr.visited[0..@intCast(tr.visited_count)] else &[_]c.cbm_node_hop_t{};
    for (visited) |*v| {
        const item = traceJsonAppend(doc, arr, &v.node, v.hop, risk_labels, include_tests) orelse continue;
        if (include_evidence) {
            if (bfsEdgeEvidenceForHop(tr, v.node.id)) |ev| {
                _ = c.yyjson_mut_obj_add_str(doc, item, "strategy", ev.class);
                if (ev.confidence >= 0.0) _ = c.yyjson_mut_obj_add_real(doc, item, "confidence", ev.confidence);
            }
        }
        // data_flow mode promises argument expressions at each call site;
        // surface the CALLS edge's serialized args array as raw JSON (#514).
        if (data_flow) {
            if (bfsEdgeArgsForHop(tr, v.node.id)) |args| {
                if (args.len > 0) {
                    if (c.yyjson_mut_rawn(doc, args.ptr, args.len)) |av| _ = c.yyjson_mut_obj_add_val(doc, item, "args", av);
                }
            }
        }
    }
    return arr;
}

fn textOrder(a: CStr, b: CStr) std.math.Order {
    return std.mem.order(u8, span0(a), span0(b));
}

fn bfsVisitLess(_: void, a: c.cbm_node_hop_t, b: c.cbm_node_hop_t) bool {
    if (a.hop != b.hop) return a.hop < b.hop;
    var o = textOrder(a.node.file_path, b.node.file_path);
    if (o == .eq) o = textOrder(a.node.qualified_name, b.node.qualified_name);
    if (o == .eq) o = textOrder(a.node.label, b.node.label);
    if (o != .eq) return o == .lt;
    if (a.node.start_line != b.node.start_line) return a.node.start_line < b.node.start_line;
    if (a.node.end_line != b.node.end_line) return a.node.end_line < b.node.end_line;
    return a.node.id < b.node.id;
}

/// BFS from EVERY node sharing the resolved name and merge the results, so
/// the caller/callee set is complete even when one logical symbol is
/// represented by more than one graph node — e.g. a real .ts implementation
/// plus an ambient .d.ts stub, whose inbound CALLS edges are otherwise split
/// across the two nodes (#546). visited hops are deduped by node id; edges
/// are concatenated. Ownership of all heap fields moves into out, freed by
/// cbm_store_traverse_free.
fn bfsUnionSameName(store: ?*c.cbm_store_t, nodes: []const c.cbm_node_t, direction: Str, edge_types: [*c]const CStr, edge_type_count: c_int, depth: c_int, need_edges: bool, out: *c.cbm_traverse_result_t) void {
    out.* = std.mem.zeroes(c.cbm_traverse_result_t);
    var vcap: usize = 0;
    var ecap: usize = 0;
    const bfs = if (need_edges) &c.cbm_store_bfs_with_edges else &c.cbm_store_bfs;
    for (nodes) |n| {
        var tr = std.mem.zeroes(c.cbm_traverse_result_t);
        _ = bfs(store, n.id, direction, @ptrCast(@constCast(edge_types)), edge_type_count, depth, MCP_BFS_LIMIT, &tr);
        // Moved entries are zeroed, so this frees only the un-moved (root +
        // duplicate) fields.
        defer c.cbm_store_traverse_free(&tr);
        var no_hops = [0]c.cbm_node_hop_t{};
        const visited: []c.cbm_node_hop_t = if (tr.visited != null and tr.visited_count > 0) tr.visited[0..@intCast(tr.visited_count)] else &no_hops;
        for (visited) |*v| {
            const dup_index = for (0..@intCast(out.visited_count)) |j| {
                if (out.visited[j].node.id == v.node.id) break j;
            } else null;
            if (dup_index) |j| {
                if (v.hop < out.visited[j].hop) {
                    c.cbm_node_free_fields(&out.visited[j].node);
                    out.visited[j] = v.*;
                    v.* = std.mem.zeroes(c.cbm_node_hop_t);
                }
                continue;
            }
            const used: usize = @intCast(out.visited_count);
            if (used >= vcap) {
                const ncap = if (vcap > 0) vcap * 2 else 8;
                const grown = interop.reallocN(c.cbm_node_hop_t, out.visited, ncap);
                if (grown == null) continue; // left in tr and freed with it
                out.visited = grown;
                vcap = ncap;
            }
            out.visited[used] = v.*;
            out.visited_count += 1;
            v.* = std.mem.zeroes(c.cbm_node_hop_t); // ownership moved
        }
        var no_edges = [0]c.cbm_edge_info_t{};
        const edges: []c.cbm_edge_info_t = if (tr.edges != null and tr.edge_count > 0) tr.edges[0..@intCast(tr.edge_count)] else &no_edges;
        for (edges) |*e| {
            const used: usize = @intCast(out.edge_count);
            if (used >= ecap) {
                const ncap = if (ecap > 0) ecap * 2 else 8;
                const grown = interop.reallocN(c.cbm_edge_info_t, out.edges, ncap);
                if (grown == null) continue;
                out.edges = grown;
                ecap = ncap;
            }
            out.edges[used] = e.*;
            out.edge_count += 1;
            e.* = std.mem.zeroes(c.cbm_edge_info_t); // ownership moved
        }
    }
    if (out.visited_count > 1) std.sort.pdq(c.cbm_node_hop_t, out.visited[0..@intCast(out.visited_count)], {}, bfsVisitLess);
}

/// Clamp a client-supplied traversal depth to the MCP ceiling
/// (cbm_mcp_max_depth), WARN-logging when it does so — never a silent
/// truncation (#887).
pub fn clampMcpDepth(depth: c_int, tool: Str) c_int {
    const cap = c.cbm_mcp_max_depth();
    if (depth <= cap) return depth;
    var req_buf: [16]u8 = undefined;
    var cap_buf: [16]u8 = undefined;
    interop.logWarn("mcp.depth_capped", .{ "tool", tool, "requested", @as(CStr, mcp.fmtZ(&req_buf, "{d}", .{depth})), "cap", @as(CStr, mcp.fmtZ(&cap_buf, "{d}", .{cap})) });
    return cap;
}

pub fn handleTraceCallPath(srv: *Server, args: CStr) HeapStr {
    const func_name = mcp.strArg(args, "function_name");
    defer c.free(func_name);
    const project = mcp.getProjectArg(args);
    defer c.free(project);
    const store = mcp.resolveStore(srv, project);
    var direction = mcp.strArg(args, "direction");
    defer c.free(direction);
    const mode = mcp.strArg(args, "mode");
    defer c.free(mode);
    const depth = clampMcpDepth(mcp.intArg(args, "depth", MCP_DEFAULT_DEPTH), "trace_call_path");
    const risk_labels = mcp.boolArg(args, "risk_labels");
    const include_tests = mcp.boolArg(args, "include_tests");
    // Off by default so the common response stays the same size.
    const include_evidence = mcp.boolArg(args, "include_evidence");

    if (func_name == null) return textResult("function_name is required", true);
    if (store == null) return mcp.textResultOwned(mcp.buildProjectListError("project not found or not indexed"), true);

    const not_indexed = mcp.verifyProjectIndexed(store, project);
    if (not_indexed != null) return not_indexed;

    if (direction == null) direction = mcp.dup("both");

    // Find the node by name. If the bare-name lookup misses, fall back to
    // qualified_name so callers passing a fully-qualified identifier (which
    // the not-found hint recommends) hit the same path.
    var nodes: [*c]c.cbm_node_t = null;
    var node_count: c_int = 0;
    _ = c.cbm_store_find_nodes_by_name(store, project, func_name, &nodes, &node_count);
    if (node_count == 0) {
        var qn_node = std.mem.zeroes(c.cbm_node_t);
        if (c.cbm_store_find_node_by_qn(store, project, func_name, &qn_node) == c.CBM_STORE_OK) {
            c.cbm_store_free_nodes(nodes, 0);
            nodes = interop.mallocN(c.cbm_node_t, 1);
            if (nodes != null) {
                nodes[0] = qn_node;
                node_count = 1;
            } else {
                freeNodeContents(&qn_node);
            }
        }
    }
    // Every early return frees what is left; node_count tracks the live ones.
    defer c.cbm_store_free_nodes(nodes, node_count);

    const file_filter = mcp.strArg(args, "file_path");
    defer c.free(file_filter);
    if (file_filter != null and node_count > 0) {
        node_count = filterNodesByFile(nodes, node_count, file_filter);
        if (node_count == 0) {
            var hint: [512]u8 = undefined;
            return textResult(mcp.fmtZ(&hint, "function {s} is not defined in file_path {s}. Omit file_path to list " ++
                "every match.", .{ sp(func_name), sp(file_filter) }), true);
        }
    }

    if (node_count == 0) {
        var hint: [512]u8 = undefined;
        return textResult(mcp.fmtZ(&hint, "{{\"error\":\"function not found\",\"function_name\":\"{s}\"," ++
            "\"hint\":\"Use search_graph(name_pattern=\\\".*{s}.*\\\") to find the exact " ++
            "name, then pass it to trace_path.\"}}", .{ sp(func_name), sp(func_name) }), true);
    }

    // Disambiguate same-named matches: report ambiguity (rather than
    // silently tracing nodes[0]) on a genuine tie — e.g. a C main() vs a
    // same-named shell-script main(). The trace unions all same-name nodes,
    // see bfsUnionSameName (#546).
    const all = nodeSlice(nodes, node_count);
    var trace_ambiguous = false;
    _ = pickResolvedNode(all, &trace_ambiguous);
    if (trace_ambiguous) return snippetSuggestions(func_name, all);

    // Edge types: explicit > mode-based > default.
    var edge_types: [MAX_EDGE_TYPES]CStr = undefined;
    var edge_type_count: c_int = 0;
    const et_doc_keep = resolveTraceEdgeTypes(args, mode, &edge_types, &edge_type_count);
    defer if (et_doc_keep) |d| c.yyjson_doc_free(d);

    // yyjson_mut_obj_add_str borrows pointers — the traversal results stay
    // alive (deferred frees) until after serialization in docResult.
    const dir = std.mem.span(direction);
    const do_outbound = eql(dir, "outbound") or eql(dir, "both");
    const do_inbound = eql(dir, "inbound") or eql(dir, "both");
    const data_flow = mode != null and mcp.cEql(mode, "data_flow");

    var tr_out = std.mem.zeroes(c.cbm_traverse_result_t);
    defer c.cbm_store_traverse_free(&tr_out);
    var tr_in = std.mem.zeroes(c.cbm_traverse_result_t);
    defer c.cbm_store_traverse_free(&tr_in);

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "function", func_name);
    _ = c.yyjson_mut_obj_add_str(doc, root, "direction", direction);
    if (mode != null) _ = c.yyjson_mut_obj_add_str(doc, root, "mode", mode);

    if (do_outbound) {
        bfsUnionSameName(store, all, "outbound", &edge_types, edge_type_count, depth, data_flow or include_evidence, &tr_out);
        _ = c.yyjson_mut_obj_add_val(doc, root, "callees", bfsToJsonArray(doc, &tr_out, risk_labels, include_tests, data_flow, include_evidence));
    }
    if (do_inbound) {
        bfsUnionSameName(store, all, "inbound", &edge_types, edge_type_count, depth, data_flow or include_evidence, &tr_in);
        _ = c.yyjson_mut_obj_add_val(doc, root, "callers", bfsToJsonArray(doc, &tr_in, risk_labels, include_tests, data_flow, include_evidence));
    }
    return mcp.docResult(doc);
}

// ── get_code_snippet ─────────────────────────────────────────────

/// root_path/file_path, contained in root_path, as source lines. Sets
/// abs_path_out (caller frees). NULL when the path escapes the root or is
/// unreadable.
fn resolveSnippetSource(root_path: CStr, file_path: CStr, start: c_int, end: c_int, abs_path_out: *HeapStr) HeapStr {
    abs_path_out.* = null;
    if (root_path == null or file_path == null) return null;
    const root_s = std.mem.span(root_path);
    const file_s = std.mem.span(file_path);
    const abs_path = interop.mallocN(u8, root_s.len + file_s.len + 2);
    if (abs_path == null) return null;
    _ = mcp.bprint(abs_path[0 .. root_s.len + file_s.len + 2], "{s}/{s}", .{ root_s, file_s });
    abs_path_out.* = abs_path;
    if (mcp.pathWithinRoot(root_path, abs_path)) return readFileLines(abs_path, start, end);
    return null;
}

/// An enriched snippet response for a resolved node.
fn buildSnippetResponse(srv: *Server, node: *const c.cbm_node_t, match_method: ?Str, include_neighbors: bool) HeapStr {
    const root_path = getProjectRoot(node.project);
    defer c.free(root_path);

    const start: c_int = if (node.start_line > 0) node.start_line else 1;
    const end: c_int = if (node.end_line > start) node.end_line else start + SNIPPET_DEFAULT_LINES;
    var abs_path: HeapStr = null;
    defer c.free(abs_path);
    const source = resolveSnippetSource(root_path, node.file_path, start, end, &abs_path);
    defer c.free(source);

    // Borrowed by the doc: freed after serialization (docResult below).
    var props_doc: ?*c.yyjson_doc = null;
    defer c.yyjson_doc_free(props_doc);
    var nb_callers: [*c][*c]u8 = null;
    var nb_caller_count: c_int = 0;
    var nb_callees: [*c][*c]u8 = null;
    var nb_callee_count: c_int = 0;
    defer {
        for (0..@intCast(nb_caller_count)) |i| c.free(nb_callers[i]);
        for (0..@intCast(nb_callee_count)) |i| c.free(nb_callees[i]);
        c.free(@ptrCast(nb_callers));
        c.free(@ptrCast(nb_callees));
    }

    const doc = c.yyjson_mut_doc_new(null);
    const root_obj = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root_obj);

    _ = c.yyjson_mut_obj_add_str(doc, root_obj, "name", orEmpty(node.name));
    _ = c.yyjson_mut_obj_add_str(doc, root_obj, "qualified_name", orEmpty(node.qualified_name));
    _ = c.yyjson_mut_obj_add_str(doc, root_obj, "label", orEmpty(node.label));
    _ = c.yyjson_mut_obj_add_str(doc, root_obj, "file_path", if (abs_path != null) abs_path else orEmpty(node.file_path));
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "start_line", start);
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "end_line", end);

    const safe_source = if (source != null) mcp.sanitizeUtf8Lossy(source) else null;
    if (safe_source != null) {
        _ = c.yyjson_mut_obj_add_strcpy(doc, root_obj, "source", safe_source);
        c.free(safe_source);
    } else {
        _ = c.yyjson_mut_obj_add_str(doc, root_obj, "source", "(source not available)");
    }

    // match_method — omitted for exact matches
    if (match_method) |m| _ = c.yyjson_mut_obj_add_str(doc, root_obj, "match_method", m);

    // coverage_note — only when the source file has a recorded parse gap
    if (srv.current_project != null) {
        var db_path: [1024]u8 = undefined;
        if (mcp.openRepository(&db_path, srv.current_project)) |repo| {
            const note = project_tools.coverageSnippetNote(repo, c.cbm_zova_repository_workspace_id(repo), node.file_path);
            c.cbm_zova_repository_close(repo);
            if (note != null) {
                _ = c.yyjson_mut_obj_add_strcpy(doc, root_obj, "coverage_note", note);
                c.free(note);
            }
        }
    }

    props_doc = mcp.enrichNodeProperties(doc, root_obj, node.properties_json);

    // Caller/callee counts — store already resolved by the calling handler.
    const store = srv.store;
    var in_deg: c_int = 0;
    var out_deg: c_int = 0;
    c.cbm_store_node_degree(store, node.id, &in_deg, &out_deg);
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "callers", in_deg);
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "callees", out_deg);

    if (include_neighbors) {
        _ = c.cbm_store_node_neighbor_names(store, node.id, MCP_DEFAULT_LIMIT, &nb_callers, &nb_caller_count, &nb_callees, &nb_callee_count);
        mcp.addStringArray(doc, root_obj, "caller_names", nb_callers, nb_caller_count);
        mcp.addStringArray(doc, root_obj, "callee_names", nb_callees, nb_callee_count);
    }
    return mcp.docResult(doc);
}

/// Move nodes[i] out of a find_nodes_* array into a standalone node.
fn takeNode(nodes: [*c]c.cbm_node_t, i: usize) c.cbm_node_t {
    const node = nodes[i];
    nodes[i] = std.mem.zeroes(c.cbm_node_t);
    return node;
}

pub fn handleGetCodeSnippet(srv: *Server, args: CStr) HeapStr {
    const qn = mcp.strArg(args, "qualified_name");
    defer c.free(qn);
    const project = mcp.getProjectArg(args);
    defer c.free(project);
    const include_neighbors = mcp.boolArg(args, "include_neighbors");

    if (qn == null) return textResult("qualified_name is required", true);

    const store = mcp.resolveStore(srv, project);
    if (store == null) return mcp.textResultOwned(mcp.buildProjectListError("project not found or not indexed"), true);
    const not_indexed = mcp.verifyProjectIndexed(store, project);
    if (not_indexed != null) return not_indexed;

    // Default to current project (same as all other tools).
    const effective_project: CStr = if (project != null) project else srv.current_project;

    // Tier 1: Exact QN match.
    var node = std.mem.zeroes(c.cbm_node_t);
    defer c.cbm_node_free_fields(&node);
    if (c.cbm_store_find_node_by_qn(store, effective_project, qn, &node) == c.CBM_STORE_OK) {
        return buildSnippetResponse(srv, &node, null, include_neighbors);
    }

    // Tier 2: Suffix match — handles partial QNs ("main.HandleRequest") and
    // short names ("ProcessOrder") via LIKE '%.X'.
    var suffix_nodes: [*c]c.cbm_node_t = null;
    var suffix_count: c_int = 0;
    _ = c.cbm_store_find_nodes_by_qn_suffix(store, effective_project, qn, &suffix_nodes, &suffix_count);

    // Tier 3: the C-macro namespace. A macro's QN is `<module>.<NAME>#macro`,
    // out of reach of '%.X'. Tried only when no definition answered to the
    // name above (tie rule).
    if (suffix_count == 0) {
        var macro_qn: [512]u8 = undefined;
        const macro_len = mcp.bprint(&macro_qn, "{s}" ++ MACRO_QN_SUFFIX, .{sp(qn)});
        if (macro_len > 0 and macro_len < macro_qn.len) {
            c.cbm_store_free_nodes(suffix_nodes, suffix_count);
            suffix_nodes = null;
            _ = c.cbm_store_find_nodes_by_qn_suffix(store, effective_project, &macro_qn, &suffix_nodes, &suffix_count);
        }
    }
    defer c.cbm_store_free_nodes(suffix_nodes, suffix_count);

    const file_filter = mcp.strArg(args, "file_path");
    defer c.free(file_filter);
    if (file_filter != null and suffix_count > 0) {
        suffix_count = filterNodesByFile(suffix_nodes, suffix_count, file_filter);
        if (suffix_count == 0) {
            var msg: [512]u8 = undefined;
            return textResult(mcp.fmtZ(&msg, "symbol {s} is not defined in file_path {s}. Omit file_path to list every " ++
                "match.", .{ sp(qn), sp(file_filter) }), true);
        }
    }

    if (suffix_count == 1) {
        node = takeNode(suffix_nodes, 0);
        return buildSnippetResponse(srv, &node, "suffix", include_neighbors);
    }

    if (suffix_count > 1) {
        // Prefer the real definition (a .c body over a .h declaration, a
        // Function over a Module) so an unambiguous-by-preference match
        // resolves directly; only a genuine tie still returns suggestions.
        const all = nodeSlice(suffix_nodes, suffix_count);
        var snip_ambiguous = false;
        const ssel = pickResolvedNode(all, &snip_ambiguous);
        if (!snip_ambiguous) {
            node = takeNode(suffix_nodes, ssel);
            return buildSnippetResponse(srv, &node, "suffix", include_neighbors);
        }
        return snippetSuggestions(qn, all);
    }

    // Nothing found — guide the caller toward search_graph.
    return textResult("symbol not found. Use search_graph(name_pattern=\"...\") first to discover " ++
        "the exact qualified_name, then pass it to get_code_snippet.", true);
}

// ── get_file_outline ─────────────────────────────────────────────

const MCP_FILE_OUTLINE_OUTPUT_MAX = 2 * 1024 * 1024;

pub fn handleGetFileOutline(srv: *Server, args: CStr) HeapStr {
    const args_doc = mcp.readJson(if (args != null) args else "{}");
    defer c.yyjson_doc_free(args_doc);
    const root: ?*Val = if (args_doc) |d| c.yyjson_doc_get_root(d) else null;
    if (root == null or !c.yyjson_is_obj(root)) return textResult("get_file_outline arguments must be a JSON object", true);
    const project = mcp.getProjectArg(if (args != null) args else "{}");
    if (project == null) return textResult("project is required", true);
    defer c.free(project);

    const file_value = c.yyjson_obj_get(root, "file_path");
    const file_path: CStr = if (c.yyjson_is_str(file_value)) c.yyjson_get_str(file_value) else null;
    if (file_path == null or file_path[0] == 0) return textResult("file_path is required", true);
    var normalized_path: [4096]u8 = undefined;
    if (project_tools.normalizeRel(file_path, false, &normalized_path) != .ok) {
        return textResult("file_path must be a repository-relative path without '..'", true);
    }

    var limit: c_int = 100;
    if (c.yyjson_obj_get(root, "limit")) |limit_value| {
        const parsed: i64 = if (c.yyjson_is_int(limit_value)) c.yyjson_get_sint(limit_value) else 0;
        if (parsed < 1 or parsed > c.CBM_STORE_FILE_OUTLINE_MAX_LIMIT) return textResult("limit must be an integer between 1 and 200", true);
        limit = @intCast(parsed);
    }
    var offset: c_int = 0;
    if (c.yyjson_obj_get(root, "offset")) |offset_value| {
        const parsed: i64 = if (c.yyjson_is_int(offset_value)) c.yyjson_get_sint(offset_value) else -1;
        if (parsed < 0 or parsed > std.math.maxInt(c_int)) return textResult("offset must be a non-negative integer", true);
        offset = @intCast(parsed);
    }

    var labels: [c.CBM_STORE_FILE_OUTLINE_MAX_LABELS]CStr = undefined;
    var label_count: c_int = 0;
    if (c.yyjson_obj_get(root, "labels")) |labels_value| {
        if (!c.yyjson_is_arr(labels_value) or c.yyjson_arr_size(labels_value) > c.CBM_STORE_FILE_OUTLINE_MAX_LABELS) {
            return textResult("labels must be an array of at most 16 strings", true);
        }
        var it: c.yyjson_arr_iter = undefined;
        _ = c.yyjson_arr_iter_init(labels_value, &it);
        while (c.yyjson_arr_iter_next(&it)) |item| {
            const label: CStr = if (c.yyjson_is_str(item)) c.yyjson_get_str(item) else null;
            if (label == null or label[0] == 0 or c.strlen(label) >= 128) {
                return textResult("labels must contain only non-empty bounded strings", true);
            }
            labels[@intCast(label_count)] = label;
            label_count += 1;
        }
    }

    const store = mcp.resolveStore(srv, project);
    if (store == null) return mcp.textResultOwned(mcp.buildProjectListError("project not found or not indexed"), true);
    const not_indexed = mcp.verifyProjectIndexed(store, project);
    if (not_indexed != null) return not_indexed;

    var rows: [*c]c.cbm_file_outline_row_t = null;
    var row_count: c_int = 0;
    var total: c_int = 0;
    const rc = c.cbm_store_get_file_outline(store, project, &normalized_path, @ptrCast(&labels), label_count, limit, offset, &rows, &row_count, &total);
    if (rc != c.CBM_STORE_OK) {
        return textResult(if (rc == c.CBM_STORE_SCAN_LIMIT) "get_file_outline exceeded its fixed output safety limit" else "get_file_outline query failed", true);
    }

    const doc = c.yyjson_mut_doc_new(null);
    const obj = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, obj);
    _ = c.yyjson_mut_obj_add_strcpy(doc, obj, "file_path", &normalized_path);
    const json_rows = c.yyjson_mut_arr(doc);
    if (rows != null and row_count > 0) {
        for (rows[0..@intCast(row_count)]) |r| {
            var lines = std.mem.zeroes([32]u8);
            if (r.start_line > 0) _ = mcp.bprint(&lines, "{d}-{d}", .{ r.start_line, @max(r.end_line, r.start_line) });
            const row = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_strcpy(doc, row, "name", r.name);
            _ = c.yyjson_mut_obj_add_strcpy(doc, row, "label", r.label);
            _ = c.yyjson_mut_obj_add_strcpy(doc, row, "lines", &lines);
            _ = c.yyjson_mut_obj_add_strcpy(doc, row, "qn", r.qualified_name);
            _ = c.yyjson_mut_arr_add_val(json_rows, row);
        }
    }
    _ = c.yyjson_mut_obj_add_val(doc, obj, "results", json_rows);
    _ = c.yyjson_mut_obj_add_int(doc, obj, "total", total);
    _ = c.yyjson_mut_obj_add_int(doc, obj, "offset", offset);
    _ = c.yyjson_mut_obj_add_int(doc, obj, "limit", limit);
    _ = c.yyjson_mut_obj_add_int(doc, obj, "returned", row_count);
    _ = c.yyjson_mut_obj_add_bool(doc, obj, "has_more", @as(i64, offset) + row_count < total);
    const payload = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    c.cbm_store_free_file_outline(rows, row_count);
    if (payload == null) return textResult("get_file_outline output allocation failed", true);
    if (c.strlen(payload) > MCP_FILE_OUTLINE_OUTPUT_MAX) {
        c.free(payload);
        return textResult("get_file_outline exceeded its fixed output safety limit", true);
    }
    return mcp.textResultOwned(payload, false);
}
