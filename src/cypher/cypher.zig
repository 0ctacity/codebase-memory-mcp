//! cypher.zig — Cypher query executor.
//!
//! Implements the executing half of src/cypher/cypher.h: cbm_cypher_execute,
//! cbm_cypher_result_free, cbm_cypher_classify_plan, the cross-join bound and
//! the deadline test hooks. The lexer and parser are in parser.zig.
//!
//! Matches a subset of Cypher against cbm_store: MATCH patterns with
//! relationships, WHERE filters, WITH, UNWIND, UNION and RETURN with
//! aggregates, ORDER BY, SKIP, LIMIT and DISTINCT. Results are malloc'd C
//! structs that callers release with cbm_cypher_result_free.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const parser = @import("parser.zig");

const CStr = parser.CStr;
const ix = parser.ix;
const narrow = parser.narrow;
const heapStrdup = parser.heapStrdup;
const strFree = parser.strFree;
const bprint = parser.bprint;
const sp = parser.sp;
const ciEq = parser.ciEq;
const isScalarValueFunc = parser.isScalarValueFunc;
const mallocN = parser.mallocN;
const callocN = parser.callocN;
const reallocN = parser.reallocN;
const safeReallocN = parser.safeReallocN;

const Node = c.cbm_node_t;
const Edge = c.cbm_edge_t;
const Store = c.cbm_store_t;
const Cond = c.cbm_condition_t;
const Expr = c.cbm_expr_t;
const ReturnItem = c.cbm_return_item_t;
const ReturnClause = c.cbm_return_clause_t;
const Query = c.cbm_query_t;
const Row = [*c]CStr;

const NOT_FOUND: c_int = c.CBM_NOT_FOUND;
const FOUND_NONE: c_int = -1; // search miss sentinel
const MAX_VARS = 16; // max Cypher variables in a query
const MAX_EDGE_VARS = 8; // max edge variables
const GROWTH_10 = 10; // binding growth factor
const GROWTH_2 = 2; // geometric buffer growth
const PROP_BUFS = 8; // rotating property buffers
const PROP_BUF_SZ = 512;
const EBUF_MASK = 7; // mask for the edge-property ring (8 entries)
const NODE_COLS = 4; // columns per node var: name, qn, label, file
const EDGE_COLS = 3; // columns per edge var: name, qn, label
const COL_BUF = 48; // max column buffer (16 vars * 3 cols)
const MAX_COLS = 32; // projection rows are this many columns wide
const DBL_MAX = 1e308;

/// execute_single binds the first pattern's leading node even when the query
/// leaves it unnamed, so the engine needs a name for it. The binding slot is
/// real, which is why checkPatternVarCapacity counts it.
const ANON_HEAD_VAR = "_n0";

fn eql(a: CStr, b: CStr) bool {
    return c.strcmp(a, b) == 0;
}

/// A C count as a for-range bound; a negative count is an empty range, as
/// in a C `for (int i = 0; i < n; i++)` loop.
fn cnt(n: c_int) usize {
    return @intCast(@max(n, 0));
}

fn At(comptime P: type) type {
    const info = @typeInfo(P).pointer;
    return if (info.attrs.@"const") *const info.child else *info.child;
}

/// C `&p[i]` as a single-item pointer. In Zig `&p[i]` on a C pointer is
/// `*allowzero`, which neither coerces to `*T` nor allows field access.
fn at(p: anytype, i: usize) At(@TypeOf(p)) {
    return p + i;
}

const Str = [*:0]const u8;

fn log(level: c.CBMLogLevel, msg: Str, kvs: anytype) void {
    @call(.auto, c.cbm_log, .{ level, msg } ++ kvs ++ .{@as(?Str, null)});
}

fn logWarn(msg: Str, kvs: anytype) void {
    log(c.CBM_LOG_WARN, msg, kvs);
}

/// C's (long long) conversion of a double. Out-of-range values and NaN are
/// undefined in C; this gives what the hardware conversion gives, so the
/// result matches the C engine on each target instead of panicking.
fn truncToI64(d: f64) i64 {
    if (std.math.isNan(d) or d >= 0x1p63 or d < -0x1p63) {
        if (builtin.cpu.arch.isAARCH64()) {
            if (std.math.isNan(d)) return 0;
            return if (d > 0) std.math.maxInt(i64) else std.math.minInt(i64);
        }
        return std.math.minInt(i64);
    }
    return @intFromFloat(d);
}

// ══════════════════════════════════════════════════════════════════
//  EXECUTOR
// ══════════════════════════════════════════════════════════════════

/// A binding: maps variable names to nodes and/or edges.
const Binding = extern struct {
    var_names: [MAX_VARS]CStr, // variable names (nodes)
    var_nodes: [MAX_VARS]Node, // node data
    var_count: c_int,
    edge_var_names: [MAX_EDGE_VARS]CStr, // variable names (edges)
    edge_vars: [MAX_EDGE_VARS]Edge, // edge data
    edge_var_count: c_int,
    store: ?*Store, // for computing in_degree/out_degree on demand
};

const EMPTY_BINDING = std.mem.zeroes(Binding);

/// A string field of a node by property name ("" for a NULL field), or null
/// when prop names no string field.
fn nodeStringField(n: *const Node, prop: CStr) CStr {
    const val: CStr = if (eql(prop, "name"))
        n.name
    else if (eql(prop, "qualified_name"))
        n.qualified_name
    else if (eql(prop, "label"))
        n.label
    else if (eql(prop, "file_path"))
        n.file_path
    else
        return null;
    return if (val != null) val else "";
}

// Computed and JSON-derived values live in rotating thread-local buffers: a
// single row (or an ORDER-BY comparison) reads several of these before any of
// them is copied out, so returning one shared buffer would alias every column
// to the last value read. Mirrors edgeProp's rotation.
threadlocal var node_prop_bufs: [PROP_BUFS][PROP_BUF_SZ]u8 = std.mem.zeroes([PROP_BUFS][PROP_BUF_SZ]u8);
threadlocal var node_prop_idx: usize = 0;

/// Node property by name. store may be null; only the virtual degree
/// properties need it.
fn nodeProp(n: ?*const Node, prop: CStr, store: ?*Store) CStr {
    const node = n orelse return "";
    if (prop == null) return "";
    const str = nodeStringField(node, prop);
    if (str != null and str[0] != 0) return str;
    const out = &node_prop_bufs[node_prop_idx];
    node_prop_idx = (node_prop_idx + 1) % PROP_BUFS;

    if (eql(prop, "start_line")) {
        _ = bprint(out, "{d}", .{node.start_line});
        return out;
    }
    if (eql(prop, "end_line")) {
        _ = bprint(out, "{d}", .{node.end_line});
        return out;
    }
    // Virtual computed properties: in_degree/out_degree via CALLS edges.
    // Enables Cypher dead-code detection: WHERE n.in_degree = '0'.
    if (store != null and (eql(prop, "in_degree") or eql(prop, "out_degree"))) {
        var in_deg: c_int = 0;
        var out_deg: c_int = 0;
        c.cbm_store_node_degree(store, node.id, &in_deg, &out_deg);
        const val = if (eql(prop, "in_degree")) in_deg else out_deg;
        _ = bprint(out, "{d}", .{val});
        return out;
    }
    // Fall back to any value stored in the node's properties JSON — exposes the
    // extraction metrics (complexity, cognitive, loop_count, loop_depth,
    // transitive_loop_depth, recursive) and any other persisted property to
    // WHERE/RETURN, e.g. WHERE n.loop_depth >= 2.
    if (node.properties_json != null and node.properties_json[0] == '{') {
        const v = jsonExtractProp(node.properties_json, prop, out);
        if (v != null and v[0] != 0) return v;
    }
    return "";
}

/// Extract a value from a properties JSON object by key into buf. Returns buf,
/// empty when the key is absent. Handles string values ("key":"value"),
/// arrays and objects (copied through the matching close bracket) and numeric
/// or other bare values ("key":1.5).
fn jsonExtractProp(json: CStr, key: CStr, buf: []u8) CStr {
    if (json == null or key == null) {
        buf[0] = 0;
        return buf.ptr;
    }
    // Build search pattern: "key":
    var pattern: [256]u8 = undefined;
    _ = bprint(&pattern, "\"{s}\":", .{sp(key)});
    var p: CStr = c.strstr(json, &pattern);
    if (p == null) {
        buf[0] = 0;
        return buf.ptr;
    }
    p += c.strlen(&pattern);
    while (p[0] == ' ' or p[0] == '\t') p += 1;
    const last = buf.len - 1;
    var i: usize = 0;
    if (p[0] == '"') {
        // String value. Keep escape pairs intact: an escaped quote must not end
        // the value early.
        p += 1;
        while (p[0] != 0 and p[0] != '"' and i < last) {
            if (p[0] == '\\' and p[1] != 0 and i + 1 < last) {
                buf[i] = p[0];
                i += 1;
                p += 1;
            }
            buf[i] = p[0];
            i += 1;
            p += 1;
        }
    } else if (p[0] == '[' or p[0] == '{') {
        // Array or object value: copy through the matching close bracket
        var depth: c_int = 0;
        var in_string = false;
        while (p[0] != 0 and i < last) {
            const ch = p[0];
            p += 1;
            buf[i] = ch;
            i += 1;
            if (in_string) {
                if (ch == '\\' and p[0] != 0 and i < last) {
                    buf[i] = p[0];
                    i += 1;
                    p += 1;
                } else if (ch == '"') {
                    in_string = false;
                }
            } else if (ch == '"') {
                in_string = true;
            } else if (ch == '[' or ch == '{') {
                depth += 1;
            } else if (ch == ']' or ch == '}') {
                depth -= 1;
                if (depth == 0) break;
            }
        }
    } else {
        // Numeric or other value
        while (p[0] != 0 and p[0] != ',' and p[0] != '}' and p[0] != ' ' and i < last) {
            buf[i] = p[0];
            i += 1;
            p += 1;
        }
    }
    buf[i] = 0;
    return buf.ptr;
}

// Rotating buffers so several edge properties can be read per row (e.g.
// projecting r.url_path and r.confidence together). Unlike node_prop_bufs
// these are process-wide, as in the C engine this replaced: queries running
// on different threads share the ring.
var edge_prop_bufs: [PROP_BUFS][PROP_BUF_SZ]u8 = std.mem.zeroes([PROP_BUFS][PROP_BUF_SZ]u8);
var edge_prop_idx: c_int = 0;

/// Edge property by name.
fn edgeProp(e: ?*const Edge, prop: CStr) CStr {
    const edge = e orelse return "";
    if (prop == null) return "";
    if (eql(prop, "type")) return if (edge.type != null) edge.type else "";
    const buf = &edge_prop_bufs[ix(edge_prop_idx & EBUF_MASK)];
    edge_prop_idx +%= 1;
    _ = jsonExtractProp(edge.properties_json, prop, buf);
    return buf;
}

/// Find an edge variable in a binding.
fn bindingGetEdge(b: *Binding, v: CStr) ?*Edge {
    for (0..ix(b.edge_var_count)) |i| {
        if (eql(b.edge_var_names[i], v)) return &b.edge_vars[i];
    }
    return null;
}

/// Find a variable's node in a binding.
fn bindingGet(b: *Binding, v: CStr) ?*Node {
    for (0..ix(b.var_count)) |i| {
        if (eql(b.var_names[i], v)) return &b.var_nodes[i];
    }
    return null;
}

/// Deep copy a node: heap-dup all string fields so the binding owns them.
fn nodeDeepCopy(dst: *Node, src: *const Node) void {
    dst.* = src.*;
    dst.project = heapStrdup(src.project);
    dst.label = heapStrdup(src.label);
    dst.name = heapStrdup(src.name);
    dst.qualified_name = heapStrdup(src.qualified_name);
    dst.file_path = heapStrdup(src.file_path);
    dst.properties_json = heapStrdup(src.properties_json);
}

fn nodeFieldsFree(n: *Node) void {
    strFree(&n.project);
    strFree(&n.label);
    strFree(&n.name);
    strFree(&n.qualified_name);
    strFree(&n.file_path);
    strFree(&n.properties_json);
}

/// Deep copy an edge (binding owns the strings).
fn edgeDeepCopy(dst: *Edge, src: *const Edge) void {
    dst.* = src.*;
    dst.project = heapStrdup(src.project);
    dst.type = heapStrdup(src.type);
    dst.properties_json = heapStrdup(src.properties_json);
}

fn edgeFieldsFree(e: *Edge) void {
    strFree(&e.project);
    strFree(&e.type);
    strFree(&e.properties_json);
}

/// Set an edge variable in a binding.
fn bindingSetEdge(b: *Binding, v: CStr, edge: *const Edge) void {
    // Check existing — free old fields first
    for (0..ix(b.edge_var_count)) |i| {
        if (eql(b.edge_var_names[i], v)) {
            edgeFieldsFree(&b.edge_vars[i]);
            edgeDeepCopy(&b.edge_vars[i], edge);
            return;
        }
    }
    if (b.edge_var_count >= MAX_EDGE_VARS) return;
    b.edge_var_names[ix(b.edge_var_count)] = v; // not owned — points to AST string
    edgeDeepCopy(&b.edge_vars[ix(b.edge_var_count)], edge);
    b.edge_var_count += 1;
}

/// Free all deep-copied nodes and edges in a binding.
fn bindingFree(b: *Binding) void {
    for (0..ix(b.var_count)) |i| nodeFieldsFree(&b.var_nodes[i]);
    for (0..ix(b.edge_var_count)) |i| edgeFieldsFree(&b.edge_vars[i]);
}

/// Deep-copy a binding (so source and dest own separate string copies).
fn bindingCopy(dst: *Binding, src: *const Binding) void {
    dst.var_count = src.var_count;
    for (0..ix(src.var_count)) |i| {
        dst.var_names[i] = src.var_names[i]; // AST-owned, not freed
        nodeDeepCopy(&dst.var_nodes[i], &src.var_nodes[i]);
    }
    dst.edge_var_count = src.edge_var_count;
    for (0..ix(src.edge_var_count)) |i| {
        dst.edge_var_names[i] = src.edge_var_names[i]; // AST-owned
        edgeDeepCopy(&dst.edge_vars[i], &src.edge_vars[i]);
    }
    dst.store = src.store;
}

/// Deep-copy a node into a binding (binding owns the strings).
fn bindingSet(b: *Binding, v: CStr, node: *const Node) void {
    // Check existing — free old fields first
    for (0..ix(b.var_count)) |i| {
        if (eql(b.var_names[i], v)) {
            nodeFieldsFree(&b.var_nodes[i]);
            nodeDeepCopy(&b.var_nodes[i], node);
            return;
        }
    }
    if (b.var_count >= MAX_VARS) return;
    b.var_names[ix(b.var_count)] = v; // not owned — points to AST string
    nodeDeepCopy(&b.var_nodes[ix(b.var_count)], node);
    b.var_count += 1;
}

// One buffer per thread for a WHERE function value: eval_condition consumes
// it before any other condition is resolved.
threadlocal var cond_func_buf: [PROP_BUF_SZ]u8 = std.mem.zeroes([PROP_BUF_SZ]u8);

/// Resolve the actual property value for a condition from a binding.
fn resolveConditionValue(cond: *const Cond, b: *Binding) CStr {
    // Multi-arg scalar function LHS: coalesce(f.depth, 0) >= 2 (#874).
    // Evaluated through the same code path as RETURN projections.
    if (cond.func != null) {
        var item = std.mem.zeroes(ReturnItem);
        item.variable = cond.variable;
        item.property = cond.property;
        item.func = cond.func;
        item.args = cond.args;
        item.arg_count = cond.arg_count;
        return evalMultiargFunc(b, &item, &cond_func_buf);
    }

    if (bindingGetEdge(b, cond.variable)) |e| return edgeProp(e, cond.property);
    const n = bindingGet(b, cond.variable) orelse return null; // unbound variable
    if (cond.property != null) return nodeProp(n, cond.property, b.store);
    // Bare alias (e.g. post-WITH virtual var) — use node name directly
    return if (n.name != null) n.name else "";
}

fn regexMatches(pattern: CStr, s: CStr) ?bool {
    var re: c.cbm_regex_t = undefined;
    if (c.cbm_regcomp(&re, pattern, c.CBM_REG_EXTENDED | c.CBM_REG_NOSUB) != 0) return null;
    const rc = c.cbm_regexec(&re, s, 0, null, 0);
    c.cbm_regfree(&re);
    return rc == 0;
}

/// Evaluate a comparison operator between actual and expected strings.
fn evalComparisonOp(op: CStr, actual: CStr, expected: CStr) bool {
    if (eql(op, "=")) return eql(actual, expected);
    if (eql(op, "<>")) return !eql(actual, expected);
    if (eql(op, "=~")) return regexMatches(expected, actual) orelse false;
    if (eql(op, "CONTAINS")) return c.strstr(actual, expected) != null;
    if (eql(op, "STARTS WITH")) return c.strncmp(actual, expected, c.strlen(expected)) == 0;
    if (eql(op, "ENDS WITH")) {
        const alen = c.strlen(actual);
        const elen = c.strlen(expected);
        return alen >= elen and eql(actual + (alen - elen), expected);
    }
    if (eql(op, ">") or eql(op, "<") or eql(op, ">=") or eql(op, "<=")) {
        const a = c.strtod(actual, null);
        const exp_val = c.strtod(expected, null);
        if (op[0] == '>' and op[1] == '=') return a >= exp_val;
        if (op[0] == '<' and op[1] == '=') return a <= exp_val;
        if (op[0] == '>') return a > exp_val;
        return a < exp_val;
    }
    return false;
}

/// Whether the bound node has an edge of type (any type when null) in a
/// direction: 0 outbound, 1 inbound, 2 either.
fn nodeHasEdge(store: ?*Store, id: i64, edge_type: CStr, dir: c_int) bool {
    var result = false;
    var edges: [*c]Edge = null;
    var edge_count: c_int = 0;
    if (dir != 1) { // outbound or any
        if (edge_type != null) {
            _ = c.cbm_store_find_edges_by_source_type(store, id, edge_type, &edges, &edge_count);
        } else {
            _ = c.cbm_store_find_edges_by_source(store, id, &edges, &edge_count);
        }
        result = edge_count > 0;
        c.cbm_store_free_edges(edges, edge_count);
    }
    if (!result and dir != 0) { // inbound or any
        edges = null;
        edge_count = 0;
        if (edge_type != null) {
            _ = c.cbm_store_find_edges_by_target_type(store, id, edge_type, &edges, &edge_count);
        } else {
            _ = c.cbm_store_find_edges_by_target(store, id, &edges, &edge_count);
        }
        result = edge_count > 0;
        c.cbm_store_free_edges(edges, edge_count);
    }
    return result;
}

/// Evaluate a WHERE condition against a binding.
fn evalCondition(cond: *const Cond, b: *Binding) bool {
    // Label test: WHERE n:Label (#241) — compare the bound node's label
    // directly rather than a property value.
    if (eql(cond.op, "HAS_LABEL")) {
        const n = bindingGet(b, cond.variable);
        const result = n != null and n.?.label != null and cond.value != null and
            eql(n.?.label, cond.value);
        return result != cond.negated;
    }

    // EXISTS { (var)-[:TYPE]->() }: does the bound node have any edge of the
    // given type in the requested direction?
    if (eql(cond.op, "EXISTS")) {
        var result = false;
        if (bindingGet(b, cond.variable)) |n| {
            if (b.store != null) result = nodeHasEdge(b.store, n.id, cond.value, cond.exists_dir);
        }
        return result != cond.negated;
    }

    const actual = resolveConditionValue(cond, b);
    if (actual == null) return true;

    // IS NULL / IS NOT NULL
    if (eql(cond.op, "IS NULL")) return (actual[0] == 0) != cond.negated;
    if (eql(cond.op, "IS NOT NULL")) return (actual[0] != 0) != cond.negated;

    // IN [...]
    if (eql(cond.op, "IN")) {
        var result = false;
        for (0..ix(cond.in_value_count)) |i| {
            if (eql(actual, cond.in_values[i])) {
                result = true;
                break;
            }
        }
        return result != cond.negated;
    }

    var expected: CStr = cond.value;
    if (cond.value_variable != null) {
        var rhs = std.mem.zeroes(Cond);
        rhs.variable = cond.value_variable;
        rhs.property = cond.value_property;
        expected = resolveConditionValue(&rhs, b);
        if (expected == null) return true; // unbound, same as an unbound left side
    }
    return evalComparisonOp(cond.op, actual, expected) != cond.negated;
}

/// Expression tree evaluator. Recurses into NOT and into the left operand of a
/// binary node; a chain of one operator is linked along the right child (see
/// parser.zig exprChainLink) and is walked here in a loop.
fn evalExpr(e: [*c]const Expr, b: *Binding) bool {
    if (e == null) return true;
    if (e.*.type == c.EXPR_CONDITION) return evalCondition(&e.*.cond, b);
    if (e.*.type == c.EXPR_NOT) return !evalExpr(e.*.left, b);
    // AND / OR / XOR. The nested form `a OP (b OP (c ...))` evaluates a, then
    // b, then c, and AND/OR stop at the first operand that decides the result;
    // the loop does exactly that, one link per iteration. A right child that
    // is not a link of the same operator is the chain's last operand.
    const ty = e.*.type;
    var acc = evalExpr(e.*.left, b);
    var rest: [*c]const Expr = e.*.right;
    while (true) {
        if ((ty == c.EXPR_AND and !acc) or (ty == c.EXPR_OR and acc)) return acc;
        const linked = rest != null and rest.*.type == ty;
        const v = evalExpr(if (linked) rest.*.left else rest, b);
        // AND reaches here only with acc true, OR only with acc false, so the
        // combined value is v itself; XOR keeps the running parity.
        acc = if (ty == c.EXPR_XOR) acc != v else v;
        if (!linked) return acc;
        rest = rest.*.right;
    }
}

/// Evaluate a WHERE clause — the expression tree when there is one, else the
/// legacy flat condition list.
fn evalWhere(w: [*c]const c.cbm_where_clause_t, b: *Binding) bool {
    if (w == null) return true;
    if (w.*.root != null) return evalExpr(w.*.root, b);

    // Legacy flat evaluation
    if (w.*.count == 0) return true;
    const is_and = w.*.op != null and eql(w.*.op, "AND");
    for (0..ix(w.*.count)) |i| {
        const r = evalCondition(at(w.*.conditions, i), b);
        if (is_and and !r) return false;
        if (!is_and and r) return true;
    }
    return is_and;
}

/// Check if a string value looks like a regex pattern.
fn looksLikeRegex(s: CStr) bool {
    if (s == null) return false;
    return c.strstr(s, ".*") != null or c.strstr(s, ".+") != null or c.strchr(s, '[') != null or
        c.strchr(s, '(') != null or c.strchr(s, '|') != null or c.strchr(s, '^') != null or
        c.strchr(s, '$') != null;
}

/// Check inline property filters. Values that look like regex patterns are
/// matched with POSIX ERE; plain values use exact strcmp.
fn checkInlineProps(n: *const Node, props: [*c]const c.cbm_prop_filter_t, count: c_int, store: ?*Store) bool {
    for (0..cnt(count)) |i| {
        const actual = nodeProp(n, props[i].key, store);
        const matched = if (looksLikeRegex(props[i].value))
            regexMatches(props[i].value, actual) orelse eql(actual, props[i].value)
        else
            eql(actual, props[i].value);
        if (!matched) return false;
    }
    return true;
}

// ── Result building helpers ────────────────────────────────────────

const ResultBuilder = struct {
    rows: [*c]Row = null,
    row_count: c_int = 0,
    row_cap: c_int = 0,
    columns: [*c]CStr = null,
    col_count: c_int = 0,
};

fn rbInit(rb: *ResultBuilder) void {
    rb.* = .{ .row_cap = 32 };
    rb.rows = mallocN(Row, ix(rb.row_cap));
}

fn rbSetColumns(rb: *ResultBuilder, cols: [*c]const CStr, count: c_int) void {
    rb.columns = mallocN(CStr, if (count > 0) ix(count) else 1);
    for (0..cnt(count)) |i| rb.columns[i] = heapStrdup(cols[i]);
    rb.col_count = count;
}

fn rbAddRow(rb: *ResultBuilder, values: [*c]const CStr) void {
    if (rb.row_count >= rb.row_cap) {
        rb.row_cap *= 2;
        rb.rows = safeReallocN(Row, rb.rows, ix(rb.row_cap));
    }
    const row = mallocN(CStr, if (rb.col_count > 0) ix(rb.col_count) else 1);
    for (0..cnt(rb.col_count)) |i| {
        row[i] = heapStrdup(if (values[i] != null) values[i] else "");
    }
    rb.rows[ix(rb.row_count)] = row;
    rb.row_count += 1;
}

fn rowFree(row: Row, cols: c_int) void {
    for (0..cnt(cols)) |col| strFree(&row[col]);
    c.free(@ptrCast(row));
}

// ── Main execution ─────────────────────────────────────────────────

/// Hard ceiling: queries returning more than this trigger an error instead of
/// data. Prevents accidental multi-GB JSON payloads from an unbounded
/// MATCH (n) RETURN n.
const RESULT_CEILING = 100000;

// Wall-clock execution deadline (upstream 6bd11ffb, #601). The row ceiling
// only fires once rows exist, but an unbounded OPTIONAL MATCH or a GROUP BY
// with ~one group per node does O(bindings x groups) work for minutes before
// a single row is produced, so the ceiling never trips and the caller hangs.
// A monotonic deadline aborts runaway queries with a clear, actionable error.
// Checked (throttled) in the scan, expansion and aggregation hot loops.
const DEADLINE_BUDGET_MS = 30000; // 30s: generous for legit heavy queries
const DEADLINE_CHECK_MASK = 0x3FF; // sample the clock every 1024 iterations

threadlocal var deadline_ms: u64 = 0; // absolute; 0 = disarmed
threadlocal var timed_out = false;
threadlocal var deadline_override_ms: i64 = -1; // test hook; <0 = default
threadlocal var test_expire_site: CStr = null; // test hook

fn deadlineArm() void {
    timed_out = false;
    const budget: i64 = if (deadline_override_ms >= 0) deadline_override_ms else DEADLINE_BUDGET_MS;
    deadline_ms = c.cbm_now_ms() +% @as(u64, @bitCast(budget));
}

/// True once the query has run past its wall-clock budget. Sticky: after the
/// first trip every later call returns true, so later loops short-circuit.
fn deadlineExceeded() bool {
    if (timed_out) return true;
    if (deadline_ms == 0) return false;
    if (c.cbm_now_ms() >= deadline_ms) {
        timed_out = true;
        return true;
    }
    return false;
}

/// Test-only: force the execution budget (ms) for later queries on this
/// thread. 0 = trip on the first hot-loop check; <0 restores the default.
pub export fn cbm_cypher_test_set_deadline_ms(budget_ms: i64) void {
    deadline_override_ms = budget_ms;
}

/// deadlineExceeded for a named phase. The test hook moves the deadline into
/// the past when that phase first checks it, so a test can expire the budget
/// inside one specific phase.
fn deadlineExceededAt(site: [*:0]const u8) bool {
    if (test_expire_site != null and eql(site, test_expire_site)) {
        deadline_ms = 1; // an instant already in the past
    }
    return deadlineExceeded();
}

/// Test-only: expire the deadline when the named phase ("cross_join",
/// "cross_join_rels", "sort_bindings", "order_by") checks it; NULL disables.
pub export fn cbm_cypher_test_expire_deadline_at(site: [*c]const u8) void {
    test_expire_site = site;
}

// ── Binding virtual variables (for WITH clause) ────────────────────

fn bindingGetVirtual(b: *Binding, v: CStr, prop: CStr) CStr {
    if (v == null) return "";
    // Check virtual vars first (from WITH projection)
    var full: [256]u8 = undefined;
    if (prop != null) {
        _ = bprint(&full, "{s}.{s}", .{ sp(v), sp(prop) });
    } else {
        _ = bprint(&full, "{s}", .{sp(v)});
    }
    for (0..ix(b.var_count)) |i| {
        if (eql(b.var_names[i], &full)) {
            return if (b.var_nodes[i].name != null) b.var_nodes[i].name else "";
        }
    }
    // Fall through to normal lookup
    if (bindingGetEdge(b, v)) |e| {
        // Bare `RETURN r` on an edge: surface the full properties JSON (or
        // "{}" if none) so callers can inspect timestamps, weights, etc.
        // without naming each property.
        if (prop != null) return edgeProp(e, prop);
        return if (e.properties_json != null) e.properties_json else "{}";
    }
    if (bindingGet(b, v)) |n| {
        if (prop != null) return nodeProp(n, prop, b.store);
        return if (n.name != null) n.name else "";
    }
    return "";
}

// ── String function application ────────────────────────────────────

fn isTrimSpace(ch: u8) bool {
    return ch == ' ' or ch == '\t' or ch == '\n' or ch == '\r';
}

fn applyStringFunc(func: CStr, val: CStr, buf: []u8) CStr {
    if (func == null or val == null) return if (val != null) val else "";
    const last = buf.len - 1;
    if (eql(func, "toLower") or eql(func, "toUpper")) {
        const upper = eql(func, "toUpper");
        var i: usize = 0;
        while (i < last and val[i] != 0) : (i += 1) {
            buf[i] = if (upper) std.ascii.toUpper(val[i]) else std.ascii.toLower(val[i]);
        }
        buf[i] = 0;
        return buf.ptr;
    }
    if (eql(func, "toString")) return val; // already strings
    if (eql(func, "toInteger")) {
        var end: [*c]u8 = null;
        var v: c_longlong = c.strtoll(val, &end, 10);
        if (end == val) {
            // Not an integer literal — accept a float string and truncate.
            var fend: [*c]u8 = null;
            const d = c.strtod(val, &fend);
            if (fend == val) return ""; // non-numeric → null
            v = truncToI64(d);
        }
        _ = c.snprintf(buf.ptr, buf.len, "%lld", v);
        return buf.ptr;
    }
    if (eql(func, "toFloat")) {
        var end: [*c]u8 = null;
        const d = c.strtod(val, &end);
        if (end == val) return ""; // non-numeric → null
        _ = c.snprintf(buf.ptr, buf.len, "%g", d);
        return buf.ptr;
    }
    if (eql(func, "toBoolean")) {
        if (ciEq(val, "true")) return "true";
        if (ciEq(val, "false")) return "false";
        return ""; // not a boolean → null
    }
    if (eql(func, "size") or eql(func, "length")) {
        _ = bprint(buf, "{d}", .{c.strlen(val)});
        return buf.ptr;
    }
    if (eql(func, "trim") or eql(func, "ltrim") or eql(func, "rtrim")) {
        const do_left = eql(func, "trim") or eql(func, "ltrim");
        const do_right = eql(func, "trim") or eql(func, "rtrim");
        var start: usize = 0;
        var end: usize = c.strlen(val);
        while (do_left and isTrimSpace(val[start])) start += 1;
        while (do_right and end > start and isTrimSpace(val[end - 1])) end -= 1;
        const n = @min(end - start, last);
        @memcpy(buf[0..n], val[start .. start + n]);
        buf[n] = 0;
        return buf.ptr;
    }
    if (eql(func, "reverse")) {
        const len = @min(c.strlen(val), last);
        for (0..len) |i| buf[i] = val[len - 1 - i];
        buf[len] = 0;
        return buf.ptr;
    }
    return val;
}

// ── CASE expression evaluation ─────────────────────────────────────

fn evalCaseExpr(k: [*c]const c.cbm_case_expr_t, b: *Binding) CStr {
    if (k == null) return "";
    for (0..ix(k.*.branch_count)) |i| {
        const br = at(k.*.branches, i);
        if (evalExpr(br.when_expr, b)) return if (br.then_val != null) br.then_val else "";
    }
    return if (k.*.else_val != null) k.*.else_val else "";
}

// ── Scan nodes for a pattern ───────────────────────────────────────

/// True if `actual` matches `pat`, where `pat` may be a '|'-alternation of
/// labels ("A|B|C") — openCypher label alternation (#242).
fn labelAltMatches(actual: CStr, pat: CStr) bool {
    if (pat == null) return true;
    if (actual == null) return false;
    if (c.strchr(pat, '|') == null) return eql(actual, pat);
    const al = c.strlen(actual);
    var seg: CStr = pat;
    while (seg[0] != 0) {
        const bar: CStr = c.strchr(seg, '|');
        const seglen: usize = if (bar != null) @intFromPtr(bar) - @intFromPtr(seg) else c.strlen(seg);
        if (seglen == al and c.strncmp(seg, actual, seglen) == 0) return true;
        if (bar == null) break;
        seg = bar + 1;
    }
    return false;
}

/// Seed nodes for a label alternation "A|B|C": union the per-label results.
/// Node-struct fields are moved (shallow) into out_nodes; each per-label array
/// container is freed.
fn scanAlternationLabels(store: ?*Store, project: CStr, labels: CStr, out_nodes: *[*c]Node, out_count: *c_int) void {
    out_nodes.* = null;
    out_count.* = 0;
    var cap: c_int = 0;
    const copy = heapStrdup(labels);
    if (copy == null) return;
    var save: [*c]u8 = null;
    var tok = c.strtok_r(copy, "|", &save);
    while (tok != null) : (tok = c.strtok_r(null, "|", &save)) {
        var part: [*c]Node = null;
        var pc: c_int = 0;
        _ = c.cbm_store_find_nodes_by_label(store, project, tok, &part, &pc);
        if (pc > 0 and part != null) {
            if (out_count.* + pc > cap) {
                cap = (out_count.* + pc) * 2;
                out_nodes.* = safeReallocN(Node, out_nodes.*, ix(cap));
            }
            @memcpy(out_nodes.*[ix(out_count.*)..ix(out_count.* + pc)], part[0..ix(pc)]);
            out_count.* += pc;
        }
        c.free(@ptrCast(part)); // container only — node fields moved to out_nodes
    }
    c.free(copy);
}

/// Every candidate is scanned: max_rows limits projected rows, and WHERE and
/// aggregation run after the scan, so a capped scan would drop real matches
/// (upstream #1196).
fn scanPatternNodes(store: ?*Store, project: CStr, first: *const c.cbm_node_pattern_t, out_nodes: *[*c]Node, out_count: *c_int) void {
    if (first.label != null and c.strchr(first.label, '|') != null) {
        scanAlternationLabels(store, project, first.label, out_nodes, out_count);
    } else if (first.label != null) {
        _ = c.cbm_store_find_nodes_by_label(store, project, first.label, out_nodes, out_count);
    } else {
        _ = c.cbm_store_find_nodes(store, project, out_nodes, out_count);
    }
    // Apply inline property filters — free rejected nodes' strings
    if (first.prop_count > 0) {
        var kept: usize = 0;
        for (0..cnt(out_count.*)) |i| {
            const nodes = out_nodes.*;
            if (checkInlineProps(nodes + i, first.props, first.prop_count, store)) {
                if (kept != i) nodes[kept] = nodes[i];
                kept += 1;
            } else {
                nodeFieldsFree(nodes + i);
            }
        }
        out_count.* = @intCast(kept);
    }
}

// ── Expand one pattern's relationships on a set of bindings ────────

/// A growable array of bindings, as the C engine kept it: rows, count, cap.
const BindingList = struct {
    rows: [*c]Binding,
    count: c_int,
    cap: c_int,
};

/// A hop's output holds EVERY matched row (upstream #1196). max_rows is an
/// output-row limit that projection enforces, and WHERE and aggregation run
/// after expansion, so the old bind_cap*10 ceiling dropped edges before
/// count() saw them, labeled source or not. The buffer grows geometrically;
/// only allocation failure stops it, and the deadline still bounds time.
fn bindingOutAppend(out: *BindingList, nb: *Binding) bool {
    if (out.count == out.cap) {
        const next: c_int = if (out.cap > 0) out.cap * GROWTH_2 else GROWTH_10;
        const grown = reallocN(Binding, out.rows, ix(next));
        if (grown == null) {
            bindingFree(nb);
            return false;
        }
        out.rows = grown;
        out.cap = next;
    }
    out.rows[ix(out.count)] = nb.*;
    out.count += 1;
    return true;
}

/// Process edges: look up target node, filter by label/props, add binding.
/// `inbound` controls which end of the edge is the target id.
fn processEdges(store: ?*Store, edges: [*c]Edge, edge_count: c_int, inbound: bool, target_node: *const c.cbm_node_pattern_t, b: *Binding, to_var: CStr, rel_var: CStr, out: *BindingList, match_count: *c_int) void {
    // When the terminal node variable is ALREADY bound (e.g. the second pattern
    // `(c)-[:CALLS]->(f)` where `f` came from an earlier MATCH), we must FILTER
    // to edges that actually reach the bound node — not overwrite the caller's
    // `f` binding with whatever node the edge leads to. Overwriting corrupted
    // the result of dead-code queries and produced wrong rows (#627).
    const bound_to = bindingGet(b, to_var);
    const bound_to_id: i64 = if (bound_to) |n| n.id else 0;
    for (0..cnt(edge_count)) |ei| {
        const tid = if (inbound) edges[ei].source_id else edges[ei].target_id;
        if (bound_to != null and tid != bound_to_id) continue; // edge does not reach the bound terminal
        var found = std.mem.zeroes(Node);
        if (c.cbm_store_find_node_by_id(store, tid, &found) != c.CBM_STORE_OK) continue;
        if (target_node.label != null and !labelAltMatches(found.label, target_node.label)) {
            nodeFieldsFree(&found);
            continue;
        }
        if (!checkInlineProps(&found, target_node.props, target_node.prop_count, store)) {
            nodeFieldsFree(&found);
            continue;
        }
        var nb = EMPTY_BINDING;
        bindingCopy(&nb, b);
        bindingSet(&nb, to_var, &found);
        if (rel_var != null) bindingSetEdge(&nb, rel_var, edges + ei);
        nodeFieldsFree(&found);
        match_count.* += 1;
        _ = bindingOutAppend(out, &nb);
    }
}

// Set when a variable-length hop range is clamped to the engine ceiling during
// the CURRENT execution; cbm_cypher_execute turns it into result->warning so
// callers can tell "clamped" from "no such path" (upstream 73a3c347/#797).
threadlocal var depth_clamped: c_int = 0;
// Set when a variable-length trail walk hit its row budget or result cap.
threadlocal var trail_truncated: c_int = 0;

/// Expand a variable-length relationship via BFS.
fn expandVarLength(store: ?*Store, rel: *const c.cbm_rel_pattern_t, target_node: *const c.cbm_node_pattern_t, b: *Binding, src: *const Node, to_var: CStr, out: *BindingList, match_count: *c_int) void {
    // Clamp BOTH the explicit (`*1..N`) and unbounded (`*`, `*..m`) forms to the
    // engine ceiling: an explicit N above the cap was previously honoured
    // verbatim, driving cbm_store_bfs to an unbounded hop count (#887). WARN on
    // clamp — never a silent truncation.
    const depth_cap = c.cbm_cypher_max_depth();
    var max_depth = if (rel.max_hops > 0) rel.max_hops else depth_cap;
    if (max_depth > depth_cap) {
        var req_buf: [16]u8 = undefined;
        var cap_buf: [16]u8 = undefined;
        _ = bprint(&req_buf, "{d}", .{max_depth});
        _ = bprint(&cap_buf, "{d}", .{depth_cap});
        logWarn("cypher.depth_capped", .{ "requested", &req_buf, "cap", &cap_buf });
        depth_clamped = depth_cap; // surfaced as result->warning (#797)
        max_depth = depth_cap;
    }
    var tr = std.mem.zeroes(c.cbm_traverse_result_t);
    const dir: CStr = if (rel.direction != null) rel.direction else "outbound";
    _ = c.cbm_store_bfs_trail(store, src.id, dir, rel.types, rel.type_count, max_depth, c.CBM_PERCENT, &tr);
    if (tr.truncated) trail_truncated = 1; // surfaced as result->warning
    // An already-bound terminal (e.g. (f)-[*1..2]->(f)) filters hops, as in
    // processEdges, instead of being overwritten.
    const bound_to = bindingGet(b, to_var);
    const bound_to_id: i64 = if (bound_to) |n| n.id else 0;
    for (0..cnt(tr.visited_count)) |v| {
        const hop = at(tr.visited, v);
        if (hop.hop < rel.min_hops) continue;
        if (bound_to != null and hop.node.id != bound_to_id) continue;
        if (target_node.label != null and !labelAltMatches(hop.node.label, target_node.label)) continue;
        if (!checkInlineProps(&hop.node, target_node.props, target_node.prop_count, store)) continue;
        var nb = EMPTY_BINDING;
        bindingCopy(&nb, b);
        bindingSet(&nb, to_var, &hop.node);
        match_count.* += 1;
        _ = bindingOutAppend(out, &nb);
    }
    c.cbm_store_traverse_free(&tr);
}

/// Find src's edges of one type (any type when edge_type is null) in one
/// direction and expand them.
fn expandEdges(store: ?*Store, src: *const Node, edge_type: CStr, inbound: bool, target_node: *const c.cbm_node_pattern_t, b: *Binding, to_var: CStr, rel_var: CStr, out: *BindingList, match_count: *c_int) void {
    var edges: [*c]Edge = null;
    var edge_count: c_int = 0;
    if (edge_type != null) {
        if (inbound) {
            _ = c.cbm_store_find_edges_by_target_type(store, src.id, edge_type, &edges, &edge_count);
        } else {
            _ = c.cbm_store_find_edges_by_source_type(store, src.id, edge_type, &edges, &edge_count);
        }
    } else if (inbound) {
        _ = c.cbm_store_find_edges_by_target(store, src.id, &edges, &edge_count);
    } else {
        _ = c.cbm_store_find_edges_by_source(store, src.id, &edges, &edge_count);
    }
    processEdges(store, edges, edge_count, inbound, target_node, b, to_var, rel_var, out, match_count);
    c.cbm_store_free_edges(edges, edge_count);
}

/// Expand fixed-length (1-hop) relationship edges.
fn expandFixedLength(store: ?*Store, rel: *const c.cbm_rel_pattern_t, target_node: *const c.cbm_node_pattern_t, b: *Binding, src: *const Node, to_var: CStr, out: *BindingList, match_count: *c_int) void {
    const is_inbound = rel.direction != null and eql(rel.direction, "inbound");
    const is_any = rel.direction != null and eql(rel.direction, "any");
    const rel_var = rel.variable;

    if (rel.type_count > 0) {
        for (0..ix(rel.type_count)) |ti| {
            expandEdges(store, src, rel.types[ti], is_inbound, target_node, b, to_var, rel_var, out, match_count);
        }
        if (is_any) {
            for (0..ix(rel.type_count)) |ti| {
                expandEdges(store, src, rel.types[ti], true, target_node, b, to_var, rel_var, out, match_count);
            }
        }
    } else {
        expandEdges(store, src, null, is_inbound, target_node, b, to_var, rel_var, out, match_count);
        if (is_any) expandEdges(store, src, null, true, target_node, b, to_var, rel_var, out, match_count);
    }
}

fn expandPatternRels(store: ?*Store, pat: *const c.cbm_pattern_t, bindings: *[*c]Binding, bind_count: *c_int, bind_cap: *c_int, var_name: *CStr, is_optional: bool) void {
    for (0..cnt(pat.rel_count)) |ri| {
        // Stop expanding further hops once the wall-clock budget is spent (an
        // unbounded expansion is exactly what blows up here).
        if (deadlineExceeded()) return;
        const rel = at(pat.rels, ri);
        const target_node = at(pat.nodes, ri + 1);
        const to_var: CStr = if (target_node.variable != null) target_node.variable else "_n_t";

        const is_variable_length = rel.min_hops != 1 or rel.max_hops != 1;

        // The hop's output grows to hold every matched row (upstream #1196);
        // the OPTIONAL fallback shares the same append, so no writer can run
        // off the end and no row is dropped.
        const new_cap: c_int = if (bind_count.* > 0) bind_count.* else GROWTH_10;
        var out: BindingList = .{ .rows = mallocN(Binding, ix(new_cap)), .count = 0, .cap = new_cap };
        if (out.rows == null) return; // OOM: leave existing bindings untouched rather than corrupt

        for (0..cnt(bind_count.*)) |bi| {
            if ((bi & DEADLINE_CHECK_MASK) == 0 and deadlineExceeded()) break;
            const b = at(bindings.*, bi);
            const src = bindingGet(b, var_name.*) orelse continue;

            var match_count: c_int = 0;
            if (is_variable_length) {
                expandVarLength(store, rel, target_node, b, src, to_var, &out, &match_count);
            } else {
                expandFixedLength(store, rel, target_node, b, src, to_var, &out, &match_count);
            }

            // OPTIONAL MATCH: no expansion for this source, so keep the binding
            // with the target unbound (projection renders it "").
            if (is_optional and match_count == 0) {
                var nb = EMPTY_BINDING;
                bindingCopy(&nb, b);
                _ = bindingOutAppend(&out, &nb);
            }
        }

        for (0..cnt(bind_count.*)) |bi| bindingFree(at(bindings.*, bi));
        c.free(@ptrCast(bindings.*));
        bindings.* = out.rows;
        bind_count.* = out.count;
        bind_cap.* = out.cap;
        var_name.* = to_var;
    }
}

// ── Sorting and key lookup ─────────────────────────────────────────
// ORDER BY, GROUP BY and DISTINCT used to compare every row with every other
// row (bubble sorts, linear group and duplicate scans). These helpers keep the
// same comparisons and keys in O(n log n) and O(n) (upstream 8df0639c).

/// Compare items a and b; positive means a sorts after b.
const IndexCmpFn = *const fn (ctx: *const anyopaque, a: c_int, b: c_int) c_int;

/// Stable sort of idx[0..n). Both this and the bubble sorts it replaced are
/// stable, so for any consistent ordering they give the same permutation. An
/// item moves before an earlier one only when cmp(earlier, item) > 0, which is
/// the bubble sorts' swap test. Without scratch memory it degrades to an
/// insertion sort with the same test and result. The deadline is checked once
/// per pass under `site`; past it the sort stops with idx still a permutation,
/// and the query fails on its final deadline check.
fn stableSort(idx: [*c]c_int, n: c_int, cmp: IndexCmpFn, ctx: *const anyopaque, site: [*:0]const u8) void {
    if (n < 2) return;
    const un = ix(n);
    const tmp = mallocN(c_int, un);
    if (tmp == null) {
        for (1..un) |i| {
            if (deadlineExceededAt(site)) return;
            var j = i;
            while (j > 0 and cmp(ctx, idx[j - 1], idx[j]) > 0) : (j -= 1) {
                std.mem.swap(c_int, at(idx, j), at(idx, j - 1));
            }
        }
        return;
    }
    var width: usize = 1;
    while (width < un) : (width *= 2) {
        if (deadlineExceededAt(site)) break;
        var lo: usize = 0;
        while (lo < un) : (lo += 2 * width) {
            const mid = @min(lo + width, un);
            const hi = @min(mid + width, un);
            var i = lo;
            var j = mid;
            var k = lo;
            while (i < mid and j < hi) : (k += 1) {
                if (cmp(ctx, idx[i], idx[j]) > 0) {
                    tmp[k] = idx[j];
                    j += 1;
                } else {
                    tmp[k] = idx[i];
                    i += 1;
                }
            }
            while (i < mid) : (i += 1) {
                tmp[k] = idx[i];
                k += 1;
            }
            while (j < hi) : (j += 1) {
                tmp[k] = idx[j];
                k += 1;
            }
        }
        @memcpy(idx[0..un], tmp[0..un]);
    }
    c.free(@ptrCast(tmp));
}

/// String key → the index it was first recorded with. Keys are copied: the
/// arrays they mirror are reallocated as they grow.
const KeyIndex = struct {
    table: ?*c.CBMHashTable = null,
    keys: [*c][*c]u8 = null,
    count: c_int = 0,
    cap: c_int = 0,
    failed: bool = false, // an allocation failed: callers fall back to their scans
};

/// The recorded index, FOUND_NONE, or KEY_INDEX_UNKNOWN after a failed
/// allocation (the caller then scans, so a result never depends on memory).
const KEY_INDEX_UNKNOWN: c_int = -2;

fn keyIndexFind(ki: *const KeyIndex, key: CStr) c_int {
    if (ki.failed) return KEY_INDEX_UNKNOWN;
    const value = if (ki.table != null) c.cbm_ht_get(ki.table, key) else null;
    const v = value orelse return FOUND_NONE;
    return @truncate(@as(isize, @bitCast(@intFromPtr(v))) - 1);
}

fn keyIndexAdd(ki: *KeyIndex, key: CStr, index: c_int) void {
    if (ki.failed) return;
    if (ki.table == null) ki.table = c.cbm_ht_create(256);
    if (ki.table != null and ki.count >= ki.cap) {
        const cap: c_int = if (ki.cap != 0) ki.cap * 2 else 256;
        const keys = reallocN([*c]u8, ki.keys, ix(cap));
        if (keys != null) {
            ki.keys = keys;
            ki.cap = cap;
        }
    }
    const copy = if (ki.table != null and ki.count < ki.cap) heapStrdup(key) else null;
    if (copy == null) {
        ki.failed = true;
        return;
    }
    ki.keys[ix(ki.count)] = copy;
    ki.count += 1;
    const before = c.cbm_ht_count(ki.table);
    _ = c.cbm_ht_set(ki.table, copy, @ptrFromInt(@as(usize, @intCast(index + 1))));
    // cbm_ht_set drops the entry silently when the table cannot grow.
    ki.failed = c.cbm_ht_count(ki.table) == before;
}

fn keyIndexFree(ki: *KeyIndex) void {
    if (ki.table != null) c.cbm_ht_free(ki.table);
    for (0..cnt(ki.count)) |i| c.free(ki.keys[i]);
    c.free(@ptrCast(ki.keys));
    ki.* = .{};
}

/// A row's DISTINCT identity: every value, length-prefixed so no value can run
/// into the next — equal keys exactly when every value is equal. Built in
/// buf.* (grown as needed); null when out of memory.
fn rowKey(vals: [*c]const CStr, n: c_int, buf: *[*c]u8, cap: *usize) CStr {
    var need: usize = 1;
    for (0..cnt(n)) |col| need += c.strlen(vals[col]) + 32;
    if (need > cap.*) {
        const grown = reallocN(u8, buf.*, need);
        if (grown == null) return null;
        buf.* = grown;
        cap.* = need;
    }
    var used: usize = 0;
    for (0..cnt(n)) |col| {
        const len = c.strlen(vals[col]);
        const prefix_len = bprint(buf.*[used..cap.*], "{d}:", .{len});
        if (prefix_len == 0 or prefix_len + len >= cap.* - used) {
            return null; // cannot happen: `need` reserved this
        }
        used += prefix_len;
        @memcpy(buf.*[used .. used + len], vals[col][0..len]);
        used += len;
    }
    buf.*[used] = 0;
    return buf.*;
}

// ── Result post-processing helpers ─────────────────────────────────

/// Find the column index for one ORDER BY key, checking both column names and
/// aliases. Returns -1 if not found.
fn rbFindOrderColumn(rb: *const ResultBuilder, ret: *const ReturnClause, key: CStr) c_int {
    for (0..cnt(rb.col_count)) |ci| {
        if (eql(rb.columns[ci], key)) return @intCast(ci);
    }
    for (0..cnt(ret.count)) |ci| {
        if (ret.items[ci].alias != null and eql(ret.items[ci].alias, key)) return @intCast(ci);
    }
    return NOT_FOUND;
}

/// Check whether a column contains numeric data by examining the first
/// non-empty value.
fn rbIsNumericColumn(rb: *const ResultBuilder, col: c_int) bool {
    for (0..cnt(rb.row_count)) |i| {
        const v = rb.rows[i][ix(col)];
        if (v != null and v[0] != 0) {
            var p2: CStr = if (v[0] == '-') v + 1 else v;
            if (p2[0] == 0) return false;
            while (p2[0] != 0) : (p2 += 1) {
                if (p2[0] < '0' or p2[0] > '9') return false;
            }
            return true;
        }
    }
    return false;
}

const RbOrderCtx = struct {
    cols: [*]const c_int,
    numeric: [*]const bool,
    descs: [*]const bool,
    keys: c_int,
    rows: [*c]Row = null, // the rows in their pre-sort order
    values: [*c]const c_int = null, // numeric keys parsed once: values[row * keys + k], or null
};

fn strtolInt(s: CStr) c_int {
    return narrow(c.strtol(s, null, 10));
}

fn cmpInts(x: anytype, y: @TypeOf(x)) c_int {
    return @as(c_int, @intFromBool(x > y)) - @intFromBool(x < y);
}

/// One ORDER BY comparison: numeric columns by their (int)strtol value, others
/// by strcmp, a later key breaking ties; positive = a after b.
fn rbOrderCompare(context: *const anyopaque, a: c_int, b: c_int) c_int {
    const ctx: *const RbOrderCtx = @ptrCast(@alignCast(context));
    const keys = ix(ctx.keys);
    const va: [*c]const c_int = if (ctx.values != null) ctx.values + ix(a) * keys else null;
    const vb: [*c]const c_int = if (ctx.values != null) ctx.values + ix(b) * keys else null;
    var result: c_int = 0;
    var k: usize = 0;
    while (k < keys and result == 0) : (k += 1) {
        const col = ix(ctx.cols[k]);
        if (ctx.numeric[k]) {
            const x = if (va != null) va[k] else strtolInt(ctx.rows[ix(a)][col]);
            const y = if (vb != null) vb[k] else strtolInt(ctx.rows[ix(b)][col]);
            result = cmpInts(x, y);
        } else {
            result = c.strcmp(ctx.rows[ix(a)][col], ctx.rows[ix(b)][col]);
        }
        if (ctx.descs[k]) result = 0 -% result;
    }
    return result;
}

fn rbApplyOrderBy(rb: *ResultBuilder, ret: *const ReturnClause) void {
    if (ret.order_key_count == 0) return;
    // Resolve every key up front; an unresolvable key is skipped, matching the
    // single-key forgiveness (sorting on what can be resolved beats dropping
    // the whole ORDER BY).
    var cols: [c.CBM_CYPHER_ORDER_KEYS_MAX]c_int = undefined;
    var numeric: [c.CBM_CYPHER_ORDER_KEYS_MAX]bool = undefined;
    var descs: [c.CBM_CYPHER_ORDER_KEYS_MAX]bool = undefined;
    var keys: usize = 0;
    for (0..cnt(ret.order_key_count)) |k| {
        const order_col = rbFindOrderColumn(rb, ret, ret.order_keys[k]);
        if (order_col < 0) continue;
        cols[keys] = order_col;
        numeric[keys] = rbIsNumericColumn(rb, order_col);
        descs[keys] = ret.order_descs[k];
        keys += 1;
    }
    const n = rb.row_count;
    if (keys == 0 or n < 2) return;
    const un = ix(n);
    var ctx: RbOrderCtx = .{ .cols = &cols, .numeric = &numeric, .descs = &descs, .keys = @intCast(keys) };
    const idx = mallocN(c_int, un);
    const rows = mallocN(Row, un);
    if (idx == null or rows == null) {
        // No scratch memory: insertion sort in place, same comparisons.
        c.free(@ptrCast(idx));
        c.free(@ptrCast(rows));
        ctx.rows = rb.rows;
        for (1..un) |i| {
            if (deadlineExceededAt("order_by")) return;
            var j = i;
            while (j > 0 and rbOrderCompare(&ctx, @intCast(j - 1), @intCast(j)) > 0) : (j -= 1) {
                std.mem.swap(Row, at(rb.rows, j), at(rb.rows, j - 1));
            }
        }
        return;
    }
    const values = mallocN(c_int, un * keys);
    for (0..un) |r| {
        idx[r] = @intCast(r);
        rows[r] = rb.rows[r];
        if (values != null) {
            for (0..keys) |k| {
                values[r * keys + k] = if (numeric[k]) strtolInt(rb.rows[r][ix(cols[k])]) else 0;
            }
        }
    }
    ctx.rows = rows;
    ctx.values = values;
    stableSort(idx, n, rbOrderCompare, &ctx, "order_by");
    for (0..un) |r| rb.rows[r] = rows[ix(idx[r])];
    c.free(@ptrCast(values));
    c.free(@ptrCast(rows));
    c.free(@ptrCast(idx));
}

fn rbFreeRow(rb: *ResultBuilder, i: usize) void {
    for (0..cnt(rb.col_count)) |col| strFree(at(rb.rows[i], col));
    c.free(@ptrCast(rb.rows[i]));
}

fn rbApplySkipLimit(rb: *ResultBuilder, skip_n: c_int, limit: c_int) void {
    // Skip
    if (skip_n > 0 and skip_n < rb.row_count) {
        for (0..ix(skip_n)) |i| rbFreeRow(rb, i);
        const rest = ix(rb.row_count - skip_n);
        std.mem.copyForwards(Row, rb.rows[0..rest], rb.rows[ix(skip_n) .. ix(skip_n) + rest]);
        rb.row_count -= skip_n;
    } else if (skip_n >= rb.row_count) {
        for (0..cnt(rb.row_count)) |i| rbFreeRow(rb, i);
        rb.row_count = 0;
    }
    // Limit
    if (limit >= 0 and rb.row_count > limit) {
        for (ix(limit)..ix(rb.row_count)) |i| rbFreeRow(rb, i);
        rb.row_count = limit;
    }
}

/// Whether row i equals one of the first `kept` rows, column by column.
fn rbRowSeenBefore(rb: *const ResultBuilder, i: usize, kept: usize) bool {
    for (0..kept) |j| {
        var same = true;
        var col: usize = 0;
        while (col < cnt(rb.col_count) and same) : (col += 1) {
            same = eql(rb.rows[i][col], rb.rows[j][col]);
        }
        if (same) return true;
    }
    return false;
}

fn rbApplyDistinct(rb: *ResultBuilder) void {
    if (rb.row_count <= 1) return;
    var seen: KeyIndex = .{};
    var key_buf: [*c]u8 = null;
    var key_cap: usize = 0;
    var kept: usize = 0;
    for (0..ix(rb.row_count)) |i| {
        const key = rowKey(rb.rows[i], rb.col_count, &key_buf, &key_cap);
        seen.failed = seen.failed or key == null;
        const found = keyIndexFind(&seen, key);
        const dup = if (found == KEY_INDEX_UNKNOWN) rbRowSeenBefore(rb, i, kept) else found >= 0;
        if (!dup) {
            keyIndexAdd(&seen, key, @intCast(kept));
            if (kept != i) rb.rows[kept] = rb.rows[i];
            kept += 1;
        } else {
            rbFreeRow(rb, i);
        }
    }
    rb.row_count = @intCast(kept);
    keyIndexFree(&seen);
    c.free(key_buf);
}

/// DISTINCT while projecting (#58). RETURN DISTINCT used to project up to the
/// row cap and only then drop duplicates, so a value first seen past the cap
/// never reached the result (`RETURN DISTINCT type(r)` over more edges than
/// max_rows could miss a rare type). The projection loops now skip duplicates
/// as they go, so the cap counts distinct rows. Only admitted rows are keyed.
const RbDistinct = struct {
    on: bool,
    seen: KeyIndex = .{},
    key_buf: [*c]u8 = null,
    key_cap: usize = 0,
};

fn rbDistinctFree(d: *RbDistinct) void {
    keyIndexFree(&d.seen);
    c.free(d.key_buf);
}

/// Whether vals equals a row rb already holds, column by column.
fn rbHoldsRow(rb: *const ResultBuilder, vals: [*c]const CStr) bool {
    for (0..cnt(rb.row_count)) |j| {
        var same = true;
        var col: usize = 0;
        while (col < cnt(rb.col_count) and same) : (col += 1) {
            same = eql(rb.rows[j][col], vals[col]);
        }
        if (same) return true;
    }
    return false;
}

/// Add one projected row under a row cap, skipping a row DISTINCT already
/// holds. Returns false once a new row arrives with the cap full: the caller
/// stops projecting. vals entries may be null; they project as "".
fn rbAddRowCapped(rb: *ResultBuilder, d: *RbDistinct, vals: [*c]CStr, cap: c_int) bool {
    var key: CStr = null;
    if (d.on) {
        for (0..cnt(rb.col_count)) |col| {
            if (vals[col] == null) vals[col] = "";
        }
        key = rowKey(vals, rb.col_count, &d.key_buf, &d.key_cap);
        d.seen.failed = d.seen.failed or key == null;
        const found = keyIndexFind(&d.seen, key);
        if (if (found == KEY_INDEX_UNKNOWN) rbHoldsRow(rb, vals) else found >= 0) return true;
    }
    if (rb.row_count >= cap) return false;
    if (d.on) keyIndexAdd(&d.seen, key, rb.row_count);
    rbAddRow(rb, vals);
    return true;
}

fn rbFree(rb: *ResultBuilder) void {
    for (0..cnt(rb.row_count)) |i| rbFreeRow(rb, i);
    c.free(@ptrCast(rb.rows));
    for (0..cnt(rb.col_count)) |i| strFree(at(rb.columns, i));
    c.free(@ptrCast(rb.columns));
}

// ── Get projection value for a binding + return item ───────────────

/// Element idx (negative from the end) of a JSON list of plain strings, as
/// labels()/keys() build them; "" (null) when out of range. list may point
/// into buf.
fn jsonStrListAt(list: CStr, idx: c_int, buf: []u8) CStr {
    var starts: [32]CStr = undefined;
    var lens: [32]usize = undefined;
    var n: c_int = 0;
    var q: CStr = list;
    while (q != null and q[0] != 0 and n < 32) : (q += 1) {
        if (q[0] != '"') continue;
        const end: CStr = c.strchr(q + 1, '"');
        if (end == null) break;
        starts[ix(n)] = q + 1;
        lens[ix(n)] = @intFromPtr(end) - @intFromPtr(q) - 1;
        n += 1;
        q = end;
    }
    const i: c_int = if (idx < 0) n +% idx else idx;
    if (i < 0 or i >= n or lens[ix(i)] >= buf.len) return "";
    const len = lens[ix(i)];
    std.mem.copyForwards(u8, buf[0..len], starts[ix(i)][0..len]);
    buf[len] = 0;
    return buf.ptr;
}

/// Build a JSON list of a node's non-null property keys: keys(n).
fn nodeKeysList(n: *const Node, buf: []u8) CStr {
    const Key = struct { k: []const u8, present: bool };
    const ks = [_]Key{
        .{ .k = "name", .present = n.name != null and n.name[0] != 0 },
        .{ .k = "qualified_name", .present = n.qualified_name != null and n.qualified_name[0] != 0 },
        .{ .k = "label", .present = n.label != null and n.label[0] != 0 },
        .{ .k = "file_path", .present = n.file_path != null and n.file_path[0] != 0 },
        .{ .k = "start_line", .present = n.start_line > 0 },
        .{ .k = "end_line", .present = n.end_line > 0 },
    };
    const last = buf.len - 1;
    var pos: usize = 0;
    var first = true;
    if (pos < last) {
        buf[pos] = '[';
        pos += 1;
    }
    for (ks) |key| {
        if (pos >= last) break;
        if (!key.present) continue;
        const sep: []const u8 = if (first) "" else ",";
        const w = bprint(buf[pos..], "{s}\"{s}\"", .{ sep, key.k });
        if (w >= buf.len - pos) break;
        pos += w;
        first = false;
    }
    if (pos < last) {
        buf[pos] = ']';
        pos += 1;
    }
    buf[pos] = 0;
    return buf.ptr;
}

/// Resolve a function argument to its string value (literal or var.prop).
fn evalFuncArg(b: *Binding, a: *const c.cbm_func_arg_t) CStr {
    if (a.literal != null) return a.literal;
    return bindingGetVirtual(b, a.variable, a.property);
}

/// Evaluate a multi-argument scalar function into buf (or a direct value).
fn evalMultiargFunc(b: *Binding, item: *const ReturnItem, buf: []u8) CStr {
    const f = item.func;
    const n = item.arg_count;
    const last = buf.len - 1;
    if (eql(f, "coalesce")) {
        for (0..cnt(n)) |i| {
            const v = evalFuncArg(b, at(item.args, i));
            if (v != null and v[0] != 0) return v;
        }
        return "";
    }
    if (eql(f, "substring") and n >= 2) {
        const s = evalFuncArg(b, at(item.args, 0));
        const start = c.strtol(evalFuncArg(b, &item.args[1]), null, 10);
        const slen = c.strlen(s);
        if (start < 0 or @as(usize, @intCast(start)) >= slen) return "";
        const ustart: usize = @intCast(start);
        var take = slen - ustart;
        if (n >= 3) {
            var len = c.strtol(evalFuncArg(b, &item.args[2]), null, 10);
            if (len < 0) len = 0;
            if (@as(usize, @intCast(len)) < take) take = @intCast(len);
        }
        if (take >= buf.len) take = last;
        @memcpy(buf[0..take], s[ustart .. ustart + take]);
        buf[take] = 0;
        return buf.ptr;
    }
    if ((eql(f, "left") or eql(f, "right")) and n >= 2) {
        const s = evalFuncArg(b, at(item.args, 0));
        var k = c.strtol(evalFuncArg(b, &item.args[1]), null, 10);
        if (k < 0) k = 0;
        const slen = c.strlen(s);
        var take: usize = @min(@as(usize, @intCast(k)), slen);
        if (take >= buf.len) take = last;
        const from: usize = if (eql(f, "left")) 0 else slen - take;
        @memcpy(buf[0..take], s[from .. from + take]);
        buf[take] = 0;
        return buf.ptr;
    }
    if (eql(f, "replace") and n >= 3) {
        const s = evalFuncArg(b, at(item.args, 0));
        const from = evalFuncArg(b, &item.args[1]);
        const to = evalFuncArg(b, &item.args[2]);
        const fromlen = c.strlen(from);
        const tolen = c.strlen(to);
        var pos: usize = 0;
        var pp: CStr = s;
        if (fromlen == 0) {
            _ = bprint(buf, "{s}", .{sp(s)});
            return buf.ptr;
        }
        while (pp[0] != 0 and pos < last) {
            if (c.strncmp(pp, from, fromlen) == 0) {
                var cpy = tolen;
                if (pos + cpy >= buf.len) cpy = last - pos;
                @memcpy(buf[pos .. pos + cpy], to[0..cpy]);
                pos += cpy;
                pp += fromlen;
            } else {
                buf[pos] = pp[0];
                pos += 1;
                pp += 1;
            }
        }
        buf[pos] = 0;
        return buf.ptr;
    }
    return ""; // wrong arity → null
}

fn projectItem(b: *Binding, item: *const ReturnItem, func_buf: []u8) CStr {
    if (item.kase != null) return evalCaseExpr(item.kase, b);
    if (item.args != null) return evalMultiargFunc(b, item, func_buf);
    // Entity-introspection functions operate on the bound node/edge itself,
    // not on a scalar property value.
    if (item.func != null) {
        if (eql(item.func, "labels")) {
            if (bindingGet(b, item.variable)) |n| {
                if (n.label != null) {
                    _ = bprint(func_buf, "[\"{s}\"]", .{sp(n.label)});
                    return if (item.has_index) jsonStrListAt(func_buf.ptr, item.index, func_buf) else func_buf.ptr;
                }
            }
            return if (item.has_index) "" else "[]";
        }
        if (eql(item.func, "type")) {
            const e = bindingGetEdge(b, item.variable) orelse return "";
            return if (e.type != null) e.type else "";
        }
        if (eql(item.func, "id")) {
            if (bindingGet(b, item.variable)) |n| {
                _ = bprint(func_buf, "{d}", .{n.id});
                return func_buf.ptr;
            }
            if (bindingGetEdge(b, item.variable)) |e| {
                _ = bprint(func_buf, "{d}", .{e.id});
                return func_buf.ptr;
            }
            return "";
        }
        if (eql(item.func, "keys")) {
            const n = bindingGet(b, item.variable) orelse return if (item.has_index) "" else "[]";
            const keys = nodeKeysList(n, func_buf);
            return if (item.has_index) jsonStrListAt(keys, item.index, func_buf) else keys;
        }
        if (eql(item.func, "properties")) {
            if (bindingGet(b, item.variable)) |n| {
                return if (n.properties_json != null) n.properties_json else "{}";
            }
            if (bindingGetEdge(b, item.variable)) |e| {
                return if (e.properties_json != null) e.properties_json else "{}";
            }
            return "{}";
        }
    }
    const raw = bindingGetVirtual(b, item.variable, item.property);
    if (isScalarValueFunc(item.func)) return applyStringFunc(item.func, raw, func_buf);
    // Copy into the caller's per-column buffer. `raw` may point to nodeProp's
    // rotating scratch buffer, which the next column's projection would
    // overwrite before rbAddRow copies the assembled row — aliasing every such
    // column to the last value read. The per-column func_buf gives each column
    // stable storage.
    if (raw != null and raw != func_buf.ptr and raw[0] != 0) {
        const len = @min(c.strlen(raw), func_buf.len - 1);
        @memcpy(func_buf[0..len], raw[0..len]);
        func_buf[len] = 0;
        return func_buf.ptr;
    }
    return if (raw != null) raw else "";
}

/// Check if a function name is an aggregate.
fn isAggregateFunc(func: CStr) bool {
    return func != null and (eql(func, "COUNT") or eql(func, "SUM") or eql(func, "AVG") or
        eql(func, "MIN") or eql(func, "MAX") or eql(func, "COLLECT"));
}

/// Grow a string list for one more entry at `idx`: the capacity doubles at
/// every power of two, so n appends cost O(n) copying, not O(n^2).
fn stringListReserve(list: [*c][*c]u8, idx: c_int) [*c][*c]u8 {
    if (idx > 0 and (idx & (idx - 1)) != 0) return list; // capacity is the next power of two above idx
    return safeReallocN([*c]u8, list, if (idx != 0) ix(idx) * 2 else 1);
}

const DISTINCT_SET_MIN = 16; // below this a set is scanned, not hashed

const SET_MEMBER: *anyopaque = @ptrFromInt(1);

/// Hash the first `count` list entries into a new set; null if it cannot be
/// built in full (the caller then keeps scanning the list).
fn distinctSetBuild(list: [*c][*c]u8, count: c_int) ?*c.CBMHashTable {
    var set = c.cbm_ht_create(64);
    var i: usize = 0;
    while (set != null and i < cnt(count)) : (i += 1) _ = c.cbm_ht_set(set, list[i], SET_MEMBER);
    if (set != null and c.cbm_ht_count(set) != @as(u32, @bitCast(count))) {
        c.cbm_ht_free(set);
        set = null;
    }
    return set;
}

/// Append `val` to a string list only if not already present — i.e. maintain
/// a set of distinct values. Used by COUNT(DISTINCT x) (#239). Past
/// DISTINCT_SET_MIN values the membership test goes through set.*, whose keys
/// are the list's own strings; a set that cannot be built stays a scan.
fn distinctListAdd(list: *[*c][*c]u8, count: *c_int, set: *?*c.CBMHashTable, val: CStr) void {
    if (set.* != null) {
        if (c.cbm_ht_has(set.*, val)) return;
    } else {
        for (0..cnt(count.*)) |i| {
            if (eql(list.*[i], val)) return;
        }
    }
    const idx = count.*;
    list.* = stringListReserve(list.*, idx);
    const copy = if (list.* != null) heapStrdup(val) else null;
    if (copy == null) {
        // Out of memory: keep a consistent (possibly emptied) set.
        if (list.* == null) {
            count.* = 0;
            if (set.* != null) {
                c.cbm_ht_free(set.*);
                set.* = null;
            }
        }
        return;
    }
    list.*[ix(idx)] = copy;
    count.* += 1;
    if (set.* != null) {
        _ = c.cbm_ht_set(set.*, copy, SET_MEMBER);
        if (c.cbm_ht_count(set.*) != @as(u32, @bitCast(count.*))) {
            c.cbm_ht_free(set.*); // the insert was dropped: back to scanning
            set.* = null;
        }
    } else if (count.* >= DISTINCT_SET_MIN) {
        set.* = distinctSetBuild(list.*, count.*);
    }
}

const WithOrderCtx = struct {
    wc: *const ReturnClause,
    values: [*c]CStr, // values[i * keys + k]: binding i's ORDER BY key k
    numbered: [*c]bool, // whether strtod consumed a number from that value
    numbers: [*c]f64,
    owned: [*c][*c]u8, // owned[i * keys + k]: copy of a transient value, else null
};

/// One WITH ORDER BY comparison: per key, numerically when both values start
/// with a number, else by strcmp; later keys break ties, direction is per key
/// (upstream 0a0ef516). Positive = a after b.
fn withOrderCompare(context: *const anyopaque, a: c_int, b: c_int) c_int {
    const ctx: *const WithOrderCtx = @ptrCast(@alignCast(context));
    const keys = ix(ctx.wc.order_key_count);
    var result: c_int = 0;
    var k: usize = 0;
    while (k < keys and result == 0) : (k += 1) {
        const ia = ix(a) * keys + k;
        const ib = ix(b) * keys + k;
        if (ctx.numbered[ia] and ctx.numbered[ib]) {
            result = cmpInts(ctx.numbers[ia], ctx.numbers[ib]);
        } else {
            result = c.strcmp(ctx.values[ia], ctx.values[ib]);
        }
        if (ctx.wc.order_descs[k]) result = 0 -% result;
    }
    return result;
}

/// Resolve one WITH ORDER BY key on a projected binding. The key is either a
/// projected name (`c`, or an unaliased `f.name`) or a property of a carried
/// node (`g.start_line` after `WITH f AS g`, #57): an exact projected name
/// wins, otherwise `var.prop` is split and resolved on the carried node. A
/// property value may live in nodeProp's rotating buffers, so it is reported
/// as transient.
fn orderKeyValue(b: *Binding, key: CStr, transient: *bool) CStr {
    transient.* = false;
    const dot: CStr = c.strchr(key, '.');
    if (dot == null) return bindingGetVirtual(b, key, null);
    for (0..cnt(b.var_count)) |i| {
        if (eql(b.var_names[i], key)) return bindingGetVirtual(b, key, null);
    }
    var v: [256]u8 = undefined;
    const vlen = @intFromPtr(dot) - @intFromPtr(key);
    if (vlen >= v.len) return "";
    @memcpy(v[0..vlen], key[0..vlen]);
    v[vlen] = 0;
    transient.* = true;
    return bindingGetVirtual(b, &v, dot + 1);
}

/// Resolve binding i's ORDER BY keys into the context. A projected name
/// resolves to a string the binding owns, which stays valid while the
/// bindings are only reordered; a carried node's property is copied into
/// ctx.owned.
fn withOrderResolve(ctx: *const WithOrderCtx, b: *Binding, i: usize) void {
    const keys = ix(ctx.wc.order_key_count);
    for (0..keys) |k| {
        const cell = i * keys + k;
        var transient = false;
        var v = orderKeyValue(b, ctx.wc.order_keys[k], &transient);
        c.free(ctx.owned[cell]);
        ctx.owned[cell] = null;
        if (transient) {
            ctx.owned[cell] = heapStrdup(v);
            v = if (ctx.owned[cell] != null) ctx.owned[cell] else "";
        }
        var end: [*c]u8 = null;
        ctx.values[cell] = v;
        ctx.numbers[cell] = c.strtod(v, &end);
        ctx.numbered[cell] = @intFromPtr(end) != @intFromPtr(v);
    }
}

/// Sort bindings by the ORDER BY key list (virtual variables), stably. Each
/// binding's keys are resolved once, not twice per comparison.
fn sortBindings(vbindings: [*c]Binding, count: c_int, wc: *const ReturnClause) void {
    if (count < 2 or wc.order_key_count <= 0) return;
    const keys = ix(wc.order_key_count);
    const n = ix(count);
    const cells = n * keys;
    const ctx: WithOrderCtx = .{
        .wc = wc,
        .values = mallocN(CStr, cells),
        .numbered = mallocN(bool, cells),
        .numbers = mallocN(f64, cells),
        .owned = callocN([*c]u8, cells),
    };
    const idx = mallocN(c_int, n);
    const sorted = mallocN(Binding, n);
    if (ctx.values != null and ctx.numbered != null and ctx.numbers != null and ctx.owned != null and
        idx != null and sorted != null)
    {
        for (0..n) |i| {
            idx[i] = @intCast(i);
            withOrderResolve(&ctx, at(vbindings, i), i);
        }
        stableSort(idx, count, withOrderCompare, &ctx, "sort_bindings");
        for (0..n) |i| sorted[i] = vbindings[ix(idx[i])];
        @memcpy(vbindings[0..n], sorted[0..n]);
    } else {
        // No scratch memory: insertion sort in place, same comparisons, with
        // the two compared bindings' keys resolved into a two-cell context.
        const PAIR = c.CBM_CYPHER_ORDER_KEYS_MAX * 2;
        var pair_values: [PAIR]CStr = undefined;
        var pair_numbered: [PAIR]bool = undefined;
        var pair_numbers: [PAIR]f64 = undefined;
        var pair_owned: [PAIR][*c]u8 = @splat(null);
        const pair: WithOrderCtx = .{
            .wc = wc,
            .values = &pair_values,
            .numbered = &pair_numbered,
            .numbers = &pair_numbers,
            .owned = &pair_owned,
        };
        outer: for (1..n) |i| {
            if (deadlineExceededAt("sort_bindings")) break :outer;
            var j = i;
            while (j > 0) : (j -= 1) {
                withOrderResolve(&pair, at(vbindings, j - 1), 0);
                withOrderResolve(&pair, at(vbindings, j), 1);
                if (withOrderCompare(&pair, 0, 1) <= 0) break;
                std.mem.swap(Binding, at(vbindings, j), at(vbindings, j - 1));
            }
        }
        for (pair_owned[0 .. keys * 2]) |p| c.free(p);
    }
    if (ctx.owned != null) {
        for (0..cells) |cell| c.free(ctx.owned[cell]);
    }
    c.free(@ptrCast(ctx.values));
    c.free(@ptrCast(ctx.numbered));
    c.free(@ptrCast(ctx.numbers));
    c.free(@ptrCast(ctx.owned));
    c.free(@ptrCast(idx));
    c.free(@ptrCast(sorted));
}

/// Apply skip and limit to a binding array, freeing discarded entries.
fn bindingsSkipLimit(vbindings: [*c]Binding, count: *c_int, skip: c_int, limit: c_int) void {
    if (skip > 0 and skip < count.*) {
        for (0..ix(skip)) |i| bindingFree(at(vbindings, i));
        const rest = ix(count.* - skip);
        std.mem.copyForwards(Binding, vbindings[0..rest], vbindings[ix(skip) .. ix(skip) + rest]);
        count.* -= skip;
    } else if (skip >= count.*) {
        for (0..cnt(count.*)) |i| bindingFree(at(vbindings, i));
        count.* = 0;
    }
    if (limit >= 0 and count.* > limit) {
        for (ix(limit)..ix(count.*)) |i| bindingFree(at(vbindings, i));
        count.* = limit;
    }
}

/// Sort, skip, and limit binding array in-place.
fn withSortSkipLimit(wc: *const ReturnClause, vbindings: [*c]Binding, vcount: *c_int) void {
    if (wc.order_key_count > 0) sortBindings(vbindings, vcount.*, wc);
    bindingsSkipLimit(vbindings, vcount, wc.skip, wc.limit);
}

/// Resolve the alias or compute a default name for a WITH/RETURN item.
fn resolveItemAlias(item: *const ReturnItem, name_buf: []u8) CStr {
    if (item.alias != null) return item.alias;
    if (item.property != null) {
        _ = bprint(name_buf, "{s}.{s}", .{ sp(item.variable), sp(item.property) });
    } else {
        _ = bprint(name_buf, "{s}", .{sp(item.variable)});
    }
    return name_buf.ptr;
}

// ── WITH clause: project bindings through aggregation or rename ────

/// WITH aggregation group entry.
const WithAgg = extern struct {
    group_key: [1024]u8,
    group_vals: [*c]CStr,
    sums: [*c]f64,
    counts: [*c]c_int,
    mins: [*c]f64,
    maxs: [*c]f64,
    distinct_lists: [*c][*c][*c]u8, // per-item set of seen values for COUNT(DISTINCT)
    distinct_n: [*c]c_int, // per-item distinct count (#239)
    distinct_sets: [*c]?*c.CBMHashTable, // per-item hash over distinct_lists (lazy)
    group_nodes: [*c]Node, // per-item carried node (deep copy; id 0 = not a node)
};

/// A WITH item that is a bare, bound node variable (`WITH f`, `WITH f AS g`)
/// carries the node itself, not a scalar: return that node, else null.
/// Property items, functions (labels/id/keys/...) and unbound OPTIONAL vars
/// (id 0) stay scalar. The carried node keeps every field, so a later
/// g.<prop> resolves exactly as it would without the WITH (#57).
fn withItemCarriedNode(item: *const ReturnItem, b: *Binding) ?*const Node {
    if (item.func != null or item.property != null or item.variable == null) return null;
    const n = bindingGet(b, item.variable) orelse return null;
    return if (n.id > 0) n else null;
}

/// Name a carried node is bound under: the AST-owned alias or variable, which
/// outlives every binding of the query (bindingSet does not copy the name).
fn withItemNodeAlias(item: *const ReturnItem) CStr {
    return if (item.alias != null) item.alias else item.variable;
}

/// Append "<v>|" to a group key with snprintf truncation: the length never
/// passes the last byte.
fn groupKeyAppend(key: []u8, kl: *c_int, v: CStr) void {
    kl.* += @intCast(bprint(key[ix(kl.*)..], "{s}|", .{sp(v)}));
    if (kl.* >= key.len) kl.* = @intCast(key.len - 1);
}

/// Build a group key from non-aggregate WITH items.
fn withAggBuildKey(wc: *const ReturnClause, b: *Binding, key: []u8) void {
    var kl: c_int = 0;
    for (0..cnt(wc.count)) |ci| {
        if (isAggregateFunc(wc.items[ci].func)) continue;
        var vbuf: [PROP_BUF_SZ]u8 = undefined;
        const gn = withItemCarriedNode(at(wc.items, ci), b);
        // Group a node by identity, never by its (non-unique) name.
        if (gn) |node| _ = bprint(&vbuf, "#{d}", .{node.id});
        const v: CStr = if (gn != null) &vbuf else projectItem(b, at(wc.items, ci), &vbuf);
        groupKeyAppend(key, &kl, v);
    }
}

/// Find or create an aggregation group. Returns its index.
fn withAggFindOrCreate(aggs: *[*c]WithAgg, agg_cnt: *c_int, agg_cap: *c_int, groups: *KeyIndex, wc: *const ReturnClause, b: *Binding, key: CStr) c_int {
    var existing = keyIndexFind(groups, key);
    var a: c_int = 0;
    while (existing == KEY_INDEX_UNKNOWN and a < agg_cnt.*) : (a += 1) {
        if (eql(&aggs.*[ix(a)].group_key, key)) existing = a;
    }
    if (existing >= 0) return existing;
    if (agg_cnt.* >= agg_cap.*) {
        agg_cap.* *= 2;
        aggs.* = safeReallocN(WithAgg, aggs.*, ix(agg_cap.*));
    }
    const found = agg_cnt.*;
    agg_cnt.* += 1;
    const g = at(aggs.*, ix(found));
    const n = ix(wc.count);
    _ = bprint(&g.group_key, "{s}", .{sp(key)});
    keyIndexAdd(groups, &g.group_key, found);
    g.group_vals = callocN(CStr, n);
    g.sums = callocN(f64, n);
    g.counts = callocN(c_int, n);
    g.mins = callocN(f64, n);
    g.maxs = callocN(f64, n);
    g.distinct_lists = callocN([*c][*c]u8, n);
    g.distinct_n = callocN(c_int, n);
    g.distinct_sets = callocN(?*c.CBMHashTable, n);
    g.group_nodes = callocN(Node, n);
    for (0..n) |ci| {
        g.mins[ci] = DBL_MAX;
        g.maxs[ci] = -DBL_MAX;
    }
    for (0..n) |ci| {
        if (isAggregateFunc(wc.items[ci].func)) {
            g.group_vals[ci] = heapStrdup("0");
            continue;
        }
        var vbuf: [PROP_BUF_SZ]u8 = undefined;
        g.group_vals[ci] = heapStrdup(projectItem(b, at(wc.items, ci), &vbuf));
        if (withItemCarriedNode(at(wc.items, ci), b)) |gn| {
            if (g.group_nodes != null) nodeDeepCopy(at(g.group_nodes, ci), gn);
        }
    }
    return found;
}

/// Whether an aggregate's argument has a value in this row: `*`, a WITH
/// projection name, or a bound node/edge. An OPTIONAL MATCH with no match
/// keeps its row with the variable unbound, which COUNT must skip (#58).
fn bindingHasValue(b: *Binding, v: CStr, prop: CStr) bool {
    if (v == null or eql(v, "*")) return true;
    var full: [256]u8 = undefined;
    if (prop != null) {
        _ = bprint(&full, "{s}.{s}", .{ sp(v), sp(prop) });
    } else {
        _ = bprint(&full, "{s}", .{sp(v)});
    }
    for (0..cnt(b.var_count)) |i| {
        if (eql(b.var_names[i], &full)) return true;
    }
    return bindingGetEdge(b, v) != null or bindingGet(b, v) != null;
}

/// COUNT(x) over a row where x is unbound: skipped, so it counts 0, not 1.
fn countSkipsRow(item: *const ReturnItem, b: *Binding) bool {
    return eql(item.func, "COUNT") and !bindingHasValue(b, item.variable, item.property);
}

/// Accumulate aggregation values for a binding.
fn withAggAccumulate(agg: *WithAgg, wc: *const ReturnClause, b: *Binding) void {
    for (0..cnt(wc.count)) |ci| {
        const item = at(wc.items, ci);
        if (!isAggregateFunc(item.func) or countSkipsRow(item, b)) continue;
        agg.counts[ci] += 1;
        const raw = bindingGetVirtual(b, item.variable, item.property);
        if (item.distinct and eql(item.func, "COUNT")) {
            distinctListAdd(at(agg.distinct_lists, ci), at(agg.distinct_n, ci), at(agg.distinct_sets, ci), raw);
        }
        const dv = c.strtod(raw, null);
        agg.sums[ci] += dv;
        if (dv < agg.mins[ci]) agg.mins[ci] = dv;
        if (dv > agg.maxs[ci]) agg.maxs[ci] = dv;
    }
}

/// "%.10g" through libc: std.fmt has no %g and prints floats differently.
fn formatG10(buf: []u8, v: f64) void {
    _ = c.snprintf(buf.ptr, buf.len, "%.10g", v);
}

fn average(sum: f64, count: c_int) f64 {
    return if (count > 0) sum / @as(f64, @floatFromInt(count)) else 0.0;
}

/// Format a WITH aggregation value into buf.
fn withAggFormat(func: CStr, agg: *const WithAgg, ci: usize, buf: []u8) void {
    if (eql(func, "SUM")) {
        formatG10(buf, agg.sums[ci]);
    } else if (eql(func, "AVG")) {
        formatG10(buf, average(agg.sums[ci], agg.counts[ci]));
    } else if (eql(func, "MIN")) {
        formatG10(buf, agg.mins[ci]);
    } else if (eql(func, "MAX")) {
        formatG10(buf, agg.maxs[ci]);
    } else {
        _ = bprint(buf, "{d}", .{agg.counts[ci]});
    }
}

/// Add a scalar virtual variable binding for one WITH item. The value lives in
/// .name; the owned alias string (var_names points at it) lives in .project,
/// which no property accessor exposes. It used to live in .qualified_name, so
/// alias.qualified_name / keys(alias) returned the variable name as data (#57).
fn withAddVbindingVar(vb: *Binding, alias: CStr, val: CStr) void {
    var vn = std.mem.zeroes(Node);
    vn.name = heapStrdup(val);
    vn.project = heapStrdup(alias);
    if (vb.var_count < MAX_VARS) {
        vb.var_names[ix(vb.var_count)] = vn.project;
        vb.var_nodes[ix(vb.var_count)] = vn;
        vb.var_count += 1;
    }
}

/// Free a WithAgg array.
fn withAggFree(aggs: [*c]WithAgg, agg_cnt: c_int, item_count: c_int) void {
    for (0..cnt(agg_cnt)) |a| {
        const g = at(aggs, a);
        for (0..cnt(item_count)) |ci| {
            strFree(at(g.group_vals, ci));
            if (g.distinct_sets != null and g.distinct_sets[ci] != null) c.cbm_ht_free(g.distinct_sets[ci]);
            if (g.distinct_lists != null and g.distinct_lists[ci] != null) {
                for (0..cnt(g.distinct_n[ci])) |j| c.free(g.distinct_lists[ci][j]);
                c.free(@ptrCast(g.distinct_lists[ci]));
            }
        }
        c.free(@ptrCast(g.group_vals));
        c.free(@ptrCast(g.sums));
        c.free(@ptrCast(g.counts));
        c.free(@ptrCast(g.mins));
        c.free(@ptrCast(g.maxs));
        c.free(@ptrCast(g.distinct_lists));
        c.free(@ptrCast(g.distinct_n));
        c.free(@ptrCast(g.distinct_sets));
        if (g.group_nodes != null) {
            for (0..cnt(item_count)) |ci| nodeFieldsFree(at(g.group_nodes, ci));
        }
        c.free(@ptrCast(g.group_nodes));
    }
    c.free(@ptrCast(aggs));
}

/// Execute the WITH aggregation path.
fn executeWithAggregate(wc: *const ReturnClause, bindings: [*c]Binding, bind_count: c_int, vbindings: *[*c]Binding, vcount: *c_int) void {
    var agg_cap: c_int = 256;
    var aggs = callocN(WithAgg, ix(agg_cap));
    var agg_cnt: c_int = 0;
    var groups: KeyIndex = .{};

    for (0..cnt(bind_count)) |bi| {
        var key: [1024]u8 = undefined;
        key[0] = 0;
        withAggBuildKey(wc, at(bindings, bi), &key);
        const found = withAggFindOrCreate(&aggs, &agg_cnt, &agg_cap, &groups, wc, at(bindings, bi), &key);
        withAggAccumulate(at(aggs, ix(found)), wc, at(bindings, bi));
    }
    keyIndexFree(&groups);

    vbindings.* = safeReallocN(Binding, vbindings.*, ix(agg_cnt + 1));
    if (vbindings.* == null) {
        withAggFree(aggs, agg_cnt, wc.count);
        return;
    }
    for (0..ix(agg_cnt)) |a| {
        var vb = EMPTY_BINDING;
        // Carry the store so nodeProp can compute a carried node's
        // in_degree/out_degree on the projected virtual binding.
        vb.store = if (bind_count > 0) bindings[0].store else null;
        for (0..cnt(wc.count)) |ci| {
            const item = at(wc.items, ci);
            var name_buf: [256]u8 = undefined;
            const alias = resolveItemAlias(item, &name_buf);
            if (isAggregateFunc(item.func)) {
                var vbuf: [64]u8 = undefined;
                if (item.distinct and eql(item.func, "COUNT")) {
                    _ = bprint(&vbuf, "{d}", .{aggs[a].distinct_n[ci]}); // #239
                } else {
                    withAggFormat(item.func, at(aggs, a), ci, &vbuf);
                }
                withAddVbindingVar(&vb, alias, &vbuf);
            } else if (aggs[a].group_nodes != null and aggs[a].group_nodes[ci].id > 0) {
                bindingSet(&vb, withItemNodeAlias(item), (aggs[a].group_nodes + ci));
            } else {
                withAddVbindingVar(&vb, alias, aggs[a].group_vals[ci]);
            }
        }
        vbindings.*[ix(vcount.*)] = vb;
        vcount.* += 1;
    }
    withAggFree(aggs, agg_cnt, wc.count);
}

/// Execute WITH simple (non-aggregate) projection.
fn executeWithSimple(wc: *const ReturnClause, bindings: [*c]Binding, bind_count: c_int, vbindings: [*c]Binding, vcount: *c_int) void {
    for (0..cnt(bind_count)) |bi| {
        var vb = EMPTY_BINDING;
        vb.store = bindings[bi].store; // so nodeProp can compute degrees on the projection
        for (0..cnt(wc.count)) |ci| {
            const item = at(wc.items, ci);
            var name_buf: [256]u8 = undefined;
            const alias = resolveItemAlias(item, &name_buf);
            if (withItemCarriedNode(item, at(bindings, bi))) |carried| {
                bindingSet(&vb, withItemNodeAlias(item), carried);
                continue;
            }
            var func_buf: [PROP_BUF_SZ]u8 = undefined;
            withAddVbindingVar(&vb, alias, projectItem(at(bindings, bi), item, &func_buf));
        }
        vbindings[ix(vcount.*)] = vb;
        vcount.* += 1;
    }
}

/// Apply a post-WITH (or late) WHERE filter.
fn filterBindingsWhere(where: [*c]const c.cbm_where_clause_t, vbindings: [*c]Binding, vcount: *c_int) void {
    var kept: usize = 0;
    for (0..cnt(vcount.*)) |i| {
        if (evalWhere(where, at(vbindings, i))) {
            if (kept != i) vbindings[kept] = vbindings[i];
            kept += 1;
        } else {
            bindingFree(at(vbindings, i));
        }
    }
    vcount.* = @intCast(kept);
}

/// One WITH output item's value on a projected vbinding, looked up by the
/// item's alias; used to detect duplicate rows for WITH DISTINCT (#238).
fn withItemValue(wc: *const ReturnClause, b: *Binding, ci: usize) CStr {
    var name_buf: [256]u8 = undefined;
    const alias = resolveItemAlias(at(wc.items, ci), &name_buf);
    const v = bindingGetVirtual(b, alias, null);
    return if (v != null) v else "";
}

/// Whether vbinding i's value tuple equals one of the first `kept` tuples.
fn withRowSeenBefore(wc: *const ReturnClause, vbindings: [*c]Binding, i: usize, kept: usize) bool {
    for (0..kept) |j| {
        var same = true;
        var ci: usize = 0;
        while (ci < cnt(wc.count) and same) : (ci += 1) {
            same = eql(withItemValue(wc, at(vbindings, i), ci), withItemValue(wc, at(vbindings, j), ci));
        }
        if (same) return true;
    }
    return false;
}

/// Apply WITH DISTINCT: drop projected rows whose value tuple duplicates an
/// earlier one, keeping the first occurrence (#238).
fn withApplyDistinct(wc: *const ReturnClause, vbindings: [*c]Binding, vcount: *c_int) void {
    const vals = mallocN(CStr, if (wc.count > 0) ix(wc.count) else 1);
    var seen: KeyIndex = .{ .failed = vals == null };
    var key_buf: [*c]u8 = null;
    var key_cap: usize = 0;
    var kept: usize = 0;
    for (0..cnt(vcount.*)) |i| {
        var key: CStr = null;
        if (!seen.failed) {
            for (0..cnt(wc.count)) |ci| vals[ci] = withItemValue(wc, at(vbindings, i), ci);
            key = rowKey(vals, wc.count, &key_buf, &key_cap);
            seen.failed = key == null;
        }
        const found = keyIndexFind(&seen, key);
        const dup = if (found == KEY_INDEX_UNKNOWN) withRowSeenBefore(wc, vbindings, i, kept) else found >= 0;
        if (dup) {
            bindingFree(at(vbindings, i));
        } else {
            keyIndexAdd(&seen, key, @intCast(kept));
            if (kept != i) vbindings[kept] = vbindings[i];
            kept += 1;
        }
    }
    vcount.* = @intCast(kept);
    keyIndexFree(&seen);
    c.free(key_buf);
    c.free(@ptrCast(vals));
}

fn executeWithClause(q: *const Query, bindings_ptr: *[*c]Binding, bind_count_ptr: *c_int) void {
    const wc: *const ReturnClause = q.with_clause orelse return;
    const bindings = bindings_ptr.*;
    const bind_count = bind_count_ptr.*;

    var vbindings = mallocN(Binding, ix(bind_count + 1));
    var vcount: c_int = 0;

    var has_agg = false;
    for (0..cnt(wc.count)) |i| {
        if (isAggregateFunc(wc.items[i].func)) {
            has_agg = true;
            break;
        }
    }

    if (has_agg) {
        executeWithAggregate(wc, bindings, bind_count, &vbindings, &vcount);
    } else {
        executeWithSimple(wc, bindings, bind_count, vbindings, &vcount);
    }

    // WITH DISTINCT: dedup projected rows (no-op for aggregation, which already
    // collapses to one row per group).
    if (wc.distinct) withApplyDistinct(wc, vbindings, &vcount);

    withSortSkipLimit(wc, vbindings, &vcount);

    for (0..cnt(bind_count)) |bi| bindingFree(at(bindings, bi));
    c.free(@ptrCast(bindings));

    if (q.post_with_where != null) filterBindingsWhere(q.post_with_where, vbindings, &vcount);

    bindings_ptr.* = vbindings;
    bind_count_ptr.* = vcount;
}

// ── Execute a single query (no UNION recursion) ────────────────────

/// Has this variable already been collected? A query may name the same
/// variable in more than one pattern, and RETURN * must give it one set of
/// columns.
fn starVarSeen(vars: [*c]const CStr, vc: usize, name: CStr) bool {
    for (0..vc) |i| {
        if (eql(vars[i], name)) return true;
    }
    return false;
}

/// Collect the variables a RETURN * projects, in the order the query names
/// them and with no repeats. Without the repeat check, `MATCH (f) OPTIONAL
/// MATCH (f)-[:CALLS]->(g)` names f in two patterns and f gets its four
/// columns twice.
fn collectPatternVars(q: *const Query, vars: [*c]CStr, max_vars: usize) usize {
    var vc: usize = 0;
    for (0..cnt(q.pattern_count)) |pi| {
        const pat = at(q.patterns, pi);
        var ni: usize = 0;
        while (ni < cnt(pat.node_count) and vc < max_vars) : (ni += 1) {
            const v = pat.nodes[ni].variable;
            if (v != null and !starVarSeen(vars, vc, v)) {
                vars[vc] = v;
                vc += 1;
            }
        }
        var ri: usize = 0;
        while (ri < cnt(pat.rel_count) and vc < max_vars) : (ri += 1) {
            const v = pat.rels[ri].variable;
            if (v != null and !starVarSeen(vars, vc, v)) {
                vars[vc] = v;
                vc += 1;
            }
        }
    }
    return vc;
}

/// Build star-projection columns: var.name, var.qualified_name, var.label,
/// var.file_path.
fn buildStarColumns(rb: *ResultBuilder, vars: [*c]const CStr, vc: usize) void {
    const col_n = vc * NODE_COLS;
    var col_names: [128]CStr = undefined;
    for (0..vc) |v| {
        var buf: [128]u8 = undefined;
        _ = bprint(&buf, "{s}.name", .{sp(vars[v])});
        col_names[v * NODE_COLS] = heapStrdup(&buf);
        _ = bprint(&buf, "{s}.qualified_name", .{sp(vars[v])});
        col_names[v * NODE_COLS + 1] = heapStrdup(&buf);
        _ = bprint(&buf, "{s}.label", .{sp(vars[v])});
        col_names[v * NODE_COLS + 2] = heapStrdup(&buf);
        _ = bprint(&buf, "{s}.file_path", .{sp(vars[v])});
        col_names[v * NODE_COLS + 3] = heapStrdup(&buf);
    }
    rbSetColumns(rb, &col_names, @intCast(col_n));
    for (col_names[0..col_n]) |*name| strFree(name);
}

fn orEmpty(s: CStr) CStr {
    return if (s != null) s else "";
}

/// Project one variable's 4 columns for RETURN *.
fn projectStarVar(b: *Binding, v: CStr, vals: [*c]CStr) void {
    if (bindingGetEdge(b, v)) |edge| {
        vals[0] = edgeProp(edge, "type");
        vals[1] = "";
        vals[2] = "";
        vals[3] = "";
        return;
    }
    const n = bindingGet(b, v);
    vals[0] = if (n) |node| orEmpty(node.name) else "";
    vals[1] = if (n) |node| orEmpty(node.qualified_name) else "";
    vals[2] = if (n) |node| orEmpty(node.label) else "";
    vals[3] = if (n) |node| orEmpty(node.file_path) else "";
}

/// RETURN * after a WITH.
///
/// The pattern's variables are out of scope by this point — the WITH replaced
/// them with the names it made. Each of those names holds one value, not a
/// node, so each is ONE column rather than the four a node variable gets.
///
/// Reading the pattern here instead is the fault this function exists to
/// avoid: it named variables the bindings no longer hold, found nothing for
/// every one of them, and answered a full result of empty strings with no
/// error.
fn executeReturnStarAfterWith(q: *const Query, bindings: [*c]Binding, bind_count: c_int, max_rows: c_int, rb: *ResultBuilder) void {
    const wc = q.with_clause;
    var name_bufs: [MAX_VARS][128]u8 = undefined;
    var cols: [MAX_VARS]CStr = undefined;
    // parseReturnOrWith refuses a WITH wider than MAX_VARS, so this clamp
    // cannot fire. It stays as the bound this function relies on.
    const col_n: usize = @min(cnt(wc.*.count), MAX_VARS);
    for (0..col_n) |i| cols[i] = resolveItemAlias(at(wc.*.items, i), &name_bufs[i]);
    rbSetColumns(rb, &cols, @intCast(col_n));
    var seen: RbDistinct = .{ .on = q.ret != null and q.ret.*.distinct };
    for (0..cnt(bind_count)) |bi| {
        var vals: [MAX_VARS]CStr = undefined;
        for (0..col_n) |i| {
            const vn = bindingGet(at(bindings, bi), cols[i]);
            vals[i] = if (vn) |n| orEmpty(n.name) else "";
        }
        if (!rbAddRowCapped(rb, &seen, &vals, max_rows)) break;
    }
    rbDistinctFree(&seen);
}

fn executeReturnStar(q: *const Query, bindings: [*c]Binding, bind_count: c_int, max_rows: c_int, rb: *ResultBuilder) void {
    if (q.with_clause != null) {
        executeReturnStarAfterWith(q, bindings, bind_count, max_rows, rb);
        return;
    }
    var vars: [32]CStr = undefined;
    const vc = collectPatternVars(q, &vars, vars.len);
    buildStarColumns(rb, &vars, vc);
    var seen: RbDistinct = .{ .on = q.ret != null and q.ret.*.distinct };
    for (0..cnt(bind_count)) |bi| {
        var vals: [128]CStr = undefined;
        for (0..vc) |v| projectStarVar(at(bindings, bi), vars[v], @as([*c]CStr, &vals) + v * NODE_COLS);
        if (!rbAddRowCapped(rb, &seen, &vals, max_rows)) break;
    }
    rbDistinctFree(&seen);
}

/// Format a COLLECT list as a JSON array string.
fn formatCollectList(items: [*c]const [*c]u8, item_count: c_int, buf: []u8) void {
    var cbuf: [2048]u8 = undefined;
    cbuf[0] = '[';
    var bl: usize = 1;
    for (0..cnt(item_count)) |i| {
        if (i > 0) {
            cbuf[bl] = ',';
            bl += 1;
        }
        bl += bprint(cbuf[bl..], "\"{s}\"", .{sp(items[i])});
        if (bl >= cbuf.len) bl = cbuf.len - 1;
    }
    if (bl < cbuf.len - 1) {
        cbuf[bl] = ']';
        bl += 1;
    }
    cbuf[bl] = 0;
    _ = bprint(buf, "{s}", .{cbuf[0..bl]});
}

fn formatAggValue(func: CStr, count: c_int, sum: f64, min_val: f64, max_val: f64, collect_lists: [*c][*c][*c]u8, collect_counts: [*c]c_int, ci: usize, buf: []u8) void {
    if (eql(func, "SUM")) {
        formatG10(buf, sum);
    } else if (eql(func, "AVG")) {
        formatG10(buf, average(sum, count));
    } else if (eql(func, "MIN")) {
        formatG10(buf, min_val);
    } else if (eql(func, "MAX")) {
        formatG10(buf, max_val);
    } else if (eql(func, "COLLECT")) {
        formatCollectList(collect_lists[ci], collect_counts[ci], buf);
    } else {
        _ = bprint(buf, "{d}", .{count});
    }
}

/// RETURN aggregation entry.
const RetAgg = extern struct {
    group_key: [1024]u8,
    group_vals: [*c]CStr,
    sums: [*c]f64,
    counts: [*c]c_int,
    mins: [*c]f64,
    maxs: [*c]f64,
    collect_lists: [*c][*c][*c]u8,
    collect_counts: [*c]c_int,
    distinct_sets: [*c]?*c.CBMHashTable, // per-item hash over collect_lists for COUNT(DISTINCT)
};

/// Initialize a new RETURN aggregation group.
fn retAggInitGroup(entry: *RetAgg, key: CStr, item_count: c_int, vals: [*c]const CStr) void {
    const n = ix(item_count);
    _ = bprint(&entry.group_key, "{s}", .{sp(key)});
    entry.group_vals = callocN(CStr, n);
    entry.sums = callocN(f64, n);
    entry.counts = callocN(c_int, n);
    entry.mins = mallocN(f64, n);
    entry.maxs = mallocN(f64, n);
    entry.collect_lists = callocN([*c][*c]u8, n);
    entry.collect_counts = callocN(c_int, n);
    entry.distinct_sets = callocN(?*c.CBMHashTable, n);
    for (0..n) |ci| {
        entry.mins[ci] = DBL_MAX;
        entry.maxs[ci] = -DBL_MAX;
        entry.group_vals[ci] = heapStrdup(vals[ci]);
    }
}

/// Accumulate a binding into RETURN aggregation.
fn retAggAccumulate(entry: *RetAgg, ret: *const ReturnClause, b: *Binding) void {
    for (0..cnt(ret.count)) |ci| {
        const item = at(ret.items, ci);
        if (!isAggregateFunc(item.func) or countSkipsRow(item, b)) continue;
        entry.counts[ci] += 1;
        const raw = bindingGetVirtual(b, item.variable, item.property);
        const dv = c.strtod(raw, null);
        entry.sums[ci] += dv;
        if (dv < entry.mins[ci]) entry.mins[ci] = dv;
        if (dv > entry.maxs[ci]) entry.maxs[ci] = dv;
        if (eql(item.func, "COLLECT")) {
            const idx = ix(entry.collect_counts[ci]);
            entry.collect_counts[ci] += 1;
            entry.collect_lists[ci] = safeReallocN([*c]u8, entry.collect_lists[ci], idx + 1);
            entry.collect_lists[ci][idx] = heapStrdup(raw);
        } else if (item.distinct and eql(item.func, "COUNT")) {
            // COUNT(DISTINCT x): track unique values; emit the set size (#239).
            distinctListAdd(at(entry.collect_lists, ci), at(entry.collect_counts, ci), at(entry.distinct_sets, ci), raw);
        }
    }
}

/// Free RETURN aggregation entries.
fn retAggFree(aggs: [*c]RetAgg, agg_count: c_int, item_count: c_int) void {
    for (0..cnt(agg_count)) |a| {
        const g = at(aggs, a);
        for (0..cnt(item_count)) |ci| {
            strFree(at(g.group_vals, ci));
            if (g.distinct_sets != null and g.distinct_sets[ci] != null) c.cbm_ht_free(g.distinct_sets[ci]);
            for (0..cnt(g.collect_counts[ci])) |j| c.free(g.collect_lists[ci][j]);
            c.free(@ptrCast(g.collect_lists[ci]));
        }
        c.free(@ptrCast(g.group_vals));
        c.free(@ptrCast(g.sums));
        c.free(@ptrCast(g.counts));
        c.free(@ptrCast(g.mins));
        c.free(@ptrCast(g.maxs));
        c.free(@ptrCast(g.collect_lists));
        c.free(@ptrCast(g.collect_counts));
        c.free(@ptrCast(g.distinct_sets));
    }
    c.free(@ptrCast(aggs));
}

/// Build the group key and projected values for one binding.
fn retAggBuildKey(ret: *const ReturnClause, b: *Binding, key: []u8, vals: [*c]CStr, valbufs: *[MAX_COLS][PROP_BUF_SZ]u8) void {
    var klen: c_int = 0;
    for (0..cnt(ret.count)) |ci| {
        if (isAggregateFunc(ret.items[ci].func)) {
            vals[ci] = "0";
            continue;
        }
        // projectItem may return its own scratch (a stable string or a
        // per-column buffer it copied into); persist the value in the
        // caller-owned valbufs so vals[] survives until retAggInitGroup
        // strdup's it.
        const v = projectItem(b, at(ret.items, ci), &valbufs[ci]);
        if (v != @as(CStr, &valbufs[ci])) _ = bprint(&valbufs[ci], "{s}", .{sp(orEmpty(v))});
        vals[ci] = &valbufs[ci];
        groupKeyAppend(key, &klen, vals[ci]);
    }
}

/// Emit one aggregated row into the result builder.
fn retAggEmitRow(ret: *const ReturnClause, agg: *const RetAgg, rb: *ResultBuilder) void {
    var row: [MAX_COLS]CStr = undefined;
    var bufs: [MAX_COLS][64]u8 = undefined;
    for (0..cnt(ret.count)) |ci| {
        const item = at(ret.items, ci);
        if (!isAggregateFunc(item.func)) {
            row[ci] = agg.group_vals[ci];
            continue;
        }
        if (item.distinct and eql(item.func, "COUNT")) {
            // COUNT(DISTINCT x) — number of unique values accumulated (#239).
            _ = bprint(&bufs[ci], "{d}", .{agg.collect_counts[ci]});
            row[ci] = &bufs[ci];
            continue;
        }
        formatAggValue(item.func, agg.counts[ci], agg.sums[ci], agg.mins[ci], agg.maxs[ci], agg.collect_lists, agg.collect_counts, ci, &bufs[ci]);
        row[ci] = &bufs[ci];
    }
    rbAddRow(rb, &row);
}

fn executeReturnAgg(ret: *const ReturnClause, bindings: [*c]Binding, bind_count: c_int, rb: *ResultBuilder) void {
    var agg_cap: c_int = 256;
    var aggs = callocN(RetAgg, ix(agg_cap));
    var agg_count: c_int = 0;
    var groups: KeyIndex = .{};

    for (0..cnt(bind_count)) |bi| {
        // Grouping dominates a whole-graph GROUP BY. Abort if the wall-clock
        // budget is blown.
        if ((bi & DEADLINE_CHECK_MASK) == 0 and deadlineExceeded()) break;
        var key: [1024]u8 = undefined;
        key[0] = 0;
        var vals: [MAX_COLS]CStr = undefined;
        var valbufs: [MAX_COLS][PROP_BUF_SZ]u8 = undefined;
        retAggBuildKey(ret, at(bindings, bi), &key, &vals, &valbufs);

        var found = keyIndexFind(&groups, &key);
        var a: c_int = 0;
        while (found == KEY_INDEX_UNKNOWN and a < agg_count) : (a += 1) {
            if (eql(&aggs[ix(a)].group_key, &key)) found = a;
        }
        if (found < 0) {
            if (agg_count >= agg_cap) {
                agg_cap *= 2;
                aggs = safeReallocN(RetAgg, aggs, ix(agg_cap));
            }
            found = agg_count;
            agg_count += 1;
            retAggInitGroup(at(aggs, ix(found)), &key, ret.count, &vals);
            keyIndexAdd(&groups, &aggs[ix(found)].group_key, found);
        }
        retAggAccumulate(at(aggs, ix(found)), ret, at(bindings, bi));
    }
    keyIndexFree(&groups);

    for (0..ix(agg_count)) |a| retAggEmitRow(ret, at(aggs, a), rb);
    retAggFree(aggs, agg_count, ret.count);
}

/// Build RETURN column names from items.
fn buildReturnColumns(rb: *ResultBuilder, ret: *const ReturnClause) void {
    var col_names: [MAX_COLS]CStr = undefined;
    const n = @min(cnt(ret.count), MAX_COLS);
    for (0..n) |i| {
        const item = at(ret.items, i);
        if (item.alias != null) {
            col_names[i] = item.alias;
        } else if (item.func != null) {
            var buf: [128]u8 = undefined;
            if (item.has_index) {
                _ = bprint(&buf, "{s}({s})[{d}]", .{ sp(item.func), sp(item.variable), item.index });
            } else {
                _ = bprint(&buf, "{s}({s})", .{ sp(item.func), sp(item.variable) });
            }
            col_names[i] = heapStrdup(&buf);
        } else if (item.kase != null) {
            col_names[i] = "CASE";
        } else if (item.property != null) {
            var buf: [128]u8 = undefined;
            _ = bprint(&buf, "{s}.{s}", .{ sp(item.variable), sp(item.property) });
            col_names[i] = heapStrdup(&buf);
        } else {
            col_names[i] = item.variable;
        }
    }
    rbSetColumns(rb, &col_names, ret.count);
    for (0..n) |i| {
        const item = at(ret.items, i);
        if (item.alias == null and (item.func != null or (item.kase == null and item.property != null))) {
            strFree(&col_names[i]);
        }
    }
}

/// Execute simple (non-aggregate) RETURN projection.
fn executeReturnSimple(ret: *const ReturnClause, bindings: [*c]Binding, bind_count: c_int, max_rows: c_int, rb: *ResultBuilder) void {
    var proj_cap = max_rows;
    if (ret.limit > 0 and ret.order_key_count == 0 and ret.skip <= 0) {
        proj_cap = ret.limit; // with DISTINCT, the cap counts distinct rows
    }
    var seen: RbDistinct = .{ .on = ret.distinct };
    for (0..cnt(bind_count)) |bi| {
        var vals: [MAX_COLS]CStr = undefined;
        var func_bufs: [MAX_COLS][PROP_BUF_SZ]u8 = undefined;
        for (0..cnt(ret.count)) |ci| vals[ci] = projectItem(at(bindings, bi), at(ret.items, ci), &func_bufs[ci]);
        if (!rbAddRowCapped(rb, &seen, &vals, proj_cap)) break;
    }
    rbDistinctFree(&seen);
}

/// Build default 3-column headers (name, qualified_name, label) per variable.
fn buildDefaultColumns(rb: *ResultBuilder, vars: [*c]const CStr, vc: usize) void {
    const col_n = vc * EDGE_COLS;
    var col_names: [COL_BUF]CStr = undefined;
    for (0..vc) |v| {
        var buf: [128]u8 = undefined;
        _ = bprint(&buf, "{s}.name", .{sp(vars[v])});
        col_names[v * EDGE_COLS] = heapStrdup(&buf);
        _ = bprint(&buf, "{s}.qualified_name", .{sp(vars[v])});
        col_names[v * EDGE_COLS + 1] = heapStrdup(&buf);
        _ = bprint(&buf, "{s}.label", .{sp(vars[v])});
        col_names[v * EDGE_COLS + 2] = heapStrdup(&buf);
    }
    rbSetColumns(rb, &col_names, @intCast(col_n));
    for (col_names[0..col_n]) |*name| strFree(name);
}

/// Default projection when there is no RETURN clause.
fn executeDefaultProjection(pat0: *const c.cbm_pattern_t, bindings: [*c]Binding, bind_count: c_int, max_rows: c_int, rb: *ResultBuilder) void {
    var vars: [MAX_VARS]CStr = undefined;
    var vc: usize = 0;
    var ni: usize = 0;
    while (ni < cnt(pat0.node_count) and vc < MAX_VARS) : (ni += 1) {
        if (pat0.nodes[ni].variable != null) {
            vars[vc] = pat0.nodes[ni].variable;
            vc += 1;
        }
    }
    buildDefaultColumns(rb, &vars, vc);
    var bi: usize = 0;
    while (bi < cnt(bind_count) and rb.row_count < max_rows) : (bi += 1) {
        var vals: [COL_BUF]CStr = undefined;
        for (0..vc) |v| {
            const n = bindingGet(at(bindings, bi), vars[v]);
            vals[v * EDGE_COLS] = if (n) |node| orEmpty(node.name) else "";
            vals[v * EDGE_COLS + 1] = if (n) |node| orEmpty(node.qualified_name) else "";
            vals[v * EDGE_COLS + 2] = if (n) |node| orEmpty(node.label) else "";
        }
        rbAddRow(rb, &vals);
    }
}

/// Worst-case Binding slot count for a node cross-join: one row per (existing
/// binding x extra node), plus — for an OPTIONAL join with no extra nodes —
/// one fallback row per existing binding, plus a trailing sentinel slot.
///
/// The plain-int product bind_count * extra_count overflows to a
/// negative/garbage malloc size on large graphs, so the count is computed in
/// size_t and rejected if it would not fit the int binding counter or would
/// overflow the size_t byte size. Returns 0 and writes out_n on success,
/// CBM_NOT_FOUND on overflow. Exported so the arithmetic boundary can be unit
/// tested directly (upstream b07dbe67).
pub export fn cbm_cypher_cross_join_alloc(bind_count: c_int, extra_count: c_int, opt: bool, out_n: [*c]usize) c_int {
    const per_binding: usize = if (extra_count > 0) @intCast(extra_count) else if (opt) 1 else 0;
    // (size_t)bind_count: a negative count sign-extends, as in C.
    const rows = @as(usize, @bitCast(@as(isize, bind_count))) *% per_binding;
    if (rows > std.math.maxInt(c_int)) return NOT_FOUND; // new_count would not fit the int binding counter
    const n = rows + 1;
    if (n > std.math.maxInt(usize) / @sizeOf(Binding)) return NOT_FOUND; // alloc_n * sizeof(Binding) would overflow size_t
    out_n.* = n;
    return 0;
}

/// Cross-join a node-only pattern into existing bindings. Returns 0 on
/// success, CBM_NOT_FOUND when the allocation is refused (overflow) or fails
/// (OOM); the caller propagates that as a query error rather than silently
/// skipping the MATCH, which would return a wrong (short) result.
fn crossJoinNodes(bindings: *[*c]Binding, bind_count: *c_int, extra_nodes: [*c]Node, extra_count: c_int, nvar: CStr, opt: bool) c_int {
    var alloc_n: usize = 0;
    if (cbm_cypher_cross_join_alloc(bind_count.*, extra_count, opt, &alloc_n) != 0) {
        return NOT_FOUND; // product overflows int/size_t: fail the query
    }
    const new_bindings = mallocN(Binding, alloc_n);
    if (new_bindings == null) return NOT_FOUND; // OOM: propagate, don't silently skip the MATCH
    var new_count: usize = 0;
    var expired = false;
    var bi: usize = 0;
    while (bi < cnt(bind_count.*) and !expired) : (bi += 1) {
        var ni: usize = 0;
        while (ni < cnt(extra_count) and new_count < alloc_n) : (ni += 1) {
            // Stop producing rows once the budget is spent; the caller reports
            // the timeout instead of the partial join.
            if ((new_count & DEADLINE_CHECK_MASK) == 0 and deadlineExceededAt("cross_join")) {
                expired = true;
                break;
            }
            var nb = EMPTY_BINDING;
            bindingCopy(&nb, at(bindings.*, bi));
            bindingSet(&nb, nvar, at(extra_nodes, ni));
            new_bindings[new_count] = nb;
            new_count += 1;
        }
        if (opt and extra_count == 0 and new_count < alloc_n) {
            var nb = EMPTY_BINDING;
            bindingCopy(&nb, at(bindings.*, bi));
            new_bindings[new_count] = nb;
            new_count += 1;
        }
    }
    for (0..cnt(bind_count.*)) |i| bindingFree(at(bindings.*, i));
    c.free(@ptrCast(bindings.*));
    bindings.* = new_bindings;
    bind_count.* = @intCast(new_count); // new_count <= INT_MAX, guaranteed above
    return 0;
}

/// Cross-join a pattern with relationships into existing bindings.
fn crossJoinWithRels(store: ?*Store, patn: *const c.cbm_pattern_t, bindings: *[*c]Binding, bind_count: *c_int, extra_nodes: [*c]Node, extra_count: c_int, nvar: CStr, opt: bool) void {
    // Each pair's expansion is uncapped, so the output grows with
    // bindingOutAppend instead of a precomputed size; a precomputed
    // bind_count * extra_count product also overflowed int on large graphs
    // (#627).
    const new_cap: c_int = if (bind_count.* > 0 and extra_count > 0) bind_count.* else 1;
    var out: BindingList = .{ .rows = mallocN(Binding, ix(new_cap)), .count = 0, .cap = new_cap };
    if (out.rows == null) return; // OOM: leave existing bindings untouched rather than corrupt
    var expired = false;
    var bi: usize = 0;
    while (bi < cnt(bind_count.*) and !expired) : (bi += 1) {
        for (0..cnt(extra_count)) |ni| {
            // Every pair runs a relationship expansion, so check each one.
            if (deadlineExceededAt("cross_join_rels")) {
                expired = true;
                break;
            }
            var nb = EMPTY_BINDING;
            bindingCopy(&nb, at(bindings.*, bi));
            bindingSet(&nb, nvar, at(extra_nodes, ni));
            var tmp = mallocN(Binding, 2);
            tmp[0] = nb;
            var tc: c_int = 1;
            var tcap: c_int = 1;
            var tv: CStr = nvar;
            expandPatternRels(store, patn, &tmp, &tc, &tcap, &tv, opt);
            for (0..cnt(tc)) |ti| _ = bindingOutAppend(&out, at(tmp, ti));
            c.free(@ptrCast(tmp));
        }
        if (opt and extra_count == 0) {
            var nb = EMPTY_BINDING;
            bindingCopy(&nb, at(bindings.*, bi));
            _ = bindingOutAppend(&out, &nb);
        }
    }
    for (0..cnt(bind_count.*)) |i| bindingFree(at(bindings.*, i));
    c.free(@ptrCast(bindings.*));
    bindings.* = out.rows;
    bind_count.* = out.count;
}

/// Drive a single-relationship additional pattern from its ALREADY-BOUND
/// terminal node, binding the unbound START var to the edge's other endpoint.
///
/// Handles `OPTIONAL MATCH (c)-[:CALLS]->(f)` where `f` is bound from an
/// earlier MATCH and `c` is new: scanning every node for `c` and
/// cross-joining (a) risks an int-overflow OOB write on large graphs and (b)
/// leaves `c` bound to an arbitrary node so a later `WHERE c IS NULL` wrongly
/// drops every row (#627). Instead we scan only the bound terminal's edges and
/// bind `c` to real neighbours; with OPTIONAL we keep the row with `c` unbound
/// when there are none — the correct dead-code semantics.
fn expandFromBoundTerminal(store: ?*Store, patn: *const c.cbm_pattern_t, bindings: *[*c]Binding, bind_count: *c_int, start_var: CStr, opt: bool) void {
    const rel = at(patn.rels, 0);
    const start_node = at(patn.nodes, 0);
    // The relationship is written start-[r]->terminal. To enumerate the start
    // nodes reachable from the bound terminal we invert the stored direction.
    const rel_inbound = rel.direction != null and eql(rel.direction, "inbound");
    const scan_targets = !rel_inbound; // (start)->(term): start = edge source = scan term's inbound

    const new_cap: c_int = if (bind_count.* > 0) bind_count.* else GROWTH_10;
    var out: BindingList = .{ .rows = mallocN(Binding, ix(new_cap)), .count = 0, .cap = new_cap };
    if (out.rows == null) return;

    for (0..cnt(bind_count.*)) |bi| {
        const b = at(bindings.*, bi);
        const term_var: CStr = if (patn.nodes[1].variable != null) patn.nodes[1].variable else "";
        var match_count: c_int = 0;
        if (bindingGet(b, term_var)) |term| {
            const passes: usize = if (rel.type_count > 0) ix(rel.type_count) else 1;
            for (0..passes) |ti| {
                var edges: [*c]Edge = null;
                var edge_count: c_int = 0;
                if (rel.type_count > 0) {
                    if (scan_targets) {
                        _ = c.cbm_store_find_edges_by_target_type(store, term.id, rel.types[ti], &edges, &edge_count);
                    } else {
                        _ = c.cbm_store_find_edges_by_source_type(store, term.id, rel.types[ti], &edges, &edge_count);
                    }
                } else if (scan_targets) {
                    _ = c.cbm_store_find_edges_by_target(store, term.id, &edges, &edge_count);
                } else {
                    _ = c.cbm_store_find_edges_by_source(store, term.id, &edges, &edge_count);
                }
                for (0..cnt(edge_count)) |ei| {
                    const sid = if (scan_targets) edges[ei].source_id else edges[ei].target_id;
                    var found = std.mem.zeroes(Node);
                    if (c.cbm_store_find_node_by_id(store, sid, &found) != c.CBM_STORE_OK) continue;
                    if (start_node.label != null and !labelAltMatches(found.label, start_node.label)) {
                        nodeFieldsFree(&found);
                        continue;
                    }
                    var nb = EMPTY_BINDING;
                    bindingCopy(&nb, b);
                    bindingSet(&nb, start_var, &found);
                    if (rel.variable != null) bindingSetEdge(&nb, rel.variable, edges + ei);
                    nodeFieldsFree(&found);
                    match_count += 1;
                    _ = bindingOutAppend(&out, &nb);
                }
                c.cbm_store_free_edges(edges, edge_count);
            }
        }
        if (opt and match_count == 0) {
            // No matching neighbour: keep the row with start_var left UNBOUND so
            // `WHERE <start> IS NULL` correctly identifies the no-edge case.
            var nb = EMPTY_BINDING;
            bindingCopy(&nb, b);
            _ = bindingOutAppend(&out, &nb);
        }
    }

    for (0..cnt(bind_count.*)) |bi| bindingFree(at(bindings.*, bi));
    c.free(@ptrCast(bindings.*));
    bindings.* = out.rows;
    bind_count.* = out.count;
}

/// Expand additional MATCH patterns (pi >= 1).
fn expandAdditionalPatterns(store: ?*Store, q: *const Query, project: CStr, bindings: *[*c]Binding, bind_count: *c_int, bind_cap: *c_int) c_int {
    var pi: usize = 1;
    while (pi < cnt(q.pattern_count)) : (pi += 1) {
        const patn = at(q.patterns, pi);
        const opt = q.pattern_optional[pi];
        const nvar: CStr = if (patn.nodes[0].variable != null) patn.nodes[0].variable else "_n_extra";
        const start_bound = bind_count.* > 0 and bindingGet(at(bindings.*, 0), nvar) != null;

        if (start_bound and patn.rel_count > 0) {
            var tv = nvar;
            expandPatternRels(store, patn, bindings, bind_count, bind_cap, &tv, opt);
            continue;
        }

        // Single-rel pattern whose START is unbound but whose TERMINAL is
        // already bound: drive from the bound terminal instead of scanning all
        // nodes for the start var (avoids the int-overflow OOB write and the
        // c-IS-NULL corruption of #627).
        if (!start_bound and patn.rel_count == 1 and bind_count.* > 0) {
            const term_var = patn.nodes[1].variable;
            if (term_var != null and bindingGet(at(bindings.*, 0), term_var) != null) {
                expandFromBoundTerminal(store, patn, bindings, bind_count, nvar, opt);
                continue;
            }
        }

        var extra_nodes: [*c]Node = null;
        var extra_count: c_int = 0;
        scanPatternNodes(store, project, at(patn.nodes, 0), &extra_nodes, &extra_count);
        var rc: c_int = 0;
        if (patn.rel_count == 0) {
            rc = crossJoinNodes(bindings, bind_count, extra_nodes, extra_count, nvar, opt);
        } else {
            crossJoinWithRels(store, patn, bindings, bind_count, extra_nodes, extra_count, nvar, opt);
        }
        c.cbm_store_free_nodes(extra_nodes, extra_count);
        if (rc != 0) return rc; // allocation refused/failed: propagate as a query error
    }
    return 0;
}

/// Project RETURN clause results.
fn executeReturnClause(q: *const Query, ret: *const ReturnClause, bindings: [*c]Binding, bind_count: c_int, max_rows: c_int, rb: *ResultBuilder) void {
    var has_agg = false;
    for (0..cnt(ret.count)) |i| {
        if (isAggregateFunc(ret.items[i].func)) {
            has_agg = true;
            break;
        }
    }

    if (ret.star) {
        executeReturnStar(q, bindings, bind_count, max_rows, rb);
    } else {
        buildReturnColumns(rb, ret);
        if (has_agg) {
            executeReturnAgg(ret, bindings, bind_count, rb);
        } else {
            executeReturnSimple(ret, bindings, bind_count, max_rows, rb);
        }
    }

    // The simple and star projections deduplicate while projecting; only
    // aggregate rows still need the post-pass.
    if (ret.distinct and has_agg) rbApplyDistinct(rb);
    rbApplyOrderBy(rb, ret);
    rbApplySkipLimit(rb, ret.skip, if (ret.limit >= 0) ret.limit else max_rows);
}

fn varIs(name: CStr, v: CStr) bool {
    return name == null or eql(name, v);
}

/// Does every condition in e reference only v? An unbound variable counts as
/// a match, so NOT over a condition on a later variable would drop every row
/// if the WHERE ran before that variable is bound. An operator chain is linked
/// along the right child (parser.zig exprChainLink), so the right child is
/// followed in a loop and only the left operand recurses.
fn exprOnlyUsesVar(expr: [*c]const Expr, v: CStr) bool {
    var e = expr;
    while (e != null and e.*.type != c.EXPR_CONDITION) {
        if (e.*.left != null and !exprOnlyUsesVar(e.*.left, v)) return false;
        e = e.*.right; // null for NOT, whose only operand is left
    }
    if (e == null) return true;
    const cond = &e.*.cond;
    if (!varIs(cond.variable, v) or !varIs(cond.value_variable, v)) return false;
    for (0..cnt(cond.arg_count)) |i| {
        if (!varIs(cond.args[i].variable, v)) return false;
    }
    return true;
}

/// Early WHERE on the first scanned node: apply each top-level AND part that
/// references only that node; the late WHERE checks the full expression. The
/// AND chain is followed along its right child in a loop.
fn evalEarlyWhere(expr: [*c]const Expr, b: *Binding, v: CStr) bool {
    var e = expr;
    while (e != null and e.*.type == c.EXPR_AND) {
        if (!evalEarlyWhere(e.*.left, b, v)) return false;
        e = e.*.right;
    }
    return !exprOnlyUsesVar(e, v) or evalExpr(e, b);
}

fn executeSingle(store: ?*Store, q: *const Query, project: CStr, max_rows: c_int, rb: *ResultBuilder) c_int {
    const pat0: *const c.cbm_pattern_t = q.patterns;

    // Step 1: Scan initial nodes
    var scanned: [*c]Node = null;
    var scan_count: c_int = 0;
    scanPatternNodes(store, project, at(pat0.nodes, 0), &scanned, &scan_count);

    // Build initial bindings with early WHERE
    var bind_cap: c_int = if (scan_count > max_rows) scan_count else if (max_rows > 0) max_rows else 1;
    var bindings = mallocN(Binding, ix(bind_cap + 1));
    var bind_count: c_int = 0;
    var var_name: CStr = if (pat0.nodes[0].variable != null) pat0.nodes[0].variable else ANON_HEAD_VAR;
    const late_where = q.where != null and (pat0.rel_count > 0 or q.pattern_count > 1);

    var i: usize = 0;
    while (i < cnt(scan_count) and bind_count < bind_cap) : (i += 1) {
        if ((i & DEADLINE_CHECK_MASK) == 0 and deadlineExceeded()) break;
        var b = EMPTY_BINDING;
        b.store = store;
        bindingSet(&b, var_name, at(scanned, i));
        const pass = q.where == null or
            (if (late_where and q.where.*.root != null) evalEarlyWhere(q.where.*.root, &b, var_name) else evalWhere(q.where, &b));
        if (pass) {
            bindings[ix(bind_count)] = b;
            bind_count += 1;
        } else {
            bindingFree(&b);
        }
    }

    // Step 2: Expand first pattern's relationships
    expandPatternRels(store, pat0, &bindings, &bind_count, &bind_cap, &var_name, q.pattern_optional[0]);

    // Step 2b: Additional patterns
    if (expandAdditionalPatterns(store, q, project, &bindings, &bind_count, &bind_cap) != 0) {
        for (0..cnt(bind_count)) |bi| bindingFree(at(bindings, bi));
        c.free(@ptrCast(bindings));
        c.cbm_store_free_nodes(scanned, scan_count);
        return NOT_FOUND; // cross-join allocation refused/failed
    }

    // Step 3: Late WHERE
    if (late_where) filterBindingsWhere(q.where, bindings, &bind_count);

    // Step 3b: WITH clause
    executeWithClause(q, &bindings, &bind_count);

    // Step 4: Project results
    rbInit(rb);
    if (q.ret != null) {
        executeReturnClause(q, q.ret, bindings, bind_count, max_rows, rb);
    } else {
        executeDefaultProjection(pat0, bindings, bind_count, max_rows, rb);
    }

    for (0..cnt(bind_count)) |bi| bindingFree(at(bindings, bi));
    c.free(@ptrCast(bindings));
    c.cbm_store_free_nodes(scanned, scan_count);
    return 0;
}

// ── Main entry point ───────────────────────────────────────────────

pub export fn cbm_cypher_classify_plan(query: [*c]const Query, reason: [*c][*c]const u8) c.cbm_cypher_route_t {
    if (query == null or query.*.pattern_count <= 0 or query.*.patterns == null) {
        if (reason != null) reason.* = "missing logical plan";
        return c.CBM_CYPHER_ROUTE_UNSUPPORTED;
    }

    // Native traversal is deliberately narrow: the graph supplies topology,
    // while predicates, aggregation and presentation remain with the existing
    // relational executor in cbm.zova.
    const pattern = at(query.*.patterns, 0);
    if (query.*.pattern_count == 1 and pattern.rel_count > 0 and query.*.where == null and
        query.*.with_clause == null and query.*.union_next == null and query.*.unwind_expr == null)
    {
        if (reason != null) reason.* = "relationship topology supported by native graph";
        return c.CBM_CYPHER_ROUTE_NATIVE_GRAPH;
    }

    if (reason != null) reason.* = "rich semantics execute in database";
    return c.CBM_CYPHER_ROUTE_IN_DATABASE_COMPAT;
}

// ── Is every name a RETURN or WITH item uses actually in scope? ────
//
// An unbound variable resolves to NULL and renders as "" on purpose: an
// OPTIONAL MATCH target that found no row has to project a blank rather than
// drop the row. The cost of that convention is that a name the query NEVER
// carried through looks exactly the same on screen. A typo, or a variable a
// WITH dropped, then reads as "the graph holds no such data" instead of "your
// query named something that is not there" (#1919). Same failure shape as
// #373, and the same answer: say so out loud.
//
// The check runs on the parsed query, not on the run-time bindings, and that
// is the whole trick. It asks whether the query DECLARED the name — a
// different question from whether a row happened to bind it. So the OPTIONAL
// MATCH convention above is untouched: a target that did not match is still
// declared, still legal, and still projects "".

/// Cap for one query's declared names. MAX_VARS is the binding cap for nodes
/// alone; a pattern also names edges, so this is the roomier of the two. A
/// query that overruns it skips the check rather than guessing — a wrong
/// refusal costs the caller a working query, which is worse than the silence
/// this guard removes.
const SCOPE_MAX_NAMES = 32;

fn scopeHolds(names: []const CStr, want: CStr) bool {
    for (names) |name| {
        if (name != null and eql(name, want)) return true;
    }
    return false;
}

/// Every name the query's patterns declare, plus an UNWIND alias. Returns null
/// when there are more names than out holds.
fn collectDeclaredNames(q: *const Query, out: []CStr) ?usize {
    var n: usize = 0;
    for (0..cnt(q.pattern_count)) |pi| {
        const pat = at(q.patterns, pi);
        for (0..cnt(pat.node_count)) |ni| {
            const v = pat.nodes[ni].variable;
            if (v != null and !scopeHolds(out[0..n], v)) {
                if (n >= out.len) return null;
                out[n] = v;
                n += 1;
            }
        }
        for (0..cnt(pat.rel_count)) |ri| {
            const v = pat.rels[ri].variable;
            if (v != null and !scopeHolds(out[0..n], v)) {
                if (n >= out.len) return null;
                out[n] = v;
                n += 1;
            }
        }
    }
    if (q.unwind_alias != null and !scopeHolds(out[0..n], q.unwind_alias)) {
        if (n >= out.len) return null;
        out[n] = q.unwind_alias;
        n += 1;
    }
    return n;
}

/// What a WITH leaves behind: its alias where it made one, and the plain
/// variable where it carried one through whole. `WITH f.name AS caller`
/// leaves `caller` and nothing else — `f` is gone, which is the case #1919 is
/// about.
fn collectWithNames(wc: *const ReturnClause, out: []CStr) ?usize {
    var n: usize = 0;
    for (0..cnt(wc.count)) |i| {
        const item = at(wc.items, i);
        var name: CStr = null;
        if (item.alias != null) {
            name = item.alias;
        } else if (item.variable != null and item.property == null and item.func == null) {
            name = item.variable;
        }
        if (name != null and !scopeHolds(out[0..n], name)) {
            if (n >= out.len) return null;
            out[n] = name;
            n += 1;
        }
    }
    return n;
}

/// Build the message. It names the variable and the clause, because an error
/// that does not say WHICH name is wrong sends the reader back to guessing.
fn scopeError(v: CStr, clause: []const u8, why: []const u8) [*c]u8 {
    var buf: [256]u8 = undefined;
    _ = bprint(&buf, "variable '{s}' is not in scope for {s} — {s}", .{ sp(v), clause, why });
    return heapStrdup(&buf);
}

/// The variable this item really references, or null when it references
/// none. Two items carry a placeholder in that field rather than a name the
/// query declared: `count(*)` stores "*", and a CASE expression stores "CASE".
/// Both would read as an unknown variable, so neither is checkable here.
fn scopeCheckableVar(item: *const ReturnItem) CStr {
    if (item.kase != null or item.variable == null) return null;
    if (eql(item.variable, "*")) return null;
    return item.variable;
}

/// Says which limit the query passed and how to get under it. An unnamed node
/// takes no slot — except the head of the first pattern, which the engine
/// binds either way — so dropping a name the query never uses is the cheap
/// way out. Splitting the MATCH is NOT — every pattern of one query shares one
/// binding, which is why the caller counts across all of them. Separate
/// queries do work, because each gets a binding of its own.
fn varCapacityError(kind: []const u8, limit: c_int) [*c]u8 {
    var buf: [256]u8 = undefined;
    _ = bprint(&buf, "too many {s} variables: a query can name at most {d} — " ++
        "leave the name off the ones you do not use, or run separate queries", .{ kind, limit });
    return heapStrdup(&buf);
}

/// A binding holds a fixed number of variables: MAX_VARS node variables and
/// MAX_EDGE_VARS edge variables, both in plain arrays (see Binding).
/// bindingSet and bindingSetEdge drop anything past those without a word, so
/// a query naming more variables than a binding holds cannot be answered —
/// the extra names bind to nothing and project as empty strings, which reads
/// as "the graph holds no such data" rather than "this query is too wide".
///
/// Refuse such a query instead, before any row is touched. Bounding the input
/// here is also what stops collectDeclaredNames below overflowing its array:
/// the patterns contribute at most MAX_VARS + MAX_EDGE_VARS names, plus one
/// UNWIND alias, which is well inside SCOPE_MAX_NAMES.
///
/// Counts DISTINCT variables across every pattern, because they all land in
/// the same binding: a multi-MATCH query shares one, and an OPTIONAL MATCH
/// pattern sits in this same array (q.pattern_optional marks which). A node
/// variable and an edge variable may share a name and each take a slot,
/// because the binding keeps the two in separate arrays.
fn checkPatternVarCapacity(q: *const Query) [*c]u8 {
    var node_vars: [MAX_VARS]CStr = @splat(null);
    var edge_vars: [MAX_EDGE_VARS]CStr = @splat(null);
    var node_n: usize = 0;
    var edge_n: usize = 0;
    // The head of the first pattern always takes a node slot, named or not:
    // executeSingle binds it under ANON_HEAD_VAR when the query leaves it
    // unnamed. Count it first, or a query with an unnamed head and MAX_VARS
    // named nodes passes this check and still loses its last name in
    // bindingSet — the exact silence this guard exists to remove.
    if (q.pattern_count > 0 and q.patterns[0].node_count > 0 and q.patterns[0].nodes[0].variable == null) {
        node_vars[node_n] = ANON_HEAD_VAR;
        node_n += 1;
    }
    for (0..cnt(q.pattern_count)) |pi| {
        const pat = at(q.patterns, pi);
        for (0..cnt(pat.node_count)) |ni| {
            const v = pat.nodes[ni].variable;
            if (v == null or scopeHolds(node_vars[0..node_n], v)) continue;
            if (node_n >= MAX_VARS) return varCapacityError("node", MAX_VARS);
            node_vars[node_n] = v;
            node_n += 1;
        }
        for (0..cnt(pat.rel_count)) |ri| {
            const v = pat.rels[ri].variable;
            if (v == null or scopeHolds(edge_vars[0..edge_n], v)) continue;
            if (edge_n >= MAX_EDGE_VARS) return varCapacityError("edge", MAX_EDGE_VARS);
            edge_vars[edge_n] = v;
            edge_n += 1;
        }
    }
    return null;
}

/// Returns null when the query is fine, or a heap message naming the first
/// variable that is not in scope. Checks one query; the caller walks a UNION.
fn checkProjectionScope(q: *const Query) [*c]u8 {
    // Runs first, so the rest of this function can trust that the query names
    // no more variables than the arrays below can model.
    const capacity_err = checkPatternVarCapacity(q);
    if (capacity_err != null) return capacity_err;

    var declared_buf: [SCOPE_MAX_NAMES]CStr = undefined;
    // Unreachable while the capacity check above holds. Kept so the guard
    // still stands if either bound ever moves. Skipping the check was the old
    // behaviour, and it let an out-of-scope name through in silence.
    const declared_n = collectDeclaredNames(q, &declared_buf) orelse return null;
    const declared = declared_buf[0..declared_n];

    // A WITH still reads the pattern variables.
    if (q.with_clause != null and !q.with_clause.*.star) {
        const wc = q.with_clause;
        for (0..cnt(wc.*.count)) |i| {
            const v = scopeCheckableVar(at(wc.*.items, i));
            if (v != null and !scopeHolds(declared, v)) {
                return scopeError(v, "WITH", "no pattern in this query names it");
            }
        }
    }

    if (q.ret == null or q.ret.*.star) return null;

    // A RETURN after a WITH reads only what the WITH left behind.
    var after_with: [SCOPE_MAX_NAMES]CStr = undefined;
    var scope: []const CStr = declared;
    if (q.with_clause != null) {
        // Unreachable: a WITH holds at most SCOPE_MAX_NAMES items.
        const n = collectWithNames(q.with_clause, &after_with) orelse return null;
        scope = after_with[0..n];
    }

    for (0..cnt(q.ret.*.count)) |i| {
        const v = scopeCheckableVar(at(q.ret.*.items, i));
        if (v != null and !scopeHolds(scope, v)) {
            return scopeError(v, "RETURN", if (q.with_clause != null)
                "the WITH clause did not carry it through"
            else
                "no pattern in this query names it");
        }
    }
    return null;
}

const OOM_ERROR = "query aborted: out of memory or an allocation limit was reached";

pub export fn cbm_cypher_execute(store: ?*Store, query: [*c]const u8, project: [*c]const u8, max_rows_arg: c_int, out: [*c]c.cbm_cypher_result_t) c_int {
    out.* = std.mem.zeroes(c.cbm_cypher_result_t);
    depth_clamped = 0;
    trail_truncated = 0;
    deadlineArm(); // start the wall-clock budget for this query
    const max_rows: c_int = if (max_rows_arg <= 0) RESULT_CEILING else max_rows_arg;

    var q: [*c]Query = null;
    var err: [*c]u8 = null;
    if (parser.cbm_cypher_parse(query, &q, &err) < 0) {
        out.*.@"error" = err;
        return NOT_FOUND;
    }

    var sq: [*c]const Query = q;
    while (sq != null) : (sq = sq.*.union_next) {
        const scope_err = checkProjectionScope(sq);
        if (scope_err != null) {
            parser.cbm_query_free(q);
            out.*.@"error" = scope_err;
            return NOT_FOUND;
        }
    }

    var rb: ResultBuilder = .{};
    if (executeSingle(store, q, project, max_rows, &rb) < 0) {
        rbFree(&rb);
        parser.cbm_query_free(q);
        out.*.@"error" = heapStrdup(OOM_ERROR);
        return NOT_FOUND;
    }

    // UNION chain
    var uq = q.*.union_next;
    while (uq != null) : (uq = uq.*.union_next) {
        var rb2: ResultBuilder = .{};
        if (executeSingle(store, uq, project, max_rows, &rb2) < 0) {
            rbFree(&rb);
            rbFree(&rb2);
            parser.cbm_query_free(q);
            out.*.@"error" = heapStrdup(OOM_ERROR);
            return NOT_FOUND;
        }
        // Concatenate rows from rb2 into rb
        for (0..cnt(rb2.row_count)) |i| rbAddRow(&rb, rb2.rows[i]);
        rbFree(&rb2);
    }

    // UNION (not ALL) deduplication
    if (q.*.union_next != null and !q.*.union_all) rbApplyDistinct(&rb);

    // Abort a runaway query that blew the wall-clock budget before it can
    // return a misleading partial result. Checks the clock itself, not only
    // the flag the hot loops set, so time spent in a phase without its own
    // check still fails the query. Checked before the row ceiling.
    if (deadlineExceeded()) {
        rbFree(&rb);
        parser.cbm_query_free(q);
        out.*.@"error" = heapStrdup("query exceeded the execution time limit — narrow the pattern with a WHERE " ++
            "filter, use a directed MATCH instead of an unbounded OPTIONAL MATCH, or " ++
            "add LIMIT");
        return NOT_FOUND;
    }

    // Check ceiling
    if (rb.row_count >= RESULT_CEILING) {
        rbFree(&rb);
        parser.cbm_query_free(q);
        out.*.@"error" = heapStrdup("result exceeded 100k rows — use narrower filters or add LIMIT");
        return NOT_FOUND;
    }

    out.*.columns = rb.columns;
    out.*.col_count = rb.col_count;
    out.*.rows = rb.rows;
    out.*.row_count = rb.row_count;
    if (depth_clamped > 0 or trail_truncated != 0) {
        var wbuf: [256]u8 = undefined;
        if (depth_clamped > 0 and trail_truncated != 0) {
            _ = bprint(&wbuf, "variable-length hop range clamped to the engine ceiling ({d}) and " ++
                "traversal budget was exhausted — results may be partial", .{depth_clamped});
        } else if (depth_clamped > 0) {
            _ = bprint(&wbuf, "variable-length hop range clamped to the engine ceiling ({d}) — an empty " ++
                "result may mean \"clamped\", not \"no such path\"", .{depth_clamped});
        } else {
            _ = bprint(&wbuf, "variable-length traversal budget was exhausted — results may be partial", .{});
        }
        out.*.warning = heapStrdup(&wbuf);
    }

    parser.cbm_query_free(q);
    return 0;
}

pub export fn cbm_cypher_result_free(r: [*c]c.cbm_cypher_result_t) void {
    if (r == null) return;
    c.free(r.*.warning);
    r.*.warning = null;
    for (0..cnt(r.*.col_count)) |i| strFree(at(r.*.columns, i));
    c.free(@ptrCast(r.*.columns));
    for (0..cnt(r.*.row_count)) |i| {
        for (0..cnt(r.*.col_count)) |j| strFree(at(r.*.rows[i], j));
        c.free(@ptrCast(r.*.rows[i]));
    }
    c.free(@ptrCast(r.*.rows));
    c.free(r.*.@"error");
    r.* = std.mem.zeroes(c.cbm_cypher_result_t);
}
