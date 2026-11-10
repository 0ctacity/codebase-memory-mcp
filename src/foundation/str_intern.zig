//! str_intern.zig — String interning pool.
//!
//! Implements src/foundation/str_intern.h. Uses a hash table for O(1) dedup
//! lookup and an arena for string storage. All strings are copied into the
//! arena — the pool owns the memory.

const std = @import("std");
const c = @import("c");
const arena = @import("arena.zig");

const INTERN_INITIAL_CAP: u32 = 256;
const INTERN_LOAD_NUM: u32 = 7;
const INTERN_LOAD_DEN: u32 = 10;

// FNV-1a hash constants (published by Fowler/Noll/Vo)
const FNV_OFFSET_BASIS: u32 = 2166136261;
const FNV_PRIME: u32 = 16777619;

/// FNV-1a for interning (matches hash_table.c)
fn internHash(s: []const u8) u32 {
    var h: u32 = FNV_OFFSET_BASIS;
    for (s) |ch| {
        h ^= ch;
        h *%= FNV_PRIME;
    }
    return h;
}

/// Bucket for open-addressing
const InternEntry = extern struct {
    str: ?[*:0]const u8, // arena-owned pointer
    hash: u32,
    len: u32, // string length (excluding NUL)
};

const CBMInternPool = extern struct {
    arena: c.CBMArena,
    buckets: [*]InternEntry,
    capacity: u32,
    count: u32,
    mask: u32,
    total_bytes: usize, // sum of string lengths stored
};

fn allocBuckets(n: u32) ?[*]InternEntry {
    return @ptrCast(@alignCast(c.calloc(n, @sizeOf(InternEntry))));
}

pub export fn cbm_intern_create() ?*CBMInternPool {
    const p: *CBMInternPool = @ptrCast(@alignCast(c.calloc(1, @sizeOf(CBMInternPool)) orelse return null));
    arena.cbm_arena_init(&p.arena);
    p.capacity = INTERN_INITIAL_CAP;
    p.mask = p.capacity - 1;
    p.buckets = allocBuckets(p.capacity) orelse {
        arena.cbm_arena_destroy(&p.arena);
        c.free(p);
        return null;
    };
    return p;
}

pub export fn cbm_intern_free(pool: ?*CBMInternPool) void {
    const p = pool orelse return;
    arena.cbm_arena_destroy(&p.arena);
    c.free(p.buckets);
    c.free(p);
}

fn internResize(p: *CBMInternPool) void {
    const new_cap = p.capacity *% 2;
    const new_mask = new_cap -% 1;
    const new_buckets = allocBuckets(new_cap) orelse return;

    for (p.buckets[0..p.capacity]) |e| {
        if (e.str == null) continue;
        var idx = e.hash & new_mask;
        while (new_buckets[idx].str != null) idx = (idx +% 1) & new_mask;
        new_buckets[idx] = e;
    }

    c.free(p.buckets);
    p.buckets = new_buckets;
    p.capacity = new_cap;
    p.mask = new_mask;
}

pub export fn cbm_intern_n(pool: ?*CBMInternPool, s: ?[*]const u8, len: usize) ?[*:0]const u8 {
    const p = pool orelse return null;
    const src = (s orelse return null)[0..len];

    const h = internHash(src);
    var idx = h & p.mask;

    // Probe for existing entry
    while (true) {
        const e = &p.buckets[idx];
        const str = e.str orelse break; // empty slot — not found
        if (e.hash == h and e.len == @as(u32, @truncate(len)) and std.mem.eql(u8, str[0..len], src)) {
            return str; // found — return existing pointer
        }
        idx = (idx +% 1) & p.mask;
    }

    // Resize at 70% load
    if (p.count *% INTERN_LOAD_DEN >= p.capacity *% INTERN_LOAD_NUM) {
        internResize(p);
        // Re-probe after resize
        idx = h & p.mask;
        while (p.buckets[idx].str != null) idx = (idx +% 1) & p.mask;
    }

    // Copy string into arena
    const copy = arena.cbm_arena_strndup(&p.arena, src.ptr, len) orelse return null;

    p.buckets[idx] = .{ .str = copy, .hash = h, .len = @truncate(len) };
    p.count += 1;
    p.total_bytes += len;
    return copy;
}

pub export fn cbm_intern(pool: ?*CBMInternPool, s: ?[*:0]const u8) ?[*:0]const u8 {
    const src = s orelse return null;
    return cbm_intern_n(pool, src, std.mem.len(src));
}

pub export fn cbm_intern_count(pool: ?*const CBMInternPool) u32 {
    return if (pool) |p| p.count else 0;
}

pub export fn cbm_intern_bytes(pool: ?*const CBMInternPool) usize {
    return if (pool) |p| p.total_bytes else 0;
}
