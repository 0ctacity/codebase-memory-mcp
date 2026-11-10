//! dump_verify.zig — Post-dump plausibility gate (#334).
//!
//! Implements src/foundation/dump_verify.h.

const c = @import("c");
const interop = @import("interop.zig");

pub export fn cbm_dump_verify_is_degraded(committed_nodes: c_int, persisted_nodes: c_int, ratio: f64, min_floor: c_int) bool {
    if (ratio <= 0.0) return false;
    if (committed_nodes < 0) return false;
    if (committed_nodes <= min_floor) return false;
    if (persisted_nodes < 0) return true;
    return @as(f64, @floatFromInt(persisted_nodes)) < @as(f64, @floatFromInt(committed_nodes)) * ratio;
}

pub export fn cbm_dump_verify_min_ratio() f64 {
    var buf: [32]u8 = undefined;
    if (c.cbm_safe_getenv("CBM_DUMP_VERIFY_MIN_RATIO", &buf, buf.len, null) != null) {
        var end: [*c]u8 = null;
        const r = c.strtod(&buf, &end);
        if (end != @as([*c]u8, &buf) and r >= 0.0 and r <= 1.0) return r;
        interop.logWarn("dump_verify.env.invalid", .{ "value", @as(interop.Str, @ptrCast(&buf)), "fallback", "0.5" });
    }
    return c.CBM_DUMP_VERIFY_DEFAULT_RATIO;
}
