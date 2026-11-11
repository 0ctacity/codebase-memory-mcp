//! limits.zig — Env-configurable safety limits (Stage 2 / Track B4).
//!
//! Implements src/foundation/limits.h.

const std = @import("std");
const c = @import("c");
const interop = @import("interop.zig");

pub export fn cbm_max_file_bytes() c_long {
    // 512 MiB — generous: real source files never approach it, but a
    // pathological / vendored blob degrades to a reported "oversized" skip
    // instead of a silent drop or an unbounded read.
    const default_cap: c_long = 512 * 1024 * 1024;
    const raw: [*c]const u8 = c.getenv("CBM_MAX_FILE_BYTES");
    if (raw != null and raw[0] != 0) {
        interop.setErrno(0);
        var end: [*c]u8 = null;
        const v = c.strtol(raw, &end, 10);
        if (interop.errno() == 0 and end != raw and end[0] == 0 and v > 0) return v;
        // Unparseable / non-positive → fall through to the safe default.
    }
    return default_cap;
}

/// Shared env-int parser: a positive integer in [1, INT_MAX], else the fallback.
/// Read fresh each call (see cbm_max_file_bytes rationale — cheap, test-friendly,
/// no stale memoized copy across runs).
fn envPositiveInt(name: [*:0]const u8, fallback: c_int) c_int {
    const raw: [*c]const u8 = c.getenv(name);
    if (raw != null and raw[0] != 0) {
        interop.setErrno(0);
        var end: [*c]u8 = null;
        const v = c.strtol(raw, &end, 10);
        if (interop.errno() == 0 and end != raw and end[0] == 0 and v > 0 and v <= std.math.maxInt(c_int)) {
            return @intCast(v);
        }
        // Unparseable / non-positive / out-of-range → safe default.
    }
    return fallback;
}

pub export fn cbm_cypher_max_depth() c_int {
    // 10 — generous for a code call/def graph; an explicit `*1..N` above this is
    // WARN-capped, never an unbounded (cyclic-graph DoS) traversal.
    return envPositiveInt("CBM_CYPHER_MAX_DEPTH", 10);
}

pub export fn cbm_mcp_max_depth() c_int {
    // 15 — ceiling for client-driven MCP graph traversals (trace_call_path,
    // detect_changes); the caller's `depth` is WARN-clamped to this.
    return envPositiveInt("CBM_MCP_MAX_DEPTH", 15);
}
