//! fqn.zig — Fully Qualified Name computation for graph nodes.
//!
//! Implements the FQN functions of src/pipeline/pipeline.h. The FQN scheme is
//! project.dir.parts.name. Handles Python __init__.py, JS/TS index.{js,ts},
//! path separators.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");

const CStr = [*c]const u8;
const MutStr = [*c]u8;

/// Maximum path segments in a FQN (CBM_SZ_256 slots total, -2 for project + name)
const FQN_MAX_PATH_SEGS = 254;
const FQN_MAX_DIR_SEGS = 255;

/// Max bytes for a derived project name. Names were once filename components,
/// so they stay under the 255-byte component limit. #571 hex-encodes each
/// non-ASCII byte to 2 chars, so a deep CJK path can triple past 255 (#624).
const FQN_MAX_NAME_LEN = 200;

// ── Internal helpers ────────────────────────────────────────────────

/// Build a dot-joined string from segments. Returns heap-allocated string.
fn joinSegments(segments: []const CStr) MutStr {
    if (segments.len == 0) return c.strdup("");
    var total: usize = 0;
    for (segments, 0..) |seg, i| {
        total += c.strlen(seg);
        if (i > 0) total += 1; // dot separator
    }
    const result: MutStr = @ptrCast(c.malloc(total + 1));
    if (result == null) return null;
    var p: [*]u8 = result;
    for (segments, 0..) |seg, i| {
        if (i > 0) {
            p[0] = '.';
            p += 1;
        }
        const len = c.strlen(seg);
        @memcpy(p[0..len], seg[0..len]);
        p += len;
    }
    p[0] = 0;
    return result;
}

/// Strip file extension from the last path component.
fn stripFileExtension(path: MutStr) void {
    const last_slash: MutStr = c.strrchr(path, '/');
    const start: MutStr = if (last_slash != null) last_slash + 1 else path;
    const ext: MutStr = c.strrchr(start, '.');
    if (ext != null) ext[0] = 0;
}

/// Tokenize path by '/' into segments array. Returns number of segments added.
fn tokenizePath(path: MutStr, segments: []CStr) usize {
    var count: usize = 0;
    if (path[0] == 0) return 0;
    var tok: MutStr = path;
    while (tok != null and tok[0] != 0 and count < segments.len) {
        const slash: MutStr = c.strchr(tok, '/');
        if (slash != null) slash[0] = 0;
        if (tok[0] != 0) {
            segments[count] = tok;
            count += 1;
        }
        tok = if (slash != null) slash + 1 else null;
    }
    return count;
}

/// Strip __init__ (Python) / index (JS/TS) from the last segment when a
/// symbol name is provided. Keeps it when no name is given to avoid QN
/// collision with Folder nodes for the same directory.
fn stripInitOrIndex(segments: []const CStr, seg_count: *usize, name: CStr) void {
    if (seg_count.* <= 1) return;
    const last = segments[seg_count.* - 1];
    if (c.strcmp(last, "__init__") != 0 and c.strcmp(last, "index") != 0) return;
    if (name != null and name[0] != 0) seg_count.* -= 1;
}

// ── Public API ──────────────────────────────────────────────────────

pub export fn cbm_pipeline_fqn_compute(project: CStr, rel_path: CStr, name: CStr) MutStr {
    if (project == null) return c.strdup("");

    const path: MutStr = c.strdup(if (rel_path != null) rel_path else "");
    _ = c.cbm_normalize_path_sep(path);
    // File-node QNs (name=="__file__") keep the full filename so files that
    // share a stem get distinct nodes: auth.py/auth.ts, node.h/node.c,
    // .env/.env.local (upstream #769/#1077/#964). Module and symbol QNs keep
    // stripping; C/C++ declaration<->definition resolution relies on that.
    // Changing this scheme requires bumping CBM_INDEX_FORMAT_VERSION_STR.
    if (name == null or c.strcmp(name, "__file__") != 0) {
        stripFileExtension(path);
    }

    var segments: [c.CBM_SZ_256]CStr = undefined;
    var seg_count: usize = 0;
    segments[seg_count] = project;
    seg_count += 1;
    seg_count += tokenizePath(path, segments[seg_count..][0..FQN_MAX_PATH_SEGS]);

    stripInitOrIndex(&segments, &seg_count, name);

    if (name != null and name[0] != 0) {
        segments[seg_count] = name;
        seg_count += 1;
    }

    const result = joinSegments(segments[0..seg_count]);
    c.free(path);
    return result;
}

pub export fn cbm_pipeline_fqn_module(project: CStr, rel_path: CStr) MutStr {
    return cbm_pipeline_fqn_compute(project, rel_path, null);
}

pub export fn cbm_pipeline_fqn_module_dir(project: CStr, rel_path: CStr, module_is_dir: bool) MutStr {
    if (!module_is_dir) {
        // Filename-stem module (default for all but Java/Go).
        return cbm_pipeline_fqn_module(project, rel_path);
    }
    // Directory-module languages (Java package, Go package): the module is the
    // CONTAINING DIRECTORY — strip the basename so a sibling file in the same
    // dir shares the module QN. This MUST agree with the extraction-side
    // cbm_fqn_module_source_lang() (internal/cbm/helpers.c) so the cross-file
    // LSP caller_qn matches the def-node QN.
    const src: CStr = if (rel_path != null) rel_path else "";
    // Strip the last path segment using either separator (the extraction side
    // normalizes too); look for the rightmost '/' or '\\'.
    const last_fwd: CStr = c.strrchr(src, '/');
    const last_bwd: CStr = c.strrchr(src, '\\');
    const last_sep: CStr = if (@intFromPtr(last_fwd) > @intFromPtr(last_bwd)) last_fwd else last_bwd;
    if (last_sep == null) {
        // Root file: empty directory → module is just the project.
        return cbm_pipeline_fqn_folder(project, "");
    }
    const dir_len = @intFromPtr(last_sep) - @intFromPtr(src);
    const dir: MutStr = @ptrCast(c.malloc(dir_len + 1)); // +1 for NUL
    if (dir == null) return null;
    @memcpy(dir[0..dir_len], src[0..dir_len]);
    dir[dir_len] = 0;
    const res = cbm_pipeline_fqn_folder(project, dir);
    c.free(dir);
    return res;
}

const FQN_PATH_BUF = 1024;
const FQN_SEP_LEN = 1; // one byte for the '/' separator
const FQN_NUL_LEN = 1; // one byte for the terminating NUL
const FQN_DOTDOT_LEN = 2;
const FQN_MIN_PY_DOTS = 1; // first leading dot is "current package", not a pop

const RelKind = enum { none, python, js };

/// Append a single path segment to a mutable buffer that already holds a
/// normalized slash-separated path. Adds a '/' separator when needed,
/// returns false if the buffer would overflow.
fn pathAppendSegment(buf: []u8, seg: []const u8) bool {
    var cur = c.strlen(buf.ptr);
    const need = cur + @as(usize, if (cur > 0) FQN_SEP_LEN else 0) + seg.len + FQN_NUL_LEN;
    if (need > buf.len) return false;
    if (cur > 0) {
        buf[cur] = '/';
        cur += 1;
    }
    @memcpy(buf[cur..][0..seg.len], seg);
    buf[cur + seg.len] = 0;
    return true;
}

/// Pop the last segment from a mutable slash-separated path.
fn pathPopSegment(buf: [*]u8) void {
    const last: MutStr = c.strrchr(buf, '/');
    if (last != null) {
        last[0] = 0;
    } else {
        buf[0] = 0;
    }
}

/// Seed `buf` with the source file's directory (strip the basename) and
/// normalize backslashes.
fn seedSourceDir(buf: []u8, source_rel: CStr) void {
    _ = c.snprintf(buf.ptr, buf.len, "%s", @as(CStr, if (source_rel != null) source_rel else ""));
    var i: usize = 0;
    while (buf[i] != 0) : (i += 1) {
        if (buf[i] == '\\') buf[i] = '/';
    }
    pathPopSegment(buf.ptr);
}

/// Detect the flavor of relative import based on the leading characters.
/// Returns .python for the Python dotted form (e.g. ".foo" or "..bar.baz"),
///         .js for the JS/TS slash form (e.g. "./foo" or "../bar/baz") and for
///           a Zig file import ("db.zig", "sub/x.zig"), which Zig resolves
///           against the importing file's directory with or without a leading
///           "./",
///         .none for anything not relative (caller should skip).
fn classifyRelativeImport(module_path: CStr) RelKind {
    const len: usize = if (module_path != null) c.strlen(module_path) else 0;
    if (len > 4 and module_path[0] != '/' and c.strcmp(module_path + len - 4, ".zig") == 0) {
        return .js;
    }
    if (module_path == null or module_path[0] != '.') return .none;
    const has_slash = c.strchr(module_path, '/') != null;
    const js_like = module_path[FQN_SEP_LEN] == '/' or
        (module_path[FQN_SEP_LEN] == '.' and module_path[FQN_DOTDOT_LEN] == '/');
    if (has_slash or js_like) return .js;
    return .python;
}

/// Python relative import: ".foo", "..bar.baz" → resolve against source dir.
fn resolvePythonRelative(buf: []u8, module_path: [*:0]const u8) MutStr {
    var p = module_path;
    var dot_count: c_int = 0;
    while (p[0] == '.') {
        dot_count += 1;
        p += 1;
    }
    var i: c_int = FQN_MIN_PY_DOTS;
    while (i < dot_count) : (i += 1) {
        pathPopSegment(buf.ptr);
    }
    while (p[0] != 0) {
        const seg_start = p;
        while (p[0] != 0 and p[0] != '.') p += 1;
        const seg_len = @intFromPtr(p) - @intFromPtr(seg_start);
        if (seg_len > 0 and !pathAppendSegment(buf, seg_start[0..seg_len])) return null;
        if (p[0] == '.') p += 1;
    }
    return c.strdup(buf.ptr);
}

/// Strip an explicit JS/TS (or Zig) module file extension while preserving dots
/// that are part of an extensionless basename (e.g. "featureX.engine").
fn stripJsModuleExt(seg: []const u8) usize {
    const extensions = [_][]const u8{
        ".js", ".jsx", ".mjs", ".cjs", ".ts", ".tsx", ".mts", ".cts", ".json", ".zig",
    };
    for (extensions) |ext| {
        if (seg.len > ext.len and std.mem.eql(u8, seg[seg.len - ext.len ..], ext)) {
            return seg.len - ext.len;
        }
    }
    return seg.len;
}

/// JS/TS relative import: "./foo", "../bar/baz" → resolve against source dir.
fn resolveJsRelative(buf: []u8, module_path: [*:0]const u8) MutStr {
    var p = module_path;
    while (p[0] != 0) {
        while (p[0] == '/') p += 1;
        if (p[0] == 0) break;
        const seg_start = p;
        while (p[0] != 0 and p[0] != '/') p += 1;
        var seg_len = @intFromPtr(p) - @intFromPtr(seg_start);
        if (seg_len == FQN_SEP_LEN and seg_start[0] == '.') continue;
        if (seg_len == FQN_DOTDOT_LEN and seg_start[0] == '.' and seg_start[FQN_SEP_LEN] == '.') {
            pathPopSegment(buf.ptr);
            continue;
        }
        if (p[0] == 0) {
            seg_len = stripJsModuleExt(seg_start[0..seg_len]);
        }
        if (seg_len > 0 and !pathAppendSegment(buf, seg_start[0..seg_len])) return null;
    }
    return c.strdup(buf.ptr);
}

pub export fn cbm_pipeline_resolve_relative_import(source_rel: CStr, module_path: CStr) MutStr {
    const kind = classifyRelativeImport(module_path);
    if (kind == .none) return null;
    var buf: [FQN_PATH_BUF]u8 = undefined;
    seedSourceDir(&buf, source_rel);
    if (kind == .python) {
        return resolvePythonRelative(&buf, module_path);
    }
    return resolveJsRelative(&buf, module_path);
}

pub export fn cbm_pipeline_fqn_folder(project: CStr, rel_dir: CStr) MutStr {
    if (project == null) return c.strdup("");

    // Work on mutable copy
    const dir: MutStr = c.strdup(if (rel_dir != null) rel_dir else "");
    _ = c.cbm_normalize_path_sep(dir);

    var segments: [c.CBM_SZ_256]CStr = undefined;
    var seg_count: usize = 0;
    segments[seg_count] = project;
    seg_count += 1;

    if (dir[0] != 0) {
        var tok: MutStr = dir;
        while (tok != null and tok[0] != 0 and seg_count < FQN_MAX_DIR_SEGS) {
            const slash: MutStr = c.strchr(tok, '/');
            if (slash != null) slash[0] = 0;
            if (tok[0] != 0) {
                segments[seg_count] = tok;
                seg_count += 1;
            }
            tok = if (slash != null) slash + 1 else null;
        }
    }

    const result = joinSegments(segments[0..seg_count]);
    c.free(dir);
    return result;
}

/// Bound a derived project name to FQN_MAX_NAME_LEN bytes so "<cache>/<name>.db"
/// stays within the filesystem's 255-byte filename-component limit (#624). Names
/// within the cap are returned UNCHANGED (no drift). Longer names keep their first
/// (CAP-9) bytes and get a "-XXXXXXXX" FNV-1a hash of the FULL name appended, so
/// two long paths that share a prefix but differ later still map to distinct
/// names. The suffix ends in a hex digit, so the result stays validator-safe.
fn fqnBoundNameLen(name: MutStr) MutStr {
    if (name == null) return name;
    const n = c.strlen(name);
    if (n <= FQN_MAX_NAME_LEN) return name; // within cap → unchanged, no drift
    var h: u32 = 2166136261; // FNV-1a offset basis over the FULL name
    for (name[0..n]) |ch| {
        h ^= ch;
        h *%= 16777619;
    }
    // Keep first (CAP-9) bytes + "-" + 8 hex = CAP total. The buffer holds n+1
    // bytes and n > CAP, so writing 9 chars + NUL at offset (CAP-9) fits.
    _ = c.snprintf(name + (FQN_MAX_NAME_LEN - 9), 10, "-%08x", h);
    return name;
}

fn pathIsRootSyntax(path: CStr) bool {
    if (path == null or path[0] == 0) return false;
    var i: usize = 0;
    while (path[i] != 0) : (i += 1) {
        if (path[i] != '/' and path[i] != '\\' and path[i] != ':') return false;
    }
    return true;
}

fn isNameSafe(ch: u8) bool {
    return (ch >= 'a' and ch <= 'z') or (ch >= 'A' and ch <= 'Z') or (ch >= '0' and ch <= '9') or
        ch == '.' or ch == '_' or ch == '-';
}

extern fn _fullpath(abs: [*]u8, rel: [*:0]const u8, max: usize) ?[*:0]u8;
extern fn _access(path: [*:0]const u8, mode: c_int) c_int;

/// realpath(3), or _access + _fullpath on Windows. True when out holds the
/// resolved path.
fn resolveRealPath(path: [*:0]const u8, out: *[c.CBM_SZ_4K]u8) bool {
    if (builtin.os.tag == .windows) {
        return _access(path, 0) == 0 and _fullpath(out, path, out.len) != null;
    }
    return c.realpath(path, out) != null;
}

const hex_digits = "0123456789abcdef";

pub export fn cbm_project_name_from_path(abs_path: CStr) MutStr {
    if (abs_path == null or abs_path[0] == 0) return c.strdup("root");
    if (pathIsRootSyntax(abs_path)) return c.strdup("root");

    var real: [c.CBM_SZ_4K]u8 = undefined;
    var name_path: CStr = abs_path;
    if (resolveRealPath(abs_path, &real)) {
        _ = c.cbm_normalize_path_sep(&real);
        name_path = &real;
    }

    // Work on mutable copy
    var path: MutStr = c.strdup(name_path);
    if (path == null) return null;
    var len = c.strlen(path);

    // Normalize path separators
    _ = c.cbm_normalize_path_sep(path);

    // Map every character that is unsafe for portable project DB names. We
    // keep derived names in [A-Za-z0-9._-], so anything else — path
    // separators, ':', spaces, '@', '+', … — must be normalized here.
    // Otherwise a repo like
    // "/home/u/my project" yields the name "home-u-my project": indexing
    // creates the DB and it shows in list_projects, but resolve_store rejects
    // the space and reports project-not-found (#349).
    //
    // Non-ASCII bytes (UTF-8 of CJK and other scripts, all >= 0x80) are NOT
    // dropped to '-' — that silently erased whole path segments and produced
    // unrecognizable / colliding names (#571). Instead each non-ASCII byte is
    // transliterated to its two lowercase hex digits, which use only [0-9a-f]
    // and therefore stay validator-safe while preserving the segment.
    const mapped: MutStr = @ptrCast(c.malloc(len * 2 + 1)); // worst case: every byte → 2 hex chars
    if (mapped == null) {
        c.free(path);
        return c.strdup("root");
    }
    var mlen: usize = 0;
    for (path[0..len]) |ch| {
        if (isNameSafe(ch)) {
            mapped[mlen] = ch;
            mlen += 1;
        } else if (ch >= 0x80) {
            mapped[mlen] = hex_digits[(ch >> 4) & 0xF];
            mapped[mlen + 1] = hex_digits[ch & 0xF];
            mlen += 2;
        } else {
            mapped[mlen] = '-';
            mlen += 1;
        }
    }
    mapped[mlen] = 0;
    c.free(path);
    path = mapped;
    len = mlen;

    // Collapse consecutive dashes, and consecutive dots (the validator also
    // rejects any ".." sequence).
    var dst: usize = 0;
    var prev: u8 = 0;
    for (0..len) |i| {
        if ((path[i] == '-' and prev == '-') or (path[i] == '.' and prev == '.')) continue;
        path[dst] = path[i];
        dst += 1;
        prev = path[i];
    }
    path[dst] = 0;

    // Trim leading dashes and dots (the validator rejects a leading dot).
    var start: MutStr = path;
    while (start[0] == '-' or start[0] == '.') start += 1;

    // Trim trailing dashes
    var slen = c.strlen(start);
    while (slen > 0 and start[slen - 1] == '-') {
        slen -= 1;
        start[slen] = 0;
    }

    if (start[0] == 0) {
        c.free(path);
        return c.strdup("root");
    }

    var result: MutStr = c.strdup(start);
    c.free(path);
    if (result != null) {
        result = fqnBoundNameLen(result); // #624: cap filename-component length
    }
    return result;
}

fn projectSelectorComponent(component: CStr) MutStr {
    if (component == null or component[0] == 0) return c.strdup("root");

    const len = c.strlen(component);
    const mapped: MutStr = @ptrCast(c.malloc(len * 2 + 1));
    if (mapped == null) return null;
    var used: usize = 0;
    var previous: u8 = 0;
    for (component[0..len]) |ch| {
        if (isNameSafe(ch)) {
            if ((ch == '-' and previous == '-') or (ch == '.' and previous == '.')) continue;
            mapped[used] = ch;
            used += 1;
            previous = ch;
        } else if (ch >= 0x80) {
            mapped[used] = hex_digits[(ch >> 4) & 0xf];
            mapped[used + 1] = hex_digits[ch & 0xf];
            used += 2;
            previous = mapped[used - 1];
        } else if (previous != '-') {
            mapped[used] = '-';
            used += 1;
            previous = '-';
        }
    }
    mapped[used] = 0;

    var start: MutStr = mapped;
    while (start[0] == '-' or start[0] == '.') start += 1;
    var clean_len = c.strlen(start);
    while (clean_len > 0 and start[clean_len - 1] == '-') {
        clean_len -= 1;
        start[clean_len] = 0;
    }
    const result: MutStr = c.strdup(if (start[0] != 0) start else "root");
    c.free(mapped);
    return if (result != null) fqnBoundNameLen(result) else null;
}

pub export fn cbm_project_selector_from_path(abs_path: CStr, parent_levels: usize) MutStr {
    if (abs_path == null or abs_path[0] == 0 or pathIsRootSyntax(abs_path)) {
        return c.strdup("root");
    }

    const path: MutStr = c.strdup(abs_path);
    if (path == null) return null;
    _ = c.cbm_normalize_path_sep(path);
    var len = c.strlen(path);
    while (len > 0 and path[len - 1] == '/') {
        len -= 1;
        path[len] = 0;
    }
    if (len == 0) {
        c.free(path);
        return c.strdup("root");
    }

    const component_capacity = len + 1;
    const components: [*c]MutStr = @ptrCast(@alignCast(c.malloc(component_capacity * @sizeOf(MutStr))));
    if (components == null) {
        c.free(path);
        return null;
    }
    var component_count: usize = 0;
    var cursor: [*]u8 = path;
    while (cursor[0] != 0) {
        while (cursor[0] == '/') cursor += 1;
        if (cursor[0] == 0) break;
        components[component_count] = cursor;
        component_count += 1;
        while (cursor[0] != 0 and cursor[0] != '/') cursor += 1;
        if (cursor[0] != 0) {
            cursor[0] = 0;
            cursor += 1;
        }
    }
    if (component_count == 0) {
        c.free(@ptrCast(components));
        c.free(path);
        return c.strdup("root");
    }

    var wanted = if (parent_levels == std.math.maxInt(usize)) component_count else parent_levels + 1;
    if (wanted > component_count) wanted = component_count;
    const first = component_count - wanted;
    const clean: [*c]MutStr = @ptrCast(@alignCast(c.calloc(wanted, @sizeOf(MutStr))));
    var output_len: usize = if (wanted > 0) wanted - 1 else 0;
    if (clean == null) {
        c.free(@ptrCast(components));
        c.free(path);
        return null;
    }
    var ok = true;
    for (0..wanted) |i| {
        clean[i] = projectSelectorComponent(components[first + i]);
        if (clean[i] == null) {
            ok = false;
            break;
        }
        if (std.math.maxInt(usize) - output_len < c.strlen(clean[i])) {
            ok = false;
            break;
        }
        output_len += c.strlen(clean[i]);
    }

    const result: MutStr = if (ok) @ptrCast(c.malloc(output_len + 1)) else null;
    if (result != null) {
        var out: [*]u8 = result;
        for (0..wanted) |i| {
            if (i != 0) {
                out[0] = '/';
                out += 1;
            }
            const part_len = c.strlen(clean[i]);
            @memcpy(out[0..part_len], clean[i][0..part_len]);
            out += part_len;
        }
        out[0] = 0;
    }
    for (0..wanted) |i| c.free(clean[i]);
    c.free(@ptrCast(clean));
    c.free(@ptrCast(components));
    c.free(path);
    return result;
}
