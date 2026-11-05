//! progress_sink.zig — Human-readable progress for --progress CLI flag.
//!
//! Implements src/cli/progress_sink.h.
//!
//! Maps structured log events (msg=pass.timing, msg=pipeline.done, etc.)
//! to phase labels on stderr. When installed, replaces default log output.
//!
//! Thread-safe: fprintf has per-FILE* locking on POSIX.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const NOT_SET = -1;

var s_out: ?*c.FILE = null;
var s_needs_newline = std.atomic.Value(bool).init(false);
var s_gbuf_nodes: c_int = NOT_SET;
var s_gbuf_edges: c_int = NOT_SET;

/// Value of "key=VALUE" in a structured log line, copied into buf (at most
/// buf.len - 1 bytes) and NUL-terminated.
fn extractKv(line: []const u8, key: []const u8, buf: []u8) ?[:0]const u8 {
    var p: usize = 0;
    while (p < line.len) : (p += 1) {
        if ((p == 0 or line[p - 1] == ' ') and std.mem.startsWith(u8, line[p..], key) and
            p + key.len < line.len and line[p + key.len] == '=')
        {
            const val = line[p + key.len + 1 ..];
            var i: usize = 0;
            while (i < val.len and val[i] != ' ' and i < buf.len - 1) : (i += 1) buf[i] = val[i];
            buf[i] = 0;
            return buf[0..i :0];
        }
    }
    return null;
}

fn parseLong(s: []const u8) c_long {
    return c.strtol(s.ptr, null, 10);
}

pub export fn cbm_progress_sink_init(out: ?*c.FILE) void {
    s_out = if (out != null) out else interop.stderr();
    s_needs_newline.store(false, .seq_cst);
    s_gbuf_nodes = NOT_SET;
    s_gbuf_edges = NOT_SET;
    c.cbm_log_set_sink(cbm_progress_sink_fn);
}

pub export fn cbm_progress_sink_fini() void {
    if (s_needs_newline.load(.seq_cst) and s_out != null) {
        _ = c.fprintf(s_out, "\n");
        _ = c.fflush(s_out);
    }
    c.cbm_log_set_sink(null);
    s_out = null;
}

/// Phase label table: maps pass names to display labels.
const phases = [_]struct { pass: []const u8, label: [*:0]const u8 }{
    .{ .pass = "parallel_extract", .label = "[2/9] Extracting definitions" },
    .{ .pass = "registry_build", .label = "[3/9] Building registry" },
    .{ .pass = "parallel_resolve", .label = "[4/9] Resolving calls & edges" },
    .{ .pass = "tests", .label = "[5/9] Detecting tests" },
    .{ .pass = "httplinks", .label = "[6/9] Scanning HTTP links" },
    .{ .pass = "githistory_compute", .label = "[7/9] Analyzing git history" },
    .{ .pass = "configlink", .label = "[8/9] Linking config files" },
    .{ .pass = "dump", .label = "[9/9] Writing database" },
};

/// Flush pending \r line if needed.
fn flushCarriage() void {
    if (s_needs_newline.load(.seq_cst)) {
        _ = c.fprintf(s_out, "\n");
        s_needs_newline.store(false, .seq_cst);
    }
}

fn onDiscover(line: []const u8) void {
    var files: [32]u8 = undefined;
    if (extractKv(line, "files", &files)) |n| {
        _ = c.fprintf(s_out, "  Discovering files (%s found)\n", n.ptr);
    } else {
        _ = c.fprintf(s_out, "  Discovering files...\n");
    }
    _ = c.fflush(s_out);
}

fn onRoute(line: []const u8) void {
    var val: [32]u8 = undefined;
    const path = extractKv(line, "path", &val);
    if (path != null and std.mem.eql(u8, path.?, "incremental")) {
        _ = c.fprintf(s_out, "  Starting incremental index\n");
    } else {
        _ = c.fprintf(s_out, "  Starting full index\n");
    }
    _ = c.fflush(s_out);
}

fn onPassStart(line: []const u8) void {
    var val: [64]u8 = undefined;
    const pass = extractKv(line, "pass", &val) orelse return;
    if (std.mem.eql(u8, pass, "structure")) {
        _ = c.fprintf(s_out, "[1/9] Building file structure\n");
        _ = c.fflush(s_out);
    }
}

fn onPassTiming(line: []const u8) void {
    var val: [64]u8 = undefined;
    const pass = extractKv(line, "pass", &val) orelse return;
    flushCarriage();
    for (phases) |ph| {
        if (std.mem.eql(u8, pass, ph.pass)) {
            _ = c.fprintf(s_out, "%s\n", ph.label);
            _ = c.fflush(s_out);
            return;
        }
    }
}

/// gbuf.dump: capture node/edge counts.
fn onGbufDump(line: []const u8) void {
    var n: [32]u8 = undefined;
    var e: [32]u8 = undefined;
    if (extractKv(line, "nodes", &n)) |v| s_gbuf_nodes = @truncate(parseLong(v));
    if (extractKv(line, "edges", &e)) |v| s_gbuf_edges = @truncate(parseLong(v));
}

fn onDone(line: []const u8) void {
    flushCarriage();
    var ms: [32]u8 = undefined;
    const elapsed = extractKv(line, "elapsed_ms", &ms);
    const have_counts = s_gbuf_nodes >= 0 and s_gbuf_edges >= 0;
    if (have_counts and elapsed != null) {
        _ = c.fprintf(s_out, "Done: %d nodes, %d edges (%s ms)\n", s_gbuf_nodes, s_gbuf_edges, elapsed.?.ptr);
    } else if (have_counts) {
        _ = c.fprintf(s_out, "Done: %d nodes, %d edges\n", s_gbuf_nodes, s_gbuf_edges);
    } else {
        _ = c.fprintf(s_out, "Done.\n");
    }
    _ = c.fflush(s_out);
}

/// parallel.extract.progress: in-place counter.
fn onExtractProgress(line: []const u8) void {
    var done: [32]u8 = undefined;
    var total: [32]u8 = undefined;
    const ds = extractKv(line, "done", &done) orelse return;
    const ts = extractKv(line, "total", &total) orelse return;
    const d = parseLong(ds);
    const t = parseLong(ts);
    const pct: c_int = if (t > 0) @truncate(@divTrunc(d *% 100, t)) else 0;
    _ = c.fprintf(s_out, "\r  Extracting: %ld/%ld files (%d%%)", d, t, pct);
    _ = c.fflush(s_out);
    s_needs_newline.store(true, .seq_cst);
}

const handlers = [_]struct { msg: []const u8, handler: *const fn ([]const u8) void }{
    .{ .msg = "pipeline.discover", .handler = onDiscover },
    .{ .msg = "pipeline.route", .handler = onRoute },
    .{ .msg = "pass.start", .handler = onPassStart },
    .{ .msg = "pass.timing", .handler = onPassTiming },
    .{ .msg = "gbuf.dump", .handler = onGbufDump },
    .{ .msg = "pipeline.done", .handler = onDone },
    .{ .msg = "parallel.extract.progress", .handler = onExtractProgress },
};

pub export fn cbm_progress_sink_fn(line_c: [*c]const u8) void {
    if (line_c == null or s_out == null) return;
    const line = std.mem.span(line_c);
    var msg: [64]u8 = undefined;
    const m = extractKv(line, "msg", &msg) orelse return;
    for (handlers) |h| {
        if (std.mem.eql(u8, m, h.msg)) {
            h.handler(line);
            return;
        }
    }
}
