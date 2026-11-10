//! slab_alloc.zig — Slab allocator for tree-sitter.
//!
//! Implements src/foundation/slab_alloc.h.
//!
//! Replaces malloc/calloc/realloc/free for ALL tree-sitter allocations,
//! eliminating ptmalloc2's per-thread arena fragmentation (the root cause
//! of 321GB VSZ when indexing large codebases with 12 workers).
//!
//! Tier 1 (≤64B): Fixed-size slab free list.
//!   Matches tree-sitter SubtreeHeapData (64 bytes). O(1) alloc/free.
//!   Backed by 64KB slab pages (malloc = mimalloc in production).
//!
//! All allocations >64B go directly to malloc() which is mimalloc
//! in production builds (MI_OVERRIDE=1). This eliminates the complex
//! tier2 bump allocator and its O(n) ownership checks.
//!
//! Cross-thread frees (tree-sitter's allocator callbacks are process-global,
//! so a chunk allocated on parser thread A can be freed on thread B):
//!
//!   - Each slab page is allocated SLAB_PAGE_SIZE-aligned, so the owning page
//!     of ANY chunk is recovered in O(1) by masking the pointer — no per-op
//!     scan and no global hot-path lock. A tiny 3-level radix page map (keyed
//!     by page number, lock-free reads, cold-path writes) tells slabFree
//!     whether a masked base is a real slab page or a plain heap pointer,
//!     without ever dereferencing an unrelated heap address.
//!   - A free by the owning thread returns the chunk to the thread-local free
//!     list (fast path, no atomics on the list). A foreign free is pushed onto
//!     that page's lock-free MPSC remote-free stack; the owner drains it on its
//!     next refill. Neither path takes a global lock.
//!
//! On reclaim/destroy: free pages with no live chunks; retire pages that still
//! have live chunks (owner=NULL, kept reachable) and free them when the final
//! chunk returns. Each page carries a refcount = (handed-out chunks) + (1 owner
//! guard while owned); the thread that drops it to zero frees the page. A page
//! holding a still-live tree-sitter lexer chunk is therefore never freed under
//! it (fixes the cross-thread invalid free and the #852 use-after-free).
//! realloc handles slab-to-heap promotion with minimal copying.

const std = @import("std");
const c = @import("c");

const Atomic = std.atomic.Value;

// ── Tier 1: Fixed-size slab (≤64B) ────────────────────────────────

/// Chunk size matches tree-sitter SubtreeHeapData (64 bytes).
/// All slab allocations are this size regardless of requested size.
const SLAB_CHUNK_SIZE: usize = 64;

// Each slab page is a 64KB region, allocated aligned to its own size so that
// masking any interior chunk pointer with SLAB_PAGE_MASK yields the page base
// (which is where the page header lives).
const SLAB_PAGE_SIZE: usize = 65536;
const SLAB_PAGE_SHIFT = 16; // log2(SLAB_PAGE_SIZE)
const SLAB_PAGE_MASK: usize = ~(SLAB_PAGE_SIZE - 1);

// Chunk region starts after the page header. SLAB_DATA_OFFSET must be a
// multiple of SLAB_CHUNK_SIZE (so chunks stay 64-byte aligned) and at least
// @sizeOf(Page). 64 bytes leaves 1023 usable chunks per page.
const SLAB_DATA_OFFSET: usize = 64;
const SLAB_PAGE_CHUNKS = (SLAB_PAGE_SIZE - SLAB_DATA_OFFSET) / SLAB_CHUNK_SIZE;

/// Free list node — occupies the first 8 bytes of a free chunk.
const FreeNode = extern struct {
    next: ?*FreeNode,
};

/// One slab page. The header lives at the SLAB_PAGE_SIZE-aligned base; the
/// SLAB_PAGE_CHUNKS chunks begin at page + SLAB_DATA_OFFSET.
const Page = extern struct {
    next: ?*Page, // owner's page list (owner thread only)
    owner: Atomic(?*State), // owning TLS state; NULL once retired
    remote_free_head: Atomic(?*FreeNode), // lock-free MPSC stack of cross-thread frees
    refcount: Atomic(c_uint), // handed-out chunks + 1 owner guard (while owned)
};

comptime {
    if (@sizeOf(Page) > SLAB_DATA_OFFSET) @compileError("slab page header must fit in SLAB_DATA_OFFSET");
    if (SLAB_DATA_OFFSET % SLAB_CHUNK_SIZE != 0) @compileError("SLAB_DATA_OFFSET must be chunk-aligned");
}

/// Per-thread Tier 1 state.
const State = extern struct {
    pages: ?*Page = null, // linked list of pages owned by this thread
    freelist: ?*FreeNode = null, // singly-linked free list (thread-local)
    installed: bool = false,
};

threadlocal var tls_slab: State = .{};

// ── Page map: safe O(1) "is this a slab page?" lookup ───────────────
//
// A 3-level radix trie keyed by page number (base >> SLAB_PAGE_SHIFT) covering
// the low 32 bits of the page number — i.e. all 48-bit addresses that malloc/
// posix_memalign/_aligned_malloc return on every supported platform. Reads are
// lock-free (atomic-acquire loads only, never dereferencing the queried
// pointer). Writes (page create / free / retire — all cold paths) take a
// dedicated spinlock that is NEVER touched on the alloc/free hot path.
//
// Intermediate tables are never freed during the run and stay reachable from
// the static root, so lock-free readers never touch freed table memory.

const SLAB_MAP_L1_BITS = 11;
const SLAB_MAP_L2_BITS = 11;
const SLAB_MAP_L3_BITS = 10;
const SLAB_MAP_TOTAL_BITS = SLAB_MAP_L1_BITS + SLAB_MAP_L2_BITS + SLAB_MAP_L3_BITS; // 32
const SLAB_MAP_L1_SIZE: usize = 1 << SLAB_MAP_L1_BITS;
const SLAB_MAP_L2_SIZE: usize = 1 << SLAB_MAP_L2_BITS;
const SLAB_MAP_L3_SIZE: usize = 1 << SLAB_MAP_L3_BITS;

const MapL3 = extern struct {
    e: [SLAB_MAP_L3_SIZE]Atomic(?*Page),
};

const MapL2 = extern struct {
    e: [SLAB_MAP_L2_SIZE]Atomic(?*MapL3),
};

var g_slab_map_root: [SLAB_MAP_L1_SIZE]Atomic(?*MapL2) = @splat(.init(null));
var g_slab_map_lock: Atomic(bool) = .init(false);

fn slabMapLock() void {
    while (g_slab_map_lock.swap(true, .acquire)) {
        // Spin — cold path only (page create/destroy/retire).
    }
}

fn slabMapUnlock() void {
    g_slab_map_lock.store(false, .release);
}

const MapIndex = struct { i1: usize, i2: usize, i3: usize };

fn slabMapIndices(base: usize) ?MapIndex {
    const pnum: u64 = @as(u64, base) >> SLAB_PAGE_SHIFT;
    if ((pnum >> SLAB_MAP_TOTAL_BITS) != 0) {
        return null; // address beyond coverage — never produced by malloc here
    }
    return .{
        .i1 = @intCast((pnum >> (SLAB_MAP_L2_BITS + SLAB_MAP_L3_BITS)) & (SLAB_MAP_L1_SIZE - 1)),
        .i2 = @intCast((pnum >> SLAB_MAP_L3_BITS) & (SLAB_MAP_L2_SIZE - 1)),
        .i3 = @intCast(pnum & (SLAB_MAP_L3_SIZE - 1)),
    };
}

/// Lock-free. Returns the slab page whose base == (ptr & mask), or null if the
/// masked base is not a registered slab page (i.e. ptr is a plain heap block).
fn slabMapLookup(base: usize) ?*Page {
    const idx = slabMapIndices(base) orelse return null;
    const l2 = g_slab_map_root[idx.i1].load(.acquire) orelse return null;
    const l3 = l2.e[idx.i2].load(.acquire) orelse return null;
    return l3.e[idx.i3].load(.acquire);
}

/// Cold path. Sets the leaf entry for `base` to `val` (page to register, null
/// to unregister), allocating intermediate tables on demand. Returns false only
/// if a needed table allocation fails while registering.
fn slabMapSet(base: usize, val: ?*Page) bool {
    const idx = slabMapIndices(base) orelse return false;
    slabMapLock();
    defer slabMapUnlock();
    const l2 = g_slab_map_root[idx.i1].load(.monotonic) orelse blk: {
        if (val == null) return true; // nothing to unregister
        const table: *MapL2 = @ptrCast(@alignCast(c.calloc(1, @sizeOf(MapL2)) orelse return false));
        g_slab_map_root[idx.i1].store(table, .release);
        break :blk table;
    };
    const l3 = l2.e[idx.i2].load(.monotonic) orelse blk: {
        if (val == null) return true;
        const table: *MapL3 = @ptrCast(@alignCast(c.calloc(1, @sizeOf(MapL3)) orelse return false));
        l2.e[idx.i2].store(table, .release);
        break :blk table;
    };
    l3.e[idx.i3].store(val, .release);
    return true;
}

// ── Tier 1 helpers ──────────────────────────────────────────────────

fn slabMapRegisterPage(page: *Page) bool {
    return slabMapSet(@intFromPtr(page), page);
}

fn slabMapUnregisterPage(page: *Page) void {
    _ = slabMapSet(@intFromPtr(page), null);
}

fn pageOf(addr: usize) *Page {
    return @ptrFromInt(addr & SLAB_PAGE_MASK);
}

/// Allocate and register a new SLAB_PAGE_SIZE-aligned page, threading its chunks
/// onto the thread-local free list. Returns false on OOM (caller falls back to
/// malloc). Registration happens before any chunk is handed out so that a later
/// cross-thread free can always recover the page.
fn slabGrow(s: *State) bool {
    var mem: ?*anyopaque = null;
    if (c.cbm_aligned_alloc(&mem, SLAB_PAGE_SIZE, SLAB_PAGE_SIZE) != 0 or mem == null) {
        return false;
    }
    const page: *Page = @ptrCast(@alignCast(mem));
    page.next = s.pages;
    page.owner = .init(s);
    page.remote_free_head = .init(null);
    page.refcount = .init(1); // owner guard
    if (!slabMapRegisterPage(page)) {
        c.cbm_aligned_free(page);
        return false;
    }
    s.pages = page;
    for (0..SLAB_PAGE_CHUNKS) |i| {
        const node: *FreeNode = @ptrFromInt(@intFromPtr(page) + SLAB_DATA_OFFSET + i * SLAB_CHUNK_SIZE);
        node.next = s.freelist;
        s.freelist = node;
    }
    return true;
}

/// Refill the thread-local free list: first reclaim any cross-thread frees
/// queued on owned pages, then grow a fresh page if still empty.
fn slabRefill(s: *State) void {
    var p = s.pages;
    while (p) |page| : (p = page.next) {
        var rf = page.remote_free_head.swap(null, .acquire);
        while (rf) |node| {
            const next = node.next;
            node.next = s.freelist;
            s.freelist = node;
            rf = next;
        }
    }
    if (s.freelist != null) return;
    _ = slabGrow(s);
}

/// Retire ownership and drop the owner guard for every page this thread owns.
/// A page with no live chunks is freed immediately; a page still holding
/// foreign-live chunks is retired (owner=NULL, left registered) and freed by
/// whichever thread returns its last chunk.
fn slabReclaimPages(s: *State, clear_installed: bool) void {
    var p = s.pages;
    while (p) |page| {
        const next = page.next;
        page.owner.store(null, .monotonic);
        const prev = page.refcount.fetchSub(1, .acq_rel);
        if (prev == 1) {
            slabMapUnregisterPage(page);
            c.cbm_aligned_free(page);
        }
        p = next;
    }
    s.pages = null;
    s.freelist = null;
    if (clear_installed) s.installed = false;
}

// ── Allocator functions (installed as tree-sitter callbacks) ───────

fn slabMalloc(size_in: usize) callconv(.c) ?*anyopaque {
    const size: usize = if (size_in == 0) c.SKIP_ONE else size_in;
    // Tier 1: ≤64B → slab free list
    if (size <= SLAB_CHUNK_SIZE) {
        const s = &tls_slab;
        if (s.freelist == null) {
            slabRefill(s);
            if (s.freelist == null) {
                return c.malloc(size); // grow failed → heap fallback
            }
        }
        const node = s.freelist.?;
        s.freelist = node.next;
        _ = pageOf(@intFromPtr(node)).refcount.fetchAdd(1, .monotonic);
        return node;
    }
    // >64B: straight to malloc (= mimalloc in production)
    return c.malloc(size);
}

fn slabCalloc(count: usize, size: usize) callconv(.c) ?*anyopaque {
    // Overflow check
    if (count > 0 and size > std.math.maxInt(usize) / count) return null;
    const total = count * size;
    const ptr = slabMalloc(total) orelse return null;
    // Must zero: free-list recycled blocks contain stale data.
    @memset(@as([*]u8, @ptrCast(ptr))[0..total], 0);
    return ptr;
}

fn slabRealloc(ptr: ?*anyopaque, new_size: usize) callconv(.c) ?*anyopaque {
    const old = ptr orelse return slabMalloc(new_size);
    if (new_size == 0) {
        // realloc(ptr, 0) = free + return NULL
        slabFree(old);
        return null;
    }

    // Case 1: ptr is in a slab page (≤64B block)
    if (slabMapLookup(@intFromPtr(old) & SLAB_PAGE_MASK) != null) {
        if (new_size <= SLAB_CHUNK_SIZE) {
            // Still fits in a slab chunk — reuse same slot
            return old;
        }
        // Promote slab → heap
        const new_ptr = c.malloc(new_size) orelse return null;
        @memcpy(@as([*]u8, @ptrCast(new_ptr))[0..SLAB_CHUNK_SIZE], @as([*]const u8, @ptrCast(old))[0..SLAB_CHUNK_SIZE]);
        slabFree(old);
        return new_ptr;
    }

    // Case 2: heap pointer (from malloc)
    return c.realloc(old, new_size);
}

fn slabFree(ptr: ?*anyopaque) callconv(.c) void {
    const p = ptr orelse return;
    const page = slabMapLookup(@intFromPtr(p) & SLAB_PAGE_MASK) orelse {
        // Not a slab chunk → plain heap pointer (>64B or grow-fallback).
        c.free(p);
        return;
    };

    // It is a slab chunk. The chunk itself keeps its page alive (refcount ≥ 1)
    // until this very free, so the page cannot be freed underneath us.
    const s = &tls_slab;
    const node: *FreeNode = @ptrCast(@alignCast(p));
    if (page.owner.load(.monotonic) == s) {
        // Fast path: owner returns the chunk to its thread-local free list.
        node.next = s.freelist;
        s.freelist = node;
        _ = page.refcount.fetchSub(1, .acq_rel);
        return; // owner guard keeps refcount ≥ 1; page is reused, not freed
    }

    // Foreign (or retired) free: push onto the page's lock-free MPSC stack.
    var head = page.remote_free_head.load(.monotonic);
    while (true) {
        node.next = head;
        head = page.remote_free_head.cmpxchgWeak(head, node, .release, .monotonic) orelse break;
    }
    const prev = page.refcount.fetchSub(1, .acq_rel);
    if (prev == 1) {
        // We returned the final chunk of a retired page → release it.
        slabMapUnregisterPage(page);
        c.cbm_aligned_free(page);
    }
}

// ── Public API ─────────────────────────────────────────────────────

pub export fn cbm_slab_install() void {
    c.ts_set_allocator(slabMalloc, slabCalloc, slabRealloc, slabFree);
}

pub export fn cbm_slab_reset_thread() void {
    cbm_slab_reclaim();
}

pub export fn cbm_slab_destroy_thread() void {
    slabReclaimPages(&tls_slab, true);
}

/// Reclaim slab memory owned by the current thread.
///
/// Call ONLY when no LOCAL live allocations remain — i.e., after ts_tree_delete()
/// AND ts_parser_delete() have freed this thread's parser-owned chunks. If a
/// chunk from one of these pages is still held by another parser thread, the
/// page is retired instead of freed and released on the final cross-thread free.
/// This keeps peak memory bounded per-file without handing foreign slab chunks
/// to plain free().
pub export fn cbm_slab_reclaim() void {
    slabReclaimPages(&tls_slab, false);
}

// ── Test API (thin wrappers for unit testing) ──────────────────────

pub export fn cbm_slab_test_malloc(size: usize) ?*anyopaque {
    return slabMalloc(size);
}

pub export fn cbm_slab_test_free(ptr: ?*anyopaque) void {
    slabFree(ptr);
}

pub export fn cbm_slab_test_realloc(ptr: ?*anyopaque, size: usize) ?*anyopaque {
    return slabRealloc(ptr, size);
}

pub export fn cbm_slab_test_calloc(count: usize, size: usize) ?*anyopaque {
    return slabCalloc(count, size);
}
