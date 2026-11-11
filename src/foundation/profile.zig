//! profile.zig — Activatable profiling implementation.
//!
//! Implements src/foundation/profile.h.

const c = @import("c");
const interop = @import("interop.zig");

const PROF_BUF_LEN = 32;
const PROF_NS_PER_US = 1000;
const PROF_US_PER_MS = 1000;
const PROF_US_PER_SEC = 1000000;

/// Runtime-active flag, read from C through the CBM_PROF_* macros.
pub export var cbm_profile_active: bool = false;

pub export fn cbm_profile_init() void {
    const env: [*c]const u8 = c.getenv("CBM_PROFILE");
    if (env != null and env[0] != 0 and env[0] != '0') cbm_profile_active = true;
}

pub export fn cbm_profile_enable() void {
    cbm_profile_active = true;
}

pub export fn cbm_profile_now(ts: *c.struct_timespec) void {
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, ts);
}

fn fmtLong(buf: *[PROF_BUF_LEN]u8, v: c_long) interop.Str {
    _ = interop.bprint(buf, "{d}", .{v});
    return @ptrCast(buf);
}

pub export fn cbm_profile_log_elapsed(phase: ?[*:0]const u8, sub: ?[*:0]const u8, start: *const c.struct_timespec, items: c_long) void {
    var now: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &now);
    const sec: c_long = @truncate(@as(i64, now.tv_sec - start.tv_sec));
    const nsec: c_long = @truncate(@as(i64, now.tv_nsec - start.tv_nsec));
    const us: c_long = sec *% PROF_US_PER_SEC +% @divTrunc(nsec, PROF_NS_PER_US);
    const ms: c_long = @divTrunc(us, PROF_US_PER_MS);

    var ms_buf: [PROF_BUF_LEN]u8 = undefined;
    var us_buf: [PROF_BUF_LEN]u8 = undefined;
    var items_buf: [PROF_BUF_LEN]u8 = undefined;
    const ms_s = fmtLong(&ms_buf, ms);
    const us_s = fmtLong(&us_buf, us);

    if (items > 0 and us > 0) {
        const rate: c_long = @intFromFloat(@as(f64, @floatFromInt(items)) * 1000000.0 / @as(f64, @floatFromInt(us)));
        var rate_buf: [PROF_BUF_LEN]u8 = undefined;
        interop.logInfo("prof", .{
            "phase", phase,                      "sub",        sub,                      "ms", ms_s, "us", us_s,
            "items", fmtLong(&items_buf, items), "rate_per_s", fmtLong(&rate_buf, rate),
        });
    } else if (items > 0) {
        interop.logInfo("prof", .{ "phase", phase, "sub", sub, "ms", ms_s, "us", us_s, "items", fmtLong(&items_buf, items) });
    } else {
        interop.logInfo("prof", .{ "phase", phase, "sub", sub, "ms", ms_s, "us", us_s });
    }
}
