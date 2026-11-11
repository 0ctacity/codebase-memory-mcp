//! compat.zig — Implementations for the compat.h shims.
//!
//! On POSIX most of compat.h maps to the standard library through macros; the
//! `windows` namespace holds the functions Windows lacks. The static inline
//! helpers of compat.h stay in the header.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");

const is_windows = builtin.os.tag == .windows;

pub const windows = if (is_windows) struct {
    // ── strndup (Windows lacks it) ───────────────────────────────

    pub export fn cbm_strndup(s: ?[*]const u8, n: usize) ?[*:0]u8 {
        const src = s orelse return null;
        var len: usize = 0;
        while (len < n and src[len] != 0) len += 1;
        const d: [*]u8 = @ptrCast(c.malloc(len + 1) orelse return null);
        @memcpy(d[0..len], src[0..len]);
        d[len] = 0;
        return @ptrCast(d);
    }

    // ── strcasestr (Windows lacks it) ────────────────────────────

    pub export fn cbm_strcasestr(haystack: [*:0]const u8, needle: [*:0]const u8) ?[*:0]u8 {
        if (needle[0] == 0) return @constCast(haystack);
        const nlen = std.mem.len(needle);
        var h = haystack;
        while (h[0] != 0) : (h += 1) {
            if (c._strnicmp(h, needle, nlen) == 0) return @constCast(h);
        }
        return null;
    }

    // ── mkdtemp / mkstemp (Windows lacks them) ───────────────────

    /// Rewrites a "/tmp/" template to %TEMP%\ in `buf`.
    fn tempTemplate(buf: *[512]u8, tmpl: [*:0]const u8) void {
        if (c.strncmp(tmpl, "/tmp/", 5) == 0) {
            var tmp: [*c]const u8 = c.getenv("TEMP");
            if (tmp == null) tmp = c.getenv("TMP");
            if (tmp == null) tmp = ".";
            _ = c.snprintf(buf, buf.len, "%s\\%s", tmp, tmpl + 5);
        } else {
            _ = c.snprintf(buf, buf.len, "%s", tmpl);
        }
    }

    var mkdtemp_buf: [512]u8 = undefined;

    pub export fn cbm_mkdtemp(tmpl: [*:0]u8) ?[*:0]u8 {
        // Build path in static buffer, then copy back to caller.
        // Callers must provide buffers >= 256 bytes (all test code does).
        const buf = &mkdtemp_buf;
        tempTemplate(buf, tmpl);
        if (c._mktemp(buf) == null) return null;
        if (c._mkdir(buf) != 0) return null;
        // Normalize to forward slashes. Callers embed this path in JSON repo_path
        // (where "\t"/"\a" are invalid escapes → index fails) and pass it to git -C.
        // Windows file APIs accept forward slashes, so the created dir is unaffected.
        for (buf) |*ch| {
            if (ch.* == 0) break;
            if (ch.* == '\\') ch.* = '/';
        }
        // Copy result back — callers now use char[256]+ buffers
        _ = c.strcpy(tmpl, buf);
        return tmpl;
    }

    var mkstemp_buf: [512]u8 = undefined;

    pub export fn cbm_mkstemp(tmpl: [*:0]u8) c_int {
        // Rewrite /tmp/ to %TEMP%\ like cbm_mkdtemp
        const buf = &mkstemp_buf;
        tempTemplate(buf, tmpl);
        if (c._mktemp(buf) == null) return c.CBM_NOT_FOUND;
        const fd = c._open(buf, c._O_CREAT | c._O_RDWR | c._O_BINARY, @as(c_int, c._S_IREAD | c._S_IWRITE));
        if (fd >= 0) _ = c.strcpy(tmpl, buf);
        return fd;
    }

    // ── clock_gettime (Windows lacks it) ─────────────────────────

    pub export fn cbm_clock_gettime(clk_id: c_int, tp: *c.struct_timespec) c_int {
        _ = clk_id;
        var freq: c.LARGE_INTEGER = undefined;
        var count: c.LARGE_INTEGER = undefined;
        _ = c.QueryPerformanceFrequency(&freq);
        _ = c.QueryPerformanceCounter(&count);
        tp.tv_sec = @intCast(@divTrunc(count.QuadPart, freq.QuadPart));
        tp.tv_nsec = @truncate(@divTrunc(@rem(count.QuadPart, freq.QuadPart) *% 1000000000, freq.QuadPart));
        return 0;
    }

    // ── getline (Windows lacks it) ───────────────────────────────

    pub export fn cbm_getline(lineptr: ?*[*c]u8, n: ?*usize, stream: ?*c.FILE) isize {
        const line = lineptr orelse return c.CBM_NOT_FOUND;
        const cap = n orelse return c.CBM_NOT_FOUND;
        if (stream == null) return c.CBM_NOT_FOUND;
        if (line.* == null or cap.* == 0) {
            cap.* = 128;
            line.* = @ptrCast(c.malloc(cap.*));
            if (line.* == null) return c.CBM_NOT_FOUND;
        }
        var pos: usize = 0;
        var ch: c_int = undefined;
        while (true) {
            ch = c.fgetc(stream);
            if (ch == c.EOF) break;
            if (pos + 1 >= cap.*) {
                const new_n = cap.* *% 2;
                const tmp: [*c]u8 = @ptrCast(c.realloc(line.*, new_n));
                if (tmp == null) return c.CBM_NOT_FOUND;
                line.* = tmp;
                cap.* = new_n;
            }
            line.*[pos] = @truncate(@as(c_uint, @bitCast(ch)));
            pos += 1;
            if (ch == '\n') break;
        }
        if (pos == 0 and ch == c.EOF) return c.CBM_NOT_FOUND;
        line.*[pos] = 0;
        return @intCast(pos);
    }
} else struct {};

pub export fn cbm_heap_strdup(s: ?[*:0]const u8) ?[*:0]u8 {
    const src = s orelse return null;
    const len = std.mem.len(src);
    const d: [*]u8 = @ptrCast(c.malloc(len + 1) orelse return null);
    @memcpy(d[0 .. len + 1], src[0 .. len + 1]);
    return @ptrCast(d);
}

pub export fn cbm_elapsed_ms(start: c.struct_timespec) f64 {
    var now: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &now);
    const s: f64 = @floatFromInt(now.tv_sec - start.tv_sec);
    const ns: f64 = @floatFromInt(now.tv_nsec - start.tv_nsec);
    return (s * 1000.0) + (ns / 1000000.0);
}

// ── Per-thread CPU time ──────────────────────────────────────────

pub export fn cbm_thread_cpu_time_ns() u64 {
    if (is_windows) {
        var creation: c.FILETIME = undefined;
        var exit_time: c.FILETIME = undefined;
        var kernel: c.FILETIME = undefined;
        var user: c.FILETIME = undefined;
        if (c.GetThreadTimes(c.GetCurrentThread(), &creation, &exit_time, &kernel, &user) == 0) return 0;
        // Each FILETIME is a 64-bit count of 100-ns ticks split across two DWORDs.
        const kernel_ticks = (@as(u64, kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        const user_ticks = (@as(u64, user.dwHighDateTime) << 32) | user.dwLowDateTime;
        return (kernel_ticks +% user_ticks) *% 100;
    } else {
        var ts: c.struct_timespec = undefined;
        if (c.clock_gettime(c.CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
        const sec: u64 = @bitCast(@as(i64, ts.tv_sec));
        const nsec: u64 = @bitCast(@as(i64, ts.tv_nsec));
        return (sec *% 1000000000) +% nsec;
    }
}
