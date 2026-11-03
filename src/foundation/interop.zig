//! interop.zig — C-interop helpers shared by the Zig modules: logging through
//! cbm_log, snprintf-style formatting, malloc'd allocations and C strings.

const std = @import("std");
const c = @import("c");

pub const Str = [*:0]const u8;
pub const CStr = [*c]const u8;

// ── Logging ────────────────────────────────────────────────────────

/// cbm_log with the NULL key terminator the C convenience macros append.
pub fn log(level: c.CBMLogLevel, msg: Str, kvs: anytype) void {
    @call(.auto, c.cbm_log, .{ level, msg } ++ kvs ++ .{@as(?Str, null)});
}

pub fn logInfo(msg: Str, kvs: anytype) void {
    log(c.CBM_LOG_INFO, msg, kvs);
}

pub fn logWarn(msg: Str, kvs: anytype) void {
    log(c.CBM_LOG_WARN, msg, kvs);
}

// ── Formatting ─────────────────────────────────────────────────────

/// snprintf into buf: writes what fits plus a NUL and returns the length the
/// whole output would have had.
pub fn bprint(buf: []u8, comptime fmt: []const u8, args: anytype) usize {
    if (buf.len == 0) return std.fmt.count(fmt, args);
    var w: std.Io.Writer = .fixed(buf[0 .. buf.len - 1]);
    w.print(fmt, args) catch {};
    buf[w.end] = 0;
    return std.fmt.count(fmt, args);
}

// ── Allocation ─────────────────────────────────────────────────────

pub fn mallocN(comptime T: type, n: usize) [*c]T {
    return @ptrCast(@alignCast(c.malloc(n * @sizeOf(T))));
}

pub fn callocN(comptime T: type, n: usize) [*c]T {
    return @ptrCast(@alignCast(c.calloc(n, @sizeOf(T))));
}

pub fn reallocN(comptime T: type, p: [*c]T, n: usize) [*c]T {
    return @ptrCast(@alignCast(c.realloc(@ptrCast(p), n * @sizeOf(T))));
}

/// safe_realloc from foundation/platform.h: frees the old block on failure.
pub fn safeReallocN(comptime T: type, p: [*c]T, n: usize) [*c]T {
    const size = if (n * @sizeOf(T) == 0) 1 else n * @sizeOf(T);
    const tmp = c.realloc(@ptrCast(p), size);
    if (tmp == null) c.free(@ptrCast(p));
    return @ptrCast(@alignCast(tmp));
}

/// safe_str_free from foundation/platform.h.
pub fn strFree(slot: *CStr) void {
    if (slot.* != null) {
        c.free(@ptrCast(@constCast(slot.*)));
        slot.* = null;
    }
}

// ── C strings ──────────────────────────────────────────────────────

/// A malloc'd copy of s; NULL for NULL.
pub const heapStrdup = c.cbm_heap_strdup;

/// The C string s as a slice; NULL reads as "(null)", the way printf's %s
/// prints it.
pub fn sp(s: CStr) []const u8 {
    if (s == null) return "(null)";
    return std.mem.span(s);
}

/// ASCII case-insensitive string equality.
pub fn ciEq(a: CStr, b: CStr) bool {
    if (a == null or b == null) return false;
    return std.ascii.eqlIgnoreCase(std.mem.span(a), std.mem.span(b));
}
