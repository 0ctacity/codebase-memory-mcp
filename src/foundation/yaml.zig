//! yaml.zig — Minimal YAML parser for config files.
//!
//! Implements src/foundation/yaml.h.
//!
//! Strategy: line-by-line parsing with indentation tracking.
//! Each line is either:
//!   - Empty or comment (#) → skip
//!   - "key: value" → scalar entry in current map
//!   - "key:" (no value) → start of nested map or list
//!   - "- value" → list item
//!
//! Indentation determines nesting depth. Uses a stack of parent nodes.

const std = @import("std");
const c = @import("c");

const YAML_INIT_CAP = 8;
const YAML_LIST_PREFIX = 2;
const YAML_ROOT_INDENT = -1;
const YAML_MAX_DEPTH = 32;

// ── Node types ───────────────────────────────────────────────────

const YamlType = enum(c_int) { map, list, scalar };

const Node = extern struct {
    type: YamlType,
    key: ?[*:0]u8, // NULL for list items and root
    value: ?[*:0]u8, // scalar value (NULL for map/list)

    // Children (for map and list nodes)
    children: ?[*]*Node,
    child_count: c_int,
    child_cap: c_int,

    fn childSlice(n: *const Node) []*Node {
        const children = n.children orelse return &.{};
        return children[0..@intCast(n.child_count)];
    }
};

// ── Node lifecycle ───────────────────────────────────────────────

fn nodeNew(node_type: YamlType) ?*Node {
    const n: *Node = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Node)) orelse return null));
    n.type = node_type;
    return n;
}

fn nodeAddChild(parent: *Node, child: *Node) bool {
    if (parent.child_count >= parent.child_cap) {
        const new_cap: c_int = if (parent.child_cap < YAML_INIT_CAP) YAML_INIT_CAP else parent.child_cap * 2;
        const new_arr: [*]*Node = @ptrCast(@alignCast(c.realloc(@ptrCast(parent.children), @as(usize, @intCast(new_cap)) * @sizeOf(*Node)) orelse return false));
        parent.children = new_arr;
        parent.child_cap = new_cap;
    }
    parent.children.?[@intCast(parent.child_count)] = child;
    parent.child_count += 1;
    return true;
}

pub export fn cbm_yaml_free(root: ?*Node) void {
    const YAML_FREE_STACK = 256;
    var stack: [YAML_FREE_STACK]*Node = undefined;
    var top: usize = 0;
    stack[top] = root orelse return;
    top += 1;

    while (top > 0) {
        top -= 1;
        const node = stack[top];
        for (node.childSlice()) |child| {
            if (top >= YAML_FREE_STACK) break;
            stack[top] = child;
            top += 1;
        }
        c.free(@ptrCast(node.children));
        c.free(node.key);
        c.free(node.value);
        c.free(node);
    }
}

// ── String helpers ───────────────────────────────────────────────

/// Duplicate a substring. Trims leading and trailing whitespace.
fn trimDup(text: []const u8) ?[*:0]u8 {
    const trimmed = std.mem.trim(u8, text, &std.ascii.whitespace);
    const s: [*]u8 = @ptrCast(c.malloc(trimmed.len + 1) orelse return null);
    @memcpy(s[0..trimmed.len], trimmed);
    s[trimmed.len] = 0;
    return @ptrCast(s);
}

/// Count leading spaces (not tabs).
fn leadingSpaces(line: []const u8) usize {
    var n: usize = 0;
    while (n < line.len and line[n] == ' ') n += 1;
    return n;
}

/// One line of `text` starting at `start`: its content after the indentation
/// (without a trailing '\r'), the indentation, and where the next line starts.
const Line = struct {
    content: []const u8,
    indent: usize,
    next: usize,

    fn at(text: []const u8, start: usize) Line {
        const eol = std.mem.indexOfScalarPos(u8, text, start, '\n') orelse text.len;
        const line = text[start..eol];
        const indent = leadingSpaces(line);
        var content = line[indent..];
        // Strip \r
        if (content.len > 0 and content[content.len - 1] == '\r') content.len -= 1;
        return .{ .content = content, .indent = indent, .next = if (eol < text.len) eol + 1 else text.len };
    }

    /// Empty lines and comments carry nothing.
    fn isBlank(line: Line) bool {
        return line.content.len == 0 or line.content[0] == '#';
    }
};

// ── Parser ───────────────────────────────────────────────────────

/// Stack entry for tracking parent context during parsing.
const StackEntry = struct {
    node: *Node,
    indent: c_int,
};

/// Parse a list item "- value" and add to the correct parent.
fn parseListItem(content: []const u8, parent: *Node) void {
    const item_text = std.mem.trimStart(u8, content[YAML_LIST_PREFIX..], &std.ascii.whitespace);

    // If parent is a map, the list items belong to the last child
    var list_parent = parent;
    if (parent.type == .map and parent.child_count > 0) {
        const last = parent.childSlice()[@intCast(parent.child_count - 1)];
        if (last.type == .list) list_parent = last;
    }

    const item = nodeNew(.scalar) orelse return;
    item.value = trimDup(item_text);
    if (!nodeAddChild(list_parent, item)) cbm_yaml_free(item);
}

/// Strip inline comments from a value string (not inside quotes).
fn stripInlineComment(after: []const u8) []const u8 {
    if (after.len > 0 and after[0] != '"' and after[0] != '\'') {
        for (after, 0..) |ch, i| {
            if (ch == '#' and i > 0 and after[i - 1] == ' ') {
                return std.mem.trimEnd(u8, after[0..i], &std.ascii.whitespace);
            }
        }
    }
    return after;
}

/// Peek ahead to check if next content line is a list item.
fn peekIsList(text: []const u8, next_line: usize) bool {
    var pos = next_line;
    while (pos < text.len) {
        const line = Line.at(text, pos);
        if (line.isBlank()) {
            pos = line.next;
            continue;
        }
        return line.content[0] == '-';
    }
    return false;
}

/// Parse a "key: value" or "key:" line.
fn parseKeyLine(text: []const u8, line: Line, parent: *Node, stack: *[YAML_MAX_DEPTH]StackEntry, stack_depth: *usize) void {
    const content = line.content;
    const colon = std.mem.indexOfScalar(u8, content, ':') orelse return;

    const key = trimDup(content[0..colon]) orelse return;

    // After colon: value or nothing
    const after = stripInlineComment(std.mem.trimStart(u8, content[colon + 1 ..], &std.ascii.whitespace));

    if (after.len > 0) {
        // "key: value" — scalar
        const child = nodeNew(.scalar) orelse {
            c.free(key);
            return;
        };
        child.key = key;
        child.value = trimDup(after);
        if (!nodeAddChild(parent, child)) cbm_yaml_free(child);
    } else {
        // "key:" — could be map or list (determined by next lines)
        const is_list = peekIsList(text, line.next);
        const child = nodeNew(if (is_list) .list else .map) orelse {
            c.free(key);
            return;
        };
        child.key = key;
        if (!nodeAddChild(parent, child)) {
            cbm_yaml_free(child);
        } else if (stack_depth.* < YAML_MAX_DEPTH) {
            stack[stack_depth.*] = .{ .node = child, .indent = @intCast(line.indent) };
            stack_depth.* += 1;
        }
    }
}

pub export fn cbm_yaml_parse(text_ptr: ?[*]const u8, len: c_int) ?*Node {
    const root = nodeNew(.map) orelse return null;
    const ptr = text_ptr orelse return root;
    if (len <= 0) return root;
    const text = ptr[0..@intCast(len)];

    // Stack for tracking parent context
    var stack: [YAML_MAX_DEPTH]StackEntry = undefined;
    stack[0] = .{ .node = root, .indent = YAML_ROOT_INDENT };
    var stack_depth: usize = 1;

    var pos: usize = 0;
    while (pos < text.len) {
        const line = Line.at(text, pos);
        pos = line.next;

        // Skip empty lines and comments
        if (line.isBlank()) continue;

        // Pop stack to find parent at correct indentation
        while (stack_depth > 1 and stack[stack_depth - 1].indent >= line.indent) stack_depth -= 1;
        const parent = stack[stack_depth - 1].node;

        const content = line.content;
        if (content[0] == '-' and content.len >= 2 and content[1] == ' ') {
            parseListItem(content, parent);
        } else {
            parseKeyLine(text, line, parent, &stack, &stack_depth);
        }
    }

    return root;
}

// ── Query helpers ────────────────────────────────────────────────

/// Find a child node by key in a map node.
fn findChild(node: *const Node, key: []const u8) ?*const Node {
    for (node.childSlice()) |child| {
        if (child.key) |child_key| {
            if (std.mem.eql(u8, std.mem.span(child_key), key)) return child;
        }
    }
    return null;
}

/// Navigate to a node by dot-separated path.
fn navigate(root: ?*const Node, path: ?[*:0]const u8) ?*const Node {
    var cur = root orelse return null;
    var p = std.mem.span(path orelse return null);

    while (p.len > 0) {
        const seg_len = std.mem.indexOfScalar(u8, p, '.') orelse p.len;
        if (seg_len == 0 or seg_len >= 256) return null;
        cur = findChild(cur, p[0..seg_len]) orelse return null;
        p = p[seg_len..];
        if (p.len > 0) p = p[1..]; // the '.'
    }
    return cur;
}

pub export fn cbm_yaml_get_str(root: ?*const Node, path: ?[*:0]const u8) ?[*:0]const u8 {
    const node = navigate(root, path) orelse return null;
    if (node.type != .scalar) return null;
    return node.value;
}

pub export fn cbm_yaml_get_float(root: ?*const Node, path: ?[*:0]const u8, default_val: f64) f64 {
    const str = cbm_yaml_get_str(root, path) orelse return default_val;
    var endptr: [*c]u8 = null;
    const val = c.strtod(str, &endptr);
    if (endptr == @as([*c]const u8, str)) return default_val;
    return val;
}

pub export fn cbm_yaml_get_bool(root: ?*const Node, path: ?[*:0]const u8, default_val: bool) bool {
    const str = std.mem.span(cbm_yaml_get_str(root, path) orelse return default_val);
    const eq = std.ascii.eqlIgnoreCase;
    if (eq(str, "true") or eq(str, "yes") or eq(str, "on") or std.mem.eql(u8, str, "1")) return true;
    if (eq(str, "false") or eq(str, "no") or eq(str, "off") or std.mem.eql(u8, str, "0")) return false;
    return default_val;
}

pub export fn cbm_yaml_get_str_list(root: ?*const Node, path: ?[*:0]const u8, out: [*]?[*:0]const u8, max_out: c_int) c_int {
    const node = navigate(root, path) orelse return 0;
    if (node.type != .list) return 0;
    var count: c_int = 0;
    for (node.childSlice()) |child| {
        if (count >= max_out) break;
        if (child.type == .scalar and child.value != null) {
            out[@intCast(count)] = child.value;
            count += 1;
        }
    }
    return count;
}

pub export fn cbm_yaml_has(root: ?*const Node, path: ?[*:0]const u8) bool {
    return navigate(root, path) != null;
}
