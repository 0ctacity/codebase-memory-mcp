//! graph_buffer.zig — In-memory graph buffer for pipeline indexing.
//!
//! Implements src/graph_buffer/graph_buffer.h.
//!
//! Uses foundation hash tables for O(1) node lookup by QN and edge dedup.
//! Uses dynamic arrays for ordered iteration and secondary indexes.
//!
//! Memory ownership: each node/edge is individually heap-allocated so that
//! pointers stored in hash tables remain stable when the pointer-array grows.
//! The buffer frees everything in cbm_gbuf_free().

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const Str = interop.Str;
const CStr = interop.CStr;
const Ht = c.CBMHashTable;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;

const GB_ERR = -1;
const NOT_FOUND = c.CBM_NOT_FOUND;
const GB_MIN_FOR_DEDUP = 2; // need at least 2 vectors to sort+dedup
const GB_DEDUP_LOOKAHEAD = 1; // compare current with next element

// ── Internal types ─────────────────────────────────────────────────

/// Edge key for dedup hash table — composite key as string "srcID:tgtID:type",
/// plus ":local_name" for IMPORTS edges (#768). 256 bytes fit two int64s, the
/// type and a ~200-char local_name verbatim; longer local_names are re-keyed
/// with a hash of the full name in makeEdgeKey (never silently truncated).
const EDGE_KEY_BUF = 256;
const ID_KEY_BUF = 32;

/// CBM_DYN_ARRAY(T) from foundation/dyn_array.h.
fn DynArray(comptime T: type) type {
    return extern struct {
        items: ?[*]T,
        count: c_int,
        cap: c_int,

        const Self = @This();

        fn slice(self: *const Self) []T {
            const items = self.items orelse return &.{};
            return items[0..@intCast(self.count)];
        }

        /// cbm_da_push: grows by 2x when full; drops the item on OOM.
        fn push(self: *Self, item: T) void {
            if (self.count >= self.cap) {
                const new_cap: c_int = if (self.cap != 0) self.cap * 2 else 8;
                const grown = c.realloc(@ptrCast(self.items), @as(usize, @intCast(new_cap)) * @sizeOf(T)) orelse return;
                self.items = @ptrCast(@alignCast(grown));
                self.cap = new_cap;
            }
            self.items.?[@intCast(self.count)] = item;
            self.count += 1;
        }

        /// cbm_da_free.
        fn free(self: *Self) void {
            c.free(@ptrCast(self.items));
            self.items = null;
            self.count = 0;
            self.cap = 0;
        }

        /// realloc to hold at least n items (cbm_gbuf_reserve).
        fn reserve(self: *Self, n: c_int) bool {
            if (n <= self.cap) return true;
            const grown = c.realloc(@ptrCast(self.items), @as(usize, @intCast(n)) * @sizeOf(T)) orelse return false;
            self.items = @ptrCast(@alignCast(grown));
            self.cap = n;
            return true;
        }
    };
}

/// Per-type or per-key edge list stored in hash tables as values.
const EdgePtrArray = DynArray(*const Edge);

/// Per-label or per-name node list.
const NodePtrArray = DynArray(*const Node);

const Gbuf = extern struct {
    project: [*c]u8,
    root_path: [*c]u8,
    next_id: i64,
    shared_ids: ?*std.atomic.Value(i64), // NULL = use next_id, non-NULL = atomic source

    /// Node storage: array of pointers to individually heap-allocated nodes.
    /// This ensures pointers stored in hash tables remain valid when the
    /// pointer array reallocs (only the pointer array moves, not the nodes).
    nodes: DynArray(*Node),

    /// Primary index: QN → Node*
    node_by_qn: ?*Ht,
    /// Dense id → node array (ids are sequential from allocNextId, shared
    /// with edges → holes where edges took ids). Replaces a hash table keyed
    /// on STRDUP'D DECIMAL STRINGS of the id — ~0.44 GB of buckets + key
    /// strings at kernel scale, plus a snprintf+strdup+hash on every one of
    /// the ~18 hot find_by_id call sites.
    by_id: ?[*]?*Node,
    by_id_cap: i64,

    // Secondary node indexes
    nodes_by_label: ?*Ht, // key: label, value: (NodePtrArray*)
    nodes_by_name: ?*Ht, // key: name, value: (NodePtrArray*)

    /// Edge storage: array of pointers to individually heap-allocated edges
    edges: DynArray(*Edge),

    /// Edge dedup index: "srcID:tgtID:type" → Edge*
    edge_by_key: ?*Ht,

    // Edge secondary indexes: composite keys → EdgePtrArray
    edges_by_source_type: ?*Ht, // "srcID:type" → EdgePtrArray*
    edges_by_target_type: ?*Ht, // "tgtID:type" → EdgePtrArray*
    edges_by_type: ?*Ht, // "type" → EdgePtrArray*

    /// String intern pool for highly-repetitive fields (node label/file_path,
    /// edge type). Maps string content → owned canonical copy, collapsing
    /// O(nodes+edges) duplicate allocations to O(distinct). The pool owns the
    /// copies; interned pointers are stable for the buffer lifetime and are NOT
    /// freed by freeNodeStrings/freeEdgeStrings — only once in cbm_gbuf_free.
    intern_pool: ?*Ht,

    /// Vector storage for semantic embeddings (filled by pass_semantic_edges,
    /// consumed by the cbm.zova publish during dump).
    dump_vectors: ?[*]c.CBMDumpVector,
    dump_vector_count: c_int,
    dump_vector_cap: c_int,

    /// Token vector storage for enriched RI vectors (query-time lookup).
    dump_token_vecs: ?[*]c.CBMDumpTokenVec,
    dump_token_vec_count: c_int,
    dump_token_vec_cap: c_int,
    reuse_zova_semantic_vectors: bool,

    /// Finalized lightweight topology retained after the SQLite writer closes.
    /// Sidecar publication happens after the pipeline persists hashes/FTS, so
    /// it must consume these arrays without re-scanning SQLite graph rows.
    zova_dump_nodes: ?[*]c.CBMDumpNode,
    zova_dump_node_count: c_int,
    zova_dump_edges: ?[*]c.CBMDumpEdge,
    zova_dump_edge_count: c_int,
    zova_prepared_view: ?*c.cbm_zova_prepared_view_t,

    /// The SQLite writer and shared user-local publisher must describe
    /// the same indexed generation. Keep the timestamp produced for the dump
    /// so publication does not create a second, later timestamp.
    indexed_at: [64]u8,

    fn dumpVectors(gb: *const Gbuf) []c.CBMDumpVector {
        const items = gb.dump_vectors orelse return &.{};
        return items[0..@intCast(gb.dump_vector_count)];
    }

    fn dumpTokenVecs(gb: *const Gbuf) []c.CBMDumpTokenVec {
        const items = gb.dump_token_vecs orelse return &.{};
        return items[0..@intCast(gb.dump_token_vec_count)];
    }

    /// Is the node still in the QN index (not deleted)?
    fn isLive(gb: *const Gbuf, n: *const Node) bool {
        return n.qualified_name != null and c.cbm_ht_get(gb.node_by_qn, n.qualified_name) != null;
    }
};

// ── Helpers ────────────────────────────────────────────────────────

fn htGet(comptime T: type, ht: ?*const Ht, key: CStr) ?*T {
    return @ptrCast(@alignCast(c.cbm_ht_get(ht, key)));
}

/// s, or "" when s is NULL.
fn orEmpty(s: CStr) CStr {
    return if (s != null) s else "";
}

fn streq(a: CStr, b: CStr) bool {
    return c.strcmp(a, b) == 0;
}

/// A zeroed malloc'd array of n items; NULL on OOM.
fn callocSlice(comptime T: type, n: usize) ?[]T {
    const items: ?[*]T = interop.callocN(T, n);
    return (items orelse return null)[0..n];
}

/// Copy s, or "{}" when s is NULL.
fn dupOrEmptyJson(s: CStr) [*c]u8 {
    return c.cbm_heap_strdup(if (s != null) s else "{}");
}

/// Intern a repetitive string into the buffer's pool: identical content collapses
/// to a single heap copy owned by the pool. NULL maps to "{}" (matches
/// dupOrEmptyJson). The returned pointer is stable for the buffer's lifetime and
/// must never be freed or mutated by callers. Returns NULL only on OOM.
fn gbIntern(gb: *Gbuf, s: CStr) [*c]u8 {
    const key: CStr = if (s != null) s else "{}";
    if (c.cbm_ht_get(gb.intern_pool, key)) |found| return @ptrCast(found);
    const copy: [*c]u8 = c.strdup(key);
    if (copy != null) _ = c.cbm_ht_set(gb.intern_pool, copy, copy); // key == value == owned copy
    return copy;
}

fn makeIdKey(buf: *[ID_KEY_BUF]u8, id: i64) void {
    _ = interop.bprint(buf, "{d}", .{id});
}

/// FNV-1a 64-bit over a byte slice — for re-keying oversized local_names.
fn fnv1a64(s: []const u8) u64 {
    var h: u64 = 14695981039346656037;
    for (s) |byte| {
        h ^= byte;
        h *%= 1099511628211;
    }
    return h;
}

/// IMPORTS edges carry exactly one imported symbol's local_name (#768): two
/// named imports from the same specifier resolve to the same (source,
/// target) pair but are distinct symbols. Key on local_name too so the
/// second import doesn't dedup-collide with and overwrite the first —
/// every pass that walks IMPORTS edges (the call resolver in pass_parallel.c,
/// pass_lsp_cross.c) expects one local_name per edge, so
/// losing an edge here silently breaks cross-file call resolution for
/// whichever symbol got dropped, not just "who imports X" queries. Other
/// edge types keep the plain (source,target,type) key: collapsing repeat
/// edges of the same type between the same two nodes (e.g. multiple call
/// sites) into one is the existing, intended dedup behavior there.
///
/// A local_name too long for the key buffer is re-keyed with an FNV-1a hash
/// of the FULL name instead of being truncated — a truncated key would
/// collide two long names sharing a prefix and silently drop an edge again.
/// The hash key is prefixed with byte 0x01, which cannot appear in the raw
/// JSON slice (control characters must be \u-escaped in JSON), so hash keys
/// can never collide with verbatim keys.
fn makeEdgeKey(buf: *[EDGE_KEY_BUF]u8, src: i64, tgt: i64, type_c: CStr, properties_json: CStr) void {
    const edge_type = std.mem.span(type_c);
    if (properties_json != null and std.mem.eql(u8, edge_type, "IMPORTS")) {
        const local_name_key = "\"local_name\":\"";
        const props = std.mem.span(properties_json);
        if (std.mem.indexOf(u8, props, local_name_key)) |at| {
            const rest = props[at + local_name_key.len ..];
            const local_name = rest[0 .. std.mem.indexOfScalar(u8, rest, '"') orelse rest.len];
            const n = interop.bprint(buf, "{d}:{d}:{s}:{s}", .{ src, tgt, edge_type, local_name });
            if (n >= buf.len) {
                _ = interop.bprint(buf, "{d}:{d}:{s}:\x01{x:0>16}", .{ src, tgt, edge_type, fnv1a64(local_name) });
            }
            return;
        }
    }
    _ = interop.bprint(buf, "{d}:{d}:{s}", .{ src, tgt, edge_type });
}

fn makeSrcTypeKey(buf: *[EDGE_KEY_BUF]u8, src: i64, type_c: CStr) void {
    _ = interop.bprint(buf, "{d}:{s}", .{ src, interop.sp(type_c) });
}

/// Get or create a pointer array (NodePtrArray or EdgePtrArray) in a hash
/// table. NULL on OOM.
fn getOrCreateArray(comptime A: type, ht: ?*Ht, key: CStr) ?*A {
    if (htGet(A, ht, key)) |arr| return arr;
    const arr: *A = @ptrCast(@alignCast(c.calloc(1, @sizeOf(A)) orelse return null));
    _ = c.cbm_ht_set(ht, c.strdup(key), arr);
    return arr;
}

/// Free a NodePtrArray (callback for hash table iteration).
fn freeNodeArray(key: [*c]const u8, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    if (value) |v| {
        const arr: *NodePtrArray = @ptrCast(@alignCast(v));
        arr.free();
        c.free(arr);
    }
    c.free(@constCast(key));
}

/// Free an EdgePtrArray (callback).
fn freeEdgeArray(key: [*c]const u8, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    if (value) |v| {
        const arr: *EdgePtrArray = @ptrCast(@alignCast(v));
        arr.free();
        c.free(arr);
    }
    c.free(@constCast(key));
}

/// Free keys only (for edge_by_key, deleted_set).
fn freeKeyOnly(key: [*c]const u8, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = value;
    _ = ud;
    c.free(@constCast(key));
}

/// Free a single node's owned strings. label and file_path are interned
/// (pool-owned) — NOT freed here; the pool frees them once in cbm_gbuf_free.
fn freeNodeStrings(n: *Node) void {
    c.free(n.name);
    c.free(n.qualified_name);
    c.free(n.properties_json);
}

/// Free a single edge's owned strings. type is interned (pool-owned) — NOT
/// freed here; the pool frees it once in cbm_gbuf_free.
fn freeEdgeStrings(e: *Edge) void {
    c.free(e.properties_json);
}

/// Allocate the next buffer-local or shared-atomic ID.
fn allocNextId(gb: *Gbuf) i64 {
    if (gb.shared_ids) |ids| return ids.fetchAdd(1, .monotonic);
    const id = gb.next_id;
    gb.next_id += 1;
    return id;
}

/// Swap-remove an edge from a pointer array by ID.
fn removeEdgeFromPtrArray(arr_opt: ?*EdgePtrArray, edge_id: i64) void {
    const arr = arr_opt orelse return;
    const items = arr.slice();
    for (items, 0..) |item, j| {
        if (item.id == edge_id) {
            arr.count -= 1;
            items[j] = items[@intCast(arr.count)];
            return;
        }
    }
}

/// Swap-remove a node from a NodePtrArray by ID.
fn removeNodeFromPtrArray(arr_opt: ?*NodePtrArray, node_id: i64) void {
    const arr = arr_opt orelse return;
    const items = arr.slice();
    for (items, 0..) |item, j| {
        if (item.id == node_id) {
            arr.count -= 1;
            items[j] = items[@intCast(arr.count)];
            return;
        }
    }
}

/// Remove an edge from all indexes (dedup + source_type + target_type + type).
fn unindexEdge(gb: *Gbuf, e: *const Edge) void {
    var key: [EDGE_KEY_BUF]u8 = undefined;

    makeEdgeKey(&key, e.source_id, e.target_id, e.type, e.properties_json);
    const ekey = c.cbm_ht_get_key(gb.edge_by_key, &key);
    _ = c.cbm_ht_delete(gb.edge_by_key, &key);
    c.free(@constCast(ekey));

    makeSrcTypeKey(&key, e.source_id, e.type);
    removeEdgeFromPtrArray(htGet(EdgePtrArray, gb.edges_by_source_type, &key), e.id);

    makeSrcTypeKey(&key, e.target_id, e.type);
    removeEdgeFromPtrArray(htGet(EdgePtrArray, gb.edges_by_target_type, &key), e.id);

    removeEdgeFromPtrArray(htGet(EdgePtrArray, gb.edges_by_type, e.type), e.id);
}

/// Cascade-delete all edges touching nodes in deleted_set.
fn cascadeDeleteEdges(gb: *Gbuf, deleted_set: ?*Ht) void {
    const edges = gb.edges.slice();
    var write_idx: usize = 0;
    for (edges) |e| {
        var src_id: [ID_KEY_BUF]u8 = undefined;
        var tgt_id: [ID_KEY_BUF]u8 = undefined;
        makeIdKey(&src_id, e.source_id);
        makeIdKey(&tgt_id, e.target_id);

        if (c.cbm_ht_get(deleted_set, &src_id) != null or c.cbm_ht_get(deleted_set, &tgt_id) != null) {
            unindexEdge(gb, e);
            freeEdgeStrings(e);
            c.free(e);
        } else {
            edges[write_idx] = e;
            write_idx += 1;
        }
    }
    gb.edges.count = @intCast(write_idx);
}

/// Register a node in primary (QN, ID) and secondary (label, name) indexes.
fn registerNodeInIndexes(gb: *Gbuf, node: *Node) void {
    _ = c.cbm_ht_set(gb.node_by_qn, node.qualified_name, node);

    if (node.id >= gb.by_id_cap) {
        var nc: i64 = if (gb.by_id_cap > 0) gb.by_id_cap else 1024;
        while (nc <= node.id) nc *= 2;
        if (c.realloc(@ptrCast(gb.by_id), @as(usize, @intCast(nc)) * @sizeOf(?*Node))) |raw| {
            const grown: [*]?*Node = @ptrCast(@alignCast(raw));
            @memset(grown[@intCast(gb.by_id_cap)..@intCast(nc)], null);
            gb.by_id = grown;
            gb.by_id_cap = nc;
        }
    }
    if (node.id >= 0 and node.id < gb.by_id_cap) {
        gb.by_id.?[@intCast(node.id)] = node;
    }

    if (getOrCreateArray(NodePtrArray, gb.nodes_by_label, orEmpty(node.label))) |by_label| by_label.push(node);
    if (getOrCreateArray(NodePtrArray, gb.nodes_by_name, orEmpty(node.name))) |by_name| by_name.push(node);
}

/// Index an edge by one key into a hash table bucket.
fn indexEdgeByKey(ht: ?*Ht, key: CStr, edge: *Edge) void {
    if (getOrCreateArray(EdgePtrArray, ht, key)) |arr| arr.push(edge);
}

/// Register an edge in secondary indexes (source_type, target_type, type).
fn registerEdgeInIndexes(gb: *Gbuf, edge: *Edge) void {
    var key: [EDGE_KEY_BUF]u8 = undefined;

    makeSrcTypeKey(&key, edge.source_id, edge.type);
    indexEdgeByKey(gb.edges_by_source_type, &key, edge);

    makeSrcTypeKey(&key, edge.target_id, edge.type);
    indexEdgeByKey(gb.edges_by_target_type, &key, edge);

    indexEdgeByKey(gb.edges_by_type, edge.type, edge);
}

/// cbm_ht_foreach(ht, free_fn) + cbm_ht_free(ht).
fn freeIndex(ht: ?*Ht, free_fn: c.cbm_ht_iter_fn) void {
    c.cbm_ht_foreach(ht, free_fn, null);
    c.cbm_ht_free(ht);
}

/// Rebuild edge secondary indexes from scratch (after bulk deletion).
fn rebuildEdgeSecondaryIndexes(gb: *Gbuf) void {
    freeIndex(gb.edges_by_source_type, freeEdgeArray);
    freeIndex(gb.edges_by_target_type, freeEdgeArray);
    freeIndex(gb.edges_by_type, freeEdgeArray);

    gb.edges_by_source_type = c.cbm_ht_create(256);
    gb.edges_by_target_type = c.cbm_ht_create(256);
    gb.edges_by_type = c.cbm_ht_create(32);

    for (gb.edges.slice()) |e| registerEdgeInIndexes(gb, e);
}

/// Release all lookup hash tables (used by dump after building arrays).
fn releaseGbufIndexes(gb: *Gbuf) void {
    c.cbm_ht_free(gb.node_by_qn);
    gb.node_by_qn = null;
    c.free(@ptrCast(gb.by_id));
    gb.by_id = null;
    gb.by_id_cap = 0;
    freeIndex(gb.nodes_by_label, freeNodeArray);
    gb.nodes_by_label = null;
    freeIndex(gb.nodes_by_name, freeNodeArray);
    gb.nodes_by_name = null;
    freeIndex(gb.edge_by_key, freeKeyOnly);
    gb.edge_by_key = null;
    freeIndex(gb.edges_by_source_type, freeEdgeArray);
    gb.edges_by_source_type = null;
    freeIndex(gb.edges_by_target_type, freeEdgeArray);
    gb.edges_by_target_type = null;
    freeIndex(gb.edges_by_type, freeEdgeArray);
    gb.edges_by_type = null;
}

// ── Lifecycle ──────────────────────────────────────────────────────

pub export fn cbm_gbuf_new(project: ?Str, root_path: ?Str) ?*Gbuf {
    const gb: *Gbuf = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Gbuf)) orelse return null));

    gb.project = c.strdup(project orelse "");
    gb.root_path = c.strdup(root_path orelse "");
    gb.next_id = 1;
    gb.shared_ids = null;

    gb.node_by_qn = c.cbm_ht_create(256);
    gb.by_id = null;
    gb.by_id_cap = 0;
    gb.nodes_by_label = c.cbm_ht_create(32);
    gb.nodes_by_name = c.cbm_ht_create(256);

    gb.edge_by_key = c.cbm_ht_create(512);
    gb.edges_by_source_type = c.cbm_ht_create(256);
    gb.edges_by_target_type = c.cbm_ht_create(256);
    gb.edges_by_type = c.cbm_ht_create(32);

    gb.intern_pool = c.cbm_ht_create(1024);

    return gb;
}

// Not `pub`: translate-c cannot express the header's `_Atomic int64_t *`
// parameter, so there is no translated declaration to check this against.
export fn cbm_gbuf_new_shared_ids(project: ?Str, root_path: ?Str, id_source: ?*std.atomic.Value(i64)) ?*Gbuf {
    const gb = cbm_gbuf_new(project, root_path) orelse return null;
    if (id_source) |ids| gb.shared_ids = ids;
    return gb;
}

pub export fn cbm_gbuf_reserve(gb_c: ?*Gbuf, node_count: c_int, edge_count: c_int) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (node_count < 0 or edge_count < 0) return NOT_FOUND;
    if (!gb.nodes.reserve(node_count)) return NOT_FOUND;
    if (!gb.edges.reserve(edge_count)) return NOT_FOUND;
    return 0;
}

pub export fn cbm_gbuf_free(gb_c: ?*Gbuf) void {
    const gb = gb_c orelse return;

    c.free(@ptrCast(gb.zova_dump_nodes));
    freeRetainedDumpEdges(gb.zova_dump_edges, gb.zova_dump_edge_count);
    c.cbm_zova_prepared_view_free(gb.zova_prepared_view);

    // Free each individually-allocated node
    for (gb.nodes.slice()) |n| {
        freeNodeStrings(n);
        c.free(n);
    }
    gb.nodes.free();

    // Free each individually-allocated edge
    for (gb.edges.slice()) |e| {
        freeEdgeStrings(e);
        c.free(e);
    }
    gb.edges.free();

    // Free hash tables — may be NULL if already released by the dump
    if (gb.node_by_qn != null) c.cbm_ht_free(gb.node_by_qn);
    c.free(@ptrCast(gb.by_id));
    if (gb.nodes_by_label != null) freeIndex(gb.nodes_by_label, freeNodeArray);
    if (gb.nodes_by_name != null) freeIndex(gb.nodes_by_name, freeNodeArray);
    if (gb.edge_by_key != null) freeIndex(gb.edge_by_key, freeKeyOnly);
    if (gb.edges_by_source_type != null) freeIndex(gb.edges_by_source_type, freeEdgeArray);
    if (gb.edges_by_target_type != null) freeIndex(gb.edges_by_target_type, freeEdgeArray);
    if (gb.edges_by_type != null) freeIndex(gb.edges_by_type, freeEdgeArray);

    // Free vector storage
    for (gb.dumpVectors()) |v| c.free(@constCast(v.vector));
    c.free(@ptrCast(gb.dump_vectors));

    // Free token vector storage
    for (gb.dumpTokenVecs()) |tv| {
        c.free(@constCast(tv.token));
        c.free(@constCast(tv.vector));
    }
    c.free(@ptrCast(gb.dump_token_vecs));

    // Free interned strings (node label/file_path, edge type) — pool owns one
    // copy each (key == value), freed exactly once via freeKeyOnly. Done after
    // nodes/edges since they borrowed these pointers.
    if (gb.intern_pool != null) freeIndex(gb.intern_pool, freeKeyOnly);

    c.free(gb.project);
    c.free(gb.root_path);
    c.free(gb);
}

// ── Vector storage ─────────────────────────────────────────────────

pub export fn cbm_gbuf_clear_semantic_vectors(gb_c: ?*Gbuf) void {
    const gb = gb_c orelse return;
    for (gb.dumpVectors()) |v| c.free(@constCast(v.vector));
    gb.dump_vector_count = 0;
    for (gb.dumpTokenVecs()) |tv| {
        c.free(@constCast(tv.token));
        c.free(@constCast(tv.vector));
    }
    gb.dump_token_vec_count = 0;
}

pub export fn cbm_gbuf_reuse_zova_semantic_vectors(gb_c: ?*Gbuf) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (gb.dump_vector_count != 0 or gb.dump_token_vec_count != 0) return NOT_FOUND;
    gb.reuse_zova_semantic_vectors = true;
    return 0;
}

/// A malloc'd copy of the first `len` bytes of `vector`.
fn dupVector(vector: [*]const u8, len: usize) ?[*]u8 {
    const copy: [*]u8 = @ptrCast(c.malloc(len) orelse return null);
    @memcpy(copy[0..len], vector[0..len]);
    return copy;
}

pub export fn cbm_gbuf_store_vector(gb_c: ?*Gbuf, node_id: i64, vector_c: ?[*]const u8, vector_len: c_int) c_int {
    const gb = gb_c orelse return GB_ERR;
    const vector = vector_c orelse return GB_ERR;
    if (vector_len <= 0) return GB_ERR;
    const VEC_INIT_CAP = 1024;
    const VEC_GROW = 2;
    if (gb.dump_vector_count >= gb.dump_vector_cap) {
        const new_cap: c_int = if (gb.dump_vector_cap < VEC_INIT_CAP) VEC_INIT_CAP else gb.dump_vector_cap * VEC_GROW;
        const grown = c.realloc(@ptrCast(gb.dump_vectors), @as(usize, @intCast(new_cap)) * @sizeOf(c.CBMDumpVector)) orelse return GB_ERR;
        gb.dump_vectors = @ptrCast(@alignCast(grown));
        gb.dump_vector_cap = new_cap;
    }
    // Copy vector data
    const vec_copy = dupVector(vector, @intCast(vector_len)) orelse return GB_ERR;

    gb.dump_vectors.?[@intCast(gb.dump_vector_count)] = .{
        .node_id = node_id,
        .project = gb.project, // borrowed — valid until cbm_gbuf_free
        .vector = vec_copy,
        .vector_len = vector_len,
    };
    gb.dump_vector_count += 1;
    return 0;
}

pub export fn cbm_gbuf_store_token_vector(gb_c: ?*Gbuf, token: ?Str, vector_c: ?[*]const u8, vector_len: c_int, idf: f32) c_int {
    const gb = gb_c orelse return GB_ERR;
    const vector = vector_c orelse return GB_ERR;
    if (token == null or vector_len <= 0) return GB_ERR;
    const TV_INIT_CAP = 256;
    const TV_GROW = 2;
    if (gb.dump_token_vec_count >= gb.dump_token_vec_cap) {
        const new_cap: c_int = if (gb.dump_token_vec_cap < TV_INIT_CAP) TV_INIT_CAP else gb.dump_token_vec_cap * TV_GROW;
        const grown = c.realloc(@ptrCast(gb.dump_token_vecs), @as(usize, @intCast(new_cap)) * @sizeOf(c.CBMDumpTokenVec)) orelse return GB_ERR;
        gb.dump_token_vecs = @ptrCast(@alignCast(grown));
        gb.dump_token_vec_cap = new_cap;
    }
    const vec_copy = dupVector(vector, @intCast(vector_len)) orelse return GB_ERR;

    const idx = gb.dump_token_vec_count;
    gb.dump_token_vecs.?[@intCast(idx)] = .{
        .id = idx + 1, // 1-based sequential ID
        .project = gb.project,
        .token = c.strdup(token),
        .vector = vec_copy,
        .vector_len = vector_len,
        .idf = idf,
    };
    gb.dump_token_vec_count += 1;
    return 0;
}

// ── ID accessors ───────────────────────────────────────────────────

pub export fn cbm_gbuf_next_id(gb_c: ?*const Gbuf) i64 {
    const gb = gb_c orelse return 1;
    if (gb.shared_ids) |ids| return ids.load(.seq_cst);
    return gb.next_id;
}

pub export fn cbm_gbuf_set_next_id(gb_c: ?*Gbuf, next_id: i64) void {
    const gb = gb_c orelse return;
    gb.next_id = next_id;
}

pub export fn cbm_gbuf_raise_next_id(gb_c: ?*Gbuf, next_id: i64) void {
    const gb = gb_c orelse return;
    if (next_id > gb.next_id) gb.next_id = next_id;
}

/// Sorts ids, counts each id an earlier entry already carries, frees ids.
fn countDuplicateIds(ids: []i64) c_int {
    std.mem.sortUnstable(i64, ids, {}, std.sort.asc(i64));
    var dups: c_int = 0;
    for (ids[1..], ids[0 .. ids.len - 1]) |id, prev| dups += @intFromBool(id == prev);
    c.free(ids.ptr);
    return dups;
}

pub export fn cbm_gbuf_duplicate_edge_ids(gb_c: ?*const Gbuf) c_int {
    const gb = gb_c orelse return 0;
    const edges = gb.edges.slice();
    if (edges.len < 2) return 0;
    const ids = callocSlice(i64, edges.len) orelse return NOT_FOUND;
    for (edges, ids) |e, *id| id.* = e.id;
    return countDuplicateIds(ids);
}

pub export fn cbm_gbuf_duplicate_node_ids(gb_c: ?*const Gbuf) c_int {
    const gb = gb_c orelse return 0;
    const nodes = gb.nodes.slice();
    if (nodes.len < 2) return 0;
    const ids = callocSlice(i64, nodes.len) orelse return NOT_FOUND;
    for (nodes, ids) |n, *id| id.* = n.id;
    return countDuplicateIds(ids);
}

// ── Node operations ────────────────────────────────────────────────

/// First key of the canonical same-QN collision order: how strong a claim the
/// entity has on a qualified name. A macro is the weakest: when a "Macro" and
/// a definition share a QN, the definition, which carries the body, the
/// signature and every edge, owns it whichever comes first.
///
/// C-preprocessor macros no longer get here: their QN carries its own
/// namespace (`...#macro`, CBM_MACRO_QN_SUFFIX) and cannot equal a definition's.
/// The case left is Chialisp, whose `defmacro` / `defmac` defs are labelled
/// "Macro" under a plain QN: a macro and a `defun` of one name in one file, or
/// in a same-stem `.clsp` / `.clib` pair (one module QN).
fn gbQnClaimRank(label: CStr) c_int {
    return if (label != null and streq(label, "Macro")) 0 else 1;
}

/// Is a per-file "Module" def arriving on a structural directory node
/// ("Project" root or "Folder") that shares its QN?
fn moduleOnContainer(existing_label: CStr, label: CStr) bool {
    return existing_label != null and label != null and streq(label, "Module") and
        (streq(existing_label, "Project") or streq(existing_label, "Folder"));
}

/// The canonical same-QN collision order: > 0 when the existing node wins
/// over the arriving one, 0 on a full tie. See cbm_gbuf_upsert_node.
fn collisionOrder(existing: *const Node, label: CStr, name: CStr, file_path: CStr, start_line: c_int) c_int {
    var order = gbQnClaimRank(existing.label) - gbQnClaimRank(label);
    if (order == 0) order = c.strcmp(orEmpty(file_path), orEmpty(existing.file_path));
    if (order == 0) order = existing.start_line -% start_line;
    if (order == 0) order = c.strcmp(orEmpty(existing.name), orEmpty(name));
    if (order == 0) order = c.strcmp(orEmpty(existing.label), orEmpty(label));
    return order;
}

pub export fn cbm_gbuf_upsert_node(gb_c: ?*Gbuf, label: ?Str, name: ?Str, qualified_name: ?Str, file_path: ?Str, start_line: c_int, end_line: c_int, properties_json: ?Str) i64 {
    const gb = gb_c orelse return 0;
    if (qualified_name == null) return 0;

    // Check if node already exists
    if (htGet(Node, gb.node_by_qn, qualified_name)) |existing| {
        // Don't let a per-file "Module" def touch a structural directory node
        // ("Project" root or "Folder"). In a directory-based-module language
        // (Go/Java) a file's module_qn equals its directory QN: a root file →
        // the project name (== the "Project" node's QN); a file in pkg/ →
        // proj.pkg (== the "pkg/" Folder node's QN). Its always-emitted Module
        // def collides here; the directory node is the package/module container
        // and must keep its structural label AND its own name/file_path/range.
        // Updating those in place set the shared node's file_path to whichever
        // same-package file happened to be processed LAST (worker-order
        // dependent) — the nondeterministic file attribution behind #787 — and
        // left the Folder node exposed to delete-nodes-by-file on incremental
        // reindex of that file. Skip the update entirely. (Both direct
        // upserts and the parallel local-gbuf merge route through this function.)
        if (moduleOnContainer(existing.label, label)) return existing.id;

        // Same-QN arrival: distinct source entities can share a QN (C: a
        // struct, a function and a macro with one name), and the same entity
        // can be re-upserted with fresh content. The old code let the LAST
        // arrival overwrite — under parallel extraction the merge order
        // varies run to run, so WHICH entity survived flickered (xfs:
        // Function-node count 4998 vs 5015 across two runs) and every
        // order-sensitive consumer downstream inherited it. Pick the
        // survivor by a canonical CONTENT rule instead, a pure function of
        // the two candidates: smallest file_path, then LARGEST start_line,
        // then largest name/label (a mixed-direction composite is still a
        // total order, so the pick is commutative and scheduling-free).
        // Line-descending within a file keeps the classic upsert contract —
        // a later definition in the same file (macro redefinition, refresh
        // of the same entity with new content) replaces the earlier one,
        // deterministically, because intra-file arrival order is fixed. A
        // full tie is the same entity re-upserted → refresh in place.
        // Ahead of all of these, a definition beats a macro
        // (gbQnClaimRank). Kind-disambiguated QNs (the real cure) remain
        // a follow-up.
        if (collisionOrder(existing, label, name, file_path, start_line) > 0) {
            return existing.id; // existing entity is the canonical winner
        }

        // Update in-place. name/properties are strdup'd BEFORE freeing old ones
        // (callers may pass existing.name as an argument). label/file_path are
        // interned: gbIntern returns a stable pool pointer (idempotent even when
        // label == existing.label), so the old value is replaced, never freed.
        // When the surviving label/name changes, keep the secondary indexes
        // consistent (the old code left the node listed under its ORIGINAL
        // label/name — cbm_gbuf_find_by_label/name then missed or mis-listed it,
        // which is how the flickering Function set reached the semantic pass).
        const new_name = dupOrEmptyJson(name);
        const new_props: [*c]u8 = if (properties_json != null) dupOrEmptyJson(properties_json) else null;
        const new_label = gbIntern(gb, label);
        const label_changed = existing.label == null or new_label == null or !streq(existing.label, new_label);
        const name_changed = existing.name == null or new_name == null or !streq(existing.name, new_name);
        if (label_changed) {
            removeNodeFromPtrArray(htGet(NodePtrArray, gb.nodes_by_label, orEmpty(existing.label)), existing.id);
        }
        if (name_changed) {
            removeNodeFromPtrArray(htGet(NodePtrArray, gb.nodes_by_name, orEmpty(existing.name)), existing.id);
        }
        existing.label = new_label;
        c.free(existing.name);
        existing.name = new_name;
        existing.file_path = gbIntern(gb, file_path);
        existing.start_line = start_line;
        existing.end_line = end_line;
        if (new_props != null) {
            c.free(existing.properties_json);
            existing.properties_json = new_props;
        }
        if (label_changed) {
            if (getOrCreateArray(NodePtrArray, gb.nodes_by_label, orEmpty(existing.label))) |by_label| by_label.push(existing);
        }
        if (name_changed) {
            if (getOrCreateArray(NodePtrArray, gb.nodes_by_name, orEmpty(existing.name))) |by_name| by_name.push(existing);
        }
        return existing.id;
    }

    // Heap-allocate a new node (pointer stays stable across array growth)
    const node: *Node = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Node)) orelse return 0));

    const id = allocNextId(gb);
    node.id = id;
    node.label = gbIntern(gb, label);
    node.name = dupOrEmptyJson(name);
    node.qualified_name = dupOrEmptyJson(qualified_name);
    node.file_path = gbIntern(gb, file_path);
    node.start_line = start_line;
    node.end_line = end_line;
    node.properties_json = dupOrEmptyJson(properties_json);

    // Store pointer in array and register in all indexes
    gb.nodes.push(node);
    registerNodeInIndexes(gb, node);

    return id;
}

pub export fn cbm_gbuf_find_by_qn(gb_c: ?*const Gbuf, qn: ?Str) ?*const Node {
    const gb = gb_c orelse return null;
    if (qn == null) return null;
    return htGet(Node, gb.node_by_qn, qn);
}

pub export fn cbm_gbuf_set_node_props(gb_c: ?*Gbuf, qualified_name: ?Str, properties_json: ?Str) bool {
    const gb = gb_c orelse return false;
    if (qualified_name == null or properties_json == null) return false;
    const node = htGet(Node, gb.node_by_qn, qualified_name) orelse return false;
    const new_props = dupOrEmptyJson(properties_json);
    if (new_props == null) return false;
    c.free(node.properties_json);
    node.properties_json = new_props;
    return true;
}

pub export fn cbm_gbuf_find_by_id(gb_c: ?*const Gbuf, id: i64) ?*const Node {
    const gb = gb_c orelse return null;
    const by_id = gb.by_id orelse return null;
    if (id < 0 or id >= gb.by_id_cap) return null;
    return by_id[@intCast(id)];
}

/// Hand a pointer array out through the find_* out parameters: NULL and 0
/// for a missing or empty array.
fn returnArray(comptime A: type, arr_opt: ?*const A, out: anytype, count: *c_int) void {
    if (arr_opt) |arr| {
        if (arr.count > 0) {
            out.* = arr.items;
            count.* = arr.count;
            return;
        }
    }
    out.* = null;
    count.* = 0;
}

pub export fn cbm_gbuf_find_by_label(gb_c: ?*const Gbuf, label: ?Str, out_c: ?*?[*]*const Node, count_c: ?*c_int) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const out = out_c orelse return NOT_FOUND;
    const count = count_c orelse return NOT_FOUND;
    returnArray(NodePtrArray, htGet(NodePtrArray, gb.nodes_by_label, label orelse ""), out, count);
    return 0;
}

pub export fn cbm_gbuf_find_by_name(gb_c: ?*const Gbuf, name: ?Str, out_c: ?*?[*]*const Node, count_c: ?*c_int) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const out = out_c orelse return NOT_FOUND;
    const count = count_c orelse return NOT_FOUND;
    returnArray(NodePtrArray, htGet(NodePtrArray, gb.nodes_by_name, name orelse ""), out, count);
    return 0;
}

pub export fn cbm_gbuf_node_count(gb_c: ?*const Gbuf) c_int {
    // Use QN hash table count since it's authoritative (handles deletes)
    const gb = gb_c orelse return 0;
    return @intCast(c.cbm_ht_count(gb.node_by_qn));
}

/// Mark a node id deleted in `deleted_set`.
fn markDeleted(deleted_set: ?*Ht, id: i64) void {
    var id_buf: [ID_KEY_BUF]u8 = undefined;
    makeIdKey(&id_buf, id);
    _ = c.cbm_ht_set(deleted_set, c.strdup(&id_buf), @ptrFromInt(1));
}

pub export fn cbm_gbuf_delete_by_label(gb_c: ?*Gbuf, label: ?Str) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (label == null) return NOT_FOUND;

    const arr = htGet(NodePtrArray, gb.nodes_by_label, label) orelse return 0;
    if (arr.count == 0) return 0;

    // Build hash set of deleted node IDs for O(1) lookup
    const deleted_set = c.cbm_ht_create(@intCast(arr.count));
    for (arr.slice()) |n| {
        markDeleted(deleted_set, n.id);

        // Remove from primary indexes
        _ = c.cbm_ht_delete(gb.node_by_qn, n.qualified_name);
        if (n.id >= 0 and n.id < gb.by_id_cap) gb.by_id.?[@intCast(n.id)] = null;
    }

    // Clear the label array
    arr.count = 0;

    // Cascade-delete edges referencing deleted nodes
    cascadeDeleteEdges(gb, deleted_set);

    freeIndex(deleted_set, freeKeyOnly);
    return 0;
}

pub export fn cbm_gbuf_delete_by_file(gb_c: ?*Gbuf, file_path: ?Str) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (file_path == null) return NOT_FOUND;

    // Collect IDs of nodes in this file
    const deleted_set = c.cbm_ht_create(64);
    var deleted_count: c_int = 0;
    var scanned: c_int = 0;

    for (gb.nodes.slice()) |n| {
        scanned += 1;
        if (n.file_path == null or !streq(n.file_path, file_path)) continue;
        if (!gb.isLive(n)) continue;

        markDeleted(deleted_set, n.id);

        // Remove from secondary indexes
        removeNodeFromPtrArray(htGet(NodePtrArray, gb.nodes_by_label, n.label), n.id);
        removeNodeFromPtrArray(htGet(NodePtrArray, gb.nodes_by_name, n.name), n.id);

        // Remove from primary indexes
        _ = c.cbm_ht_delete(gb.node_by_qn, n.qualified_name);
        if (n.id >= 0 and n.id < gb.by_id_cap) gb.by_id.?[@intCast(n.id)] = null;

        // NULL out QN so dump's liveness check (cbm_ht_get by QN) fails
        // even if a new node with the same QN is inserted later via merge.
        c.free(n.qualified_name);
        n.qualified_name = null;
        deleted_count += 1;
    }

    if (deleted_count == 0) {
        c.cbm_ht_free(deleted_set);
        return 0;
    }

    // Cascade-delete edges referencing deleted nodes
    cascadeDeleteEdges(gb, deleted_set);

    freeIndex(deleted_set, freeKeyOnly);
    {
        var s_buf: [16]u8 = undefined;
        var d_buf: [16]u8 = undefined;
        interop.logInfo("gbuf.delete_by_file", .{ "file", file_path, "scanned", fmtInt(&s_buf, scanned), "deleted", fmtInt(&d_buf, deleted_count) });
    }
    return deleted_count;
}

/// snprintf(buf, sizeof(buf), "%d", v) for a log value.
fn fmtInt(buf: *[16]u8, v: c_int) Str {
    _ = interop.bprint(buf, "{d}", .{v});
    return @ptrCast(buf);
}

fn gbufFolderMatchesAffectedPath(folder_path: CStr, affected_path: CStr) bool {
    if (folder_path == null or folder_path[0] == 0 or affected_path == null) return false;
    const folder = std.mem.span(folder_path);
    const affected = std.mem.span(affected_path);
    return std.mem.startsWith(u8, affected, folder) and
        (affected.len == folder.len or affected[folder.len] == '/');
}

fn gbufDeleteEmptyFoldersImpl(gb_c: ?*Gbuf, affected_paths: []const CStr, scoped: bool) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    var removed: c_int = 0;
    var changed = true;
    while (changed) {
        changed = false;
        var folders: ?[*]*const Node = null;
        var count: c_int = 0;
        if (cbm_gbuf_find_by_label(gb, "Folder", &folders, &count) != 0) break;
        const list: []const *const Node = if (folders) |f| f[0..@intCast(count)] else &.{};
        for (list) |folder| {
            if (folder.file_path == null or folder.qualified_name == null) continue;
            if (scoped) {
                const affected = for (affected_paths) |path| {
                    if (gbufFolderMatchesAffectedPath(folder.file_path, path)) break true;
                } else false;
                if (!affected) continue;
            }
            var edges: ?[*]*const Edge = null;
            var edge_count: c_int = 0;
            _ = cbm_gbuf_find_edges_by_source_type(gb, folder.id, "CONTAINS_FILE", &edges, &edge_count);
            if (edge_count > 0) continue;
            _ = cbm_gbuf_find_edges_by_source_type(gb, folder.id, "CONTAINS_FOLDER", &edges, &edge_count);
            if (edge_count > 0) continue;
            _ = cbm_gbuf_delete_by_file(gb, folder.file_path);
            removed += 1;
            changed = true;
            break;
        }
    }
    return removed;
}

pub export fn cbm_gbuf_delete_empty_folders(gb: ?*Gbuf) c_int {
    return gbufDeleteEmptyFoldersImpl(gb, &.{}, false);
}

pub export fn cbm_gbuf_delete_empty_folders_under(gb: ?*Gbuf, affected_paths_c: ?[*]const CStr, affected_count: c_int) c_int {
    const affected_paths = affected_paths_c orelse return 0;
    if (affected_count <= 0) return 0;
    return gbufDeleteEmptyFoldersImpl(gb, affected_paths[0..@intCast(affected_count)], true);
}

pub export fn cbm_gbuf_foreach_node(gb_c: ?*const Gbuf, fn_c: c.cbm_gbuf_node_visitor_fn, userdata: ?*anyopaque) void {
    const gb = gb_c orelse return;
    const visit = fn_c orelse return;
    for (gb.nodes.slice()) |n| {
        if (gb.isLive(n)) visit(n, userdata);
    }
}

pub export fn cbm_gbuf_foreach_edge(gb_c: ?*const Gbuf, fn_c: c.cbm_gbuf_edge_visitor_fn, userdata: ?*anyopaque) void {
    const gb = gb_c orelse return;
    const visit = fn_c orelse return;
    for (gb.edges.slice()) |e| visit(e, userdata);
}

// ── Edge operations ────────────────────────────────────────────────

/// "confidence":<double> from an edge property blob; -1 when absent or
/// unreadable, so a blob that states a confidence outranks one that does not.
/// strtod answers 0.0 for text it cannot read ("confidence":null), and 0.0 is a
/// real confidence, so an end pointer that never moved means absent.
fn edgePropertiesConfidence(props_json: CStr) f64 {
    const conf_key = "\"confidence\":";
    if (props_json == null) return -1.0;
    const p: [*c]const u8 = c.strstr(props_json, conf_key);
    if (p == null) return -1.0;
    const value: [*c]const u8 = p + conf_key.len;
    var end: [*c]u8 = null;
    const conf = c.strtod(value, &end);
    return if (end == value) -1.0 else conf;
}

/// A total order over the two candidates, so the result does not depend on
/// arrival order: higher confidence wins (an LSP-resolved call outranks a
/// textual match, upstream d08fccd9), then the smaller blob.
fn edgePropertiesShouldReplace(existing: CStr, candidate: CStr) bool {
    if (candidate == null or streq(candidate, "{}")) return false;
    if (existing == null or streq(existing, "{}")) return true;
    const cand_conf = edgePropertiesConfidence(candidate);
    const cur_conf = edgePropertiesConfidence(existing);
    if (cand_conf != cur_conf) return cand_conf > cur_conf;
    return c.strcmp(candidate, existing) < 0;
}

pub export fn cbm_gbuf_insert_edge(gb_c: ?*Gbuf, source_id: i64, target_id: i64, type_c: ?Str, properties_json: ?Str) i64 {
    const gb = gb_c orelse return 0;
    if (type_c == null) return 0;

    // Check for dedup
    var key: [EDGE_KEY_BUF]u8 = undefined;
    makeEdgeKey(&key, source_id, target_id, type_c, properties_json);

    if (htGet(Edge, gb.edge_by_key, &key)) |existing| {
        // Pick one canonical value so parallel worker merge order cannot
        // change the retained properties for a deduplicated edge.
        if (edgePropertiesShouldReplace(existing.properties_json, properties_json)) {
            const replacement = dupOrEmptyJson(properties_json);
            if (replacement != null) {
                c.free(existing.properties_json);
                existing.properties_json = replacement;
            }
        }
        return existing.id;
    }

    // Heap-allocate a new edge (pointer stays stable)
    const edge: *Edge = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Edge)) orelse return 0));

    const id = allocNextId(gb);
    edge.id = id;
    edge.source_id = source_id;
    edge.target_id = target_id;
    edge.type = gbIntern(gb, type_c);
    edge.properties_json = dupOrEmptyJson(properties_json);

    // Store pointer in array
    gb.edges.push(edge);

    // Dedup index
    _ = c.cbm_ht_set(gb.edge_by_key, c.strdup(&key), edge);

    // Secondary indexes
    registerEdgeInIndexes(gb, edge);

    return id;
}

pub export fn cbm_gbuf_find_edges_by_source_type(gb_c: ?*const Gbuf, source_id: i64, type_c: ?Str, out_c: ?*?[*]*const Edge, count_c: ?*c_int) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const out = out_c orelse return NOT_FOUND;
    const count = count_c orelse return NOT_FOUND;
    var key: [EDGE_KEY_BUF]u8 = undefined;
    makeSrcTypeKey(&key, source_id, type_c);
    returnArray(EdgePtrArray, htGet(EdgePtrArray, gb.edges_by_source_type, &key), out, count);
    return 0;
}

pub export fn cbm_gbuf_find_edges_by_target_type(gb_c: ?*const Gbuf, target_id: i64, type_c: ?Str, out_c: ?*?[*]*const Edge, count_c: ?*c_int) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const out = out_c orelse return NOT_FOUND;
    const count = count_c orelse return NOT_FOUND;
    var key: [EDGE_KEY_BUF]u8 = undefined;
    makeSrcTypeKey(&key, target_id, type_c);
    returnArray(EdgePtrArray, htGet(EdgePtrArray, gb.edges_by_target_type, &key), out, count);
    return 0;
}

pub export fn cbm_gbuf_find_edges_by_type(gb_c: ?*const Gbuf, type_c: ?Str, out_c: ?*?[*]*const Edge, count_c: ?*c_int) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const out = out_c orelse return NOT_FOUND;
    const count = count_c orelse return NOT_FOUND;
    returnArray(EdgePtrArray, htGet(EdgePtrArray, gb.edges_by_type, type_c), out, count);
    return 0;
}

pub export fn cbm_gbuf_edge_count(gb_c: ?*const Gbuf) c_int {
    const gb = gb_c orelse return 0;
    return gb.edges.count;
}

pub export fn cbm_gbuf_edge_count_by_type(gb_c: ?*const Gbuf, type_c: ?Str) c_int {
    const gb = gb_c orelse return 0;
    if (type_c == null) return 0;
    const arr = htGet(EdgePtrArray, gb.edges_by_type, type_c) orelse return 0;
    return arr.count;
}

pub export fn cbm_gbuf_delete_edges_by_type(gb_c: ?*Gbuf, type_c: ?Str) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (type_c == null) return NOT_FOUND;

    // Remove edges of the given type from array and dedup index
    const edges = gb.edges.slice();
    var write_idx: usize = 0;
    for (edges) |e| {
        if (streq(e.type, type_c)) {
            var key: [EDGE_KEY_BUF]u8 = undefined;
            makeEdgeKey(&key, e.source_id, e.target_id, e.type, e.properties_json);
            const ekey = c.cbm_ht_get_key(gb.edge_by_key, &key);
            _ = c.cbm_ht_delete(gb.edge_by_key, &key);
            c.free(@constCast(ekey));
            freeEdgeStrings(e);
            c.free(e);
        } else {
            edges[write_idx] = e;
            write_idx += 1;
        }
    }
    gb.edges.count = @intCast(write_idx);

    // Rebuild edge secondary indexes
    rebuildEdgeSecondaryIndexes(gb);

    return 0;
}

pub export fn cbm_gbuf_delete_edges_by_source_type(gb_c: ?*Gbuf, source_id: i64, type_c: ?Str) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (type_c == null) return NOT_FOUND;
    const edges = gb.edges.slice();
    var write_idx: usize = 0;
    for (edges) |e| {
        if (e.source_id == source_id and streq(e.type, type_c)) {
            unindexEdge(gb, e);
            freeEdgeStrings(e);
            c.free(e);
        } else {
            edges[write_idx] = e;
            write_idx += 1;
        }
    }
    gb.edges.count = @intCast(write_idx);
    rebuildEdgeSecondaryIndexes(gb);
    return 0;
}

pub export fn cbm_gbuf_delete_edges_by_type_unless(gb_c: ?*Gbuf, type_c: ?Str, keep_c: c.cbm_gbuf_edge_keep_fn, userdata: ?*anyopaque) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const keep = keep_c orelse return NOT_FOUND;
    if (type_c == null) return NOT_FOUND;
    const edges = gb.edges.slice();
    var write_idx: usize = 0;
    for (edges) |e| {
        if (streq(e.type, type_c) and !keep(@ptrCast(gb), e, userdata)) {
            unindexEdge(gb, e);
            freeEdgeStrings(e);
            c.free(e);
        } else {
            edges[write_idx] = e;
            write_idx += 1;
        }
    }
    if (write_idx != edges.len) {
        gb.edges.count = @intCast(write_idx);
        rebuildEdgeSecondaryIndexes(gb);
    }
    return 0;
}

// ── Merge ──────────────────────────────────────────────────────────

/// Free remap hash table entries (key = heap string, value = heap i64*).
fn freeRemapEntry(key: [*c]const u8, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    c.free(@constCast(key));
    c.free(val);
}

/// Handle QN collision: update dst node fields (src wins), record remap if IDs differ.
/// label/file_path are re-interned into dst's pool (sn's pointers belong to src).
fn mergeUpdateExisting(dst: *Gbuf, existing: *Node, sn: *const Node, remap: *?*Ht) void {
    // Same guard as cbm_gbuf_upsert_node: a per-file "Module" def coming from a
    // worker-local gbuf must not touch the structural directory node ("Project"
    // root or "Folder") that shares its QN in a directory-based-module language
    // (Java/Go). pass_structure seeds Folder/Project nodes on the MAIN gbuf
    // before the parallel extract, so every worker's always-emitted Module def
    // for that package collides here; unconditional "src wins" relabelled the
    // directory node to Module and set its file_path to whichever worker merged
    // LAST — the nondeterministic USAGE-source misattribution of #787. Keep the
    // structural node intact; the ID remap below still redirects the worker's
    // edges onto the canonical node.
    if (!moduleOnContainer(existing.label, sn.label)) {
        // Canonical collision winner (determinism) — mirrors
        // cbm_gbuf_upsert_node exactly. Distinct source entities can share a
        // QN (C: struct/function/macro with one name); unconditional "src
        // wins" made the survivor depend on worker merge order, flickering
        // the node set (and every downstream consumer) run to run. Winner =
        // smallest file_path, then LARGEST start_line, then largest
        // name/label — one total order, commutative, scheduling-free; a full
        // tie is the same entity → refresh from src. A definition beats a
        // macro ahead of all of these (gbQnClaimRank).
        const sn_wins = collisionOrder(existing, sn.label, sn.name, sn.file_path, sn.start_line) <= 0;
        if (sn_wins) {
            // Keep the secondary indexes consistent when the surviving
            // label/name changes (the old code left the node listed under its
            // original label/name, so find_by_label/name mis-listed it).
            const new_label = gbIntern(dst, sn.label);
            const label_changed = existing.label == null or new_label == null or !streq(existing.label, new_label);
            const name_changed = existing.name == null or sn.name == null or !streq(existing.name, sn.name);
            if (label_changed) {
                removeNodeFromPtrArray(htGet(NodePtrArray, dst.nodes_by_label, orEmpty(existing.label)), existing.id);
            }
            if (name_changed) {
                removeNodeFromPtrArray(htGet(NodePtrArray, dst.nodes_by_name, orEmpty(existing.name)), existing.id);
            }
            existing.label = new_label;
            c.free(existing.name);
            existing.name = dupOrEmptyJson(sn.name);
            existing.file_path = gbIntern(dst, sn.file_path);
            existing.start_line = sn.start_line;
            existing.end_line = sn.end_line;
            if (sn.properties_json != null) {
                c.free(existing.properties_json);
                existing.properties_json = dupOrEmptyJson(sn.properties_json);
            }
            if (label_changed) {
                if (getOrCreateArray(NodePtrArray, dst.nodes_by_label, orEmpty(existing.label))) |by_label| by_label.push(existing);
            }
            if (name_changed) {
                if (getOrCreateArray(NodePtrArray, dst.nodes_by_name, orEmpty(existing.name))) |by_name| by_name.push(existing);
            }
        }
    }

    if (sn.id != existing.id) {
        if (remap.* == null) remap.* = c.cbm_ht_create(32);
        var key: [ID_KEY_BUF]u8 = undefined;
        makeIdKey(&key, sn.id);
        const val: *i64 = @ptrCast(@alignCast(c.malloc(@sizeOf(i64)) orelse return));
        val.* = existing.id;
        _ = c.cbm_ht_set(remap.*, c.strdup(&key), val);
    }
}

/// Copy a non-colliding src node into dst with its original ID.
fn mergeCopyNewNode(dst: *Gbuf, sn: *const Node) void {
    const node: *Node = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Node)) orelse return));

    node.id = sn.id;
    node.label = gbIntern(dst, sn.label);
    node.name = dupOrEmptyJson(sn.name);
    node.qualified_name = dupOrEmptyJson(sn.qualified_name);
    node.file_path = gbIntern(dst, sn.file_path);
    node.start_line = sn.start_line;
    node.end_line = sn.end_line;
    node.properties_json = dupOrEmptyJson(sn.properties_json);

    dst.nodes.push(node);
    registerNodeInIndexes(dst, node);

    if (node.id >= dst.next_id) dst.next_id = node.id + 1;
}

/// The collision remap's replacement for `id`, or `id` itself.
fn remappedId(remap: ?*Ht, id: i64) i64 {
    var key: [ID_KEY_BUF]u8 = undefined;
    makeIdKey(&key, id);
    const remapped = htGet(i64, remap, &key) orelse return id;
    return remapped.*;
}

/// Remap edge IDs using the collision remap table and insert into dst.
fn mergeRemapEdges(dst: *Gbuf, src: *Gbuf, remap: ?*Ht) void {
    for (src.edges.slice()) |se| {
        var new_src = se.source_id;
        var new_tgt = se.target_id;

        if (remap != null) {
            new_src = remappedId(remap, se.source_id);
            new_tgt = remappedId(remap, se.target_id);
        }

        _ = cbm_gbuf_insert_edge(dst, new_src, new_tgt, se.type, se.properties_json);
    }
}

pub export fn cbm_gbuf_merge(dst_c: ?*Gbuf, src_c: ?*Gbuf) c_int {
    const dst = dst_c orelse return NOT_FOUND;
    const src = src_c orelse return NOT_FOUND;
    if (src.nodes.count == 0 and src.edges.count == 0) return 0;

    // ID remap for QN-colliding nodes: "src_id" → (i64*) dst_id.
    // Only populated when a src node's QN already exists in dst.
    var remap: ?*Ht = null;

    for (src.nodes.slice()) |sn| {
        // Skip nodes deleted from QN index
        if (!src.isLive(sn)) continue;

        if (htGet(Node, dst.node_by_qn, sn.qualified_name)) |existing| {
            mergeUpdateExisting(dst, existing, sn, &remap);
        } else {
            mergeCopyNewNode(dst, sn);
        }
    }

    // Merge edges with optional ID remapping
    mergeRemapEdges(dst, src, remap);

    if (remap != null) freeIndex(remap, freeRemapEntry);

    return 0;
}

// ── Dump / Flush ───────────────────────────────────────────────────

/// Extract a string property from a properties JSON string.
/// Returns heap-allocated string or NULL. Caller must free.
/// Parses real JSON: the dump writer feeds these values into indexes whose
/// backing columns are GENERATED AS json_extract(properties,'$.<key>').
/// Naive byte slicing returned the ESCAPED text (and cut at embedded \")
/// while json_extract yields the unescaped value — the mismatch left rows
/// "missing from index idx_edges_url_path" under PRAGMA integrity_check.
/// key_quoted ("\"key\"") is a fast pre-filter to skip the JSON parse.
fn extractPropString(props: CStr, key_quoted: Str, key: Str) [*c]u8 {
    if (props == null or c.strstr(props, key_quoted) == null) return null;
    const doc = c.yyjson_read(props, c.strlen(props), 0) orelse return null;
    defer c.yyjson_doc_free(doc);
    const v = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), key);
    if (v == null or !c.yyjson_is_str(v)) return null;
    const sv = c.yyjson_get_str(v);
    return c.cbm_strndup(sv, c.strlen(sv));
}

fn extractUrlPath(props: CStr) [*c]u8 {
    return extractPropString(props, "\"url_path\"", "url_path");
}

/// local_name feeds the hand-built sqlite_autoindex_edges_1 — its backing
/// column local_name_gen is GENERATED only for IMPORTS edges (#768).
fn extractLocalName(props: CStr) [*c]u8 {
    return extractPropString(props, "\"local_name\"", "local_name");
}

/// Remap a temp ID to its final sequential ID, or 0 if out of range.
fn remapId(temp_to_final: []const i64, temp_id: i64) i64 {
    return if (temp_id >= 0 and temp_id < temp_to_final.len) temp_to_final[@intCast(temp_id)] else 0;
}

fn cmpDumpVectorsById(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const da = @as(*const c.CBMDumpVector, @ptrCast(@alignCast(a))).node_id;
    const db = @as(*const c.CBMDumpVector, @ptrCast(@alignCast(b))).node_id;
    return @as(c_int, @intFromBool(da > db)) - @as(c_int, @intFromBool(da < db));
}

/// Build dump-ready node array with sequential IDs. Populates temp_to_final
/// mapping. NULL on OOM.
fn buildDumpNodes(gb: *Gbuf, live_count: c_int, temp_to_final: []i64) ?[]c.CBMDumpNode {
    const cap: usize = @intCast(if (live_count > 0) live_count else 1);
    const dump_nodes = callocSlice(c.CBMDumpNode, cap) orelse return null;
    var idx: usize = 0;

    for (gb.nodes.slice()) |n| {
        if (!gb.isLive(n)) continue;

        const final_id: i64 = @intCast(idx + 1); // 1-based sequential
        if (n.id >= 0 and n.id < temp_to_final.len) temp_to_final[@intCast(n.id)] = final_id;

        dump_nodes[idx] = .{
            .id = final_id,
            .project = gb.project,
            .label = n.label,
            .name = n.name,
            .qualified_name = n.qualified_name,
            .file_path = orEmpty(n.file_path),
            .start_line = n.start_line,
            .end_line = n.end_line,
            .properties = if (n.properties_json != null) n.properties_json else "{}",
        };
        idx += 1;
    }

    return dump_nodes[0..idx];
}

/// Build dump-ready edge array with remapped IDs. Each edge's url_path and
/// local_name are heap strings the caller owns (free with
/// freeRetainedDumpEdges). NULL on OOM.
fn buildDumpEdges(gb: *Gbuf, temp_to_final: []const i64) ?[]c.CBMDumpEdge {
    // Count valid edges (both endpoints resolved)
    var valid_edges: usize = 0;
    for (gb.edges.slice()) |e| {
        if (remapId(temp_to_final, e.source_id) > 0 and remapId(temp_to_final, e.target_id) > 0) valid_edges += 1;
    }

    const dump_edges = callocSlice(c.CBMDumpEdge, @max(valid_edges, 1)) orelse return null;
    var idx: usize = 0;

    for (gb.edges.slice()) |e| {
        const src = remapId(temp_to_final, e.source_id);
        const tgt = remapId(temp_to_final, e.target_id);
        if (src == 0 or tgt == 0) continue;

        const url_path = extractUrlPath(e.properties_json);

        // IMPORTS only — mirrors the local_name_gen CASE in the edges DDL.
        const local_name: [*c]u8 = if (e.type != null and streq(e.type, "IMPORTS")) extractLocalName(e.properties_json) else null;

        dump_edges[idx] = .{
            .id = @intCast(idx + 1),
            .project = gb.project,
            .source_id = src,
            .target_id = tgt,
            .type = e.type,
            .properties = if (e.properties_json != null) e.properties_json else "{}",
            .url_path = url_path,
            .local_name = local_name,
        };
        idx += 1;
    }

    return dump_edges[0..idx];
}

/// Remap vector node IDs through temp_to_final, sort by ID, deduplicate.
fn remapSortDedupVectors(gb: *Gbuf, temp_to_final: []const i64) void {
    const vectors = gb.dumpVectors();
    var remapped: usize = 0;
    var dropped: c_int = 0;
    for (vectors) |v| {
        const new_id = if (v.node_id > 0) remapId(temp_to_final, v.node_id) else 0;
        if (new_id > 0) {
            vectors[remapped] = v;
            vectors[remapped].node_id = new_id;
            remapped += 1;
        } else {
            dropped += 1;
        }
    }
    if (dropped > 0) {
        var r_buf: [16]u8 = undefined;
        var d_buf: [16]u8 = undefined;
        interop.logInfo("dump.vectors.remap", .{ "remapped", fmtInt(&r_buf, @intCast(remapped)), "dropped", fmtInt(&d_buf, dropped) });
    }
    gb.dump_vector_count = @intCast(remapped);

    if (remapped >= GB_MIN_FOR_DEDUP) {
        const kept = vectors[0..remapped];
        c.qsort(kept.ptr, kept.len, @sizeOf(c.CBMDumpVector), cmpDumpVectorsById);
        var deduped: usize = 0;
        for (kept, 0..) |v, i| {
            if (i + GB_DEDUP_LOOKAHEAD < kept.len and v.node_id == kept[i + GB_DEDUP_LOOKAHEAD].node_id) continue;
            kept[deduped] = v;
            deduped += 1;
        }
        gb.dump_vector_count = @intCast(deduped);
    }
}

fn logDumpSummary(node_count: usize, edge_count: usize) void {
    var b1: [16]u8 = undefined;
    var b2: [16]u8 = undefined;
    interop.logInfo("gbuf.dump", .{ "nodes", fmtInt(&b1, @intCast(node_count)), "edges", fmtInt(&b2, @intCast(edge_count)) });
}

fn freeRetainedDumpEdges(edges_opt: ?[*]c.CBMDumpEdge, edge_count: c_int) void {
    const edges = edges_opt orelse return;
    for (edges[0..@intCast(edge_count)]) |e| {
        c.free(@constCast(e.url_path));
        c.free(@constCast(e.local_name));
    }
    c.free(@ptrCast(edges));
}

/// Give every edge an owned url_path and local_name: the extracted value, or
/// an owned "" where the property was absent. On OOM the strings are freed
/// and the caller frees the array.
fn retainDumpEdgeStrings(edges: []c.CBMDumpEdge) bool {
    for (edges, 0..) |*e, i| {
        if (e.url_path == null) e.url_path = c.strdup("");
        if (e.local_name == null) e.local_name = c.strdup("");
        if (e.url_path == null or e.local_name == null) {
            for (edges[0 .. i + 1]) |done| {
                c.free(@constCast(done.url_path));
                c.free(@constCast(done.local_name));
            }
            for (edges[i + 1 ..]) |rest| {
                c.free(@constCast(rest.url_path));
                c.free(@constCast(rest.local_name));
            }
            return false;
        }
    }
    return true;
}

fn countLiveNodes(gb: *Gbuf) c_int {
    var count: c_int = 0;
    for (gb.nodes.slice()) |n| count += @intFromBool(gb.isLive(n));
    return count;
}

fn generateIsoTimestamp(buf: []u8) void {
    var now: c.time_t = c.time(null);
    var tm_buf: c.struct_tm = undefined;
    const tm_val = c.cbm_gmtime_r(&now, &tm_buf);
    if (c.strftime(buf.ptr, buf.len, "%Y-%m-%dT%H:%M:%SZ", tm_val) == 0) {
        _ = interop.bprint(buf, "1970-01-01T00:00:00Z", .{});
    }
}

/// CBM_PROF_START / CBM_PROF_END / CBM_PROF_END_N from foundation/profile.h.
const Prof = struct {
    start: c.struct_timespec = undefined,

    fn begin() Prof {
        var prof: Prof = .{};
        if (c.cbm_profile_active) c.cbm_profile_now(&prof.start);
        return prof;
    }

    fn end(prof: *const Prof, phase: Str, sub: Str, items: c_long) void {
        if (c.cbm_profile_active) c.cbm_profile_log_elapsed(phase, sub, &prof.start, items);
    }
};

/// Build the final node, edge and vector arrays published to cbm.zova.
fn gbufBuildDump(gb_c: ?*Gbuf) c_int {
    const gb = gb_c orelse return NOT_FOUND;

    const t_count = Prof.begin();
    const live_count = countLiveNodes(gb);
    t_count.end("dump", "1_count_live_nodes", live_count);

    const t_build_nodes = Prof.begin();
    const max_temp_id: usize = @intCast(gb.next_id);
    const temp_to_final = callocSlice(i64, max_temp_id) orelse return NOT_FOUND;
    defer c.free(temp_to_final.ptr);

    const dump_nodes = buildDumpNodes(gb, live_count, temp_to_final) orelse return NOT_FOUND;
    t_build_nodes.end("dump", "2_build_dump_nodes", @intCast(dump_nodes.len));

    // Release ALL lookup indexes NOW: nothing between here and finalize reads
    // them (buildDumpEdges uses temp_to_final + gb.edges, the vector remap
    // uses temp_to_final). At kernel scale they are ~3.8 GB (hash buckets +
    // key strings + pointer arrays); releasing them only AFTER edge building
    // made them coexist with every dump-side transient array — the dump-phase
    // RSS peak.
    const t_release_idx = Prof.begin();
    releaseGbufIndexes(gb);
    t_release_idx.end("dump", "4_release_gbuf_indexes", 0);

    generateIsoTimestamp(&gb.indexed_at);

    const t_build_edges = Prof.begin();
    const dump_edges = buildDumpEdges(gb, temp_to_final) orelse {
        c.free(dump_nodes.ptr);
        return NOT_FOUND;
    };
    t_build_edges.end("dump", "3_build_dump_edges", @intCast(dump_edges.len));

    // Remap+sort+dedup vectors to the final node ids.
    const t_vec_remap = Prof.begin();
    remapSortDedupVectors(gb, temp_to_final);
    t_vec_remap.end("dump", "5_vector_remap_sort", gb.dump_vector_count);

    const retained = retainDumpEdgeStrings(dump_edges);
    logDumpSummary(dump_nodes.len, dump_edges.len);
    if (!retained) {
        c.free(dump_edges.ptr);
        c.free(dump_nodes.ptr);
        return NOT_FOUND;
    }

    c.free(@ptrCast(gb.zova_dump_nodes));
    freeRetainedDumpEdges(gb.zova_dump_edges, gb.zova_dump_edge_count);
    gb.zova_dump_nodes = dump_nodes.ptr;
    gb.zova_dump_node_count = @intCast(dump_nodes.len);
    gb.zova_dump_edges = dump_edges.ptr;
    gb.zova_dump_edge_count = @intCast(dump_edges.len);
    return 0;
}

/// The generation described by the prepared dump arrays.
fn generationInput(gb: *const Gbuf, indexed_at: CStr) c.cbm_zova_workspace_generation_input_t {
    return .{
        .root_path = gb.root_path,
        .project = gb.project,
        .indexed_at = indexed_at,
        .model_fingerprint = c.CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = c.CBM_SEM_DIM,
        .nodes = gb.zova_dump_nodes,
        .node_count = gb.zova_dump_node_count,
        .edges = gb.zova_dump_edges,
        .edge_count = gb.zova_dump_edge_count,
        .node_vectors = gb.dump_vectors,
        .node_vector_count = gb.dump_vector_count,
        .token_vectors = gb.dump_token_vecs,
        .token_vector_count = gb.dump_token_vec_count,
    };
}

/// generationInput plus the file hashes and project summary, stamped with the
/// dump's timestamp (or a fresh one when no dump was prepared).
fn publishInput(gb: *const Gbuf, indexed_at: *[64]u8, file_hashes: ?[*]const c.cbm_zova_file_hash_input_t, file_hash_count: c_int, project_summary: ?*const c.cbm_zova_project_summary_input_t) c.cbm_zova_workspace_generation_input_t {
    if (gb.indexed_at[0] != 0) {
        indexed_at.* = gb.indexed_at;
    } else {
        generateIsoTimestamp(indexed_at);
    }
    var input = generationInput(gb, indexed_at);
    input.file_hashes = file_hashes;
    input.file_hash_count = file_hash_count;
    if (project_summary) |summary| input.project_summary = summary.*;
    return input;
}

pub export fn cbm_gbuf_prepare_zova_dump(gb_c: ?*Gbuf) c_int {
    const rc = gbufBuildDump(gb_c);
    if (rc != 0) return rc;
    const gb = gb_c.?;
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    const input = generationInput(gb, &gb.indexed_at);
    var view: ?*c.cbm_zova_prepared_view_t = null;
    if (c.cbm_zova_workspace_id_for_root(gb.root_path, &workspace_id, workspace_id.len) != 0 or
        c.cbm_zova_prepared_view_build_base(&workspace_id, &input, &view) != 0)
    {
        return NOT_FOUND;
    }
    c.cbm_zova_prepared_view_free(gb.zova_prepared_view);
    gb.zova_prepared_view = view;
    return 0;
}

pub export fn cbm_gbuf_prepare_zova_dump_delta(gb: ?*Gbuf) c_int {
    return gbufBuildDump(gb);
}

pub export fn cbm_gbuf_publish_zova_user_database(gb_c: ?*Gbuf, file_hashes: ?[*]const c.cbm_zova_file_hash_input_t, file_hash_count: c_int, project_summary: ?*const c.cbm_zova_project_summary_input_t, out_result: ?*c.cbm_zova_workspace_generation_result_t) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (out_result == null or file_hash_count < 0 or (file_hash_count > 0 and file_hashes == null)) {
        return NOT_FOUND;
    }
    var user_database_path: [c.CBM_SZ_1K]u8 = undefined;
    if (c.cbm_zova_workspace_registry_path(&user_database_path, user_database_path.len) != 0) {
        return NOT_FOUND;
    }
    var indexed_at: [64]u8 = undefined;
    const input = publishInput(gb, &indexed_at, file_hashes, file_hash_count, project_summary);
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    var view = gb.zova_prepared_view;
    var owned_view = false;
    var rc = c.cbm_zova_workspace_id_for_root(input.root_path, &workspace_id, workspace_id.len);
    if (rc == 0 and view == null) {
        rc = c.cbm_zova_prepared_view_build(&workspace_id, &input, &view);
        owned_view = rc == 0;
    } else if (rc == 0) {
        rc = c.cbm_zova_prepared_view_complete(view, file_hashes, file_hash_count, project_summary);
    }
    if (rc == 0) rc = c.cbm_zova_user_database_publish_prepared_view(&user_database_path, view, out_result);
    if (owned_view) c.cbm_zova_prepared_view_free(view);
    return rc;
}

pub export fn cbm_gbuf_publish_zova_user_database_delta(gb_c: ?*const Gbuf, before_c: ?*c.cbm_zova_workspace_snapshot_t, file_hashes: ?[*]const c.cbm_zova_file_hash_input_t, file_hash_count: c_int, project_summary: ?*const c.cbm_zova_project_summary_input_t, out_result: ?*c.cbm_zova_workspace_generation_result_t) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    const before = before_c orelse return NOT_FOUND;
    if (out_result == null or file_hash_count < 0 or (file_hash_count > 0 and file_hashes == null)) {
        return NOT_FOUND;
    }
    var user_database_path: [c.CBM_SZ_1K]u8 = undefined;
    if (c.cbm_zova_workspace_registry_path(&user_database_path, user_database_path.len) != 0) {
        return NOT_FOUND;
    }
    var indexed_at: [64]u8 = undefined;
    const input = publishInput(gb, &indexed_at, file_hashes, file_hash_count, project_summary);
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    var model: ?*c.cbm_zova_publish_model_t = null;
    var delta: ?*c.cbm_zova_workspace_delta_t = null;
    var rc = c.cbm_zova_workspace_id_for_root(input.root_path, &workspace_id, workspace_id.len);
    if (rc == 0 and c.strcmp(&workspace_id, &before.workspace_id) != 0) rc = -1;
    if (rc == 0) rc = c.cbm_zova_publish_model_build(&workspace_id, &input, &model);
    if (rc == 0 and gb.reuse_zova_semantic_vectors) {
        rc = c.cbm_zova_publish_model_reuse_vector_digests(model, &before.integrity, c.CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
            c.CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS);
    }
    var required: c.cbm_zova_snapshot_components_t = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    if (rc == 0) rc = c.cbm_zova_workspace_delta_required_components(before, model, &required);
    if (rc == 0 and required != c.CBM_ZOVA_SNAPSHOT_COMPONENT_NONE) {
        rc = c.cbm_zova_repository_hydrate_incremental_components(&user_database_path, &workspace_id, before.generation, required, before);
    }
    if (rc == 0) rc = c.cbm_zova_workspace_delta_build(before, model, &delta);
    if (rc == 0) rc = c.cbm_zova_user_database_publish_delta(&user_database_path, model, delta, out_result);
    c.cbm_zova_workspace_delta_free(delta);
    c.cbm_zova_publish_model_free(model);
    return rc;
}

/// Upsert every live node and insert every edge whose endpoints were written,
/// mapping buffer ids to the ids the store assigned.
fn writeToStore(gb: *Gbuf, store: ?*c.cbm_store_t, temp_to_real: []i64) void {
    for (gb.nodes.slice()) |n| {
        // Skip if deleted from QN index
        if (!gb.isLive(n)) continue;

        const sn: c.cbm_node_t = .{
            .project = gb.project,
            .label = n.label,
            .name = n.name,
            .qualified_name = n.qualified_name,
            .file_path = n.file_path,
            .start_line = n.start_line,
            .end_line = n.end_line,
            .properties_json = n.properties_json,
        };
        const real_id = c.cbm_store_upsert_node(store, &sn);
        if (real_id > 0 and n.id >= 0 and n.id < temp_to_real.len) temp_to_real[@intCast(n.id)] = real_id;
    }

    // Insert all edges with remapped IDs
    for (gb.edges.slice()) |e| {
        const real_src = remapId(temp_to_real, e.source_id);
        const real_tgt = remapId(temp_to_real, e.target_id);
        if (real_src == 0 or real_tgt == 0) continue;

        const se: c.cbm_edge_t = .{
            .project = gb.project,
            .source_id = real_src,
            .target_id = real_tgt,
            .type = e.type,
            .properties_json = e.properties_json,
        };
        _ = c.cbm_store_insert_edge(store, &se);
    }
}

/// temp_id → real_id map. Temp IDs start at 1 and are sequential, but can
/// have gaps from edge inserts, so the map is sized by next_id.
fn allocTempToReal(gb: *const Gbuf) ?[]i64 {
    return callocSlice(i64, @intCast(gb.next_id));
}

pub export fn cbm_gbuf_flush_to_store(gb_c: ?*Gbuf, store: ?*c.cbm_store_t) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (store == null) return NOT_FOUND;
    const temp_to_real = allocTempToReal(gb) orelse return NOT_FOUND;
    defer c.free(temp_to_real.ptr);

    // Upsert project
    _ = c.cbm_store_upsert_project(store, gb.project, gb.root_path);

    // Begin bulk mode
    _ = c.cbm_store_begin_bulk(store);
    _ = c.cbm_store_drop_indexes(store);
    _ = c.cbm_store_begin(store);

    // Delete existing project data
    _ = c.cbm_store_delete_edges_by_project(store, gb.project);
    _ = c.cbm_store_delete_nodes_by_project(store, gb.project);

    writeToStore(gb, store, temp_to_real);

    _ = c.cbm_store_commit(store);
    _ = c.cbm_store_create_indexes(store);
    _ = c.cbm_store_end_bulk(store);
    return 0;
}

pub export fn cbm_gbuf_merge_into_store(gb_c: ?*Gbuf, store: ?*c.cbm_store_t) c_int {
    const gb = gb_c orelse return NOT_FOUND;
    if (store == null) return NOT_FOUND;
    const temp_to_real = allocTempToReal(gb) orelse return NOT_FOUND;
    defer c.free(temp_to_real.ptr);

    // Begin bulk mode — no project wipe
    _ = c.cbm_store_begin(store);
    writeToStore(gb, store, temp_to_real);
    _ = c.cbm_store_commit(store);
    return 0;
}
