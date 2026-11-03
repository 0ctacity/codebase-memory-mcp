//! minhash.zig — MinHash fingerprinting + LSH for near-clone detection.
//!
//! Implements src/simhash/minhash.h. Computes K=64 MinHash signatures from
//! normalised AST node-type trigrams. Uses XXH3 with distinct seeds for each
//! permutation; std.hash.XxHash3 produces the same values as the vendored
//! xxHash, so fingerprints stored by earlier builds stay comparable.
//! Pure functions — thread-safe, no shared state.
//!
//! LSH index uses b=32 bands × r=2 rows for candidate generation.

const std = @import("std");
const c = @import("c");

const XxHash3 = std.hash.XxHash3;

const K = c.CBM_MINHASH_K;
const MIN_NODES = c.CBM_MINHASH_MIN_NODES;
const BANDS = c.CBM_LSH_BANDS;
const ROWS = c.CBM_LSH_ROWS;

/// Maximum trigram string length: 3 node types × max 40 chars each + separators.
const TRIGRAM_BUF_LEN = 160;
/// Maximum AST body nodes to walk (stack depth).
const AST_WALK_CAP = 2048;
/// Minimum unique structural trigrams for a meaningful fingerprint.
/// K/2 = 32 ensures most MinHash slots get populated from distinct features.
const MIN_UNIQUE_TRIGRAMS = 32;
/// Maximum structural weight per trigram (3 tokens × 1 each).
const MAX_STRUCTURAL_WEIGHT = 3;
/// Dynamic array growth.
const BUCKET_INIT_CAP = 8;
const ENTRY_INIT_CAP = 64;
const RESULT_INIT_CAP = 64;
/// Skip oversized buckets (noise from trivially similar functions).
const MAX_BUCKET_SIZE = 200;
/// Seen-set for O(1) dedup during query (open addressing on node ids).
const SEEN_SET_SIZE = 16384;
const SEEN_SET_MASK = SEEN_SET_SIZE - 1;
/// Knuth multiplicative hash constant for node_id → seen-set slot.
const KNUTH_MULT: u64 = 2654435761;
/// Maximum normalised tokens per function body.
const MAX_TOKENS = 4096;
/// Unique-trigram set: open addressing on 64-bit hashes.
const UNIQ_SET_SIZE = 4096;
const UNIQ_SET_MASK = UNIQ_SET_SIZE - 1;
/// Band hash table: 65536 buckets per band (16-bit hash of r=2 values).
const LSH_BUCKET_COUNT = 65536;
const LSH_BUCKET_MASK = LSH_BUCKET_COUNT - 1;

const Token = [*:0]const u8;

// ── AST node type normalisation ─────────────────────────────────────

fn isOneOf(kind: []const u8, comptime names: []const []const u8) bool {
    inline for (names) |name| {
        if (std.mem.eql(u8, kind, name)) return true;
    }
    return false;
}

/// Normalise a node type string. Returns a short canonical string or the
/// original kind if no normalisation applies.
fn normaliseNodeType(kind: Token) Token {
    const k = std.mem.span(kind);
    if (isOneOf(k, &.{
        "identifier",      "field_identifier",              "property_identifier",
        "type_identifier", "shorthand_property_identifier", "shorthand_field_identifier",
        "variable_name",   "name",
    })) return "I";
    if (isOneOf(k, &.{
        "string",          "string_literal", "interpreted_string_literal", "raw_string_literal",
        "template_string", "string_content", "escape_sequence",
    })) return "S";
    if (isOneOf(k, &.{
        "number",        "integer",     "float",          "integer_literal",
        "float_literal", "int_literal", "number_literal",
    })) return "N";
    if (isOneOf(k, &.{
        "type_identifier", "predefined_type", "primitive_type",
        "builtin_type",    "type_annotation", "simple_type",
    })) return "T";
    return kind;
}

// ── MinHash computation ─────────────────────────────────────────────

/// A normalised token is one of the generic types (I/S/N/T). These carry
/// no structural info — used to compute trigram weight.
fn isNormalisedToken(tok: Token) bool {
    return tok[0] != 0 and tok[1] == 0 and
        (tok[0] == 'I' or tok[0] == 'S' or tok[0] == 'N' or tok[0] == 'T');
}

/// Structural weight of a trigram: count of non-normalised tokens (0-3).
/// Weight 0 = pure data manipulation (noise), weight 3 = rich control flow.
fn trigramStructuralWeight(a: Token, b: Token, t: Token) u32 {
    var w: u32 = 0;
    if (!isNormalisedToken(a)) w += 1;
    if (!isNormalisedToken(b)) w += 1;
    if (!isNormalisedToken(t)) w += 1;
    return w;
}

/// Phase 1: walk the AST iteratively and collect normalised LEAF token types.
/// Leaf-only counting is language-agnostic: leaf nodes correspond to actual
/// source tokens, not grammar-internal structure that varies across parsers.
fn collectAstTokens(root: c.TSNode, tokens: []Token) usize {
    var token_count: usize = 0;
    var stack: [AST_WALK_CAP]c.TSNode = undefined;
    var top: usize = 0;
    stack[top] = root;
    top += 1;

    while (top > 0 and token_count < tokens.len) {
        top -= 1;
        const node = stack[top];
        const child_count = c.ts_node_child_count(node);
        if (child_count == 0) {
            // Leaf node — actual source token. Normalise and record.
            const kind: Token = @ptrCast(c.ts_node_type(node));
            if (kind[0] != 0) {
                tokens[token_count] = normaliseNodeType(kind);
                token_count += 1;
            }
        } else {
            // Internal node — push children only (skip the node itself).
            // Structural info comes from leaf token patterns, not grammar nodes.
            var i: u32 = child_count;
            while (i > 0 and top < AST_WALK_CAP) {
                i -= 1;
                stack[top] = c.ts_node_child(node, i);
                top += 1;
            }
        }
    }
    return token_count;
}

const UniqTrigSet = struct {
    slots: [UNIQ_SET_SIZE]u64 = @splat(0),
    count: u32 = 0,

    /// Insert a trigram hash. Returns true if newly inserted.
    fn insert(self: *UniqTrigSet, trig_hash: u64) bool {
        const val = trig_hash | 1; // ensure non-zero
        const slot: u32 = @truncate(trig_hash & UNIQ_SET_MASK);
        var probe: u32 = 0;
        while (probe < UNIQ_SET_SIZE) : (probe += 1) {
            const idx = (slot +% probe) & UNIQ_SET_MASK;
            if (self.slots[idx] == 0) {
                self.slots[idx] = val;
                self.count += 1;
                return true;
            }
            if (self.slots[idx] == val) return false;
        }
        return false;
    }
};

/// Weighted MinHash for one trigram: hash w times per seed.
fn weightedMinhashUpdate(out: *c.cbm_minhash_t, trigram: []const u8, w: u32) void {
    for (0..K) |k| {
        for (0..w) |rep| {
            const seed: u64 = @as(u64, k) * MAX_STRUCTURAL_WEIGHT + rep;
            const h32: u32 = @truncate(XxHash3.hash(seed, trigram));
            if (h32 < out.values[k]) out.values[k] = h32;
        }
    }
}

/// Phase 2: hash trigrams into the MinHash signature with structural weighting.
///
/// - Skip weight-0 trigrams (all tokens are I/S/N/T — pure noise)
/// - Repetition-based weighted MinHash: hash w times per seed for weight w
/// - Track unique trigrams via hash set; reject if < MIN_UNIQUE_TRIGRAMS
///
/// Returns the number of unique structural trigrams processed.
fn hashTrigrams(tokens: []const Token, out: *c.cbm_minhash_t) u32 {
    @memset(&out.values, std.math.maxInt(u32));

    var uniq: UniqTrigSet = .{};
    var trigram_buf: [TRIGRAM_BUF_LEN]u8 = undefined;

    var i: usize = 0;
    while (i + 2 < tokens.len) : (i += 1) {
        const w = trigramStructuralWeight(tokens[i], tokens[i + 1], tokens[i + 2]);
        if (w == 0) continue;

        const trigram = std.fmt.bufPrint(&trigram_buf, "{s}|{s}|{s}", .{
            std.mem.span(tokens[i]), std.mem.span(tokens[i + 1]), std.mem.span(tokens[i + 2]),
        }) catch continue;
        // The C implementation formatted into a NUL-terminated buffer of this
        // size and skipped trigrams that would not fit with their NUL.
        if (trigram.len >= TRIGRAM_BUF_LEN) continue;

        _ = uniq.insert(XxHash3.hash(0, trigram));
        weightedMinhashUpdate(out, trigram, w);
    }
    return uniq.count;
}

pub export fn cbm_minhash_compute(
    func_body: c.TSNode,
    source: [*c]const u8,
    language: c_int,
    out: *c.cbm_minhash_t,
) bool {
    _ = source;
    _ = language;
    if (c.ts_node_is_null(func_body)) return false;

    var tokens: [MAX_TOKENS]Token = undefined;
    const token_count = collectAstTokens(func_body, &tokens);
    if (token_count < MIN_NODES) return false;

    return hashTrigrams(tokens[0..token_count], out) >= MIN_UNIQUE_TRIGRAMS;
}

// ── Jaccard similarity ──────────────────────────────────────────────

pub export fn cbm_minhash_jaccard(a_c: ?*const c.cbm_minhash_t, b_c: ?*const c.cbm_minhash_t) f64 {
    const a = a_c orelse return 0.0;
    const b = b_c orelse return 0.0;
    var matching: u32 = 0;
    for (a.values, b.values) |x, y| {
        if (x == y) matching += 1;
    }
    return @as(f64, @floatFromInt(matching)) / @as(f64, K);
}

// ── Hex encoding/decoding ───────────────────────────────────────────

pub export fn cbm_minhash_to_hex(fp_c: ?*const c.cbm_minhash_t, buf_c: ?[*]u8, bufsize: c_int) void {
    const buf = buf_c orelse return;
    const fp = fp_c orelse {
        if (bufsize > 0) buf[0] = 0;
        return;
    };
    if (bufsize < c.CBM_MINHASH_HEX_BUF) {
        if (bufsize > 0) buf[0] = 0;
        return;
    }
    for (fp.values, 0..) |v, i| {
        _ = std.fmt.bufPrint(buf[i * 8 ..][0..8], "{x:0>8}", .{v}) catch unreachable;
    }
    buf[K * 8] = 0;
}

pub export fn cbm_minhash_from_hex(hex_c: ?[*:0]const u8, out_c: ?*c.cbm_minhash_t) bool {
    const hex = hex_c orelse return false;
    const out = out_c orelse return false;
    if (std.mem.len(hex) != c.CBM_MINHASH_HEX_LEN) return false;
    for (&out.values, 0..) |*v, i| {
        var chunk: [9]u8 = undefined;
        @memcpy(chunk[0..8], hex[i * 8 ..][0..8]);
        chunk[8] = 0;
        // strtoul keeps the C decoder's leniency for malformed chunks.
        v.* = @truncate(c.strtoul(&chunk, null, 16));
    }
    return true;
}

// ── LSH index ───────────────────────────────────────────────────────

/// Each band bucket is a dynamic array of entry indices (not pointers —
/// the entries array may be reallocated during insert, invalidating ptrs).
const Bucket = extern struct {
    items: ?[*]c_int,
    count: c_int,
    cap: c_int,
};

const LshIndex = extern struct {
    /// b=32 bands, each with LSH_BUCKET_COUNT buckets.
    bands: [BANDS][LSH_BUCKET_COUNT]Bucket,
    /// All entries stored for lifetime management.
    entries: ?[*]c.cbm_lsh_entry_t,
    entry_count: c_int,
    entry_cap: c_int,
    /// Candidate result buffer (reused across queries).
    result_buf: ?[*]?*const c.cbm_lsh_entry_t,
    result_count: c_int,
    result_cap: c_int,
};

/// Band hash for band `band`: r=2 consecutive values starting at band*r.
fn bandHash(fp: *const c.cbm_minhash_t, band: usize) usize {
    const base = band * ROWS;
    const combined: [ROWS]u32 = fp.values[base..][0..ROWS].*;
    return @intCast(XxHash3.hash(0, std.mem.asBytes(&combined)) & LSH_BUCKET_MASK);
}

/// Grows a C-allocated array to hold at least one more element.
fn grow(comptime T: type, items: *?[*]T, cap: *c_int, init_cap: c_int) bool {
    const new_cap: c_int = if (cap.* < init_cap) init_cap else cap.* * 2;
    const raw = c.realloc(@ptrCast(items.*), @as(usize, @intCast(new_cap)) * @sizeOf(T)) orelse return false;
    items.* = @ptrCast(@alignCast(raw));
    cap.* = new_cap;
    return true;
}

fn bucketPush(bucket: *Bucket, entry_index: c_int) void {
    if (bucket.count >= bucket.cap and !grow(c_int, &bucket.items, &bucket.cap, BUCKET_INIT_CAP)) return;
    bucket.items.?[@intCast(bucket.count)] = entry_index;
    bucket.count += 1;
}

pub export fn cbm_lsh_new() ?*LshIndex {
    // calloc keeps the 32 MB band table lazily zeroed by the OS.
    return @ptrCast(@alignCast(c.calloc(1, @sizeOf(LshIndex))));
}

pub export fn cbm_lsh_insert(idx_c: ?*LshIndex, entry_c: ?*const c.cbm_lsh_entry_t) void {
    const idx = idx_c orelse return;
    const entry = entry_c orelse return;
    const fp = entry.fingerprint orelse return;

    // Store a copy of the entry.
    if (idx.entry_count >= idx.entry_cap and
        !grow(c.cbm_lsh_entry_t, &idx.entries, &idx.entry_cap, ENTRY_INIT_CAP)) return;
    const entry_idx = idx.entry_count;
    idx.entries.?[@intCast(entry_idx)] = entry.*;
    idx.entry_count += 1;

    // Insert the index into each band's bucket.
    for (0..BANDS) |b| {
        bucketPush(&idx.bands[b][bandHash(fp, b)], entry_idx);
    }
}

/// O(1) seen-set: open-addressing hash table on node_id for dedup.
/// 0 means empty — node ids are always > 0.
const SeenSet = struct {
    slots: ?[*]i64,

    fn init() SeenSet {
        return .{ .slots = @ptrCast(@alignCast(c.calloc(SEEN_SET_SIZE, @sizeOf(i64)))) };
    }

    fn deinit(self: *SeenSet) void {
        c.free(self.slots);
        self.slots = null;
    }

    /// Returns true when node_id was not present and is now inserted.
    fn insert(self: *SeenSet, node_id: i64) bool {
        const slots = self.slots orelse return false;
        const idx: u32 = @as(u32, @truncate(@as(u64, @bitCast(node_id)) *% KNUTH_MULT)) & SEEN_SET_MASK;
        var probe: u32 = 0;
        while (probe < SEEN_SET_SIZE) : (probe += 1) {
            const slot = (idx +% probe) & SEEN_SET_MASK;
            if (slots[slot] == 0) {
                slots[slot] = node_id;
                return true;
            }
            if (slots[slot] == node_id) return false;
        }
        return false; // table full
    }
};

fn resultPush(idx: *LshIndex, candidate: *const c.cbm_lsh_entry_t) bool {
    if (idx.result_count >= idx.result_cap and
        !grow(?*const c.cbm_lsh_entry_t, &idx.result_buf, &idx.result_cap, RESULT_INIT_CAP)) return false;
    idx.result_buf.?[@intCast(idx.result_count)] = candidate;
    idx.result_count += 1;
    return true;
}

/// Calls `visit` for each distinct candidate in the fingerprint's band
/// buckets, in band order; stops when `visit` returns false.
fn forEachCandidate(
    idx: *const LshIndex,
    fp: *const c.cbm_minhash_t,
    ctx: anytype,
    comptime visit: fn (@TypeOf(ctx), *const c.cbm_lsh_entry_t) bool,
) void {
    var seen = SeenSet.init();
    defer seen.deinit();
    for (0..BANDS) |b| {
        const bucket = &idx.bands[b][bandHash(fp, b)];
        // Skip oversized buckets — noise from trivially similar utility functions.
        if (bucket.count > MAX_BUCKET_SIZE) continue;
        for (0..@intCast(bucket.count)) |i| {
            const candidate = &idx.entries.?[@intCast(bucket.items.?[i])];
            if (!seen.insert(candidate.node_id)) continue;
            if (!visit(ctx, candidate)) return;
        }
    }
}

pub export fn cbm_lsh_query(
    idx_c: ?*const LshIndex,
    fp_c: ?*const c.cbm_minhash_t,
    out: *?[*]?*const c.cbm_lsh_entry_t,
    count: *c_int,
) void {
    out.* = null;
    count.* = 0;
    const idx: *LshIndex = @constCast(idx_c orelse return);
    const fp = fp_c orelse return;

    idx.result_count = 0;
    forEachCandidate(idx, fp, idx, resultPush);
    out.* = idx.result_buf;
    count.* = idx.result_count;
}

const IntoBuf = struct {
    buf: [*]?*const c.cbm_lsh_entry_t,
    cap: usize,
    count: usize = 0,

    fn push(self: *IntoBuf, candidate: *const c.cbm_lsh_entry_t) bool {
        self.buf[self.count] = candidate;
        self.count += 1;
        return self.count < self.cap;
    }
};

pub export fn cbm_lsh_query_into(
    idx_c: ?*const LshIndex,
    fp_c: ?*const c.cbm_minhash_t,
    out_buf: ?[*]?*const c.cbm_lsh_entry_t,
    out_cap: c_int,
) c_int {
    const idx = idx_c orelse return 0;
    const fp = fp_c orelse return 0;
    const buf = out_buf orelse return 0;
    if (out_cap <= 0) return 0;

    // Local dedup state — no shared state touched, so this is thread-safe.
    var into = IntoBuf{ .buf = buf, .cap = @intCast(out_cap) };
    forEachCandidate(idx, fp, &into, IntoBuf.push);
    return @intCast(into.count);
}

pub export fn cbm_lsh_free(idx_c: ?*LshIndex) void {
    const idx = idx_c orelse return;
    for (&idx.bands) |*band| {
        for (band) |bucket| c.free(bucket.items);
    }
    c.free(idx.entries);
    c.free(@ptrCast(idx.result_buf));
    c.free(idx);
}
