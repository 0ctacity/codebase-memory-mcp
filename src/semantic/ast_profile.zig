//! ast_profile.zig — Extraction-time AST structural signals.
//!
//! Implements src/semantic/ast_profile.h. Walks a function body AST
//! (alongside MinHash) to compute:
//!   Signal 8:  Control flow counts, nesting depth, expression types
//!   Signal 9:  Approximate data flow (params in returns/conditions)
//!   Signal 11: Halstead-lite (unique/total operators/operands)
//!
//! Pure functions — thread-safe, no shared state.

const std = @import("std");
const c = @import("c");

const Profile = c.cbm_ast_profile_t;
const Str = [*:0]const u8;

// ── Node type classification ────────────────────────────────────────

const WALK_STACK_CAP = 2048;
const HALSTEAD_SET_SIZE = 512;
const HALSTEAD_SET_MASK = 511;
const PROFILE_FIELD_COUNT = 25;
const DEPTH_SCALE = 10;
const BASE_DECIMAL_AST = 10;
const HALSTEAD_HASH_MUL = 31;

const ProfileFrame = struct {
    node: c.TSNode,
    depth: c_int,
};

fn isOneOf(kind: []const u8, comptime names: []const []const u8) bool {
    inline for (names) |name| {
        if (std.mem.eql(u8, kind, name)) return true;
    }
    return false;
}

fn isControlIf(k: []const u8) bool {
    return isOneOf(k, &.{ "if_statement", "if_expression", "elif_clause" });
}

fn isControlFor(k: []const u8) bool {
    return isOneOf(k, &.{ "for_statement", "for_range_loop", "for_expression", "for_in_clause" });
}

fn isControlWhile(k: []const u8) bool {
    return isOneOf(k, &.{ "while_statement", "while_expression", "do_statement" });
}

fn isControlSwitch(k: []const u8) bool {
    return isOneOf(k, &.{
        "switch_statement", "switch_expression", "match_expression", "type_switch_statement",
    });
}

fn isControlTry(k: []const u8) bool {
    return isOneOf(k, &.{ "try_statement", "try_expression", "catch_clause", "except_clause" });
}

fn isReturn(k: []const u8) bool {
    return isOneOf(k, &.{ "return_statement", "return_expression" });
}

fn isComparison(k: []const u8) bool {
    return isOneOf(k, &.{ "binary_expression", "comparison_operator", "boolean_operator" });
}

fn isArithmetic(k: []const u8) bool {
    return isOneOf(k, &.{ "unary_expression", "update_expression" });
}

fn isAssignment(k: []const u8) bool {
    return isOneOf(k, &.{
        "assignment_expression", "assignment_statement", "augmented_assignment",
        "short_var_declaration",
    });
}

fn isStringLit(k: []const u8) bool {
    return isOneOf(k, &.{
        "string", "string_literal", "interpreted_string_literal", "raw_string_literal",
    });
}

fn isNumberLit(k: []const u8) bool {
    return isOneOf(k, &.{ "number", "integer", "float", "integer_literal", "float_literal" });
}

fn isBoolLit(k: []const u8) bool {
    return isOneOf(k, &.{ "true", "false" });
}

fn isOperatorNode(k: []const u8) bool {
    // Named nodes that represent operations (not data).
    return isControlIf(k) or isControlFor(k) or isControlWhile(k) or isControlSwitch(k) or
        isControlTry(k) or isReturn(k) or isComparison(k) or isArithmetic(k) or
        isAssignment(k) or
        isOneOf(k, &.{ "call_expression", "member_expression", "subscript_expression" });
}

fn isIdentifier(k: []const u8) bool {
    return isOneOf(k, &.{
        "identifier", "field_identifier", "property_identifier", "type_identifier",
    });
}

/// Simple hash set for Halstead unique counting (open addressing).
fn halsteadInsert(set: *[HALSTEAD_SET_SIZE]u32, key: []const u8) bool {
    var h: u32 = 0;
    for (key) |ch| {
        h = (h *% HALSTEAD_HASH_MUL) +% ch;
    }
    const idx = h & HALSTEAD_SET_MASK;
    for (0..HALSTEAD_SET_SIZE) |probe| {
        const slot = (idx +% @as(u32, @intCast(probe))) & HALSTEAD_SET_MASK;
        if (set[slot] == 0) {
            set[slot] = h | 1;
            return true; // new
        }
        if (set[slot] == (h | 1)) {
            return false; // existing
        }
    }
    return false;
}

/// Check if an identifier matches any parameter name.
fn isParamName(ident: Str, param_names: ?[*]?Str, param_count: c_int) bool {
    const names = param_names orelse return false;
    if (param_count <= 0) return false;
    for (names[0..@intCast(param_count)]) |name| {
        if (name != null and c.strcmp(ident, name) == 0) return true;
    }
    return false;
}

// ── Main computation ────────────────────────────────────────────────

/// Count control-flow statement kinds (if/for/while/switch/try/return).
fn accumulateControlFlow(kind: []const u8, out: *Profile, in_return: *bool) void {
    if (isControlIf(kind)) out.if_count +%= 1;
    if (isControlFor(kind)) out.for_count +%= 1;
    if (isControlWhile(kind)) out.while_count +%= 1;
    if (isControlSwitch(kind)) out.switch_count +%= 1;
    if (isControlTry(kind)) out.try_count +%= 1;
    if (isReturn(kind)) {
        out.return_count +%= 1;
        in_return.* = true;
    }
}

/// Count expression- and literal-class counters for one node.
fn accumulateExpressions(kind: []const u8, out: *Profile) void {
    if (isComparison(kind)) out.comparison_ops +%= 1;
    if (isArithmetic(kind)) out.arithmetic_ops +%= 1;
    if (isOneOf(kind, &.{ "not_operator", "boolean_operator" })) out.logical_ops +%= 1;
    if (isAssignment(kind)) {
        out.assignment_count +%= 1;
        out.variable_reassigns +%= 1;
    }
    if (isStringLit(kind)) out.string_literals +%= 1;
    if (isNumberLit(kind)) out.number_literals +%= 1;
    if (isBoolLit(kind)) out.bool_literals +%= 1;
}

/// Accumulate Halstead operator/operand counts. Leaf identifiers, literals,
/// and booleans are "operands"; recognized statement/expression kinds are
/// "operators". Uses an open-addressed hash set for unique counting.
fn accumulateHalstead(
    kind: []const u8,
    child_count: u32,
    op_set: *[HALSTEAD_SET_SIZE]u32,
    operand_set: *[HALSTEAD_SET_SIZE]u32,
    out: *Profile,
) void {
    if (isOperatorNode(kind)) {
        out.total_operators +%= 1;
        if (halsteadInsert(op_set, kind)) out.unique_operators +%= 1;
    }
    if (child_count == 0 and
        (isIdentifier(kind) or isStringLit(kind) or isNumberLit(kind) or isBoolLit(kind)))
    {
        out.total_operands +%= 1;
        if (halsteadInsert(operand_set, kind)) out.unique_operands +%= 1;
        out.body_tokens +%= 1;
    }
}

/// Data-flow counter update: bump params_in_returns / params_in_conditions
/// when the current leaf identifier names a parameter and we're inside the
/// corresponding syntactic scope.
fn accumulateDataFlow(
    node: c.TSNode,
    kind: []const u8,
    child_count: u32,
    source_c: ?[*]const u8,
    param_names: ?[*]?Str,
    param_count: c_int,
    in_return: bool,
    in_condition: bool,
    out: *Profile,
) void {
    const source = source_c orelse return;
    if (!(child_count == 0 and isIdentifier(kind))) return;
    const start = c.ts_node_start_byte(node);
    const end = c.ts_node_end_byte(node);
    if (end <= start or (end - start) >= c.CBM_SZ_128) return;
    var ident_buf: [c.CBM_SZ_128]u8 = undefined;
    const ilen: usize = end - start;
    @memcpy(ident_buf[0..ilen], source[start..end]);
    ident_buf[ilen] = 0;
    if (!isParamName(@ptrCast(&ident_buf), param_names, param_count)) return;
    if (in_return) out.params_in_returns +%= 1;
    if (in_condition) out.params_in_conditions +%= 1;
}

pub export fn cbm_ast_profile_compute(
    func_body: c.TSNode,
    source: ?[*]const u8,
    param_names: ?[*]?Str,
    param_count: c_int,
    out: *Profile,
) bool {
    if (c.ts_node_is_null(func_body)) return false;

    out.* = std.mem.zeroes(Profile);
    out.param_count = @truncate(@as(c_uint, @bitCast(param_count)));

    var op_set: [HALSTEAD_SET_SIZE]u32 = @splat(0);
    var operand_set: [HALSTEAD_SET_SIZE]u32 = @splat(0);

    var total_depth: c_int = 0;
    var node_count: c_int = 0;
    var in_return = false;
    var in_condition = false;

    var stack: [WALK_STACK_CAP]ProfileFrame = undefined;
    var top: usize = 0;
    stack[top] = .{ .node = func_body, .depth = 0 };
    top += 1;

    while (top > 0) {
        top -= 1;
        const frame = stack[top];
        const node = frame.node;
        const depth = frame.depth;
        const child_count = c.ts_node_child_count(node);
        const kind = std.mem.span(@as(Str, @ptrCast(c.ts_node_type(node))));

        // Anonymous leaves (punctuation, keywords) are skipped.
        if (c.ts_node_is_named(node) or child_count != 0) {
            node_count += 1;
            total_depth +%= depth;

            const depth16: u16 = @truncate(@as(c_uint, @bitCast(depth)));
            if (depth16 > out.max_nesting_depth) out.max_nesting_depth = depth16;

            accumulateControlFlow(kind, out, &in_return);
            accumulateExpressions(kind, out);
            accumulateHalstead(kind, child_count, &op_set, &operand_set, out);
            accumulateDataFlow(node, kind, child_count, source, param_names, param_count, in_return, in_condition, out);

            // Track context for data flow: are we inside a condition?
            if (isControlIf(kind) or isControlWhile(kind)) in_condition = true;
        }

        // Reset context flags when leaving return/condition scope.
        if (isReturn(kind)) in_return = false;
        if (child_count > 0 and (isControlIf(kind) or isControlWhile(kind))) in_condition = false;

        // Push children in reverse order.
        var i = child_count;
        while (i > 0 and top < WALK_STACK_CAP) {
            i -= 1;
            stack[top] = .{ .node = c.ts_node_child(node, i), .depth = depth + 1 };
            top += 1;
        }
    }

    // Compute averages.
    if (node_count > 0) {
        const avg = @divTrunc(total_depth *% DEPTH_SCALE, node_count);
        out.avg_nesting_depth_x10 = @truncate(@as(c_uint, @bitCast(avg)));
    }

    return node_count > 0;
}

// ── Serialization ───────────────────────────────────────────────────

/// Serialization order of the profile fields. Must match the argument order
/// in cbm_ast_profile_to_str().
const field_order = [PROFILE_FIELD_COUNT][]const u8{
    "if_count",          "for_count",            "while_count",        "switch_count",
    "try_count",         "return_count",         "max_nesting_depth",  "avg_nesting_depth_x10",
    "comparison_ops",    "arithmetic_ops",       "logical_ops",        "assignment_count",
    "string_literals",   "number_literals",      "bool_literals",      "param_count",
    "params_in_returns", "params_in_conditions", "variable_reassigns", "unique_operators",
    "unique_operands",   "total_operators",      "total_operands",     "body_lines",
    "body_tokens",
};

pub export fn cbm_ast_profile_to_str(p_c: ?*const Profile, buf_c: ?[*]u8, bufsize: c_int) void {
    const buf = buf_c orelse return;
    const p = p_c orelse {
        if (bufsize > 0) buf[0] = 0;
        return;
    };
    if (bufsize < 1) return;
    const u = struct {
        fn u(v: u16) c_uint {
            return v;
        }
    }.u;
    _ = c.snprintf(
        buf,
        @intCast(bufsize),
        "%u,%u,%u,%u,%u,%u,%u,%u," ++
            "%u,%u,%u,%u," ++
            "%u,%u,%u," ++
            "%u,%u,%u,%u," ++
            "%u,%u,%u,%u," ++
            "%u,%u",
        u(p.if_count),
        u(p.for_count),
        u(p.while_count),
        u(p.switch_count),
        u(p.try_count),
        u(p.return_count),
        u(p.max_nesting_depth),
        u(p.avg_nesting_depth_x10),
        u(p.comparison_ops),
        u(p.arithmetic_ops),
        u(p.logical_ops),
        u(p.assignment_count),
        u(p.string_literals),
        u(p.number_literals),
        u(p.bool_literals),
        u(p.param_count),
        u(p.params_in_returns),
        u(p.params_in_conditions),
        u(p.variable_reassigns),
        u(p.unique_operators),
        u(p.unique_operands),
        u(p.total_operators),
        u(p.total_operands),
        u(p.body_lines),
        u(p.body_tokens),
    );
}

pub export fn cbm_ast_profile_from_str(str: ?Str, out_c: ?*Profile) bool {
    const out = out_c orelse return false;
    var p: Str = str orelse return false;
    out.* = std.mem.zeroes(Profile);
    // Field layout must match cbm_ast_profile_to_str() exactly.
    inline for (field_order, 0..) |name, i| {
        var end: [*c]u8 = null;
        const parsed = c.strtoul(p, &end, BASE_DECIMAL_AST);
        if (@intFromPtr(end) == @intFromPtr(p)) return false;
        @field(out, name) = @truncate(parsed);
        p = @ptrCast(end);
        // Skip the comma separator between fields.
        if (i + 1 < PROFILE_FIELD_COUNT) {
            if (p[0] != ',') return false;
            p += 1;
        }
    }
    return true;
}

pub export fn cbm_ast_profile_to_vector(p_c: ?*const Profile, out_c: ?[*]f32) void {
    const p = p_c orelse return;
    const out = out_c orelse return;
    // Normalize each field to [0,1] range using reasonable maximums.
    const MAX_COUNT = 100;
    const MAX_DEPTH = 20;
    const MAX_HALSTEAD = 200;
    const MAX_TOKENS = 2000;
    const maxima = [PROFILE_FIELD_COUNT]f32{
        MAX_COUNT,    MAX_COUNT,    MAX_COUNT,    MAX_COUNT,
        MAX_COUNT,    MAX_COUNT,    MAX_DEPTH,    MAX_DEPTH * DEPTH_SCALE,
        MAX_COUNT,    MAX_COUNT,    MAX_COUNT,    MAX_COUNT,
        MAX_COUNT,    MAX_COUNT,    MAX_COUNT,    MAX_DEPTH,
        MAX_COUNT,    MAX_COUNT,    MAX_COUNT,    MAX_HALSTEAD,
        MAX_HALSTEAD, MAX_HALSTEAD, MAX_HALSTEAD, MAX_TOKENS,
        MAX_TOKENS,
    };
    inline for (field_order, maxima, 0..) |name, max, i| {
        out[i] = @as(f32, @floatFromInt(@field(p, name))) / max;
    }
}
