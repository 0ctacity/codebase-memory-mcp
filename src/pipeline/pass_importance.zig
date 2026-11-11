//! pass_importance.zig — Index-time per-symbol importance score (weighted degree).
//!
//! Predump pass. Computes an importance score for every Function/Method/Class
//! node and stores it as a numeric "importance" key inside the node's EXISTING
//! properties_json blob — no new column, no schema change, and therefore no
//! CBM_INDEX_FORMAT_VERSION bump. Indexes written by older builds simply lack
//! the key; every consumer must tolerate its absence.
//!
//! Model (weighted degree, Aider-repomap shaped):
//!
//!   importance = sqrt(num_refs) * priv * generic * distinct * test_penalty
//!
//!     num_refs      incoming CALLS + USAGE edges. 0 -> sqrt(0) = 0.
//!     priv     0.1  name is private (leading underscore)
//!     generic  0.1  the name is defined in >= 5 DISTINCT files
//!     distinct 10   name is snake_case or camelCase AND len >= 8
//!     test_penalty
//!              0.1  the symbol is test scaffolding — either the target of an
//!                   incoming TESTS edge (a production helper exercised by a
//!                   test), or it lives in a test file per the graph's own
//!                   canonical cbm_is_test_path() classifier (the same one
//!                   pass_tests uses).
//!
//! Transitive importance (PageRank) is deliberately NOT built here: weighted
//! degree is what this pass claims, and a transitive refinement has to earn
//! itself against a fixed judgment set before it ships.
//!
//! ── Cost ─────────────────────────────────────────────────────────────
//! The generic-name multiplier needs |{ file a name is defined in }|. The
//! obvious shape — for each node, walk its whole same-name group and
//! pairwise-compare file paths — is O(k^2) per node and therefore O(k^3) per
//! same-name group of size k. That is not a theoretical concern: on a large
//! Java corpus a single name group (`toString`, k≈4.6k) costs ~2 minutes, and
//! a handful of such names alone multiplies total index time several-fold.
//!
//! This implementation is linear in the graph instead:
//!   - the distinct-file count is computed ONCE PER DISTINCT NAME and memoized,
//!     so a group of size k is scanned once, not k times;
//!   - within a group, file paths are deduplicated through a hash set rather
//!     than pairwise strcmp.
//! Total distinct-file work is therefore Σ over distinct names of k = O(N).
//! Both gbuf lookups it rests on (find_by_name, find_edges_by_target_type) are
//! hash-indexed, so the per-node loop is O(1) amortised per node.
//!
//! g_importance_name_visits counts same-name-group member visits and is GATED
//! in the importance suite: it must stay linear (ratio ~2) under a corpus
//! doubling. A regression to the per-node scan lands at ~4 and fails the gate.
//!
//! ── Ordering (load-bearing) ──────────────────────────────────────────
//! Registered LAST among the predump passes, so every edge type the score
//! reads (CALLS, USAGE from extraction; TESTS from pass_tests) already exists.
//! A pass ordered earlier would silently score everything at num_refs = 0 with
//! no test penalty. In the incremental post-pass sequence it runs right after
//! complexity and before the derived-state digest, which hashes
//! properties_json: scoring after the digest would make every incremental run
//! look changed and defeat the similarity/semantic reuse gates.
//!
//! Ported from upstream e5f3aab0. Upstream's closure-delta store rescore
//! (396ac348) does not apply here: the fork's incremental route always scores
//! a whole-project buffer exported from the committed Zova generation, with
//! inbound edges relinked before the post-passes.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;
const HashTable = c.CBMHashTable;
const isSpace = std.ascii.isWhitespace;

/// Work counters — deltas are read by the importance suite's linearity gate.
/// Never reset here: callers snapshot before/after so nested runs compose.
/// Declared `extern _Atomic uint64_t` in pipeline_internal.h.
export var g_importance_nodes: u64 = 0;
export var g_importance_name_visits: u64 = 0;

const GENERIC_MIN_FILES = 5; // name defined in >= N files -> generic
const DISTINCT_MIN_LEN = 8; // distinctive-identifier length floor
const PRIV_MUL: f64 = 0.1;
const GENERIC_MUL: f64 = 0.1;
const DISTINCT_MUL: f64 = 10.0;
const TEST_MUL: f64 = 0.1;

/// The node labels that carry an importance score. File/Module and other
/// non-symbol nodes are deliberately excluded.
const IMPORTANCE_LABELS = [_][*:0]const u8{ "Function", "Method", "Class" };

/// Int → string for structured logging (thread-local ring buffer).
threadlocal var itoa_bufs: [2][c.CBM_SZ_32]u8 = undefined;
threadlocal var itoa_idx: usize = 0;

fn itoaImp(val: u64) CStr {
    const i = itoa_idx;
    itoa_idx = (itoa_idx + 1) & 1;
    _ = c.snprintf(&itoa_bufs[i], itoa_bufs[i].len, "%llu", @as(c_ulonglong, val));
    return &itoa_bufs[i];
}

fn nameIsPrivate(name: CStr) bool {
    return name != null and name[0] == '_';
}

/// Distinctive = (snake_case OR camelCase) AND length >= 8. snake_case is an
/// embedded '_'; camelCase is a lower->upper "hump". A plain len>=8 lowercase
/// word (no '_', no hump) is NOT distinctive.
fn nameIsDistinctive(name: CStr) bool {
    if (name == null) return false;
    const len = c.strlen(name);
    if (len < DISTINCT_MIN_LEN) return false;
    if (c.strchr(name, '_') != null) return true; // snake_case
    for (1..len) |i| {
        if (std.ascii.isLower(name[i - 1]) and std.ascii.isUpper(name[i])) {
            return true; // camelCase hump
        }
    }
    return false;
}

// ── JSON property write-back ────────────────────────────────────────
//
// properties_json is a flat object here (same assumption the sibling passes'
// jsonGetInt/jsonGetBool helpers make), but the value scanner below is
// written to survive nested values so a future richer blob cannot be
// truncated by an in-place overwrite.

/// Return a pointer just past the JSON value beginning at p (first non-space
/// char of the value). NULL if the value is unterminated/malformed.
fn impValueEnd(p_in: [*]const u8) ?[*]const u8 {
    var p = p_in;
    if (p[0] == '"') {
        p += 1;
        while (p[0] != 0) {
            if (p[0] == '\\' and p[1] != 0) {
                p += 2;
                continue;
            }
            if (p[0] == '"') return p + 1;
            p += 1;
        }
        return null;
    }
    if (p[0] == '{' or p[0] == '[') {
        var depth: c_int = 0;
        var in_str = false;
        while (p[0] != 0) {
            if (in_str) {
                if (p[0] == '\\' and p[1] != 0) {
                    p += 2;
                    continue;
                }
                if (p[0] == '"') in_str = false;
            } else if (p[0] == '"') {
                in_str = true;
            } else if (p[0] == '{' or p[0] == '[') {
                depth += 1;
            } else if (p[0] == '}' or p[0] == ']') {
                depth -= 1;
                if (depth == 0) return p + 1;
            }
            p += 1;
        }
        return null;
    }
    while (p[0] != 0 and p[0] != ',' and p[0] != '}' and p[0] != ']') p += 1;
    return p;
}

/// Find `"key"` where it is genuinely in KEY position: preceded (modulo
/// whitespace) by '{' or ',' and followed (modulo whitespace) by ':'. A bare
/// strstr would also match the text inside a string VALUE. Returns a pointer
/// to the opening quote, or NULL.
fn impFindKey(json: [*:0]const u8, key: CStr) ?[*]const u8 {
    var pat: [c.CBM_SZ_64]u8 = undefined;
    const n_c = c.snprintf(&pat, pat.len, "\"%s\"", key);
    if (n_c < 0 or @as(usize, @intCast(n_c)) >= pat.len) return null;
    const n: usize = @intCast(n_c);
    var p: CStr = c.strstr(json, &pat);
    while (p != null) : (p = c.strstr(p + n, &pat)) {
        var before: [*]const u8 = p;
        while (@intFromPtr(before) > @intFromPtr(json) and isSpace((before - 1)[0])) before -= 1;
        const prev: u8 = if (@intFromPtr(before) > @intFromPtr(json)) (before - 1)[0] else 0;
        var after: [*]const u8 = p + n;
        while (isSpace(after[0])) after += 1;
        if ((prev == '{' or prev == ',') and after[0] == ':') return p;
    }
    return null;
}

/// Write the numeric "importance" key into a node's properties JSON object.
///
/// IDEMPOTENT BY CONTRACT. The incremental path exports nodes from the
/// committed Zova generation, and those nodes ALREADY carry an
/// "importance" key from the previous run. A pure append would emit
/// {"importance":1.0,...,"importance":2.0} — silent property corruption that
/// no build or schema check would catch. So an existing key is removed and
/// the new one appended, which also keeps the key last: the incremental
/// complexity pass re-appends its transitive keys after the stored
/// importance, and an in-place overwrite would leave importance mid-object
/// and make the stored properties differ from a fresh full index (the same
/// remove-then-add that pass_complexity uses).
///
/// A properties_json that is not a JSON object is left untouched.
pub export fn cbm_pipeline_importance_append_prop(node_c: ?*Node, score: f64) void {
    const node = node_c orelse return;
    var old: [*:0]const u8 = if (node.properties_json != null) node.properties_json else "{}";
    var olen = c.strlen(old);
    if (olen < 2 or old[olen - 1] != '}') return; // not a JSON object — leave untouched

    var val: [c.CBM_SZ_32]u8 = undefined;
    const vn = c.snprintf(&val, val.len, "%.6f", score);
    if (vn < 0 or @as(usize, @intCast(vn)) >= val.len) return;

    var stripped: MutStr = null;
    if (impFindKey(old, "importance")) |k| {
        const colon: CStr = c.strchr(k, ':');
        if (colon == null) return; // impFindKey guarantees one, but never trust a raw scan
        var v: [*]const u8 = colon + 1;
        while (isSpace(v[0])) v += 1;
        const vend = impValueEnd(v) orelse return;
        // Drop the member with one adjacent comma: the preceding one, or the
        // following one when the member is first.
        var start = k;
        while (@intFromPtr(start) > @intFromPtr(old) and isSpace((start - 1)[0])) start -= 1;
        var end = vend;
        if (@intFromPtr(start) > @intFromPtr(old) and (start - 1)[0] == ',') {
            start -= 1;
        } else {
            start = k;
            while (isSpace(end[0])) end += 1;
            if (end[0] == ',') end += 1;
        }
        const head = util.ptrDiff(start, old);
        const tail = c.strlen(end);
        stripped = @ptrCast(c.malloc(head + tail + 1));
        if (stripped == null) return;
        @memcpy(stripped[0..head], old[0..head]);
        @memcpy(stripped[head..][0 .. tail + 1], end[0 .. tail + 1]);
        old = stripped;
        olen = c.strlen(old);
        if (olen < 2 or old[olen - 1] != '}') {
            c.free(stripped);
            return;
        }
    }

    // An empty object, possibly with whitespace, takes no leading comma.
    var last: [*]const u8 = old + olen - 1;
    while (@intFromPtr(last) > @intFromPtr(old + 1) and isSpace((last - 1)[0])) last -= 1;
    const empty = (last - 1)[0] == '{';
    var frag: [c.CBM_SZ_64]u8 = undefined;
    const fn_c = c.snprintf(
        &frag,
        frag.len,
        "%s\"importance\":%s}",
        @as(CStr, if (empty) "" else ","),
        @as(CStr, &val),
    );
    if (fn_c < 0 or @as(usize, @intCast(fn_c)) >= frag.len) {
        c.free(stripped);
        return;
    }
    const frag_len: usize = @intCast(fn_c);
    const neu: MutStr = @ptrCast(c.malloc(olen - 1 + frag_len + 1));
    if (neu == null) {
        c.free(stripped);
        return;
    }
    @memcpy(neu[0 .. olen - 1], old[0 .. olen - 1]); // copy without the trailing '}'
    @memcpy(neu[olen - 1 ..][0 .. frag_len + 1], frag[0 .. frag_len + 1]);
    c.free(stripped);
    c.free(node.properties_json);
    node.properties_json = neu;
}

// ── Distinct-file counting (the linear core) ────────────────────────

const ImpNameIndex = struct {
    counts: ?*HashTable, // name -> (void *)(intptr_t)(distinct_files + 1)
    paths: ?*HashTable, // scratch file-path set, cleared between names
};

/// Distinct files a name is DEFINED in. Counts distinct file paths, not
/// distinct nodes — two defs of one name in the same file count once.
/// Computed once per distinct name; every later node in the group is a hash
/// hit. Name keys are borrowed from the gbuf nodes, which outlive the pass.
fn impDistinctFileCount(gb: ?*const c.cbm_gbuf_t, name: CStr, index: *ImpNameIndex) c_int {
    if (name == null or name[0] == 0 or index.counts == null) return 0;
    const cached = c.cbm_ht_get(index.counts, name);
    if (cached != null) {
        return @intCast(@as(isize, @bitCast(@intFromPtr(cached))) - 1);
    }

    var nodes: [*c][*c]const Node = null;
    var count: c_int = 0;
    var distinct: c_int = 0;
    if (c.cbm_gbuf_find_by_name(gb, name, &nodes, &count) == 0 and count > 0) {
        _ = @atomicRmw(u64, &g_importance_name_visits, .Add, @intCast(count), .monotonic);
        if (index.paths) |paths| {
            c.cbm_ht_clear(paths);
            for (0..util.ix(count)) |i| {
                const file_path: CStr = nodes[i].*.file_path;
                if (file_path == null or c.cbm_ht_has(paths, file_path)) continue;
                _ = c.cbm_ht_set(paths, file_path, @ptrFromInt(1));
                distinct += 1;
            }
        } else {
            // Scratch set unavailable (allocation failure): the generic-name
            // multiplier degrades to "never generic" rather than falling back
            // to a pairwise scan — a silent quadratic is worse than a missing
            // 0.1 multiplier.
            distinct = 0;
        }
    }
    _ = c.cbm_ht_set(index.counts, name, @ptrFromInt(@as(usize, @intCast(distinct + 1))));
    return distinct;
}

fn incomingEdgeCount(gb: ?*const c.cbm_gbuf_t, id: i64, edge_type: CStr) c_int {
    var edges: [*c][*c]const Edge = null;
    var ne: c_int = 0;
    if (c.cbm_gbuf_find_edges_by_target_type(gb, id, edge_type, &edges, &ne) != 0) return 0;
    return ne;
}

pub export fn cbm_pipeline_pass_importance(ctx_c: ?*c.cbm_pipeline_ctx_t) void {
    const ctx = ctx_c orelse return;
    const gb = ctx.gbuf orelse return;

    // Modest reservations, deliberately not sized from node_count: the memo
    // holds one entry per DISTINCT name, far fewer than nodes, and reserving
    // buckets for every node would cost hundreds of MB of transient peak on a
    // multi-million-node graph to save a handful of amortised rehashes.
    var index: ImpNameIndex = .{
        .counts = c.cbm_ht_create(c.CBM_SZ_4K),
        .paths = c.cbm_ht_create(c.CBM_SZ_256),
    };
    if (index.counts == null) {
        c.cbm_ht_free(index.paths);
        interop.log(c.CBM_LOG_ERROR, "pass.importance", .{
            @as(CStr, "msg"), @as(CStr, "name_index_alloc_failed"),
        });
        return;
    }

    var updated: u64 = 0;
    for (IMPORTANCE_LABELS) |label| {
        var nodes: [*c][*c]const Node = null;
        var count: c_int = 0;
        if (c.cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) continue;
        for (0..util.ix(count)) |i| {
            const n: *Node = @constCast(nodes[i]);

            const num_refs = incomingEdgeCount(gb, n.id, "CALLS") + incomingEdgeCount(gb, n.id, "USAGE");
            var score: f64 = @sqrt(@as(f64, @floatFromInt(num_refs)));

            if (nameIsPrivate(n.name)) score *= PRIV_MUL;
            if (impDistinctFileCount(gb, n.name, &index) >= GENERIC_MIN_FILES) score *= GENERIC_MUL;
            if (nameIsDistinctive(n.name)) score *= DISTINCT_MUL;
            if (c.cbm_is_test_path(n.file_path) or incomingEdgeCount(gb, n.id, "TESTS") > 0) {
                score *= TEST_MUL;
            }

            cbm_pipeline_importance_append_prop(n, score);
            updated += 1;
        }
    }

    c.cbm_ht_free(index.paths);
    c.cbm_ht_free(index.counts);
    _ = @atomicRmw(u64, &g_importance_nodes, .Add, updated, .monotonic);
    interop.logInfo("pass.importance", .{ @as(CStr, "symbols"), itoaImp(updated) });
}
