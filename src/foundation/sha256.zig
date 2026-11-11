//! sha256.zig — SHA-256 per FIPS 180-4.
//!
//! Implements src/foundation/sha256.h and sha256_backend.h. Straightforward
//! reference implementation; validated against the NIST test vectors in
//! tests/test_cli.c. The backend uses CommonCrypto on Apple platforms.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");

const is_apple = builtin.os.tag.isDarwin();

const K = [64]u32{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

const rotr = std.math.rotr;

fn ch(x: u32, y: u32, z: u32) u32 {
    return (x & y) ^ (~x & z);
}
fn maj(x: u32, y: u32, z: u32) u32 {
    return (x & y) ^ (x & z) ^ (y & z);
}
fn ep0(x: u32) u32 {
    return rotr(u32, x, 2) ^ rotr(u32, x, 13) ^ rotr(u32, x, 22);
}
fn ep1(x: u32) u32 {
    return rotr(u32, x, 6) ^ rotr(u32, x, 11) ^ rotr(u32, x, 25);
}
fn sig0(x: u32) u32 {
    return rotr(u32, x, 7) ^ rotr(u32, x, 18) ^ (x >> 3);
}
fn sig1(x: u32) u32 {
    return rotr(u32, x, 17) ^ rotr(u32, x, 19) ^ (x >> 10);
}

fn sha256Transform(ctx: *c.cbm_sha256_ctx, data: *const [64]u8) void {
    var m: [64]u32 = undefined;
    for (0..16) |i| m[i] = std.mem.readInt(u32, data[i * 4 ..][0..4], .big);
    for (16..64) |i| m[i] = sig1(m[i - 2]) +% m[i - 7] +% sig0(m[i - 15]) +% m[i - 16];

    var s: [8]u32 = ctx.state;
    for (0..64) |i| {
        const t1 = s[7] +% ep1(s[4]) +% ch(s[4], s[5], s[6]) +% K[i] +% m[i];
        const t2 = ep0(s[0]) +% maj(s[0], s[1], s[2]);
        s[7] = s[6];
        s[6] = s[5];
        s[5] = s[4];
        s[4] = s[3] +% t1;
        s[3] = s[2];
        s[2] = s[1];
        s[1] = s[0];
        s[0] = t1 +% t2;
    }
    for (&ctx.state, s) |*state, v| state.* +%= v;
}

pub export fn cbm_sha256_init(ctx: *c.cbm_sha256_ctx) void {
    ctx.bitlen = 0;
    ctx.buflen = 0;
    ctx.state = .{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
}

pub export fn cbm_sha256_update(ctx: *c.cbm_sha256_ctx, data: ?*const anyopaque, len: usize) void {
    if (len == 0) return;
    var p: []const u8 = @as([*]const u8, @ptrCast(data.?))[0..len];
    if (ctx.buflen != 0) {
        const take = @min(64 - ctx.buflen, p.len);
        @memcpy(ctx.buf[ctx.buflen..][0..take], p[0..take]);
        ctx.buflen += take;
        p = p[take..];
        if (ctx.buflen == 64) {
            sha256Transform(ctx, &ctx.buf);
            ctx.bitlen +%= 512;
            ctx.buflen = 0;
        }
    }
    while (p.len >= 64) : (p = p[64..]) {
        sha256Transform(ctx, p[0..64]);
        ctx.bitlen +%= 512;
    }
    if (p.len != 0) {
        @memcpy(ctx.buf[0..p.len], p);
        ctx.buflen = p.len;
    }
}

pub export fn cbm_sha256_final(ctx: *c.cbm_sha256_ctx, out: [*]u8) void {
    ctx.bitlen +%= @as(u64, ctx.buflen) * 8;
    var i = ctx.buflen;
    ctx.buf[i] = 0x80; // append the '1' bit + zero padding
    i += 1;
    if (i > 56) {
        @memset(ctx.buf[i..], 0);
        sha256Transform(ctx, &ctx.buf);
        i = 0;
    }
    @memset(ctx.buf[i..56], 0);
    // append the 64-bit big-endian message length
    std.mem.writeInt(u64, ctx.buf[56..64], ctx.bitlen, .big);
    sha256Transform(ctx, &ctx.buf);
    for (ctx.state, 0..) |word, j| std.mem.writeInt(u32, out[j * 4 ..][0..4], word, .big);
}

pub export fn cbm_sha256_hex(data: ?*const anyopaque, len: usize, out: [*]u8) void {
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    var ctx: c.cbm_sha256_ctx = undefined;
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, data, len);
    cbm_sha256_final(&ctx, &digest);
    const hex = "0123456789abcdef";
    for (digest, 0..) |byte, i| {
        out[i * 2] = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 0x0f];
    }
    out[c.CBM_SHA256_HEX_LEN] = 0;
}

// ── Backend (sha256_backend.h) ───────────────────────────────────

const Backend = if (is_apple) c.CC_SHA256_CTX else c.cbm_sha256_ctx;

comptime {
    if (@sizeOf(Backend) > 256) @compileError("SHA-256 backend storage is too small");
}

fn backendState(ctx: *c.cbm_sha256_backend_ctx) *Backend {
    return @ptrCast(@alignCast(&ctx.state.bytes));
}

pub export fn cbm_sha256_backend_init(ctx: *c.cbm_sha256_backend_ctx) void {
    if (is_apple) {
        _ = c.CC_SHA256_Init(backendState(ctx));
    } else {
        cbm_sha256_init(backendState(ctx));
    }
}

pub export fn cbm_sha256_backend_update(ctx: *c.cbm_sha256_backend_ctx, data: ?*const anyopaque, len_in: usize) void {
    if (is_apple) {
        if (len_in == 0) return; // data may be NULL for an empty update
        const chunk_max: usize = std.math.maxInt(c_uint);
        var bytes: [*]const u8 = @ptrCast(data.?);
        var len = len_in;
        while (len > chunk_max) {
            _ = c.CC_SHA256_Update(backendState(ctx), bytes, @intCast(chunk_max));
            bytes += chunk_max;
            len -= chunk_max;
        }
        if (len != 0) _ = c.CC_SHA256_Update(backendState(ctx), bytes, @intCast(len));
    } else {
        cbm_sha256_update(backendState(ctx), data, len_in);
    }
}

pub export fn cbm_sha256_backend_final(ctx: *c.cbm_sha256_backend_ctx, out: [*]u8) void {
    if (is_apple) {
        _ = c.CC_SHA256_Final(out, backendState(ctx));
    } else {
        cbm_sha256_final(backendState(ctx), out);
    }
}

pub export fn cbm_sha256_backend_is_accelerated() bool {
    return is_apple;
}
