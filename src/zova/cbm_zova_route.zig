//! cbm_zova_route.zig — Location of the user-wide cbm.zova database.
//!
//! Implements src/zova/cbm_zova_route.h.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

pub export fn cbm_zova_user_database_path(out: [*c]u8, out_size: usize) c_int {
    const cache = c.cbm_resolve_cache_dir();
    if (out == null or out_size == 0 or cache == null or cache[0] == 0) return -1;
    const n = interop.bprint(out[0..out_size], "{s}/cbm.zova", .{std.mem.span(cache)});
    return if (n > 0 and n < out_size) 0 else -1;
}
