//! gitignore.zig — Gitignore-style pattern matching.
//!
//! Implements the gitignore part of src/discover/discover.h, using the
//! core gitignore pattern matching algorithm:
//!   - * matches anything except /
//!   - ** matches any number of path components
//!   - ? matches any single character except /
//!   - [abc] and [a-z] character classes
//!   - ! prefix for negation
//!   - trailing / for directory-only matching
//!   - patterns with / are rooted (anchored to base)
//!
//! Memory is malloc'd: cbm_gitignore_free releases it.

const std = @import("std");
const c = @import("c");

const Str = [*:0]const u8;

const GI_INIT_CAP = 16;

// ── Pattern representation ─────────────────────────────────────────

const Pattern = struct {
    pattern: [*:0]u8, // the glob pattern (normalized)
    negated: bool, // starts with !
    dir_only: bool, // ends with /
    rooted: bool, // contains / (anchored to root)
};

/// The C header's opaque cbm_gitignore_t.
pub const Gitignore = struct {
    patterns: ?[*]Pattern,
    count: usize,
    capacity: usize,
};

// ── Pattern matching engine ────────────────────────────────────────

/// A byte as C `char`, whose signedness depends on the target: character
/// class ranges compare as C did, also for bytes >= 0x80.
fn cchar(b: u8) c_char {
    return @bitCast(b);
}

/// Match a ** (doublestar-slash) pattern: try rest at every / boundary.
fn globMatchDoublestarSlash(pat: Str, str: Str) bool {
    if (globMatch(pat, str)) return true;
    var s = str;
    while (s[0] != 0) : (s += 1) {
        if (s[0] == '/' and globMatch(pat, s + 1)) return true;
    }
    return false;
}

/// Match a ** (doublestar) followed by non-slash: try at every position.
fn globMatchDoublestarAny(pat: Str, str: Str) bool {
    var s = str;
    while (true) : (s += 1) {
        if (globMatch(pat, s)) return true;
        if (s[0] == 0) return false;
    }
}

/// Match a * (single star): match any sequence not containing /.
fn globMatchStar(pat: Str, str: Str) bool {
    var s = str;
    while (true) : (s += 1) {
        if (globMatch(pat, s)) return true;
        if (s[0] == 0 or s[0] == '/') return false;
    }
}

/// Match a [...] character class at the current position. Returns true if
/// matched and advances `pat_out` past the closing ']'.
fn globMatchCharclass(pat_in: Str, ch: u8, pat_out: *Str) bool {
    var pat = pat_in;
    var negate_class = false;
    if (pat[0] == '!' or pat[0] == '^') {
        negate_class = true;
        pat += 1;
    }
    var matched = false;
    var prev: u8 = 0;
    while (pat[0] != 0 and pat[0] != ']') {
        if (pat[0] == '-' and prev != 0 and pat[1] != 0 and pat[1] != ']') {
            pat += 1;
            if (cchar(ch) >= cchar(prev) and cchar(ch) <= cchar(pat[0])) matched = true;
            prev = pat[0];
            pat += 1;
        } else {
            if (ch == pat[0]) matched = true;
            prev = pat[0];
            pat += 1;
        }
    }
    if (pat[0] == ']') pat += 1;
    pat_out.* = pat;
    return if (negate_class) !matched else matched;
}

/// Handle ** at the current position.
fn globMatchDoublestar(pat: Str, str: Str) bool {
    if (pat[2] == '/') return globMatchDoublestarSlash(pat + 3, str);
    if (pat[2] == 0) return true;
    return globMatchDoublestarAny(pat + 2, str);
}

/// Match a glob pattern against a string.
/// Handles: * (non-slash), ** (any path), ? (single non-slash), [class]
fn globMatch(pat_in: Str, str_in: Str) bool {
    var pat = pat_in;
    var str = str_in;
    while (pat[0] != 0 and str[0] != 0) {
        if (pat[0] == '*' and pat[1] == '*') return globMatchDoublestar(pat, str);

        if (pat[0] == '*') return globMatchStar(pat + 1, str);

        if (pat[0] == '?') {
            if (str[0] == '/') return false;
            pat += 1;
            str += 1;
            continue;
        }

        if (pat[0] == '[') {
            var new_pat: Str = undefined;
            if (!globMatchCharclass(pat + 1, str[0], &new_pat)) return false;
            pat = new_pat;
            str += 1;
            continue;
        }

        if (pat[0] != str[0]) return false;
        pat += 1;
        str += 1;
    }

    while (pat[0] == '*') pat += 1;
    return pat[0] == 0 and str[0] == 0;
}

// ── Pattern parsing ────────────────────────────────────────────────

fn addPattern(gi: *Gitignore, line_in: []const u8) void {
    var line = line_in;
    // Trim trailing whitespace
    while (line.len > 0 and (line[line.len - 1] == ' ' or line[line.len - 1] == '\t' or
        line[line.len - 1] == '\r'))
    {
        line.len -= 1;
    }
    if (line.len == 0) return;

    var p: Pattern = .{ .pattern = undefined, .negated = false, .dir_only = false, .rooted = false };

    // Check for negation
    if (line[0] == '!') {
        p.negated = true;
        line = line[1..];
    }
    if (line.len == 0) return;

    // Check for trailing / (directory-only)
    if (line[line.len - 1] == '/') {
        p.dir_only = true;
        line.len -= 1;
    }
    if (line.len == 0) return;

    // Check for leading / (rooted)
    if (line[0] == '/') {
        p.rooted = true;
        line = line[1..];
    }
    if (line.len == 0) return;

    // A / anywhere in the pattern makes it rooted
    if (!p.rooted and std.mem.indexOfScalar(u8, line, '/') != null) p.rooted = true;

    // Copy pattern
    const copy: [*]u8 = @ptrCast(c.malloc(line.len + 1) orelse return);
    @memcpy(copy[0..line.len], line);
    copy[line.len] = 0;
    p.pattern = @ptrCast(copy);

    // Grow array if needed
    if (gi.count >= gi.capacity) {
        const new_cap = if (gi.capacity != 0) gi.capacity * 2 else GI_INIT_CAP;
        const grown = c.realloc(@ptrCast(gi.patterns), new_cap * @sizeOf(Pattern)) orelse {
            c.free(copy);
            return;
        };
        gi.patterns = @ptrCast(@alignCast(grown));
        gi.capacity = new_cap;
    }

    gi.patterns.?[gi.count] = p;
    gi.count += 1;
}

// ── Public API ─────────────────────────────────────────────────────

pub export fn cbm_gitignore_parse(content_c: ?Str) ?*Gitignore {
    const content = std.mem.span(content_c orelse return null);

    const gi: *Gitignore = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Gitignore)) orelse return null));

    var lines = std.mem.splitScalar(u8, content, '\n');
    while (lines.next()) |line| {
        // Skip comments and blank lines
        if (line.len > 0 and line[0] != '#') addPattern(gi, line);
    }
    return gi;
}

pub export fn cbm_gitignore_load(path_c: ?Str) ?*Gitignore {
    const path = path_c orelse return null;

    const f = c.cbm_fopen(path, "r") orelse return null;

    // Read entire file
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);

    if (size <= 0) {
        _ = c.fclose(f);
        return cbm_gitignore_parse("");
    }

    const len: usize = @intCast(size);
    const buf: [*]u8 = @ptrCast(c.malloc(len + 1) orelse {
        _ = c.fclose(f);
        return null;
    });

    const n = c.fread(buf, 1, len, f);
    buf[n] = 0;
    _ = c.fclose(f);

    const gi = cbm_gitignore_parse(@ptrCast(buf));
    c.free(buf);
    return gi;
}

/// Match a non-rooted pattern against the basename and every path suffix
/// that starts after a /.
fn matchUnrooted(pattern: Str, rel_path: Str, basename: Str) bool {
    if (globMatch(pattern, basename)) return true;
    const path = std.mem.span(rel_path);
    if (std.mem.indexOfScalar(u8, path, '/') == null) return false;
    var start: usize = 0;
    while (start < path.len) {
        if (globMatch(pattern, rel_path + start)) return true;
        const next = std.mem.indexOfScalarPos(u8, path, start, '/') orelse break;
        start = next + 1;
    }
    return false;
}

/// 1 when the last matching pattern ignores rel_path, -1 when it re-includes
/// it (a negated pattern), 0 when no pattern matches. discover.c tells the
/// last two apart, so it declares this export itself.
export fn cbm_gitignore_match_result(gi_c: ?*const Gitignore, rel_path_c: ?Str, is_dir: bool) c_int {
    const gi = gi_c orelse return 0;
    const rel_path = rel_path_c orelse return 0;

    // Extract the basename for non-rooted pattern matching
    const path = std.mem.span(rel_path);
    const basename: Str = if (std.mem.lastIndexOfScalar(u8, path, '/')) |i| rel_path + i + 1 else rel_path;

    var matched: c_int = 0;
    const patterns = if (gi.patterns) |ps| ps[0..gi.count] else &[_]Pattern{};
    for (patterns) |*p| {
        if (p.dir_only and !is_dir) continue;

        const this_match = if (p.rooted)
            globMatch(p.pattern, rel_path)
        else
            matchUnrooted(p.pattern, rel_path, basename);

        if (this_match) matched = if (p.negated) -1 else 1;
    }
    return matched;
}

pub export fn cbm_gitignore_matches(gi: ?*const Gitignore, rel_path: ?Str, is_dir: bool) bool {
    return cbm_gitignore_match_result(gi, rel_path, is_dir) > 0;
}

pub export fn cbm_gitignore_free(gi_c: ?*Gitignore) void {
    const gi = gi_c orelse return;
    if (gi.patterns) |ps| {
        for (ps[0..gi.count]) |p| c.free(p.pattern);
    }
    c.free(@ptrCast(gi.patterns));
    c.free(gi);
}

/// Test seam: lets a unit test simulate strdup() failure mid-merge so the
/// atomic-rollback path can be exercised without real OOM. NULL = use strdup.
export var cbm_gitignore_merge_dup_hook_for_test: ?*const fn (Str) callconv(.c) ?[*:0]u8 = null;

pub export fn cbm_gitignore_merge(dst_c: ?*Gitignore, src_c: ?*const Gitignore) bool {
    const dst = dst_c orelse return false;
    const src = src_c orelse return true; // nothing to merge
    if (src.count == 0) return true;

    const needed = dst.count + src.count;
    if (needed > dst.capacity) {
        const grown = c.realloc(@ptrCast(dst.patterns), needed * @sizeOf(Pattern)) orelse
            return false; // dst left unchanged
        dst.patterns = @ptrCast(@alignCast(grown));
        dst.capacity = needed;
    }
    const start_count = dst.count;
    for (src.patterns.?[0..src.count]) |sp| {
        const pat: ?[*:0]u8 = if (cbm_gitignore_merge_dup_hook_for_test) |dup|
            dup(sp.pattern)
        else
            c.strdup(sp.pattern);
        const copy = pat orelse {
            // Roll back partial copies so dst is unchanged on failure (atomic
            // merge). A silent partial merge could drop the very exclude
            // pattern the caller relied on while keeping others.
            for (dst.patterns.?[start_count..dst.count]) |p| c.free(p.pattern);
            dst.count = start_count;
            return false;
        };
        dst.patterns.?[dst.count] = .{
            .pattern = copy,
            .negated = sp.negated,
            .dir_only = sp.dir_only,
            .rooted = sp.rooted,
        };
        dst.count += 1;
    }
    return true;
}
