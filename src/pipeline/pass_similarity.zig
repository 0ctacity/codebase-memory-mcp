//! pass_similarity.zig — Generate SIMILAR_TO edges from MinHash fingerprints.
//!
//! Reads "fp" hex strings from Function/Method node properties,
//! builds an LSH index, and emits SIMILAR_TO edges for pairs with
//! Jaccard similarity ≥ threshold.
//!
//! Runs as a post-pass after enrichment (both full and incremental).

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const ix = util.ix;
const Node = c.cbm_gbuf_node_t;
const AtomicInt = std.atomic.Value(c_int);

const SIM_EDGE_INIT_CAP = 256;
const SIM_EDGE_GROW = 2;

const FP_KEY_PREFIX_LEN = 6; // strlen("\"fp\":\"")
const MIN_FP_ENTRIES = 2;

// ── Helpers ─────────────────────────────────────────────────────────

/// Parse "fp" hex string from a node's properties_json.
/// Returns true if found and decoded successfully.
fn parseFpFromProps(props_json: CStr, out: *c.cbm_minhash_t) bool {
    if (props_json == null) return false;
    const fp_key: CStr = c.strstr(props_json, "\"fp\":\"");
    if (fp_key == null) return false;
    const hex_start: CStr = fp_key + FP_KEY_PREFIX_LEN;
    // Find closing quote
    const hex_end: CStr = c.strchr(hex_start, '"');
    if (hex_end == null) return false;
    const hex_len = util.ptrDiff(hex_end, hex_start);
    if (hex_len != c.CBM_MINHASH_HEX_LEN) return false;
    var hex_buf: [c.CBM_MINHASH_HEX_BUF]u8 = undefined;
    @memcpy(hex_buf[0..hex_len], hex_start[0..hex_len]);
    hex_buf[hex_len] = 0;
    return c.cbm_minhash_from_hex(&hex_buf, out);
}

// ── Internal types ──────────────────────────────────────────────────

const FP_ENTRY_INIT_CAP = 256;
const FP_ENTRY_GROW = 2;
const PROPS_BUF_LEN = 256;

const FpEntry = extern struct {
    node_id: i64,
    fp: c.cbm_minhash_t,
    file_path: CStr,
    ext: CStr,
    qn: CStr, // canonical ordering + pair-ownership (determinism)
};

/// Canonical entry order: by qualified name (unique). The label-index order
/// from collectFpEntries is gbuf insertion order = parallel-extraction merge
/// order, which varies run to run; LSH bucket chains and the per-node edge-cap
/// truncation both inherit it, flickering WHICH SIMILAR_TO edges are emitted.
fn cmpFpEntryByQn(pa: ?*const anyopaque, pb: ?*const anyopaque) callconv(.c) c_int {
    const a: *const FpEntry = @ptrCast(@alignCast(pa));
    const b: *const FpEntry = @ptrCast(@alignCast(pb));
    const qa: CStr = if (a.qn != null) a.qn else "";
    const qb: CStr = if (b.qn != null) b.qn else "";
    const r = c.strcmp(qa, qb);
    if (r != 0) return r;
    if (a.node_id != b.node_id) return if (a.node_id < b.node_id) -1 else 1;
    return 0;
}

/// Collect all Function/Method nodes with fingerprints from graph buffer.
fn collectFpEntries(gbuf: ?*c.cbm_gbuf_t, out_entries: *[*c]FpEntry) c_int {
    var entries: [*c]FpEntry = null;
    var count: c_int = 0;
    var cap: c_int = 0;

    outer: for ([_][*:0]const u8{ "Function", "Method" }) |label| {
        var nodes: [*c][*c]const Node = null;
        var node_count: c_int = 0;
        if (c.cbm_gbuf_find_by_label(gbuf, label, &nodes, &node_count) != 0) continue;
        var i: usize = 0;
        while (i < node_count) : (i += 1) {
            const n: *const Node = nodes[i];
            var fp: c.cbm_minhash_t = undefined;
            if (!parseFpFromProps(n.properties_json, &fp)) continue;
            if (count >= cap) {
                const new_cap: c_int = if (cap < FP_ENTRY_INIT_CAP) FP_ENTRY_INIT_CAP else cap * FP_ENTRY_GROW;
                const grown = interop.reallocN(FpEntry, entries, ix(new_cap));
                // The C loop broke out of this label only; the next label's
                // first fingerprint then failed the same realloc.
                if (grown == null) continue :outer;
                entries = grown;
                cap = new_cap;
            }
            entries[ix(count)] = .{
                .node_id = n.id,
                .fp = fp,
                .file_path = n.file_path,
                .ext = c.cbm_path_last_dot(n.file_path),
                .qn = n.qualified_name,
            };
            count += 1;
        }
    }
    // Canonicalize (determinism) — see cmpFpEntryByQn.
    if (count > 1) {
        c.qsort(entries, ix(count), @sizeOf(FpEntry), cmpFpEntryByQn);
    }
    out_entries.* = entries;
    return count;
}

// ── Parallel query + emit ───────────────────────────────────────────

/// Deferred edge record; collected per-worker, merged into gbuf sequentially.
const SimDeferredEdge = extern struct {
    source_id: i64,
    target_id: i64,
    jaccard: f64,
    same_file: bool,
};

const SimEdgeBuf = extern struct {
    edges: [*c]SimDeferredEdge,
    count: c_int,
    cap: c_int,
};

fn simEdgeBufPush(buf: *SimEdgeBuf, src: i64, tgt: i64, jaccard: f64, same_file: bool) void {
    if (buf.count >= buf.cap) {
        const nc: c_int = if (buf.cap < SIM_EDGE_INIT_CAP) SIM_EDGE_INIT_CAP else buf.cap * SIM_EDGE_GROW;
        const grown = interop.reallocN(SimDeferredEdge, buf.edges, ix(nc));
        if (grown == null) return;
        buf.edges = grown;
        buf.cap = nc;
    }
    buf.edges[ix(buf.count)] = .{
        .source_id = src,
        .target_id = tgt,
        .jaccard = jaccard,
        .same_file = same_file,
    };
    buf.count += 1;
}

const SimQueryCtx = struct {
    entries: [*]const FpEntry,
    entry_count: c_int,
    lsh: ?*const c.cbm_lsh_index_t,
    worker_bufs: [*c]SimEdgeBuf,
    next_idx: AtomicInt,
    edge_counts: [*c]AtomicInt, // shared atomic array, one per entry
};

const SIM_CAND_CAP = 4096;

fn simQueryWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    const sc: *SimQueryCtx = @ptrCast(@alignCast(ctx_ptr));
    const my_buf = util.at(sc.worker_bufs, ix(worker_id));

    // Thread-local candidate buffer (stack-allocated)
    var cands: [SIM_CAND_CAP][*c]const c.cbm_lsh_entry_t = undefined;

    while (true) {
        const i = sc.next_idx.fetchAdd(1, .monotonic);
        if (i >= sc.entry_count) break;
        const edge_count = util.at(sc.edge_counts, ix(i));

        const ec = edge_count.load(.monotonic);
        if (ec >= c.CBM_MINHASH_MAX_EDGES_PER_NODE) continue;

        const src = util.at(sc.entries, ix(i));
        const cand_count = c.cbm_lsh_query_into(sc.lsh, &src.fp, &cands, SIM_CAND_CAP);

        var emitted: c_int = 0;
        for (cands[0..ix(cand_count)]) |cand_c| {
            const cand: *const c.cbm_lsh_entry_t = cand_c;
            if (cand.node_id == src.node_id) continue;
            if (c.strcmp(src.ext, cand.file_ext) != 0) continue;
            // Pair ownership by canonical QN order, not node id: ids are
            // assigned in parallel-merge order and vary run to run, which
            // flipped which side owned a pair and (with the per-source edge
            // cap) flickered the emitted set (determinism).
            if (src.qn == null or cand.qualified_name == null or
                c.strcmp(src.qn, cand.qualified_name) >= 0)
            {
                continue;
            }

            const cur = edge_count.load(.monotonic);
            if (cur + emitted >= c.CBM_MINHASH_MAX_EDGES_PER_NODE) break;

            const jaccard = c.cbm_minhash_jaccard(&src.fp, cand.fingerprint);
            if (jaccard < c.CBM_MINHASH_JACCARD_THRESHOLD) continue;

            const same_file = src.file_path != null and cand.file_path != null and
                util.streq(src.file_path, cand.file_path);
            simEdgeBufPush(my_buf, src.node_id, cand.node_id, jaccard, same_file);
            emitted += 1;
        }
        if (emitted > 0) {
            _ = edge_count.fetchAdd(emitted, .monotonic);
        }
    }
}

/// Merge worker edge buffers into gbuf. Returns total edge count. Frees worker buffers.
fn mergeSimEdges(gbuf: ?*c.cbm_gbuf_t, worker_bufs: [*c]SimEdgeBuf, worker_count: c_int) c_int {
    var total: c_int = 0;
    for (0..ix(worker_count)) |w| {
        for (0..ix(worker_bufs[w].count)) |e| {
            const de = util.at(worker_bufs[w].edges, e);
            var props: [PROPS_BUF_LEN]u8 = undefined;
            _ = c.snprintf(
                &props,
                props.len,
                "{\"jaccard\":%.3f,\"same_file\":%s}",
                de.jaccard,
                @as(CStr, if (de.same_file) "true" else "false"),
            );
            _ = c.cbm_gbuf_insert_edge(gbuf, de.source_id, de.target_id, "SIMILAR_TO", &props);
            total += 1;
        }
        c.free(worker_bufs[w].edges);
    }
    c.free(worker_bufs);
    return total;
}

// ── Pass entry point ────────────────────────────────────────────────

pub export fn cbm_pipeline_pass_similarity(ctx: *c.cbm_pipeline_ctx_t) c_int {
    interop.logInfo("pass.start", .{ @as(CStr, "pass"), @as(CStr, "similarity") });

    const gbuf = ctx.gbuf;

    // Phase 1: Collect fingerprints from Function/Method nodes
    const t_collect = util.profStart();
    var entries: [*c]FpEntry = null;
    const entry_count = collectFpEntries(gbuf, &entries);
    util.profEnd("similarity", "1_collect_fp", &t_collect, entry_count);

    interop.logInfo("pass.similarity.collected", .{
        @as(CStr, "nodes_with_fp"), @as(CStr, c.cbm_log_itoa(entry_count)),
    });

    if (entry_count < MIN_FP_ENTRIES) {
        c.free(entries);
        interop.logInfo("pass.done", .{
            @as(CStr, "pass"), @as(CStr, "similarity"), @as(CStr, "edges"), @as(CStr, "0"),
        });
        return 0;
    }

    // Phase 2: Build LSH index (sequential — cbm_lsh_insert mutates shared state)
    const t_lsh_build = util.profStart();
    const lsh = c.cbm_lsh_new();
    const lsh_entries = interop.mallocN(c.cbm_lsh_entry_t, ix(entry_count));
    if (lsh_entries == null) {
        c.free(entries);
        c.cbm_lsh_free(lsh);
        return c.CBM_NOT_FOUND;
    }

    for (0..ix(entry_count)) |i| {
        lsh_entries[i] = .{
            .node_id = entries[i].node_id,
            .fingerprint = &util.at(entries, i).fp,
            .file_path = entries[i].file_path,
            .file_ext = entries[i].ext,
            .qualified_name = entries[i].qn,
        };
        c.cbm_lsh_insert(lsh, util.at(lsh_entries, i));
    }
    util.profEnd("similarity", "2_lsh_build_seq", &t_lsh_build, entry_count);

    // Phase 3: Query LSH + emit edges (PARALLEL via cbm_lsh_query_into).
    // Each worker claims entries, queries, scores candidates, stashes edges
    // in its own deferred buffer. Shared edge_counts is atomic.
    // Final merge into gbuf is sequential (gbuf not thread-safe).
    const t_query_emit = util.profStart();
    const edge_counts = interop.callocN(AtomicInt, ix(entry_count));
    const worker_count = c.cbm_default_worker_count(false);
    const worker_bufs = interop.callocN(SimEdgeBuf, ix(worker_count));

    {
        var sc: SimQueryCtx = .{
            .entries = entries,
            .entry_count = entry_count,
            .lsh = lsh,
            .worker_bufs = worker_bufs,
            .next_idx = .init(0),
            .edge_counts = edge_counts,
        };
        const opts: c.cbm_parallel_for_opts_t = .{ .max_workers = worker_count, .force_pthreads = false };
        c.cbm_parallel_for(worker_count, simQueryWorker, &sc, opts);
    }
    util.profEnd("similarity", "3_query_parallel", &t_query_emit, entry_count);

    const t_merge = util.profStart();
    const total_edges = mergeSimEdges(gbuf, worker_bufs, worker_count);
    util.profEnd("similarity", "4_edge_merge_seq", &t_merge, total_edges);

    interop.logInfo("pass.done", .{
        @as(CStr, "pass"),  @as(CStr, "similarity"),
        @as(CStr, "edges"), @as(CStr, c.cbm_log_itoa(total_edges)),
    });

    c.free(edge_counts);
    c.free(lsh_entries);
    c.free(entries);
    c.cbm_lsh_free(lsh);
    return 0;
}
