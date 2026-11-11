//! lsp_resolve.zig — Shared LSP-override resolver for the call-edge pipeline.
//!
//! The call resolver (pass_parallel.zig resolveFileCalls) and the tests look
//! up an LSP-resolved call for a given (caller, callee) pair here before
//! falling back to the registry's name-based resolver, so every caller admits
//! the same set of LSP overrides. Edge emission stays with the caller; this
//! module only does the matching.
//!
//! lsp_resolve.h declares the three functions the C tests call and the
//! CBM_LSP_CONFIDENCE_FLOOR constant; everything else is Zig-only.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const ix = util.ix;
const streq = util.streq;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Row = c.CBMResolvedCall;
const Rows = c.CBMResolvedCallArray;
const Language = c.CBMLanguage;

/// Confidence floor below which LSP-resolved calls are ignored and the
/// registry resolver is consulted instead (CBM_LSP_CONFIDENCE_FLOOR).
const confidence_floor: f32 = c.CBM_LSP_CONFIDENCE_FLOOR;

/// Tail-match cost counters (upstream f6e3af43). The tail scan in
/// cbm_pipeline_lsp_target_node has no candidate cap and its candidate set
/// grows with the corpus, so candidates per lookup is what would turn resolve
/// superlinear. Counted always (two relaxed adds), reset and logged by
/// pass_parallel.zig as parallel.resolve.scan_cost.
pub var tail_lookups: std.atomic.Value(u64) = .init(0);
pub var tail_candidates: std.atomic.Value(u64) = .init(0);

// ── Name helpers ────────────────────────────────────────────────────

/// Bare last segment of a (possibly qualified) name, splitting on the LAST
/// member/scope separator. C++ textual callees carry `::` (Class::method,
/// Ns::f) and `->` (p->run), while the LSP records dotted internal QNs
/// (Class.method). Splitting only on '.' (strrchr) leaves `Math::square`
/// and `p->run` intact, so they never match the LSP's `square`/`run` short
/// name and the type-aware strategy is silently dropped to the textual
/// registry. Treat '.', ':' and '>' as terminal separators so the bare
/// method name is recovered on BOTH the QN side (dotted, occasionally `::`
/// for template/alias scopes) and the textual side (`.`/`::`/`->`). Other
/// languages' callee names contain none of `::`/`->`, so this is a no-op
/// for them.
pub fn bareSegment(name: CStr) CStr {
    if (name == null) return name;
    var seg = name;
    var p = name;
    while (p[0] != 0) : (p += 1) {
        // '.' (dotted QN / Java-style member) and ':' (C++ `::`, last colon
        // wins) are member/scope separators. '>' is only a separator when it
        // closes the `->` arrow (preceded by '-'); a bare '>' closes a template
        // argument list ("identity<int>") and must NOT split, else the segment
        // would be the empty string after the trailing '>'.
        if (p[0] == '.' or p[0] == ':' or (p[0] == '>' and p != name and (p - 1)[0] == '-')) {
            seg = p + 1;
        }
    }
    return seg;
}

/// The text after the last '.', or the whole name when it has none.
fn lastDotSegment(qn: CStr) CStr {
    const dot: CStr = c.strrchr(qn, '.');
    return if (dot != null) dot + 1 else qn;
}

/// The start of the final two dot-separated segments ("Class.method"), or
/// null when the QN is too short.
fn qnClassMethodTail(qn: CStr) CStr {
    if (qn == null) return null;
    const last: CStr = c.strrchr(qn, '.');
    if (last == null or last == qn) return null;
    var second = last;
    while (second != qn) {
        second -= 1;
        if (second[0] == '.') return if (second == qn) qn else second + 1;
    }
    return qn;
}

fn qnClassMethodTailEq(qn: CStr, tail: CStr) bool {
    const qt = qnClassMethodTail(qn);
    return qt != null and tail != null and streq(qt, tail);
}

// ── CALLS join ──────────────────────────────────────────────────────

/// Gate for the unique-`Class.method`-tail fallbacks below. Tail-matching by
/// leaf is safe where class-per-file package semantics hold — the JVM
/// languages (Java/Kotlin): the declared `package` is ground truth, a class
/// name is unique within a package, and mixed Gradle/Maven source roots
/// (`src/main/java` + `src/main/kotlin`) legitimately produce path-derived
/// module QNs that disagree with the package-shaped QNs the LSP emits, so
/// the tail is the only reliable join key. In other languages the same-name
/// guarantee does not exist (Python/TS re-export shims, Go internal clones,
/// C++ template instantiations), and a single wrong-module coincidence
/// would fabricate a CALLS edge — so the fallbacks stay off there.
pub export fn cbm_pipeline_lsp_allow_tail_match(lang: Language) bool {
    return lang == c.CBM_LANG_JAVA or lang == c.CBM_LANG_KOTLIN;
}

/// An invocation row that clears the confidence floor and names both ends.
fn isInvocationRow(rc: *const Row) bool {
    return rc.caller_qn != null and rc.callee_qn != null and
        rc.kind == c.CBM_RESOLVED_INVOCATION and rc.confidence >= confidence_floor;
}

/// An invocation row that carries a site span (end > start) proves ONE call: it
/// joins only the call whose node starts at that byte, so `db.deinit()` and
/// `st.deinit()` in one function each keep their own target. Rows without a span
/// join every same-named call in the caller, as before.
fn siteAdmits(rc: *const Row, call: *const c.CBMCall) bool {
    return rc.site_end_byte <= rc.site_start_byte or rc.site_start_byte == call.site_start_byte;
}

/// Indirect/implicit resolution: the textual callee differs from the
/// resolved callee_qn's short name. A function-pointer / DLL call's callee
/// is the pointer name (`fp`); a C++ destructor's only textual anchor is the
/// deleted operand (`p`, vs. the `T.~T` callee QN). In both the LSP stashed
/// the original textual name in `reason`. Match the call site on that name,
/// gated to those strategies so `reason` is never misread as an
/// unresolved-call diagnostic.
fn reasonNamesCall(rc: *const Row, call_short: CStr) bool {
    if (rc.reason == null or rc.strategy == null) return false;
    const s = rc.strategy;
    const indirect = streq(s, "lsp_func_ptr") or streq(s, "lsp_dll_resolve") or
        streq(s, "lsp_dict_dispatch") or streq(s, "lsp_destructor") or
        streq(s, "php_method_dynamic");
    return indirect and streq(bareSegment(rc.reason), call_short);
}

/// Look up the highest-confidence LSP-resolved call entry whose caller QN
/// matches the textual call's enclosing function and whose callee QN
/// short-name matches the textual callee. Returns a pointer into `arr`
/// or null if no qualifying entry exists.
///
/// Match rule:
///   1. exact caller_qn + callee short-name match wins first;
///   2. if no exact caller match exists AND allow_tail_match is set
///      (JVM callers only, see cbm_pipeline_lsp_allow_tail_match), a
///      unique Class.method tail match between rc.caller_qn and
///      call.enclosing_func_qn may win;
///   3. ambiguous tails return null so the registry fallback stays in
///      control.
///
/// The pointer returned aliases into `arr` and stays valid as long as the
/// underlying CBMFileResult is alive.
pub export fn cbm_pipeline_find_lsp_resolution(
    arr_opt: ?*const Rows,
    call_opt: ?*const c.CBMCall,
    allow_tail_match: bool,
) ?*const Row {
    const arr = arr_opt orelse return null;
    const call = call_opt orelse return null;
    if (arr.count == 0) return null;
    if (call.enclosing_func_qn == null or call.callee_name == null) return null;

    // The call's callee_name is receiver-qualified for method/qualified
    // calls ("c.inc", "A.Helper", "Math::square", "p->run"); the LSP
    // records the resolved class-qualified callee_qn ("Class.inc"). Compare
    // the bare last segment on BOTH sides so method-dispatch resolutions
    // join — the LSP already did the receiver->type resolution, and matching
    // the full "c.inc" against "inc" would always miss, silently dropping the
    // type-aware LSP strategy to the weaker textual registry. Free-function
    // calls (bare callee_name) are unaffected.
    const call_short = bareSegment(call.callee_name);
    var best_exact: ?*const Row = null;
    for (0..ix(arr.count)) |i| {
        const rc = util.at(arr.items, i);
        if (!isInvocationRow(rc)) continue;
        if (!streq(rc.caller_qn, call.enclosing_func_qn)) continue;
        if (!siteAdmits(rc, call)) continue;
        if (!streq(bareSegment(rc.callee_qn), call_short) and !reasonNamesCall(rc, call_short)) continue;
        if (best_exact == null or rc.confidence > best_exact.?.confidence) best_exact = rc;
    }
    if (best_exact != null) return best_exact;
    if (!allow_tail_match) return null;

    const call_tail = qnClassMethodTail(call.enclosing_func_qn);
    if (call_tail == null) return null;

    var best_tail: ?*const Row = null;
    for (0..ix(arr.count)) |i| {
        const rc = util.at(arr.items, i);
        if (!isInvocationRow(rc)) continue;
        if (call_short == null or !streq(lastDotSegment(rc.callee_qn), call_short)) continue;
        if (!qnClassMethodTailEq(rc.caller_qn, call_tail)) continue;
        if (!siteAdmits(rc, call)) continue;
        if (best_tail != null) return null;
        best_tail = rc;
    }
    return best_tail;
}

/// Resolve an LSP-emitted callee_qn to a graph-buffer node.
///
/// Per-file LSPs sometimes emit `callee_qn` as the raw package-shaped
/// import path the source code uses rather than the project-qualified QN
/// the gbuf actually stores. The fallback rule is:
///   1. try the LSP-emitted QN as-is;
///   2. retry with `<project>.<callee_qn>` when needed;
///   3. if both fail AND allow_tail_match is set (JVM callers only, see
///      cbm_pipeline_lsp_allow_tail_match), use the exact node-name index
///      to narrow candidates by short method name and accept exactly one
///      Function/Method whose qualified_name has the same Class.method
///      tail.
///
/// Returns the matching node, or null if neither lookup hits.
pub export fn cbm_pipeline_lsp_target_node(
    gbuf: ?*const Gbuf,
    project_name: CStr,
    callee_qn: CStr,
    allow_tail_match: bool,
) ?*const Node {
    if (gbuf == null or callee_qn == null) return null;
    const direct: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, callee_qn);
    if (direct != null) return direct;

    // Exact retries, in tie-rule order (CBM_MACRO_QN_SUFFIX): the definition
    // first -- the QN as given, then with the project prefix -- and only when
    // neither names a node the C-preprocessor macro of that name, in the same
    // two spellings. The macro case is a prototype the C resolver knows whose
    // only node in the graph is its rename macro (`extern T f(...);` plus
    // `#define f f_v2`). One buffer, "<project>.<callee_qn>#macro", serves all
    // three retries.
    const suffix = c.CBM_MACRO_QN_SUFFIX;
    const proj_len: usize = if (project_name != null and project_name[0] != 0) c.strlen(project_name) else 0;
    const callee_len: usize = c.strlen(callee_qn);
    const add_prefix = proj_len > 0 and
        !(c.strncmp(callee_qn, project_name, proj_len) == 0 and callee_qn[proj_len] == '.');
    if (proj_len <= std.math.maxInt(usize) -% callee_len -% suffix.len -% 2) {
        const buf = interop.mallocN(u8, proj_len + 1 + callee_len + suffix.len + 1);
        if (buf != null) {
            const own = buf + proj_len + 1; // callee_qn inside the buffer
            if (proj_len > 0) @memcpy(buf[0..proj_len], project_name[0..proj_len]);
            buf[proj_len] = '.';
            @memcpy(own[0..callee_len], callee_qn[0..callee_len]);
            own[callee_len] = 0;
            var hit: ?*const Node = if (add_prefix) c.cbm_gbuf_find_by_qn(gbuf, buf) else null;
            if (hit == null) {
                @memcpy(own[callee_len .. callee_len + suffix.len], suffix);
                own[callee_len + suffix.len] = 0;
                hit = c.cbm_gbuf_find_by_qn(gbuf, own);
                if (hit == null and add_prefix) hit = c.cbm_gbuf_find_by_qn(gbuf, buf);
            }
            c.free(buf);
            if (hit != null) return hit;
        }
    }
    if (!allow_tail_match) return null;

    const short_name = lastDotSegment(callee_qn);
    const callee_tail = qnClassMethodTail(callee_qn);
    if (callee_tail == null) return null;
    var hits: [*c][*c]const Node = null;
    var hit_count: c_int = 0;
    if (c.cbm_gbuf_find_by_name(gbuf, short_name, &hits, &hit_count) != 0 or hit_count == 0) return null;
    _ = tail_lookups.fetchAdd(1, .monotonic);
    _ = tail_candidates.fetchAdd(@bitCast(@as(i64, hit_count)), .monotonic);

    var match: ?*const Node = null;
    for (0..ix(hit_count)) |i| {
        const cand_opt: ?*const Node = hits[i];
        const cand = cand_opt orelse continue;
        if (cand.label == null or cand.qualified_name == null) continue;
        if (!streq(cand.label, "Function") and !streq(cand.label, "Method")) continue;
        if (!qnClassMethodTailEq(cand.qualified_name, callee_tail)) continue;
        if (match != null) return null;
        match = cand;
    }
    return match;
}

/// Rows that take part in the CALLS join. Count-based heuristics that compare
/// LSP output with textual calls must not count reference rows, nor the
/// confidence-0 lsp_unresolved diagnostics the join ignores.
pub fn invocationRowCount(arr: *const Rows) c_int {
    var n: c_int = 0;
    for (0..ix(arr.count)) |i| {
        const rc = util.at(arr.items, i);
        n += @intFromBool(rc.kind == c.CBM_RESOLVED_INVOCATION and rc.confidence >= confidence_floor);
    }
    return n;
}

// ── Callable references (CALL_REFERENCE) ────────────────────────────
//
// A function or method named as a value is a CALL_REFERENCE only when the
// language LSP proved one exact callable at that source occurrence. The
// semantic join is by (enclosing function, exact byte span of the reference
// token); the textual registry never earns CALL_REFERENCE, it only yields an
// ordinary USAGE. Reference rows never take part in the CALLS join.

/// Rows that take part in the CALL_REFERENCE join.
pub fn referenceRowCount(arr: *const Rows) c_int {
    var n: c_int = 0;
    for (0..ix(arr.count)) |i| {
        n += @intFromBool(util.at(arr.items, i).kind == c.CBM_RESOLVED_CALL_REFERENCE);
    }
    return n;
}

/// Extraction marked this usage as a callable-reference candidate: explicit
/// reference syntax, or a bare value in a narrow position (direct argument,
/// assignment right-hand side) the language LSP knows how to prove.
fn usageIsReferenceCandidate(usage: *const c.CBMUsage) bool {
    return (usage.kind == c.CBM_USAGE_CALL_REFERENCE or usage.may_be_call_reference) and
        usage.site_end_byte > usage.site_start_byte;
}

/// Graph labels a CALL_REFERENCE may point at (a Class covers constructor
/// references such as Java `Foo::new`). Anything else (a field, a variable, an
/// interface) proved by the LSP still lands as USAGE.
pub fn nodeIsCallableTarget(node_opt: ?*const Node) bool {
    const node = node_opt orelse return false;
    if (node.label == null) return false;
    return streq(node.label, "Function") or streq(node.label, "Method") or
        streq(node.label, "Constructor") or streq(node.label, "Class");
}

/// Per-file index of CALL_REFERENCE rows sorted by site, so each candidate
/// usage costs one binary search instead of a scan of every resolved row.
pub const RefIndex = struct {
    rows: []*const Row = &.{},

    /// On allocation failure the index is empty: the caller then finds no
    /// semantic row and every candidate stays USAGE (fail closed).
    pub fn build(arr: *const Rows) RefIndex {
        const n: usize = @intCast(referenceRowCount(arr));
        if (n == 0) return .{};
        const rows = interop.mallocN(*const Row, n);
        if (rows == null) return .{};
        var k: usize = 0;
        for (0..ix(arr.count)) |i| {
            const rc = util.at(arr.items, i);
            if (rc.kind == c.CBM_RESOLVED_CALL_REFERENCE) {
                rows[k] = rc;
                k += 1;
            }
        }
        c.qsort(@ptrCast(rows), n, @sizeOf(*const Row), rowCompare);
        return .{ .rows = rows[0..n] };
    }

    pub fn deinit(self: *RefIndex) void {
        if (self.rows.len > 0) c.free(@ptrCast(self.rows.ptr));
        self.* = .{};
    }

    fn rowCompare(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
        const l: *const Row = @as(*const *const Row, @ptrCast(@alignCast(a))).*;
        const r: *const Row = @as(*const *const Row, @ptrCast(@alignCast(b))).*;
        if (l.site_start_byte != r.site_start_byte) return if (l.site_start_byte < r.site_start_byte) -1 else 1;
        if (l.site_end_byte != r.site_end_byte) return if (l.site_end_byte < r.site_end_byte) -1 else 1;
        // Stable order within one site: confidence ties keep the first row.
        const lp = @intFromPtr(l);
        const rp = @intFromPtr(r);
        return if (lp < rp) -1 else if (lp > rp) 1 else 0;
    }

    /// Scan the rows sharing the usage's byte span. `tail` null requires the
    /// exact enclosing function; otherwise the caller's Class.method tail must
    /// match.
    fn scan(self: RefIndex, first: usize, usage: *const c.CBMUsage, tail: CStr) ?*const Row {
        const ref_leaf = bareSegment(usage.ref_name);
        var best: ?*const Row = null;
        for (self.rows[first..]) |r| {
            if (r.site_start_byte != usage.site_start_byte or r.site_end_byte != usage.site_end_byte) break;
            if (r.caller_qn == null or r.callee_qn == null or r.confidence < confidence_floor) continue;
            const caller_ok = if (tail != null)
                qnClassMethodTailEq(r.caller_qn, tail)
            else
                streq(r.caller_qn, usage.enclosing_func_qn);
            if (!caller_ok) continue;
            // The row must name this reference: the callee leaf, or `reason`
            // for an alias (`cb` proven to hold `handler`).
            if (!streq(bareSegment(r.callee_qn), ref_leaf) and
                !(r.reason != null and streq(bareSegment(r.reason), ref_leaf)))
            {
                continue;
            }
            if (best == null) {
                best = r;
            } else if (!streq(best.?.callee_qn, r.callee_qn)) {
                return null; // two targets at one occurrence: ambiguous, fail closed
            } else if (r.confidence > best.?.confidence) {
                best = r;
            }
        }
        return best;
    }

    /// The exact semantic row for one candidate usage, or null. Rows must
    /// share the usage's byte span and enclosing function and clear the
    /// confidence floor. JVM callers (allow_tail_match) may fall back to a
    /// Class.method tail match on the caller, the same rule the CALLS join
    /// uses for package-shaped LSP QNs.
    fn find(self: RefIndex, usage: *const c.CBMUsage, allow_tail_match: bool) ?*const Row {
        if (self.rows.len == 0 or !usageIsReferenceCandidate(usage) or
            usage.enclosing_func_qn == null or usage.ref_name == null)
        {
            return null;
        }
        var lo: usize = 0;
        var hi: usize = self.rows.len;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            const r = self.rows[mid];
            if (r.site_start_byte < usage.site_start_byte or
                (r.site_start_byte == usage.site_start_byte and r.site_end_byte < usage.site_end_byte))
            {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        const exact = self.scan(lo, usage, null);
        if (exact != null or !allow_tail_match) return exact;
        const tail = qnClassMethodTail(usage.enclosing_func_qn);
        return if (tail != null) self.scan(lo, usage, tail) else null;
    }
};

/// Outcome of the exact semantic join for one usage.
pub const RefJoinKind = enum {
    /// no LSP row at this occurrence: registry fallback decides
    none,
    /// proven callable target: emit CALL_REFERENCE
    callable,
    /// proven non-callable target: at most a USAGE
    value,
    /// a row owns the occurrence but its target is not in the graph: emit
    /// nothing rather than guess by name
    skip,
};

pub const RefJoin = struct {
    kind: RefJoinKind = .none,
    target: ?*const Node = null,
    row: ?*const Row = null,
};

pub fn joinLspReference(
    gbuf: ?*const Gbuf,
    project_name: CStr,
    index: RefIndex,
    usage: *const c.CBMUsage,
    allow_tail_match: bool,
) RefJoin {
    const row = index.find(usage, allow_tail_match) orelse return .{};
    const tgt = cbm_pipeline_lsp_target_node(gbuf, project_name, row.callee_qn, allow_tail_match) orelse
        return .{ .kind = .skip, .row = row };
    return .{
        .kind = if (nodeIsCallableTarget(tgt)) .callable else .value,
        .target = tgt,
        .row = row,
    };
}

/// Edge properties for a USAGE / CALL_REFERENCE edge. A CALL_REFERENCE always
/// comes from an LSP row (row non-null), whose strategy and confidence are the
/// edge's provenance, in the same keys CALLS edges use; USAGE stays
/// {"callee":...}. esc_ref is already JSON-escaped.
pub fn referenceProps(buf: []u8, esc_ref: CStr, row_opt: ?*const Row) void {
    if (row_opt) |row| {
        if (row.strategy != null and row.strategy[0] != 0) {
            _ = c.snprintf(buf.ptr, buf.len, "{\"callee\":\"%s\",\"confidence\":%.2f,\"strategy\":\"%s\"}", esc_ref, @as(f64, row.confidence), row.strategy);
            return;
        }
    }
    _ = c.snprintf(buf.ptr, buf.len, "{\"callee\":\"%s\"}", esc_ref);
}

/// Registry fallback for a usage the LSP did not prove. A candidate emitted
/// only for the join (reference_only) never falls back. Explicit JVM reference
/// syntax may not bind by a bare-name guess (unique_name / suffix_match),
/// except a Kotlin `::name` whose spelling the file imports.
pub fn referenceFallbackAllowed(
    lang: Language,
    usage: *const c.CBMUsage,
    strategy: CStr,
    import_names: [*c]const CStr,
    import_count: c_int,
) bool {
    if (usage.reference_only) return false;
    if (usage.kind != c.CBM_USAGE_CALL_REFERENCE or (lang != c.CBM_LANG_JAVA and lang != c.CBM_LANG_KOTLIN)) {
        return true;
    }
    if (strategy != null and strategy[0] != 0 and !streq(strategy, "unique_name") and
        !streq(strategy, "suffix_match"))
    {
        return true;
    }
    if (lang == c.CBM_LANG_KOTLIN and usage.ref_name != null and import_names != null) {
        for (0..ix(import_count)) |i| {
            if (import_names[i] != null and streq(import_names[i], usage.ref_name)) return true;
        }
    }
    return false;
}
