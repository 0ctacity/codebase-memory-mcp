//! registry.zig — Function/Method/Class registry for call resolution.
//!
//! Implements the registry functions of src/pipeline/pipeline.h. Indexes all
//! callable symbols by qualified name and simple name. Resolution uses a
//! prioritized strategy chain:
//!   1. Import map lookup
//!   2. Same-module match
//!   3. Unique name (single candidate project-wide)
//!   4. Suffix match with import distance scoring

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const Resolution = c.cbm_resolution_t;
const HashTable = c.CBMHashTable;

const REG_INIT_CAP = 16;
const REG_MIN_CANDIDATES = 3;
const REG_RESOLVED = 1;
const REG_SUFFIX_ALLOC = 2;
/// Names with more registered definitions than this are unresolvable by name
/// alone: candidateCountPenalty already floors their confidence to ~3/count
/// (<= 0.006 at 256), so the emitted edge is noise — while the candidate walk
/// (reachability + scoring per candidate, re-done per file) is the dominant
/// resolution cost on identifier-dense repos. On the Linux kernel, 274 names
/// exceed 256 candidates ("list_head" 7188, "flags" 5520, "dev" 4374, ...) and
/// accounted for ~900 s of the 987 s usage-resolution CPU. Bail out early.
const REG_MAX_CANDIDATES = 256;
const REG_FULL_CONF: f64 = 1.0;
const REG_HALF_PENALTY: f64 = 0.5;

const DEFAULT_CONFIDENCE: f64 = 0.5;

/// Confidence score → human-readable band label.
const CONF_BAND_HIGH: f64 = 0.7;
const CONF_BAND_MEDIUM: f64 = 0.45;
const CONF_BAND_SPECULATIVE: f64 = 0.25;

pub export fn cbm_confidence_band(score: f64) CStr {
    if (score >= CONF_BAND_HIGH) return "high";
    if (score >= CONF_BAND_MEDIUM) return "medium";
    if (score >= CONF_BAND_SPECULATIVE) return "speculative";
    return "";
}

// ── Resolution confidence scores ────────────────────────────────────
/// Strategy 1: import_map — direct import → high confidence
const CONF_IMPORT_MAP: f64 = 0.95;
const CONF_IMPORT_MAP_SUFFIX: f64 = 0.85;
/// Strategy 2: same_module — same file/package → high confidence
const CONF_SAME_MODULE: f64 = 0.90;
/// Strategy 3: unique_name — only one candidate project-wide
const CONF_UNIQUE_NAME: f64 = 0.75;
/// Strategy 4: suffix_match — multiple candidates, filtered
const CONF_SUFFIX_MATCH: f64 = 0.55;
/// Fuzzy fallback: lower confidence
const CONF_FUZZY_SINGLE: f64 = 0.40;
const CONF_FUZZY_MULTI: f64 = 0.30;
/// Candidate count penalty cap
const CANDIDATE_PENALTY_CAP: f64 = 3.0;

// ── Internal types ──────────────────────────────────────────────────

/// Array of QN strings for byName index
const QnArray = util.DynArray(MutStr);

const LABEL_POOL_CAP = 64;

const Registry = extern struct {
    /// Interned label strings (<=~30 distinct labels; owned here, freed in
    /// _free). The exact map's VALUES point into this pool instead of one
    /// strdup per registered definition (~8.5M strdups on the kernel).
    label_pool: [LABEL_POOL_CAP]MutStr,
    label_pool_n: c_int,
    /// exact: qualifiedName → label string (heap-owned copies)
    exact: ?*HashTable,

    /// byName: simpleName → QnArray* (heap-owned)
    by_name: ?*HashTable,
};

fn byName(r: *const Registry, name: CStr) ?*QnArray {
    return @ptrCast(@alignCast(c.cbm_ht_get(r.by_name, name)));
}

// ── Helpers ─────────────────────────────────────────────────────────

/// Extract the last path segment from a QN. Returns pointer into s.
/// Recognizes both '.' (most langs) and Rust/C++ '::' separators, so a
/// scoped callee like "lib::square" yields "square" rather than the whole
/// scoped path (which never matches the by-name index).
fn simpleName(qn: CStr) CStr {
    const dot: CStr = c.strrchr(qn, '.');
    var seg: CStr = if (dot != null) dot + 1 else qn;
    // Find the last "::" and, if it sits after the last '.', use the
    // segment following it.
    var colons: CStr = null;
    var p: CStr = c.strstr(qn, "::");
    while (p != null) : (p = c.strstr(p + 2, "::")) {
        colons = p;
    }
    if (colons != null and @intFromPtr(colons + 2) > @intFromPtr(seg)) {
        seg = colons + 2;
    }
    return seg;
}

/// Count common dot-separated prefix segments.
fn commonPrefixLen(a_in: CStr, b_in: CStr) c_int {
    if (a_in == null or b_in == null) return 0;
    var a = a_in;
    var b = b_in;
    var count: c_int = 0;
    while (a[0] != 0 and b[0] != 0) {
        // Find next segment in each
        const adot: CStr = c.strchr(a, '.');
        const bdot: CStr = c.strchr(b, '.');
        const alen: usize = if (adot != null) util.ptrDiff(adot, a) else c.strlen(a);
        const blen: usize = if (bdot != null) util.ptrDiff(bdot, b) else c.strlen(b);
        if (alen != blen or c.memcmp(a, b, alen) != 0) break;
        count += 1;
        a += alen + @as(usize, if (adot != null) 1 else 0);
        b += blen + @as(usize, if (bdot != null) 1 else 0);
        if (adot == null or bdot == null) break;
    }
    return count;
}

const REG_TEST_PENALTY = 1000;

/// Check if a qualified name looks like a test/mock path.
fn isTestQn(qn: CStr) bool {
    if (qn == null) return false;
    return c.strstr(qn, "Test") != null or c.strstr(qn, "test") != null or
        c.strstr(qn, "Mock") != null or c.strstr(qn, "mock") != null or
        c.strstr(qn, "Stub") != null or c.strstr(qn, "stub") != null or
        c.strstr(qn, "Fake") != null or c.strstr(qn, "fake") != null or
        c.strstr(qn, "Fixture") != null or c.strstr(qn, "spec") != null;
}

/// Score a candidate for tiebreaking. Higher = better.
/// Layer 1: Non-test code preferred over test code (+1000)
/// Layer 2: Namespace proximity via common prefix length (+plen)
fn candidateScore(candidate_qn: CStr, module_qn: CStr) c_int {
    var score: c_int = 0;
    if (!isTestQn(candidate_qn)) score += REG_TEST_PENALTY;
    score += commonPrefixLen(candidate_qn, module_qn);
    return score;
}

/// Pick candidate with highest composite score (test-deprioritization + namespace proximity).
fn bestByImportDistance(candidates: [*c]const CStr, count: c_int, module_qn: CStr) CStr {
    var best: CStr = null;
    var best_score: c_int = c.CBM_NOT_FOUND;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const score = candidateScore(candidates[i], module_qn);
        if (score > best_score) {
            best_score = score;
            best = candidates[i];
        }
    }
    return best;
}

// ── Per-file is_import_reachable memoization cache ──────────────────
//
// The hot path on kubernetes spends ~50% of resolve_calls CPU in
// isImportReachable's O(candidates × imports × strstr) scan. The
// SAME candidate_qn is re-evaluated dozens of times per file because
// the same callee_name appears in multiple call sites and each lookup
// re-checks all candidates for that name.
//
// Cache life-cycle is per-FILE (because import_vals changes between
// files). resolve_file_calls calls _begin at file entry and _end at
// file exit. Thread-local so each worker has its own cache without
// contention.
threadlocal var reach_cache: ?*HashTable = null;

/// Sentinels stored as values in the cache. NULL means "not cached".
/// We need two distinct non-NULL pointers to encode true/false.
const REACH_CACHE_TRUE: ?*anyopaque = @ptrFromInt(1);
const REACH_CACHE_FALSE: ?*anyopaque = @ptrFromInt(2);

fn reachCacheFreeKey(key: CStr, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = val;
    _ = ud;
    c.free(@constCast(key));
}

pub export fn cbm_registry_reach_cache_begin(estimated_capacity_in: c_int) void {
    var estimated_capacity = estimated_capacity_in;
    if (reach_cache) |cache| {
        // Defensive: caller forgot to call _end. Clear and reuse.
        c.cbm_ht_foreach(cache, reachCacheFreeKey, null);
        c.cbm_ht_clear(cache);
        return;
    }
    if (estimated_capacity < 16) estimated_capacity = 16;
    reach_cache = c.cbm_ht_create(@intCast(estimated_capacity));
}

pub export fn cbm_registry_reach_cache_end() void {
    const cache = reach_cache orelse return;
    c.cbm_ht_foreach(cache, reachCacheFreeKey, null);
    c.cbm_ht_free(cache);
    reach_cache = null;
}

// ── Per-file import-map prefix → module-QN hash ─────────────────────
//
// resolveImportMap does a linear strcmp scan over the per-file
// import list for every call (and every usage, throw, rw, def). On
// kubernetes typical files have 20-30 imports and ~200 calls. That's
// 4000-6000 strcmps per file × 20K files = ~80M strcmps.
//
// Build a hash from prefix → module_qn ONCE per file at file entry.
// Each lookup becomes O(1). Keys and values are BORROWED from the
// caller's import_map — they outlive the cache scope.
threadlocal var import_map_cache: ?*HashTable = null;

pub export fn cbm_registry_import_map_cache_begin(keys: [*c]CStr, vals: [*c]CStr, count: c_int) void {
    if (import_map_cache) |cache| {
        c.cbm_ht_free(cache);
        import_map_cache = null;
    }
    if (keys == null or vals == null or count <= 0) return;
    import_map_cache = c.cbm_ht_create(@as(u32, @intCast(count)) *% 2 +% 8);
    const cache = import_map_cache orelse return;
    for (0..ix(count)) |i| {
        if (keys[i] != null and vals[i] != null) {
            _ = c.cbm_ht_set(cache, keys[i], @ptrCast(@constCast(vals[i])));
        }
    }
}

pub export fn cbm_registry_import_map_cache_end() void {
    const cache = import_map_cache orelse return;
    // Keys/values borrowed from caller — no free callback needed.
    c.cbm_ht_free(cache);
    import_map_cache = null;
}

// ── Per-file full-result cache for cbm_registry_resolve ─────────────
//
// THE big one: 98.7% of resolve_calls CPU on kubernetes lives inside
// cbm_registry_resolve's strategy chain (suffix_match iterating
// hundreds of candidates per common name like "Get"/"Add"/"New"
// × 1M LSP-miss calls = ~800s of work).
//
// Since module_qn is CONSTANT per file, the resolution for a given
// callee_name within a file is also constant — same name resolves
// to the same QN every time. Cache the full cbm_resolution_t result
// keyed by callee_name; first lookup does the full chain, repeats
// are O(1). On K8s a typical file has ~200 calls but only ~50
// unique callee_names → ~75% hit rate → ~75% of the resolve cost
// eliminated.
const ResolveCacheEntry = extern struct {
    res: Resolution,
};

/// Entries are malloc'd; keys are strdup'd. Both freed in _end.
threadlocal var resolve_cache: ?*HashTable = null;

fn resolveCacheFreeEntry(key: CStr, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    c.free(@constCast(key));
    c.free(val);
}

pub export fn cbm_registry_resolve_cache_begin(estimated_capacity_in: c_int) void {
    var estimated_capacity = estimated_capacity_in;
    if (resolve_cache) |cache| {
        c.cbm_ht_foreach(cache, resolveCacheFreeEntry, null);
        c.cbm_ht_free(cache);
        resolve_cache = null;
    }
    if (estimated_capacity < 32) estimated_capacity = 32;
    resolve_cache = c.cbm_ht_create(@intCast(estimated_capacity));
}

pub export fn cbm_registry_resolve_cache_end() void {
    const cache = resolve_cache orelse return;
    c.cbm_ht_foreach(cache, resolveCacheFreeEntry, null);
    c.cbm_ht_free(cache);
    resolve_cache = null;
}

/// Check if candidate's module prefix appears in import map values.
/// Uses stack buffer to avoid malloc/free per call in hot resolution loop.
/// Per-file memoization via TLS cache: repeated lookups of the same
/// candidate_qn (same name appears in many call sites) become O(1)
/// after the first computation.
fn isImportReachable(candidate_qn: CStr, import_vals: [*c]const CStr, import_count: c_int) bool {
    if (reach_cache) |cache| {
        const cached = c.cbm_ht_get(cache, candidate_qn);
        if (cached == REACH_CACHE_TRUE) return true;
        if (cached == REACH_CACHE_FALSE) return false;
    }

    var cand_mod: [c.CBM_SZ_512]u8 = undefined;
    const last: CStr = c.strrchr(candidate_qn, '.');
    if (last != null) {
        var len = util.ptrDiff(last, candidate_qn);
        if (len >= cand_mod.len) len = cand_mod.len - 1;
        @memcpy(cand_mod[0..len], candidate_qn[0..len]);
        cand_mod[len] = 0;
    } else {
        _ = c.snprintf(&cand_mod, cand_mod.len, "%s", candidate_qn);
    }
    var reachable = false;
    var i: usize = 0;
    while (i < import_count) : (i += 1) {
        if (c.strstr(&cand_mod, import_vals[i]) != null or c.strstr(import_vals[i], &cand_mod) != null) {
            reachable = true;
            break;
        }
    }

    if (reach_cache) |cache| {
        const kdup: MutStr = c.strdup(candidate_qn);
        if (kdup != null) {
            _ = c.cbm_ht_set(cache, kdup, if (reachable) REACH_CACHE_TRUE else REACH_CACHE_FALSE);
        }
    }
    return reachable;
}

/// Scale confidence inversely with candidate count.
fn candidateCountPenalty(base: f64, count: c_int) f64 {
    if (count <= REG_MIN_CANDIDATES) return base;
    return base * @min(REG_FULL_CONF, CANDIDATE_PENALTY_CAP / @as(f64, @floatFromInt(count)));
}

fn emptyResult() Resolution {
    return std.mem.zeroes(Resolution);
}

fn resolved(qn: CStr, strategy: CStr, confidence: f64, candidate_count: c_int) Resolution {
    return .{
        .qualified_name = qn,
        .strategy = strategy,
        .confidence = confidence,
        .candidate_count = candidate_count,
    };
}

// ── Perl builtin guard (#459 follow-up: call-graph noise) ───────────
// Curated subset of perlfunc core builtins. When a Perl CALL resolves
// only by the generic short-name matcher (no LSP, no import, after the
// same-module/name-lookup chain), a builtin name like `push`/`shift`/
// `keys` must NOT be wired to a project sub that merely shares the name
// — that is virtually always a false positive. A genuine intra-project
// call is resolved by earlier (LSP/textual) stages before this guard.
// MUST stay sorted ASCII-ascending for bsearch.
const PERL_BUILTINS = [_][*:0]const u8{
    "abs",       "atan2",   "binmode", "bless",     "caller",   "chdir",    "chmod",   "chomp",
    "chop",      "chown",   "chr",     "chroot",    "close",    "closedir", "cos",     "defined",
    "delete",    "die",     "do",      "each",      "eof",      "eval",     "exec",    "exists",
    "exit",      "fork",    "gmtime",  "goto",      "grep",     "hex",      "index",   "int",
    "join",      "keys",    "last",    "lc",        "lcfirst",  "length",   "local",   "localtime",
    "log",       "lstat",   "map",     "mkdir",     "my",       "next",     "oct",     "open",
    "opendir",   "ord",     "our",     "pop",       "pos",      "print",    "printf",  "push",
    "quotemeta", "rand",    "read",    "readdir",   "readline", "redo",     "ref",     "rename",
    "require",   "return",  "reverse", "rindex",    "rmdir",    "say",      "scalar",  "seek",
    "shift",     "sin",     "sleep",   "sort",      "splice",   "split",    "sprintf", "sqrt",
    "srand",     "stat",    "substr",  "system",    "time",     "uc",       "ucfirst", "undef",
    "unlink",    "unshift", "values",  "wantarray", "warn",     "write",
};

fn perlBuiltinCmp(key: ?*const anyopaque, elem: ?*const anyopaque) callconv(.c) c_int {
    const entry: *const [*:0]const u8 = @ptrCast(@alignCast(elem));
    return c.strcmp(@ptrCast(key), entry.*);
}

/// True if `name` is one of the curated Perl core builtins. Used to suppress
/// generic-resolver CALLS edges from Perl builtin invocations to project subs
/// that happen to share the builtin's name. Perl-scoped: callers gate on the
/// file language so no other language's resolution is affected.
pub export fn cbm_perl_is_builtin(name: CStr) bool {
    if (name == null or name[0] == 0) return false;
    return c.bsearch(
        name,
        &PERL_BUILTINS,
        PERL_BUILTINS.len,
        @sizeOf([*:0]const u8),
        perlBuiltinCmp,
    ) != null;
}

/// Decide whether a *resolved* Perl call edge is generic-resolver noise that
/// should be suppressed (#476). Returns true only for Perl, only for a builtin
/// invocation or a method call, and only when the registry landed the match via
/// a WEAK short-name strategy. High-confidence import/same-module strategies
/// (same_module, import_map, import_map_suffix) are KEPT so a genuine same-file
/// or imported call to a builtin-named sub still resolves — only the weak
/// short-name guesses (suffix_match, unique_name) are dropped. `strategy` is the
/// cbm_resolution_t.strategy of a non-empty match;
/// NULL/empty (no match) returns false. Pure + side-effect-free so the
/// suppression contract is unit-testable without a full pipeline.
pub export fn cbm_perl_suppress_generic_match(
    is_perl: bool,
    is_method: bool,
    callee_name: CStr,
    strategy: CStr,
) bool {
    if (!is_perl) return false;
    if (!(is_method or cbm_perl_is_builtin(callee_name))) return false;
    if (strategy == null or strategy[0] == 0) return false;
    if (util.streq(strategy, "same_module") or util.streq(strategy, "import_map") or
        util.streq(strategy, "import_map_suffix"))
    {
        return false; // high-confidence import/same-module match — keep the genuine edge
    }
    return true; // weak short-name match (suffix_match / unique_name / …) → drop
}

/// Dynamic-language analogue of the Perl guard above (#592/#606/#1276
/// direction; precedent #477). A member call `x.foo()` reaches the weak textual
/// cascade ONLY when the language's LSP could not resolve the receiver type —
/// type-resolved calls win via lsp_* strategies before the registry runs.
/// Binding such a call to a project symbol by a weak short-name strategy
/// fabricates a CALLS edge (`re.test()` -> SalesforceRestClient.test,
/// `accelerator.print()` -> MockAccelerator.print). Drop ONLY the weak
/// strategies; keep import/same-module/qualified-tail matches and every lsp_*
/// strategy. Uses an EXPLICIT drop-list (not keep-list + default-drop) because
/// the parallel resolver runs lsp_* strategies through the same guard variable —
/// a default-drop would silently kill lsp_ts_method. Pure + side-effect-free so
/// the contract is unit-testable without a full pipeline.
///
/// `enabled` is the CALLER's per-language gate, deliberately kept OUT of this
/// helper: the guard applies only to the language set each call site enumerates
/// (today Python plus the JS/TS family). Widening it is a
/// per-language decision made at the call site in pass_parallel.
///
/// The weak short-name strategies that actually reach the call-resolution
/// guards: the registry's suffix_match / unique_name and the parallel
/// field_type_hint. "fuzzy" is listed as defensive insurance only —
/// cbm_registry_fuzzy_resolve is not wired into the resolver
/// today, so it never reaches these helpers, but naming it keeps a future wiring
/// from silently reintroducing the noise. Everything else — same_module /
/// import_map / import_map_suffix / qualified_suffix / callee_suffix /
/// service_pattern / lsp_* — is a receiver- or import-aware match and is KEPT.
///
/// Shared by BOTH weak-call guards below so the drop-list exists exactly once: a
/// list that drifted between the member guard and the local-binding guard would
/// make the two disagree about what "weak" means.
fn weakShortNameStrategy(strategy: CStr) bool {
    if (strategy == null or strategy[0] == 0) return false;
    return util.streq(strategy, "suffix_match") or util.streq(strategy, "unique_name") or
        util.streq(strategy, "field_type_hint") or util.streq(strategy, "fuzzy");
}

pub export fn cbm_registry_strategy_is_weak(strategy: CStr) bool {
    return weakShortNameStrategy(strategy);
}

pub export fn cbm_suppress_weak_member_match(enabled: bool, is_method: bool, strategy: CStr) bool {
    if (!enabled or !is_method) return false;
    return weakShortNameStrategy(strategy);
}

/// Zig member calls are typed by the Zig LSP: mapped @import aliases and relative
/// files resolve as lsp_zig_*. A qualified_suffix match on a member call that
/// reached the registry therefore only matched spelling, e.g. `old.hello()` with
/// an unmapped `old` binding a same-named libs/old/src/old.zig.
pub export fn cbm_suppress_zig_qualified_member(is_zig: bool, is_method: bool, strategy: CStr) bool {
    return is_zig and is_method and strategy != null and util.streq(strategy, "qualified_suffix");
}

/// Bare-call counterpart of the member guard above. A Python call `foo()` whose
/// callee identifier is bound as a parameter of an enclosing scope cannot be the
/// module-level `foo`: the parameter shadows it for the whole body. Binding such
/// a call to a project Function/Method by a weak short-name strategy fabricates
/// the edge by construction (`def _run_with_heavy_slot(run): run()` ->
/// SatoriLive.run).
///
/// This is deliberately NOT keyed on the callee's spelling. A list of
/// "generic-looking" names (get / run / execute) asserts that certain spellings
/// are usually noise, which is a claim about corpus fashion rather than about
/// what the resolver knew — and it ages invisibly, because nothing fails when the
/// distribution shifts, the graph just quietly loses different edges. A parameter
/// binding is a fact about THIS file's scope, decidable outright.
///
/// `enabled` is the caller's per-language gate, kept out of the helper for the
/// same reason as the member guard. Pure; unit-tested in test_registry.c.
pub export fn cbm_suppress_weak_local_binding_call(
    enabled: bool,
    callee_is_locally_bound: bool,
    strategy: CStr,
) bool {
    if (!enabled or !callee_is_locally_bound) return false;
    return weakShortNameStrategy(strategy);
}

fn jsTsFamily(lang: c.CBMLanguage) bool {
    return lang == c.CBM_LANG_JAVASCRIPT or lang == c.CBM_LANG_TYPESCRIPT or lang == c.CBM_LANG_TSX;
}

/// C and C++ are one family for cross-language checks: .h maps to CBM_LANG_CPP
/// in the extension table, so a .c file referencing a symbol declared in its
/// own header would otherwise read as a language boundary.
fn cCppFamily(lang: c.CBMLanguage) bool {
    return lang == c.CBM_LANG_C or lang == c.CBM_LANG_CPP;
}

/// cbm_path_base, but on Windows a backslash also separates directories.
fn nativePathBasename(path: CStr) CStr {
    var base: CStr = c.cbm_path_base(path);
    if (builtin.os.tag == .windows) {
        const bslash: CStr = c.strrchr(base, '\\');
        if (bslash != null) base = bslash + 1;
    }
    return base;
}

pub export fn cbm_suppress_cross_language_suffix_match(
    caller_lang: c.CBMLanguage,
    target_file_path: CStr,
    strategy: CStr,
) bool {
    // Two same-named symbols in different languages: suffix_match picks one
    // winner by import-distance and attaches every bare-name call to it
    // (#725, Bash/Python main, JS/Python commit). unique_name is the
    // candidates==1 case (#1572) and is not this guard.
    if (strategy == null or !util.streq(strategy, "suffix_match")) return false;
    if (caller_lang == c.CBM_LANG_COUNT or target_file_path == null or target_file_path[0] == 0) {
        return false;
    }
    const target_lang = c.cbm_language_for_filename(nativePathBasename(target_file_path));
    if (target_lang == c.CBM_LANG_COUNT) return false;
    if (caller_lang == target_lang) return false;
    if (jsTsFamily(caller_lang) and jsTsFamily(target_lang)) return false;
    return true;
}

pub export fn cbm_suppress_cross_language_ref(caller_lang: c.CBMLanguage, target_file_path: CStr) bool {
    // #1928: USAGE / WRITES / READS analog of the CALLS guard above. A
    // variable or field reference resolved by the short-name registry must
    // not cross a language boundary: unlike CALLS, a reference edge carries
    // no import-closure evidence at all — a Go test's local `event` and an
    // eBPF C probe's automatic `event` share nothing but the spelling, so
    // EVERY registry strategy is a bare-name guess here and none is exempt.
    // LSP-backed semantic references resolve before the registry fallback
    // and never reach this predicate, which is where a genuine cross-language
    // binding (a future cgo resolver) would live. The JS/TS family keeps its
    // exemption (.js/.ts/.d.ts pairs legitimately share symbols), and C/C++
    // count as one family (.h maps to CBM_LANG_CPP).
    if (caller_lang == c.CBM_LANG_COUNT or target_file_path == null or target_file_path[0] == 0) {
        return false;
    }
    const target_lang = c.cbm_language_for_filename(nativePathBasename(target_file_path));
    if (target_lang == c.CBM_LANG_COUNT) return false;
    if (caller_lang == target_lang) return false;
    if (jsTsFamily(caller_lang) and jsTsFamily(target_lang)) return false;
    if (cCppFamily(caller_lang) and cCppFamily(target_lang)) return false;
    return true;
}

pub export fn cbm_go_suppress_bare_field_ref(is_go: bool, is_member_access: bool, target_label: CStr) bool {
    // #1942/#1962: a bare Go identifier can never denote a struct field —
    // field access is always a selector expression (x.f). The extractor
    // strips the receiver before the resolver runs (resolve_lhs_write_name
    // records the trailing field name; is_reference_node records the inner
    // field_identifier), so the reference TEXT is always dot-less and cannot
    // carry the distinction — the recorded is_member_access shape can. Only a
    // reference that was never the member half of a selector is refused a
    // Field bind. Go-gated: a C#/Java/C++/Python method body legitimately
    // references its own members bare (cp_reads_writes_cs_static_field pins
    // that shape as required), so a global veto would break those languages.
    if (!is_go or is_member_access or target_label == null) return false;
    return util.streq(target_label, "Field");
}

// ── Lifecycle ───────────────────────────────────────────────────────

pub export fn cbm_registry_new() ?*Registry {
    const r: *Registry = @as(?*Registry, interop.callocN(Registry, 1)) orelse return null;
    r.exact = c.cbm_ht_create(c.CBM_SZ_1K);
    r.by_name = c.cbm_ht_create(c.CBM_SZ_512);
    return r;
}

fn freeLabel(key: CStr, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    _ = value; // interned in the registry's label_pool
    c.free(@constCast(key));
}

fn freeQnArray(key: CStr, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    const arr_c: ?*QnArray = @ptrCast(@alignCast(value));
    if (arr_c) |arr| {
        // items borrow the exact map's keys — freed there, not here
        util.daFree(arr);
        c.free(arr);
    }
    c.free(@constCast(key));
}

pub export fn cbm_registry_free(r_c: ?*Registry) void {
    const r = r_c orelse return;
    // by_name first: its items borrow exact's keys.
    c.cbm_ht_foreach(r.by_name, freeQnArray, null);
    c.cbm_ht_free(r.by_name);
    c.cbm_ht_foreach(r.exact, freeLabel, null);
    c.cbm_ht_free(r.exact);
    for (r.label_pool[0..ix(r.label_pool_n)]) |label| {
        c.free(label);
    }
    c.free(r);
}

// ── Registration ────────────────────────────────────────────────────

pub export fn cbm_registry_add(r_c: ?*Registry, name: CStr, qualified_name: CStr, label: CStr) void {
    _ = name;
    const r = r_c orelse return;
    if (qualified_name == null or label == null) return;

    // Check for duplicate
    if (c.cbm_ht_get(r.exact, qualified_name) != null) return;

    // Intern the label (bounded set; linear scan is fine at this size).
    var interned: CStr = null;
    for (r.label_pool[0..ix(r.label_pool_n)]) |pooled| {
        if (util.streq(pooled, label)) {
            interned = pooled;
            break;
        }
    }
    if (interned == null and r.label_pool_n < LABEL_POOL_CAP) {
        r.label_pool[ix(r.label_pool_n)] = c.strdup(label);
        interned = r.label_pool[ix(r.label_pool_n)];
        r.label_pool_n += 1;
    }
    if (interned == null) return; // pool exhausted (cannot happen with sane label sets)

    // Store in exact map: QN → interned label. The key is the registry's ONE
    // owned copy of the QN; by_name below borrows it (same lifetime) instead
    // of a second strdup — this pair of copies was ~280 MB on the kernel.
    _ = c.cbm_ht_set(r.exact, c.strdup(qualified_name), @ptrCast(@constCast(interned)));
    const owned_qn: CStr = c.cbm_ht_get_key(r.exact, qualified_name);

    // Index by simple name.
    // No array dedup needed: exact-map check above guarantees uniqueness.
    const simple = simpleName(qualified_name);
    var arr: ?*QnArray = byName(r, simple);
    if (arr == null) {
        arr = interop.callocN(QnArray, 1);
        _ = c.cbm_ht_set(r.by_name, c.strdup(simple), arr);
    }
    util.daPush(arr.?, @constCast(owned_qn));
}

// ── Lookup ──────────────────────────────────────────────────────────

pub export fn cbm_registry_exists(r_c: ?*const Registry, qn: CStr) bool {
    const r = r_c orelse return false;
    if (qn == null) return false;
    return c.cbm_ht_get(r.exact, qn) != null;
}

pub export fn cbm_registry_label_of(r_c: ?*const Registry, qn: CStr) CStr {
    const r = r_c orelse return null;
    if (qn == null) return null;
    return @ptrCast(c.cbm_ht_get(r.exact, qn));
}

pub export fn cbm_registry_find_by_name(
    r_c: ?*const Registry,
    name: CStr,
    out_c: ?*[*c]CStr,
    count_c: ?*c_int,
) c_int {
    const r = r_c orelse return c.CBM_NOT_FOUND;
    const out = out_c orelse return c.CBM_NOT_FOUND;
    const count = count_c orelse return c.CBM_NOT_FOUND;
    const arr = byName(r, name);
    if (arr != null and arr.?.count > 0) {
        out.* = @ptrCast(arr.?.items);
        count.* = arr.?.count;
    } else {
        out.* = null;
        count.* = 0;
    }
    return 0;
}

pub export fn cbm_registry_size(r: ?*const Registry) c_int {
    return if (r) |reg| @intCast(c.cbm_ht_count(reg.exact)) else 0;
}

// ── Resolution ──────────────────────────────────────────────────────

/// Strategy 1: Import map lookup (exact → suffix fallback)
fn resolveImportMap(
    r: *const Registry,
    prefix: CStr,
    suffix: CStr,
    keys: [*c]const CStr,
    vals: [*c]const CStr,
    map_count: c_int,
) Resolution {
    if (keys == null or vals == null or map_count <= 0) return emptyResult();

    // Find prefix in import map keys. Prefer the per-file TLS hash
    // cache (O(1)) over the linear scan when available.
    var target: CStr = null;
    if (import_map_cache) |cache| {
        target = @ptrCast(c.cbm_ht_get(cache, prefix));
    } else {
        for (0..ix(map_count)) |i| {
            if (util.streq(keys[i], prefix)) {
                target = vals[i];
                break;
            }
        }
    }
    if (target == null) return emptyResult();

    // Build candidate: resolved.suffix or resolved.prefix.
    // When the callee has a dot ("pkg.Func"), prefix="pkg" is the import key
    // and suffix="Func" is the function name, so the target QN is
    // resolved.Func. When the callee is bare ("requireAdmin"), prefix IS the
    // function name and suffix is NULL, so the target QN must be
    // resolved.requireAdmin — not just resolved, which would point at the
    // module node and miss the function entirely.
    //
    // Direct hit only for a bare callee whose import already names the
    // definition: `from m import f as g; g()` must reach m.f, not invent m.g
    // (upstream #875/#979, 7fda9f71). With a suffix (`imported.method()`) a
    // direct hit would swallow the suffix and bind the call to the imported
    // symbol itself (upstream #1000).
    const has_suffix = suffix != null and suffix[0] != 0;
    if (!has_suffix) {
        const direct: CStr = c.cbm_ht_get_key(r.exact, target);
        if (direct != null) {
            return resolved(direct, "import_map", CONF_IMPORT_MAP, REG_RESOLVED);
        }
    }
    var candidate: [c.CBM_SZ_512]u8 = undefined;
    if (has_suffix) {
        _ = c.snprintf(&candidate, candidate.len, "%s.%s", target, suffix);
    } else {
        _ = c.snprintf(&candidate, candidate.len, "%s.%s", target, prefix);
    }
    // Use cbm_ht_get_key to get the persistent heap-owned key string
    const stored_key: CStr = c.cbm_ht_get_key(r.exact, &candidate);
    if (stored_key != null) {
        return resolved(stored_key, "import_map", CONF_IMPORT_MAP, REG_RESOLVED);
    }

    // import_map_suffix fallback: find a QN starting with resolved+"." and
    // ending with "."+suffix. Any such QN's last segment equals the last
    // segment of suffix, so probe the by_name index and tail-check the (few)
    // candidates instead of cbm_ht_foreach over the WHOLE exact table — that
    // scan ran per unresolved call and dominated elasticsearch's resolve
    // phase (94% of samples: 700k-entry foreach + strlen per entry).
    if (has_suffix) {
        var resolved_dot: [c.CBM_SZ_512]u8 = undefined;
        var dot_suffix: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&resolved_dot, resolved_dot.len, "%s.", target);
        _ = c.snprintf(&dot_suffix, dot_suffix.len, ".%s", suffix);
        if (byName(r, simpleName(suffix))) |arr| {
            const rd_len = c.strlen(&resolved_dot);
            const ds_len = c.strlen(&dot_suffix);
            for (0..ix(arr.count)) |i| {
                const qn: CStr = arr.items[i];
                const klen = c.strlen(qn);
                if (klen >= rd_len + ds_len and c.strncmp(qn, &resolved_dot, rd_len) == 0 and
                    util.streq(qn + klen - ds_len, &dot_suffix))
                {
                    return resolved(qn, "import_map_suffix", CONF_IMPORT_MAP_SUFFIX, REG_RESOLVED);
                }
            }
        }
    }
    return emptyResult();
}

/// Strategy 2: Same-module match
fn resolveSameModule(r: *const Registry, callee_name: CStr, suffix: CStr, module_qn: CStr) Resolution {
    var candidate: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(&candidate, candidate.len, "%s.%s", module_qn, callee_name);
    var stored_key: CStr = c.cbm_ht_get_key(r.exact, &candidate);
    if (stored_key != null) {
        return resolved(stored_key, "same_module", CONF_SAME_MODULE, REG_RESOLVED);
    }
    if (suffix != null and suffix[0] != 0) {
        _ = c.snprintf(&candidate, candidate.len, "%s.%s", module_qn, suffix);
        stored_key = c.cbm_ht_get_key(r.exact, &candidate);
        if (stored_key != null) {
            return resolved(stored_key, "same_module", CONF_SAME_MODULE, REG_RESOLVED);
        }
    }
    return emptyResult();
}

/// Strategy 4: multiple candidates with import filtering.
fn resolveMultiWithImports(
    arr: *const QnArray,
    module_qn: CStr,
    import_vals: [*c]const CStr,
    import_count: c_int,
) Resolution {
    var filtered: [c.CBM_SZ_256]CStr = undefined;
    var fcount: c_int = 0;
    var i: usize = 0;
    while (i < arr.count and fcount < c.CBM_SZ_256) : (i += 1) {
        if (isImportReachable(arr.items[i], import_vals, import_count)) {
            filtered[ix(fcount)] = arr.items[i];
            fcount += 1;
        }
    }
    if (fcount == 1) {
        const conf = candidateCountPenalty(CONF_SUFFIX_MATCH, arr.count);
        return resolved(filtered[0], "suffix_match", conf, arr.count);
    }
    if (fcount > 1) {
        const best = bestByImportDistance(&filtered, fcount, module_qn);
        if (best != null) {
            const conf = candidateCountPenalty(CONF_SUFFIX_MATCH, fcount);
            return resolved(best, "suffix_match", conf, fcount);
        }
    }
    // No import-reachable — use all candidates with penalty
    const best = bestByImportDistance(@ptrCast(arr.items), arr.count, module_qn);
    if (best != null) {
        const conf = candidateCountPenalty(CONF_SUFFIX_MATCH * REG_HALF_PENALTY, arr.count);
        return resolved(best, "suffix_match", conf, arr.count);
    }
    return emptyResult();
}

/// Confidence for a full qualified-tail match (Strategy 3.5). A package- or
/// namespace-qualified callee that uniquely matches one candidate's full tail is
/// as trustworthy as a same-module hit.
const CONF_QUALIFIED_SUFFIX: f64 = 0.90;

/// Copy callee_name into dotted with every "::" turned into ".", so the tail
/// composes with dotted candidate QNs. Returns the length written.
fn normalizeScopeSeparators(callee_name: CStr, dotted: *[c.CBM_SZ_512]u8) usize {
    var w: usize = 0;
    var s: [*]const u8 = callee_name;
    while (s[0] != 0 and w + 1 < dotted.len) {
        if (s[0] == ':' and s[1] == ':') {
            dotted[w] = '.';
            s += 2;
        } else {
            dotted[w] = s[0];
            s += 1;
        }
        w += 1;
    }
    dotted[w] = 0;
    return w;
}

/// When a callee is package/namespace-qualified (Foo::Bar::sub or Foo.Bar.sub),
/// disambiguate among same-simple-name candidates by matching the FULL qualified
/// tail against each candidate QN at a segment boundary. Returns the sole
/// candidate whose QN equals or ends with ".<dotted-callee>", or NULL when zero
/// or several candidates match (the caller then falls back to bare-name scoring).
///
/// Fixes qualified cross-file calls collapsing onto one namespace when the bare
/// symbol name is defined in several — e.g. Perl's Foo::Bar::run and Foo::Baz::run
/// both reduce to "run", so the bare-name scorer would route every caller to a
/// single winner. Language agnostic: callees with no separator return NULL and
/// leave behavior unchanged.
fn qualifiedSuffixMatch(arr: *const QnArray, callee_name: CStr) CStr {
    // Normalize "::" → "." so the tail composes with dotted candidate QNs.
    var dotted: [c.CBM_SZ_512]u8 = undefined;
    const w = normalizeScopeSeparators(callee_name, &dotted);
    // Must be qualified (contain a '.') — a bare name matches every candidate
    // and carries no disambiguating signal.
    if (c.strchr(&dotted, '.') == null) return null;
    var match: CStr = null;
    for (0..ix(arr.count)) |i| {
        const qn: CStr = arr.items[i];
        const qlen = c.strlen(qn);
        if (qlen < w) continue;
        const tail = qn + (qlen - w);
        if (!util.streq(tail, &dotted)) continue;
        // Segment boundary: tail is the whole QN or is preceded by '.'.
        if (tail != qn and (tail - 1)[0] != '.') continue;
        if (match != null) return null; // ambiguous — more than one qualified tail matches
        match = qn;
    }
    return match;
}

/// A dotted callee whose FIRST segment starts upper-case names a type — URLSession,
/// Calendar, JSONEncoder. That receiver chain is evidence the bare-name scorers
/// throw away, and throwing it away binds Foundation's URLSession.shared.data to
/// a project's own PickedFile.data: high confidence, and nothing in the graph
/// shows it is wrong. Require instead that the candidate's own parent segment
/// appears somewhere in the chain. Calendar.utcGregorian.startOfDayUTC resolving
/// to AuthDTOs.Calendar.startOfDayUTC passes, because Calendar is in the chain.
///
/// Only an upper-case first segment is guarded. A lower-case root names a value
/// (vm.load, http.Get, os.path.join) whose declared type the chain does not
/// show, so the chain proves nothing there and the call passes through
/// unchanged. A callee with no separator passes through as well.
///
/// Language agnostic by design: the registry holds no language, and every
/// language that writes receiver chains gains the same protection.
fn receiverChainAdmits(callee_name: CStr, candidate_qn: CStr) bool {
    // Normalize "::" -> "." so the chain composes with dotted candidate QNs,
    // the same way qualifiedSuffixMatch does.
    var dotted: [c.CBM_SZ_512]u8 = undefined;
    _ = normalizeScopeSeparators(callee_name, &dotted);

    const last_dot: CStr = c.strrchr(&dotted, '.');
    if (last_dot == null) return true; // bare name — no receiver chain to judge
    if (dotted[0] < 'A' or dotted[0] > 'Z') return true; // lower-case root names a value, not a type
    // A name written in capitals with underscores is a constant holding a
    // value, not a type: ISO_4217_URL.lower is a string's own method. JSON and
    // URL carry no underscore and stay guarded.
    var has_underscore = false;
    var all_caps = true;
    var ch: [*]const u8 = &dotted;
    while (@intFromPtr(ch) < @intFromPtr(last_dot) and ch[0] != '.') : (ch += 1) {
        if (ch[0] == '_') {
            has_underscore = true;
        } else if (ch[0] >= 'a' and ch[0] <= 'z') {
            all_caps = false;
            break;
        }
    }
    if (all_caps and has_underscore) return true;

    // The candidate's parent segment: the one before its final name.
    const cand_last: CStr = c.strrchr(candidate_qn, '.');
    if (cand_last == null or cand_last == candidate_qn) {
        return true; // top-level candidate — no parent to look for
    }
    var parent: [*]const u8 = cand_last;
    while (@intFromPtr(parent) > @intFromPtr(candidate_qn) and (parent - 1)[0] != '.') {
        parent -= 1;
    }
    const parent_len = util.ptrDiff(cand_last, parent);

    // Walk the chain — every segment before the final callee name. A trailing
    // "()" is dropped so JSONEncoder().encode reads as JSONEncoder.
    var seg: [*]const u8 = &dotted;
    while (@intFromPtr(seg) < @intFromPtr(last_dot)) {
        const end: [*]const u8 = c.strchr(seg, '.');
        var len = util.ptrDiff(end, seg);
        if (len >= 2 and seg[len - 2] == '(' and seg[len - 1] == ')') {
            len -= 2; // an empty "()" — JSONEncoder().encode names JSONEncoder
        }
        if (len == parent_len and c.strncmp(seg, parent, parent_len) == 0) return true;
        seg = end + 1;
    }
    return false;
}

/// Strategy 3+4: Name lookup + suffix match
fn resolveNameLookup(
    r: *const Registry,
    callee_name: CStr,
    module_qn: CStr,
    import_vals: [*c]const CStr,
    import_count: c_int,
) Resolution {
    const lookup = simpleName(callee_name);
    const arr = byName(r, lookup) orelse return emptyResult();
    if (arr.count == 0) return emptyResult();
    if (arr.count > REG_MAX_CANDIDATES) {
        return emptyResult(); // unresolvably ambiguous — see REG_MAX_CANDIDATES
    }

    // Strategy 3.5: a qualified callee disambiguates among multiple same-name
    // candidates by full qualified tail, before bare-name scoring collapses
    // them onto a single winner.
    if (arr.count > 1) {
        const q = qualifiedSuffixMatch(arr, callee_name);
        if (q != null) {
            return resolved(q, "qualified_suffix", CONF_QUALIFIED_SUFFIX, REG_RESOLVED);
        }
    }

    // Strategy 3: unique name
    if (arr.count == 1) {
        if (!receiverChainAdmits(callee_name, arr.items[0])) return emptyResult();
        var conf: f64 = CONF_UNIQUE_NAME;
        if (import_vals != null and import_count > 0 and
            !isImportReachable(arr.items[0], import_vals, import_count))
        {
            conf *= DEFAULT_CONFIDENCE;
        }
        return resolved(arr.items[0], "unique_name", conf, REG_RESOLVED);
    }

    // Strategy 4: multiple candidates
    if (import_vals != null and import_count > 0) {
        return resolveMultiWithImports(arr, module_qn, import_vals, import_count);
    }
    const best = bestByImportDistance(@ptrCast(arr.items), arr.count, module_qn);
    if (best != null) {
        if (!receiverChainAdmits(callee_name, best)) return emptyResult();
        const conf = candidateCountPenalty(CONF_SUFFIX_MATCH, arr.count);
        return resolved(best, "suffix_match", conf, arr.count);
    }
    return emptyResult();
}

pub export fn cbm_registry_resolve(
    r_c: ?*const Registry,
    callee_name: CStr,
    module_qn: CStr,
    import_map_keys: [*c]CStr,
    import_map_vals: [*c]CStr,
    import_map_count: c_int,
) Resolution {
    const r = r_c orelse return emptyResult();
    if (callee_name == null) return emptyResult();

    // Per-file cache: same callee_name in N call sites → 1 chain walk
    // + N-1 O(1) hash hits. module_qn is constant per file so the
    // cache key only needs callee_name.
    if (resolve_cache) |cache| {
        const cached: ?*ResolveCacheEntry = @ptrCast(@alignCast(c.cbm_ht_get(cache, callee_name)));
        if (cached) |entry| return entry.res;
    }

    // Split callee at the first path separator: "pkg.Func" → prefix="pkg",
    // suffix="Func".  Rust/C++ use "::" rather than ".", so honor whichever
    // separator appears first ("lib::square" → prefix="lib", suffix="square").
    // Both separators are unambiguous, so handling "::" never affects "."-only
    // callees.
    var prefix: [c.CBM_SZ_256]u8 = @splat(0);
    var suffix: CStr = null;
    const dot: CStr = c.strchr(callee_name, '.');
    const colons: CStr = c.strstr(callee_name, "::");
    var sep: CStr = dot;
    var sep_len: usize = 1; // length of '.'
    if (colons != null and (sep == null or @intFromPtr(colons) < @intFromPtr(sep))) {
        sep = colons;
        sep_len = 2; // length of "::"
    }
    if (sep != null) {
        var plen = util.ptrDiff(sep, callee_name);
        if (plen >= prefix.len) plen = prefix.len - 1;
        @memcpy(prefix[0..plen], callee_name[0..plen]);
        prefix[plen] = 0;
        suffix = sep + sep_len;
    } else {
        _ = c.snprintf(&prefix, prefix.len, "%s", callee_name);
    }

    // Strategy 1: import map
    var res = resolveImportMap(r, &prefix, suffix, import_map_keys, import_map_vals, import_map_count);
    if (!(res.qualified_name != null and res.qualified_name[0] != 0)) {
        // Strategy 2: same module
        res = resolveSameModule(r, callee_name, suffix, module_qn);
    }
    if (!(res.qualified_name != null and res.qualified_name[0] != 0)) {
        // Strategy 3+4: name lookup
        res = resolveNameLookup(r, callee_name, module_qn, import_map_vals, import_map_count);
    }

    // Cache the result (including empty — caching the negative answer
    // is just as valuable; same name asks the same question).
    if (resolve_cache) |cache| {
        const entry: ?*ResolveCacheEntry = interop.mallocN(ResolveCacheEntry, 1);
        if (entry) |e| {
            e.res = res;
            const kdup: MutStr = c.strdup(callee_name);
            if (kdup != null) {
                _ = c.cbm_ht_set(cache, kdup, e);
            } else {
                c.free(e);
            }
        }
    }
    return res;
}

// ── Fuzzy Resolve ───────────────────────────────────────────────────

/// Filter candidates by import reachability. Returns count of reachable.
fn filterImportReachable(
    candidates: [*c]const CStr,
    count: c_int,
    import_vals: [*c]const CStr,
    import_count: c_int,
    out: []CStr,
) c_int {
    var n: usize = 0;
    var i: usize = 0;
    while (i < count and n < out.len) : (i += 1) {
        if (isImportReachable(candidates[i], import_vals, import_count)) {
            out[n] = candidates[i];
            n += 1;
        }
    }
    return @intCast(n);
}

fn fuzzy(qn: CStr, confidence: f64, candidate_count: c_int) c.cbm_fuzzy_result_t {
    return .{ .result = resolved(qn, "fuzzy", confidence, candidate_count), .ok = true };
}

pub export fn cbm_registry_fuzzy_resolve(
    r_c: ?*const Registry,
    callee_name: CStr,
    module_qn: CStr,
    import_map_keys: [*c]CStr,
    import_map_vals: [*c]CStr,
    import_map_count: c_int,
) c.cbm_fuzzy_result_t {
    _ = import_map_keys;
    const no_match: c.cbm_fuzzy_result_t = .{ .result = emptyResult(), .ok = false };
    const r = r_c orelse return no_match;
    if (callee_name == null) return no_match;

    // Extract simple name (last segment after dots)
    const lookup = simpleName(callee_name);
    const arr = byName(r, lookup) orelse return no_match;
    if (arr.count == 0) return no_match;
    if (arr.count > REG_MAX_CANDIDATES) {
        return no_match; // unresolvably ambiguous — see REG_MAX_CANDIDATES
    }

    const have_imports = import_map_vals != null and import_map_count > 0;

    // Single candidate
    if (arr.count == 1) {
        var conf: f64 = CONF_FUZZY_SINGLE;
        if (have_imports and !isImportReachable(arr.items[0], import_map_vals, import_map_count)) {
            conf *= DEFAULT_CONFIDENCE;
        }
        return fuzzy(arr.items[0], conf, REG_RESOLVED);
    }

    // Multiple candidates: filter by import reachability
    var filtered: [c.CBM_SZ_256]CStr = undefined;
    var fcount = arr.count;
    var fptr: [*c]const CStr = @ptrCast(arr.items);

    if (have_imports) {
        fcount = filterImportReachable(@ptrCast(arr.items), arr.count, import_map_vals, import_map_count, &filtered);
        fptr = &filtered;
    }

    if (fcount == 0) {
        // No import-reachable — use originals with penalty
        const best = bestByImportDistance(@ptrCast(arr.items), arr.count, module_qn);
        if (best == null) return no_match;
        return fuzzy(best, candidateCountPenalty(CONF_FUZZY_MULTI * REG_HALF_PENALTY, arr.count), arr.count);
    }
    if (fcount == 1) {
        return fuzzy(fptr[0], candidateCountPenalty(CONF_FUZZY_SINGLE, arr.count), arr.count);
    }
    const best = bestByImportDistance(fptr, fcount, module_qn);
    if (best == null) return no_match;
    return fuzzy(best, candidateCountPenalty(CONF_FUZZY_MULTI, fcount), fcount);
}

// ── FindEndingWith ──────────────────────────────────────────────────

const FewCtx = struct {
    target: CStr, // ".suffix"
    target_len: usize,
    results: [*c]CStr,
    count: c_int,
    cap: c_int,
};

fn fewScan(key: CStr, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = value;
    const ctx: *FewCtx = @ptrCast(@alignCast(ud));
    const klen = c.strlen(key);
    if (klen >= ctx.target_len and util.streq(key + klen - ctx.target_len, ctx.target)) {
        if (ctx.count >= ctx.cap) {
            ctx.cap = if (ctx.cap != 0) ctx.cap * 2 else REG_INIT_CAP;
            ctx.results = interop.safeReallocN(CStr, ctx.results, ix(ctx.cap));
        }
        ctx.results[ix(ctx.count)] = key;
        ctx.count += 1;
    }
}

pub export fn cbm_registry_find_ending_with(r_c: ?*const Registry, suffix: CStr, out_c: ?*[*c]CStr) c_int {
    const out = out_c orelse return 0;
    const r = r_c orelse {
        out.* = null;
        return 0;
    };
    if (suffix == null) {
        out.* = null;
        return 0;
    }

    // Build ".suffix" target
    const slen = c.strlen(suffix);
    const target: MutStr = @ptrCast(c.malloc(slen + REG_SUFFIX_ALLOC));
    target[0] = '.';
    @memcpy(target[1 .. slen + 2], suffix[0 .. slen + 1]);

    var ctx: FewCtx = .{ .target = target, .target_len = slen + 1, .results = null, .count = 0, .cap = 0 };
    c.cbm_ht_foreach(r.exact, fewScan, &ctx);

    c.free(target);
    out.* = ctx.results;
    return ctx.count;
}

/// Public wrapper for isImportReachable (testing).
pub export fn cbm_registry_is_import_reachable(candidate_qn: CStr, import_vals: [*c]CStr, import_count: c_int) bool {
    return isImportReachable(candidate_qn, import_vals, import_count);
}

// ── Class-qualified route handlers (PHP, #1146) ─────────────────────

/// Method QNs are path-based: <project>.<dirs>.<file stem>.<Class>.<method>.
/// A PHP class is named by its namespace instead, so a "Ns\\Class::method"
/// handler is placed on a QN by what places the class file itself.
const CONF_PSR4_MEMBER: f64 = 0.95;
const CONF_NAMESPACE_PATH_MEMBER: f64 = 0.90;

fn segEqNocase(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| {
        const ca = if (x >= 'A' and x <= 'Z') x - 'A' + 'a' else x;
        const cb = if (y >= 'A' and y <= 'Z') y - 'A' + 'a' else y;
        if (ca != cb) return false;
    }
    return true;
}

/// Does qn end with ".<cls>.<member>"? Returns the offset of that tail or -1.
fn qnClassMemberTail(qn: CStr, cls: [*]const u8, cls_len: usize, member: CStr) c_int {
    const qlen = c.strlen(qn);
    const mlen = c.strlen(member);
    if (qlen < cls_len + mlen + 2) return c.CBM_NOT_FOUND;
    const m: [*]const u8 = qn + qlen - mlen;
    const cl: [*]const u8 = m - 1 - cls_len;
    if (!util.streq(m, member) or (m - 1)[0] != '.' or c.strncmp(cl, cls, cls_len) != 0 or
        (cl - 1)[0] != '.')
    {
        return c.CBM_NOT_FOUND;
    }
    return @intCast(util.ptrDiff(cl - 1, qn));
}

/// PSR-4: the class file Composer's autoloader loads for the FQN, the class it
/// defines, and that class's member — through the PSR-4 import resolver
/// (cbm_pipeline_psr4_member_qn), so a root whose folder does not mirror its
/// namespace (Acme\\Blog\\ -> packages/blog/src) places the class as well.
/// Returns its tri-state outcome; out_qn is the member QN when RESOLVED.
fn psr4MemberQn(
    gbuf: ?*const c.cbm_gbuf_t,
    fqn: [*]const u8,
    fqn_len: usize,
    member: CStr,
    out_qn: *CStr,
) c.cbm_psr4_member_t {
    var cls_fqn: [c.CBM_SZ_512]u8 = undefined;
    out_qn.* = null;
    if (gbuf == null or fqn_len >= cls_fqn.len) return c.CBM_PSR4_MEMBER_NOT_COVERED;
    @memcpy(cls_fqn[0..fqn_len], fqn[0..fqn_len]);
    cls_fqn[fqn_len] = 0;
    return c.cbm_pipeline_psr4_member_qn(gbuf, &cls_fqn, member, out_qn);
}

/// Do the directory segments of qn (between the project root segment and the
/// file stem, i.e. before offset tail = ".<Class>.<member>") end with every
/// segment of the namespace fqn[0, ns_len)? Case-insensitive: App\\Http is
/// conventionally app/Http. An empty namespace is trivially aligned.
fn namespaceMatchesDirs(qn: CStr, tail: c_int, fqn: [*]const u8, ns_len: usize) bool {
    const root_dot: CStr = c.strchr(qn, '.');
    const stem_end: [*]const u8 = qn + ix(tail);
    if (root_dot == null or @intFromPtr(stem_end) <= @intFromPtr(root_dot)) return false;
    var dir_end: [*]const u8 = stem_end - 1; // back over the file stem
    while (dir_end[0] != '.') dir_end -= 1;
    var ns_end = ns_len;
    while (ns_end > 0) {
        var ns_start = ns_end;
        while (ns_start > 0 and fqn[ns_start - 1] != '\\') ns_start -= 1;
        if (@intFromPtr(dir_end) <= @intFromPtr(root_dot)) {
            return false; // more namespace segments than directories
        }
        var dir_start = dir_end;
        while ((dir_start - 1)[0] != '.') dir_start -= 1;
        if (!segEqNocase(fqn[ns_start..ns_end], dir_start[0..util.ptrDiff(dir_end, dir_start)])) {
            return false;
        }
        dir_end = dir_start - 1;
        ns_end = if (ns_start > 0) ns_start - 1 else 0;
    }
    return true;
}

/// Namespace/path alignment: the one "<Class>.<member>" candidate that sits
/// where its namespace says. NULL when none or several do.
fn alignedMemberQn(
    r: *const Registry,
    fqn: [*]const u8,
    ns_len: usize,
    cls: [*]const u8,
    cls_len: usize,
    member: CStr,
) CStr {
    const arr = byName(r, member) orelse return null;
    var match: CStr = null;
    for (0..ix(arr.count)) |i| {
        const qn: CStr = arr.items[i];
        const tail = qnClassMemberTail(qn, cls, cls_len, member);
        if (tail == c.CBM_NOT_FOUND or !namespaceMatchesDirs(qn, tail, fqn, ns_len)) continue;
        if (match != null) return null; // ambiguous
        match = qn;
    }
    return match;
}

pub export fn cbm_registry_resolve_handler(
    r_c: ?*const Registry,
    handler_ref: CStr,
    module_qn: CStr,
    import_map_keys: [*c]CStr,
    import_map_vals: [*c]CStr,
    import_map_count: c_int,
    gbuf: ?*const c.cbm_gbuf_t,
) Resolution {
    const sep: CStr = if (handler_ref != null) c.strstr(handler_ref, "::") else null;
    const r = r_c orelse return cbm_registry_resolve(r_c, handler_ref, module_qn, import_map_keys, import_map_vals, import_map_count);
    if (sep == null) {
        return cbm_registry_resolve(r, handler_ref, module_qn, import_map_keys, import_map_vals, import_map_count);
    }
    const member: CStr = sep + 2;
    const fqn_len = util.ptrDiff(sep, handler_ref);
    var ns_len = fqn_len;
    while (ns_len > 0 and handler_ref[ns_len - 1] != '\\') ns_len -= 1;
    const cls: [*]const u8 = handler_ref + ns_len;
    const cls_len = fqn_len - ns_len;
    ns_len = if (ns_len > 0) ns_len - 1 else 0; // drop the separator before the class
    if (cls_len == 0 or member[0] == 0) return emptyResult();
    var qn: CStr = null;
    switch (psr4MemberQn(gbuf, handler_ref, fqn_len, member, &qn)) {
        c.CBM_PSR4_MEMBER_RESOLVED => return resolved(qn, "php_psr4", CONF_PSR4_MEMBER, REG_RESOLVED),
        // A covered class lives where PSR-4 says or nowhere; a namespace-
        // mirroring folder elsewhere would be a guessed edge (#1186).
        c.CBM_PSR4_MEMBER_UNRESOLVED => return emptyResult(),
        else => {},
    }
    qn = alignedMemberQn(r, handler_ref, ns_len, cls, cls_len, member);
    if (qn != null) {
        return resolved(qn, "php_namespace_path", CONF_NAMESPACE_PATH_MEMBER, REG_RESOLVED);
    }
    return emptyResult();
}
