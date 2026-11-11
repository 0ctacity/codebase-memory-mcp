//! rotsq.zig — see rotsq.h. From-paper implementation, no dependencies.

const std = @import("std");
const c = @import("c");
const fp = @import("fp.zig");

const XxHash3 = std.hash.XxHash3;

const IN_DIM: comptime_int = c.CBM_RSQ_IN_DIM;
const DIM: comptime_int = c.CBM_RSQ_DIM;
const LEVELS: comptime_int = c.CBM_RSQ_LEVELS;

// ── Deterministic ±1 diagonal ──────────────────────────────────────

/// The C code generated this table once at first use. It is a pure function
/// of the index, so it is computed at compile time here.
const rsq_diag: [DIM]f32 = blk: {
    @setEvalBranchQuota(4_000_000);
    var diag: [DIM]f32 = undefined;
    for (&diag, 0..) |*v, d| {
        const h = XxHash3.hash(0x5bd1e995, &std.mem.toBytes(@as(c_int, @intCast(d))));
        v.* = if (h & 1 != 0) 1.0 else -1.0;
    }
    break :blk diag;
};

// ── Fast Walsh–Hadamard Transform (in place, unnormalized) ─────────

fn rsqFwht(v: *[DIM]f32) void {
    var len: usize = 1;
    while (len < DIM) : (len <<= 1) {
        var i: usize = 0;
        while (i < DIM) : (i += len << 1) {
            for (i..i + len) |j| {
                const a = v[j];
                const b = v[j + len];
                v[j] = a + b;
                v[j + len] = a - b;
            }
        }
    }
}

// ── Encode ─────────────────────────────────────────────────────────

pub export fn cbm_rsq_encode(v: [*]const f32, out: *c.cbm_rsq_code_t) void {
    var rot: [DIM]f32 = undefined;
    for (rot[0..IN_DIM], v[0..IN_DIM], rsq_diag[0..IN_DIM]) |*r, x, sign| {
        r.* = x * sign;
    }
    @memset(rot[IN_DIM..], 0.0);
    rsqFwht(&rot);
    // Normalize the transform so the rotation is orthonormal (H/√D): keeps
    // the coordinates in a data-independent range and makes the estimated IP
    // directly comparable to the pre-rotation IP.
    const inv_sqrt_d: f32 = 1.0 / 32.0; // 1/√1024
    var lo: f32 = rot[0] * inv_sqrt_d;
    var hi: f32 = lo;
    for (&rot) |*r| {
        r.* *= inv_sqrt_d;
        if (r.* < lo) lo = r.*;
        if (r.* > hi) hi = r.*;
    }

    // Per-vector scalar quantization over [lo, hi] at CBM_RSQ_BITS.
    const range = hi - lo;
    const step: f32 = if (range > 0.0) range / @as(f32, LEVELS) else 1.0;
    out.offset = lo;
    out.scale = step;

    var sum: i32 = 0;
    @memset(&out.codes, 0);
    for (rot, 0..) |r, d| {
        const q = (r - lo) / step;
        var code: i32 = @intFromFloat(q + 0.5);
        if (code < 0) code = 0;
        if (code > LEVELS) code = LEVELS;
        sum += code;
        const nibble: u8 = @intCast(code);
        if (d & 1 != 0) {
            out.codes[d >> 1] |= nibble << 4;
        } else {
            out.codes[d >> 1] |= nibble;
        }
    }
    out.code_sum = sum;
}

// ── Estimated inner product from two codes ─────────────────────────

pub export fn cbm_rsq_ip(a: *const c.cbm_rsq_code_t, b: *const c.cbm_rsq_code_t) f32 {
    // Integer dot of the 4-bit codes.
    var dot: i64 = 0;
    for (a.codes, b.codes) |ba, bb| {
        dot += @as(i64, ba & 0x0F) * (bb & 0x0F);
        dot += @as(i64, ba >> 4) * (bb >> 4);
    }
    // x_i ≈ oa + sa·ca_i, y_i ≈ ob + sb·cb_i (in the rotated space, where the
    // IP equals the original IP because the rotation is orthonormal).
    const d: f64 = DIM;
    const oa: f64 = a.offset;
    const ob: f64 = b.offset;
    const sa: f64 = a.scale;
    const sb: f64 = b.scale;
    // C: d*oa*ob + oa*sb*Σcb + ob*sa*Σca + sa*sb*dot, summed left to right.
    // The first sum fuses with its left product, the later ones with their
    // right product.
    var ip = fp.mulAdd(f64, d * oa, ob, oa * sb * @as(f64, @floatFromInt(b.code_sum)));
    ip = fp.mulAdd(f64, ob * sa, @as(f64, @floatFromInt(a.code_sum)), ip);
    ip = fp.mulAdd(f64, sa * sb, @as(f64, @floatFromInt(dot)), ip);
    return @floatCast(ip);
}

// ── Dequantize into the rotated basis ──────────────────────────────

pub export fn cbm_rsq_decode(code: *const c.cbm_rsq_code_t, out: [*]f32) void {
    for (code.codes, 0..) |b, i| {
        out[i * 2] = fp.mulAdd(f32, code.scale, @floatFromInt(b & 0x0F), code.offset);
        out[i * 2 + 1] = fp.mulAdd(f32, code.scale, @floatFromInt(b >> 4), code.offset);
    }
}
