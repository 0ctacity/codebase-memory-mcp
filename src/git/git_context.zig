//! git_context.zig — git metadata for a repository path.
//!
//! Implements src/git/git_context.h. Strings handed to C are allocated with
//! the C allocator because cbm_git_context_free() and C callers free() them.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");

const is_windows = builtin.os.tag == .windows;

const GIT_CMD_MAX = 1024;
const GIT_OUTPUT_MAX = 4096;

fn dupBytes(s: []const u8) ?[*:0]u8 {
    const raw: [*]u8 = @ptrCast(c.malloc(s.len + 1) orelse return null);
    @memcpy(raw[0..s.len], s);
    raw[s.len] = 0;
    return @ptrCast(raw);
}

fn gitStrdup(s: ?[*:0]const u8) ?[*:0]u8 {
    return dupBytes(if (s) |p| std.mem.span(p) else "");
}

fn spanOrEmpty(s: [*c]const u8) []const u8 {
    return if (s == null) "" else std.mem.span(@as([*:0]const u8, @ptrCast(s)));
}

fn gitValidateRepoPath(repo_path: [*:0]const u8) bool {
    if (!c.cbm_validate_shell_arg(repo_path)) return false;
    if (is_windows) {
        for (std.mem.span(repo_path)) |ch| {
            if (ch == '%' or ch == '!' or ch == '^') return false;
        }
    }
    return true;
}

/// Runs `git -C <repo_path> <git_args>` and returns its first output line,
/// or CBM_NOT_FOUND when git fails or prints nothing.
fn gitCapture(repo_path: [*:0]const u8, git_args: []const u8, out: *[*c]u8) c_int {
    out.* = null;
    if (!gitValidateRepoPath(repo_path)) return c.CBM_NOT_FOUND;

    const null_dev = if (is_windows) "NUL" else "/dev/null";
    // Double quotes work for POSIX shells and cmd.exe. cbm_validate_shell_arg()
    // rejects quote/backslash/substitution metacharacters before interpolation.
    var cmd: [GIT_CMD_MAX]u8 = undefined;
    const cmd_z = std.mem.printSentinel(&cmd, "git -C \"{s}\" {s} 2>{s}", .{
        std.mem.span(repo_path), git_args, null_dev,
    }, 0) catch return c.CBM_NOT_FOUND;

    const fp = c.cbm_popen_git(cmd_z.ptr);
    if (fp == null) return c.CBM_NOT_FOUND;

    var buf: [GIT_OUTPUT_MAX]u8 = undefined;
    if (c.fgets(&buf, buf.len, fp) == null) {
        _ = c.cbm_pclose(fp);
        return c.CBM_NOT_FOUND;
    }
    const line = std.mem.trimEnd(u8, std.mem.sliceTo(&buf, 0), "\r\n");

    const rc = c.cbm_pclose(fp);
    if (rc != 0 or line.len == 0) return c.CBM_NOT_FOUND;

    out.* = dupBytes(line) orelse return c.CBM_NOT_FOUND;
    return 0;
}

/// `git -C <repo_path> ls-files -z`: every path git tracks below repo_path,
/// relative to it and NUL-separated, in one malloc'd buffer the caller frees.
/// One extra NUL follows the output; *length excludes it. NULL when the path
/// is rejected, git fails, or memory runs out.
pub export fn cbm_git_ls_files(repo_path_c: ?[*:0]const u8, length_c: ?*usize) ?[*:0]u8 {
    const length = length_c orelse return null;
    length.* = 0;
    const repo_path = repo_path_c orelse return null;
    if (!gitValidateRepoPath(repo_path)) return null;

    const null_dev = if (is_windows) "NUL" else "/dev/null";
    // core.fsmonitor=false: a configured fsmonitor hook must not run for a
    // read-only listing.
    var cmd: [GIT_CMD_MAX]u8 = undefined;
    const cmd_z = std.mem.printSentinel(&cmd, "git -C \"{s}\" -c core.fsmonitor=false ls-files -z 2>{s}", .{
        std.mem.span(repo_path), null_dev,
    }, 0) catch return null;

    const fp = c.cbm_popen_git(cmd_z.ptr);
    if (fp == null) return null;

    var cap: usize = GIT_OUTPUT_MAX;
    var buf: [*]u8 = @ptrCast(c.malloc(cap + 1) orelse {
        _ = c.cbm_pclose(fp);
        return null;
    });
    var len: usize = 0;
    while (true) {
        if (len == cap) {
            const grown = c.realloc(buf, cap * 2 + 1) orelse {
                c.free(buf);
                _ = c.cbm_pclose(fp);
                return null;
            };
            buf = @ptrCast(grown);
            cap *= 2;
        }
        const n = c.fread(buf + len, 1, cap - len, fp);
        if (n == 0) break;
        len += n;
    }
    if (c.cbm_pclose(fp) != 0) {
        c.free(buf);
        return null;
    }
    buf[len] = 0;
    length.* = len;
    return @ptrCast(buf);
}

fn pathIsAbsolute(path: []const u8) bool {
    if (path.len == 0) return false;
    if (path[0] == '/') return true;
    if (is_windows) return std.ascii.isAlphabetic(path[0]) and path.len > 1 and path[1] == ':';
    return false;
}

fn joinRootRelative(root: []const u8, rel: []const u8) ?[*:0]u8 {
    if (root.len == 0) return dupBytes(rel);
    const raw: [*]u8 = @ptrCast(c.malloc(root.len + 1 + rel.len + 1) orelse return null);
    @memcpy(raw[0..root.len], root);
    raw[root.len] = '/';
    @memcpy(raw[root.len + 1 ..][0..rel.len], rel);
    raw[root.len + 1 + rel.len] = 0;
    return @ptrCast(raw);
}

/// Derive the canonical repo root.
///
/// Preferred (git 2.31+): abs_common_dir is `git rev-parse --path-format=absolute
/// --git-common-dir` — git's OWN absolute, canonical common-dir. Because a main
/// repo and its linked worktree both ask the same git binary, they resolve to the
/// IDENTICAL path, so canonical_root is consistent across worktrees AND platforms
/// with no manual join or realpath/_fullpath — which diverged under msys vs native
/// path representations (#659 root cause, and the worktree==main-root breakage in
/// test_pipeline.c git_context_linked_worktree). git also resolves the relative
/// ".." internally, so the subdirectory case (#659) is fixed here too.
///
/// Fallback (git < 2.31, no --path-format → abs_common_dir empty): the relative
/// --git-common-dir is relative to input_path (the -C dir), so join against it and
/// realpath-normalize the "..". Unix only — on Windows git emits an absolute
/// common-dir so this branch isn't reached in practice, and _fullpath there
/// reintroduces the msys divergence.
fn deriveCanonicalRoot(
    input_path: []const u8,
    worktree_root: [*c]const u8,
    git_common_dir: [*c]const u8,
    abs_common_dir: [*c]const u8,
) ?[*:0]u8 {
    var root: [*:0]u8 = undefined;
    const abs = spanOrEmpty(abs_common_dir);
    if (abs.len > 0 and pathIsAbsolute(abs)) {
        root = dupBytes(abs) orelse return null;
    } else {
        const common = spanOrEmpty(git_common_dir);
        const src: []const u8 = if (common.len > 0)
            common
        else if (worktree_root != null)
            spanOrEmpty(worktree_root)
        else
            return gitStrdup("");
        if (is_windows) {
            root = (if (pathIsAbsolute(src))
                dupBytes(src)
            else
                joinRootRelative(spanOrEmpty(worktree_root), src)) orelse return null;
        } else {
            root = (if (pathIsAbsolute(src)) dupBytes(src) else joinRootRelative(input_path, src)) orelse
                return null;
            var resolved: [4096]u8 = undefined;
            if (c.realpath(root, &resolved) != null) {
                c.free(root);
                root = dupBytes(std.mem.sliceTo(&resolved, 0)) orelse return null;
            }
        }
    }

    var len = std.mem.len(root);
    while (len > 1 and (root[len - 1] == '/' or root[len - 1] == '\\')) {
        len -= 1;
        root[len] = 0;
    }
    const tail = root[0..len];
    if (std.mem.endsWith(u8, tail, "/.git") or (is_windows and std.mem.endsWith(u8, tail, "\\.git"))) {
        root[len - 5] = 0;
    }
    return root;
}

fn slugFromBranch(branch: [*c]const u8, detached: bool) ?[*:0]u8 {
    const fallback: []const u8 = if (detached) "detached" else "working-tree";
    const name = spanOrEmpty(branch);
    const src = if (detached or name.len == 0) fallback else name;
    const slug: [*]u8 = @ptrCast(c.malloc(src.len + 1) orelse return null);

    var j: usize = 0;
    var in_dash = false;
    for (src) |ch| {
        if (std.ascii.isAlphanumeric(ch) or ch == '-' or ch == '_' or ch == '.') {
            if (j == 0 and ch == '-') {
                in_dash = true;
                continue;
            }
            slug[j] = ch;
            j += 1;
            in_dash = false;
        } else if (j > 0 and !in_dash) {
            slug[j] = '-';
            j += 1;
            in_dash = true;
        }
    }
    while (j > 0 and slug[j - 1] == '-') j -= 1;
    slug[j] = 0;

    if (j == 0) {
        c.free(slug);
        return dupBytes(fallback);
    }
    return @ptrCast(slug);
}

pub export fn cbm_git_context_free(ctx: ?*c.cbm_git_context_t) void {
    const g = ctx orelse return;
    c.free(g.input_path);
    c.free(g.worktree_root);
    c.free(g.git_dir);
    c.free(g.git_common_dir);
    c.free(g.canonical_root);
    c.free(g.branch);
    c.free(g.branch_slug);
    c.free(g.head_sha);
    c.free(g.base_sha);
    g.* = std.mem.zeroes(c.cbm_git_context_t);
}

/// gitCapture, or an empty string when the lookup fails.
fn captureOrEmpty(path: [*:0]const u8, args: []const u8, out: *[*c]u8) void {
    if (gitCapture(path, args, out) != 0) out.* = gitStrdup("");
}

pub export fn cbm_git_context_resolve(path_c: ?[*:0]const u8, out_c: ?*c.cbm_git_context_t) c_int {
    const out = out_c orelse return c.CBM_NOT_FOUND;
    out.* = std.mem.zeroes(c.cbm_git_context_t);
    const path = path_c orelse return c.CBM_NOT_FOUND;
    if (path[0] == 0) return c.CBM_NOT_FOUND;

    out.input_path = gitStrdup(path) orelse return c.CBM_NOT_FOUND;

    var st: c.struct_stat = undefined;
    out.root_exists = c.stat(path, &st) == 0;
    if (!out.root_exists) return 0;

    if (gitCapture(path, "rev-parse --show-toplevel", &out.worktree_root) != 0) {
        out.is_git = false;
        return 0;
    }
    out.is_git = true;

    captureOrEmpty(path, "rev-parse --git-dir", &out.git_dir);
    captureOrEmpty(path, "rev-parse --git-common-dir", &out.git_common_dir);
    captureOrEmpty(path, "rev-parse --verify HEAD", &out.head_sha);

    if (gitCapture(path, "symbolic-ref --quiet --short HEAD", &out.branch) != 0) {
        out.branch = gitStrdup("DETACHED");
        out.is_detached = true;
    }

    out.is_worktree = out.git_dir != null and out.git_common_dir != null and
        !std.mem.eql(u8, spanOrEmpty(out.git_dir), spanOrEmpty(out.git_common_dir));
    // git 2.31+ canonical absolute common-dir (best-effort; NULL on older git,
    // where deriveCanonicalRoot falls back to the relative common-dir).
    var abs_common_dir: [*c]u8 = null;
    _ = gitCapture(path, "rev-parse --path-format=absolute --git-common-dir", &abs_common_dir);
    out.canonical_root =
        deriveCanonicalRoot(std.mem.span(path), out.worktree_root, out.git_common_dir, abs_common_dir);
    c.free(abs_common_dir);
    out.branch_slug = slugFromBranch(out.branch, out.is_detached);
    captureOrEmpty(path, "merge-base HEAD @{upstream}", &out.base_sha);

    if (out.git_dir == null or out.git_common_dir == null or out.head_sha == null or
        out.branch == null or out.canonical_root == null or out.branch_slug == null or
        out.base_sha == null)
    {
        cbm_git_context_free(out);
        return c.CBM_NOT_FOUND;
    }
    return 0;
}

pub export fn cbm_git_context_branch_qn(
    project_name: [*c]const u8,
    ctx_c: ?*const c.cbm_git_context_t,
) ?[*:0]u8 {
    const name = spanOrEmpty(project_name);
    const project = if (name.len > 0) name else "project";
    var slug: []const u8 = "working-tree";
    if (ctx_c) |ctx| {
        if (ctx.is_detached) {
            slug = "detached";
        } else if (ctx.is_git and spanOrEmpty(ctx.branch_slug).len > 0) {
            slug = spanOrEmpty(ctx.branch_slug);
        }
    }

    const sep = ".__branch__.";
    const n = project.len + sep.len + slug.len;
    const raw: [*]u8 = @ptrCast(c.malloc(n + 1) orelse return null);
    @memcpy(raw[0..project.len], project);
    @memcpy(raw[project.len..][0..sep.len], sep);
    @memcpy(raw[project.len + sep.len ..][0..slug.len], slug);
    raw[n] = 0;
    return @ptrCast(raw);
}

/// Appends into a fixed C buffer with snprintf semantics: false when the text
/// does not fit together with its terminating NUL.
const JsonOut = struct {
    buf: [*]u8,
    size: usize,
    off: usize = 0,

    fn put(self: *JsonOut, s: []const u8) bool {
        if (self.off + s.len >= self.size) {
            self.buf[self.size - 1] = 0;
            return false;
        }
        @memcpy(self.buf[self.off..][0..s.len], s);
        self.off += s.len;
        self.buf[self.off] = 0;
        return true;
    }

    fn boolField(self: *JsonOut, name: []const u8, value: bool, comma: bool) bool {
        return self.put("\"") and self.put(name) and self.put("\":") and
            self.put(if (value) "true" else "false") and self.put(if (comma) "," else "");
    }

    fn stringField(self: *JsonOut, name: []const u8, value_c: [*c]const u8, comma: bool) bool {
        const value: [*:0]const u8 = if (value_c == null) "" else @ptrCast(value_c);
        const needed = jsonEscapedLen(std.mem.span(value));
        const escaped: [*]u8 = @ptrCast(c.malloc(needed + 1) orelse return false);
        defer c.free(escaped);
        const actual = c.cbm_json_escape(escaped, @intCast(needed + 1), value);
        return actual == needed and self.put("\"") and self.put(name) and self.put("\":\"") and
            self.put(escaped[0..needed]) and self.put("\"") and self.put(if (comma) "," else "");
    }
};

fn jsonEscapedLen(src: []const u8) usize {
    var len: usize = 0;
    for (src) |ch| {
        if (ch == '"' or ch == '\\' or ch == '\n' or ch == '\r' or ch == '\t') {
            len += 2;
        } else if (ch < 0x20) {
            len += 6; // \u00XX
        } else {
            len += 1;
        }
    }
    return len;
}

pub export fn cbm_git_context_props_json(
    ctx_c: ?*const c.cbm_git_context_t,
    buf: ?[*]u8,
    buf_size: c_int,
) c_int {
    const ctx = ctx_c orelse return 0;
    const out_buf = buf orelse return 0;
    if (buf_size <= 0) return 0;

    var out = JsonOut{ .buf = out_buf, .size = @intCast(buf_size) };
    const ok = out.put("{") and
        out.boolField("is_git", ctx.is_git, true) and
        out.boolField("is_worktree", ctx.is_worktree, true) and
        out.boolField("is_detached", ctx.is_detached, true) and
        out.boolField("root_exists", ctx.root_exists, true) and
        out.stringField("canonical_root", ctx.canonical_root, true) and
        out.stringField("worktree_root", ctx.worktree_root, true) and
        out.stringField("git_common_dir", ctx.git_common_dir, true) and
        out.stringField("branch", ctx.branch, true) and
        out.stringField("head_sha", ctx.head_sha, true) and
        out.stringField("base_sha", ctx.base_sha, false) and
        out.put("}");
    if (!ok) {
        out_buf[0] = 0;
        return 0;
    }
    return @intCast(out.off);
}
