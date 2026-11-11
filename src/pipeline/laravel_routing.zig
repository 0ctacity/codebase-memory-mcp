//! laravel_routing.zig — Laravel 11+ framework route mounts (#1146).
//!
//! See laravel_routing.h for the contract. bootstrap/app.php is parsed with
//! the same PHP grammar the extractor uses; only named `withRouting(...)`
//! arguments whose values are plain literals (or `__DIR__`/`dirname(__DIR__)`
//! concatenations and `base_path()` calls over literals) are trusted.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = [*c]const u8;
const MutStr = [*c]u8;
const TSNode = c.TSNode;

const LR_PATH_MAX = c.CBM_SZ_512; // repo-relative path buffer
const LR_KEY_MAX = c.CBM_SZ_32; // named-argument key
const LR_PREFIX_MAX = c.CBM_SZ_128; // apiPrefix literal
const LR_SEG_MAX = c.CBM_SZ_64; // path segments tracked while normalising
const LR_SOURCE_MAX = c.CBM_SZ_256 * c.CBM_SZ_1K; // bootstrap/app.php read cap

const LR_BOOTSTRAP_REL = "bootstrap/app.php";

/// ts_node_child_by_field_name with a literal field name (TS_FIELD in C).
fn field(node: TSNode, comptime name: [:0]const u8) TSNode {
    return c.ts_node_child_by_field_name(node, name, name.len);
}

/// Copy a node's source text into buf. False when it does not fit.
fn lrNodeText(src: [*]const u8, node: TSNode, buf: []u8) bool {
    if (c.ts_node_is_null(node)) return false;
    const start = c.ts_node_start_byte(node);
    const end = c.ts_node_end_byte(node);
    const len: usize = if (end > start) end - start else 0;
    if (len >= buf.len) return false;
    @memcpy(buf[0..len], src[start..][0..len]);
    buf[len] = 0;
    return true;
}

fn lrNodeIs(node: TSNode, node_type: [*:0]const u8) bool {
    return !c.ts_node_is_null(node) and c.strcmp(c.ts_node_type(node), node_type) == 0;
}

fn lrNodeTextIs(src: [*]const u8, node: TSNode, text: [*:0]const u8) bool {
    var buf: [LR_KEY_MAX]u8 = undefined;
    return lrNodeText(src, node, &buf) and c.strcmp(&buf, text) == 0;
}

/// A plain string literal: '...' or "..." whose only children are
/// string_content (no interpolation, no escapes). Writes the unquoted value.
fn lrStringLiteral(src: [*]const u8, node: TSNode, buf: []u8) bool {
    if (!lrNodeIs(node, "string") and !lrNodeIs(node, "encapsed_string")) return false;
    var pos: usize = 0;
    buf[0] = 0;
    const nc = c.ts_node_named_child_count(node);
    var i: u32 = 0;
    while (i < nc) : (i += 1) {
        const part = c.ts_node_named_child(node, i);
        if (!lrNodeIs(part, "string_content") or !lrNodeText(src, part, buf[pos..])) {
            return false;
        }
        pos += c.strlen(buf[pos..].ptr);
    }
    return true;
}

/// The value expression of a call argument: its first named child that is
/// neither the `name:` label nor a comment.
fn lrArgValue(arg: TSNode, label: TSNode) TSNode {
    const nc = c.ts_node_named_child_count(arg);
    var i: u32 = 0;
    while (i < nc) : (i += 1) {
        const child = c.ts_node_named_child(arg, i);
        if ((c.ts_node_is_null(label) or !c.ts_node_eq(child, label)) and
            !lrNodeIs(child, "comment"))
        {
            return child;
        }
    }
    return std.mem.zeroes(TSNode);
}

/// The single literal argument of `fn('...')` when the callee is `fn`.
fn lrCallLiteralArg(src: [*]const u8, call: TSNode, func: [*:0]const u8, buf: []u8) bool {
    if (!lrNodeIs(call, "function_call_expression") or
        !lrNodeTextIs(src, field(call, "function"), func))
    {
        return false;
    }
    const args = field(call, "arguments");
    if (c.ts_node_is_null(args) or c.ts_node_named_child_count(args) != 1) return false;
    const arg = c.ts_node_named_child(args, 0);
    const val = lrArgValue(arg, field(arg, "name"));
    return lrStringLiteral(src, val, buf);
}

/// `dirname(__DIR__)` — the application base directory.
fn lrIsDirnameDir(src: [*]const u8, node: TSNode) bool {
    if (!lrNodeIs(node, "function_call_expression") or
        !lrNodeTextIs(src, field(node, "function"), "dirname"))
    {
        return false;
    }
    const args = field(node, "arguments");
    if (c.ts_node_is_null(args) or c.ts_node_named_child_count(args) != 1) return false;
    const arg = c.ts_node_named_child(args, 0);
    const val = lrArgValue(arg, field(arg, "name"));
    return lrNodeIs(val, "name") and lrNodeTextIs(src, val, "__DIR__");
}

/// Resolve "." and ".." segments of a '/'-joined path in place. False when the
/// path climbs above the repository root.
fn lrNormalize(path: [*:0]u8) bool {
    var seg_start: [LR_SEG_MAX]usize = undefined;
    var depth: usize = 0;
    var out: usize = 0;
    var p: [*:0]const u8 = path;
    while (p[0] != 0) {
        const slash: CStr = c.strchr(p, '/');
        const len: usize = if (slash != null) @intFromPtr(slash) - @intFromPtr(p) else c.strlen(p);
        if (len == 2 and p[0] == '.' and p[1] == '.') {
            if (depth == 0) return false;
            depth -= 1;
            out = seg_start[depth];
        } else if (len > 0 and !(len == 1 and p[0] == '.')) {
            if (depth == LR_SEG_MAX) return false;
            const at: usize = if (out > 0) out + 1 else 0;
            seg_start[depth] = out;
            depth += 1;
            if (out > 0) path[out] = '/';
            _ = c.memmove(path + at, p, len);
            out = at + len;
        }
        p += len + @as(usize, if (slash != null) 1 else 0);
    }
    path[out] = 0;
    return true;
}

/// Resolve one `api:` path expression of the app rooted at app_dir to a
/// normalised repo-relative path:
///   __DIR__ . '/../routes/api.php'           (relative to bootstrap/)
///   dirname(__DIR__) . '/routes/api.php'     (relative to the app base)
///   base_path('routes/api.php')              (relative to the app base)
fn lrRouteFilePath(src: [*]const u8, expr: TSNode, app_dir: CStr, out: []u8) bool {
    var lit: [LR_PATH_MAX]u8 = undefined;
    var base_suffix: CStr = null;
    if (lrNodeIs(expr, "binary_expression")) {
        const op = field(expr, "operator");
        const left = field(expr, "left");
        if (!lrNodeTextIs(src, op, ".") or !lrStringLiteral(src, field(expr, "right"), &lit)) {
            return false;
        }
        if (lrNodeIs(left, "name") and lrNodeTextIs(src, left, "__DIR__")) {
            base_suffix = "bootstrap";
        } else if (lrIsDirnameDir(src, left)) {
            base_suffix = "";
        } else {
            return false;
        }
    } else if (lrCallLiteralArg(src, expr, "base_path", &lit)) {
        base_suffix = "";
    } else {
        return false;
    }
    const n = c.snprintf(out.ptr, out.len, "%s/%s/%s", app_dir, base_suffix, @as(CStr, &lit));
    return n > 0 and @as(usize, @intCast(n)) < out.len and lrNormalize(@ptrCast(out.ptr));
}

/// Does the `api:` value (one path or an array of paths) name rel_path?
fn lrApiNamesFile(src: [*]const u8, value: TSNode, app_dir: CStr, rel_path: CStr) bool {
    var resolved: [LR_PATH_MAX]u8 = undefined;
    if (!lrNodeIs(value, "array_creation_expression")) {
        return lrRouteFilePath(src, value, app_dir, &resolved) and
            c.strcmp(&resolved, rel_path) == 0;
    }
    const nc = c.ts_node_named_child_count(value);
    var i: u32 = 0;
    while (i < nc) : (i += 1) {
        const elem = c.ts_node_named_child(value, i);
        const ec = c.ts_node_named_child_count(elem);
        if (!lrNodeIs(elem, "array_element_initializer") or ec == 0) continue;
        const path_expr = c.ts_node_named_child(elem, ec - 1); // value of `k => v` too
        if (lrRouteFilePath(src, path_expr, app_dir, &resolved) and
            c.strcmp(&resolved, rel_path) == 0)
        {
            return true;
        }
    }
    return false;
}

/// Evaluate the named arguments of a withRouting(...) call.
fn lrEvalWithRouting(src: [*]const u8, call: TSNode, app_dir: CStr, rel_path: CStr, out: []u8) bool {
    const args = field(call, "arguments");
    // ApplicationBuilder::withRouting default
    var prefix: [LR_PREFIX_MAX]u8 = @splat(0);
    prefix[0..3].* = "api".*;
    var prefix_known = true;
    var mounted = false;
    const nc: u32 = if (c.ts_node_is_null(args)) 0 else c.ts_node_named_child_count(args);
    var i: u32 = 0;
    while (i < nc) : (i += 1) {
        const arg = c.ts_node_named_child(args, i);
        if (lrNodeIs(arg, "comment")) continue;
        const label = field(arg, "name");
        var key: [LR_KEY_MAX]u8 = undefined;
        // Positional arguments would need the parameter order modelled, and a
        // `using:` callback replaces the web/api registration entirely.
        if (!lrNodeIs(arg, "argument") or c.ts_node_is_null(label) or
            !lrNodeText(src, label, &key) or c.strcmp(&key, "using") == 0)
        {
            return false;
        }
        const value = lrArgValue(arg, label);
        if (c.strcmp(&key, "api") == 0) {
            mounted = lrApiNamesFile(src, value, app_dir, rel_path);
        } else if (c.strcmp(&key, "apiPrefix") == 0) {
            prefix_known = lrStringLiteral(src, value, &prefix);
        }
    }
    if (!mounted or !prefix_known) return false;
    // Laravel trims the group prefix of '/' on both ends.
    var start: [*]const u8 = &prefix;
    var len = c.strlen(&prefix);
    while (start[0] == '/') {
        start += 1;
        len -= 1;
    }
    while (len > 0 and start[len - 1] == '/') len -= 1;
    const n = c.snprintf(out.ptr, out.len, "/%.*s", @as(c_int, @intCast(len)), start);
    return len > 0 and n > 0 and @as(usize, @intCast(n)) < out.len;
}

/// First `->withRouting(...)` call in the tree (iterative pre-order walk).
fn lrFindWithRouting(src: [*]const u8, root: TSNode) TSNode {
    var found = std.mem.zeroes(TSNode);
    var cur = c.ts_tree_cursor_new(root);
    var more = true;
    while (more) {
        const node = c.ts_tree_cursor_current_node(&cur);
        if ((lrNodeIs(node, "member_call_expression") or
            lrNodeIs(node, "nullsafe_member_call_expression")) and
            lrNodeTextIs(src, field(node, "name"), "withRouting"))
        {
            found = node;
            break;
        }
        if (c.ts_tree_cursor_goto_first_child(&cur)) continue;
        while (!c.ts_tree_cursor_goto_next_sibling(&cur)) {
            if (!c.ts_tree_cursor_goto_parent(&cur)) {
                more = false;
                break;
            }
        }
    }
    c.ts_tree_cursor_delete(&cur);
    return found;
}

fn lrMountFromBootstrap(src: [*]const u8, len: u32, app_dir: CStr, rel_path: CStr, out: []u8) bool {
    const php = c.cbm_ts_language(c.CBM_LANG_PHP);
    const parser = if (php != null) c.ts_parser_new() else null;
    if (parser == null) return false;
    var mounted = false;
    const tree = if (c.ts_parser_set_language(parser, php))
        c.ts_parser_parse_string(parser, null, src, len)
    else
        null;
    if (tree != null) {
        const call = lrFindWithRouting(src, c.ts_tree_root_node(tree));
        mounted = !c.ts_node_is_null(call) and
            lrEvalWithRouting(src, call, app_dir, rel_path, out);
        c.ts_tree_delete(tree);
    }
    c.ts_parser_delete(parser);
    return mounted;
}

/// Read <repo>/<app_dir>/bootstrap/app.php. NULL when absent or unreadable.
fn lrReadBootstrap(repo_path: CStr, app_dir: [*:0]const u8, out_len: *u32) MutStr {
    var path: [c.CBM_SZ_4K]u8 = undefined;
    const n = c.snprintf(
        &path,
        path.len,
        "%s/%s%s%s",
        repo_path,
        app_dir,
        @as(CStr, if (app_dir[0] != 0) "/" else ""),
        @as(CStr, LR_BOOTSTRAP_REL),
    );
    if (n <= 0 or @as(usize, @intCast(n)) >= path.len) return null;
    const f = c.cbm_fopen(&path, "rb") orelse return null;
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    const buf: MutStr = if (size > 0 and size <= LR_SOURCE_MAX)
        @ptrCast(c.malloc(@as(usize, @intCast(size)) + 1))
    else
        null;
    if (buf != null) {
        const nread = c.fread(buf, 1, @intCast(size), f);
        buf[nread] = 0;
        out_len.* = @intCast(nread);
    }
    _ = c.fclose(f);
    return buf;
}

pub export fn cbm_laravel_api_mount(repo_path: CStr, rel_path: CStr, out_c: MutStr, out_sz: usize) bool {
    if (out_c == null or out_sz == 0) return false;
    const out = out_c[0..out_sz];
    out[0] = 0;
    const rel_len: usize = if (rel_path != null) c.strlen(rel_path) else 0;
    if (repo_path == null or rel_len < ".php".len + 1 or rel_len >= LR_PATH_MAX or
        c.strcmp(rel_path + rel_len - ".php".len, ".php") != 0)
    {
        return false;
    }
    // Walk the file's ancestor directories nearest-first; the nearest one
    // holding bootstrap/app.php is the Laravel app that owns the file.
    var app_dir: [LR_PATH_MAX:0]u8 = undefined;
    @memcpy(app_dir[0 .. rel_len + 1], rel_path[0 .. rel_len + 1]);
    var slash: MutStr = c.strrchr(&app_dir, '/');
    while (true) {
        if (slash != null) {
            slash[0] = 0;
        } else {
            app_dir[0] = 0;
        }
        var len: u32 = 0;
        const src = lrReadBootstrap(repo_path, &app_dir, &len);
        if (src != null) {
            const mounted = lrMountFromBootstrap(src, len, &app_dir, rel_path, out);
            c.free(src);
            if (!mounted) out[0] = 0;
            return mounted;
        }
        if (app_dir[0] == 0) return false;
        slash = c.strrchr(&app_dir, '/');
    }
}

pub export fn cbm_laravel_file_route_mount(
    repo_path: CStr,
    rel_path: CStr,
    lang: c.CBMLanguage,
    result_c: ?*const c.CBMFileResult,
    out: MutStr,
    out_sz: usize,
) void {
    if (out == null or out_sz == 0) return;
    out[0] = 0;
    const result = result_c orelse return;
    if (lang != c.CBM_LANG_PHP) return;
    var i: usize = 0;
    while (i < result.calls.count) : (i += 1) {
        const call = util.at(result.calls.items, i);
        if (call.callee_name != null and call.first_string_arg != null and
            call.first_string_arg[0] == '/' and
            c.cbm_service_pattern_route_method(call.callee_name) != null)
        {
            _ = cbm_laravel_api_mount(repo_path, rel_path, out, out_sz);
            return;
        }
    }
}

pub export fn cbm_laravel_mount_route(mount: CStr, path: CStr, buf: MutStr, buf_sz: usize) CStr {
    if (mount == null or mount[0] == 0 or path == null or buf == null or buf_sz == 0) return path;
    var rel: [*]const u8 = path;
    while (rel[0] == '/') rel += 1;
    const n = if (rel[0] != 0)
        c.snprintf(buf, buf_sz, "%s/%s", mount, rel)
    else
        c.snprintf(buf, buf_sz, "%s", mount);
    return if (n > 0 and @as(usize, @intCast(n)) < buf_sz) buf else path;
}
