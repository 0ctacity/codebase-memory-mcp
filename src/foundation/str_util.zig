//! str_util.zig — Safe string operations (arena-allocated).
//!
//! Implements src/foundation/str_util.h.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const arena = @import("arena.zig");

const Str = [*:0]const u8;

const JSON_ESC_LEN = 2; // escaped char takes 2 bytes (backslash + char)
const JSON_NUL_RESERVE = 1; // reserve 1 byte for NUL terminator
const JSON_CTRL_LIMIT = 0x20; // ASCII control character upper bound

pub export fn cbm_path_join(a: ?*c.CBMArena, base: ?Str, name: ?Str) ?[*:0]u8 {
    const b = base orelse return null;
    var n = name orelse return null;
    var blen = std.mem.len(b);
    var nlen = std.mem.len(n);

    // Handle empty components
    if (blen == 0) return arena.cbm_arena_strdup(a, n);
    if (nlen == 0) return arena.cbm_arena_strdup(a, b);

    // Strip trailing slash from base
    while (blen > 0 and b[blen - 1] == '/') blen -= 1;
    // Strip leading slash from name
    while (nlen > 0 and n[0] == '/') {
        n += 1;
        nlen -= 1;
    }

    if (blen == 0) return arena.cbm_arena_strndup(a, n, nlen);
    if (nlen == 0) return arena.cbm_arena_strndup(a, b, blen);

    const result: [*]u8 = @ptrCast(arena.cbm_arena_alloc(a, blen + 1 + nlen + 1) orelse return null);
    @memcpy(result[0..blen], b[0..blen]);
    result[blen] = '/';
    @memcpy(result[blen + 1 ..][0..nlen], n[0..nlen]);
    result[blen + 1 + nlen] = 0;
    return @ptrCast(result);
}

pub export fn cbm_path_join_n(a: ?*c.CBMArena, parts: ?[*]?Str, n: c_int) ?[*:0]u8 {
    const p = parts orelse return arena.cbm_arena_strdup(a, "");
    if (n <= 0) return arena.cbm_arena_strdup(a, "");
    var result = arena.cbm_arena_strdup(a, p[0]);
    for (1..@intCast(n)) |i| {
        result = cbm_path_join(a, result, p[i]);
    }
    return result;
}

/// The last '.' and the last '/' of `path`, as indexes.
fn lastDotAndSlash(path: []const u8) struct { dot: ?usize, slash: ?usize } {
    return .{
        .dot = std.mem.lastIndexOfScalar(u8, path, '.'),
        .slash = std.mem.lastIndexOfScalar(u8, path, '/'),
    };
}

pub export fn cbm_path_ext(path: ?Str) Str {
    const p = path orelse return "";
    const marks = lastDotAndSlash(std.mem.span(p));
    // dot must be after last slash and not at start of basename
    const dot = marks.dot orelse return "";
    if (marks.slash) |slash| {
        if (dot < slash) return "";
    }
    return p + dot + 1;
}

pub export fn cbm_path_last_dot(path: ?Str) Str {
    const p = path orelse return "";
    const dot = std.mem.lastIndexOfScalar(u8, std.mem.span(p), '.') orelse return "";
    return p + dot;
}

pub export fn cbm_path_base(path: ?Str) Str {
    const p = path orelse return "";
    const slash = std.mem.lastIndexOfScalar(u8, std.mem.span(p), '/') orelse return p;
    return p + slash + 1;
}

pub export fn cbm_path_dir(a: ?*c.CBMArena, path: ?Str) ?[*:0]u8 {
    const p = path orelse return arena.cbm_arena_strdup(a, ".");
    const slash = std.mem.lastIndexOfScalar(u8, std.mem.span(p), '/') orelse
        return arena.cbm_arena_strdup(a, ".");
    return arena.cbm_arena_strndup(a, p, slash);
}

pub export fn cbm_str_starts_with(s: ?Str, prefix: ?Str) bool {
    const str = s orelse return false;
    const pre = prefix orelse return false;
    return std.mem.startsWith(u8, std.mem.span(str), std.mem.span(pre));
}

pub export fn cbm_str_ends_with(s: ?Str, suffix: ?Str) bool {
    const str = s orelse return false;
    const suf = suffix orelse return false;
    return std.mem.endsWith(u8, std.mem.span(str), std.mem.span(suf));
}

pub export fn cbm_str_contains(s: ?Str, sub: ?Str) bool {
    const str = s orelse return false;
    const needle = sub orelse return false;
    return std.mem.indexOf(u8, std.mem.span(str), std.mem.span(needle)) != null;
}

pub export fn cbm_str_tolower(a: ?*c.CBMArena, s: ?Str) ?[*:0]u8 {
    const src = std.mem.span(s orelse return null);
    const result: [*]u8 = @ptrCast(arena.cbm_arena_alloc(a, src.len + 1) orelse return null);
    for (src, 0..) |ch, i| result[i] = std.ascii.toLower(ch);
    result[src.len] = 0;
    return @ptrCast(result);
}

pub export fn cbm_str_replace_char(a: ?*c.CBMArena, s: ?Str, from: u8, to: u8) ?[*:0]u8 {
    const src = std.mem.span(s orelse return null);
    const result: [*]u8 = @ptrCast(arena.cbm_arena_alloc(a, src.len + 1) orelse return null);
    for (src, 0..) |ch, i| result[i] = if (ch == from) to else ch;
    result[src.len] = 0;
    return @ptrCast(result);
}

pub export fn cbm_str_strip_ext(a: ?*c.CBMArena, path: ?Str) ?[*:0]u8 {
    const p = path orelse return null;
    const marks = lastDotAndSlash(std.mem.span(p));
    const dot = marks.dot orelse return arena.cbm_arena_strdup(a, p);
    if (marks.slash) |slash| {
        if (dot < slash) return arena.cbm_arena_strdup(a, p);
    }
    return arena.cbm_arena_strndup(a, p, dot);
}

pub export fn cbm_str_split(a: ?*c.CBMArena, s: ?Str, delim: u8, out_count: ?*c_int) ?[*]?[*:0]u8 {
    const src = std.mem.span(s orelse return null);
    const count_out = out_count orelse return null;

    // Count parts
    const count = 1 + std.mem.count(u8, src, &.{delim});

    const result: [*]?[*:0]u8 = @ptrCast(@alignCast(arena.cbm_arena_alloc(a, (count + 1) * @sizeOf(?[*:0]u8)) orelse return null));

    var idx: usize = 0;
    var parts = std.mem.splitScalar(u8, src, delim);
    while (parts.next()) |part| : (idx += 1) {
        result[idx] = arena.cbm_arena_strndup(a, part.ptr, part.len);
    }
    result[idx] = null;
    count_out.* = @intCast(count);
    return result;
}

pub export fn cbm_validate_shell_arg(s: ?Str) bool {
    const str = std.mem.span(s orelse return false);
    for (str) |ch| {
        switch (ch) {
            '\'', '"', ';', '|', '&', '$', '`', '<', '>', '\n', '\r' => return false,
            // A backslash is a path separator on Windows.
            '\\' => if (builtin.os.tag != .windows) return false,
            else => {},
        }
    }
    return true;
}

pub export fn cbm_validate_project_name(name: ?Str) bool {
    const n = std.mem.span(name orelse return false);
    if (n.len == 0) return false;
    // Reject directory traversal
    if (std.mem.indexOf(u8, n, "..") != null) return false;
    // Reject leading dot (hidden files / relative refs)
    if (n[0] == '.') return false;
    // Allow only alphanumeric, dash, underscore, dot (so no path separators)
    for (n) |ch| {
        if (!(std.ascii.isAlphanumeric(ch) or ch == '-' or ch == '_' or ch == '.')) return false;
    }
    return true;
}

pub export fn cbm_json_escape(buf: ?[*]u8, bufsize: c_int, src: ?Str) c_int {
    const out = buf orelse return 0;
    if (bufsize <= 0) return 0;
    const s = src orelse {
        out[0] = 0;
        return 0;
    };

    const limit: usize = @intCast(bufsize - JSON_NUL_RESERVE);
    var pos: usize = 0;
    var i: usize = 0;
    while (s[i] != 0 and pos < limit) : (i += 1) {
        const ch = s[i];
        const short: ?u8 = switch (ch) {
            '"', '\\' => ch,
            '\n' => 'n',
            '\r' => 'r',
            '\t' => 't',
            else => null,
        };
        if (short) |esc| {
            if (pos + JSON_ESC_LEN > limit) break;
            out[pos] = '\\';
            out[pos + 1] = esc;
            pos += JSON_ESC_LEN;
        } else if (ch < JSON_CTRL_LIMIT) {
            // Other control chars: escape as \u00XX
            if (pos + 6 > limit) break;
            const hex = "0123456789abcdef";
            out[pos..][0..6].* = .{ '\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0xf] };
            pos += 6;
        } else {
            out[pos] = ch;
            pos += 1;
        }
    }
    out[pos] = 0;
    return @intCast(pos);
}
