//! hook_augment.zig — `codebase-memory-mcp hook-augment`
//!
//! Implements cbm_cmd_hook_augment and cbm_hook_path_is_abs from
//! src/cli/cli.h.
//!
//! A non-blocking Claude Code PreToolUse augmenter. Reads the hook JSON from
//! stdin, and for Grep/Glob calls injects matching graph symbols as
//! `additionalContext` so the agent gets structured context alongside its
//! normal search results. SessionStart and SubagentStart payloads get the
//! codebase-memory-mcp usage reminder instead.
//!
//! Cardinal rule: this NEVER blocks a tool call. Every error, timeout, missing
//! project, or short/odd pattern path results in `exit 0` with NO stdout
//! output (a clean pass-through). This is what makes issue #362 structurally
//! impossible to recur — the hook cannot deny a tool.
//!
//! The underlying query is `search_graph` (pure SQLite, shell-free) — chosen
//! over `search_code` (which shells out to grep|xargs) so the hook stays cheap
//! enough to run before every Grep/Glob.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const is_windows = builtin.os.tag == .windows;

const STDIN_CAP = 256 * 1024; // hook payloads are tiny; cap defensively
const MIN_TOKEN = 4; // skip short/noisy patterns before any work
const MAX_TOKEN = 96;
const RESULT_LIMIT = 5;
const MAX_WALKUP = 8; // cwd may be a subdir of the indexed root
const DEADLINE_MS = 300; // hard in-process budget (see also: the settings.json "timeout" backstop)
const CONTEXT_CAP = 4096;
const CONTEXT_FILL_LIMIT = 3900;

/// Lifecycle reminders, kept in sync with the cbm-session-reminder and
/// cbm-subagent-reminder scripts in cli.zig. SessionStart injects plain stdout
/// as context; SubagentStart injects only JSON additionalContext.
const SESSION_REMINDER =
    "CRITICAL - Code Discovery Protocol:\n" ++
    "1. ALWAYS use codebase-memory-mcp tools FIRST for ANY code exploration:\n" ++
    "   - search_graph(name_pattern/label/qn_pattern) to find functions/classes/routes\n" ++
    "   - trace_path(function_name, mode=calls|data_flow|cross_service) for call chains\n" ++
    "   - get_code_snippet(qualified_name) for exact symbol source (precise ranges)\n" ++
    "   - query_graph(query) for complex Cypher patterns\n" ++
    "   - get_architecture(aspects) for project structure\n" ++
    "   - search_code(pattern) for text search (graph-augmented grep)\n" ++
    "2. Use Grep/Glob/Read freely for text, configs, non-code files, and\n" ++
    "   always Read a file before editing it.\n" ++
    "3. If a project is not indexed yet, run index_repository FIRST.\n";
const SUBAGENT_REMINDER =
    "{\"hookSpecificOutput\":{\"hookEventName\":\"SubagentStart\"," ++
    "\"additionalContext\":\"Code discovery: prefer codebase-memory-mcp tools " ++
    "(search_graph, trace_path, get_code_snippet, query_graph, get_architecture, " ++
    "search_code) over grep/file-read for navigating code. Use Grep/Glob/Read for " ++
    "text, configs, and non-code files.\"}}\n";

// ── Hard deadline ──────────────────────────────────────────────────
// A slow SQLite open or query must never stall the agent. When the timer
// fires we _exit(0) immediately. Output is written exactly once at the very
// end, so firing mid-work simply yields a clean no-op (no partial JSON).

fn deadlineExit(_: std.posix.SIG) callconv(.c) void {
    std.c._exit(0);
}

fn armDeadline() void {
    if (is_windows) return; // Windows: rely on settings.json timeout
    const sa: std.posix.Sigaction = .{
        .handler = .{ .handler = deadlineExit },
        .mask = std.posix.sigemptyset(),
        .flags = 0,
    };
    std.posix.sigaction(.ALRM, &sa, null);

    var it = std.mem.zeroes(c.struct_itimerval);
    it.it_value.tv_sec = DEADLINE_MS / 1000;
    it.it_value.tv_usec = (DEADLINE_MS % 1000) * 1000;
    _ = c.setitimer(c.ITIMER_REAL, &it, null);
}

// ── stdin ──────────────────────────────────────────────────────────

fn readStdin() ?[*:0]u8 {
    const buf = interop.mallocN(u8, STDIN_CAP + 1) orelse return null;
    var total: usize = 0;
    while (total < STDIN_CAP) {
        const n = c.fread(buf + total, 1, STDIN_CAP - total, interop.stdin());
        if (n == 0) break;
        total += n;
    }
    buf[total] = 0;
    return @ptrCast(buf);
}

// ── pattern → token ────────────────────────────────────────────────
// Extract the longest identifier-like run ([A-Za-z_][A-Za-z0-9_]*) of at
// least MIN_TOKEN chars. Pure-identifier output means it is always safe to
// embed in a regex (name_pattern) with no escaping. Returns null when the
// pattern has no usable token (path globs, short/regex-only patterns) — the
// caller then no-ops, which keeps the common cheap case cheap.

fn isIdentChar(ch: u8) bool {
    return std.ascii.isAlphanumeric(ch) or ch == '_';
}

fn extractToken(pattern: ?[*:0]const u8, out: []u8) ?[:0]const u8 {
    const p = std.mem.span(pattern orelse return null);
    var best_start: usize = 0;
    var best_len: usize = 0;
    var i: usize = 0;
    while (i < p.len) {
        if (std.ascii.isAlphabetic(p[i]) or p[i] == '_') {
            const start = i;
            while (i < p.len and isIdentChar(p[i])) i += 1;
            if (i - start > best_len) {
                best_len = i - start;
                best_start = start;
            }
        } else {
            i += 1;
        }
    }
    if (best_len < MIN_TOKEN) return null;
    best_len = @min(best_len, MAX_TOKEN, out.len - 1);
    @memcpy(out[0..best_len], p[best_start..][0..best_len]);
    out[best_len] = 0;
    return out[0..best_len :0];
}

// ── JSON helpers ───────────────────────────────────────────────────

fn objStr(obj: ?*c.yyjson_val, key: [*:0]const u8) ?[*:0]const u8 {
    const v = if (obj != null) c.yyjson_obj_get(obj, key) else null;
    return if (v != null and c.yyjson_is_str(v)) c.yyjson_get_str(v) else null;
}

fn nonEmpty(s: ?[*:0]const u8) bool {
    return s != null and s.?[0] != 0;
}

/// The search_graph args JSON: {"project":..,"name_pattern":".*tok.*",
/// "limit":N}. `token` is a pure identifier so regex embedding is safe.
/// Caller frees.
fn buildArgs(project: [*:0]const u8, token: [:0]const u8) [*c]u8 {
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    var name_pattern: [MAX_TOKEN + 8]u8 = undefined;
    _ = interop.bprint(&name_pattern, ".*{s}.*", .{token});

    _ = c.yyjson_mut_obj_add_str(doc, root, "project", project);
    _ = c.yyjson_mut_obj_add_str(doc, root, "name_pattern", &name_pattern);
    _ = c.yyjson_mut_obj_add_int(doc, root, "limit", RESULT_LIMIT);

    const out = c.yyjson_mut_write(doc, 0, null);
    c.yyjson_mut_doc_free(doc);
    return out;
}

/// Parse the MCP envelope returned by cbm_mcp_handle_tool and, if it is a
/// successful search_graph result with >=1 hit, format a compact
/// additionalContext string. Returns malloc'd text or null.
///
/// is_error is set when the envelope is an MCP error (e.g. project not
/// indexed) so the caller can try a parent directory.
fn formatContext(envelope: [*:0]const u8, token: [:0]const u8, is_error: *bool) ?[*:0]u8 {
    is_error.* = false;
    const edoc = c.yyjson_read(envelope, std.mem.len(envelope), 0) orelse return null;
    defer c.yyjson_doc_free(edoc);
    const eroot = c.yyjson_doc_get_root(edoc);
    const err = c.yyjson_obj_get(eroot, "isError");
    if (err != null and c.yyjson_is_true(err)) {
        is_error.* = true;
        return null;
    }
    const content = c.yyjson_obj_get(eroot, "content");
    const item0 = if (content != null and c.yyjson_is_arr(content)) c.yyjson_arr_get(content, 0) else null;
    const inner = objStr(item0, "text") orelse return null;

    const idoc = c.yyjson_read(inner, std.mem.len(inner), 0) orelse return null;
    defer c.yyjson_doc_free(idoc);
    const results = c.yyjson_obj_get(c.yyjson_doc_get_root(idoc), "results");
    const nres: usize = if (results != null and c.yyjson_is_arr(results)) c.yyjson_arr_size(results) else 0;
    if (nres == 0) return null; // valid project, just no matching symbols

    const text = interop.mallocN(u8, CONTEXT_CAP) orelse return null;
    var off: c_int = c.snprintf(text, CONTEXT_CAP,
        \\[codebase-memory] %zu graph symbol(s) match "%s" (structured context; your search results below are unaffected):
    , nres, token.ptr);
    var iter: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(results, &iter);
    while (true) {
        const r = c.yyjson_arr_iter_next(&iter);
        if (r == null) break;
        if (off < 0 or off >= CONTEXT_FILL_LIMIT) break;
        const qn = objStr(r, "qualified_name");
        const nm = objStr(r, "name");
        const fp = objStr(r, "file_path");
        const lb = objStr(r, "label");
        const disp: [*:0]const u8 = if (nonEmpty(qn)) qn.? else (nm orelse "");
        const file: [*:0]const u8 = fp orelse "";
        const sep: [*:0]const u8 = if (nonEmpty(lb)) "  " else "";
        const label: [*:0]const u8 = if (nonEmpty(lb)) lb.? else "";
        off += c.snprintf(text + @as(usize, @intCast(off)), @intCast(CONTEXT_CAP - off), "\n- %s  %s%s%s", disp, file, sep, label);
    }
    return @ptrCast(text);
}

/// Emit the PreToolUse additionalContext payload to stdout (exactly once).
fn emit(text: [*:0]const u8) void {
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    const hso = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_str(doc, hso, "hookEventName", "PreToolUse");
    _ = c.yyjson_mut_obj_add_str(doc, hso, "additionalContext", text);
    _ = c.yyjson_mut_obj_add_val(doc, root, "hookSpecificOutput", hso);

    const json = c.yyjson_mut_write(doc, 0, null);
    if (json != null) {
        _ = c.fputs(json, interop.stdout());
        c.free(json);
    }
    c.yyjson_mut_doc_free(doc);
}

/// True for an absolute path we can walk up: POSIX "/..." or a Windows drive
/// root — "X:/..." or a bare "X:" (callers normalize '\\' to '/' first).
/// Declared in cli.h so the Windows drive-letter handling (#618) has direct
/// regression coverage.
pub export fn cbm_hook_path_is_abs(d: [*c]const u8) bool {
    if (d == null or d[0] == 0) return false;
    if (d[0] == '/') return true;
    return std.ascii.isAlphabetic(d[0]) and d[1] == ':' and (d[2] == '/' or d[2] == 0);
}

/// Walk up from `start`, deriving a project name at each level and querying
/// search_graph until an indexed project is found (or the walk is exhausted).
/// Stops at the first non-error result: a valid project with zero hits is a
/// legitimate "no match" and must NOT cause a parent-directory probe.
fn resolveAndQuery(srv: *c.cbm_mcp_server_t, start: [*:0]const u8, token: [:0]const u8) ?[*:0]u8 {
    var dir: [4096]u8 = undefined;
    _ = interop.bprint(&dir, "{s}", .{std.mem.span(start)});

    var level: usize = 0;
    while (level < MAX_WALKUP and cbm_hook_path_is_abs(&dir)) : (level += 1) {
        if (c.cbm_project_name_from_path(&dir)) |project| {
            const args = buildArgs(project, token);
            c.free(project);
            if (args != null) {
                const res = c.cbm_mcp_handle_tool(srv, "search_graph", args);
                c.free(args);
                if (res != null) {
                    var is_error = false;
                    const ctx = formatContext(res, token, &is_error);
                    c.free(res);
                    if (ctx != null) return ctx; // hits → done
                    if (!is_error) return null; // valid project, no hits → stop
                }
            }
        }
        // Not indexed at this level — climb to the parent.
        const cur = std.mem.sliceTo(&dir, 0);
        const slash = std.mem.lastIndexOfScalar(u8, cur, '/') orelse break;
        if (slash == 0) break; // POSIX root "/"
        if (slash == 2 and dir[1] == ':') break; // Windows drive root "X:/" — don't strip to "X:"
        dir[slash] = 0;
    }
    return null;
}

pub export fn cbm_cmd_hook_augment() c_int {
    armDeadline();

    const input = readStdin() orelse return 0;
    defer c.free(input);
    const doc = c.yyjson_read(input, std.mem.len(input), 0) orelse return 0;
    defer c.yyjson_doc_free(doc);
    const root = c.yyjson_doc_get_root(doc);

    // Claude registers the SessionStart/SubagentStart reminders as this same
    // shell-free exec-form command, so answer them from the payload's event.
    if (objStr(root, "hook_event_name")) |event_c| {
        const event = std.mem.span(event_c);
        const reminder: ?[*:0]const u8 = if (std.mem.eql(u8, event, "SessionStart"))
            SESSION_REMINDER
        else if (std.mem.eql(u8, event, "SubagentStart"))
            SUBAGENT_REMINDER
        else
            null;
        if (reminder) |r| {
            _ = c.fputs(r, interop.stdout());
            _ = c.fflush(interop.stdout());
            return 0;
        }
    }

    const tool = std.mem.span(objStr(root, "tool_name") orelse return 0);
    if (!std.mem.eql(u8, tool, "Grep") and !std.mem.eql(u8, tool, "Glob")) return 0;

    const pattern = objStr(c.yyjson_obj_get(root, "tool_input"), "pattern");
    var token_buf: [MAX_TOKEN + 1]u8 = undefined;
    const token = extractToken(pattern, &token_buf) orelse return 0;

    var cwd = objStr(root, "cwd");
    var cwdbuf: [4096]u8 = undefined;
    if (!is_windows) {
        if (cwd == null or !cbm_hook_path_is_abs(cwd.?)) {
            if (c.getcwd(&cwdbuf, cwdbuf.len) == null) return 0;
            cwd = @ptrCast(&cwdbuf);
        }
    } else {
        // Windows: Claude Code passes an absolute drive-letter cwd in the hook
        // payload (e.g. C:\repo). Normalize '\\' -> '/' and require an
        // absolute path; the walk-up loop handles POSIX and "X:/..." roots
        // alike. Without a usable cwd there is nothing to augment — fail open
        // cleanly.
        if (cwd) |raw| {
            const n = interop.bprint(&cwdbuf, "{s}", .{std.mem.span(raw)});
            std.mem.replaceScalar(u8, cwdbuf[0..@min(n, cwdbuf.len - 1)], '\\', '/');
            cwd = @ptrCast(&cwdbuf);
        }
        if (cwd == null or !cbm_hook_path_is_abs(cwd.?)) return 0;
    }

    const srv = c.cbm_mcp_server_new() orelse return 0;
    defer c.cbm_mcp_server_free(srv);

    if (resolveAndQuery(srv, cwd.?, token)) |ctx| {
        emit(ctx);
        c.free(ctx);
    }
    return 0;
}
