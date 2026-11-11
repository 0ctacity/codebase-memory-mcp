//! log.zig — Structured key-value logging to stderr.
//!
//! Implements src/foundation/log.h. The variadic cbm_log() itself lives in
//! c_shim.c: it walks the va_list and feeds each pair to the line
//! builder below (log_internal.h).

const std = @import("std");
const c = @import("c");
const interop = @import("interop.zig");

var g_log_level: c.CBMLogLevel = c.CBM_LOG_INFO;
var g_log_format: c.CBMLogFormat = c.CBM_LOG_FORMAT_TEXT;
var g_log_sink: c.cbm_log_sink_fn = null;
var g_log_sink_mode: c.CBMLogSinkMode = c.CBM_LOG_SINK_REPLACE;

/// Lower-cases `raw` into `lower` (NUL-terminated). Returns the text when it
/// fit completely, else null.
fn lowerSmall(lower: *[8]u8, raw: [*:0]const u8) ?[]const u8 {
    var i: usize = 0;
    while (i < lower.len - 1 and raw[i] != 0) : (i += 1) {
        lower[i] = std.ascii.toLower(raw[i]);
    }
    lower[i] = 0;
    return if (raw[i] == 0) lower[0..i] else null;
}

/// Applies CBM_LOG_LEVEL. Unrecognised values leave the level unchanged
/// (fail-open).
fn initLevelFromEnv() void {
    // getenv() is safe here: this runs at startup before any thread is created,
    // so there is no concurrent setenv() to race against.
    const raw: [*:0]const u8 = @ptrCast(c.getenv("CBM_LOG_LEVEL") orelse return);
    if (raw[0] == 0) return;

    // Textual form, case-insensitive. Index of each name == its enum value.
    const names = [_][]const u8{ "debug", "info", "warn", "error", "none" };
    var lower: [8]u8 = undefined;
    if (lowerSmall(&lower, raw)) |text| { // fully consumed — candidate textual match
        for (names, 0..) |name, lvl| {
            if (std.mem.eql(u8, text, name)) {
                cbm_log_set_level(@intCast(lvl));
                return;
            }
        }
    }

    // Numeric form: 0=debug .. 4=none, matching CBMLogLevel.
    var end: [*c]u8 = null;
    const n = c.strtol(raw, &end, 10);
    if (end != @as([*c]const u8, raw) and end[0] == 0 and n >= c.CBM_LOG_DEBUG and n <= c.CBM_LOG_NONE) {
        cbm_log_set_level(@intCast(n));
    }
}

/// CBM_LOG_LEVEL support — distilled from #414 (closes #413, thanks @santanusinha).
pub export fn cbm_log_init_from_env() void {
    initLevelFromEnv();

    const fmt: [*:0]const u8 = @ptrCast(c.getenv("CBM_LOG_FORMAT") orelse return);
    if (fmt[0] == 0) return;
    var lower_fmt: [8]u8 = undefined;
    if (lowerSmall(&lower_fmt, fmt)) |text| {
        if (std.mem.eql(u8, text, "json")) {
            cbm_log_set_format(c.CBM_LOG_FORMAT_JSON);
        } else if (std.mem.eql(u8, text, "text")) {
            cbm_log_set_format(c.CBM_LOG_FORMAT_TEXT);
        }
    }

    // Format is intentionally explicit-only. Logs stay local to stderr and the
    // optional in-process sink; deployment environment variables must not
    // silently change the operator-selected output shape.
}

pub export fn cbm_log_set_sink(sink: c.cbm_log_sink_fn) void {
    cbm_log_set_sink_ex(sink, c.CBM_LOG_SINK_REPLACE);
}

pub export fn cbm_log_set_sink_ex(sink: c.cbm_log_sink_fn, mode: c.CBMLogSinkMode) void {
    g_log_sink = sink;
    g_log_sink_mode = mode;
}

pub export fn cbm_log_set_level(level: c.CBMLogLevel) void {
    g_log_level = level;
}

pub export fn cbm_log_get_level() c.CBMLogLevel {
    return g_log_level;
}

pub export fn cbm_log_set_format(format: c.CBMLogFormat) void {
    g_log_format = format;
}

pub export fn cbm_log_get_format() c.CBMLogFormat {
    return g_log_format;
}

fn levelStr(level: c.CBMLogLevel) []const u8 {
    return switch (level) {
        c.CBM_LOG_DEBUG => "debug",
        c.CBM_LOG_INFO => "info",
        c.CBM_LOG_WARN => "warn",
        c.CBM_LOG_ERROR => "error",
        else => "unknown",
    };
}

const Line = c.cbm_log_line_t;

fn appendChar(line: *Line, ch: u8) void {
    if (line.pos < line.buf.len - 1) line.buf[line.pos] = ch;
    line.pos += 1;
}

fn appendRaw(line: *Line, s: []const u8) void {
    for (s) |ch| appendChar(line, ch);
}

fn appendTextAtom(line: *Line, s: []const u8) void {
    for (s) |ch| {
        appendChar(line, if (ch <= ' ' or ch == 0x7f) '_' else ch);
    }
}

fn appendJsonString(line: *Line, s: []const u8) void {
    appendChar(line, '"');
    for (s) |ch| {
        switch (ch) {
            '"' => appendRaw(line, "\\\""),
            '\\' => appendRaw(line, "\\\\"),
            0x08 => appendRaw(line, "\\b"),
            0x0c => appendRaw(line, "\\f"),
            '\n' => appendRaw(line, "\\n"),
            '\r' => appendRaw(line, "\\r"),
            '\t' => appendRaw(line, "\\t"),
            else => {
                if (ch < 0x20) {
                    const hex = "0123456789abcdef";
                    appendRaw(line, "\\u00");
                    appendChar(line, hex[ch >> 4]);
                    appendChar(line, hex[ch & 0xf]);
                } else {
                    appendChar(line, ch);
                }
            },
        }
    }
    appendChar(line, '"');
}

fn finishLine(line: *Line) void {
    if (line.pos >= line.buf.len) {
        line.buf[line.buf.len - 1] = 0;
    } else {
        line.buf[line.pos] = 0;
    }
}

fn emitLine(line: [*:0]const u8) void {
    if (g_log_sink) |sink| {
        sink(line);
        if (g_log_sink_mode == c.CBM_LOG_SINK_REPLACE) return;
    }
    _ = c.fprintf(interop.stderr(), "%s\n", line);
    // The Windows CRT fully buffers a redirected stderr; a crashed index worker
    // would otherwise leave an empty log.
    _ = c.fflush(interop.stderr());
}

fn spanOrEmpty(s: ?[*:0]const u8) []const u8 {
    return if (s) |p| std.mem.span(p) else "";
}

// ── Line builder (log_internal.h) ────────────────────────────────

pub export fn cbm_log_line_begin(line: *Line, level: c.CBMLogLevel, msg: ?[*:0]const u8) bool {
    if (level < g_log_level) return false;

    line.pos = 0;
    line.json = g_log_format == c.CBM_LOG_FORMAT_JSON;
    if (line.json) {
        appendRaw(line, "{\"level\":");
        appendJsonString(line, levelStr(level));
        appendRaw(line, ",\"event\":");
        appendJsonString(line, spanOrEmpty(msg));
    } else {
        appendRaw(line, "level=");
        appendTextAtom(line, levelStr(level));
        appendRaw(line, " msg=");
        appendTextAtom(line, spanOrEmpty(msg));
    }
    return true;
}

pub export fn cbm_log_line_pair(line: *Line, key: ?[*:0]const u8, val: ?[*:0]const u8) void {
    if (line.json) {
        appendChar(line, ',');
        appendJsonString(line, spanOrEmpty(key));
        appendChar(line, ':');
        appendJsonString(line, spanOrEmpty(val));
    } else {
        appendChar(line, ' ');
        appendTextAtom(line, spanOrEmpty(key));
        appendChar(line, '=');
        appendTextAtom(line, spanOrEmpty(val));
    }
}

pub export fn cbm_log_line_end(line: *Line) void {
    if (line.json) appendChar(line, '}');
    finishLine(line);
    emitLine(@ptrCast(&line.buf));
}

/// cbm_log() for the fixed key/value lists of this file.
fn logPairs(level: c.CBMLogLevel, msg: [*:0]const u8, pairs: []const [2]?[*:0]const u8) void {
    var line: Line = undefined;
    if (!cbm_log_line_begin(&line, level, msg)) return;
    for (pairs) |pair| cbm_log_line_pair(&line, pair[0], pair[1]);
    cbm_log_line_end(&line);
}

fn fmtInt(buf: []u8, value: anytype) [*:0]const u8 {
    _ = interop.bprint(buf, "{d}", .{value});
    return @ptrCast(buf.ptr);
}

pub export fn cbm_log_int(level: c.CBMLogLevel, msg: ?[*:0]const u8, key: ?[*:0]const u8, value: i64) void {
    var value_buf: [32]u8 = undefined;
    var line: Line = undefined;
    if (!cbm_log_line_begin(&line, level, msg)) return;
    cbm_log_line_pair(&line, key orelse "?", fmtInt(&value_buf, value));
    cbm_log_line_end(&line);
}

const ITOA_RING = 16;
threadlocal var itoa_bufs: [ITOA_RING][32]u8 = undefined;
threadlocal var itoa_idx: usize = 0;

pub export fn cbm_log_itoa(value: c_int) [*:0]const u8 {
    const out = &itoa_bufs[itoa_idx];
    itoa_idx = (itoa_idx + 1) % ITOA_RING;
    return fmtInt(out, value);
}

fn copyPathWithoutQuery(path: ?[*:0]const u8, out: []u8) void {
    out[0] = 0;
    const p = path orelse return;
    var n: usize = 0;
    while (p[n] != 0 and p[n] != '?' and p[n] != '#' and n < out.len - 1) : (n += 1) {
        out[n] = p[n];
    }
    out[n] = 0;
}

pub export fn cbm_log_mcp_request(method: ?[*:0]const u8, tool_name: ?[*:0]const u8, is_error: bool, duration_us: i64) void {
    var duration_buf: [32]u8 = undefined;
    const duration_ms = fmtInt(&duration_buf, @divTrunc(duration_us, 1000));
    const level: c.CBMLogLevel = if (is_error) c.CBM_LOG_WARN else c.CBM_LOG_INFO;
    const status: [*:0]const u8 = if (is_error) "error" else "ok";
    if (tool_name != null and tool_name.?[0] != 0) {
        logPairs(level, "mcp.request", &.{
            .{ "protocol", "jsonrpc" },
            .{ "method", method orelse "" },
            .{ "tool", tool_name },
            .{ "status", status },
            .{ "duration_ms", duration_ms },
        });
    } else {
        logPairs(level, "mcp.request", &.{
            .{ "protocol", "jsonrpc" },
            .{ "method", method orelse "" },
            .{ "status", status },
            .{ "duration_ms", duration_ms },
        });
    }
}

pub export fn cbm_log_http_request(
    component: ?[*:0]const u8,
    method: ?[*:0]const u8,
    path: ?[*:0]const u8,
    status: c_int,
    duration_ms: i64,
    request_bytes: usize,
    response_bytes: usize,
) void {
    var safe_path: [1024]u8 = undefined;
    var status_buf: [16]u8 = undefined;
    var duration_buf: [32]u8 = undefined;
    var request_buf: [32]u8 = undefined;
    var response_buf: [32]u8 = undefined;
    copyPathWithoutQuery(path, &safe_path);

    var level: c.CBMLogLevel = c.CBM_LOG_INFO;
    if (status >= 500) {
        level = c.CBM_LOG_ERROR;
    } else if (status >= 400) {
        level = c.CBM_LOG_WARN;
    }

    logPairs(level, "http.request", &.{
        .{ "component", component orelse "" },
        .{ "method", method orelse "" },
        .{ "path", @ptrCast(&safe_path) },
        .{ "status", fmtInt(&status_buf, status) },
        .{ "duration_ms", fmtInt(&duration_buf, duration_ms) },
        .{ "request_bytes", fmtInt(&request_buf, request_bytes) },
        .{ "response_bytes", fmtInt(&response_buf, response_bytes) },
    });
}
