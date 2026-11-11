//! pass_route_nodes.zig — Create Route nodes for HTTP_CALLS/ASYNC_CALLS edges.
//!
//! After parallel resolve merges edges into the main gbuf, HTTP_CALLS and
//! ASYNC_CALLS edges have url_path/method/broker in properties but point to
//! the library function (e.g., requests.get). This pass:
//!
//!   1. Scans all HTTP_CALLS/ASYNC_CALLS edges
//!   2. Extracts url_path from edge properties
//!   3. Creates Route nodes with deterministic QNs (__route__METHOD__/path)
//!   4. Re-targets edges from library function → Route node
//!
//! Route nodes are the rendezvous point for cross-service communication:
//!   Service A: checkout() → HTTP_CALLS → Route("POST /api/orders")
//!   Service B: create_order() → HANDLES → Route("POST /api/orders")

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;
const NodeList = [*c][*c]const Node;
const EdgeList = [*c][*c]const Edge;

const RN_MAX_SCHEME = 12;
const RN_MIN_SCHEME = 3;
const RN_STRIP_PASSES = 2;
const RN_SCHEME_SKIP = 3;
const RN_ARGS_SKIP = 7;
const RN_PROPS_MARGIN = 100;
const RN_CALLEE_SKIP = 14; // strlen('"callee_args":[') - 1
const ROUTE_QN_SIZE = c.CBM_ROUTE_QN_SIZE;

fn hasPrefix(s: CStr, comptime prefix: [:0]const u8) bool {
    return c.strncmp(s, prefix, prefix.len) == 0;
}

/// Logs one integer counter under `key`.
fn logCount(msg: [*:0]const u8, key: [*:0]const u8, value: c_int) void {
    var buf: [c.CBM_SZ_16]u8 = undefined;
    _ = c.snprintf(&buf, buf.len, "%d", value);
    interop.logInfo(msg, .{ @as(CStr, key), @as(CStr, &buf) });
}

/// True for characters that may appear in a ":name" route parameter.
inline fn isRouteIdentChar(ch: u8) bool {
    return (ch >= 'a' and ch <= 'z') or (ch >= 'A' and ch <= 'Z') or (ch >= '0' and ch <= '9') or
        ch == '_';
}

/// Canonicalize route-path parameter placeholders to a single "{}" token so that
/// client call sites and server handlers rendezvous on the same Route QN
/// regardless of framework syntax. Each parameter token collapses to "{}":
///
///   :name    Express / React-Router / Rails / typical JS API clients
///   {name}   Axum / Spring / OpenAPI / ASP.NET
///   <name>   Flask / Rocket (incl. typed "<int:id>")
///   ${...}   JS template interpolation captured into the path
///
/// Parameter names are intentionally discarded so the same logical endpoint
/// matches across services that name the path variable differently. Static path
/// text is copied verbatim; the result never exceeds the input length.
pub export fn cbm_route_canon_path(in: CStr, out: MutStr, out_sz: usize) CStr {
    if (out == null or out_sz == 0) return out;
    if (in == null) {
        out[0] = 0;
        return out;
    }
    const last = out_sz - 1;
    var oi: usize = 0;
    var i: usize = 0;
    while (in[i] != 0 and oi < last) {
        const ch = in[i];
        const at_seg_start = oi == 0 or out[oi - 1] == '/';
        var is_param = false;

        if (ch == ':' and at_seg_start and isRouteIdentChar(in[i + 1])) {
            i += 1;
            while (in[i] != 0 and isRouteIdentChar(in[i])) i += 1;
            is_param = true;
        } else if (ch == '{') {
            i += 1;
            while (in[i] != 0 and in[i] != '}' and in[i] != '/') i += 1;
            if (in[i] == '}') i += 1;
            is_param = true;
        } else if (ch == '<') {
            i += 1;
            while (in[i] != 0 and in[i] != '>' and in[i] != '/') i += 1;
            if (in[i] == '>') i += 1;
            is_param = true;
        } else if (ch == '$' and in[i + 1] == '{') {
            i += 2;
            while (in[i] != 0 and in[i] != '}' and in[i] != '/') i += 1;
            if (in[i] == '}') i += 1;
            is_param = true;
        }

        if (is_param) {
            if (oi + 2 > last) break;
            out[oi] = '{';
            out[oi + 1] = '}';
            oi += 2;
            continue;
        }
        out[oi] = ch;
        oi += 1;
        i += 1;
    }
    out[oi] = 0;
    return out;
}

/// Extract a JSON string value by key from properties.
/// Returns pointer into buf (caller provides buffer). NULL if not found.
fn jsonExtract(json: CStr, key: CStr, buf: []u8) CStr {
    if (json == null or key == null) return null;
    // Build "key":" pattern
    var pattern: [c.CBM_SZ_128]u8 = undefined;
    _ = c.snprintf(&pattern, pattern.len, "\"%s\":\"", key);
    var start: CStr = c.strstr(json, &pattern);
    if (start == null) return null;
    start += c.strlen(&pattern);
    const end: CStr = c.strchr(start, '"');
    if (end == null or end == start) return null;
    var len = util.ptrDiff(end, start);
    if (len >= buf.len) len = buf.len - 1;
    @memcpy(buf[0..len], start[0..len]);
    buf[len] = 0;
    return buf.ptr;
}

/// Visitor context for edge scanning
const RouteCtx = struct {
    gb: *Gbuf,
    created: c_int,
};

fn routeEdgeVisitor(edge: [*c]const Edge, userdata: ?*anyopaque) callconv(.c) void {
    const ctx: *RouteCtx = @ptrCast(@alignCast(userdata));

    // Only process HTTP_CALLS and ASYNC_CALLS
    const is_http = streq(edge.*.type, "HTTP_CALLS");
    if (!is_http and !streq(edge.*.type, "ASYNC_CALLS")) return;

    // Extract url_path from properties
    var url_buf: [c.CBM_SZ_512]u8 = undefined;
    const url = jsonExtract(edge.*.properties_json, "url_path", &url_buf);
    if (url == null or url[0] == 0) return;
    var callee_buf: [c.CBM_SZ_256]u8 = undefined;
    const callee = jsonExtract(edge.*.properties_json, "callee", &callee_buf);
    if (is_http and !c.cbm_service_pattern_is_http_route_literal(url, callee)) return;

    // Extract method or broker
    var method_buf: [c.CBM_SZ_16]u8 = undefined;
    var broker_buf: [c.CBM_SZ_64]u8 = undefined;
    const method = jsonExtract(edge.*.properties_json, "method", &method_buf);
    const broker = jsonExtract(edge.*.properties_json, "broker", &broker_buf);

    // Build Route QN
    var route_qn: [ROUTE_QN_SIZE]u8 = undefined;
    if (is_http) {
        var cpath: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(
            &route_qn,
            route_qn.len,
            "__route__%s__%s",
            if (method != null) method else @as(CStr, "ANY"),
            cbm_route_canon_path(url, &cpath, cpath.len),
        );
    } else {
        _ = c.snprintf(
            &route_qn,
            route_qn.len,
            "__route__%s__%s",
            if (broker != null) broker else @as(CStr, "async"),
            url,
        );
    }

    // Build properties for Route node
    var route_props: [c.CBM_SZ_256]u8 = undefined;
    if (method != null) {
        _ = c.snprintf(&route_props, route_props.len, "{\"method\":\"%s\"}", method);
    } else if (broker != null) {
        _ = c.snprintf(&route_props, route_props.len, "{\"broker\":\"%s\"}", broker);
    } else {
        _ = c.snprintf(&route_props, route_props.len, "{}");
    }

    // Create or find Route node (deduped by QN)
    _ = c.cbm_gbuf_upsert_node(ctx.gb, "Route", url, &route_qn, "", 0, 0, &route_props);
    ctx.created += 1;

    // Note: we do NOT re-target the edge here because modifying edges during
    // iteration is unsafe. The edge stays pointing to the library function.
    // The URL-in-args detection in pass_parallel will create Route→handler HANDLES
    // edge separately. The caller→library edge
    // with url_path in properties is sufficient for query_graph to find
    // the Route via: caller → HTTP_CALLS(url_path="/api/x") + Route("/api/x").
}

/// Extract URL path from full URL: "https://host/path/" → "/path/"
fn urlPath(url: CStr) CStr {
    if (url == null) return null;
    const scheme_end: CStr = c.strstr(url, "://");
    if (scheme_end == null) return url; // Already a path
    const path: CStr = c.strchr(scheme_end + RN_SCHEME_SKIP, '/');
    return if (path != null) path else "/";
}

/// Check if a trailing dash-segment is a hash/project-number (strippable).
/// Returns true if the segment is short alphanumeric but not a meaningful word.
fn isHashSegment(seg: []const u8) bool {
    if (seg.len > RN_MAX_SCHEME or seg.len == 0) return false;
    var has_letter = false;
    for (seg) |ch| {
        if (!((ch >= '0' and ch <= '9') or (ch >= 'a' and ch <= 'z'))) return false;
        if (ch >= 'a' and ch <= 'z') has_letter = true;
    }
    // Don't strip meaningful words (>=3 chars with letters like "api", "endpoint")
    return !(has_letter and seg.len >= RN_MIN_SCHEME);
}

/// Strip up to 2 trailing hash/project-number segments from a Cloud Run hostname.
fn stripCloudRunSuffixes(hostname: [*:0]u8) void {
    for (0..RN_STRIP_PASSES) |_| {
        const last_dash: MutStr = c.strrchr(hostname, '-');
        if (last_dash == null) break;
        const seg: [*]const u8 = last_dash + 1;
        if (!isHashSegment(seg[0..c.strlen(seg)])) break;
        last_dash[0] = 0;
    }
}

/// Extract service name from Cloud Run URL hostname.
/// "my-svc-ab12cd34ef-uc.a.run.app/path" → "my-svc"
fn extractServiceName(url: CStr, buf: []u8) CStr {
    if (url == null) return null;
    const scheme_end: CStr = c.strstr(url, "://");
    if (scheme_end == null) return null;
    const host_start: [*]const u8 = scheme_end + RN_SCHEME_SKIP;
    var end = host_start;
    while (end[0] != 0 and end[0] != '.' and end[0] != '/') end += 1;
    const hlen = util.ptrDiff(end, host_start);
    if (hlen == 0 or hlen >= buf.len) return null;

    var tmp: [c.CBM_SZ_256:0]u8 = undefined;
    if (hlen >= c.CBM_SZ_256) return null;
    @memcpy(tmp[0..hlen], host_start[0..hlen]);
    tmp[hlen] = 0;

    stripCloudRunSuffixes(&tmp);

    _ = c.snprintf(buf.ptr, buf.len, "%s", @as(CStr, &tmp));
    return buf.ptr;
}

/// Check if a Route QN indicates an infra/async/broker Route (not a handler).
fn isBrokerRoute(qn: CStr) bool {
    const prefixes = [_][*:0]const u8{
        "__route__infra__", "__route__pubsub__",          "__route__cloud_tasks__",
        "__route__async__", "__route__cloud_scheduler__", "__route__kafka__",
        "__route__sqs__",
    };
    if (qn == null) return false;
    for (prefixes) |prefix| {
        if (c.strstr(qn, prefix) == qn) return true;
    }
    return false;
}

/// Length of a URL path without its query/fragment and trailing '/' ("/" stays 1).
fn routePathLen(path: CStr) usize {
    var len = c.strcspn(path, "?#");
    while (len > 1 and path[len - 1] == '/') len -= 1;
    return len;
}

/// True when `prefix` is `path` or a whole-segment prefix of it:
/// /api/v1 covers /api/v1/items but not /api/v10.
fn routePathCovers(prefix: CStr, prefix_len: usize, path: CStr, path_len: usize) bool {
    return prefix_len <= path_len and c.strncmp(prefix, path, prefix_len) == 0 and
        (prefix_len == path_len or path[prefix_len] == '/');
}

/// Try to match a single infra Route to a handler Route and create HANDLES bridge.
/// Picks the best match, not the first: an exact path beats a segment-prefix
/// match, then the longer handler path, then the smaller QN. Route list order
/// follows parallel extraction and changes between runs, so first-match was not
/// reproducible. Returns 1 if matched, 0 otherwise.
fn matchOneInfraRoute(
    gb: *Gbuf,
    infra: *const Node,
    infra_path: CStr,
    svc_name: CStr,
    all_routes: NodeList,
    route_count: c_int,
) c_int {
    const infra_len = routePathLen(infra_path);
    const infra_is_root = infra_len <= 1;
    var best: ?*const Node = null;
    var best_exact = false;
    var best_len: usize = 0;
    for (0..ix(route_count)) |j| {
        const handler_route: *const Node = all_routes[j];
        if (isBrokerRoute(handler_route.qualified_name) or handler_route.qualified_name == null) {
            continue;
        }
        const file_matches = handler_route.file_path != null and
            c.strstr(handler_route.file_path, svc_name) != null;
        const is_prefix_route = hasPrefix(handler_route.qualified_name, "__route__ANY__");
        if (!file_matches and !is_prefix_route) continue;
        const handler_name: CStr = handler_route.name;
        const handler_path: CStr = if (handler_name != null) c.strchr(handler_name, '/') else null;
        if (handler_path == null) continue;
        const handler_len = routePathLen(handler_path);
        const handler_is_root = handler_len <= 1;

        // A root handler stands only for its own service's root URL; a URL
        // without a path names no endpoint beyond that root.
        var exact: bool = undefined;
        if (handler_is_root or infra_is_root) {
            if (!(handler_is_root and infra_is_root and file_matches)) continue;
            exact = true;
        } else {
            exact = handler_len == infra_len and
                c.strncmp(handler_path, infra_path, handler_len) == 0;
            if (!exact and
                !routePathCovers(handler_path, handler_len, infra_path, infra_len) and
                !routePathCovers(infra_path, infra_len, handler_path, handler_len))
            {
                continue;
            }
        }
        const better = if (best) |b|
            (if (exact != best_exact)
                exact
            else if (handler_len != best_len)
                handler_len > best_len
            else
                c.strcmp(handler_route.qualified_name, b.qualified_name) < 0)
        else
            true;
        if (better) {
            best = handler_route;
            best_exact = exact;
            best_len = handler_len;
        }
    }
    const winner = best orelse return 0;
    var fn_handles: EdgeList = null;
    var fn_hcount: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_target_type(gb, winner.id, "HANDLES", &fn_handles, &fn_hcount);
    for (0..ix(fn_hcount)) |fh| {
        _ = c.cbm_gbuf_insert_edge(gb, fn_handles[fh].*.source_id, infra.id, "HANDLES", "{\"source\":\"infra_match\"}");
    }
    return 1;
}

/// Phase 2: Match infra Route URLs to handler Route nodes by URL path + service name.
fn matchInfraRoutes(gb: *Gbuf) void {
    var all_routes: NodeList = null;
    var route_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, "Route", &all_routes, &route_count) != 0 or route_count == 0) {
        return;
    }

    var matched: c_int = 0;

    for (0..ix(route_count)) |i| {
        const infra: *const Node = all_routes[i];
        if (infra.qualified_name == null or !hasPrefix(infra.qualified_name, "__route__infra__")) {
            continue;
        }
        if (infra.name == null or c.strstr(infra.name, "://") == null) continue;

        const infra_path = urlPath(infra.name);
        var svc_buf: [c.CBM_SZ_128]u8 = undefined;
        const svc_name = extractServiceName(infra.name, &svc_buf);
        if (infra_path == null or svc_name == null) continue;

        matched += matchOneInfraRoute(gb, infra, infra_path, svc_name, all_routes, route_count);
    }

    if (matched > 0) logCount("pass.route_match", "infra_matched", matched);
}

// Phase 2a: Ensure all functions with route_path properties have Route+HANDLES edges.
// During incremental indexing, only changed files get Route nodes from extraction.
// This pass scans ALL Function/Method nodes and creates missing Route+HANDLES.

/// Extract a JSON string property value into buf. Returns true if found.
fn extractJsonProp(json: CStr, key: CStr, buf: []u8) bool {
    if (json == null) return false;
    var pattern: [c.CBM_SZ_64]u8 = undefined;
    _ = c.snprintf(&pattern, pattern.len, "\"%s\":\"", key);
    var p: CStr = c.strstr(json, &pattern);
    if (p == null) return false;
    p += c.strlen(&pattern);
    const end: CStr = c.strchr(p, '"');
    if (end == null or @intFromPtr(end) <= @intFromPtr(p)) return false;
    const len = util.ptrDiff(end, p);
    if (len >= buf.len) return false;
    @memcpy(buf[0..len], p[0..len]);
    buf[len] = 0;
    return true;
}

/// Process a single Function/Method node: create Route+HANDLES if it has route_path.
/// Returns 1 if a new HANDLES edge was created, 0 otherwise.
fn ensureOneDecoratorRoute(gb: *Gbuf, func: *const Node) c_int {
    if (func.properties_json == null) return 0;

    var path: [c.CBM_SZ_256]u8 = undefined;
    if (!extractJsonProp(func.properties_json, "route_path", &path)) return 0;
    if (path[0] != '/') return 0;

    var method: [c.CBM_SZ_16]u8 = @splat(0);
    method[0..3].* = "ANY".*;
    _ = extractJsonProp(func.properties_json, "route_method", &method);

    var route_qn: [ROUTE_QN_SIZE]u8 = undefined;
    var cpath: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(
        &route_qn,
        route_qn.len,
        "__route__%s__%s",
        @as(CStr, &method),
        cbm_route_canon_path(&path, &cpath, cpath.len),
    );
    const existing: ?*const Node = c.cbm_gbuf_find_by_qn(gb, &route_qn);

    var rprops: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&rprops, rprops.len, "{\"method\":\"%s\",\"source\":\"decorator\"}", @as(CStr, &method));
    const route_id = c.cbm_gbuf_upsert_node(gb, "Route", &path, &route_qn, if (func.file_path != null) func.file_path else "", 0, 0, &rprops);

    if (existing != null) {
        var existing_handles: EdgeList = null;
        var eh_count: c_int = 0;
        _ = c.cbm_gbuf_find_edges_by_target_type(gb, route_id, "HANDLES", &existing_handles, &eh_count);
        for (0..ix(eh_count)) |eh| {
            if (existing_handles[eh].*.source_id == func.id) return 0;
        }
    }

    var hprops: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(
        &hprops,
        hprops.len,
        "{\"handler\":\"%s\"}",
        if (func.qualified_name != null) func.qualified_name else @as(CStr, ""),
    );
    _ = c.cbm_gbuf_insert_edge(gb, func.id, route_id, "HANDLES", &hprops);
    return 1;
}

/// Phase 2a: Ensure all functions with route_path properties have Route+HANDLES edges.
fn ensureDecoratorRoutes(gb: *Gbuf) void {
    var created: c_int = 0;

    for ([_][*:0]const u8{ "Function", "Method" }) |label| {
        var nodes: NodeList = null;
        var count: c_int = 0;
        if (c.cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) continue;
        for (0..ix(count)) |i| {
            created += ensureOneDecoratorRoute(gb, nodes[i]);
        }
    }

    if (created > 0) logCount("pass.ensure_decorator_routes", "created", created);
}

// Phase 2b: Connect prefix Routes to decorator handler Functions.
// For each prefix Route (__route__ANY__/path), find the CALLS edge leading to it
// (from the registering file), derive the service directory, then find decorator
// Routes in that directory tree and create HANDLES from their handler Functions
// to the prefix Route. This bridges include_router → decorator → handler.

/// Bridge decorator handler Functions to a prefix Route. Returns number connected.
fn bridgeFuncsToPrefix(
    gb: *Gbuf,
    prefix_route: *const Node,
    registrar_path: CStr,
    dir_len: usize,
    prefix_segs: CStr,
) c_int {
    var funcs: NodeList = null;
    var func_count: c_int = 0;
    _ = c.cbm_gbuf_find_by_label(gb, "Function", &funcs, &func_count);

    var connected: c_int = 0;
    for (0..ix(func_count)) |fi| {
        const func: *const Node = funcs[fi];
        if (func.file_path == null) continue;
        if (c.strncmp(func.file_path, registrar_path, dir_len) != 0) continue;
        if (func.properties_json == null or c.strstr(func.properties_json, "\"route_path\"") == null) {
            continue;
        }
        if (prefix_segs != null and prefix_segs[0] != 0 and c.strstr(func.file_path, prefix_segs) == null) {
            continue;
        }
        _ = c.cbm_gbuf_insert_edge(gb, func.id, prefix_route.id, "HANDLES", "{\"source\":\"prefix_decorator_bridge\"}");
        connected += 1;
    }
    return connected;
}

/// Phase 2b: Connect prefix Routes to decorator handler Functions.
fn connectPrefixToDecorators(gb: *Gbuf) void {
    var routes: NodeList = null;
    var route_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, "Route", &routes, &route_count) != 0) return;

    var connected: c_int = 0;

    for (0..ix(route_count)) |ri| {
        const prefix_route: *const Node = routes[ri];
        if (prefix_route.qualified_name == null or
            !hasPrefix(prefix_route.qualified_name, "__route__ANY__/"))
        {
            continue;
        }

        // The root prefix "/" has no segment to scope handlers by, so bridging
        // it would claim every route handler under the registrar's directory.
        const prefix_path: CStr = prefix_route.name;
        const prefix_segs: CStr = if (prefix_path != null and prefix_path[0] == '/') prefix_path + 1 else prefix_path;
        if (prefix_segs == null or prefix_segs[0] == 0) continue;

        var calls_in: EdgeList = null;
        var calls_count: c_int = 0;
        _ = c.cbm_gbuf_find_edges_by_target_type(gb, prefix_route.id, "CALLS", &calls_in, &calls_count);

        // Every registering file bridges its own directory. Taking only the
        // first CALLS edge depended on parallel merge order. Repeated bridges
        // dedup in the gbuf.
        for (0..ix(calls_count)) |ci| {
            const registrar: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_id(gb, calls_in[ci].*.source_id)) orelse continue;
            if (registrar.file_path == null) continue;
            const last_slash: CStr = c.strrchr(registrar.file_path, '/');
            if (last_slash == null) continue;
            const dir_len = util.ptrDiff(last_slash, registrar.file_path) + 1;
            connected += bridgeFuncsToPrefix(gb, prefix_route, registrar.file_path, dir_len, prefix_segs);
        }
    }

    if (connected > 0) logCount("pass.prefix_bridge", "connected", connected);
}

// Phase 3: Create DATA_FLOWS edges by linking callers through Route to handlers.
// For each HTTP_CALLS/ASYNC_CALLS edge (caller → Route), find the HANDLES edge
// (handler → Route) and create DATA_FLOWS (caller → handler) with route context.

/// Check if a direct CALLS edge already exists between two nodes
fn hasDirectCall(gb: *const Gbuf, source: i64, target: i64) bool {
    var edges: EdgeList = null;
    var count: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_source_type(gb, source, "CALLS", &edges, &count);
    for (0..ix(count)) |i| {
        if (edges[i].*.target_id == target) return true;
    }
    return false;
}

/// Extract param_names from a node's properties_json.
/// Writes a comma-separated string into buf, or the empty string.
fn extractParamNames(node: ?*const Node, buf: []u8) void {
    buf[0] = 0;
    const n = node orelse return;
    if (n.properties_json == null) return;
    var p: CStr = c.strstr(n.properties_json, "\"param_names\":");
    if (p == null) return;
    p = c.strchr(p, '[');
    if (p == null) return;
    p += 1; // skip '['
    const end: CStr = c.strchr(p, ']');
    if (end == null or @intFromPtr(end) <= @intFromPtr(p)) return;
    var len = util.ptrDiff(end, p);
    if (len >= buf.len) len = buf.len - 1;
    @memcpy(buf[0..len], p[0..len]);
    buf[len] = 0;
}

/// Extract the "args" JSON fragment from an edge's properties.
/// Returns pointer into the properties string (not copied).
fn findArgsInProps(props: CStr) CStr {
    if (props == null) return null;
    const p: CStr = c.strstr(props, "\"args\":[");
    if (p == null) return null;
    return p + RN_ARGS_SKIP; // skip "args":[ , points to first { or ]
}

/// Append handler_params and caller_args to DATA_FLOWS props. Closes with '}'.
fn finishDataFlowProps(props: [*]u8, propsz: usize, pos_in: usize, handler_params: [*:0]const u8, args_json: CStr) void {
    var pos = pos_in;
    if (handler_params[0] != 0) {
        const w = c.snprintf(props + pos, propsz -% pos, ",\"handler_params\":[%s]", handler_params);
        if (w > 0) pos += @intCast(w);
    }
    if (args_json != null) {
        const w = c.snprintf(props + pos, propsz -% pos, ",\"caller_args\":[%.*s", @as(c_int, 400), args_json);
        if (w > 0) {
            pos += @intCast(w);
            const close: CStr = c.strchr(props + (pos - @as(usize, @intCast(w))) + RN_CALLEE_SKIP, ']');
            if (close != null and @intFromPtr(close) < @intFromPtr(props + propsz - 2)) {
                pos = util.ptrDiff(close, props) + 1;
            }
        }
    }
    if (pos < propsz - 1) {
        props[pos] = '}';
        props[pos + 1] = 0;
    }
}

const CallerEdgeRef = struct {
    source_id: i64,
    props: CStr,
    edge_type: CStr,
};

fn httpCallEdgeHasValidRoute(edge: *const Edge) bool {
    var url_buf: [c.CBM_SZ_512]u8 = undefined;
    const url = jsonExtract(edge.properties_json, "url_path", &url_buf);
    var callee_buf: [c.CBM_SZ_256]u8 = undefined;
    const callee = jsonExtract(edge.properties_json, "callee", &callee_buf);
    return c.cbm_service_pattern_is_http_route_literal(url, callee);
}

/// Try to create a DATA_FLOWS edge between caller and handler via a route.
/// Returns: 1=created, 0=skipped (self/duplicate), -1=skipped (has direct call).
fn tryCreateDataFlow(
    gb: *Gbuf,
    caller_id: i64,
    handler_id: i64,
    route: *const Node,
    edge_type: CStr,
    caller_props: CStr,
    via_infra: bool,
) c_int {
    @setEvalBranchQuota(10000);
    if (caller_id == handler_id) return 0;
    if (hasDirectCall(gb, caller_id, handler_id)) return c.CBM_NOT_FOUND;

    const args_json = findArgsInProps(caller_props);
    const handler_node: ?*const Node = c.cbm_gbuf_find_by_id(gb, handler_id);
    var handler_params: [c.CBM_SZ_512:0]u8 = undefined;
    extractParamNames(handler_node, &handler_params);

    // Route names/QNs are sliced source text (URL strings, decorator args) and
    // can contain quotes — escape them or the edge properties JSON is
    // malformed (aborts json_extract consumers incl. integrity_check).
    var esc_rname: [c.CBM_SZ_256]u8 = undefined;
    var esc_rqn: [c.CBM_SZ_512]u8 = undefined;
    _ = c.cbm_json_escape(&esc_rname, esc_rname.len, if (route.name != null) route.name else "");
    _ = c.cbm_json_escape(&esc_rqn, esc_rqn.len, if (route.qualified_name != null) route.qualified_name else "");
    var props: [c.CBM_SZ_2K]u8 = undefined;
    const n = if (via_infra)
        c.snprintf(
            &props,
            props.len,
            "{\"via\":\"%s\",\"route\":\"%s\",\"edge_type\":\"%s\",\"via_infra\":true",
            @as(CStr, &esc_rname),
            @as(CStr, &esc_rqn),
            edge_type,
        )
    else
        c.snprintf(
            &props,
            props.len,
            "{\"via\":\"%s\",\"route\":\"%s\",\"edge_type\":\"%s\"",
            @as(CStr, &esc_rname),
            @as(CStr, &esc_rqn),
            edge_type,
        );

    if (n > 0 and @as(usize, @intCast(n)) < props.len - RN_PROPS_MARGIN) {
        finishDataFlowProps(&props, props.len, @intCast(n), &handler_params, args_json);
    }
    // handler_params/args_json are sliced text fragments; truncation or stray
    // quotes can leave the assembled JSON malformed, which aborts every
    // json_extract consumer downstream. Validate and fall back to the minimal
    // envelope rather than persisting invalid properties.
    {
        const vdoc = c.yyjson_read(&props, c.strlen(&props), 0);
        if (vdoc == null) {
            _ = c.snprintf(&props, props.len, "{\"via\":\"%s\",\"edge_type\":\"%s\"}", @as(CStr, &esc_rname), edge_type);
        } else {
            c.yyjson_doc_free(vdoc);
        }
    }
    _ = c.cbm_gbuf_insert_edge(gb, caller_id, handler_id, "DATA_FLOWS", &props);
    return 1;
}

/// Collect extra handler IDs reachable via INFRA_MAPS chain from a route.
fn collectInfraHandlers(gb: *Gbuf, route_id: i64, out: []i64) usize {
    var infra_edges: EdgeList = null;
    var infra_count: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_source_type(gb, route_id, "INFRA_MAPS", &infra_edges, &infra_count);

    var n: usize = 0;
    for (0..ix(infra_count)) |ie| {
        var ep_handles: EdgeList = null;
        var ep_hcount: c_int = 0;
        _ = c.cbm_gbuf_find_edges_by_target_type(gb, infra_edges[ie].*.target_id, "HANDLES", &ep_handles, &ep_hcount);
        var eh: usize = 0;
        while (eh < ep_hcount and n < out.len) : (eh += 1) {
            out[n] = ep_handles[eh].*.source_id;
            n += 1;
        }
    }
    return n;
}

/// Collect caller edges (HTTP_CALLS + ASYNC_CALLS) targeting a route.
fn collectCallerEdges(gb: *Gbuf, route_id: i64, out: []CallerEdgeRef) usize {
    var n: usize = 0;
    var http_edges: EdgeList = null;
    var http_count: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_target_type(gb, route_id, "HTTP_CALLS", &http_edges, &http_count);
    var i: usize = 0;
    while (i < http_count and n < out.len) : (i += 1) {
        if (!httpCallEdgeHasValidRoute(http_edges[i])) continue;
        out[n] = .{
            .source_id = http_edges[i].*.source_id,
            .props = http_edges[i].*.properties_json,
            .edge_type = "HTTP_CALLS",
        };
        n += 1;
    }
    var async_edges: EdgeList = null;
    var async_count: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_target_type(gb, route_id, "ASYNC_CALLS", &async_edges, &async_count);
    i = 0;
    while (i < async_count and n < out.len) : (i += 1) {
        out[n] = .{
            .source_id = async_edges[i].*.source_id,
            .props = async_edges[i].*.properties_json,
            .edge_type = "ASYNC_CALLS",
        };
        n += 1;
    }
    return n;
}

/// Create DATA_FLOWS between callers and handlers for one route node.
fn createRouteDataFlows(gb: *Gbuf, route: *const Node, callers: []const CallerEdgeRef, flows: *c_int, skipped: *c_int) void {
    var handles_edges: EdgeList = null;
    var handles_count: c_int = 0;
    _ = c.cbm_gbuf_find_edges_by_target_type(gb, route.id, "HANDLES", &handles_edges, &handles_count);

    var extra_handlers: [c.CBM_SZ_32]i64 = undefined;
    const n_extra = collectInfraHandlers(gb, route.id, &extra_handlers);

    for (callers) |caller| {
        for (0..ix(handles_count)) |hi| {
            const rc = tryCreateDataFlow(gb, caller.source_id, handles_edges[hi].*.source_id, route, caller.edge_type, caller.props, false);
            if (rc > 0) {
                flows.* += 1;
            } else if (rc < 0) {
                skipped.* += 1;
            }
        }
        for (extra_handlers[0..n_extra]) |handler_id| {
            const rc = tryCreateDataFlow(gb, caller.source_id, handler_id, route, caller.edge_type, caller.props, true);
            if (rc > 0) {
                flows.* += 1;
            } else if (rc < 0) {
                skipped.* += 1;
            }
        }
    }
}

/// Find the enclosing proto service for a function by file + line range.
fn findEnclosingService(func: *const Node, services: []const [*c]const Node) ?*const Node {
    for (services) |service_c| {
        const service: *const Node = service_c;
        if (!streq(func.file_path, service.file_path)) continue;
        if (func.start_line >= service.start_line and func.end_line <= service.end_line) {
            return service;
        }
    }
    return null;
}

/// Phase 4: Create __grpc__ Route nodes from protobuf service definitions.
fn createGrpcRoutes(gb: *Gbuf) void {
    var classes: NodeList = null;
    var class_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, "Class", &classes, &class_count) != 0 or class_count == 0) {
        return;
    }

    // Every .proto class, uncapped: a fixed cap kept whichever classes the
    // parallel workers happened to emit first, so routes varied by run.
    const services = interop.mallocN([*c]const Node, ix(class_count));
    if (services == null) return;
    var svc_count: usize = 0;
    for (0..ix(class_count)) |i| {
        if (classes[i].*.file_path != null and c.strstr(classes[i].*.file_path, ".proto") != null) {
            services[svc_count] = classes[i];
            svc_count += 1;
        }
    }
    var funcs: NodeList = null;
    var func_count: c_int = 0;
    if (svc_count == 0 or
        c.cbm_gbuf_find_by_label(gb, "Function", &funcs, &func_count) != 0 or func_count == 0)
    {
        c.free(@ptrCast(services));
        return;
    }

    var grpc_routes: c_int = 0;
    for (0..ix(func_count)) |f| {
        const func: *const Node = funcs[f];
        if (func.file_path == null or c.strstr(func.file_path, ".proto") == null or func.name == null) {
            continue;
        }
        const svc = findEnclosingService(func, services[0..svc_count]) orelse continue;
        var route_qn: [ROUTE_QN_SIZE]u8 = undefined;
        _ = c.snprintf(&route_qn, route_qn.len, "__grpc__%s/%s", svc.name, func.name);

        var props: [c.CBM_SZ_128]u8 = undefined;
        _ = c.snprintf(&props, props.len, "{\"source\":\"proto\",\"service\":\"%s\"}", svc.name);

        const route_id = c.cbm_gbuf_upsert_node(gb, "Route", func.name, &route_qn, func.file_path, func.start_line, func.end_line, &props);
        _ = c.cbm_gbuf_insert_edge(gb, func.id, route_id, "HANDLES", "{\"via\":\"proto_rpc\"}");
        grpc_routes += 1;
    }
    c.free(@ptrCast(services));
    if (grpc_routes > 0) logCount("pass.route_nodes.grpc", "routes", grpc_routes);
}

/// Phase 3: Create DATA_FLOWS edges by linking callers through Route to handlers.
fn createDataFlows(gb: *Gbuf) void {
    var routes: NodeList = null;
    var route_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, "Route", &routes, &route_count) != 0 or route_count == 0) {
        return;
    }

    var flows: c_int = 0;
    var skipped: c_int = 0;

    for (0..ix(route_count)) |ri| {
        var caller_edges: [c.CBM_SZ_64]CallerEdgeRef = undefined;
        const n_callers = collectCallerEdges(gb, routes[ri].*.id, &caller_edges);
        createRouteDataFlows(gb, routes[ri], caller_edges[0..n_callers], &flows, &skipped);
    }

    if (flows > 0 or skipped > 0) {
        var buf1: [c.CBM_SZ_16]u8 = undefined;
        var buf2: [c.CBM_SZ_16]u8 = undefined;
        _ = c.snprintf(&buf1, buf1.len, "%d", flows);
        _ = c.snprintf(&buf2, buf2.len, "%d", skipped);
        interop.logInfo("pass.data_flows", .{
            @as(CStr, "created"), @as(CStr, &buf1), @as(CStr, "skipped_has_call"), @as(CStr, &buf2),
        });
    }
}

// ── SvelteKit filesystem-based route extractor ──────────────────────
//
// SvelteKit derives REST endpoints and SSR loaders from the filesystem
// layout under `src/routes/`. No `app.get(...)` style call exists for
// the call resolver to pick up, so we walk File nodes and synthesise Route
// nodes + HANDLES edges directly here.
//
// Recognised filenames (TypeScript and JavaScript variants):
//
//   +server.{ts,js}            → REST endpoints, exports GET/POST/...
//   +page.server.{ts,js}       → SSR page server module: load + actions
//   +layout.server.{ts,js}     → SSR layout server module: load
//
// Route path is derived from the directory chain under `routes/`:
//
//   apps/x/src/routes/foo/+server.ts        → /foo
//   apps/x/src/routes/api/items/+server.ts  → /api/items
//   apps/x/src/routes/(grp)/foo/+server.ts  → /foo            (group stripped)
//   apps/x/src/routes/[slug]/+server.ts     → /:slug          (param rewrite)
//   apps/x/src/routes/+server.ts            → /               (root)

const SKR_PATH_BUF = 1024;

const SveltekitKind = enum { none, server, page_server, layout_server };

/// Detect the SvelteKit kind of a file path: +server, +page.server,
/// +layout.server, or none when it is not a SvelteKit server-side route file.
fn sveltekitFileKind(file_path: CStr) SveltekitKind {
    if (file_path == null) return .none;
    // The basename must match one of the three patterns. We also require
    // a "/routes/" segment somewhere in the path so we don't snag files
    // in unrelated directories that happen to be named "+server.ts".
    if (c.strstr(file_path, "/routes/") == null) return .none;
    const slash: CStr = c.strrchr(file_path, '/');
    const base: CStr = if (slash != null) slash + 1 else file_path;
    if (streq(base, "+server.ts") or streq(base, "+server.js")) return .server;
    if (streq(base, "+page.server.ts") or streq(base, "+page.server.js")) return .page_server;
    if (streq(base, "+layout.server.ts") or streq(base, "+layout.server.js")) return .layout_server;
    return .none;
}

/// Compute the URL route path for a SvelteKit file path. Writes into
/// `out` (caller-provided buffer) and returns the buffer or NULL on
/// error. The leading "/" is always present; "/" is used for the root
/// route. Group segments wrapped in parentheses are stripped. Dynamic
/// params `[slug]` become `:slug`, and rest params `[...slug]` become
/// `*slug` so the result is recognisable as a path pattern.
fn sveltekitRoutePath(file_path: CStr, out: []u8) CStr {
    if (file_path == null or out.len <= 1) return null;
    const routes_seg: CStr = c.strstr(file_path, "/routes/");
    if (routes_seg == null) return null;
    // Walk segment-by-segment between "/routes/" and the trailing "+...".
    var p: [*]const u8 = routes_seg + "/routes/".len;
    const last_slash: CStr = c.strrchr(file_path, '/');
    if (last_slash == null or @intFromPtr(last_slash) < @intFromPtr(p)) {
        // File is directly under /routes/ — root route.
        out[0] = '/';
        out[1] = 0;
        return out.ptr;
    }
    // p points at first char after /routes/, last_slash points at the
    // slash before the +...filename. Iterate segments between them.
    const limit = out.len - 1;
    var pos: usize = 0;
    out[pos] = 0;
    while (@intFromPtr(p) < @intFromPtr(last_slash)) {
        var seg_end: [*]const u8 = last_slash;
        const next_slash: CStr = c.strchr(p, '/');
        if (next_slash != null and @intFromPtr(next_slash) <= @intFromPtr(last_slash)) {
            seg_end = next_slash;
        }
        const seg_len = util.ptrDiff(seg_end, p);
        if (seg_len > 0) {
            // Group `(name)` — skip entirely.
            if (p[0] == '(' and p[seg_len - 1] == ')') {
                p = seg_end + 1;
                continue;
            }
            // Emit "/" then rewritten segment.
            if (pos + 1 < limit) {
                out[pos] = '/';
                pos += 1;
            }
            // Dynamic param: `[slug]` → `:slug`, `[...slug]` → `*slug`.
            if (seg_len >= 2 and p[0] == '[' and p[seg_len - 1] == ']') {
                var inner: [*]const u8 = p + 1;
                var inner_len = seg_len - 2;
                if (inner_len >= 3 and c.strncmp(inner, "...", 3) == 0) {
                    if (pos < limit) {
                        out[pos] = '*';
                        pos += 1;
                    }
                    inner += 3;
                    inner_len -= 3;
                } else {
                    if (pos < limit) {
                        out[pos] = ':';
                        pos += 1;
                    }
                }
                // Strip an optional `=matcher` suffix after the param name.
                var copy_len = inner_len;
                for (inner[0..inner_len], 0..) |ch, i| {
                    if (ch == '=') {
                        copy_len = i;
                        break;
                    }
                }
                if (copy_len > limit - pos) copy_len = limit - pos;
                @memcpy(out[pos..][0..copy_len], inner[0..copy_len]);
                pos += copy_len;
            } else {
                var copy_len = seg_len;
                if (copy_len > limit - pos) copy_len = limit - pos;
                @memcpy(out[pos..][0..copy_len], p[0..copy_len]);
                pos += copy_len;
            }
            out[pos] = 0;
        }
        p = seg_end + 1;
    }
    if (pos == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    return out.ptr;
}

/// Returns the HTTP method string a function name maps to for a +server
/// file, or NULL if the function isn't a SvelteKit REST handler. The set
/// mirrors SvelteKit's documented HTTP verb exports.
fn sveltekitServerMethod(name: CStr) CStr {
    if (name == null) return null;
    const verbs = [_][*:0]const u8{ "GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "HEAD" };
    for (verbs) |verb| {
        if (streq(name, verb)) return verb;
    }
    // `fallback` catches any verb not explicitly exported.
    if (streq(name, "fallback")) return "ANY";
    return null;
}

const SveltekitCtx = struct {
    gb: *Gbuf,
    routes_created: c_int = 0,
    handles_created: c_int = 0,
    files_seen: c_int = 0,
};

/// Process one File node: if it matches a SvelteKit server-side file,
/// synthesise a Route node and HANDLES edges from any handler functions
/// (or actions Variable) defined in the file.
fn sveltekitFileVisitor(node_c: [*c]const Node, userdata: ?*anyopaque) callconv(.c) void {
    const ctx: *SveltekitCtx = @ptrCast(@alignCast(userdata));
    const node: *const Node = @as(?*const Node, node_c) orelse return;
    if (node.label == null or !streq(node.label, "File")) return;
    const kind = sveltekitFileKind(node.file_path);
    if (kind == .none) return;
    ctx.files_seen += 1;

    var route_path: [SKR_PATH_BUF]u8 = undefined;
    if (sveltekitRoutePath(node.file_path, &route_path) == null) return;

    // Find every DEFINES edge from this file to a Function or Variable.
    var edges: EdgeList = null;
    var edge_count: c_int = 0;
    if (c.cbm_gbuf_find_edges_by_source_type(ctx.gb, node.id, "DEFINES", &edges, &edge_count) != 0 or
        edge_count == 0)
    {
        return;
    }

    for (0..ix(edge_count)) |i| {
        const child: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_id(ctx.gb, edges[i].*.target_id)) orelse continue;
        if (child.name == null or child.label == null) continue;
        const is_fn = streq(child.label, "Function");
        const is_var = streq(child.label, "Variable");
        if (!is_fn and !is_var) continue;

        var method: CStr = null;
        var is_actions = false;
        switch (kind) {
            .server => method = sveltekitServerMethod(child.name),
            .page_server => {
                if (is_fn and streq(child.name, "load")) {
                    method = "LOAD";
                } else if (streq(child.name, "actions")) {
                    // SvelteKit form actions: emit a single Route at this
                    // path with method=ACTIONS; finer-grained action names
                    // aren't exposed as top-level identifiers.
                    method = "ACTIONS";
                    is_actions = is_var;
                }
            },
            .layout_server => {
                if (is_fn and streq(child.name, "load")) method = "LOAD";
            },
            .none => {},
        }
        if (method == null) continue;

        var route_qn: [ROUTE_QN_SIZE]u8 = undefined;
        var cpath: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(
            &route_qn,
            route_qn.len,
            "__route__%s__%s",
            method,
            cbm_route_canon_path(&route_path, &cpath, cpath.len),
        );
        var route_props: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&route_props, route_props.len, "{\"method\":\"%s\",\"framework\":\"sveltekit\"}", method);
        const route_id = c.cbm_gbuf_upsert_node(ctx.gb, "Route", &route_path, &route_qn, "", 0, 0, &route_props);
        if (route_id == 0) continue;
        ctx.routes_created += 1;

        var hprops: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(
            &hprops,
            hprops.len,
            "{\"handler\":\"%s\",\"framework\":\"sveltekit\"%s}",
            if (child.qualified_name != null) child.qualified_name else child.name,
            @as(CStr, if (is_actions) ",\"via\":\"actions_object\"" else ""),
        );
        _ = c.cbm_gbuf_insert_edge(ctx.gb, child.id, route_id, "HANDLES", &hprops);
        ctx.handles_created += 1;
    }
}

/// Scan all File nodes for SvelteKit server modules and synthesise
/// Route + HANDLES nodes from the filesystem convention.
fn createSveltekitRoutes(gb: *Gbuf) void {
    var ctx: SveltekitCtx = .{ .gb = gb };
    c.cbm_gbuf_foreach_node(gb, sveltekitFileVisitor, &ctx);
    if (ctx.files_seen > 0) {
        var b1: [c.CBM_SZ_16]u8 = undefined;
        var b2: [c.CBM_SZ_16]u8 = undefined;
        var b3: [c.CBM_SZ_16]u8 = undefined;
        _ = c.snprintf(&b1, b1.len, "%d", ctx.files_seen);
        _ = c.snprintf(&b2, b2.len, "%d", ctx.routes_created);
        _ = c.snprintf(&b3, b3.len, "%d", ctx.handles_created);
        interop.logInfo("pass.sveltekit_routes", .{
            @as(CStr, "files"),   @as(CStr, &b1),
            @as(CStr, "routes"),  @as(CStr, &b2),
            @as(CStr, "handles"), @as(CStr, &b3),
        });
    }
}

pub export fn cbm_pipeline_create_route_nodes(gb_c: ?*Gbuf) void {
    const gb = gb_c orelse return;

    var ctx: RouteCtx = .{ .gb = gb, .created = 0 };
    c.cbm_gbuf_foreach_edge(gb, routeEdgeVisitor, &ctx);

    if (ctx.created > 0) logCount("pass.route_nodes", "created", ctx.created);

    // Phase 2a: ensure all functions with route_path have Route+HANDLES.
    // Handles incremental mode where unchanged files don't re-extract.
    ensureDecoratorRoutes(gb);

    // Phase 2b: connect prefix Routes to decorator handler Functions.
    // Must run BEFORE matchInfraRoutes so infra matching can find
    // HANDLES edges on prefix Routes for the bridge.
    connectPrefixToDecorators(gb);

    // Phase 2b: match infra Routes to handler Routes by URL path
    matchInfraRoutes(gb);

    // Phase 3: create DATA_FLOWS edges through Routes
    createDataFlows(gb);

    // Phase 4: create gRPC Route nodes from protobuf service definitions.
    // Scans Class nodes from .proto files, follows DEFINES_METHOD edges
    // to find rpc methods, creates __grpc__ServiceName/MethodName Route nodes.
    createGrpcRoutes(gb);

    // Phase 5: filesystem-based SvelteKit routes (+server / +page.server /
    // +layout.server) — no call-site equivalent for the call resolver to
    // pick up, so we walk File nodes directly here.
    createSveltekitRoutes(gb);
}
