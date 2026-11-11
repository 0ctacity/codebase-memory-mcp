//! pass_semantic_edges.zig — Emit SEMANTICALLY_RELATED edges from combined
//! algorithmic embeddings (11 signals, zero external dependencies).
//!
//! Runs as a post-pass after pass_similarity. Reads all Function/Method
//! nodes from the graph buffer, computes TF-IDF + Random Indexing + API
//! signatures + type/decorator vectors + AST profile, builds LSH index,
//! scores candidate pairs, applies graph diffusion, emits edges.
//!
//! Runs in moderate and full modes (not fast). Controlled by pipeline mode.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const XxHash3 = std.hash.XxHash3;
const AtomicInt = std.atomic.Value(c_int);

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;
const Vec = c.cbm_sem_vec_t;
const Func = c.cbm_sem_func_t;
const HashTable = c.CBMHashTable;
const NodePtrs = [*c][*c]const Node;

// ── Helpers ─────────────────────────────────────────────────────────

const PROPS_BUF = 512;
const MAX_FUNCS_INIT = 4096;
const GROW = 2;
const MAX_CALLEES = 64;
const PSE_FP_PREFIX_LEN = 6; // strlen("\"fp\":\"")
const PSE_MIN_FUNCS_FOR_PAIR = 2;
const PSE_CHUNK = 64;

const SEM_DIM: comptime_int = c.CBM_SEM_DIM;
const RSQ_DIM: comptime_int = c.CBM_RSQ_DIM;
const MAX_TOKENS: comptime_int = c.CBM_SEM_MAX_TOKENS;

/// Scalar weight constants used in scoreWorker and related helpers.
const PSE_UNIT_POS: f32 = 1.0;
const PSE_INT8_MAX: f32 = 127.0;
const PSE_ROUND_BIAS: f32 = 0.5;
const PSE_FLOW_WEIGHT: f32 = 0.01;

fn parallelOpts(worker_count: c_int) c.cbm_parallel_for_opts_t {
    return .{ .max_workers = worker_count, .force_pthreads = false };
}

// ── Deferred edge buffer (thread-local, merged after parallel pass) ──

const DeferredEdge = extern struct {
    source_id: i64,
    target_id: i64,
    score: f32,
    same_file: bool,
    /// Canonical admission keys (determinism): func index of the discovering
    /// side, its candidate rank, and the partner's func index. The sequential
    /// admission pass replays pairs in (i, c) order so which pairs win the
    /// per-node max_edges budget no longer depends on worker scheduling.
    i: c_int,
    j: c_int,
    c: c_int,
};

const DeferredEdgeBuf = extern struct {
    edges: [*c]DeferredEdge,
    count: c_int,
    cap: c_int,
};

fn deferredBufInit(buf: *DeferredEdgeBuf) void {
    buf.edges = null;
    buf.count = 0;
    buf.cap = 0;
}

fn deferredBufPush(buf: *DeferredEdgeBuf, edge: DeferredEdge) void {
    if (buf.count >= buf.cap) {
        const nc: c_int = if (buf.cap < c.CBM_SZ_256) c.CBM_SZ_256 else buf.cap * GROW;
        const grown = interop.reallocN(DeferredEdge, buf.edges, ix(nc));
        if (grown == null) return;
        buf.edges = grown;
        buf.cap = nc;
    }
    buf.edges[ix(buf.count)] = edge;
    buf.count += 1;
}

fn deferredBufFree(buf: *DeferredEdgeBuf) void {
    c.free(buf.edges);
    buf.edges = null;
    buf.count = 0;
    buf.cap = 0;
}

// ── Technique 2: Code pattern vocabulary injection ──────────────────
// Inject semantic tokens based on detected code patterns.
// This bridges the vocabulary gap for abstract concepts like "error handling".

/// Append the tokens in `texts` while there is capacity. A token whose strdup
/// fails is dropped.
fn pushPatternTokens(tokens: [*]MutStr, count_in: c_int, max_tokens: c_int, texts: []const [*:0]const u8) c_int {
    var count = count_in;
    for (texts) |text| {
        if (count >= max_tokens) continue;
        tokens[ix(count)] = c.strdup(text);
        if (tokens[ix(count)] != null) count += 1;
    }
    return count;
}

/// True if `s` contains any of the substrings in `needles`.
fn hasAny(s: CStr, needles: []const [*:0]const u8) bool {
    if (s == null) return false;
    for (needles) |needle| {
        if (c.strstr(s, needle) != null) return true;
    }
    return false;
}

/// Inject tokens derived from body-text patterns (try/catch, raise, log).
fn injectBodyPatternTokens(bt: CStr, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (bt == null) return count;
    if (hasAny(bt, &.{ "except", "catch", "rescue" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "error", "handling", "exception" });
    }
    if (hasAny(bt, &.{ "raise", "throw" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "error", "exception", "throw" });
    }
    if (hasAny(bt, &.{ "logger", "logging", "log_" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "logging", "log" });
    }
    return count;
}

/// Inject tokens for one CALLS-target name based on keyword groups.
fn injectCalleeTokens(name: CStr, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (hasAny(name, &.{ "log", "Log", "warn", "debug", "info" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "logging", "log" });
    }
    if (hasAny(name, &.{ "Error", "error", "Errorf", "panic" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "error", "handling" });
    }
    if (hasAny(name, &.{ "open", "read", "write", "close", "Open", "Read" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "io", "file" });
    }
    return count;
}

/// Walk the outbound CALLS edges of n and inject tokens for each target.
fn injectCallsPatternTokens(n: *const Node, gbuf: ?*const Gbuf, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (gbuf == null) return count;
    var neighbor_count: c_int = 0;
    const names = collectSortedCallNeighbors(gbuf, n.id, true, &neighbor_count);
    var i: usize = 0;
    while (i < neighbor_count and count < max_tokens) : (i += 1) {
        count = injectCalleeTokens(names[i], tokens, count, max_tokens);
    }
    c.free(@ptrCast(names));
    return count;
}

/// Inject tokens from decorator annotations (@route, @middleware, @pytest.*).
fn injectDecoratorTokens(decs: CStr, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (decs == null) return count;
    if (hasAny(decs, &.{ "route", "Route", "app." })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "routing", "endpoint", "handler" });
    }
    if (hasAny(decs, &.{ "middleware", "Middleware" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{"middleware"});
    }
    if (hasAny(decs, &.{ "test", "Test", "pytest" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "test", "testing" });
    }
    return count;
}

/// Inject tokens keyed off the node's own name (test_*, *Handler, validator).
fn injectNamePatternTokens(name: CStr, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (name == null) return count;
    if (hasAny(name, &.{ "test_", "Test" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{ "test", "testing" });
    }
    if (hasAny(name, &.{ "middleware", "Middleware" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{"middleware"});
    }
    if (hasAny(name, &.{ "handler", "Handler" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{"handler"});
    }
    if (hasAny(name, &.{ "validator", "Validator", "validate", "Validate" })) {
        count = pushPatternTokens(tokens, count, max_tokens, &.{"validation"});
    }
    return count;
}

fn injectPatternTokens(n_c: ?*const Node, gbuf: ?*const Gbuf, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    const n = n_c orelse return count;
    if (count >= max_tokens) return count;

    var bt_buf: [c.CBM_SZ_512]u8 = undefined;
    const bt: CStr = if (n.properties_json != null)
        jsonStrValue(n.properties_json, "bt", &bt_buf)
    else
        null;
    var dec_buf: [c.CBM_SZ_256]u8 = undefined;
    const decs: CStr = if (n.properties_json != null)
        jsonStrValue(n.properties_json, "decorators", &dec_buf)
    else
        null;

    count = injectBodyPatternTokens(bt, tokens, count, max_tokens);
    count = injectCallsPatternTokens(n, gbuf, tokens, count, max_tokens);
    count = injectDecoratorTokens(decs, tokens, count, max_tokens);
    count = injectNamePatternTokens(n.name, tokens, count, max_tokens);
    return count;
}

/// Extract a JSON string value by key (simple strstr-based, no full parse).
fn jsonStrValue(json: CStr, key: CStr, buf: []u8) CStr {
    if (json == null or key == null) return null;
    var search: [c.CBM_SZ_64]u8 = undefined;
    _ = c.snprintf(&search, search.len, "\"%s\":\"", key);
    var start: CStr = c.strstr(json, &search);
    if (start == null) return null;
    start += c.strlen(&search);
    const end: CStr = c.strchr(start, '"');
    if (end == null) return null;
    var len = util.ptrDiff(end, start);
    if (len >= buf.len) len = buf.len - 1;
    @memcpy(buf[0..len], start[0..len]);
    buf[len] = 0;
    return buf.ptr;
}

/// Extract a JSON array of strings by key. Returns count.
fn jsonStrArray(json: CStr, key: CStr, out: []MutStr) usize {
    if (json == null or key == null) return 0;
    var search: [c.CBM_SZ_64]u8 = undefined;
    _ = c.snprintf(&search, search.len, "\"%s\":[", key);
    var start: CStr = c.strstr(json, &search);
    if (start == null) return 0;
    start += c.strlen(&search);
    var count: usize = 0;
    while (start[0] != 0 and start[0] != ']' and count < out.len) {
        if (start[0] == '"') {
            start += 1;
            const end: CStr = c.strchr(start, '"');
            if (end == null) break;
            const len = util.ptrDiff(end, start);
            out[count] = @ptrCast(c.malloc(len + 1));
            @memcpy(out[count][0..len], start[0..len]);
            out[count][len] = 0;
            count += 1;
            start = end + 1;
        } else {
            start += 1;
        }
    }
    return count;
}

// ── Tokenize node metadata ──────────────────────────────────────────

/// Tokenize a single string field keyed out of node.properties_json, if
/// present.  Returns the new count after appending any tokens.
fn tokenizeJsonStringField(json: CStr, key: CStr, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (count >= max_tokens) return count;
    var buf: [c.CBM_SZ_512]u8 = undefined;
    if (jsonStrValue(json, key, &buf) == null) return count;
    count += c.cbm_sem_tokenize(&buf, tokens + ix(count), max_tokens - count);
    return count;
}

/// Tokenize a JSON array field (e.g. "param_names", "decorators").
fn tokenizeJsonArrayField(json: CStr, key: CStr, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (count >= max_tokens) return count;
    var arr: [c.CBM_SZ_16]MutStr = undefined;
    const n = jsonStrArray(json, key, &arr);
    for (arr[0..n]) |item| {
        if (count < max_tokens) {
            count += c.cbm_sem_tokenize(item, tokens + ix(count), max_tokens - count);
        }
        c.free(item);
    }
    return count;
}

fn cmpNamePtr(pa: ?*const anyopaque, pb: ?*const anyopaque) callconv(.c) c_int {
    const a: *const CStr = @ptrCast(@alignCast(pa));
    const b: *const CStr = @ptrCast(@alignCast(pb));
    return c.strcmp(a.*, b.*);
}

/// Collect the CALLS-neighbor names of `node_id`, SORTED by name. The edge
/// arrays are in insertion order — which under parallel extraction varies run
/// to run — and both consumers truncate (MAX_CALLEES / max_tokens), so an
/// unstable order changed WHICH neighbors contribute to the semantic vectors
/// and flickered near-threshold SEMANTICALLY_RELATED edges (determinism).
/// Returns a malloc'd array of borrowed name pointers; caller frees the array.
fn collectSortedCallNeighbors(gbuf: ?*const Gbuf, node_id: i64, outbound: bool, out_n: *c_int) [*c]CStr {
    out_n.* = 0;
    var edges: [*c][*c]const Edge = null;
    var ec: c_int = 0;
    const rc = if (outbound)
        c.cbm_gbuf_find_edges_by_source_type(gbuf, node_id, "CALLS", &edges, &ec)
    else
        c.cbm_gbuf_find_edges_by_target_type(gbuf, node_id, "CALLS", &edges, &ec);
    if (rc != 0 or ec <= 0) return null;
    const names = interop.mallocN(CStr, ix(ec));
    if (names == null) return null;
    var n: usize = 0;
    for (0..ix(ec)) |e| {
        const id = if (outbound) edges[e].*.target_id else edges[e].*.source_id;
        const neighbor: ?*const Node = c.cbm_gbuf_find_by_id(gbuf, id);
        if (neighbor) |nb| {
            if (nb.name != null) {
                names[n] = nb.name;
                n += 1;
            }
        }
    }
    if (n > 1) {
        c.qsort(@ptrCast(names), n, @sizeOf(CStr), cmpNamePtr);
    }
    out_n.* = @intCast(n);
    return names;
}

/// Walk the CALLS edges rooted at n (either outbound or inbound depending on
/// `outbound`) and tokenize the names of the target/source nodes.  Caller-side
/// caps via max_tokens and MAX_CALLEES.
fn tokenizeCallNeighbors(n: *const Node, gbuf: ?*const Gbuf, outbound: bool, tokens: [*]MutStr, count_in: c_int, max_tokens: c_int) c_int {
    var count = count_in;
    if (gbuf == null or count >= max_tokens) return count;
    var nn: c_int = 0;
    const names = collectSortedCallNeighbors(gbuf, n.id, outbound, &nn);
    if (names == null) return count;
    var e: usize = 0;
    while (e < nn and e < MAX_CALLEES and count < max_tokens) : (e += 1) {
        count += c.cbm_sem_tokenize(names[e], tokens + ix(count), max_tokens - count);
    }
    c.free(@ptrCast(names));
    return count;
}

fn tokenizeNode(n: *const Node, gbuf: ?*const Gbuf, tokens: [*]MutStr, max_tokens: c_int) c_int {
    var count: c_int = 0;
    count += c.cbm_sem_tokenize(n.name, tokens + ix(count), max_tokens - count);
    if (n.qualified_name != null and count < max_tokens) {
        count += c.cbm_sem_tokenize(n.qualified_name, tokens + ix(count), max_tokens - count);
    }
    if (n.file_path != null and count < max_tokens) {
        count += c.cbm_sem_tokenize(n.file_path, tokens + ix(count), max_tokens - count);
    }
    if (n.properties_json != null) {
        const props = n.properties_json;
        count = tokenizeJsonStringField(props, "signature", tokens, count, max_tokens);
        count = tokenizeJsonStringField(props, "return_type", tokens, count, max_tokens);
        count = tokenizeJsonStringField(props, "docstring", tokens, count, max_tokens);
        count = tokenizeJsonArrayField(props, "param_names", tokens, count, max_tokens);
        count = tokenizeJsonArrayField(props, "param_types", tokens, count, max_tokens);
        count = tokenizeJsonArrayField(props, "decorators", tokens, count, max_tokens);
        count = tokenizeJsonStringField(props, "bt", tokens, count, max_tokens);
    }
    count = tokenizeCallNeighbors(n, gbuf, true, tokens, count, max_tokens);

    // Caller names: what CALLS this function (contextual vocabulary).
    // Functions called by error handlers inherit "error" context.
    count = tokenizeCallNeighbors(n, gbuf, false, tokens, count, max_tokens);
    return count;
}

// ── Build per-function semantic data ────────────────────────────────

/// Add the random-index vector of `token` to `out`.
fn addRandomIndex(out: *Vec, token: CStr) void {
    var ri: Vec = undefined;
    c.cbm_sem_random_index(token, &ri);
    c.cbm_sem_vec_add_scaled(out, &ri, PSE_UNIT_POS);
}

fn buildApiVec(gbuf: ?*const Gbuf, node_id: i64, out: *Vec) void {
    @memset(&out.v, 0.0);
    // Sorted neighbors: stable MAX_CALLEES subset + stable float-accumulation
    // order (see collectSortedCallNeighbors).
    var n: c_int = 0;
    const names = collectSortedCallNeighbors(gbuf, node_id, true, &n);
    if (names == null) return;
    var i: usize = 0;
    while (i < n and i < MAX_CALLEES) : (i += 1) {
        addRandomIndex(out, names[i]);
    }
    c.free(@ptrCast(names));
    c.cbm_sem_normalize(out);
}

fn buildTypeVec(props_json: CStr, out: *Vec) void {
    @memset(&out.v, 0.0);
    if (props_json == null) return;
    // Extract param_types and return_type
    var rt_buf: [c.CBM_SZ_128]u8 = undefined;
    if (jsonStrValue(props_json, "return_type", &rt_buf) != null) {
        addRandomIndex(out, &rt_buf);
    }
    var ptypes: [c.CBM_SZ_16]MutStr = undefined;
    const pt_count = jsonStrArray(props_json, "param_types", &ptypes);
    for (ptypes[0..pt_count]) |ptype| {
        addRandomIndex(out, ptype);
        c.free(ptype);
    }
    c.cbm_sem_normalize(out);
}

fn buildDecoVec(props_json: CStr, out: *Vec) void {
    @memset(&out.v, 0.0);
    if (props_json == null) return;
    var decos: [c.CBM_SZ_16]MutStr = undefined;
    const dc = jsonStrArray(props_json, "decorators", &decos);
    for (decos[0..dc]) |deco| {
        addRandomIndex(out, deco);
        c.free(deco);
    }
    c.cbm_sem_normalize(out);
}

fn decodeStructProfile(props_json: CStr, out: *[c.CBM_AST_PROFILE_DIMS]f32) void {
    @memset(out, 0.0);
    if (props_json == null) return;
    var sp_buf: [c.CBM_AST_PROFILE_BUF]u8 = undefined;
    if (jsonStrValue(props_json, "sp", &sp_buf) != null) {
        var profile: c.cbm_ast_profile_t = undefined;
        if (c.cbm_ast_profile_from_str(&sp_buf, &profile)) {
            c.cbm_ast_profile_to_vector(&profile, out);
        }
    }
}

fn decodeMinhash(props_json: CStr, func: *Func) void {
    func.has_minhash = false;
    if (props_json == null) return;
    const fp_key: CStr = c.strstr(props_json, "\"fp\":\"");
    if (fp_key == null) return;
    const hex: CStr = fp_key + PSE_FP_PREFIX_LEN; // strlen("\"fp\":\"")
    const end: CStr = c.strchr(hex, '"');
    if (end == null or util.ptrDiff(end, hex) != c.CBM_MINHASH_HEX_LEN) return;
    var hex_buf: [c.CBM_MINHASH_HEX_BUF]u8 = undefined;
    @memcpy(hex_buf[0..c.CBM_MINHASH_HEX_LEN], hex[0..c.CBM_MINHASH_HEX_LEN]);
    hex_buf[c.CBM_MINHASH_HEX_LEN] = 0;
    var mh: c.cbm_minhash_t = undefined;
    if (c.cbm_minhash_from_hex(&hex_buf, &mh)) {
        func.minhash = mh.values;
        func.has_minhash = true;
    }
}

// ── Parallel Phase 2: Tokenize nodes ────────────────────────────────

const TokenizeCtx = struct {
    node_ptrs: NodePtrs, // node pointer per function index
    gbuf: ?*Gbuf, // read-only during tokenization
    all_tokens: [*c]MutStr, // output: all_tokens[f * MAX + t]
    token_counts: [*c]c_int, // output: token count per function
    func_count: c_int,
    next_idx: AtomicInt,
    /// Per-worker token intern pools (key==value==the one owned strdup):
    /// identical tokens ("xfs", "error", ...) recur across hundreds of
    /// thousands of functions; per-func strdups made all_tokens hold every
    /// instance. Interned, it holds at most workers x unique tokens.
    pools: [*c]?*HashTable,
};

fn tokenizeWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    const tc: *TokenizeCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const f = tc.next_idx.fetchAdd(1, .monotonic);
        if (f >= tc.func_count) break;

        const n: *const Node = tc.node_ptrs[ix(f)];
        // Write directly into the shared buffer slice for this function — the
        // strdup'd tokens are owned by all_tokens[] from the moment they land
        // in this slot.
        const dst: [*]MutStr = @as([*]MutStr, tc.all_tokens) + ix(f) * MAX_TOKENS;
        var count = tokenizeNode(n, tc.gbuf, dst, MAX_TOKENS);
        count = injectPatternTokens(n, tc.gbuf, dst, count, MAX_TOKENS);
        if (tc.pools != null) {
            if (tc.pools[ix(worker_id)]) |pool| {
                for (dst[0..ix(count)]) |*token| {
                    const canon: MutStr = @ptrCast(c.cbm_ht_get(pool, token.*));
                    if (canon != null) {
                        c.free(token.*);
                        token.* = canon;
                    } else {
                        _ = c.cbm_ht_set(pool, token.*, token.*); // key borrows the value
                    }
                }
            }
        }
        tc.token_counts[ix(f)] = count;
    }
}

// ── Parallel Phase 4: Build per-function vectors ────────────────────

const VecBuildCtx = struct {
    funcs: [*c]Func,
    all_tokens: [*c]MutStr,
    token_counts: [*c]c_int,
    corpus: ?*c.cbm_sem_corpus_t,
    qvecs: [*]u8, // output: pre-quantized int8 vectors [func_count * CBM_SEM_DIM]
    func_count: c_int,
    next_idx: AtomicInt,
};

/// (uint8_t)(int8_t)(clamp(v, -1, 1) * 127): the int8 quantization stored in
/// the graph buffer.
inline fn quantizeUnit(v_in: f32) u8 {
    var v = v_in;
    if (v > PSE_UNIT_POS) v = PSE_UNIT_POS;
    if (v < -PSE_UNIT_POS) v = -PSE_UNIT_POS;
    return @bitCast(@as(i8, @intFromFloat(v * PSE_INT8_MAX)));
}

fn vecBuildWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const vc: *VecBuildCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const f_c = vc.next_idx.fetchAdd(1, .monotonic);
        if (f_c >= vc.func_count) break;
        const f = ix(f_c);
        const func = util.at(vc.funcs, f);

        const tc = ix(vc.token_counts[f]);
        const tokens: [*]MutStr = @as([*]MutStr, vc.all_tokens) + f * MAX_TOKENS;

        // TF-IDF weights
        const indices = interop.mallocN(c_int, tc);
        const weights = interop.mallocN(f32, tc);
        var tfidf_len: usize = 0;
        for (0..tc) |t| {
            const idf = c.cbm_sem_corpus_idf(vc.corpus, tokens[t]);
            if (idf > 0.0) {
                indices[tfidf_len] = @intCast(t);
                weights[tfidf_len] = idf;
                tfidf_len += 1;
            }
        }
        func.tfidf_indices = indices;
        func.tfidf_weights = weights;
        func.tfidf_len = @intCast(tfidf_len);

        // RI vector: sum of enriched token vectors weighted by IDF. Built in a
        // LOCAL dense buffer; only the quantized code is retained per func
        // (the resident dense floats were ~9.4 GB on the kernel).
        var ri_dense: Vec = undefined;
        @memset(&ri_dense.v, 0.0);
        for (0..tc) |t| {
            const ri = c.cbm_sem_corpus_ri_vec(vc.corpus, tokens[t]);
            if (ri != null) {
                const idf = c.cbm_sem_corpus_idf(vc.corpus, tokens[t]);
                c.cbm_sem_vec_add_scaled(&ri_dense, ri, idf);
            }
        }
        c.cbm_sem_normalize(&ri_dense);
        c.cbm_rsq_encode(&ri_dense.v, &func.ri_code);

        // Int8 quantize into pre-allocated output array (parallel-safe)
        const qv = vc.qvecs[f * SEM_DIM ..][0..SEM_DIM];
        for (qv, ri_dense.v) |*q, v| q.* = quantizeUnit(v);
    }
}

// ── Parallel Phase 5: LSH signatures ────────────────────────────────

const NUM_HYPERPLANES = 64;
const SEM_LSH_BANDS = 16;
const SEM_LSH_ROWS = 4;
const SEM_BUCKET_COUNT = 65536;
const SEM_BUCKET_MASK = 65535;
const SEM_BUCKET_CAP_INIT = 16;
const SEM_MAX_CANDIDATES = 200;

/// Row of a hyperplane matrix in the ROTATED (quantized) basis: LSH only
/// needs signs of dots against random directions, and a random direction in
/// the rotated basis is as random as one in the original — so signatures are
/// computed from dequantized codes without keeping dense originals.
const HyperplaneRow = [RSQ_DIM]f32;

const SigBuildCtx = struct {
    funcs: [*c]Func,
    signatures: [*]u64,
    hyperplanes: [*]HyperplaneRow,
    func_count: c_int,
    next_idx: AtomicInt,
};

fn sigBuildWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const sc: *SigBuildCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const f = sc.next_idx.fetchAdd(1, .monotonic);
        if (f >= sc.func_count) break;

        var dec: [RSQ_DIM]f32 = undefined;
        c.cbm_rsq_decode(&util.at(sc.funcs, ix(f)).ri_code, &dec);
        var sig: u64 = 0;
        for (0..NUM_HYPERPLANES) |h| {
            var dot: f32 = 0.0;
            for (dec, sc.hyperplanes[h]) |d, plane| {
                dot += d * plane;
            }
            if (dot > 0.0) sig |= @as(u64, 1) << @intCast(h);
        }
        sc.signatures[ix(f)] = sig;
    }
}

// ── Parallel Phase 6: Score candidates + collect edges ──────────────

const SemBucket = extern struct {
    items: [*c]c_int,
    count: c_int,
    cap: c_int,
};

const ScoreCtx = struct {
    funcs: [*c]Func,
    signatures: [*c]u64,
    edge_counts: [*c]c_int, // budget applied sequentially in phase6b (determinism)
    cfg: c.cbm_sem_config_t,
    func_count: c_int,

    /// LSH buckets (read-only during scoring)
    band_buckets: [*c][*c]SemBucket,

    /// Per-worker edge buffer
    worker_bufs: [*c]DeferredEdgeBuf,
    max_workers: c_int,
    next_idx: AtomicInt,
};

const SCORE_SEEN_CAP = 8192;
const SCORE_SEEN_MASK = 8191;
const SCORE_SEEN_EMPTY = -1;

/// Check whether `j` has already been recorded in the open-addressed `seen`
/// set for this function; insert it if not.  Returns true when the insertion
/// was fresh (caller should add to candidates).
fn scoreSeenInsert(seen: *[SCORE_SEEN_CAP]c_int, j: c_int) bool {
    const slot: u32 = @as(u32, @bitCast(j)) & SCORE_SEEN_MASK;
    for (0..SCORE_SEEN_CAP) |p| {
        const idx = (slot +% @as(u32, @intCast(p))) & SCORE_SEEN_MASK;
        if (seen[idx] == SCORE_SEEN_EMPTY) {
            seen[idx] = j;
            return true;
        }
        if (seen[idx] == j) return false;
    }
    return false;
}

/// The LSH bucket index of a signature in band `b`.
fn bandBucketIndex(signature: u64, b: usize) usize {
    const shift: u6 = @intCast(b * SEM_LSH_ROWS);
    const band_val: u32 = @truncate((signature >> shift) & ((@as(u64, 1) << SEM_LSH_ROWS) - 1));
    const bh = XxHash3.hash(b, std.mem.asBytes(&band_val));
    return @intCast(bh & SEM_BUCKET_MASK);
}

/// Collect the unique candidate function indices for node `i` by iterating
/// every LSH band and merging bucket members via `seen[]`.  Returns the
/// populated candidate count.
fn scoreCollectCandidates(sc: *ScoreCtx, i: c_int, seen: *[SCORE_SEEN_CAP]c_int, candidates: []c_int) usize {
    var cand_count: usize = 0;
    var b: usize = 0;
    while (b < SEM_LSH_BANDS and cand_count < candidates.len) : (b += 1) {
        const bucket = &sc.band_buckets[b][bandBucketIndex(sc.signatures[ix(i)], b)];
        const bcount = bucket.count;
        const bitems = bucket.items;
        if (bcount > SEM_MAX_CANDIDATES) continue;
        var k: usize = 0;
        while (k < bcount and cand_count < candidates.len) : (k += 1) {
            const j = bitems[k];
            if (j <= i) continue;
            if (scoreSeenInsert(seen, j)) {
                candidates[cand_count] = j;
                cand_count += 1;
            }
        }
    }
    return cand_count;
}

/// Score one candidate pair (i, j) and push a deferred candidate edge if the
/// score passes the threshold. ADMISSION (the per-node max_edges budget) is
/// deliberately NOT decided here: the old check-then-increment on shared
/// atomic counts made the admitted edge SET depend on worker scheduling —
/// multi-threaded runs lost edges vs single-threaded and differed run-to-run
/// (repro_parallel_edge_determinism). Scoring is pure math and stays parallel;
/// the budget is applied afterwards in one sequential pass over the pairs in
/// canonical (i, candidate-rank) order.
fn scoreTryEmit(sc: *ScoreCtx, i: c_int, j: c_int, rank: c_int, my_buf: *DeferredEdgeBuf) void {
    const a = util.at(sc.funcs, ix(i));
    const b = util.at(sc.funcs, ix(j));
    if (c.strcmp(a.file_ext, b.file_ext) != 0) return;
    const score = c.cbm_sem_combined_score(a, b, &sc.cfg);
    if (score < sc.cfg.threshold) return;
    const same_file = a.file_path != null and b.file_path != null and
        streq(a.file_path, b.file_path);
    deferredBufPush(my_buf, .{
        .source_id = a.node_id,
        .target_id = b.node_id,
        .score = score,
        .same_file = same_file,
        .i = i,
        .j = j,
        .c = rank,
    });
}

fn scoreWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    const sc: *ScoreCtx = @ptrCast(@alignCast(ctx_ptr));
    const my_buf = util.at(sc.worker_bufs, ix(worker_id));

    while (true) {
        const i = sc.next_idx.fetchAdd(1, .monotonic);
        if (i >= sc.func_count) break;
        var seen: [SCORE_SEEN_CAP]c_int = @splat(SCORE_SEEN_EMPTY);
        var candidates: [SEM_MAX_CANDIDATES]c_int = undefined;
        const cand_count = scoreCollectCandidates(sc, i, &seen, &candidates);
        for (candidates[0..cand_count], 0..) |j, rank| {
            scoreTryEmit(sc, i, j, @intCast(rank), my_buf);
        }
    }
}

// ── Parallel Phase 1b: decode minhash/profile + build per-func vectors ──

const CollectCtx = struct {
    funcs: [*c]Func,
    node_ptrs: NodePtrs,
    gbuf: ?*const Gbuf,
    func_count: c_int,
    next_idx: AtomicInt,
};

fn collectWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const cc: *CollectCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const f = cc.next_idx.fetchAdd(PSE_CHUNK, .monotonic);
        if (f >= cc.func_count) break;
        var end = f + PSE_CHUNK;
        if (end > cc.func_count) end = cc.func_count;
        for (ix(f)..ix(end)) |i| {
            const n: *const Node = cc.node_ptrs[i];
            const func = util.at(cc.funcs, i);
            decodeMinhash(n.properties_json, func);
            decodeStructProfile(n.properties_json, &func.struct_profile);
            var tmp_vec: Vec = undefined;
            buildApiVec(cc.gbuf, n.id, &tmp_vec);
            c.cbm_rsq_encode(&tmp_vec.v, &func.api_code);
            buildTypeVec(n.properties_json, &tmp_vec);
            c.cbm_rsq_encode(&tmp_vec.v, &func.type_code);
            buildDecoVec(n.properties_json, &tmp_vec);
            c.cbm_rsq_encode(&tmp_vec.v, &func.deco_code);
        }
    }
}

// ── Phase helpers (keep cbm_pipeline_pass_semantic_edges complexity low) ──

/// Canonical node order: by qualified name (unique per node), id tie-break
/// for defensiveness. Gives the semantic pass a stable, content-derived input
/// order regardless of how parallel extraction merged the graph buffer.
fn cmpNodePtrByQn(pa: ?*const anyopaque, pb: ?*const anyopaque) callconv(.c) c_int {
    const a: *const Node = @as(*const *const Node, @ptrCast(@alignCast(pa))).*;
    const b: *const Node = @as(*const *const Node, @ptrCast(@alignCast(pb))).*;
    const qa: CStr = if (a.qualified_name != null) a.qualified_name else "";
    const qb: CStr = if (b.qualified_name != null) b.qualified_name else "";
    const r = c.strcmp(qa, qb);
    if (r != 0) return r;
    if (a.id != b.id) return if (a.id < b.id) -1 else 1;
    return 0;
}

/// Phase 1a: seed the funcs[] / node_ptrs[] arrays from all Function and
/// Method nodes in the graph buffer.  Returns the number of functions collected
/// (0 on OOM), and fills out_funcs / out_nodes with newly malloc'd arrays.
fn phase1ScanFunctions(gbuf: ?*Gbuf, out_funcs: *[*c]Func, out_nodes: *NodePtrs) c_int {
    out_funcs.* = null;
    out_nodes.* = null;
    var funcs: [*c]Func = null;
    var node_ptrs: NodePtrs = null;
    var func_count: c_int = 0;
    var func_cap: c_int = 0;
    labels: for ([_][*:0]const u8{ "Function", "Method" }) |label| {
        var nodes: NodePtrs = null;
        var node_count: c_int = 0;
        if (c.cbm_gbuf_find_by_label(gbuf, label, &nodes, &node_count) != 0) continue;
        for (0..ix(node_count)) |i| {
            if (func_count >= func_cap) {
                const new_cap: c_int = if (func_cap < MAX_FUNCS_INIT) MAX_FUNCS_INIT else func_cap * GROW;
                const grown = interop.reallocN(Func, funcs, ix(new_cap));
                // A failed growth ends this label's scan, as the C `break` did.
                if (grown == null) continue :labels;
                funcs = grown;
                const np_grown = interop.reallocN([*c]const Node, node_ptrs, ix(new_cap));
                if (np_grown == null) continue :labels;
                node_ptrs = np_grown;
                func_cap = new_cap;
            }
            const func = util.at(funcs, ix(func_count));
            func.* = std.mem.zeroes(Func);
            func.node_id = nodes[i].*.id;
            func.file_path = nodes[i].*.file_path;
            func.file_ext = c.cbm_path_last_dot(nodes[i].*.file_path);
            node_ptrs[ix(func_count)] = nodes[i];
            func_count += 1;
        }
    }
    // Canonicalize the func order (determinism): the label-index order above
    // is gbuf insertion order = parallel-extraction merge order, which varies
    // run to run. Everything downstream is order-sensitive — LSH bucket chain
    // order, the SEM_MAX_CANDIDATES truncation, seen[] and the admission
    // sequence — so an unstable order changes WHICH semantic edges are
    // emitted. Sort the cheap pointer array by qualified name (unique) and
    // re-derive the three fields set so far; the heavy per-func payloads are
    // filled in later phases, so no 12.7 KB structs are moved.
    if (func_count > 1) {
        c.qsort(@ptrCast(node_ptrs), ix(func_count), @sizeOf([*c]const Node), cmpNodePtrByQn);
    }
    for (0..ix(func_count)) |k| {
        funcs[k].node_id = node_ptrs[k].*.id;
        funcs[k].file_path = node_ptrs[k].*.file_path;
        funcs[k].file_ext = c.cbm_path_last_dot(node_ptrs[k].*.file_path);
    }
    out_funcs.* = funcs;
    out_nodes.* = node_ptrs;
    return func_count;
}

/// Phase 5c: partition functions into LSH buckets by their signature bands.
/// Sequential because each bucket grows its `items` array via realloc.
fn phase5cBuildLshBuckets(signatures: [*c]const u64, func_count: c_int, band_buckets: [*c][*c]SemBucket) void {
    for (0..ix(func_count)) |f| {
        for (0..SEM_LSH_BANDS) |b| {
            const bucket = &band_buckets[b][bandBucketIndex(signatures[f], b)];
            if (bucket.count >= bucket.cap) {
                const nc: c_int = if (bucket.cap < SEM_BUCKET_CAP_INIT) SEM_BUCKET_CAP_INIT else bucket.cap * GROW;
                const ni = interop.reallocN(c_int, bucket.items, ix(nc));
                if (ni == null) continue;
                bucket.items = ni;
                bucket.cap = nc;
            }
            bucket.items[ix(bucket.count)] = @intCast(f);
            bucket.count += 1;
        }
    }
}

/// Canonical order for candidate pairs: ascending discovering-func index,
/// then ascending candidate rank — the order a sequential scoring loop over
/// canonically-sorted funcs would have produced.
fn cmpDeferredEdgeCanonical(pa: ?*const anyopaque, pb: ?*const anyopaque) callconv(.c) c_int {
    const a: *const DeferredEdge = @ptrCast(@alignCast(pa));
    const b: *const DeferredEdge = @ptrCast(@alignCast(pb));
    if (a.i != b.i) return if (a.i < b.i) -1 else 1;
    if (a.c != b.c) return if (a.c < b.c) -1 else 1;
    return 0;
}

/// Phase 6b. Sequential, deterministic admission + merge: gather every
/// above-threshold pair from the worker buffers, replay them in canonical
/// (i, rank) order, and apply the per-node max_edges budget HERE —
/// single-threaded — so the admitted edge set is a pure function of the
/// (canonically sorted) inputs, independent of worker count and scheduling.
fn phase6bMergeEdges(gbuf: ?*Gbuf, worker_bufs: [*c]DeferredEdgeBuf, worker_count: c_int, edge_counts: [*c]c_int, max_edges: c_int) c_int {
    const workers = ix(worker_count);
    var total_pairs: c_int = 0;
    for (0..workers) |w| total_pairs += worker_bufs[w].count;
    var pairs: [*c]DeferredEdge = null;
    if (total_pairs > 0) {
        pairs = interop.mallocN(DeferredEdge, ix(total_pairs));
    }
    if (pairs == null) {
        for (0..workers) |w| deferredBufFree(util.at(worker_bufs, w));
        return 0;
    }
    var n: usize = 0;
    for (0..workers) |w| {
        const buf = util.at(worker_bufs, w);
        if (buf.count > 0) {
            @memcpy(pairs[n..][0..ix(buf.count)], buf.edges[0..ix(buf.count)]);
        }
        n += ix(buf.count);
        deferredBufFree(buf);
    }
    if (n > 1) {
        c.qsort(pairs, n, @sizeOf(DeferredEdge), cmpDeferredEdgeCanonical);
    }

    var total_edges: c_int = 0;
    for (0..n) |e| {
        const de = util.at(pairs, e);
        if (edge_counts[ix(de.i)] >= max_edges or edge_counts[ix(de.j)] >= max_edges) continue;
        var props: [PROPS_BUF]u8 = undefined;
        _ = c.snprintf(
            &props,
            props.len,
            "{\"score\":%.3f,\"same_file\":%s}",
            @as(f64, de.score),
            @as(CStr, if (de.same_file) "true" else "false"),
        );
        _ = c.cbm_gbuf_insert_edge(gbuf, de.source_id, de.target_id, "SEMANTICALLY_RELATED", &props);
        edge_counts[ix(de.i)] += 1;
        edge_counts[ix(de.j)] += 1;
        total_edges += 1;
    }
    c.free(pairs);
    return total_edges;
}

/// Phase 3c: export co-occurrence-enriched token vectors into the graph buffer
/// so query-time lookups can use them without re-running the finalize step.
fn phase3cExportTokenVectors(gbuf: ?*Gbuf, corpus: ?*c.cbm_sem_corpus_t) void {
    const tv_count = c.cbm_sem_corpus_token_count(corpus);
    var t: c_int = 0;
    while (t < tv_count) : (t += 1) {
        var vec: [*c]const Vec = null;
        var idf: f32 = 0.0;
        const tok: CStr = c.cbm_sem_corpus_token_at(corpus, t, &vec, &idf);
        if (tok == null or vec == null or idf <= PSE_FLOW_WEIGHT) continue;
        var qvec: [SEM_DIM]u8 = undefined;
        for (&qvec, vec.*.v) |*q, v| q.* = quantizeUnit(v);
        _ = c.cbm_gbuf_store_token_vector(gbuf, tok, &qvec, SEM_DIM, idf);
    }
    interop.logInfo("pass.semantic.token_vectors", .{
        @as(CStr, "count"), @as(CStr, c.cbm_log_itoa(tv_count)),
    });
}

/// Phase 5a: generate NUM_HYPERPLANES × CBM_RSQ_DIM deterministic random
/// float hyperplanes seeded from XXH3 so signatures are reproducible.
fn phase5aBuildHyperplanes() ?[*]HyperplaneRow {
    const hyperplanes: [*]HyperplaneRow = @as(?[*]HyperplaneRow, interop.mallocN(HyperplaneRow, NUM_HYPERPLANES)) orelse return null;
    const u32_max: f32 = @floatFromInt(@as(u32, std.math.maxInt(u32)));
    for (0..NUM_HYPERPLANES) |h| {
        for (0..RSQ_DIM) |d| {
            const seed = XxHash3.hash(@as(u64, h) * RSQ_DIM, &std.mem.toBytes(@as(c_int, @intCast(d))));
            const low: u32 = @truncate(seed);
            hyperplanes[h][d] = (@as(f32, @floatFromInt(low)) / u32_max) - PSE_ROUND_BIAS;
        }
    }
    return hyperplanes;
}

/// Phase 1b: decode per-function minhash/profile/API/type/deco vectors in
/// parallel.  Must be called after cbm_sem_ensure_ready() so the pretrained
/// token map is initialized.
fn phase1bDecodeAndBuild(funcs: [*c]Func, node_ptrs: NodePtrs, gbuf: ?*const Gbuf, func_count: c_int, worker_count: c_int) void {
    if (func_count <= 0) return;
    var cc: CollectCtx = .{
        .funcs = funcs,
        .node_ptrs = node_ptrs,
        .gbuf = gbuf,
        .func_count = func_count,
        .next_idx = .init(0),
    };
    c.cbm_parallel_for(worker_count, collectWorker, &cc, parallelOpts(worker_count));
}

/// Phase 2: tokenize each function's metadata in parallel, filling
/// all_tokens[] and token_counts[].  Caller allocates the arrays.
fn phase2Tokenize(node_ptrs: NodePtrs, gbuf: ?*Gbuf, all_tokens: [*c]MutStr, token_counts: [*c]c_int, func_count: c_int, worker_count: c_int, pools: [*c]?*HashTable) void {
    var tc: TokenizeCtx = .{
        .node_ptrs = node_ptrs,
        .gbuf = gbuf,
        .all_tokens = all_tokens,
        .token_counts = token_counts,
        .func_count = func_count,
        .next_idx = .init(0),
        .pools = pools,
    };
    c.cbm_parallel_for(worker_count, tokenizeWorker, &tc, parallelOpts(worker_count));
}

/// Phase 4a: build per-function TF-IDF + RI vectors in parallel, producing
/// int8-quantized qvecs for subsequent storage.  Phase 4b runs sequentially
/// to store them in gbuf because gbuf is not thread-safe.
fn phase4BuildAndStoreVectors(gbuf: ?*Gbuf, funcs: [*c]Func, all_tokens: [*c]MutStr, token_counts: [*c]c_int, corpus: ?*c.cbm_sem_corpus_t, func_count: c_int, worker_count: c_int) void {
    const qvecs: [*]u8 = @as(?[*]u8, interop.mallocN(u8, ix(func_count) * SEM_DIM)) orelse return;
    var vc: VecBuildCtx = .{
        .funcs = funcs,
        .all_tokens = all_tokens,
        .token_counts = token_counts,
        .corpus = corpus,
        .qvecs = qvecs,
        .func_count = func_count,
        .next_idx = .init(0),
    };
    c.cbm_parallel_for(worker_count, vecBuildWorker, &vc, parallelOpts(worker_count));
    for (0..ix(func_count)) |f| {
        _ = c.cbm_gbuf_store_vector(gbuf, funcs[f].node_id, qvecs + f * SEM_DIM, SEM_DIM);
    }
    c.free(qvecs);
}

/// Phase 5: hyperplane generation → signatures → LSH bucket population.
/// Returns the malloc'd signatures array and band_buckets[] via out-params;
/// caller frees both.
fn phase5LshBuild(funcs: [*c]Func, func_count: c_int, worker_count: c_int, out_signatures: *[*c]u64, out_buckets: *[*c][*c]SemBucket) void {
    const hyperplanes = phase5aBuildHyperplanes();
    const signatures = interop.callocN(u64, ix(func_count));
    if (hyperplanes != null and signatures != null) {
        var sc: SigBuildCtx = .{
            .funcs = funcs,
            .signatures = signatures,
            .hyperplanes = hyperplanes.?,
            .func_count = func_count,
            .next_idx = .init(0),
        };
        c.cbm_parallel_for(worker_count, sigBuildWorker, &sc, parallelOpts(worker_count));
    }
    c.free(@ptrCast(hyperplanes));

    const band_buckets = interop.callocN([*c]SemBucket, SEM_LSH_BANDS);
    if (band_buckets != null) {
        for (0..SEM_LSH_BANDS) |b| {
            band_buckets[b] = interop.callocN(SemBucket, SEM_BUCKET_COUNT);
        }
        phase5cBuildLshBuckets(signatures, func_count, band_buckets);
    }
    out_signatures.* = signatures;
    out_buckets.* = band_buckets;
}

/// Phase 6a: score candidate pairs in parallel and collect deferred edges.
fn phase6aScoreCandidates(funcs: [*c]Func, signatures: [*c]u64, edge_counts: [*c]c_int, band_buckets: [*c][*c]SemBucket, cfg: c.cbm_sem_config_t, worker_bufs: [*c]DeferredEdgeBuf, func_count: c_int, worker_count: c_int) void {
    var sc: ScoreCtx = .{
        .funcs = funcs,
        .signatures = signatures,
        .edge_counts = edge_counts,
        .cfg = cfg,
        .func_count = func_count,
        .band_buckets = band_buckets,
        .worker_bufs = worker_bufs,
        .max_workers = worker_count,
        .next_idx = .init(0),
    };
    c.cbm_parallel_for(worker_count, scoreWorker, &sc, parallelOpts(worker_count));
}

/// Phase 7: free LSH bucket storage (items arrays and the per-band arrays).
fn freeLshBuckets(band_buckets: [*c][*c]SemBucket) void {
    if (band_buckets == null) return;
    for (0..SEM_LSH_BANDS) |b| {
        if (band_buckets[b] == null) continue;
        for (0..SEM_BUCKET_COUNT) |h| {
            c.free(band_buckets[b][h].items);
        }
        c.free(band_buckets[b]);
    }
    c.free(@ptrCast(band_buckets));
}

/// Phases 3a/3b/3c bundled: create corpus, batch-add docs, finalize, export
/// enriched token vectors to the graph buffer.  Returns the new corpus, which
/// the caller must cbm_sem_corpus_free() later.
fn runCorpusPhase(gbuf: ?*Gbuf, all_tokens: [*c]MutStr, token_counts: [*c]c_int, func_count: c_int) ?*c.cbm_sem_corpus_t {
    const t_phase3a = util.profStart();
    const corpus = c.cbm_sem_corpus_new();
    c.cbm_sem_corpus_add_docs_batch(corpus, all_tokens, token_counts, func_count, MAX_TOKENS);
    util.profEnd("semantic_edges", "3a_corpus_batch", &t_phase3a, func_count);

    const t_phase3b = util.profStart();
    c.cbm_sem_corpus_finalize(corpus);
    util.profEnd("semantic_edges", "3b_corpus_finalize_seq", &t_phase3b, c.cbm_sem_corpus_token_count(corpus));

    const t_phase3c = util.profStart();
    phase3cExportTokenVectors(gbuf, corpus);
    util.profEnd("semantic_edges", "3c_token_vec_export_seq", &t_phase3c, c.cbm_sem_corpus_token_count(corpus));
    return corpus;
}

/// Phases 6a/6b bundled: parallel scoring + sequential merge of deferred
/// edges.  Returns the number of emitted edges.
fn runScoringPhase(gbuf: ?*Gbuf, funcs: [*c]Func, signatures: [*c]u64, band_buckets: [*c][*c]SemBucket, cfg: c.cbm_sem_config_t, func_count: c_int, worker_count: c_int) c_int {
    const edge_counts = interop.callocN(c_int, ix(func_count));
    const worker_bufs = interop.callocN(DeferredEdgeBuf, ix(worker_count));
    if (edge_counts == null or worker_bufs == null) {
        c.free(edge_counts);
        c.free(worker_bufs);
        return 0;
    }
    for (0..ix(worker_count)) |w| deferredBufInit(util.at(worker_bufs, w));

    const t_phase6a = util.profStart();
    phase6aScoreCandidates(funcs, signatures, edge_counts, band_buckets, cfg, worker_bufs, func_count, worker_count);
    util.profEnd("semantic_edges", "6a_score_parallel", &t_phase6a, func_count);

    const t_phase6b = util.profStart();
    const total = phase6bMergeEdges(gbuf, worker_bufs, worker_count, edge_counts, cfg.max_edges);
    util.profEnd("semantic_edges", "6b_edge_merge_seq", &t_phase6b, total);

    c.free(worker_bufs);
    c.free(edge_counts);
    return total;
}

fn freeTokenPoolEntry(key: CStr, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = key; // key == value: one owned string per unique token
    _ = ud;
    c.free(value);
}

/// Free the per-function arrays malloc'd during phases 1b and 4a.
/// all_tokens slots BORROW their strings from the per-worker intern pools;
/// the pools own exactly one copy per unique token per worker.
fn freeFuncsAndTokens(funcs: [*c]Func, func_count: c_int, all_tokens: [*c]MutStr, pools: [*c]?*HashTable, worker_count: c_int) void {
    for (0..ix(func_count)) |f| {
        c.free(funcs[f].tfidf_indices);
        c.free(funcs[f].tfidf_weights);
    }
    c.free(@ptrCast(all_tokens));
    c.free(funcs);
    if (pools != null) {
        for (0..ix(worker_count)) |w| {
            if (pools[w]) |pool| {
                c.cbm_ht_foreach(pool, freeTokenPoolEntry, null);
                c.cbm_ht_free(pool);
            }
        }
        c.free(@ptrCast(pools));
    }
}

fn logCount(msg: [*:0]const u8, key: [*:0]const u8, value: c_int) void {
    interop.logInfo(msg, .{ @as(CStr, key), @as(CStr, c.cbm_log_itoa(value)) });
}

// ── Pass entry point ────────────────────────────────────────────────

pub export fn cbm_pipeline_pass_semantic_edges(ctx: *c.cbm_pipeline_ctx_t) c_int {
    // Controlled by pipeline mode (moderate/full), not env var
    interop.logInfo("pass.start", .{ @as(CStr, "pass"), @as(CStr, "semantic_edges") });

    const gbuf = ctx.gbuf;
    const cfg = c.cbm_sem_get_config();

    const t_phase1a = util.profStart();
    var funcs: [*c]Func = null;
    var node_ptrs: NodePtrs = null;
    const func_count = phase1ScanFunctions(gbuf, &funcs, &node_ptrs);
    util.profEnd("semantic_edges", "1a_scan_seq", &t_phase1a, func_count);

    // Phase 1b: Decode minhash + profile + build api/type/deco vectors (PARALLEL).
    c.cbm_sem_ensure_ready();
    const t_phase1b = util.profStart();
    phase1bDecodeAndBuild(funcs, node_ptrs, gbuf, func_count, c.cbm_default_worker_count(false));
    util.profEnd("semantic_edges", "1b_decode_build_parallel", &t_phase1b, func_count);
    logCount("pass.semantic.collected", "functions", func_count);

    if (func_count < PSE_MIN_FUNCS_FOR_PAIR) {
        c.free(funcs);
        c.free(@ptrCast(node_ptrs));
        interop.logInfo("pass.done", .{
            @as(CStr, "pass"), @as(CStr, "semantic_edges"), @as(CStr, "edges"), @as(CStr, "0"),
        });
        return 0;
    }

    // Phase 2: Tokenize all nodes (PARALLEL)
    const worker_count = c.cbm_default_worker_count(false);
    const all_tokens = interop.mallocN(MutStr, ix(func_count) * MAX_TOKENS);
    const token_counts = interop.callocN(c_int, ix(func_count));

    const t_phase2 = util.profStart();
    const token_pools = interop.callocN(?*HashTable, ix(worker_count));
    if (token_pools != null) {
        for (0..ix(worker_count)) |w| {
            token_pools[w] = c.cbm_ht_create(c.CBM_SZ_1K);
        }
    }
    phase2Tokenize(node_ptrs, gbuf, all_tokens, token_counts, func_count, worker_count, token_pools);
    util.profEnd("semantic_edges", "2_tokenize_parallel", &t_phase2, func_count);
    c.free(@ptrCast(node_ptrs));

    // Phase 3: Build corpus (batch add), finalize, export enriched token vectors.
    const corpus = runCorpusPhase(gbuf, all_tokens, token_counts, func_count);

    // Phase 4: Build per-function TF-IDF + RI vectors (PARALLEL) and store them.
    const t_phase4 = util.profStart();
    phase4BuildAndStoreVectors(gbuf, funcs, all_tokens, token_counts, corpus, func_count, worker_count);
    util.profEnd("semantic_edges", "4_build_and_store_vec", &t_phase4, func_count);

    logCount("pass.semantic.vectors_stored", "count", func_count);

    // Phase 5: LSH hyperplanes → signatures → buckets.
    const t_phase5 = util.profStart();
    var signatures: [*c]u64 = null;
    var band_buckets: [*c][*c]SemBucket = null;
    phase5LshBuild(funcs, func_count, worker_count, &signatures, &band_buckets);
    util.profEnd("semantic_edges", "5_lsh_build", &t_phase5, func_count);

    interop.logInfo("pass.semantic.lsh_built", .{
        @as(CStr, "functions"), @as(CStr, c.cbm_log_itoa(func_count)),
        @as(CStr, "bands"),     @as(CStr, c.cbm_log_itoa(SEM_LSH_BANDS)),
    });

    // Phase 6: Parallel scoring + sequential edge merge.
    const total_edges = runScoringPhase(gbuf, funcs, signatures, band_buckets, cfg, func_count, worker_count);

    // Phase 7: Cleanup
    const t_phase7 = util.profStart();
    freeLshBuckets(band_buckets);
    c.free(signatures);
    interop.logInfo("pass.done", .{
        @as(CStr, "pass"),  @as(CStr, "semantic_edges"),
        @as(CStr, "edges"), @as(CStr, c.cbm_log_itoa(total_edges)),
    });
    freeFuncsAndTokens(funcs, func_count, all_tokens, token_pools, worker_count);
    c.free(token_counts);
    c.cbm_sem_corpus_free(corpus);
    util.profEnd("semantic_edges", "7_cleanup", &t_phase7, 0);

    return 0;
}
