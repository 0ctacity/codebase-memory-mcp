//! semantic.zig — Algorithmic code embeddings: TF-IDF, Random Indexing,
//! API/Type/Decorator signatures, combined scoring, graph diffusion.
//!
//! Implements src/semantic/semantic.h. All signals computed from graph buffer
//! metadata — no source file reads. Uses xxHash for deterministic random
//! vectors; std.hash.XxHash3 produces the same values as the vendored xxHash.
//!
//! Float expressions go through fp.mulAdd wherever the C code wrote a
//! product plus a term in one expression: see fp.zig.

const std = @import("std");
const c = @import("c");
const fp = @import("fp.zig");
const interop = @import("../foundation/interop.zig");

const XxHash3 = std.hash.XxHash3;
const AtomicInt = std.atomic.Value(c_int);

const Vec = c.cbm_sem_vec_t;
const Func = c.cbm_sem_func_t;
const Str = [*:0]const u8;
const CStr = [*c]const u8;

// ── Constants ───────────────────────────────────────────────────────

const DIM: comptime_int = c.CBM_SEM_DIM;
const SPARSE_NNZE: comptime_int = c.CBM_SEM_SPARSE_NNZE;
const WINDOW: comptime_int = c.CBM_SEM_WINDOW;
const MAX_OCCUR: comptime_int = c.CBM_SEM_MAX_OCCUR;
const AST_PROFILE_DIMS: comptime_int = c.CBM_SEM_AST_PROFILE_DIMS;
const PRETRAINED_TOKEN_COUNT: comptime_int = c.PRETRAINED_TOKEN_COUNT;
const PRETRAINED_DIM: comptime_int = c.PRETRAINED_DIM;
const NOT_FOUND: c_int = c.CBM_NOT_FOUND;

const TOKEN_BUF_LEN = 128;
const CORPUS_INIT_CAP = 4096;
const DOC_TOKENS_INIT = 64;
const RI_SEED_BASE = 0x52494E44; // "RIND"

/// Default signal weights for cbm_sem_combined_score. Must sum to ~1.0;
/// proximity is a multiplier applied on top.
const W_TFIDF: f32 = 0.20;
const W_RI: f32 = 0.25;
const W_MINHASH: f32 = 0.10;
const W_API: f32 = 0.15;
const W_TYPE: f32 = 0.10;
const W_DECORATOR: f32 = 0.05;
const W_STRUCT_PROFILE: f32 = 0.10;
const W_DATAFLOW: f32 = 0.05;

/// Threshold bounds for CBM_SEMANTIC_THRESHOLD env override.
const THRESHOLD_MIN: f32 = 0.0;
const THRESHOLD_MAX: f32 = 1.0;

/// Epsilon for denominator guards in cosine math.
const DENOM_EPS: f32 = 1e-10;

/// Int8 quantization bound for vector storage (maps [-1.0, 1.0] to [-127, 127]).
const INT8_MAX: f32 = 127.0;

/// Unit vector bound (normalized similarity / cosine value limit).
const UNIT_POS: f32 = 1.0;

/// Proximity boost: same-file gets 1.0 + PROX_MAX_BOOST, distant gets 1.0.
const PROX_MAX_BOOST: f32 = 0.10;

/// Reflective Random Indexing blend factors for pass2 (context mixing).
const RRI_ALPHA: f32 = 0.3;
const RRI_BETA: f32 = 0.7;

/// Rounding offset for the int8 quantization.
const IDF_BASE: f32 = 0.5;

/// Worker tile/chunk sizes for parallel cooccurrence passes.
const WORKER_STACK_CAP = 256;
const TILE_SIZE = 40;
const SEEN_INIT_CAP = 256;
const RESOLVE_CHUNK = 64;
const COOCCUR_CHUNK = 32;
const RRI_TILE = 128;

/// Mutex-init state machine for the lazy-initialized pretrained token map.
const MTX_STATE_UNINIT = 0;
const MTX_STATE_INITIALIZING = 1;
const MTX_STATE_READY = 2;

/// Pretrained map ready flag (signals a one-shot atomic transition 0 → 1).
const MAP_READY = 1;

/// Numeric conversion radix for strtol (base 10 decimal).
const BASE_DECIMAL = 10;

/// A C int used as an array index.
inline fn ix(i: c_int) usize {
    return @intCast(i);
}

/// &p[i] for a C pointer. Plain indexing gives a `*allowzero T`, which does
/// not coerce to `*T`.
inline fn at(p: anytype, i: usize) @TypeOf(&p[0..1][0]) {
    return &p[i .. i + 1][0];
}

// ── Configuration ───────────────────────────────────────────────────

pub export fn cbm_sem_get_config() c.cbm_sem_config_t {
    var cfg: c.cbm_sem_config_t = .{
        .w_tfidf = W_TFIDF,
        .w_ri = W_RI,
        .w_minhash = W_MINHASH,
        .w_api = W_API,
        .w_type = W_TYPE,
        .w_decorator = W_DECORATOR,
        .w_struct_profile = W_STRUCT_PROFILE,
        .w_dataflow = W_DATAFLOW,
        .threshold = @floatCast(c.CBM_SEM_EDGE_THRESHOLD),
        .max_edges = c.CBM_SEM_MAX_EDGES,
    };
    const thresh: CStr = c.getenv("CBM_SEMANTIC_THRESHOLD");
    if (thresh != null) {
        // strtod reports errors via endptr; reject non-numeric input silently.
        var end: [*c]u8 = null;
        const parsed = c.strtod(thresh, &end);
        if (end != thresh and parsed > THRESHOLD_MIN and parsed <= THRESHOLD_MAX) {
            cfg.threshold = @floatCast(parsed);
        }
    }
    return cfg;
}

pub export fn cbm_sem_is_enabled() bool {
    const val: CStr = c.getenv("CBM_SEMANTIC_ENABLED");
    return val != null and val[0] == '1';
}

// ── Token extraction ────────────────────────────────────────────────

/// True for characters that terminate a token regardless of case.
fn isTokenDelim(ch: u8) bool {
    return switch (ch) {
        '.', '/', '_', '-', ' ', '(', ')', ',', ':' => true,
        else => false,
    };
}

/// True for a camelCase transition: uppercase letter preceded by a lowercase.
fn isCamelBreak(name: Str, i: usize) bool {
    if (i == 0) return false;
    const ch = name[i];
    const p = name[i - 1];
    return ch >= 'A' and ch <= 'Z' and p >= 'a' and p <= 'z';
}

/// Flush the current buffer as a token into out[].
fn flushToken(buf: *[TOKEN_BUF_LEN]u8, blen: *usize, out: [*][*c]u8, count: *c_int, max_out: c_int) void {
    if (blen.* > 0 and count.* < max_out) {
        buf[blen.*] = 0;
        out[ix(count.*)] = c.strdup(buf);
        count.* += 1;
    }
    blen.* = 0;
}

/// Cross-language abbreviation table — covers Go, Python, JS/TS, Rust,
/// Java, C/C++, Ruby, PHP, Kotlin, Swift, Scala, C#, and common patterns.
const abbrevs = [_][2][:0]const u8{
    // Error/exception handling
    .{ "err", "error" },
    .{ "exc", "exception" },
    .{ "ex", "exception" },
    // Context/config
    .{ "ctx", "context" },
    .{ "cfg", "config" },
    .{ "conf", "configuration" },
    .{ "env", "environment" },
    .{ "opt", "option" },
    .{ "opts", "options" },
    // Request/response (HTTP, RPC)
    .{ "req", "request" },
    .{ "res", "response" },
    .{ "resp", "response" },
    .{ "rsp", "response" },
    .{ "hdr", "header" },
    .{ "hdrs", "headers" },
    // Strings/formatting
    .{ "str", "string" },
    .{ "fmt", "format" },
    .{ "msg", "message" },
    .{ "txt", "text" },
    .{ "lbl", "label" },
    .{ "desc", "description" },
    // Data structures
    .{ "buf", "buffer" },
    .{ "arr", "array" },
    .{ "vec", "vector" },
    .{ "lst", "list" },
    .{ "dict", "dictionary" },
    .{ "tbl", "table" },
    .{ "stk", "stack" },
    .{ "que", "queue" },
    // Functions/callbacks
    .{ "fn", "function" },
    .{ "func", "function" },
    .{ "cb", "callback" },
    .{ "proc", "procedure" },
    .{ "ctor", "constructor" },
    .{ "dtor", "destructor" },
    // Database/storage
    .{ "db", "database" },
    .{ "col", "column" },
    .{ "tbl", "table" },
    .{ "stmt", "statement" },
    .{ "txn", "transaction" },
    .{ "trx", "transaction" },
    .{ "repo", "repository" },
    // Auth/security
    .{ "auth", "authentication" },
    .{ "authz", "authorization" },
    .{ "perm", "permission" },
    .{ "cred", "credential" },
    .{ "tok", "token" },
    .{ "pwd", "password" },
    // Values/types
    .{ "val", "value" },
    .{ "num", "number" },
    .{ "int", "integer" },
    .{ "bool", "boolean" },
    .{ "flt", "float" },
    .{ "dbl", "double" },
    // Indexing/iteration
    .{ "idx", "index" },
    .{ "iter", "iterator" },
    .{ "elem", "element" },
    .{ "cnt", "count" },
    .{ "len", "length" },
    .{ "sz", "size" },
    .{ "pos", "position" },
    .{ "off", "offset" },
    .{ "cap", "capacity" },
    // Lifecycle
    .{ "init", "initialize" },
    .{ "deinit", "deinitialize" },
    .{ "alloc", "allocate" },
    .{ "dealloc", "deallocate" },
    .{ "del", "delete" },
    .{ "rm", "remove" },
    // Implementation/interface
    .{ "impl", "implementation" },
    .{ "iface", "interface" },
    .{ "abs", "abstract" },
    .{ "decl", "declaration" },
    // Parameters/attributes
    .{ "param", "parameter" },
    .{ "arg", "argument" },
    .{ "attr", "attribute" },
    .{ "prop", "property" },
    .{ "ret", "return" },
    // Source/destination
    .{ "src", "source" },
    .{ "dst", "destination" },
    .{ "tgt", "target" },
    .{ "orig", "original" },
    .{ "prev", "previous" },
    .{ "cur", "current" },
    .{ "tmp", "temporary" },
    .{ "temp", "temporary" },
    // Networking/IO
    .{ "conn", "connection" },
    .{ "sess", "session" },
    .{ "sock", "socket" },
    .{ "addr", "address" },
    .{ "url", "uniform" },
    .{ "srv", "server" },
    .{ "cli", "client" },
    .{ "svc", "service" },
    .{ "ep", "endpoint" },
    // Management
    .{ "mgr", "manager" },
    .{ "ctrl", "controller" },
    .{ "hdlr", "handler" },
    .{ "sched", "scheduler" },
    .{ "disp", "dispatcher" },
    .{ "reg", "registry" },
    // Async/concurrent
    .{ "chan", "channel" },
    .{ "sem", "semaphore" },
    .{ "mtx", "mutex" },
    .{ "wg", "waitgroup" },
    .{ "sig", "signal" },
    .{ "evt", "event" },
    .{ "sub", "subscriber" },
    .{ "pub", "publisher" },
    // Testing
    .{ "spec", "specification" },
    .{ "mock", "mock" },
    .{ "stub", "stub" },
    .{ "assert", "assertion" },
    // Logging/monitoring
    .{ "log", "logging" },
    .{ "lvl", "level" },
    .{ "dbg", "debug" },
    .{ "wrn", "warning" },
    .{ "inf", "info" },
    // Time
    .{ "ts", "timestamp" },
    .{ "dur", "duration" },
    .{ "ttl", "timetolive" },
    // Miscellaneous
    .{ "ver", "version" },
    .{ "ns", "namespace" },
    .{ "pkg", "package" },
    .{ "mod", "module" },
    .{ "lib", "library" },
    .{ "dep", "dependency" },
    .{ "ref", "reference" },
    .{ "ptr", "pointer" },
    .{ "obj", "object" },
    .{ "doc", "document" },
    .{ "cmd", "command" },
    .{ "ops", "operations" },
    .{ "util", "utility" },
    .{ "hlp", "helper" },
    .{ "ext", "extension" },
};

pub export fn cbm_sem_tokenize(name_c: ?Str, out_c: ?[*][*c]u8, max_out: c_int) c_int {
    const name = name_c orelse return 0;
    const out = out_c orelse return 0;
    if (max_out <= 0) return 0;
    var count: c_int = 0;
    var buf: [TOKEN_BUF_LEN]u8 = undefined;
    var blen: usize = 0;

    var i: usize = 0;
    while (name[i] != 0 and count < max_out) : (i += 1) {
        const ch = name[i];
        const split = isTokenDelim(ch);
        const camel = isCamelBreak(name, i);
        if (split or camel) {
            flushToken(&buf, &blen, out, &count, max_out);
            if (split) continue;
        }
        if (blen < TOKEN_BUF_LEN - 1 and std.ascii.isAlphanumeric(ch)) {
            buf[blen] = std.ascii.toLower(ch);
            blen += 1;
        }
    }
    flushToken(&buf, &blen, out, &count, max_out);

    // Abbreviation expansion: add expanded forms for common code abbreviations.
    // "err" → also add "error", "ctx" → "context", etc.
    const orig_count = count;
    var t: c_int = 0;
    while (t < orig_count and count < max_out) : (t += 1) {
        const tok = std.mem.span(out[ix(t)]);
        for (abbrevs) |ab| {
            if (std.mem.eql(u8, tok, ab[0])) {
                out[ix(count)] = c.strdup(ab[1]);
                count += 1;
                break;
            }
        }
    }

    return count;
}

// ── Dense vector operations ─────────────────────────────────────────

pub export fn cbm_sem_cosine(a_c: ?*const Vec, b_c: ?*const Vec) f32 {
    const a = a_c orelse return 0.0;
    const b = b_c orelse return 0.0;
    var dot: f32 = 0.0;
    var mag_a: f32 = 0.0;
    var mag_b: f32 = 0.0;
    for (a.v, b.v) |x, y| {
        dot = fp.mulAdd(f32, x, y, dot);
        mag_a = fp.mulAdd(f32, x, x, mag_a);
        mag_b = fp.mulAdd(f32, y, y, mag_b);
    }
    const denom = @sqrt(mag_a) * @sqrt(mag_b);
    if (denom < DENOM_EPS) return 0.0;
    return dot / denom;
}

/// Pretrained token lookup table — built lazily on first use.
var g_pretrained_map: ?*c.CBMHashTable = null;
var g_pretrained_ready: AtomicInt = .init(0);
var g_pretrained_mtx: c.cbm_mutex_t = undefined;
var g_pretrained_mtx_init: AtomicInt = .init(MTX_STATE_UNINIT);

/// Thread-safe lazy init of the pretrained token lookup map.
/// Uses double-checked locking: fast path reads an atomic flag.
fn ensurePretrainedMap() void {
    if (g_pretrained_ready.load(.acquire) != 0) return;
    // First-time init of the mutex itself (also needs to be thread-safe)
    if (g_pretrained_mtx_init.cmpxchgStrong(
        MTX_STATE_UNINIT,
        MTX_STATE_INITIALIZING,
        .acq_rel,
        .acquire,
    ) == null) {
        c.cbm_mutex_init(&g_pretrained_mtx);
        g_pretrained_mtx_init.store(MTX_STATE_READY, .release);
    } else {
        // Spin until another thread finishes initializing the mutex
        while (g_pretrained_mtx_init.load(.acquire) != MTX_STATE_READY) {
            // brief spin
        }
    }
    c.cbm_mutex_lock(&g_pretrained_mtx);
    if (g_pretrained_ready.load(.acquire) == 0) {
        g_pretrained_map = c.cbm_ht_create(PRETRAINED_TOKEN_COUNT);
        var idx_buf: [c.CBM_SZ_16]u8 = undefined;
        for (0..PRETRAINED_TOKEN_COUNT) |i| {
            const tok: CStr = c.PRETRAINED_TOKENS[i];
            if (tok != null and tok[0] != 0) {
                _ = c.snprintf(&idx_buf, idx_buf.len, "%d", @as(c_int, @intCast(i)));
                _ = c.cbm_ht_set(g_pretrained_map, c.strdup(tok), c.strdup(&idx_buf));
            }
        }
        g_pretrained_ready.store(MAP_READY, .release);
    }
    c.cbm_mutex_unlock(&g_pretrained_mtx);
}

pub export fn cbm_sem_ensure_ready() void {
    ensurePretrainedMap();
}

/// The pretrained int8 vector for a token, or null when the token is not in
/// the pretrained vocabulary. The map must be initialized.
fn pretrainedVec(token: CStr) ?[*]const i8 {
    const idx_str: CStr = @ptrCast(c.cbm_ht_get(g_pretrained_map, token));
    if (idx_str == null) return null;
    var end: [*c]u8 = null;
    const idx = c.strtol(idx_str, &end, BASE_DECIMAL);
    if (end != idx_str and idx >= 0 and idx < PRETRAINED_TOKEN_COUNT) {
        return @ptrCast(c.pretrained_vec_at(@intCast(idx)));
    }
    return null;
}

pub export fn cbm_sem_random_index(token_c: ?Str, out: *Vec) void {
    @memset(&out.v, 0.0);
    const token = token_c orelse return;

    // Try pretrained nomic-embed-code vector first (768d, distilled from 7B).
    ensurePretrainedMap();
    if (pretrainedVec(token)) |pvec| {
        for (out.v[0..@min(DIM, PRETRAINED_DIM)], pvec[0..@min(DIM, PRETRAINED_DIM)]) |*o, p| {
            o.* = @as(f32, @floatFromInt(p)) / INT8_MAX;
        }
        return;
    }

    // Fallback: sparse random vector for tokens not in pretrained vocab.
    const seed = XxHash3.hash(0, std.mem.span(token));
    for (0..SPARSE_NNZE) |i| {
        const h = XxHash3.hash(seed +% RI_SEED_BASE, &std.mem.toBytes(@as(c_int, @intCast(i))));
        const pos: usize = @intCast(h % DIM);
        const sign: f32 = if (h & 1 != 0) UNIT_POS else -UNIT_POS;
        out.v[pos] += sign;
    }
}

pub export fn cbm_sem_normalize(v_c: ?*Vec) void {
    const v = v_c orelse return;
    var mag: f32 = 0.0;
    for (v.v) |x| {
        mag = fp.mulAdd(f32, x, x, mag);
    }
    mag = @sqrt(mag);
    if (mag < DENOM_EPS) return;
    const inv = UNIT_POS / mag;
    for (&v.v) |*x| {
        x.* *= inv;
    }
}

pub export fn cbm_sem_vec_add_scaled(dst_c: ?*Vec, src_c: ?*const Vec, scale: f32) void {
    const dst = dst_c orelse return;
    const src = src_c orelse return;
    for (&dst.v, src.v) |*d, s| {
        d.* = fp.mulAdd(f32, scale, s, d.*);
    }
}

// ── Corpus (IDF + Random Indexing enrichment) ───────────────────────

const CorpusEntry = extern struct {
    token: [*c]u8,
    doc_freq: c_int,
    enriched_vec: Vec, // context-enriched via co-occurrence
};

const Corpus = extern struct {
    token_map: ?*c.CBMHashTable, // token → index into entries[]
    entries: [*c]CorpusEntry,
    entry_count: c_int,
    entry_cap: c_int,
    doc_count: c_int,
    finalized: bool,

    // Per-document token lists for co-occurrence pass
    doc_token_ids: [*c][*c]c_int,
    doc_token_counts: [*c]c_int,
    doc_cap: c_int,
};

/// Parse a decimal index string via strtol, returning CBM_NOT_FOUND on parse
/// failure. Centralizes the atoi-replacement pattern for token_map lookups.
fn parseTokenIndex(idx_str: CStr) c_int {
    if (idx_str == null) return NOT_FOUND;
    var end: [*c]u8 = null;
    const parsed = c.strtol(idx_str, &end, BASE_DECIMAL);
    return if (end != idx_str) @truncate(parsed) else NOT_FOUND;
}

fn corpusGetOrAdd(cp: *Corpus, token: CStr) c_int {
    var idx_buf: [c.CBM_SZ_16]u8 = undefined;
    const existing: CStr = @ptrCast(c.cbm_ht_get(cp.token_map, token));
    if (existing != null) {
        return parseTokenIndex(existing);
    }
    if (cp.entry_count >= cp.entry_cap) {
        const new_cap: c_int = if (cp.entry_cap < CORPUS_INIT_CAP) CORPUS_INIT_CAP else cp.entry_cap * 2;
        const grown = interop.reallocN(CorpusEntry, cp.entries, ix(new_cap));
        if (grown == null) return NOT_FOUND;
        cp.entries = grown;
        cp.entry_cap = new_cap;
    }
    const idx = cp.entry_count;
    cp.entry_count += 1;
    const entry = at(cp.entries, ix(idx));
    entry.token = c.strdup(token);
    entry.doc_freq = 0;
    @memset(&entry.enriched_vec.v, 0.0);
    _ = c.snprintf(&idx_buf, idx_buf.len, "%d", idx);
    _ = c.cbm_ht_set(cp.token_map, c.strdup(token), c.strdup(&idx_buf));
    return idx;
}

pub export fn cbm_sem_corpus_new() ?*Corpus {
    const cp: ?*Corpus = interop.callocN(Corpus, 1);
    if (cp) |corpus| {
        corpus.token_map = c.cbm_ht_create(CORPUS_INIT_CAP);
    }
    return cp;
}

pub export fn cbm_sem_corpus_add_doc(corpus_c: ?*Corpus, tokens_c: ?[*]CStr, count: c_int) void {
    const corpus = corpus_c orelse return;
    const tokens = tokens_c orelse return;
    if (count <= 0) return;
    // Track document for co-occurrence pass
    if (corpus.doc_count >= corpus.doc_cap) {
        const new_cap: c_int = if (corpus.doc_cap < DOC_TOKENS_INIT) DOC_TOKENS_INIT else corpus.doc_cap * 2;
        const grown_ids = interop.reallocN([*c]c_int, corpus.doc_token_ids, ix(new_cap));
        const grown_counts = interop.reallocN(c_int, corpus.doc_token_counts, ix(new_cap));
        if (grown_ids == null or grown_counts == null) {
            c.free(@ptrCast(grown_ids));
            c.free(@ptrCast(grown_counts));
            return;
        }
        corpus.doc_token_ids = grown_ids;
        corpus.doc_token_counts = grown_counts;
        corpus.doc_cap = new_cap;
    }
    const doc_idx = ix(corpus.doc_count);
    corpus.doc_count += 1;
    corpus.doc_token_ids[doc_idx] = interop.mallocN(c_int, ix(count));
    corpus.doc_token_counts[doc_idx] = count;

    // Per-doc unique set for IDF
    const seen = interop.callocN(c_int, ix(corpus.entry_cap) + ix(count) + CORPUS_INIT_CAP);
    var seen_count: usize = 0;

    for (0..ix(count)) |i| {
        const tid = corpusGetOrAdd(corpus, tokens[i]);
        corpus.doc_token_ids[doc_idx][i] = tid;
        if (tid < 0) continue;
        // Check uniqueness for IDF (simple linear scan — tokens per doc is small)
        var is_new = true;
        for (0..seen_count) |j| {
            if (seen[j] == tid) {
                is_new = false;
                break;
            }
        }
        if (is_new) {
            seen[seen_count] = tid;
            seen_count += 1;
            corpus.entries[ix(tid)].doc_freq += 1;
        }
    }
    c.free(@ptrCast(seen));
}

// ── Parallel corpus batch build ─────────────────────────────────────
// Strategy:
//   Phase A (SEQUENTIAL): Scan all documents once to build the global
//     token_map (inserts unique tokens, assigns global IDs). This is
//     inherently sequential (hash table mutation), but much faster than
//     the current per-doc add_doc because we avoid the per-doc malloc of
//     the `seen` array and per-doc bookkeeping.
//   Phase B (PARALLEL): Each worker processes a chunk of docs, translates
//     tokens → global IDs via read-only token_map lookups, fills
//     doc_token_ids[d], and accumulates doc_freq contributions via atomics.

const BatchResolveCtx = struct {
    corpus: *Corpus,
    all_tokens: [*][*c]u8,
    token_counts: [*]const c_int,
    max_tokens: c_int,
    doc_count: c_int,
    doc_freq_atomic: [*]AtomicInt, // per-entry atomic counter (entry_count long)
    next_idx: AtomicInt,
};

/// Resolve one document: look up each token's global ID, fill the corpus
/// doc_token_ids[d], and bump the per-token doc_freq counter atomically. The
/// caller is responsible for ensuring `seen` has capacity for `count` ints
/// before calling (the worker grows its per-thread scratch buffer).
fn batchResolveOneDoc(bc: *BatchResolveCtx, doc_index: usize, seen: [*c]c_int) void {
    const count = bc.token_counts[doc_index];
    if (count <= 0) {
        bc.corpus.doc_token_ids[doc_index] = null;
        bc.corpus.doc_token_counts[doc_index] = 0;
        return;
    }
    const ids = interop.mallocN(c_int, ix(count));
    bc.corpus.doc_token_ids[doc_index] = ids;
    bc.corpus.doc_token_counts[doc_index] = count;

    var seen_count: usize = 0;
    const tokens = bc.all_tokens + doc_index * ix(bc.max_tokens);
    for (0..ix(count)) |i| {
        const tid = parseTokenIndex(@ptrCast(c.cbm_ht_get(bc.corpus.token_map, tokens[i])));
        ids[i] = tid;
        if (tid < 0) continue;
        // Unique-per-doc check for IDF
        var is_new = true;
        for (0..seen_count) |j| {
            if (seen[j] == tid) {
                is_new = false;
                break;
            }
        }
        if (is_new) {
            seen[seen_count] = tid;
            seen_count += 1;
            _ = bc.doc_freq_atomic[ix(tid)].fetchAdd(1, .monotonic);
        }
    }
}

fn batchResolveWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const bc: *BatchResolveCtx = @ptrCast(@alignCast(ctx_ptr));
    // Per-worker scratch for unique-per-doc tracking
    var local_seen_cap: c_int = SEEN_INIT_CAP;
    var seen = interop.mallocN(c_int, ix(local_seen_cap));
    if (seen == null) return;

    while (true) {
        const start = bc.next_idx.fetchAdd(RESOLVE_CHUNK, .monotonic);
        if (start >= bc.doc_count) break;
        var end = start + RESOLVE_CHUNK;
        if (end > bc.doc_count) end = bc.doc_count;
        for (ix(start)..ix(end)) |d| {
            const count = bc.token_counts[d];
            if (count > local_seen_cap) {
                const grown = interop.reallocN(c_int, seen, ix(count));
                if (grown == null) continue;
                seen = grown;
                local_seen_cap = count;
            }
            batchResolveOneDoc(bc, d, seen);
        }
    }
    c.free(@ptrCast(seen));
}

pub export fn cbm_sem_corpus_add_docs_batch(
    corpus_c: ?*Corpus,
    all_tokens_c: ?[*][*c]u8,
    token_counts_c: ?[*]const c_int,
    doc_count: c_int,
    max_tokens_per_doc: c_int,
) void {
    const corpus = corpus_c orelse return;
    const all_tokens = all_tokens_c orelse return;
    const token_counts = token_counts_c orelse return;
    if (doc_count <= 0) return;

    // Phase A (SEQUENTIAL): Build token_map and allocate doc arrays.
    // Hash table mutation can't be parallelized; strdup+insert is the cost.
    if (corpus.doc_cap < corpus.doc_count + doc_count) {
        const new_cap = corpus.doc_count + doc_count;
        const grown_ids = interop.reallocN([*c]c_int, corpus.doc_token_ids, ix(new_cap));
        const grown_counts = interop.reallocN(c_int, corpus.doc_token_counts, ix(new_cap));
        if (grown_ids == null or grown_counts == null) {
            c.free(@ptrCast(grown_ids));
            c.free(@ptrCast(grown_counts));
            return;
        }
        corpus.doc_token_ids = grown_ids;
        corpus.doc_token_counts = grown_counts;
        corpus.doc_cap = new_cap;
    }
    const base_doc = corpus.doc_count;
    corpus.doc_count += doc_count;

    for (0..ix(doc_count)) |d| {
        const count = token_counts[d];
        const tokens = all_tokens + d * ix(max_tokens_per_doc);
        var i: c_int = 0;
        while (i < count) : (i += 1) {
            // Inserts token into token_map if new; we discard return here —
            // Phase B will re-lookup in read-only mode to get the ID.
            _ = corpusGetOrAdd(corpus, tokens[ix(i)]);
        }
    }

    // Phase B (PARALLEL): Resolve tokens → IDs and count doc_freq per entry.
    // token_map is now read-only; each worker owns its doc range (no writes
    // to shared state except atomic doc_freq counters).
    const doc_freq_atomic: ?[*]AtomicInt = interop.callocN(AtomicInt, ix(corpus.entry_count));
    if (doc_freq_atomic == null) {
        // OOM fallback: sequential path. Roll back doc_count first since
        // add_doc increments it itself.
        corpus.doc_count = base_doc;
        for (0..ix(doc_count)) |d| {
            const count = token_counts[d];
            const tokens = all_tokens + d * ix(max_tokens_per_doc);
            cbm_sem_corpus_add_doc(corpus, @ptrCast(tokens), count);
        }
        return;
    }

    const worker_count = c.cbm_default_worker_count(false);
    var bc: BatchResolveCtx = .{
        .corpus = corpus,
        .all_tokens = all_tokens,
        .token_counts = token_counts,
        .max_tokens = max_tokens_per_doc,
        .doc_count = doc_count,
        .doc_freq_atomic = doc_freq_atomic.?,
        .next_idx = .init(0),
    };
    // Temporarily re-base doc arrays so workers write to base_doc..base_doc+doc_count
    corpus.doc_token_ids += ix(base_doc);
    corpus.doc_token_counts += ix(base_doc);
    const opts: c.cbm_parallel_for_opts_t = .{ .max_workers = worker_count, .force_pthreads = false };
    c.cbm_parallel_for(worker_count, batchResolveWorker, &bc, opts);
    corpus.doc_token_ids -= ix(base_doc);
    corpus.doc_token_counts -= ix(base_doc);

    // Phase C (SEQUENTIAL reduce): atomic counters → entries[].doc_freq
    for (0..ix(corpus.entry_count)) |i| {
        corpus.entries[i].doc_freq += doc_freq_atomic.?[i].load(.monotonic);
    }
    c.free(@ptrCast(doc_freq_atomic));
}

// ── Parallel corpus_finalize ────────────────────────────────────────
// Strategy:
//   1. Precompute base RI vectors into a shared array (eliminates ~333M
//      redundant cbm_sem_random_index calls on kernel-scale corpora).
//   2. Co-occurrence passes: partition TARGET tokens across workers so each
//      worker writes to a disjoint range of enriched_vec (zero contention).
//      Each worker still scans all documents but only accumulates for targets
//      in its range. Inner vector add is the parallelized work.
//   3. Normalize/blend loops are trivially parallel per-entry.

/// Reverse index: for each token id, a list of (doc_id, position_in_doc) pairs.
/// Built once, reused for both cooccur passes. Eliminates the O(num_chunks × doc_count)
/// redundant outer scan in the old algorithm.
const CooccurPos = extern struct {
    doc_id: i32,
    pos: i32,
};

const ReverseIndex = extern struct {
    offsets: [*c]c_int, // offsets[entry_count + 1], prefix sum of occurrences
    flat: [*c]CooccurPos, // flat array of positions, total = offsets[entry_count]
};

/// Tagged source vector: sparse (~30% of tokens, 8 inline nonzeros) or dense int8
/// reference into PRETRAINED_VECTOR_BLOB (no copy). Dense path converts int8→float
/// on the fly in the hot loop. Tested with both int8 and float32 blob storage
/// formats — cooccur passes are memory-bandwidth-bound, so the int8 format (4x
/// less source traffic) is equivalent in wall time despite the conversion cost,
/// while saving 90 MB of binary size.
const SrcEntry = extern struct {
    is_sparse: u8, // 1 = sparse path, 0 = dense int8 reference
    nnz: u8, // number of nonzeros used in sparse path
    _pad: u16,
    indices: [SPARSE_NNZE]u16, // 8 * 2 = 16 bytes
    values: [SPARSE_NNZE]f32, // 8 * 4 = 32 bytes
    dense_int8: ?[*]const i8, // points into PRETRAINED_VECTOR_BLOB
};

/// Initialize a target vector from a sparse/dense source.
inline fn semTargetInitFromSrc(dst: *Vec, src: *const SrcEntry) void {
    @memset(&dst.v, 0.0);
    if (src.is_sparse != 0) {
        for (src.indices[0..src.nnz], src.values[0..src.nnz]) |idx, val| {
            dst.v[idx] = val;
        }
    } else {
        const s = src.dense_int8.?;
        const inv127 = UNIT_POS / INT8_MAX;
        for (&dst.v, s[0..DIM]) |*d, x| {
            d.* = inv127 * @as(f32, @floatFromInt(x));
        }
    }
}

/// Add weighted source into target.
/// Sparse path: ~8 operations, ~48 bytes source memory traffic.
/// Dense path: 768 mul-adds with int8→float conversion, ~768 bytes traffic.
inline fn semVecAddSrcScaled(dst: *Vec, src: *const SrcEntry, scale: f32) void {
    if (src.is_sparse != 0) {
        for (src.indices[0..src.nnz], src.values[0..src.nnz]) |idx, val| {
            dst.v[idx] = fp.mulAdd(f32, scale, val, dst.v[idx]);
        }
    } else {
        const s = src.dense_int8.?;
        const mul = scale * (UNIT_POS / INT8_MAX);
        for (&dst.v, s[0..DIM]) |*d, x| {
            d.* = fp.mulAdd(f32, mul, @floatFromInt(x), d.*);
        }
    }
}

/// Pass 1 context: uses sparse/int8 tagged sources (most memory-efficient).
const CooccurSparseCtx = struct {
    entries: [*c]CorpusEntry,
    src_entries: [*c]const SrcEntry, // sparse or int8-dense per token
    doc_token_ids: [*c][*c]c_int,
    doc_token_counts: [*c]const c_int,
    rev: *const ReverseIndex,
    doc_count: c_int,
    entry_count: c_int,
    next_chunk: AtomicInt,
    num_chunks: c_int,
    chunk_size: c_int,

    // Cache-blocked tiling parameters
    tile_size: c_int, // targets per L2-resident tile
};

/// Accumulate co-occurrence context for a single target token into `target`.
/// Reads neighbors within ±CBM_SEM_WINDOW positions across all documents that
/// reference this token id via the reverse index.
fn cooccurSparseOneTarget(cc: *CooccurSparseCtx, tid: c_int, target: *Vec) void {
    const occ_start = cc.rev.offsets[ix(tid)];
    const occ_end = cc.rev.offsets[ix(tid) + 1];
    // Frequent-token subsampling: stride over occurrences when a token is very
    // common so the enriched vector's direction is preserved (it's normalized
    // after) while bounding work to ~CBM_SEM_MAX_OCCUR samples. See semantic.h.
    var occ_step: c_int = 1;
    if (occ_end - occ_start > MAX_OCCUR) {
        occ_step = @divTrunc(occ_end - occ_start, MAX_OCCUR);
    }
    var p = occ_start;
    while (p < occ_end) : (p += occ_step) {
        const d = cc.rev.flat[ix(p)].doc_id;
        const i = cc.rev.flat[ix(p)].pos;
        const ids = cc.doc_token_ids[ix(d)];
        const len = cc.doc_token_counts[ix(d)];
        var w: c_int = -WINDOW;
        while (w <= WINDOW) : (w += 1) {
            if (w == 0) continue;
            const j = i + w;
            if (j < 0 or j >= len) continue;
            const nid = ids[ix(j)];
            if (nid < 0) continue;
            const weight = UNIT_POS / @as(f32, @floatFromInt(@abs(w)));
            semVecAddSrcScaled(target, at(cc.src_entries, ix(nid)), weight);
        }
    }
}

fn cooccurWorkerSparse(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const cc: *CooccurSparseCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const ci = cc.next_chunk.fetchAdd(1, .monotonic);
        if (ci >= cc.num_chunks) break;
        const chunk_start = ci * cc.chunk_size;
        var chunk_end = chunk_start + cc.chunk_size;
        if (chunk_end > cc.entry_count) chunk_end = cc.entry_count;

        // Cache-blocked target tiling: process tile_size targets at a time so
        // their vectors stay resident in L2 cache during their accumulation.
        var tile_start = chunk_start;
        while (tile_start < chunk_end) : (tile_start += cc.tile_size) {
            var tile_end = tile_start + cc.tile_size;
            if (tile_end > chunk_end) tile_end = chunk_end;
            var tid = tile_start;
            while (tid < tile_end) : (tid += 1) {
                const target = &at(cc.entries, ix(tid)).enriched_vec;
                semTargetInitFromSrc(target, at(cc.src_entries, ix(tid)));
                cooccurSparseOneTarget(cc, tid, target);
            }
        }
    }
}

/// Pass 2 (RRI) context: uses int8-quantized pass1 vectors as source.
/// Pass1 outputs are dense float32 post-normalization, values in [-1,1].
/// We quantize to int8 once (×127) to cut source memory traffic 4x.
const CooccurInt8Ctx = struct {
    entries: [*c]CorpusEntry,
    pass1_q: [*]const i8, // [entry_count × CBM_SEM_DIM] int8 quantized pass1
    doc_token_ids: [*c][*c]c_int,
    doc_token_counts: [*c]const c_int,
    rev: *const ReverseIndex,
    doc_count: c_int,
    entry_count: c_int,
    next_chunk: AtomicInt,
    num_chunks: c_int,
    chunk_size: c_int,
    tile_size: c_int,
};

inline fn semVecAddInt8Scaled(dst: *Vec, src: *const [DIM]i8, scale: f32) void {
    const mul = scale * (UNIT_POS / INT8_MAX);
    for (&dst.v, src) |*d, x| {
        d.* = fp.mulAdd(f32, mul, @floatFromInt(x), d.*);
    }
}

/// RRI pass 2 accumulator for a single target token, reading int8-quantized
/// pass1 vectors as the source.
fn cooccurInt8OneTarget(cc: *CooccurInt8Ctx, tid: c_int, target: *Vec) void {
    const occ_start = cc.rev.offsets[ix(tid)];
    const occ_end = cc.rev.offsets[ix(tid) + 1];
    // Frequent-token subsampling (see cooccurSparseOneTarget / semantic.h).
    var occ_step: c_int = 1;
    if (occ_end - occ_start > MAX_OCCUR) {
        occ_step = @divTrunc(occ_end - occ_start, MAX_OCCUR);
    }
    var p = occ_start;
    while (p < occ_end) : (p += occ_step) {
        const d = cc.rev.flat[ix(p)].doc_id;
        const i = cc.rev.flat[ix(p)].pos;
        const ids = cc.doc_token_ids[ix(d)];
        const len = cc.doc_token_counts[ix(d)];
        var w: c_int = -WINDOW;
        while (w <= WINDOW) : (w += 1) {
            if (w == 0) continue;
            const j = i + w;
            if (j < 0 or j >= len) continue;
            const nid = ids[ix(j)];
            if (nid < 0) continue;
            const weight = UNIT_POS / @as(f32, @floatFromInt(@abs(w)));
            semVecAddInt8Scaled(target, cc.pass1_q[ix(nid) * DIM ..][0..DIM], weight);
        }
    }
}

fn cooccurWorkerInt8(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const cc: *CooccurInt8Ctx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const ci = cc.next_chunk.fetchAdd(1, .monotonic);
        if (ci >= cc.num_chunks) break;
        const chunk_start = ci * cc.chunk_size;
        var chunk_end = chunk_start + cc.chunk_size;
        if (chunk_end > cc.entry_count) chunk_end = cc.entry_count;

        var tile_start = chunk_start;
        while (tile_start < chunk_end) : (tile_start += cc.tile_size) {
            var tile_end = tile_start + cc.tile_size;
            if (tile_end > chunk_end) tile_end = chunk_end;
            var tid = tile_start;
            while (tid < tile_end) : (tid += 1) {
                // RRI pass 2 starts from zero (no self-init)
                const target = &at(cc.entries, ix(tid)).enriched_vec;
                @memset(&target.v, 0.0);
                cooccurInt8OneTarget(cc, tid, target);
            }
        }
    }
}

const NormCtx = struct {
    entries: [*c]CorpusEntry,
    entry_count: c_int,
    next_idx: AtomicInt,
};

fn normalizeWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const nc: *NormCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const start = nc.next_idx.fetchAdd(WORKER_STACK_CAP, .monotonic);
        if (start >= nc.entry_count) break;
        var end = start + WORKER_STACK_CAP;
        if (end > nc.entry_count) end = nc.entry_count;
        for (ix(start)..ix(end)) |i| {
            cbm_sem_normalize(&at(nc.entries, i).enriched_vec);
        }
    }
}

const BlendCtx = struct {
    entries: [*c]CorpusEntry,
    pass1: [*]const Vec,
    entry_count: c_int,
    next_idx: AtomicInt,
};

fn blendWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const bc: *BlendCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const start = bc.next_idx.fetchAdd(WORKER_STACK_CAP, .monotonic);
        if (start >= bc.entry_count) break;
        var end = start + WORKER_STACK_CAP;
        if (end > bc.entry_count) end = bc.entry_count;
        for (ix(start)..ix(end)) |i| {
            const vec = &at(bc.entries, i).enriched_vec;
            cbm_sem_normalize(vec);
            for (&vec.v, bc.pass1[i].v) |*e, p1| {
                e.* = fp.mulAdd(f32, RRI_BETA, p1, RRI_ALPHA * e.*);
            }
        }
    }
}

const SrcBuildCtx = struct {
    entries: [*c]CorpusEntry,
    src_entries: [*c]SrcEntry,
    entry_count: c_int,
    next_idx: AtomicInt,
};

/// Build one src_entry for a token: dense int8 reference if in nomic vocab,
/// sparse inline representation otherwise. Collisions in the sparse hash are
/// merged and zeros filtered so the final representation is exactly the same
/// mathematical vector that the old dense path produced.
fn buildSrcEntry(token: CStr, out: *SrcEntry) void {
    out.* = std.mem.zeroes(SrcEntry);
    if (token == null) {
        out.is_sparse = 1;
        out.nnz = 0;
        return;
    }
    // Dense path: direct int8 pointer into pretrained blob (zero-copy).
    if (pretrainedVec(token)) |pvec| {
        out.is_sparse = 0;
        out.dense_int8 = pvec;
        return;
    }
    // Sparse path: compute 8 hash positions with collision merging.
    out.is_sparse = 1;
    var tmp_idx: [SPARSE_NNZE]u16 = undefined;
    var tmp_val: [SPARSE_NNZE]f32 = undefined;
    var count: usize = 0;
    const seed = XxHash3.hash(0, std.mem.span(token));
    for (0..SPARSE_NNZE) |i| {
        const h = XxHash3.hash(seed +% RI_SEED_BASE, &std.mem.toBytes(@as(c_int, @intCast(i))));
        const pos: u16 = @intCast(h % DIM);
        const sign: f32 = if (h & 1 != 0) UNIT_POS else -UNIT_POS;
        // Merge collisions
        var found: ?usize = null;
        for (tmp_idx[0..count], 0..) |idx, j| {
            if (idx == pos) {
                found = j;
                break;
            }
        }
        if (found) |j| {
            tmp_val[j] += sign;
        } else {
            tmp_idx[count] = pos;
            tmp_val[count] = sign;
            count += 1;
        }
    }
    // Filter zeros
    var nnz: u8 = 0;
    for (tmp_idx[0..count], tmp_val[0..count]) |idx, val| {
        if (val != 0.0) {
            out.indices[nnz] = idx;
            out.values[nnz] = val;
            nnz += 1;
        }
    }
    out.nnz = nnz;
}

fn srcBuildWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const sc: *SrcBuildCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const start = sc.next_idx.fetchAdd(WORKER_STACK_CAP, .monotonic);
        if (start >= sc.entry_count) break;
        var end = start + WORKER_STACK_CAP;
        if (end > sc.entry_count) end = sc.entry_count;
        for (ix(start)..ix(end)) |i| {
            buildSrcEntry(sc.entries[i].token, at(sc.src_entries, i));
        }
    }
}

/// Quantize pass1 float vectors to int8 (× 127) for use as pass2 source.
/// Input vectors are unit-normalized, so values are in [-1, 1] → int8 preserves
/// ~99% precision. 4x less memory traffic for pass2.
const Pass1QuantCtx = struct {
    entries: [*c]const CorpusEntry,
    pass1_q: [*]i8,
    entry_count: c_int,
    next_idx: AtomicInt,
};

fn pass1QuantizeWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    _ = worker_id;
    const qc: *Pass1QuantCtx = @ptrCast(@alignCast(ctx_ptr));
    while (true) {
        const start = qc.next_idx.fetchAdd(RRI_TILE, .monotonic);
        if (start >= qc.entry_count) break;
        var end = start + RRI_TILE;
        if (end > qc.entry_count) end = qc.entry_count;
        for (ix(start)..ix(end)) |i| {
            const src = &at(qc.entries, i).enriched_vec;
            const dst = qc.pass1_q[i * DIM ..][0..DIM];
            for (dst, src.v) |*q, x| {
                var v = x * INT8_MAX;
                if (v > INT8_MAX) v = INT8_MAX;
                if (v < -INT8_MAX) v = -INT8_MAX;
                q.* = @intFromFloat(if (v >= 0) v + IDF_BASE else v - IDF_BASE);
            }
        }
    }
}

/// Build reverse index: token_id → list of (doc_id, position) pairs.
/// SEQUENTIAL (fast: just pointer arithmetic + flat array fill).
fn buildReverseIndex(corpus: *Corpus) ?*ReverseIndex {
    const rev: *ReverseIndex = @as(?*ReverseIndex, interop.callocN(ReverseIndex, 1)) orelse return null;
    const entry_count = ix(corpus.entry_count);
    const doc_count = ix(corpus.doc_count);
    // Phase A: count occurrences per token
    const counts = interop.callocN(c_int, entry_count + 1);
    if (counts == null) {
        c.free(rev);
        return null;
    }
    var total: c_long = 0;
    for (0..doc_count) |d| {
        const ids = corpus.doc_token_ids[d];
        const len = corpus.doc_token_counts[d];
        var i: c_int = 0;
        while (i < len) : (i += 1) {
            const tid = ids[ix(i)];
            if (tid >= 0 and tid < corpus.entry_count) {
                counts[ix(tid)] += 1;
                total += 1;
            }
        }
    }
    // Phase B: exclusive prefix sum → offsets[]
    rev.offsets = interop.mallocN(c_int, entry_count + 1);
    if (rev.offsets == null) {
        c.free(@ptrCast(counts));
        c.free(rev);
        return null;
    }
    var running: c_int = 0;
    for (0..entry_count) |t| {
        rev.offsets[t] = running;
        running += counts[t];
        counts[t] = 0; // reuse as per-token fill cursor
    }
    rev.offsets[entry_count] = running;
    // Phase C: fill flat array. Ensure allocation size > 0 even for empty
    // corpora (avoids malloc(0) which is implementation-defined).
    rev.flat = interop.mallocN(CooccurPos, if (total > 0) @intCast(total) else 1);
    if (rev.flat == null) {
        c.free(@ptrCast(rev.offsets));
        c.free(@ptrCast(counts));
        c.free(rev);
        return null;
    }
    for (0..doc_count) |d| {
        const ids = corpus.doc_token_ids[d];
        const len = corpus.doc_token_counts[d];
        var i: c_int = 0;
        while (i < len) : (i += 1) {
            const tid = ids[ix(i)];
            if (tid >= 0 and tid < corpus.entry_count) {
                const slot = ix(rev.offsets[ix(tid)] + counts[ix(tid)]);
                counts[ix(tid)] += 1;
                rev.flat[slot].doc_id = @intCast(d);
                rev.flat[slot].pos = i;
            }
        }
    }
    c.free(@ptrCast(counts));
    return rev;
}

fn freeReverseIndex(rev_c: ?*ReverseIndex) void {
    const rev = rev_c orelse return;
    c.free(@ptrCast(rev.offsets));
    c.free(@ptrCast(rev.flat));
    c.free(rev);
}

/// Bundle of parameters shared by the finalize sub-phases.
const FinalizeParams = struct {
    corpus: *Corpus,
    rev: *ReverseIndex,
    src_entries: [*c]SrcEntry,
    worker_count: c_int,
    num_chunks: c_int,
    chunk_size: c_int,
    tile_size: c_int,
    opts: c.cbm_parallel_for_opts_t,
};

/// Sub-phase 1: build tagged source vectors (sparse or dense-int8) in parallel.
fn finalizeBuildSources(p: *FinalizeParams) void {
    var sc: SrcBuildCtx = .{
        .entries = p.corpus.entries,
        .src_entries = p.src_entries,
        .entry_count = p.corpus.entry_count,
        .next_idx = .init(0),
    };
    c.cbm_parallel_for(p.worker_count, srcBuildWorker, &sc, p.opts);
}

/// Sub-phases 2+3: co-occurrence pass 1 + normalize.
fn finalizePass1(p: *FinalizeParams) void {
    var cc: CooccurSparseCtx = .{
        .entries = p.corpus.entries,
        .src_entries = p.src_entries,
        .doc_token_ids = p.corpus.doc_token_ids,
        .doc_token_counts = p.corpus.doc_token_counts,
        .rev = p.rev,
        .doc_count = p.corpus.doc_count,
        .entry_count = p.corpus.entry_count,
        .next_chunk = .init(0),
        .num_chunks = p.num_chunks,
        .chunk_size = p.chunk_size,
        .tile_size = p.tile_size,
    };
    c.cbm_parallel_for(p.worker_count, cooccurWorkerSparse, &cc, p.opts);

    var nc: NormCtx = .{
        .entries = p.corpus.entries,
        .entry_count = p.corpus.entry_count,
        .next_idx = .init(0),
    };
    c.cbm_parallel_for(p.worker_count, normalizeWorker, &nc, p.opts);
}

/// Sub-phases 4+5: quantize pass1 to int8, run RRI pass 2, blend + normalize.
fn finalizePass2(p: *FinalizeParams) void {
    const entry_count = ix(p.corpus.entry_count);
    const pass1_q: ?[*]i8 = interop.mallocN(i8, entry_count * DIM);
    if (pass1_q) |q| {
        var qc: Pass1QuantCtx = .{
            .entries = p.corpus.entries,
            .pass1_q = q,
            .entry_count = p.corpus.entry_count,
            .next_idx = .init(0),
        };
        c.cbm_parallel_for(p.worker_count, pass1QuantizeWorker, &qc, p.opts);
    }

    const pass1: ?[*]Vec = interop.mallocN(Vec, entry_count);
    if (pass1) |p1| {
        for (0..entry_count) |i| {
            p1[i] = p.corpus.entries[i].enriched_vec;
        }
    }

    if (pass1_q) |q| {
        var cc: CooccurInt8Ctx = .{
            .entries = p.corpus.entries,
            .pass1_q = q,
            .doc_token_ids = p.corpus.doc_token_ids,
            .doc_token_counts = p.corpus.doc_token_counts,
            .rev = p.rev,
            .doc_count = p.corpus.doc_count,
            .entry_count = p.corpus.entry_count,
            .next_chunk = .init(0),
            .num_chunks = p.num_chunks,
            .chunk_size = p.chunk_size,
            .tile_size = p.tile_size,
        };
        c.cbm_parallel_for(p.worker_count, cooccurWorkerInt8, &cc, p.opts);
    }

    if (pass1) |p1| {
        var bc: BlendCtx = .{
            .entries = p.corpus.entries,
            .pass1 = p1,
            .entry_count = p.corpus.entry_count,
            .next_idx = .init(0),
        };
        c.cbm_parallel_for(p.worker_count, blendWorker, &bc, p.opts);
        c.free(@ptrCast(p1));
    }
    c.free(@ptrCast(pass1_q));

    var nc: NormCtx = .{
        .entries = p.corpus.entries,
        .entry_count = p.corpus.entry_count,
        .next_idx = .init(0),
    };
    c.cbm_parallel_for(p.worker_count, normalizeWorker, &nc, p.opts);
}

pub export fn cbm_sem_corpus_finalize(corpus_c: ?*Corpus) void {
    const corpus = corpus_c orelse return;
    if (corpus.finalized) return;

    // Eager init before parallel dispatch to avoid lazy-init races
    ensurePretrainedMap();

    const worker_count = c.cbm_default_worker_count(false);
    const opts: c.cbm_parallel_for_opts_t = .{ .max_workers = worker_count, .force_pthreads = false };

    // Finer chunks = better load balancing for skewed token distributions.
    var num_chunks = worker_count * COOCCUR_CHUNK;
    if (num_chunks > corpus.entry_count) num_chunks = corpus.entry_count;
    if (num_chunks < 1) num_chunks = 1;
    const chunk_size = @divTrunc(corpus.entry_count + num_chunks - 1, num_chunks);

    const rev = buildReverseIndex(corpus) orelse {
        corpus.finalized = true;
        return;
    };
    const src_entries = interop.callocN(SrcEntry, ix(corpus.entry_count));
    if (src_entries == null) {
        freeReverseIndex(rev);
        corpus.finalized = true;
        return;
    }

    var params: FinalizeParams = .{
        .corpus = corpus,
        .rev = rev,
        .src_entries = src_entries,
        .worker_count = worker_count,
        .num_chunks = num_chunks,
        .chunk_size = chunk_size,
        .tile_size = TILE_SIZE,
        .opts = opts,
    };
    finalizeBuildSources(&params);
    finalizePass1(&params);
    finalizePass2(&params);

    c.free(@ptrCast(src_entries));
    freeReverseIndex(rev);
    corpus.finalized = true;
}

/// log(doc_count / doc_freq) through libm, as the C code computed it.
fn idfOf(doc_count: c_int, df: c_int) f32 {
    return c.logf(@as(f32, @floatFromInt(doc_count)) / @as(f32, @floatFromInt(df)));
}

pub export fn cbm_sem_corpus_idf(corpus_c: ?*const Corpus, token: ?Str) f32 {
    const corpus = corpus_c orelse return 0.0;
    if (token == null or corpus.doc_count == 0) return 0.0;
    const idx = parseTokenIndex(@ptrCast(c.cbm_ht_get(corpus.token_map, token)));
    if (idx < 0 or idx >= corpus.entry_count) return 0.0;
    const df = corpus.entries[ix(idx)].doc_freq;
    if (df <= 0) return 0.0;
    return idfOf(corpus.doc_count, df);
}

pub export fn cbm_sem_corpus_ri_vec(corpus_c: ?*const Corpus, token: ?Str) ?*const Vec {
    const corpus = corpus_c orelse return null;
    if (token == null) return null;
    const idx = parseTokenIndex(@ptrCast(c.cbm_ht_get(corpus.token_map, token)));
    if (idx < 0 or idx >= corpus.entry_count) return null;
    return &at(corpus.entries, ix(idx)).enriched_vec;
}

pub export fn cbm_sem_corpus_doc_count(corpus: ?*const Corpus) c_int {
    return if (corpus) |cp| cp.doc_count else 0;
}

pub export fn cbm_sem_corpus_token_count(corpus: ?*const Corpus) c_int {
    return if (corpus) |cp| cp.entry_count else 0;
}

pub export fn cbm_sem_corpus_token_at(
    corpus_c: ?*const Corpus,
    index: c_int,
    out_vec: ?*?*const Vec,
    out_idf: ?*f32,
) CStr {
    const corpus = corpus_c orelse return null;
    if (index < 0 or index >= corpus.entry_count) return null;
    const entry = at(corpus.entries, ix(index));
    if (out_vec) |ov| {
        ov.* = &entry.enriched_vec;
    }
    if (out_idf) |oi| {
        if (corpus.doc_count > 0) {
            const df = entry.doc_freq;
            oi.* = if (df > 0) idfOf(corpus.doc_count, df) else 0.0;
        }
    }
    return entry.token;
}

fn freeHtKv(key: CStr, value: ?*anyopaque, userdata: ?*anyopaque) callconv(.c) void {
    _ = userdata;
    c.free(@ptrCast(@constCast(key)));
    c.free(value);
}

pub export fn cbm_sem_corpus_free(corpus_c: ?*Corpus) void {
    const corpus = corpus_c orelse return;
    for (0..ix(corpus.entry_count)) |i| {
        c.free(corpus.entries[i].token);
    }
    c.free(@ptrCast(corpus.entries));
    for (0..ix(corpus.doc_count)) |d| {
        c.free(@ptrCast(corpus.doc_token_ids[d]));
    }
    c.free(@ptrCast(corpus.doc_token_ids));
    c.free(@ptrCast(corpus.doc_token_counts));
    if (corpus.token_map) |map| {
        c.cbm_ht_foreach(map, freeHtKv, null);
        c.cbm_ht_free(map);
    }
    c.free(corpus);
}

// ── Combined scoring ────────────────────────────────────────────────

pub export fn cbm_sem_proximity(path_a: ?Str, path_b: ?Str) f32 {
    const pa = path_a orelse return UNIT_POS;
    const pb = path_b orelse return UNIT_POS;
    // Count shared directory components
    var shared: c_int = 0;
    var total_a: c_int = 0;
    var total_b: c_int = 0;
    var i: usize = 0;
    while (pa[i] != 0 and pb[i] != 0 and pa[i] == pb[i]) : (i += 1) {
        if (pa[i] == '/') shared += 1;
    }
    for (std.mem.span(pa)) |ch| {
        if (ch == '/') total_a += 1;
    }
    for (std.mem.span(pb)) |ch| {
        if (ch == '/') total_b += 1;
    }
    const max_total = @max(total_a, total_b);
    if (max_total == 0) return UNIT_POS;
    const ratio = @as(f32, @floatFromInt(shared)) / @as(f32, @floatFromInt(max_total));
    // Same file = 1.10, same dir = 1.05, distant = 1.00
    return fp.mulAdd(f32, ratio, PROX_MAX_BOOST, UNIT_POS);
}

/// Cosine similarity for small float arrays (AST profile).
fn smallCosine(a: []const f32, b: []const f32) f32 {
    var dot: f32 = 0.0;
    var ma: f32 = 0.0;
    var mb: f32 = 0.0;
    for (a, b) |x, y| {
        dot = fp.mulAdd(f32, x, y, dot);
        ma = fp.mulAdd(f32, x, x, ma);
        mb = fp.mulAdd(f32, y, y, mb);
    }
    const denom = @sqrt(ma) * @sqrt(mb);
    return if (denom < DENOM_EPS) 0.0 else dot / denom;
}

/// Sparse cosine over two pre-sorted (index, weight) vectors. Returns 0 when
/// either side is empty or the magnitude product is below the epsilon guard.
fn sparseTfidfCosine(a: *const Func, b: *const Func) f32 {
    if (a.tfidf_len <= 0 or b.tfidf_len <= 0) return 0.0;
    const a_len = ix(a.tfidf_len);
    const b_len = ix(b.tfidf_len);
    var dot: f32 = 0.0;
    var ma: f32 = 0.0;
    var mb: f32 = 0.0;
    var ia: usize = 0;
    var ib: usize = 0;
    while (ia < a_len and ib < b_len) {
        if (a.tfidf_indices[ia] == b.tfidf_indices[ib]) {
            dot = fp.mulAdd(f32, a.tfidf_weights[ia], b.tfidf_weights[ib], dot);
            ia += 1;
            ib += 1;
        } else if (a.tfidf_indices[ia] < b.tfidf_indices[ib]) {
            ia += 1;
        } else {
            ib += 1;
        }
    }
    for (a.tfidf_weights[0..a_len]) |w| {
        ma = fp.mulAdd(f32, w, w, ma);
    }
    for (b.tfidf_weights[0..b_len]) |w| {
        mb = fp.mulAdd(f32, w, w, mb);
    }
    const denom = @sqrt(ma) * @sqrt(mb);
    return if (denom > DENOM_EPS) dot / denom else 0.0;
}

pub export fn cbm_sem_combined_score(
    a_c: ?*const Func,
    b_c: ?*const Func,
    cfg_c: ?*const c.cbm_sem_config_t,
) f32 {
    const a = a_c orelse return 0.0;
    const b = b_c orelse return 0.0;
    const cfg = cfg_c orelse return 0.0;
    const a_minhash: *const c.cbm_minhash_t = @ptrCast(&a.minhash);
    const b_minhash: *const c.cbm_minhash_t = @ptrCast(&b.minhash);

    // Short-circuit: if MinHash Jaccard is already above the SIMILAR_TO threshold,
    // the pass_similarity pipeline already emitted a SIMILAR_TO edge for this pair.
    // Returning 0 here avoids flooding top-k with cross-service copy-paste boilerplate
    // (logging_middleware, shared push/pull handlers) that SIMILAR_TO already covers,
    // freeing the edge budget for true semantic leaps (vocabulary-bridged relations).
    if (a.has_minhash and b.has_minhash) {
        const early_j = c.cbm_minhash_jaccard(a_minhash, b_minhash);
        if (early_j >= c.CBM_MINHASH_JACCARD_THRESHOLD) return 0.0;
    }

    var score = cfg.w_tfidf * sparseTfidfCosine(a, b);

    // Signal 2: Random Indexing
    score = fp.mulAdd(f32, cfg.w_ri, c.cbm_rsq_ip(&a.ri_code, &b.ri_code), score);

    // Signal 3: MinHash Jaccard
    if (a.has_minhash and b.has_minhash) {
        const j = c.cbm_minhash_jaccard(a_minhash, b_minhash);
        score = fp.mulAdd(f32, cfg.w_minhash, @floatCast(j), score);
    }

    // Signal 4: API Signatures
    score = fp.mulAdd(f32, cfg.w_api, c.cbm_rsq_ip(&a.api_code, &b.api_code), score);

    // Signal 5: Type Signatures
    score = fp.mulAdd(f32, cfg.w_type, c.cbm_rsq_ip(&a.type_code, &b.type_code), score);

    // Signal 7: Decorator Pattern
    score = fp.mulAdd(f32, cfg.w_decorator, c.cbm_rsq_ip(&a.deco_code, &b.deco_code), score);

    // Signal 8+9+11: Structural profile + data flow + Halstead
    const sp_score = smallCosine(&a.struct_profile, &b.struct_profile);
    score = fp.mulAdd(f32, cfg.w_struct_profile, sp_score, score);

    // Signal 6: Module proximity (multiplier, not additive).
    // Proximity returns [1.0, 1.10] — a same-file/same-dir boost for ranking.
    // Clamp the final product to [0, 1] so the output stays within valid
    // cosine-similarity range. Without the clamp, a 0.95 base × 1.10 proximity
    // would emit a 1.045 score which violates the semantic of "similarity" and
    // breaks downstream consumers that expect a normalized value.
    score *= cbm_sem_proximity(a.file_path, b.file_path);
    if (score > UNIT_POS) score = UNIT_POS;
    if (score < 0.0) score = 0.0;

    return score;
}

// ── Graph diffusion ─────────────────────────────────────────────────

pub export fn cbm_sem_diffuse(
    combined_c: ?*Vec,
    neighbors_c: ?[*]const Vec,
    neighbor_count: c_int,
    alpha: f32,
) void {
    const combined = combined_c orelse return;
    const neighbors = neighbors_c orelse return;
    if (neighbor_count <= 0) return;
    // Blend: combined = (1-α) × combined + α × mean(neighbors)
    var mean: Vec = undefined;
    @memset(&mean.v, 0.0);
    for (neighbors[0..ix(neighbor_count)]) |*neighbor| {
        for (&mean.v, neighbor.v) |*m, x| {
            m.* += x;
        }
    }
    const inv_n = UNIT_POS / @as(f32, @floatFromInt(neighbor_count));
    const one_minus_alpha = UNIT_POS - alpha;
    for (&combined.v, mean.v) |*x, m| {
        x.* = fp.mulAdd(f32, one_minus_alpha, x.*, alpha * m * inv_n);
    }
    cbm_sem_normalize(combined);
}
