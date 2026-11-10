//! arena.zig — Bump allocator implementation.
//!
//! Implements src/foundation/arena.h. Arena blocks use malloc/free
//! (= mimalloc in production builds). The variadic cbm_arena_sprintf stays in
//! C (c_shim.c): Zig cannot forward a va_list to vsnprintf portably.

const std = @import("std");
const c = @import("c");

const ARENA_ALIGN: usize = 7;
const ARENA_MIN_BLOCK: usize = 64;

pub export fn cbm_arena_init(a: *c.CBMArena) void {
    cbm_arena_init_sized(a, c.CBM_ARENA_DEFAULT_BLOCK_SIZE);
}

pub export fn cbm_arena_init_sized(a: *c.CBMArena, block_size_in: usize) void {
    a.* = std.mem.zeroes(c.CBMArena);
    // minimum sanity
    const block_size = @max(block_size_in, ARENA_MIN_BLOCK);
    a.block_size = block_size;
    a.blocks[0] = @ptrCast(c.malloc(block_size));
    if (a.blocks[0] != null) {
        a.block_sizes[0] = block_size;
        a.nblocks = 1;
    }
}

fn arenaGrow(a: *c.CBMArena, min_size: usize) bool {
    if (a.nblocks >= c.CBM_ARENA_MAX_BLOCKS) return false;
    var new_size = a.block_size *% 2;
    if (new_size < min_size) new_size = min_size;
    const block: [*c]u8 = @ptrCast(c.malloc(new_size));
    if (block == null) return false;
    const i: usize = @intCast(a.nblocks);
    a.blocks[i] = block;
    a.block_sizes[i] = new_size;
    a.nblocks += 1;
    a.block_size = new_size;
    a.used = 0;
    return true;
}

pub export fn cbm_arena_alloc(a: ?*c.CBMArena, n_in: usize) ?*anyopaque {
    const arena = a orelse return null;
    if (n_in == 0) return null;

    // 8-byte alignment
    const n = (n_in +% ARENA_ALIGN) & ~ARENA_ALIGN;

    if (arena.nblocks == 0) return null;

    if (arena.used +% n > arena.block_size) {
        if (!arenaGrow(arena, n)) return null;
    }

    const last: usize = @intCast(arena.nblocks - 1);
    const ptr = arena.blocks[last] + arena.used;
    arena.used +%= n;
    arena.total_alloc +%= n;
    return @ptrCast(ptr);
}

pub export fn cbm_arena_calloc(a: ?*c.CBMArena, n: usize) ?*anyopaque {
    const p = cbm_arena_alloc(a, n) orelse return null;
    @memset(@as([*]u8, @ptrCast(p))[0..n], 0);
    return p;
}

pub export fn cbm_arena_strdup(a: ?*c.CBMArena, s: ?[*:0]const u8) ?[*:0]u8 {
    const src = s orelse return null;
    const len = std.mem.len(src);
    const dst: [*]u8 = @ptrCast(cbm_arena_alloc(a, len + 1) orelse return null);
    @memcpy(dst[0 .. len + 1], src[0 .. len + 1]);
    return @ptrCast(dst);
}

pub export fn cbm_arena_strndup(a: ?*c.CBMArena, s: ?[*]const u8, len: usize) ?[*:0]u8 {
    const src = s orelse return null;
    const dst: [*]u8 = @ptrCast(cbm_arena_alloc(a, len +% 1) orelse return null);
    @memcpy(dst[0..len], src[0..len]);
    dst[len] = 0;
    return @ptrCast(dst);
}

pub export fn cbm_arena_reset(a: *c.CBMArena) void {
    // Keep first block, free the rest
    var i: usize = 1;
    while (i < a.nblocks) : (i += 1) {
        c.free(a.blocks[i]);
        a.blocks[i] = null;
        a.block_sizes[i] = 0;
    }
    if (a.nblocks > 1) a.nblocks = 1;
    a.used = 0;
    a.total_alloc = 0;
    // Reset block_size to match surviving block — prevents overflow if
    // block_size grew during previous allocations (e.g., 128 → 256).
    if (a.nblocks == 1) a.block_size = a.block_sizes[0];
}

pub export fn cbm_arena_destroy(a: *c.CBMArena) void {
    var i: usize = 0;
    while (i < a.nblocks) : (i += 1) {
        c.free(a.blocks[i]);
    }
    a.* = std.mem.zeroes(c.CBMArena);
}

pub export fn cbm_arena_total(a: *const c.CBMArena) usize {
    return a.total_alloc;
}
