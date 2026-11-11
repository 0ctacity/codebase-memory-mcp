//! pass_parallel.zig — Three-phase parallel pipeline.
//!
//! Phase 3A: Parallel extract + create definition nodes (per-worker gbufs)
//! Phase 3B: Serial registry build + edge creation from cached results
//! Phase 4:  Parallel call/usage/semantic resolution (per-worker edge bufs)
//!
//! Each file is read and parsed ONCE (Phase 3A). The CBMFileResult is cached
//! and reused for resolution (Phase 4), eliminating 3x redundant I/O + parsing.
//!
//! Depends on: worker_pool, graph_buffer (shared IDs + merge), extraction (cbm.h)

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");
const lsp_resolve = @import("lsp_resolve.zig");
const spawns = @import("spawns.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const ptrDiff = util.ptrDiff;
const HashTable = c.CBMHashTable;
const Arena = c.CBMArena;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;
const Ctx = c.cbm_pipeline_ctx_t;
const FileInfo = c.cbm_file_info_t;
const FileError = c.cbm_file_error_t;
const FileResult = c.CBMFileResult;
const Definition = c.CBMDefinition;
const LspDef = c.CBMLSPDef;
const Registry = c.cbm_registry_t;
const Language = c.CBMLanguage;
const Spill = c.cbm_result_spill_t;
const Timespec = c.struct_timespec;
/// A NULL-terminated or counted array of borrowed C strings.
const StrList = [*c]CStr;

/// int fields of ResolveWorkerState
const PP_RESOLVE_COUNTERS = 5;
const PP_JSON_MARGIN = 10;
const PP_ESC_MARGIN = 3;
const PP_ESC_SPACE = 2;
/// Fixed bytes around a serialized JSON field: ,"key":"value" / ,"key":[...]
/// -> comma + 2 key quotes + colon + 2 value quotes (resp. brackets).
const PP_JSON_FIELD_OVERHEAD = 6;
const PP_ARGS_MARGIN = 20;
/// ,"line":<int> -> comma + key (7) + colon + up to 10 digits + NUL.
const PP_LINE_MARGIN = 24;
const PP_LOG_THRESH = 24;
const PP_LOG_INTERVAL = 10;
const PP_TIMER_THRESH = 1000;
/// Extraction memory back-pressure: when the process is over its RSS budget,
/// a worker reclaims + naps before pulling another file so peers can finish
/// and return pages. Bounded spins avoid deadlock when the resident graph
/// itself is near budget: then ONE confirmation cycle decides — drained
/// means continue, still over means the attempt fails whole (upstream
/// decision A, #1997 #832) instead of the former open-ended soft overshoot.
const PP_BACKPRESSURE_MAX_SPINS = 40;
const PP_BACKPRESSURE_NAP_NS = 3000000; // 3 ms
const PP_NSEC_PER_SEC: u64 = 1000000000;
const PP_USEC_PER_MS: u64 = 1000000;
const PP_HALF_CONF = 0.5;
const PP_FIELD_HINT_CONF = 0.85;
const PP_CSHARP_M_PREFIX_LEN = 2;

// Absolute source-retention ceilings for the parallel extract pipeline.
//
// The extract worker copies each file's source bytes into result->arena so
// the fused cross-file LSP step in resolve_worker can re-parse without
// re-opening the file. That retention is TRANSIENT (freed at run end) but it
// is a PEAK-RSS driver: every retained byte is resident at once across the
// extract→resolve handoff.
//
// A cap here is a FLOOR as much as a ceiling: whatever total we allow, we
// WILL hold that much resident at peak on a large repo. rust-analyzer bounds
// retained file *text* to a small fixed budget and re-reads source on a miss
// rather than scaling the retained set with host RAM — because the re-read is
// cheap relative to holding tens of GB resident. We follow the same model:
//   - derive the total budget from the process memory budget (budget/8),
//   - BUT clamp the RAM-derived DEFAULT to a small absolute ceiling (1 GiB)
//     so a 512 GiB host does not retain 64 GiB of source it would re-read
//     cheaply anyway, and keep the per-file cap modest (32 MiB) so one
//     pathological generated blob cannot monopolise the budget.
// A file dropped from retention is NOT lost to cross-file resolution:
// resolve_worker re-reads it on demand, bounded and freed immediately
// (source_reread fallback). Both caps are env-overridable — CBM_RETAIN_TOTAL_MB
// / CBM_RETAIN_PER_FILE_MB raise or lower the auto-derived defaults directly
// (the hard ceilings bound only the RAM-derived default, never a deliberate
// operator/caller choice).
const PP_RETAIN_TOTAL_HARD_MAX_BYTES: usize = 1024 * 1024 * 1024; // 1 GiB default ceiling
const PP_RETAIN_PER_FILE_HARD_MAX_BYTES: usize = 32 * 1024 * 1024; // 32 MiB per file

/// Back-pressure nap-cycle counter (test observability): each execution of the
/// over-budget collect+nap gate counts one cycle. Lets tests assert the gate does
/// not re-pay the full nap tax on every file pull when napping cannot reclaim
/// memory (the resident floor, not in-flight transients, holds the budget).
var g_bp_nap_cycles: std.atomic.Value(c_long) = .init(0);

pub export fn cbm_pp_bp_nap_cycles() c_long {
    return g_bp_nap_cycles.load(.monotonic);
}

pub export fn cbm_pp_bp_nap_cycles_reset() void {
    g_bp_nap_cycles.store(0, .monotonic);
}

/// Test seam: when set, a parallel extract with a spill store spills from its
/// first file instead of waiting for the budget signal.
var g_force_spill: std.atomic.Value(c_int) = .init(0);
/// Results parked by the parallel extract (test observability).
var g_spill_parked: std.atomic.Value(c_long) = .init(0);

pub export fn cbm_parallel_force_spill_for_test(on: c_int) void {
    g_force_spill.store(@intFromBool(on != 0), .monotonic);
}

pub export fn cbm_pp_spill_parked() c_long {
    return g_spill_parked.load(.monotonic);
}

pub export fn cbm_pp_spill_parked_reset() void {
    g_spill_parked.store(0, .monotonic);
}

fn logError(msg: interop.Str, kvs: anytype) void {
    interop.log(c.CBM_LOG_ERROR, msg, kvs);
}

fn itoa(v: c_int) CStr {
    return c.cbm_log_itoa(v);
}

/// "true" or "false", as the properties JSON spells a bool.
fn boolText(b: bool) CStr {
    return if (b) "true" else "false";
}

/// Parse a positive MB-valued retention env knob (CBM_RETAIN_*_MB) into bytes.
/// Follows the limits.c strtol convention: unset / unparseable / non-positive
/// → return 0 so the caller keeps its derived default.
fn cbmRetainEnvBytes(name: CStr) usize {
    const raw: CStr = c.getenv(name);
    if (raw == null or raw[0] == 0) return 0;
    std.c._errno().* = 0;
    var end: MutStr = null;
    const v = c.strtol(raw, &end, 10);
    if (std.c._errno().* != 0 or end == raw or end[0] != 0 or v <= 0) return 0;
    return @as(usize, @intCast(v)) *% 1024 *% 1024;
}

/// Auto-derived TOTAL retention budget: a fraction of the process memory
/// budget, clamped to the absolute ceiling. The clamp bounds ONLY this
/// RAM-derived default — a huge-RAM host must not retain tens of GB it would
/// re-read cheaply. When the budget is unset (tests / no cgroup) fall back to
/// the ceiling.
fn cbmParallelExtractDefaultTotalCap() usize {
    var cap: usize = c.cbm_mem_budget();
    if (cap > 0) {
        cap /= 8;
    } else {
        cap = PP_RETAIN_TOTAL_HARD_MAX_BYTES;
    }
    if (cap > PP_RETAIN_TOTAL_HARD_MAX_BYTES) cap = PP_RETAIN_TOTAL_HARD_MAX_BYTES;
    return cap;
}

/// Auto-derived PER-FILE cap: modest absolute ceiling, never above the total.
fn cbmParallelExtractDefaultPerFileCap(total_cap: usize) usize {
    var cap: usize = PP_RETAIN_PER_FILE_HARD_MAX_BYTES;
    if (cap > total_cap) cap = total_cap;
    return cap;
}

/// Resolve the effective retention options from (in precedence order):
/// explicit caller opts > CBM_RETAIN_*_MB env knobs > RAM-derived defaults.
/// The absolute hard ceilings apply only to the RAM-derived defaults; an
/// operator/caller that sets a value explicitly is trusted. The only invariant
/// enforced unconditionally is per-file ≤ total.
fn cbmParallelExtractResolveOpts(opts: ?*const c.cbm_parallel_extract_opts_t) c.cbm_parallel_extract_opts_t {
    var resolved = std.mem.zeroes(c.cbm_parallel_extract_opts_t);
    resolved.retain_sources = true;
    resolved.retain_sources_set = true;
    resolved.retain_total_budget_bytes = cbmParallelExtractDefaultTotalCap();
    resolved.retain_per_file_max_bytes = 0;

    const env_total = cbmRetainEnvBytes("CBM_RETAIN_TOTAL_MB");
    if (env_total > 0) resolved.retain_total_budget_bytes = env_total;

    resolved.retain_per_file_max_bytes =
        cbmParallelExtractDefaultPerFileCap(resolved.retain_total_budget_bytes);
    const env_per_file = cbmRetainEnvBytes("CBM_RETAIN_PER_FILE_MB");
    if (env_per_file > 0) resolved.retain_per_file_max_bytes = env_per_file;

    if (opts) |o| {
        if (o.retain_sources_set) resolved.retain_sources = o.retain_sources;
        if (o.retain_total_budget_bytes > 0) {
            resolved.retain_total_budget_bytes = o.retain_total_budget_bytes;
        }
        if (o.retain_per_file_max_bytes > 0) {
            resolved.retain_per_file_max_bytes = o.retain_per_file_max_bytes;
        }
    }

    // Correctness invariant: a single file can never exceed the total budget.
    if (resolved.retain_per_file_max_bytes > resolved.retain_total_budget_bytes) {
        resolved.retain_per_file_max_bytes = resolved.retain_total_budget_bytes;
    }
    return resolved;
}

fn extractNowNs() u64 {
    var ts: Timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &ts);
    return (@as(u64, @bitCast(@as(i64, ts.tv_sec))) *% PP_NSEC_PER_SEC) +%
        @as(u64, @bitCast(@as(i64, ts.tv_nsec)));
}

// ── Helpers (duplicated from pass files — kept static for isolation) ──

/// Read file into a malloc'd buffer (= mimalloc in production).
/// *out_size receives the on-disk size and *out_status the failure reason so the
/// caller can attribute a skip to the right phase (read vs oversized) instead of
/// a silent drop. Both out params may be NULL.
fn readFile(path: CStr, out_len: *c_int, out_size: ?*c_long, out_status: ?*c.cbm_read_status_t) MutStr {
    if (out_size) |s| s.* = 0;
    if (out_status) |s| s.* = c.CBM_READ_OK;
    const f = c.cbm_fopen(path, "rb") orelse {
        if (out_status) |s| s.* = c.CBM_READ_OPEN_FAIL;
        return null;
    };
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    if (out_size) |s| s.* = size;
    if (size <= 0) {
        _ = c.fclose(f);
        if (out_status) |s| s.* = c.CBM_READ_EMPTY;
        return null;
    }
    if (size > c.cbm_max_file_bytes()) { // generous, env-configurable cap (B4)
        _ = c.fclose(f);
        if (out_status) |s| s.* = c.CBM_READ_OVERSIZED;
        return null;
    }
    // +pad: tree-sitter lexer lookahead reads past EOF; keep it in-bounds
    const CBM_TS_LOOKAHEAD_PAD = 16;
    const usize_size: usize = @intCast(size);
    const buf: MutStr = @ptrCast(c.malloc(usize_size + CBM_TS_LOOKAHEAD_PAD));
    if (buf == null) {
        _ = c.fclose(f);
        if (out_status) |s| s.* = c.CBM_READ_OOM;
        return null;
    }
    var nread = c.fread(buf, 1, usize_size, f);
    _ = c.fclose(f);
    if (nread > usize_size) nread = usize_size;
    @memset(buf[nread .. nread + CBM_TS_LOOKAHEAD_PAD], 0);
    out_len.* = @intCast(nread);
    return buf;
}

// ── Per-worker skip list (Stage 2 / Track B) ────────────────────────
// Each extract worker appends read/extract/oversized skips into its OWN list
// (no lock on the hot path); the lists are merged into the pipeline's
// cbm_file_error_t array in the existing sequential merge loop.
const PpErrList = extern struct {
    items: [*c]FileError = null,
    count: c_int = 0,
    cap: c_int = 0,
};

/// NULL-safe heap strdup.
fn ppErrDup(s: CStr) MutStr {
    if (s == null) return null;
    const n = c.strlen(s) + 1;
    const d: MutStr = @ptrCast(c.malloc(n));
    if (d != null) @memcpy(d[0..n], s[0..n]);
    return d;
}

fn ppErrAdd(list_c: ?*PpErrList, path: CStr, reason: CStr, phase: CStr) void {
    const list = list_c orelse return;
    if (list.count >= list.cap) {
        const ncap: c_int = if (list.cap != 0) list.cap * 2 else 8;
        const grown = interop.reallocN(FileError, list.items, ix(ncap));
        if (grown == null) return; // drop on OOM — never fail extraction to record a skip
        list.items = grown;
        list.cap = ncap;
    }
    const e = util.at(list.items, ix(list.count));
    e.path = ppErrDup(path);
    e.reason = ppErrDup(reason);
    e.phase = ppErrDup(phase);
    list.count += 1;
}

/// Free source buffer.
fn freeSource(buf: MutStr) void {
    c.free(buf);
}

// Append a JSON-escaped string value to buf at position *pos.
/// Escape one character for JSON. Returns bytes written (1 or 2).
fn jsonEscapeChar(buf: [*]u8, avail: usize, ch: u8) usize {
    const esc: u8 = switch (ch) {
        '"' => '"',
        '\\' => '\\',
        '\n' => 'n',
        '\r' => 'r',
        '\t' => 't',
        else => {
            // Any other raw control byte (e.g. form feed) is invalid inside a
            // JSON string — degrade to a space.
            if (avail >= 1) buf[0] = if (ch < 0x20) ' ' else ch;
            return 1;
        },
    };
    if (avail >= PP_ESC_SPACE) {
        buf[0] = '\\';
        buf[1] = esc;
    }
    return PP_ESC_SPACE;
}

/// Escaped length of a string under jsonEscapeChar's rules: escaped
/// characters expand to 2 bytes, everything else stays 1.
fn ppJsonEscapedLen(s_in: CStr) usize {
    var n: usize = 0;
    var s = s_in;
    while (s[0] != 0) : (s += 1) {
        n += switch (s[0]) {
            '"', '\\', '\n', '\r', '\t' => PP_ESC_SPACE,
            else => 1,
        };
    }
    return n;
}

/// Appends are ATOMIC: a field is emitted only if the WHOLE serialized form
/// fits (with PP_ESC_SPACE bytes reserved for the closing '}' + NUL). Cutting a
/// field mid-value produced unterminated strings/arrays — malformed properties
/// JSON that aborts every json_extract()-based consumer downstream (seen on the
/// Linux kernel: 50-param functions truncated at the 2 KB cap). Dropping an
/// oversized optional field whole keeps the JSON valid.
fn appendJsonString(buf: [*]u8, bufsize: usize, pos: *usize, key: CStr, val: CStr) void {
    if (val == null or val[0] == 0) return;
    const required = c.strlen(key) + ppJsonEscapedLen(val) + PP_JSON_FIELD_OVERHEAD;
    if (pos.* + required + PP_ESC_SPACE > bufsize) {
        return; // whole field would not fit — skip it atomically
    }
    var p = pos.*;
    const w = c.snprintf(buf + p, bufsize - p, ",\"%s\":\"", key);
    if (w <= 0 or ix(w) >= bufsize - p) return;
    p += ix(w);
    var s = val;
    while (s[0] != 0 and p < bufsize - PP_ESC_MARGIN) : (s += 1) {
        p += jsonEscapeChar(buf + p, bufsize - p - PP_ESC_SPACE, s[0]);
    }
    if (p < bufsize - 1) {
        buf[p] = '"';
        p += 1;
    }
    buf[p] = 0;
    pos.* = p;
}

/// Append a JSON array of strings: ,"key":["a","b","c"]. Atomic like
/// appendJsonString: emitted only if the whole array fits.
fn appendJsonStrArray(buf: [*]u8, bufsize: usize, pos: *usize, key: CStr, arr: [*c]const CStr) void {
    if (arr == null or arr[0] == null or pos.* >= bufsize - PP_JSON_MARGIN) return;
    // ,"key":[ + per item "<escaped>" + separating commas + ]
    var required = c.strlen(key) + PP_JSON_FIELD_OVERHEAD;
    var i: usize = 0;
    while (arr[i] != null) : (i += 1) {
        required += ppJsonEscapedLen(arr[i]) + PP_ESC_SPACE + @intFromBool(i > 0);
    }
    if (pos.* + required + PP_ESC_SPACE > bufsize) {
        return; // whole array would not fit — skip it atomically
    }
    var p = pos.*;
    const n = c.snprintf(buf + p, bufsize - p, ",\"%s\":[", key);
    if (n <= 0 or p + ix(n) >= bufsize - PP_ESC_SPACE) return;
    p += ix(n);
    i = 0;
    while (arr[i] != null) : (i += 1) {
        if (i > 0 and p < bufsize - 1) {
            buf[p] = ',';
            p += 1;
        }
        if (p < bufsize - 1) {
            buf[p] = '"';
            p += 1;
        }
        // Full escaping (not just quote/backslash): items like C param types
        // sliced from multi-line declarations carry raw \n/\t bytes, which are
        // invalid inside JSON strings.
        var s: CStr = arr[i];
        while (s[0] != 0 and p < bufsize - PP_ESC_SPACE) : (s += 1) {
            p += jsonEscapeChar(buf + p, bufsize - p - PP_ESC_SPACE, s[0]);
        }
        if (p < bufsize - 1) {
            buf[p] = '"';
            p += 1;
        }
    }
    if (p < bufsize - 1) {
        buf[p] = ']';
        p += 1;
    }
    buf[p] = 0;
    pos.* = p;
}

/// Append an already-serialized JSON value verbatim: ,"key":<json>. Atomic like
/// appendJsonString. For `variants`, which extraction builds as a JSON array
/// of objects (cbm.h).
fn appendJsonRaw(buf: [*]u8, bufsize: usize, pos: *usize, key: CStr, json: CStr) void {
    if (json == null or json[0] == 0) return;
    const required = c.strlen(key) + c.strlen(json) + PP_JSON_FIELD_OVERHEAD;
    if (pos.* + required + PP_ESC_SPACE > bufsize) {
        return; // whole field would not fit — skip it atomically
    }
    const w = c.snprintf(buf + pos.*, bufsize - pos.*, ",\"%s\":%s", key, json);
    if (w > 0 and ix(w) < bufsize - pos.*) pos.* += ix(w);
}

fn buildDefProps(buf: [*]u8, bufsize: usize, def: *const Definition) void {
    // Complexity/loop/recursion metrics are meaningful only for Function/Method.
    // Gate the block so the millions of Macro/Field/Variable/Class/Enum nodes
    // keep a lean properties blob (lossless — those fields are always zero for
    // non-functions). Cuts RAM, gbuf-merge copy and dump volume.
    const is_fn = def.label != null and (streq(def.label, "Function") or streq(def.label, "Method"));
    const n = if (is_fn)
        c.snprintf(
            buf,
            bufsize,
            "{\"complexity\":%d,\"cognitive\":%d,\"loop_count\":%d,\"loop_depth\":%d," ++
                "\"self_recursive\":%s,\"param_count\":%d,\"max_access_depth\":%d," ++
                "\"linear_scan_in_loop\":%d,\"alloc_in_loop\":%d,\"recursion_in_loop\":%s," ++
                "\"unguarded_recursion\":%s," ++
                "\"lines\":%d,\"is_exported\":%s,\"is_test\":%s,\"is_entry_point\":%s",
            def.complexity,
            def.cognitive,
            def.loop_count,
            def.loop_depth,
            boolText(def.is_recursive),
            def.param_count,
            def.max_access_depth,
            def.linear_scan_in_loop,
            def.alloc_in_loop,
            boolText(def.recursion_in_loop),
            boolText(def.unguarded_recursion),
            def.lines,
            boolText(def.is_exported),
            boolText(def.is_test),
            boolText(def.is_entry_point),
        )
    else
        c.snprintf(
            buf,
            bufsize,
            "{\"complexity\":%d,\"lines\":%d,\"is_exported\":%s,\"is_test\":%s," ++
                "\"is_entry_point\":%s",
            def.complexity,
            def.lines,
            boolText(def.is_exported),
            boolText(def.is_test),
            boolText(def.is_entry_point),
        );
    if (n <= 0 or ix(n) >= bufsize) {
        buf[0] = 0;
        return;
    }
    var pos: usize = ix(n);
    appendJsonString(buf, bufsize, &pos, "docstring", def.docstring);
    // Right after the docstring, ahead of the bounded fields, so a long
    // signature cannot squeeze it out of the fixed buffer.
    appendJsonRaw(buf, bufsize, &pos, "variants", def.variants);
    appendJsonString(buf, bufsize, &pos, "signature", def.signature);
    appendJsonString(buf, bufsize, &pos, "return_type", def.return_type);
    appendJsonString(buf, bufsize, &pos, "parent_class", def.parent_class);
    appendJsonStrArray(buf, bufsize, &pos, "decorators", def.decorators);
    appendJsonStrArray(buf, bufsize, &pos, "base_classes", def.base_classes);
    appendJsonStrArray(buf, bufsize, &pos, "param_names", def.param_names);
    appendJsonStrArray(buf, bufsize, &pos, "param_types", def.param_types);
    appendJsonString(buf, bufsize, &pos, "route_path", def.route_path);
    appendJsonString(buf, bufsize, &pos, "route_method", def.route_method);
    appendJsonString(buf, bufsize, &pos, "http_client", def.http_client);
    appendJsonString(buf, bufsize, &pos, "http_base_url", def.http_base_url);

    // MinHash fingerprint — append if present and buffer has room.
    // Hex-encoded K=64 uint32 = 512 chars + key/quotes ≈ 520 chars.
    if (def.fingerprint != null and def.fingerprint_k > 0 and
        pos + c.CBM_MINHASH_HEX_LEN + c.CBM_MINHASH_JSON_OVERHEAD < bufsize)
    {
        var fp_hex: [c.CBM_MINHASH_HEX_BUF]u8 = undefined;
        c.cbm_minhash_to_hex(@ptrCast(@alignCast(def.fingerprint)), &fp_hex, fp_hex.len);
        appendJsonString(buf, bufsize, &pos, "fp", &fp_hex);
    }

    // AST structural profile — append if present and buffer has room.
    if (def.structural_profile != null and pos + c.CBM_AST_PROFILE_BUF < bufsize) {
        appendJsonString(buf, bufsize, &pos, "sp", def.structural_profile);
    }

    // Body tokens — raw identifiers from function body AST for semantic search.
    if (def.body_tokens != null and pos + c.CBM_SZ_512 < bufsize) {
        appendJsonString(buf, bufsize, &pos, "bt", def.body_tokens);
    }

    if (pos < bufsize - 1) {
        buf[pos] = '}';
        buf[pos + 1] = 0;
    }
}

/// Build import map from graph buffer IMPORTS edges (read-only access to gbuf).
fn buildImportMap(
    gbuf: ?*const Gbuf,
    project_name: CStr,
    rel_path: CStr,
    out_keys: *StrList,
    out_vals: *StrList,
    out_count: *c_int,
) c_int {
    out_keys.* = null;
    out_vals.* = null;
    out_count.* = 0;

    const file_qn: MutStr = c.cbm_pipeline_fqn_compute(project_name, rel_path, "__file__");
    const file_node: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, file_qn);
    c.free(file_qn);
    const fnode = file_node orelse return 0;

    var edges: [*c][*c]const Edge = null;
    var edge_count: c_int = 0;
    const rc = c.cbm_gbuf_find_edges_by_source_type(gbuf, fnode.id, "IMPORTS", &edges, &edge_count);
    if (rc != 0 or edge_count == 0) return 0;

    const keys = interop.callocN(CStr, ix(edge_count));
    const vals = interop.callocN(CStr, ix(edge_count));
    var count: usize = 0;

    const marker = "\"local_name\":\"";
    for (0..ix(edge_count)) |i| {
        const e: *const Edge = edges[i];
        const target: ?*const Node = c.cbm_gbuf_find_by_id(gbuf, e.target_id);
        if (target == null or e.properties_json == null) continue;
        var start: CStr = c.strstr(e.properties_json, marker);
        if (start != null) {
            start += marker.len;
            const end: CStr = c.strchr(start, '"');
            if (end != null and @intFromPtr(end) > @intFromPtr(start)) {
                keys[count] = c.cbm_strndup(start, ptrDiff(end, start));
                vals[count] = target.?.qualified_name;
                count += 1;
            }
        }
    }

    out_keys.* = keys;
    out_vals.* = vals;
    out_count.* = @intCast(count);
    return 0;
}

fn freeImportMap(keys: StrList, vals: StrList, count: c_int) void {
    if (keys != null) {
        for (0..ix(count)) |i| c.free(@constCast(keys[i]));
        c.free(@ptrCast(keys));
    }
    if (vals != null) c.free(@ptrCast(vals));
}

/// True for languages whose module QN derives from the CONTAINING DIRECTORY
/// (Java/Go package). MUST match cbm_lang_module_is_dir() (internal/cbm/helpers.c)
/// and pxcModuleIsDir() (pass_lsp_cross) so same-module callee resolution
/// keys against the directory-based def-node QNs in the registry.
fn ppModuleIsDir(lang: Language) bool {
    return lang == c.CBM_LANG_JAVA or lang == c.CBM_LANG_GO;
}

fn isCheckedException(name: CStr) bool {
    if (name == null) return false;
    if (c.strstr(name, "Error") != null or c.strstr(name, "Panic") != null or
        c.strstr(name, "error") != null or c.strstr(name, "panic") != null)
    {
        return false;
    }
    return true;
}

fn resolveAsClass(reg: ?*const Registry, name: CStr, module_qn: CStr, imp_keys: StrList, imp_vals: StrList, imp_count: c_int) CStr {
    const res = c.cbm_registry_resolve(reg, name, module_qn, imp_keys, imp_vals, imp_count);
    if (res.qualified_name == null or res.qualified_name[0] == 0) return null;
    // Accept any type-like container (Class/Struct/Interface/Enum/Type/Trait):
    // base classes, Rust `impl Trait for S` struct receivers, and Go struct
    // embedding all resolve through here. Struct included so the struct receiver
    // of an IMPLEMENTS edge is not dropped.
    const label: CStr = c.cbm_registry_label_of(reg, res.qualified_name);
    if (!c.cbm_label_is_type_like(label)) return null;
    return res.qualified_name;
}

fn extractDecoratorFunc(dec: CStr, out: [*]u8, outsz: usize) void {
    out[0] = 0;
    if (dec == null) return;
    var start = dec;
    if (start[0] == '@') start += 1;
    const paren: CStr = c.strchr(start, '(');
    const len = if (paren != null) ptrDiff(paren, start) else c.strlen(start);
    if (len == 0 or len >= outsz) return;
    @memcpy(out[0..len], start[0..len]);
    out[len] = 0;
}

// ── File sort for tail-latency reduction ────────────────────────────

const FileSortEntry = extern struct {
    idx: c_int,
    size: i64,
};

fn compareBySizeDesc(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const fa: *const FileSortEntry = @ptrCast(@alignCast(a));
    const fb: *const FileSortEntry = @ptrCast(@alignCast(b));
    if (fb.size > fa.size) return 1;
    if (fb.size < fa.size) return c.CBM_NOT_FOUND;
    return 0;
}

// ── Phase 3A: Parallel Extract ──────────────────────────────────────

const CBM_CACHE_LINE = c.CBM_SZ_128;

/// One per extract worker, padded to a cache line of its own.
const ExtractWorkerState = extern struct {
    local_gbuf: ?*Gbuf align(CBM_CACHE_LINE),
    nodes_created: c_int,
    errors: c_int,
};

const AtomicInt = std.atomic.Value(c_int);

const ExtractCtx = struct {
    files: [*c]const FileInfo,
    sorted: [*c]FileSortEntry,
    file_count: c_int,
    project_name: CStr,
    repo_path: CStr,

    workers: [*c]ExtractWorkerState,
    max_workers: c_int,
    next_worker_id: AtomicInt = .init(0),

    result_cache: [*c][*c]FileResult,
    shared_ids: [*c]i64,
    cancelled: [*c]c_int,
    next_file_idx: AtomicInt = .init(0),

    /// per-worker manifest arrays (separate allocation)
    pkg_entries: [*c]c.cbm_pkg_entries_t,
    /// which Module definitions take `qn#ext`
    stem_marks: ?*const c.cbm_stem_marks_t,

    /// copy source into result->arena for cross-file LSP
    retain_sources: bool,
    /// project-wide retention cap (peak-RSS bound)
    retain_total_budget_bytes: usize,
    /// per-file retention cap
    retain_per_file_max_bytes: usize,
    /// total source bytes copied into result arenas
    retained_bytes: std.atomic.Value(i64) = .init(0),
    /// WARN index.retain_capped emitted once per run
    retain_cap_warned: AtomicInt = .init(0),

    /// Per-worker skip lists (separate allocation, indexed by worker_id — no hot-
    /// path lock). Merged into the pipeline in the sequential merge loop.
    err_lists: [*c]PpErrList,
    /// throttle for the index.file_oversized WARN
    oversized_warned: AtomicInt = .init(0),

    /// Back-pressure futility latch: set when a full collect+nap cycle ended
    /// still over budget — the resident floor (graph + retained sources), not
    /// in-flight transients, holds the memory, so napping cannot reclaim it.
    /// While set, pulls skip the nap (the designed soft overshoot); the cheap
    /// over-budget probe re-arms the gate once RSS drains under budget.
    bp_futile: AtomicInt = .init(0),
    /// Set once futility was CONFIRMED by a second full cycle that still ended
    /// over budget. Every pull loop breaks on it and extract returns
    /// CBM_PIPELINE_ABORT_OVER_BUDGET. Deliberately not the shared cancel
    /// token — that one belongs to client cancellation.
    over_budget_abort: AtomicInt = .init(0),

    /// Spill (F1). With a store, the first futility latch turns spilling on
    /// instead of failing the attempt: every finished result is then parked on
    /// disk and its result_cache slot cleared. result_owner[file_idx] is the
    /// extracting worker id + 1, so each worker parks its own backlog once
    /// (its own allocations, which its next cbm_mem_collect returns). Only a
    /// latch that fires again while spilling fails the attempt whole.
    spill: ?*Spill,
    result_owner: [*c]c_int,
    spilling: AtomicInt = .init(0),
};

/// Relaxed load of a C atomic_int reached through a pointer.
inline fn loadInt(p: *const c_int) c_int {
    return @atomicLoad(c_int, p, .monotonic);
}

/// Cap on the number of index.file_oversized WARN lines (the full list still goes
/// to the response/logfile — this only throttles the stderr noise).
const PP_OVERSIZED_WARN_MAX = 32;

/// Insert one definition node (and its route if present) into the local gbuf.
fn insertDefIntoGbuf(ws: *ExtractWorkerState, marks: ?*const c.cbm_stem_marks_t, fi: *const FileInfo, def: *Definition) void {
    var props: [c.CBM_SZ_2K]u8 = undefined;
    buildDefProps(&props, props.len, def);
    const rel: CStr = if (def.file_path != null) def.file_path else fi.rel_path;
    var marked: [c.CBM_SZ_4K]u8 = undefined;
    const qn: CStr = if (def.label != null and streq(def.label, "Module"))
        c.cbm_stem_marks_module_qn(marks, def.qualified_name, rel, fi.language, &marked, marked.len)
    else
        def.qualified_name;
    const func_id = c.cbm_gbuf_upsert_node(
        ws.local_gbuf,
        if (def.label != null) def.label else "Function",
        def.name,
        qn,
        rel,
        @bitCast(def.start_line),
        @bitCast(def.end_line),
        &props,
    );
    ws.nodes_created += 1;
    if (def.route_path != null and def.route_path[0] != 0) {
        const rm: CStr = if (def.route_method != null) def.route_method else "ANY";
        var route_qn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
        var cpath: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&route_qn, route_qn.len, "__route__%s__%s", rm, c.cbm_route_canon_path(def.route_path, &cpath, cpath.len));
        var rprops: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&rprops, rprops.len, "{\"method\":\"%s\",\"source\":\"decorator\"}", rm);
        const route_id = c.cbm_gbuf_upsert_node(
            ws.local_gbuf,
            "Route",
            def.route_path,
            &route_qn,
            if (def.file_path != null) def.file_path else fi.rel_path,
            0,
            0,
            &rprops,
        );
        var hprops: [c.CBM_SZ_512]u8 = undefined;
        var esc_h: [c.CBM_SZ_512]u8 = undefined;
        _ = c.cbm_json_escape(&esc_h, esc_h.len, def.qualified_name);
        _ = c.snprintf(&hprops, hprops.len, "{\"handler\":\"%s\"}", @as(CStr, &esc_h));
        _ = c.cbm_gbuf_insert_edge(ws.local_gbuf, func_id, route_id, "HANDLES", &hprops);
    }
}

/// (int)ms for a millisecond count, as C converts it.
fn msInt(ms: u64) c_int {
    return @bitCast(@as(c_uint, @truncate(ms)));
}

/// (int)(bytes / 1 MiB), as C converts it.
fn mbInt(bytes: usize) c_int {
    return @bitCast(@as(c_uint, @truncate(bytes / (1024 * 1024))));
}

fn logExtractFail(pos: c_int, ms: u64, path: CStr) void {
    if (pos < PP_LOG_THRESH) {
        interop.logWarn("parallel.extract.file.fail", .{
            "pos",        itoa(pos),
            "elapsed_ms", itoa(msInt(ms)),
            "path",       path,
        });
    }
}

fn logExtractDone(pos: c_int, ms: u64, defs: c_int, path: CStr) void {
    if (pos < PP_LOG_THRESH or ms > PP_TIMER_THRESH) {
        interop.logInfo("parallel.extract.file.done", .{
            "pos",        itoa(pos),
            "elapsed_ms", itoa(msInt(ms)),
            "defs",       itoa(defs),
            "path",       path,
        });
    }
}

/// One back-pressure cycle: reclaim this thread's freed pages, then nap in
/// bounded 3 ms steps while the process stays over budget. Counted for test
/// observability. Returns true when the FULL cycle elapsed still over budget —
/// the resident floor, not in-flight transients, holds the memory.
fn ppBackpressureCycleStillOver(ec: *ExtractCtx) bool {
    c.cbm_mem_collect();
    _ = g_bp_nap_cycles.fetchAdd(1, .monotonic);
    var bp: c_int = 0;
    while (bp < PP_BACKPRESSURE_MAX_SPINS and c.cbm_mem_over_budget() and
        loadInt(ec.cancelled) == 0 and ec.over_budget_abort.load(.monotonic) == 0) : (bp += 1)
    {
        var nap: Timespec = .{ .tv_sec = 0, .tv_nsec = PP_BACKPRESSURE_NAP_NS };
        _ = c.cbm_nanosleep(&nap, null);
    }
    return bp == PP_BACKPRESSURE_MAX_SPINS and c.cbm_mem_over_budget();
}

/// Back-pressure was futile and the confirmation cycle still ended over budget.
/// The budget stops being advisory here: flag the attempt so every worker stops
/// pulling and extract returns CBM_PIPELINE_ABORT_OVER_BUDGET. Nothing is
/// published, so the previously served Zova generation keeps answering.
fn ppFailWholeOverBudget(ec: *ExtractCtx) void {
    if (ec.over_budget_abort.swap(1, .monotonic) == 0) {
        logError("mem.budget.exceeded", .{
            "rss_mb",    itoa(mbInt(c.cbm_mem_rss())),
            "budget_mb", itoa(mbInt(c.cbm_mem_budget())),
            "phase",     "parallel_extract",
            "action",    "fail_whole",
        });
    }
}

fn ppPark(ec: *ExtractCtx, file_idx: c_int, result: ?*FileResult) bool {
    if (!c.cbm_result_spill_park(ec.spill, file_idx, result)) return false;
    _ = g_spill_parked.fetchAdd(1, .monotonic);
    return true;
}

/// Park every result this worker extracted that is still in memory.
fn ppSpillBacklog(ec: *ExtractCtx, worker_id: c_int) void {
    for (0..ix(ec.file_count)) |i| {
        if (loadInt(util.at(ec.result_owner, i)) != worker_id + 1) continue;
        if (ec.result_cache[i] != null and ppPark(ec, @intCast(i), ec.result_cache[i])) {
            ec.result_cache[i] = null;
        }
    }
    c.cbm_mem_collect();
}

/// The budget gate's futility latch fired. With a spill store that is not yet
/// spilling, turn spilling on and re-arm the gate; otherwise run the B3
/// confirmation cycle and fail the attempt whole if it still ends over.
fn ppBackpressureFutile(ec: *ExtractCtx, worker_id: c_int) void {
    if (ec.spill != null and ec.spilling.swap(1, .monotonic) == 0) {
        interop.logWarn("mem.spill.engaged", .{
            "rss_mb",    itoa(mbInt(c.cbm_mem_rss())),
            "budget_mb", itoa(mbInt(c.cbm_mem_budget())),
        });
        ppSpillBacklog(ec, worker_id);
        ec.bp_futile.store(0, .monotonic);
        return;
    }
    interop.logWarn("mem.backpressure.futile", .{ "action", "confirm" });
    if (ppBackpressureCycleStillOver(ec)) {
        ppFailWholeOverBudget(ec);
    } else {
        ec.bp_futile.store(0, .monotonic);
    }
}

fn extractWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    const ec: *ExtractCtx = @ptrCast(@alignCast(ctx_ptr));
    const ws = util.at(ec.workers, ix(worker_id));
    var backlog_spilled = false;

    // Lazy gbuf creation
    if (ws.local_gbuf == null) {
        ws.local_gbuf = c.cbm_gbuf_new_shared_ids(ec.project_name, ec.repo_path, ec.shared_ids);
    }

    // Pull files from shared atomic counter
    while (true) {
        const sort_pos = ec.next_file_idx.fetchAdd(1, .monotonic);
        if (sort_pos >= ec.file_count) break;
        if (loadInt(ec.cancelled) != 0 or ec.over_budget_abort.load(.monotonic) != 0) break;

        // Memory back-pressure (large repos): if the process is over its RSS
        // budget, reclaim this thread's freed pages and nap so peer workers can
        // finish their current file and return memory before this worker adds
        // another parse working set. Caps the concurrent extraction transient
        // near the budget instead of letting all workers parse their biggest
        // files at once. Self-disabling when the budget is unset (tests) or RSS
        // is under budget; bounded spins avoid deadlock when the resident graph
        // is itself near budget.
        //
        // Futility latch: when a FULL nap cycle ends still over budget, the
        // resident floor — not transients — holds the memory; napping again on
        // the next pull cannot reclaim it and only idles workers (linux kernel:
        // one full cycle per pull ≈ 390 s at 79% avg CPU). The worker that
        // latches bp_futile (0→1) runs ONE confirmation cycle while its peers
        // skip the nap. A confirmation that ends under budget re-arms the gate
        // (RSS drained after all); one that still ends over budget fails the
        // attempt whole: every worker stops pulling, extract returns
        // CBM_PIPELINE_ABORT_OVER_BUDGET, nothing is published and the
        // previously served generation keeps answering.
        if (c.cbm_mem_budget() > 0) {
            const over = c.cbm_mem_over_budget();
            const futile = ec.bp_futile.load(.monotonic) != 0;
            if (over and !futile) {
                // Act only on the 0→1 transition: all workers race into the
                // gate before anyone latches, so a plain store would confirm
                // (and WARN) once per worker.
                if (ppBackpressureCycleStillOver(ec) and ec.bp_futile.swap(1, .monotonic) == 0) {
                    ppBackpressureFutile(ec, worker_id);
                }
            } else if (!over and futile) {
                ec.bp_futile.store(0, .monotonic);
            }
        }
        if (ec.over_budget_abort.load(.monotonic) != 0) break;
        const spilling = ec.spilling.load(.monotonic) != 0;
        if (spilling and !backlog_spilled) {
            ppSpillBacklog(ec, worker_id);
            backlog_spilled = true;
        }

        const file_idx = ec.sorted[ix(sort_pos)].idx;
        const fi = util.at(ec.files, ix(file_idx));
        const errs: ?*PpErrList = if (ec.err_lists != null) util.at(ec.err_lists, ix(worker_id)) else null;

        // Crash-quarantine skip (Stage 3c): a file the supervisor pinned as a
        // crasher must never be extracted again. Record it as a phase="crash"
        // skip in this worker's list (merged into the pipeline's file-error list
        // later, surfacing in skipped[]) and move on — the good files still
        // index and status stays "indexed". No-op unless the supervisor set
        // CBM_INDEX_QUARANTINE_FILE. cbm_extract_file's hard guard backstops
        // this check.
        if (c.cbm_index_is_quarantined(fi.rel_path)) {
            var phase: CStr = c.cbm_index_quarantine_phase(fi.rel_path);
            if (phase == null) phase = "crash";
            const reason: CStr =
                if (streq(phase, "hang")) "quarantined after hang" else "quarantined after crash";
            ppErrAdd(errs, fi.rel_path, reason, phase);
            ws.errors += 1;
            continue;
        }

        // Read + extract
        var source_len: c_int = 0;
        var file_size: c_long = 0;
        var rst: c.cbm_read_status_t = c.CBM_READ_OK;
        const source = readFile(fi.path, &source_len, &file_size, &rst);
        if (source == null) {
            ws.errors += 1;
            if (rst == c.CBM_READ_OVERSIZED) {
                // Never a silent drop: record the oversized skip + a throttled
                // WARN so the file surfaces in the response/logfile.
                const cap = c.cbm_max_file_bytes();
                const mib: c_long = c.CBM_SZ_1K * c.CBM_SZ_1K;
                var reason: [96]u8 = undefined;
                _ = c.snprintf(
                    &reason,
                    reason.len,
                    "oversized (%lld MB > %lld MB)",
                    @as(c_longlong, @divTrunc(file_size, mib)),
                    @as(c_longlong, @divTrunc(cap, mib)),
                );
                ppErrAdd(errs, fi.rel_path, &reason, "oversized");
                if (ec.oversized_warned.fetchAdd(1, .monotonic) < PP_OVERSIZED_WARN_MAX) {
                    interop.logWarn("index.file_oversized", .{
                        "path",    fi.rel_path,
                        "size_mb", itoa(@truncate(@divTrunc(file_size, mib))),
                        "cap_mb",  itoa(@truncate(@divTrunc(cap, mib))),
                    });
                }
            } else if (rst == c.CBM_READ_OPEN_FAIL or rst == c.CBM_READ_OOM) {
                ppErrAdd(errs, fi.rel_path, "read failed", "read");
            }
            // CBM_READ_EMPTY: benign 0-byte file — not reported.
            continue;
        }

        // Per-file start log: shows which file each worker is processing.
        // Critical for diagnosing stuck workers on large vendored files.
        if (sort_pos < PP_LOG_THRESH) { // first 2 rounds of workers = most interesting
            interop.logInfo("parallel.extract.file.start", .{
                "pos",     itoa(sort_pos),
                "size_kb", itoa(@divTrunc(source_len, c.CBM_SZ_1K)),
                "path",    fi.rel_path,
            });
        }

        const file_t0 = extractNowNs();

        const result_opt: ?*FileResult = c.cbm_extract_file(source, source_len, fi.language, ec.project_name, fi.rel_path, c.CBM_EXTRACT_BUDGET, null, null);

        const file_elapsed_ms = (extractNowNs() -% file_t0) / PP_USEC_PER_MS;

        const result = result_opt orelse {
            logExtractFail(sort_pos, file_elapsed_ms, fi.rel_path);
            freeSource(source);
            ws.errors += 1;
            ppErrAdd(errs, fi.rel_path, "extract failed", "extract");
            continue;
        };
        logExtractDone(sort_pos, file_elapsed_ms, result.defs.count, fi.rel_path);

        // Consume the previously-ignored has_error flag: a parse timeout / parse
        // failure / unsupported-grammar result carries no defs but must still be
        // reported (phase "extract", reason = the extractor's message). The empty
        // result flows through unchanged below (the defs loop is a no-op).
        if (result.has_error) {
            ppErrAdd(errs, fi.rel_path, if (result.error_msg != null) result.error_msg else "extract failed", "extract");
            ws.errors += 1;
        } else if (result.parse_incomplete) {
            // Indexed, but with unrecovered ERROR/MISSING regions. Not a skip:
            // recorded under the distinct "parse_partial" phase with the
            // line-range list as the reason, or "parse_unusable" when one range
            // covers most of the file (see parse_unusable in cbm.h).
            ppErrAdd(
                errs,
                fi.rel_path,
                if (result.error_ranges != null) result.error_ranges else "",
                if (result.parse_unusable) "parse_unusable" else "parse_partial",
            );
        }

        // Create definition nodes in local gbuf
        for (0..ix(result.defs.count)) |d| {
            const def = util.at(result.defs.items, d);
            if (def.qualified_name != null and def.name != null) {
                insertDefIntoGbuf(ws, ec.stem_marks, fi, def);
            }
        }

        // Free TSTree immediately — arena strings survive for registry+resolve.
        // This makes slab reset safe: tree-sitter's internal nodes (in slab)
        // are released before the slab is bulk-reclaimed.
        c.cbm_free_tree(result);

        // Detect and parse manifest files for package map
        {
            const bn: CStr = c.strrchr(fi.rel_path, '/');
            _ = c.cbm_pkgmap_try_parse(
                if (bn != null) bn + 1 else fi.rel_path,
                fi.rel_path,
                source,
                source_len,
                util.at(ec.pkg_entries, ix(worker_id)),
            );
        }

        // Retain source bytes in result->arena so the fused cross-file LSP
        // step in resolve_worker can re-parse without re-reading from disk.
        // Bounded per-file (retain_per_file_max_bytes) and by a project-wide
        // budget (retain_total_budget_bytes) to bound peak RSS — see the
        // retention-cap comment at the top of this file. A file DROPPED here
        // is NOT lost to cross-file resolution: resolve_worker re-reads it on
        // demand (bounded, freed immediately). WARN once per run so the
        // operator knows retention was capped (index.retain_capped).
        if (ec.retain_sources and source_len > 0) {
            var dropped = false;
            const len: usize = ix(source_len);
            if (len > ec.retain_per_file_max_bytes) {
                dropped = true; // over the per-file cap
            } else {
                const prior = ec.retained_bytes.fetchAdd(source_len, .monotonic);
                if (@as(usize, @bitCast(prior + source_len)) <= ec.retain_total_budget_bytes) {
                    const copy: MutStr = @ptrCast(c.cbm_arena_alloc(&result.arena, len + 1));
                    if (copy != null) {
                        @memcpy(copy[0..len], source[0..len]);
                        copy[len] = 0;
                        result.source = copy;
                        result.source_len = source_len;
                    } else {
                        // Arena OOM — not a cap drop; re-read still covers
                        // cross-file resolution, so don't emit the WARN.
                        _ = ec.retained_bytes.fetchSub(source_len, .monotonic);
                    }
                } else {
                    _ = ec.retained_bytes.fetchSub(source_len, .monotonic);
                    dropped = true; // project-wide budget exhausted
                }
            }
            if (dropped and ec.retain_cap_warned.swap(1, .monotonic) == 0) {
                interop.logWarn("index.retain_capped", .{
                    "path",  @as(CStr, if (fi.rel_path != null) fi.rel_path else "?"),
                    "bytes", itoa(source_len),
                });
            }
        }

        // Free source buffer — extraction captured everything needed,
        // and the retention copy (if any) lives in result->arena.
        freeSource(source);

        // Cache result (arena + extracted data, no tree) for Phase 3B and Phase 4.
        // While spilling it goes straight to disk; a failed park keeps it.
        if (!spilling or !ppPark(ec, file_idx, result)) {
            ec.result_cache[ix(file_idx)] = result;
            if (ec.result_owner != null) {
                @atomicStore(c_int, util.at(ec.result_owner, ix(file_idx)), worker_id + 1, .monotonic);
            }
        }

        // Progress logging: log every 10 files (atomic read, no contention)
        if (@rem(sort_pos + 1, PP_LOG_INTERVAL) == 0 or sort_pos + 1 == ec.file_count) {
            interop.logInfo("parallel.extract.progress", .{
                "done",  itoa(sort_pos + 1),
                "total", itoa(ec.file_count),
            });
        }

        // Reclaim all slab + tier2 memory between files.
        //
        // After cbm_free_tree(result), all tree nodes are on free lists.
        // We then destroy the parser (frees its internal allocations too),
        // leaving ZERO live slab/tier2 pointers. At that point, we can
        // safely munmap/free every page, bounding peak memory per-file
        // instead of accumulating across all 644 files.
        //
        // get_thread_parser() in cbm_extract_file will create a fresh
        // parser for the next file — cost is microseconds vs seconds
        // for parsing. This prevents unbounded memory accumulation and works
        // identically on macOS, Linux, and Windows.
        c.cbm_destroy_thread_parser();
        c.cbm_slab_reclaim();
        c.cbm_mem_collect();
    }

    // Final cleanup (parser already destroyed in loop, just slab state)
    c.cbm_slab_destroy_thread();
    c.cbm_kind_in_set_free_cache(); // free this worker thread's node-type bitset cache
}

fn mergePkgEntries(ctx: *Ctx, pkg_entries: [*c]c.cbm_pkg_entries_t, worker_count: c_int) void {
    if (pkg_entries == null) return;
    // Supplement with a repo-wide filesystem walk so manifests filtered
    // by the main discoverer (package.json, composer.json — in
    // IGNORED_JSON_FILES) still feed pkgmap. Append into worker 0's
    // array so the existing merge below sees them.
    _ = c.cbm_pkgmap_scan_repo(ctx.repo_path, util.at(pkg_entries, 0), ctx.excluded_dirs, ctx.excluded_count);
    ctx.pkgmap = c.cbm_pkgmap_build(pkg_entries, worker_count, ctx.project_name);
    c.cbm_pipeline_set_pkgmap(ctx.pkgmap);
    for (0..ix(worker_count)) |i| c.cbm_pkg_entries_free(util.at(pkg_entries, i));
    c.free(pkg_entries);
}

fn logExtractMemStats(worker_count: c_int) void {
    if (c.cbm_mem_budget() > 0) {
        interop.logInfo("parallel.extract.mem", .{
            "rss_mb",        itoa(mbInt(c.cbm_mem_rss())),
            "peak_mb",       itoa(mbInt(c.cbm_mem_peak_rss())),
            "budget_mb",     itoa(mbInt(c.cbm_mem_budget())),
            "per_worker_mb", itoa(mbInt(c.cbm_mem_worker_budget(worker_count))),
        });
    }
}

pub export fn cbm_parallel_extract_ex(
    ctx: *Ctx,
    files: [*c]const FileInfo,
    file_count: c_int,
    result_cache: [*c][*c]FileResult,
    shared_ids: [*c]i64,
    worker_count: c_int,
    opts: ?*const c.cbm_parallel_extract_opts_t,
) c_int {
    const resolved_opts = cbmParallelExtractResolveOpts(opts);

    if (file_count == 0) return 0;

    interop.logInfo("parallel.extract.start", .{
        "files",   itoa(file_count),
        "workers", itoa(worker_count),
    });
    interop.logInfo("parallel.extract.retention", .{
        "retain_sources", boolText(resolved_opts.retain_sources),
        "total_mb",       itoa(mbInt(resolved_opts.retain_total_budget_bytes)),
        "per_file_mb",    itoa(mbInt(resolved_opts.retain_per_file_max_bytes)),
    });

    // Log per-worker memory budget
    if (c.cbm_mem_budget() > 0) {
        const worker_budget = c.cbm_mem_worker_budget(worker_count);
        interop.logInfo("parallel.mem.budget", .{
            "total_mb",      itoa(mbInt(c.cbm_mem_budget())),
            "per_worker_mb", itoa(mbInt(worker_budget)),
        });
    }

    // Sub-phase: Ensure extraction library is initialized
    const t_init = util.profStart();
    _ = c.cbm_init();

    // Slab allocator for tree-sitter (thread-safe via TLS).
    c.cbm_slab_install();
    util.profEnd("parallel_extract", "1_init_libs", &t_init, 0);

    // Sub-phase: Sort files by descending size for tail-latency reduction
    const t_sort = util.profStart();
    const sorted = interop.mallocN(FileSortEntry, ix(file_count));
    if (sorted == null) return c.CBM_NOT_FOUND;
    for (0..ix(file_count)) |i| {
        sorted[i].idx = @intCast(i);
        sorted[i].size = files[i].size;
    }
    if (file_count > 1) {
        c.qsort(sorted, ix(file_count), @sizeOf(FileSortEntry), compareBySizeDesc);
    }
    util.profEnd("parallel_extract", "2_sort_files", &t_sort, file_count);

    // Allocate per-worker state (cache-line aligned via posix_memalign)
    var workers_raw: ?*anyopaque = null;
    const workers_bytes = ix(worker_count) * @sizeOf(ExtractWorkerState);
    if (c.cbm_aligned_alloc(&workers_raw, CBM_CACHE_LINE, workers_bytes) != 0) {
        c.free(sorted);
        return c.CBM_NOT_FOUND;
    }
    const workers: [*c]ExtractWorkerState = @ptrCast(@alignCast(workers_raw));
    @memset(@as([*]u8, @ptrCast(workers))[0..workers_bytes], 0);

    // Per-worker manifest entry arrays (separate from cache-line-aligned worker state)
    const pkg_entries = interop.callocN(c.cbm_pkg_entries_t, ix(worker_count));
    if (pkg_entries == null) {
        c.cbm_aligned_free(workers);
        c.free(sorted);
        return c.CBM_NOT_FOUND;
    }

    // Per-worker skip lists (separate allocation; merged into the pipeline in the
    // sequential merge loop below).
    const err_lists = interop.callocN(PpErrList, ix(worker_count));

    var ec: ExtractCtx = .{
        .files = files,
        .sorted = sorted,
        .file_count = file_count,
        .project_name = ctx.project_name,
        .repo_path = ctx.repo_path,
        .workers = workers,
        .max_workers = worker_count,
        .result_cache = result_cache,
        .shared_ids = shared_ids,
        .cancelled = ctx.cancelled,
        .pkg_entries = pkg_entries,
        .stem_marks = ctx.stem_marks,
        .err_lists = err_lists,
        .retain_sources = resolved_opts.retain_sources,
        .retain_total_budget_bytes = resolved_opts.retain_total_budget_bytes,
        .retain_per_file_max_bytes = resolved_opts.retain_per_file_max_bytes,
        .spill = ctx.spill,
        .result_owner = null,
    };
    ec.result_owner = if (ec.spill != null) interop.callocN(c_int, ix(file_count)) else null;
    if (ec.result_owner == null) {
        ec.spill = null; // no owner map, no spilling: the B3 path stays
    }
    ec.spilling = .init(@intFromBool(ec.spill != null and g_force_spill.load(.monotonic) != 0));

    // Sub-phase: Dispatch workers (parse + extract per file, PARALLEL)
    const t_dispatch = util.profStart();
    var parallel_opts = std.mem.zeroes(c.cbm_parallel_for_opts_t);
    parallel_opts.max_workers = worker_count;
    parallel_opts.force_pthreads = false;
    c.cbm_parallel_for(worker_count, extractWorker, &ec, parallel_opts);
    util.profEnd("parallel_extract", "3_dispatch_workers_parallel", &t_dispatch, file_count);

    // Sub-phase: Merge all local gbufs into main gbuf (SEQUENTIAL, gbuf not thread-safe)
    const t_merge = util.profStart();
    var total_nodes: c_int = 0;
    var total_errors: c_int = 0;
    for (0..ix(worker_count)) |i| {
        if (workers[i].local_gbuf != null) {
            _ = c.cbm_gbuf_merge(ctx.gbuf, workers[i].local_gbuf);
            total_nodes += workers[i].nodes_created;
            total_errors += workers[i].errors;
            c.cbm_gbuf_free(workers[i].local_gbuf);
        }
    }
    util.profEnd("parallel_extract", "4_merge_gbufs_seq", &t_merge, total_nodes);

    // Merge per-worker skip lists into the pipeline (SEQUENTIAL — no lock).
    // Runs unconditionally (not gated on local_gbuf) so a worker whose files all
    // failed still surfaces its skips.
    if (err_lists != null) {
        for (0..ix(worker_count)) |i| {
            for (0..ix(err_lists[i].count)) |j| {
                const item = &err_lists[i].items[j];
                c.cbm_pipeline_add_file_error(ctx.pipeline, item.path, item.reason, item.phase);
                c.free(item.path);
                c.free(item.reason);
                c.free(item.phase);
            }
            c.free(err_lists[i].items);
        }
        c.free(err_lists);
    }

    mergePkgEntries(ctx, pkg_entries, worker_count);

    c.cbm_aligned_free(workers);
    c.free(sorted);
    c.free(ec.result_owner);

    // The over-budget verdict outranks the cancel sentinel: a caller must be
    // able to name the cause. Every non-zero code stops before publication.
    if (ec.over_budget_abort.load(.seq_cst) != 0) return c.CBM_PIPELINE_ABORT_OVER_BUDGET;
    if (@atomicLoad(c_int, ctx.cancelled, .seq_cst) != 0) return c.CBM_NOT_FOUND;

    logExtractMemStats(worker_count);

    interop.logInfo("parallel.extract.done", .{
        "nodes",  itoa(total_nodes),
        "errors", itoa(total_errors),
    });
    return 0;
}

pub export fn cbm_parallel_extract(
    ctx: *Ctx,
    files: [*c]const FileInfo,
    file_count: c_int,
    result_cache: [*c][*c]FileResult,
    shared_ids: [*c]i64,
    worker_count: c_int,
) c_int {
    return cbm_parallel_extract_ex(ctx, files, file_count, result_cache, shared_ids, worker_count, null);
}

// ── Phase 3B: Serial Registry Build ─────────────────────────────────

/// Register one definition and create DEFINES + DEFINES_METHOD edges. Returns edge count.
fn registerAndLinkDef(ctx: *Ctx, def: *const Definition, rel: CStr, lang: Language, reg_entries: *c_int) c_int {
    var edges: c_int = 0;
    if (def.name == null or def.qualified_name == null or def.label == null) return 0;
    // Register callable symbols + every type-like container (Class/Struct/
    // Interface/Enum/Type/Trait). Type-like defs must be in the registry so
    // `class Foo : IBar` (INHERITS), `impl Trait for S` (IMPLEMENTS), and
    // method/field resolution can reach them — Struct included so Rust/Go/
    // Swift/D structs resolve as type targets. Variable/Field defs are
    // registered too so READS/WRITES can resolve.
    // KEEP IN SYNC with pipeline_incremental's seed set.
    if (streq(def.label, "Function") or streq(def.label, "Method") or
        c.cbm_label_is_type_like(def.label) or streq(def.label, "Variable") or
        streq(def.label, "Field"))
    {
        c.cbm_registry_add(ctx.registry, def.name, def.qualified_name, def.label);
        reg_entries.* += 1;
    }
    const file_qn: MutStr = c.cbm_pipeline_fqn_compute(ctx.project_name, rel, "__file__");
    const file_node: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, file_qn);
    var marked: [c.CBM_SZ_4K]u8 = undefined;
    const qn: CStr = if (streq(def.label, "Module"))
        c.cbm_stem_marks_module_qn(
            ctx.stem_marks,
            def.qualified_name,
            if (def.file_path != null) def.file_path else rel,
            lang,
            &marked,
            marked.len,
        )
    else
        def.qualified_name;
    const def_node: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, qn);
    if (file_node != null and def_node != null) {
        _ = c.cbm_gbuf_insert_edge(ctx.gbuf, file_node.?.id, def_node.?.id, "DEFINES", "{}");
        edges += 1;
    }
    c.free(file_qn);
    if (def.parent_class != null and streq(def.label, "Method")) {
        const parent: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, def.parent_class);
        if (parent != null and def_node != null) {
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, parent.?.id, def_node.?.id, "DEFINES_METHOD", "{}");
        }
    }
    return edges;
}

/// Create IMPORTS edges for one file's imports (parallel path).
fn createImportsEdges(ctx: *Ctx, result: *const FileResult, rel: CStr, namespace_map: ?*HashTable) c_int {
    var count: c_int = 0;
    const file_qn: MutStr = c.cbm_pipeline_fqn_compute(ctx.project_name, rel, "__file__");
    const source_node: *const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, file_qn) orelse {
        c.free(file_qn);
        return 0;
    };
    for (0..ix(result.imports.count)) |j| {
        const imp = util.at(result.imports.items, j);
        if (imp.module_path == null) continue;
        const target: ?*const Node = c.cbm_pipeline_resolve_import_node(ctx, rel, file_qn, imp, namespace_map);
        if (target != null and target.?.id != source_node.id) {
            var esc_ln: [c.CBM_SZ_128]u8 = undefined;
            _ = c.cbm_json_escape(&esc_ln, esc_ln.len, if (imp.local_name != null) imp.local_name else "");
            var imp_props: [c.CBM_SZ_256]u8 = undefined;
            _ = c.snprintf(&imp_props, imp_props.len, "{\"local_name\":\"%s\"}", @as(CStr, &esc_ln));
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, source_node.id, target.?.id, "IMPORTS", &imp_props);
            count += 1;
        }
    }
    c.free(file_qn);
    return count;
}

/// Find channel source node (enclosing function or file).
fn findChannelSrc(ctx: *Ctx, ch: *const c.CBMChannel, rel: CStr) ?*const Node {
    var node: ?*const Node = null;
    if (ch.enclosing_func_qn != null and ch.enclosing_func_qn[0] != 0) {
        node = c.cbm_gbuf_find_by_qn(ctx.gbuf, ch.enclosing_func_qn);
    }
    if (node == null) {
        const file_qn: MutStr = c.cbm_pipeline_fqn_compute(ctx.project_name, rel, "__file__");
        node = c.cbm_gbuf_find_by_qn(ctx.gbuf, file_qn);
        c.free(file_qn);
    }
    return node;
}

/// Create Channel nodes + EMITS/LISTENS_ON edges for one file.
fn createChannelEdges(ctx: *Ctx, result: *const FileResult, rel: CStr) void {
    for (0..ix(result.channels.count)) |j| {
        const ch = util.at(result.channels.items, j);
        if (ch.channel_name == null or ch.channel_name[0] == 0) continue;
        const transport: CStr = if (ch.transport != null) ch.transport else "unknown";
        var channel_qn: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(&channel_qn, channel_qn.len, "__channel__%s__%s", transport, ch.channel_name);
        var esc_cn: [c.CBM_SZ_256]u8 = undefined;
        _ = c.cbm_json_escape(&esc_cn, esc_cn.len, ch.channel_name);
        var channel_props: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(&channel_props, channel_props.len, "{\"transport\":\"%s\",\"name\":\"%s\"}", transport, @as(CStr, &esc_cn));
        const channel_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "Channel", ch.channel_name, &channel_qn, "", 0, 0, &channel_props);
        const src_node = findChannelSrc(ctx, ch, rel);
        if (src_node != null and channel_id > 0) {
            const edge_type: CStr = if (ch.direction == c.CBM_CHANNEL_EMIT) "EMITS" else "LISTENS_ON";
            var edge_props: [c.CBM_SZ_128]u8 = undefined;
            _ = c.snprintf(&edge_props, edge_props.len, "{\"transport\":\"%s\"}", transport);
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, src_node.?.id, channel_id, edge_type, &edge_props);
        }
    }
}

/// Create CONFIGURES edges for one file's env accesses.
fn createEnvConfigures(ctx: *Ctx, result: *const FileResult, rel: CStr) void {
    var file_qn: MutStr = null;
    var file_node: ?*const Node = null;
    for (0..ix(result.env_accesses.count)) |j| {
        const ea = util.at(result.env_accesses.items, j);
        if (ea.env_key == null or ea.env_key[0] == 0) continue;
        var env_qn: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(&env_qn, env_qn.len, "__env__%s", ea.env_key);
        var env_props: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(&env_props, env_props.len, "{\"env_key\":\"%s\"}", ea.env_key);
        const env_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "EnvVar", ea.env_key, &env_qn, "", 0, 0, &env_props);
        if (env_id <= 0) continue;
        var src: ?*const Node = null;
        if (ea.enclosing_func_qn != null and ea.enclosing_func_qn[0] != 0) {
            src = c.cbm_gbuf_find_by_qn(ctx.gbuf, ea.enclosing_func_qn);
        }
        if (src == null) {
            if (file_qn == null) {
                file_qn = c.cbm_pipeline_fqn_compute(ctx.project_name, rel, "__file__");
                file_node = c.cbm_gbuf_find_by_qn(ctx.gbuf, file_qn);
            }
            src = file_node;
        }
        if (src != null and src.?.id != env_id) {
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, src.?.id, env_id, "CONFIGURES", "{\"strategy\":\"env_access\"}");
        }
    }
    c.free(file_qn);
}

pub export fn cbm_build_registry_from_cache(
    ctx: *Ctx,
    files: [*c]const FileInfo,
    file_count: c_int,
    result_cache: [*c][*c]FileResult,
) c_int {
    interop.logInfo("parallel.registry.start", .{ "files", itoa(file_count) });

    var reg_entries: c_int = 0;
    var defines_edges: c_int = 0;
    var imports_edges: c_int = 0;

    // Namespace/package → File-QN map for namespace imports (C# `using`,
    // Java/Kotlin `import`, PHP `use`). Built from the full result cache so
    // every declaring file is visible regardless of loop order.
    const rels = interop.callocN(CStr, ix(file_count));
    if (rels != null) {
        for (0..ix(file_count)) |i| rels[i] = files[i].rel_path;
    }
    const namespace_map =
        c.cbm_pipeline_namespace_map_build(ctx.project_name, result_cache, ctx.spill, rels, file_count);
    c.free(@ptrCast(rels));

    for (0..ix(file_count)) |i| {
        if (util.checkCancel(ctx) != 0) {
            c.cbm_pipeline_namespace_map_free(namespace_map);
            return c.CBM_NOT_FOUND;
        }

        const result: *FileResult = c.cbm_result_cache_acquire(result_cache, ctx.spill, @intCast(i)) orelse continue;

        const rel: CStr = files[i].rel_path;

        // Register callable symbols + DEFINES/DEFINES_METHOD edges
        for (0..ix(result.defs.count)) |d| {
            defines_edges += registerAndLinkDef(ctx, util.at(result.defs.items, d), rel, files[i].language, &reg_entries);
        }

        imports_edges += createImportsEdges(ctx, result, rel, namespace_map);
        createChannelEdges(ctx, result, rel);
        createEnvConfigures(ctx, result, rel);
        c.cbm_result_cache_release(result_cache, @intCast(i), result);
    }

    c.cbm_pipeline_namespace_map_free(namespace_map);

    interop.logInfo("parallel.registry.done", .{
        "entries", itoa(reg_entries),
        "defines", itoa(defines_edges),
        "imports", itoa(imports_edges),
    });
    return 0;
}

// ── Phase 4: Parallel Resolution ────────────────────────────────────

/// One per resolve worker, padded to a cache line of its own.
const ResolveWorkerState = extern struct {
    local_edge_buf: ?*Gbuf align(CBM_CACHE_LINE),
    calls_resolved: c_int,
    usages_resolved: c_int,
    semantic_resolved: c_int,
    errors: c_int,
    /// Subset of calls_resolved that were attributed via the LSP-override
    /// path (cbm_pipeline_find_lsp_resolution hit) rather than the
    /// registry's textual matcher. Surfaced in the parallel.resolve.done
    /// log line so divergence between pipelines becomes observable.
    lsp_overrides: c_int,
};

const AtomicU64 = std.atomic.Value(u64);

const ResolveCtx = struct {
    files: [*c]const FileInfo,
    file_count: c_int,
    project_name: CStr,
    repo_path: CStr,

    workers: [*c]ResolveWorkerState,
    max_workers: c_int,

    result_cache: [*c][*c]FileResult,
    /// parked results (F1); NULL when nothing spilled
    spill: ?*Spill,
    /// READ-ONLY during Phase 4
    main_gbuf: ?*const Gbuf,
    /// READ-ONLY during Phase 4
    registry: ?*const Registry,
    shared_ids: [*c]i64,
    cancelled: [*c]c_int,
    next_file_idx: AtomicInt = .init(0),

    /// Cross-file LSP inputs — pre-built once by the caller in pipeline.zig
    /// and shared read-only by usage across workers (typed non-const to
    /// match the existing cbm_run_X_lsp_cross callee signatures the
    /// worker forwards them to). NULL/0 → cross-LSP no-ops.
    all_defs: [*c]LspDef,
    def_count: c_int,
    /// per-file module QN; def_modules[i] for files[i]
    def_modules: [*c]const MutStr,
    /// Optional inverted index for per-file def filtering (gopls pattern).
    /// When non-NULL, the fused worker calls cbm_pxc_filter_defs_for_file
    /// to shrink the def array passed to the LSP from O(all_defs) to
    /// O(relevant_defs). NULL → each file sees the full all_defs[].
    module_def_index: ?*c.struct_CBMModuleDefIndex,
    /// Tier 2 full: pre-built per-language registries (project-wide,
    /// finalized, READ-ONLY). When non-NULL for a lang, the worker uses
    /// cbm_run_X_lsp_cross_with_registry — skip per-file build entirely.
    /// Stored as CBMCrossLspRegistries* (typedef from pass_lsp_cross.h).
    cross_registries: ?*c.CBMCrossLspRegistries,

    /// Parsed once on the coordinator thread and borrowed read-only by resolve
    /// workers.  The pointer is installed into each worker's TLS slot so Rust's
    /// manifest-aware cross-file resolver sees the same Cargo context.
    rust_manifest: ?*const c.CBMCargoManifest = null,

    /// F4: LAZILY-built shared Rust registry (built ONCE, on the first NULL-filter
    /// rust file — the ~all_defs amplifier files). Not eager: repos whose rust files
    /// all filter to subsets never pay the O(all_defs) build + multi-GB RSS. Built
    /// under rust_shared_mu into rust_shared_arena, published via rust_shared_reg;
    /// torn down after the worker dispatch.
    rust_shared_reg: std.atomic.Value(?*c.CBMTypeRegistry) = .init(null),
    rust_shared_mu: c.cbm_mutex_t = undefined,
    rust_shared_arena: Arena = undefined,
    rust_shared_arena_live: bool = false,

    /// Counters for parallel.resolve.lsp_cross_done summary.
    lsp_cross_processed: AtomicInt = .init(0),
    lsp_cross_skipped_no_source: AtomicInt = .init(0),
    /// Error message of a cross-file LSP walk that stopped incomplete, per
    /// file index. Each file is owned by one worker; the coordinator reports
    /// them after join, so the pipeline's error list is never mutated
    /// concurrently.
    lsp_failures: StrList = null,

    /// Per-sub-phase timing (ns aggregated across workers) — surfaces
    /// exactly where parallel_resolve's wall time is spent so we stop
    /// guessing about hot paths. Logged once at the end of
    /// cbm_parallel_resolve.
    time_ns_import_map: AtomicU64 = .init(0),
    time_ns_cross_lsp: AtomicU64 = .init(0),
    time_ns_calls: AtomicU64 = .init(0),
    time_ns_usages: AtomicU64 = .init(0),
    time_ns_throws: AtomicU64 = .init(0),
    time_ns_rw: AtomicU64 = .init(0),
    time_ns_semantic: AtomicU64 = .init(0),
    /// Whole-iteration timer — captures everything from atomic file_idx
    /// pickup through cleanup. If this >> sum of sub-phases, the
    /// unmeasured cost is either in skip-eligibility checks, gbuf
    /// setup, or — most likely — workers waiting on the
    /// cbm_parallel_for synchronization barrier at the end.
    time_ns_total_loop: AtomicU64 = .init(0),
    total_files_visited: AtomicInt = .init(0),
    /// Sub-breakdowns inside resolve_file_calls — finds the 553µs-per-
    /// iteration hot path that the high-level resolve_calls counter
    /// doesn't pinpoint.
    /// lsp_idx + fallback scan
    time_ns_rc_lsp_lookup: AtomicU64 = .init(0),
    /// lsp_target_node OR registry_resolve
    time_ns_rc_resolve: AtomicU64 = .init(0),
    /// try_field_type_hint
    time_ns_rc_hint: AtomicU64 = .init(0),
    /// gbuf_find_by_qn for target
    time_ns_rc_target: AtomicU64 = .init(0),
    /// emit_service_edge
    time_ns_rc_emit: AtomicU64 = .init(0),
    /// find_source_node
    time_ns_rc_source: AtomicU64 = .init(0),
};

/// Minimum buffer space needed per arg JSON object
const CBM_ARG_JSON_GUARD = c.CBM_SZ_32;

// Append arg data as JSON to edge properties: ,"args":[{"i":0,"e":"x","v":"val"},...]
// Returns new position in buffer.
/// Sanitize expression string for JSON (in-place).
fn sanitizeExpr(expr_buf: *[c.CBM_SZ_128]u8, expr: CStr) void {
    if (expr != null) {
        _ = c.snprintf(expr_buf, 128, "%.*s", @as(c_int, 120), expr);
        var i: usize = 0;
        while (expr_buf[i] != 0) : (i += 1) {
            if (expr_buf[i] == '"') expr_buf[i] = '\'';
            if (expr_buf[i] == '\n' or expr_buf[i] == '\r') expr_buf[i] = ' ';
        }
    } else {
        expr_buf[0] = 0;
    }
}

/// Format one call arg as JSON. Returns snprintf result.
fn formatCallArg(buf: [*]u8, bufsize: usize, a: *const c.CBMCallArg, expr: CStr) c_int {
    var esc_k: [c.CBM_SZ_128]u8 = undefined;
    var esc_e: [c.CBM_SZ_128]u8 = undefined;
    var esc_v: [c.CBM_SZ_128]u8 = undefined;
    const k: CStr = &esc_k;
    const e: CStr = &esc_e;
    const v: CStr = &esc_v;
    _ = c.cbm_json_escape(&esc_e, esc_e.len, expr);
    if (a.keyword != null and a.value != null) {
        _ = c.cbm_json_escape(&esc_k, esc_k.len, a.keyword);
        _ = c.cbm_json_escape(&esc_v, esc_v.len, a.value);
        return c.snprintf(buf, bufsize, "{\"i\":%d,\"k\":\"%s\",\"e\":\"%s\",\"v\":\"%s\"}", a.index, k, e, v);
    }
    if (a.keyword != null) {
        _ = c.cbm_json_escape(&esc_k, esc_k.len, a.keyword);
        return c.snprintf(buf, bufsize, "{\"i\":%d,\"k\":\"%s\",\"e\":\"%s\"}", a.index, k, e);
    }
    if (a.value != null) {
        _ = c.cbm_json_escape(&esc_v, esc_v.len, a.value);
        return c.snprintf(buf, bufsize, "{\"i\":%d,\"e\":\"%s\",\"v\":\"%s\"}", a.index, e, v);
    }
    return c.snprintf(buf, bufsize, "{\"i\":%d,\"e\":\"%s\"}", a.index, e);
}

fn appendArgsJson(buf: [*]u8, bufsize: usize, pos_in: usize, call: *const c.CBMCall) usize {
    var pos = pos_in;
    if (call.arg_count == 0 or pos >= bufsize - PP_ARGS_MARGIN) return pos;
    var n = c.snprintf(buf + pos, bufsize - pos, ",\"args\":[");
    if (n <= 0) return pos;
    pos += ix(n);
    var i: c_int = 0;
    while (i < call.arg_count and pos < bufsize - CBM_ARG_JSON_GUARD) : (i += 1) {
        const a = &call.args[ix(i)];
        const mark = pos; // rollback point (before the separator)
        if (i > 0 and pos < bufsize - 1) {
            buf[pos] = ',';
            pos += 1;
        }
        var expr_buf: [c.CBM_SZ_128]u8 = undefined;
        sanitizeExpr(&expr_buf, a.expr);
        n = formatCallArg(buf + pos, bufsize - pos, a, &expr_buf);
        // snprintf returns the UNtruncated length: if the arg did not fully
        // fit, advancing pos by n would push it past buf and the buf[pos]
        // writes below would overflow. Drop the arg whole (atomic field —
        // keeps the array valid) and stop appending.
        if (n <= 0 or ix(n) >= bufsize - pos) {
            pos = mark;
            break;
        }
        pos += ix(n);
    }
    if (pos < bufsize - 1) {
        buf[pos] = ']';
        pos += 1;
    }
    buf[pos] = 0;
    return pos;
}

/// Scan call args for a URL-like route path and handler reference.
fn isPathKeyword(keyword: CStr) bool {
    const path_keywords = [_][*:0]const u8{
        "prefix",   "path", "route",      "pattern",    "url",
        "endpoint", "rule", "mount_path", "route_path", "url_path",
    };
    for (path_keywords) |kw| {
        if (streq(keyword, kw)) return true;
    }
    return false;
}

fn findRoutePathInArgs(call: *const c.CBMCall, out_handler: *CStr) CStr {
    out_handler.* = null;
    // 1. First string arg starting with /
    if (call.first_string_arg != null and call.first_string_arg[0] == '/') {
        out_handler.* = call.second_arg_name;
        return call.first_string_arg;
    }
    // 2. Keyword args (prefix=, path=, route=, etc.)
    var found: CStr = null;
    for (0..ix(call.arg_count)) |ai| {
        if (found != null) break;
        const ca = &call.args[ai];
        const val: CStr = if (ca.value != null) ca.value else ca.expr;
        if (val == null or val[0] != '/') continue;
        if ((ca.keyword != null and isPathKeyword(ca.keyword)) or (ca.keyword == null and ca.index == 0)) {
            found = val;
        }
    }
    if (found == null) return null;
    // 3. Handler: first identifier arg that's not a path/keyword
    for (0..ix(call.arg_count)) |ai| {
        const ca = &call.args[ai];
        if (ca.expr == null or ca.expr[0] == '/' or ca.expr[0] == '"' or ca.expr[0] == '\'') continue;
        if (ca.keyword != null and (streq(ca.keyword, "prefix") or streq(ca.keyword, "name") or
            streq(ca.keyword, "tags")))
        {
            continue;
        }
        out_handler.* = ca.expr;
        break;
    }
    return found;
}

const PROPS_2K = c.CBM_SZ_2K;

/// Build props JSON, append args, close brace, emit edge.
fn finalizeAndEmit(gbuf: ?*Gbuf, src_id: i64, tgt_id: i64, edge_type: CStr, props: *[PROPS_2K]u8, n: c_int, call: *const c.CBMCall) void {
    if (n > 0 and ix(n) < PROPS_2K - PP_ESC_SPACE) {
        var pos = appendArgsJson(props, PROPS_2K, ix(n), call);
        if (call.start_line > 0 and streq(edge_type, "CALLS") and pos < PROPS_2K - PP_LINE_MARGIN) {
            const ln = c.snprintf(@as([*]u8, props) + pos, PROPS_2K - pos, ",\"line\":%d", call.start_line);
            if (ln > 0) pos += ix(ln);
        }
        if (pos < PROPS_2K - 1) {
            props[pos] = '}';
            props[pos + 1] = 0;
        }
    }
    _ = c.cbm_gbuf_insert_edge(gbuf, src_id, tgt_id, edge_type, props);
}

/// Build Route node QN and properties for HTTP/async service edges.
fn buildServiceRoute(gbuf: ?*Gbuf, arg: CStr, method: CStr, broker: CStr, svc: c.cbm_svc_kind_t) i64 {
    var route_qn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
    var prefix: CStr = undefined;
    var cpath: [c.CBM_SZ_256]u8 = undefined;
    var qpath: CStr = arg;
    if (svc == c.CBM_SVC_HTTP) {
        prefix = if (method != null) method else "ANY";
        qpath = c.cbm_route_canon_path(arg, &cpath, cpath.len);
    } else {
        prefix = if (broker != null) broker else "async";
    }
    _ = c.snprintf(&route_qn, route_qn.len, "__route__%s__%s", prefix, qpath);
    var route_props: [c.CBM_SZ_256]u8 = undefined;
    if (method != null) {
        _ = c.snprintf(&route_props, route_props.len, "{\"method\":\"%s\"}", method);
    } else if (broker != null) {
        _ = c.snprintf(&route_props, route_props.len, "{\"broker\":\"%s\"}", broker);
    } else {
        _ = c.snprintf(&route_props, route_props.len, "{}");
    }
    return c.cbm_gbuf_upsert_node(gbuf, "Route", arg, &route_qn, "", 0, 0, &route_props);
}

/// Emit HTTP_CALLS or ASYNC_CALLS edge via Route node.
fn emitHttpAsyncServiceEdge(gbuf: ?*Gbuf, source: *const Node, call: *const c.CBMCall, res: *const c.cbm_resolution_t, svc: c.cbm_svc_kind_t, arg: CStr) void {
    const edge_type: CStr = if (svc == c.CBM_SVC_HTTP) "HTTP_CALLS" else "ASYNC_CALLS";
    const method: CStr =
        if (svc == c.CBM_SVC_HTTP) c.cbm_service_pattern_http_method(call.callee_name) else null;
    const broker: CStr =
        if (svc == c.CBM_SVC_ASYNC) c.cbm_service_pattern_broker(res.qualified_name) else null;

    const route_id = buildServiceRoute(gbuf, arg, method, broker, svc);

    var esc_c: [c.CBM_SZ_256]u8 = undefined;
    var esc_a: [c.CBM_SZ_256]u8 = undefined;
    _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
    _ = c.cbm_json_escape(&esc_a, esc_a.len, arg);
    var props: [PROPS_2K]u8 = undefined;
    const base: [*]u8 = &props;
    var n = c.snprintf(&props, props.len, "{\"callee\":\"%s\",\"url_path\":\"%s\"", @as(CStr, &esc_c), @as(CStr, &esc_a));
    if (method != null) {
        n += c.snprintf(base + ix(n), props.len -% ix(n), ",\"method\":\"%s\"", method);
    }
    if (broker != null) {
        n += c.snprintf(base + ix(n), props.len -% ix(n), ",\"broker\":\"%s\"", broker);
    }
    finalizeAndEmit(gbuf, source.id, route_id, edge_type, &props, n, call);
}

/// Emit CONFIGURES edge.
fn emitConfigEdge(gbuf: ?*Gbuf, source: *const Node, target_opt: ?*const Node, call: *const c.CBMCall, res: *const c.cbm_resolution_t, arg: CStr) void {
    // emit_service_edge may be reached with target==NULL on the HTTP/ASYNC
    // external-client bypass (#523); a CONFIGURES edge needs a real target, so
    // never deref a NULL target here.
    const target = target_opt orelse return;
    var esc_c: [c.CBM_SZ_256]u8 = undefined;
    var esc_k: [c.CBM_SZ_256]u8 = undefined;
    _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
    _ = c.cbm_json_escape(&esc_k, esc_k.len, if (arg != null) arg else "");
    var props: [PROPS_2K]u8 = undefined;
    const n = c.snprintf(
        &props,
        props.len,
        "{\"callee\":\"%s\",\"key\":\"%s\",\"confidence\":%.2f",
        @as(CStr, &esc_c),
        @as(CStr, &esc_k),
        res.confidence,
    );
    finalizeAndEmit(gbuf, source.id, target.id, "CONFIGURES", &props, n, call);
}

/// Emit normal CALLS edge.
fn emitNormalCallsEdge(gbuf: ?*Gbuf, source: *const Node, target_opt: ?*const Node, call: *const c.CBMCall, res: *const c.cbm_resolution_t) void {
    // A CALLS edge needs a real target; the HTTP/ASYNC external-client bypass
    // (#523) can reach emit_service_edge with target==NULL, so guard the deref.
    const target = target_opt orelse return;
    var esc_c: [c.CBM_SZ_256]u8 = undefined;
    _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
    var props: [PROPS_2K]u8 = undefined;
    const n = c.snprintf(
        &props,
        props.len,
        "{\"callee\":\"%s\",\"confidence\":%.2f,\"strategy\":\"%s\",\"candidates\":%d",
        @as(CStr, &esc_c),
        res.confidence,
        @as(CStr, if (res.strategy != null) res.strategy else "unknown"),
        res.candidate_count,
    );
    finalizeAndEmit(gbuf, source.id, target.id, "CALLS", &props, n, call);
}

// Classify a resolved call by library identity and emit the appropriate edge.
/// Create Route node + CALLS + HANDLES edges for a route registration call.
/// route_mount is the framework mount of the registering file ("/api" for a
/// Laravel 11+ `withRouting(api: ...)` file, #1146) or "".
fn emitRouteRegistration(
    gbuf: ?*Gbuf,
    source: *const Node,
    call: *const c.CBMCall,
    route_path_in: CStr,
    handler_ref: CStr,
    module_qn: CStr,
    registry: ?*const Registry,
    main_gbuf: ?*const Gbuf,
    ik: StrList,
    iv: StrList,
    ic: c_int,
    route_mount: CStr,
) void {
    const method: CStr = c.cbm_service_pattern_route_method(call.callee_name);
    const method_or_any: CStr = if (method != null) method else "ANY";
    var mounted: [c.CBM_SZ_256]u8 = undefined;
    const route_path: CStr = c.cbm_laravel_mount_route(route_mount, route_path_in, &mounted, mounted.len);
    var rqn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
    var cpath: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&rqn, rqn.len, "__route__%s__%s", method_or_any, c.cbm_route_canon_path(route_path, &cpath, cpath.len));
    var rp: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&rp, rp.len, "{\"method\":\"%s\"}", method_or_any);
    const rid = c.cbm_gbuf_upsert_node(gbuf, "Route", route_path, &rqn, "", 0, 0, &rp);
    var esc_cn: [c.CBM_SZ_256]u8 = undefined; // sliced source text: escape quotes/newlines
    var esc_rp: [c.CBM_SZ_512]u8 = undefined;
    _ = c.cbm_json_escape(&esc_cn, esc_cn.len, call.callee_name);
    _ = c.cbm_json_escape(&esc_rp, esc_rp.len, route_path);
    var props: [c.CBM_SZ_1K]u8 = undefined;
    _ = c.snprintf(
        &props,
        props.len,
        "{\"callee\":\"%s\",\"url_path\":\"%s\",\"via\":\"route_registration\"}",
        @as(CStr, &esc_cn),
        @as(CStr, &esc_rp),
    );
    _ = c.cbm_gbuf_insert_edge(gbuf, source.id, rid, "CALLS", &props);
    if (handler_ref != null and handler_ref[0] != 0) {
        const hres = c.cbm_registry_resolve_handler(registry, handler_ref, module_qn, ik, iv, ic, main_gbuf);
        if (hres.qualified_name != null and hres.qualified_name[0] != 0) {
            const h: ?*const Node = c.cbm_gbuf_find_by_qn(main_gbuf, hres.qualified_name);
            if (h) |handler| {
                // must exceed escaped value + wrapper or snprintf cuts the closing brace
                var hp: [c.CBM_SZ_1K]u8 = undefined;
                var esc_h2: [c.CBM_SZ_512]u8 = undefined;
                _ = c.cbm_json_escape(&esc_h2, esc_h2.len, hres.qualified_name);
                _ = c.snprintf(&hp, hp.len, "{\"handler\":\"%s\"}", @as(CStr, &esc_h2));
                _ = c.cbm_gbuf_insert_edge(gbuf, handler.id, rid, "HANDLES", &hp);
            }
        }
    }
}

/// Reject regex metacharacters, spaces, double-slashes in URL candidates.
fn isJunkUrl(s: CStr) bool {
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {
        const ch = s[i];
        if (ch == '\\' or ch == '^' or ch == '$' or ch == '*' or ch == '+' or ch == '(' or
            ch == ')' or ch == '[' or ch == ']' or ch == '|' or ch == ' ')
        {
            return true;
        }
        if (ch == '/' and i > 0 and s[i - 1] == '/') return true;
    }
    return false;
}

/// Normalize a template literal URL and reject junk patterns.
/// Returns true if norm contains a valid API path.
fn normalizeUrlArg(url: CStr, norm: [*]u8, norm_sz: usize) bool {
    var ni: usize = 0;
    var p = url;
    if (p[0] == '`' or p[0] == '"' or p[0] == '\'') p += 1;
    if (p[0] != '/') return false;
    while (p[0] != 0 and ni < norm_sz - 2) {
        if (p[0] == '$' and p[1] == '{') {
            norm[ni] = ':';
            ni += 1;
            p += 2;
            while (p[0] != 0 and p[0] != '}' and ni < norm_sz - 2) {
                norm[ni] = p[0];
                ni += 1;
                p += 1;
            }
            if (p[0] == '}') p += 1;
        } else if (p[0] == '`' or p[0] == '"' or p[0] == '\'' or p[0] == '?') {
            break;
        } else {
            norm[ni] = p[0];
            ni += 1;
            p += 1;
        }
    }
    norm[ni] = 0;
    const MIN_URL_LEN = 4;
    if (ni < MIN_URL_LEN or c.strchr(norm + 1, '/') == null) return false;
    return !isJunkUrl(norm);
}

fn argIsTopic(arg: *const c.CBMCallArg) bool {
    const keywords = [_][*:0]const u8{
        "topic", "topic_id", "topic_name", "queue", "queue_name", "queue_id", "subject", "channel",
    };
    if (arg.keyword == null) return false;
    for (keywords) |kw| {
        if (streq(arg.keyword, kw)) return true;
    }
    return false;
}

/// File-language scope of the client-navigation heuristic (#1250): the JS/TS
/// family and the hosts that embed it. Kept explicit so changes to the
/// weak-member guard cannot broaden it.
fn ppClientNavLang(lang: Language) bool {
    return lang == c.CBM_LANG_TYPESCRIPT or lang == c.CBM_LANG_TSX or lang == c.CBM_LANG_JAVASCRIPT or
        lang == c.CBM_LANG_VUE or lang == c.CBM_LANG_SVELTE or lang == c.CBM_LANG_HTML or
        lang == c.CBM_LANG_ASTRO;
}

/// Detect API paths in call arguments and create HTTP_CALLS edges.
fn detectUrlInArgs(gbuf: ?*Gbuf, source: *const Node, call: *const c.CBMCall, lang: Language, svc: c.cbm_svc_kind_t) void {
    // A resolved alias may be ASYNC even when its source spelling is unknown.
    // Neither raw slash topics nor URL projections are HTTP evidence there.
    if (svc == c.CBM_SVC_ASYNC or c.cbm_service_pattern_match(call.callee_name) == c.CBM_SVC_ASYNC) {
        return;
    }
    if (c.cbm_service_pattern_is_client_navigation(ppClientNavLang(lang), call.callee_name)) return;
    // A bare `fetch` reaching here resolved to the project's own `fetch`,
    // which shadows the global API (#856): a plain call, not HTTP.
    if (c.cbm_service_pattern_is_global_fetch(call.callee_name)) return;
    for (0..ix(call.arg_count)) |ai| {
        const ca = &call.args[ai];
        if (argIsTopic(ca)) continue;
        // A slash-prefixed raw expression is not a URL string. In JS/TS this
        // is notably a regex literal (`/<table/i`); genuine string literals
        // and propagated constants are carried in `value`, while template
        // literals keep their leading backtick in `expr`.
        if (ca.value == null and ca.url_value == null and ca.expr != null and ca.expr[0] == '/') {
            continue;
        }
        const url: CStr = if (ca.url_value != null)
            ca.url_value
        else if (ca.value != null) ca.value else ca.expr;
        if (url == null or (url[0] != '/' and url[0] != '`')) continue;
        var norm: [c.CBM_SZ_256]u8 = undefined;
        if (!normalizeUrlArg(url, &norm, norm.len)) continue;
        if (!c.cbm_service_pattern_is_http_route_literal(&norm, call.callee_name)) continue;
        var route_qn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
        var cpath: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&route_qn, route_qn.len, "__route__ANY__%s", c.cbm_route_canon_path(&norm, &cpath, cpath.len));
        const route_id = c.cbm_gbuf_upsert_node(gbuf, "Route", &norm, &route_qn, "", 0, 0, "{\"source\":\"arg_url\"}");
        var esc_c: [c.CBM_SZ_256]u8 = undefined;
        var esc_n: [c.CBM_SZ_256]u8 = undefined;
        _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
        _ = c.cbm_json_escape(&esc_n, esc_n.len, &norm);
        var eprops: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(
            &eprops,
            eprops.len,
            "{\"callee\":\"%s\",\"url_path\":\"%s\",\"via\":\"arg_url\"}",
            @as(CStr, &esc_c),
            @as(CStr, &esc_n),
        );
        _ = c.cbm_gbuf_insert_edge(gbuf, source.id, route_id, "HTTP_CALLS", &eprops);
        break;
    }
}

/// Extract gRPC service and method from a callee name.
/// Handles patterns like: pb.NewFooServiceClient(conn).GetBar → Foo/GetBar
/// Also: FooServiceGrpc.newBlockingStub(ch).getBar → FooService/getBar
export fn extract_grpc_service_method(callee: CStr, service: MutStr, srv_sz: usize, method: MutStr, meth_sz: usize) bool {
    service[0] = 0;
    method[0] = 0;
    if (callee == null) return false;
    // Find last dot to split service.Method
    const last_dot: CStr = c.strrchr(callee, '.');
    if (last_dot == null or last_dot[1] == 0) return false;
    _ = c.snprintf(method, meth_sz, "%s", last_dot + 1);

    // Extract service name: everything before the last dot, stripped of prefixes/suffixes
    var prefix_len = ptrDiff(last_dot, callee);
    var raw: [c.CBM_SZ_256]u8 = undefined;
    if (prefix_len >= raw.len) prefix_len = raw.len - 1;
    @memcpy(raw[0..prefix_len], callee[0..prefix_len]);
    raw[prefix_len] = 0;

    // Strip common prefixes: pb.New, New, pb.
    var s: CStr = &raw;
    if (c.strncmp(s, "pb.New", 6) == 0) {
        s += 6;
    } else if (c.strncmp(s, "pb.", 3) == 0 or c.strncmp(s, "New", 3) == 0) {
        s += 3;
    }

    // Strip the generated-stub/client suffix, preserving the canonical
    // proto-declared service name. The proto service is `<X>Service`; the
    // generated client is `<X>ServiceClient` / `<X>ServiceGrpc`, so we strip
    // only the trailing stub/client token (Client/Stub/Grpc/…), NOT "Service"
    // itself — stripping "ServiceClient" yielded `<X>` and broke cross-repo
    // matching against the `<X>Service` declared name (#294). Longest tokens
    // first so e.g. BlockingStub wins over Stub.
    //
    // A match also serves as the gRPC stub-type signal: we ONLY emit a Route
    // when a recognized suffix is actually present. Without this gate the
    // fallback turned ordinary receiver vars (`_provider.GetGroup`,
    // `_builder.AddSomeService`) into phantom `__grpc__provider/...` Routes
    // that correspond to no .proto anywhere (#294).
    _ = c.snprintf(service, srv_sz, "%s", s);
    const slen = c.strlen(service);
    const suffixes = [_][:0]const u8{
        "BlockingStub", "FutureStub", "AsyncStub", "AsyncClient", "Servicer", "Client", "Stub", "Grpc",
    };
    var stripped = false;
    for (suffixes) |sfx| {
        if (slen > sfx.len and streq(service + slen - sfx.len, sfx.ptr)) {
            service[slen - sfx.len] = 0;
            stripped = true;
            break;
        }
    }

    return stripped and service[0] != 0 and method[0] != 0;
}

/// Emit GRPC_CALLS edge via gRPC Route node.
fn emitGrpcEdge(gbuf: ?*Gbuf, source: *const Node, call: *const c.CBMCall, res: *const c.cbm_resolution_t) void {
    var service: [c.CBM_SZ_256]u8 = undefined;
    var method: [c.CBM_SZ_256]u8 = undefined;
    // Try callee_name first (e.g., "pb.NewCartServiceClient.GetCart")
    if (!extract_grpc_service_method(call.callee_name, &service, service.len, &method, method.len)) {
        // Fallback: try the resolved QN for Go chained calls.
        // Go pattern: pb.NewCartServiceClient(conn).GetCart(ctx, req)
        // callee_name = "GetCart", QN = "...CartServiceClient.GetCart"
        // The QN contains the full ServiceClient.Method pattern.
        if (res.qualified_name == null or
            !extract_grpc_service_method(res.qualified_name, &service, service.len, &method, method.len))
        {
            return;
        }
    }
    const svc_name: CStr = &service;
    const method_name: CStr = &method;

    var route_qn: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(&route_qn, route_qn.len, "__grpc__%s/%s", svc_name, method_name);

    var route_name: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&route_name, route_name.len, "%s/%s", svc_name, method_name);

    const route_id = c.cbm_gbuf_upsert_node(gbuf, "Route", &route_name, &route_qn, "", 0, 0, "{\"source\":\"grpc\"}");

    var esc_c: [c.CBM_SZ_256]u8 = undefined;
    _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
    var props: [c.CBM_SZ_1K]u8 = undefined;
    _ = c.snprintf(
        &props,
        props.len,
        "{\"callee\":\"%s\",\"service\":\"%s\",\"method\":\"%s\",\"confidence\":%.2f}",
        @as(CStr, &esc_c),
        svc_name,
        method_name,
        res.confidence,
    );
    _ = c.cbm_gbuf_insert_edge(gbuf, source.id, route_id, "GRPC_CALLS", &props);
}

/// Emit GRAPHQL_CALLS edge. The Route is keyed by the operation NAME only
/// (#598), never by the operation text:
///   named operation          -> __graphql__<Name>                  name "<Name>"
///   anonymous query/mutation/ -> __graphql__<type>__anonymous       name "(anonymous <type>)"
///   subscription (incl. the `{ ... }` query shorthand)
///   no GraphQL document      -> __graphql__operation__anonymous    name "(anonymous operation)"
///   (URL, variable, missing string argument)
/// Keys are bounded by the GraphQL Name grammar and deterministic.
fn emitGraphqlEdge(gbuf: ?*Gbuf, source: *const Node, call: *const c.CBMCall, res: *const c.cbm_resolution_t) void {
    var op_type: CStr = "operation";
    var op_name: [c.CBM_SZ_128]u8 = undefined;
    const named = c.cbm_service_pattern_graphql_operation(call.first_string_arg, &op_type, &op_name, op_name.len);

    var route_name: [c.CBM_SZ_256]u8 = undefined;
    var route_qn: [c.CBM_SZ_256]u8 = undefined;
    if (named) {
        _ = c.snprintf(&route_name, route_name.len, "%s", @as(CStr, &op_name));
        _ = c.snprintf(&route_qn, route_qn.len, "__graphql__%s", @as(CStr, &op_name));
    } else {
        _ = c.snprintf(&route_name, route_name.len, "(anonymous %s)", op_type);
        _ = c.snprintf(&route_qn, route_qn.len, "__graphql__%s__anonymous", op_type);
    }

    const route_id = c.cbm_gbuf_upsert_node(gbuf, "Route", &route_name, &route_qn, "", 0, 0, "{\"source\":\"graphql\"}");

    var esc_c: [c.CBM_SZ_256]u8 = undefined;
    var esc_op: [c.CBM_SZ_512]u8 = undefined;
    _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
    _ = c.cbm_json_escape(&esc_op, esc_op.len, &route_name);
    var props: [c.CBM_SZ_1K]u8 = undefined;
    _ = c.snprintf(
        &props,
        props.len,
        "{\"callee\":\"%s\",\"operation\":\"%s\",\"operation_type\":\"%s\"," ++
            "\"confidence\":%.2f}",
        @as(CStr, &esc_c),
        @as(CStr, &esc_op),
        op_type,
        res.confidence,
    );
    _ = c.cbm_gbuf_insert_edge(gbuf, source.id, route_id, "GRAPHQL_CALLS", &props);
}

/// Emit TRPC_CALLS edge. Extract procedure path from callee chain.
fn emitTrpcEdge(gbuf: ?*Gbuf, source: *const Node, call: *const c.CBMCall, res: *const c.cbm_resolution_t) void {
    // tRPC calls: trpc.user.getById.query() → extract "user.getById"
    const callee: CStr = call.callee_name;
    if (callee == null) return;
    // Strip trailing .query/.mutate/.subscribe
    var proc: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&proc, proc.len, "%s", callee);
    const last_dot: MutStr = c.strrchr(&proc, '.');
    if (last_dot != null and (streq(last_dot, ".query") or streq(last_dot, ".mutate") or
        streq(last_dot, ".subscribe") or streq(last_dot, ".useQuery") or
        streq(last_dot, ".useMutation")))
    {
        last_dot[0] = 0;
    }
    // Strip leading trpc.
    var p: CStr = &proc;
    if (c.strncmp(p, "trpc.", 5) == 0) p += 5;

    var route_qn: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(&route_qn, route_qn.len, "__trpc__%s", p);

    const route_id = c.cbm_gbuf_upsert_node(gbuf, "Route", p, &route_qn, "", 0, 0, "{\"source\":\"trpc\"}");

    var esc_c: [c.CBM_SZ_256]u8 = undefined;
    _ = c.cbm_json_escape(&esc_c, esc_c.len, call.callee_name);
    var props: [c.CBM_SZ_1K]u8 = undefined;
    _ = c.snprintf(
        &props,
        props.len,
        "{\"callee\":\"%s\",\"procedure\":\"%s\",\"confidence\":%.2f}",
        @as(CStr, &esc_c),
        p,
        res.confidence,
    );
    _ = c.cbm_gbuf_insert_edge(gbuf, source.id, route_id, "TRPC_CALLS", &props);
}

/// When suppress_plain_calls is true (a TS/JS/TSX weak short-name member-call
/// match, #592/#606), every service classification below still runs — only the
/// plain CALLS fall-through (emitNormalCallsEdge) is skipped. detectUrlInArgs
/// and the HTTP/ASYNC/gRPC/GraphQL/tRPC/CONFIG/route branches are unaffected, so
/// a verb-suffix HTTP client (api.patch('/x')), broker, or route registration
/// keeps its edge; only the fabricated project CALLS edge is dropped.
fn emitServiceEdge(
    gbuf: ?*Gbuf,
    source: *const Node,
    target: ?*const Node,
    call: *const c.CBMCall,
    res: *const c.cbm_resolution_t,
    module_qn: CStr,
    registry: ?*const Registry,
    main_gbuf: ?*const Gbuf,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
    suppress_plain_calls: bool,
    lang: Language,
    route_mount: CStr,
) void {
    var svc = c.cbm_service_pattern_match(res.qualified_name);
    const arg: CStr = call.first_string_arg;

    // Also detect route registration by callee name suffix alone (handles unresolved
    // local variables like app.include_router where QN resolution fails).
    if (svc == c.CBM_SVC_NONE and c.cbm_service_pattern_route_method(call.callee_name) != null) {
        svc = c.CBM_SVC_ROUTE_REG;
    }

    // Detect gRPC stub method calls by resolved QN.
    // Go pattern: pb.NewCartServiceClient(conn).GetCart(ctx, req)
    // Tree-sitter extracts GetCart as the callee, which resolves to the
    // generated pb interface method (QN contains "ServiceClient").
    if (svc == c.CBM_SVC_NONE and res.qualified_name != null) {
        if (c.strstr(res.qualified_name, "ServiceClient") != null or
            c.strstr(res.qualified_name, "ServiceGrpc") != null or
            c.strstr(res.qualified_name, "Servicer") != null)
        {
            svc = c.CBM_SVC_GRPC;
        }
    }

    if (svc == c.CBM_SVC_ROUTE_REG) {
        var handler_ref: CStr = null;
        const route_path = findRoutePathInArgs(call, &handler_ref);
        if (route_path != null) {
            emitRouteRegistration(gbuf, source, call, route_path, handler_ref, module_qn, registry, main_gbuf, imp_keys, imp_vals, imp_count, route_mount);
            return;
        }
        // No path found — fall through to normal CALLS edge
    }

    const has_arg = arg != null and arg[0] != 0;
    const has_url = has_arg and (arg[0] == '/' or c.strstr(arg, "://") != null);
    const has_topic = has_arg and svc == c.CBM_SVC_ASYNC and c.strlen(arg) > PP_ESC_SPACE;
    // Set when this call already has its HTTP_CALLS edge from the service
    // patterns (typed with the verb its callee names).
    var http_edge_emitted = false;

    if ((svc == c.CBM_SVC_HTTP or svc == c.CBM_SVC_ASYNC) and (has_url or has_topic)) {
        emitHttpAsyncServiceEdge(gbuf, source, call, res, svc, arg);
        http_edge_emitted = svc == c.CBM_SVC_HTTP;
    } else if (svc == c.CBM_SVC_GRPC) {
        emitGrpcEdge(gbuf, source, call, res);
    } else if (svc == c.CBM_SVC_GRAPHQL) {
        emitGraphqlEdge(gbuf, source, call, res);
    } else if (svc == c.CBM_SVC_TRPC) {
        emitTrpcEdge(gbuf, source, call, res);
    } else if (svc == c.CBM_SVC_CONFIG) {
        emitConfigEdge(gbuf, source, target, call, res, arg);
    } else if (!suppress_plain_calls) {
        emitNormalCallsEdge(gbuf, source, target, call, res);
    }

    // The arg-URL heuristic is for calls no service pattern knows (a local
    // fetch wrapper). On a classified HTTP call that already has its typed
    // edge, skip it so we do not mint a second, method-less ANY edge.
    if (!http_edge_emitted) detectUrlInArgs(gbuf, source, call, lang, svc);
}

/// The #725 guard refuses a suffix_match binding across a language boundary,
/// which leaves the callee unresolved. An unresolved callee the route
/// classifier recognises is still a registration: `Route::get('/x', ...)`
/// beside a JS `get`, `app.get('/x', h)` beside a Python `get`. Emit exactly
/// the Route + CALLS + HANDLES that the unresolved-callee (callee_suffix)
/// fallback in resolveFileCalls emits for it, classified by callee name the
/// same way emitServiceEdge classifies that fallback, and nothing else: no
/// CALLS edge to the refused target and no URL-argument scan. Dropping the
/// whole call lost every GET registration in a mixed-language repo while POST
/// (no `post` to collide with) survived.
fn emitXlangRefusedRoute(
    gbuf: ?*Gbuf,
    source: *const Node,
    call: *const c.CBMCall,
    module_qn: CStr,
    registry: ?*const Registry,
    main_gbuf: ?*const Gbuf,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
    route_mount: CStr,
) void {
    if (c.cbm_service_pattern_route_method(call.callee_name) == null) return;
    const svc = c.cbm_service_pattern_match(call.callee_name);
    if (svc != c.CBM_SVC_NONE and svc != c.CBM_SVC_ROUTE_REG) return;
    var handler_ref: CStr = null;
    const route_path = findRoutePathInArgs(call, &handler_ref);
    if (route_path != null) {
        emitRouteRegistration(gbuf, source, call, route_path, handler_ref, module_qn, registry, main_gbuf, imp_keys, imp_vals, imp_count, route_mount);
    }
}

/// Find the source node for an edge: enclosing function or file node.
fn findSourceNode(gbuf: ?*const Gbuf, project: CStr, rel: CStr, enclosing_qn: CStr) ?*const Node {
    var src: ?*const Node = null;
    if (enclosing_qn != null) {
        src = c.cbm_gbuf_find_by_qn(gbuf, enclosing_qn);
        // A class-level reference in a directory-module language carries the
        // DIRECTORY module QN, which hits the shared Folder/Project node —
        // attribute to this file's File node instead (#787).
        if (c.cbm_pipeline_node_is_dir_container(src)) src = null;
    }
    if (src == null) {
        const file_qn: MutStr = c.cbm_pipeline_fqn_compute(project, rel, "__file__");
        src = c.cbm_gbuf_find_by_qn(gbuf, file_qn);
        c.free(file_qn);
    }
    return src;
}

/// Field type hint resolution for obj.Method() with multiple candidates.
/// Strips C# field prefixes (_ / m_), capitalizes to get type name, and
/// checks if TypeName.Method or ITypeName.Method exists among candidates.
fn tryFieldTypeHint(rc: *ResolveCtx, res: *c.cbm_resolution_t, callee_name: CStr, source_id: i64) void {
    if (res.qualified_name == null or res.candidate_count <= 1) return;
    const dot: CStr = c.strchr(callee_name, '.');
    if (dot == null) return;
    const plen = ptrDiff(dot, callee_name);
    var obj_name: [c.CBM_SZ_256]u8 = undefined;
    if (plen >= obj_name.len) return;
    @memcpy(obj_name[0..plen], callee_name[0..plen]);
    obj_name[plen] = 0;

    var type_hint: CStr = &obj_name;
    if (type_hint[0] == '_') type_hint += 1;
    if (type_hint[0] == 'm' and type_hint[1] == '_') type_hint += PP_CSHARP_M_PREFIX_LEN;

    var type_name: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&type_name, type_name.len, "%s", type_hint);
    if (type_name[0] >= 'a' and type_name[0] <= 'z') type_name[0] -= 'a' - 'A';

    var iface_name: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&iface_name, iface_name.len, "I%s", @as(CStr, &type_name));

    const method: CStr = dot + 1;
    var cands: StrList = null;
    var cand_count: c_int = 0;
    _ = c.cbm_registry_find_by_name(rc.registry, method, &cands, &cand_count);
    for (0..ix(cand_count)) |ci| {
        if (c.strstr(cands[ci], &type_name) != null or c.strstr(cands[ci], &iface_name) != null) {
            const better: ?*const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, cands[ci]);
            if (better != null and better.?.id != source_id) {
                res.qualified_name = cands[ci];
                res.confidence = PP_FIELD_HINT_CONF;
                res.strategy = "field_type_hint";
                return;
            }
        }
    }
}

/// Free a strdup'd key stored in the per-file lsp_idx hash table.
fn lspIdxFreeKey(key: CStr, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = value;
    _ = ud;
    c.free(@constCast(key));
}

/// Adds the time since `t0` to a per-phase counter.
inline fn addElapsed(counter: *AtomicU64, t0: u64) void {
    _ = counter.fetchAdd(extractNowNs() -% t0, .monotonic);
}

/// A resolution that stands in for a call classified by its callee name.
fn nameResolution(name: CStr, strategy: CStr) c.cbm_resolution_t {
    var res = std.mem.zeroes(c.cbm_resolution_t);
    res.qualified_name = name;
    res.confidence = PP_HALF_CONF;
    res.strategy = strategy;
    return res;
}

/// A non-empty string argument that is a URL path or an absolute URL.
fn isUrlArg(u: CStr) bool {
    return u != null and u[0] != 0 and (u[0] == '/' or c.strstr(u, "://") != null);
}

/// isUrlArg, or for an async service any topic longer than two bytes.
fn isUrlOrTopicArg(u: CStr, svc: c.cbm_svc_kind_t) bool {
    return u != null and u[0] != 0 and
        (u[0] == '/' or c.strstr(u, "://") != null or
            (svc == c.CBM_SVC_ASYNC and c.strlen(u) > PP_ESC_SPACE));
}

/// Resolve calls for one file and emit CALLS/HTTP_CALLS/ASYNC_CALLS edges.
fn resolveFileCalls(
    rc: *ResolveCtx,
    ws: *ResolveWorkerState,
    result: *FileResult,
    rel: CStr,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
    lang: Language,
) void {
    // Framework mount of this file's routes (Laravel 11+ withRouting, #1146).
    var route_mount: [c.CBM_SZ_128]u8 = undefined;
    _ = c.cbm_laravel_file_route_mount(rc.repo_path, rel, lang, result, &route_mount, route_mount.len);

    // Build a per-file hash index of resolved_calls keyed by
    // "caller_qn|callee_short" for O(1) lookup. cbm_pipeline_find_lsp_
    // resolution would otherwise do an O(N) linear scan over
    // resolved_calls for EACH of result->calls.count calls — the
    // dominant cost in parallel_resolve on kubernetes (~50s of pure
    // scanning). On insert, keep the highest-confidence entry per key
    // (matches the original "best" tie-break). Skip the build entirely
    // when there are no calls (nothing to look up) or no resolved
    // entries (lookups would all miss).
    var lsp_idx: ?*HashTable = null;
    if (result.calls.count > 0 and result.resolved_calls.count > 0) {
        lsp_idx = c.cbm_ht_create(@as(u32, @bitCast(result.resolved_calls.count)) *% 2 +% 16);
        if (lsp_idx != null) {
            for (0..ix(result.resolved_calls.count)) |i| {
                const rc_e = util.at(result.resolved_calls.items, i);
                if (rc_e.caller_qn == null or rc_e.callee_qn == null or
                    rc_e.kind != c.CBM_RESOLVED_INVOCATION or
                    rc_e.confidence < c.CBM_LSP_CONFIDENCE_FLOOR)
                {
                    continue;
                }
                var short_name: CStr = c.strrchr(rc_e.callee_qn, '.');
                short_name = if (short_name != null) short_name + 1 else rc_e.callee_qn;
                var key: [1024]u8 = undefined;
                // A row with a site span proves one call site: key it by that
                // site so it never answers a same-named call elsewhere in the
                // caller (lsp_resolve.zig siteAdmits).
                const kn = if (rc_e.site_end_byte > rc_e.site_start_byte)
                    c.snprintf(&key, key.len, "%s|%s@%u", rc_e.caller_qn, short_name, rc_e.site_start_byte)
                else
                    c.snprintf(&key, key.len, "%s|%s", rc_e.caller_qn, short_name);
                if (kn <= 0 or kn >= @as(c_int, key.len)) continue;
                const existing: ?*c.CBMResolvedCall = @ptrCast(@alignCast(c.cbm_ht_get(lsp_idx, &key)));
                if (existing == null) {
                    // New entry — strdup so the key outlives the loop body.
                    const kdup: MutStr = c.strdup(&key);
                    if (kdup != null) _ = c.cbm_ht_set(lsp_idx, kdup, rc_e);
                } else if (rc_e.confidence > existing.?.confidence) {
                    // Update value; reuse stored key pointer to avoid leak.
                    const skey: CStr = c.cbm_ht_get_key(lsp_idx, &key);
                    if (skey != null) _ = c.cbm_ht_set(lsp_idx, skey, rc_e);
                }
            }
        }
    }

    for (0..ix(result.calls.count)) |ci| {
        const call = util.at(result.calls.items, ci);
        if (call.callee_name == null) continue;
        var t0 = extractNowNs();
        const source_node_opt = findSourceNode(rc.main_gbuf, rc.project_name, rel, call.enclosing_func_qn);
        addElapsed(&rc.time_ns_rc_source, t0);
        const source_node = source_node_opt orelse continue;

        // LSP-resolved calls take precedence over registry textual matching.
        // The shared helper (lsp_resolve.zig) and CBM_LSP_CONFIDENCE_FLOOR
        // decide which LSP overrides are admitted. Unique-tail fallbacks
        // are JVM-only (see cbm_pipeline_lsp_allow_tail_match).
        const allow_tail = c.cbm_pipeline_lsp_allow_tail_match(lang);
        var res = std.mem.zeroes(c.cbm_resolution_t);
        var lsp: ?*const c.CBMResolvedCall = null;
        t0 = extractNowNs();
        if (lsp_idx != null and call.enclosing_func_qn != null) {
            const call_leaf: CStr = lsp_resolve.bareSegment(call.callee_name);
            var key: [1024]u8 = undefined;
            var kn: c_int = if (call_leaf != null)
                c.snprintf(&key, key.len, "%s|%s@%u", call.enclosing_func_qn, call_leaf, call.site_start_byte)
            else
                -1;
            if (kn > 0 and kn < @as(c_int, key.len)) {
                lsp = @ptrCast(@alignCast(c.cbm_ht_get(lsp_idx, &key)));
            }
            kn = if (call_leaf != null and lsp == null)
                c.snprintf(&key, key.len, "%s|%s", call.enclosing_func_qn, call_leaf)
            else
                -1;
            if (kn > 0 and kn < @as(c_int, key.len)) {
                lsp = @ptrCast(@alignCast(c.cbm_ht_get(lsp_idx, &key)));
            }
        }
        if (lsp == null) {
            // Fallback to the linear scan for edge cases the index may
            // miss (e.g. callee_name that wasn't the registered short
            // name). Keeps semantics identical.
            lsp = c.cbm_pipeline_find_lsp_resolution(&result.resolved_calls, call, allow_tail);
        }
        addElapsed(&rc.time_ns_rc_lsp_lookup, t0);
        t0 = extractNowNs();
        var lsp_target: ?*const Node = null;
        if (lsp) |l| {
            // Canonicalise to the gbuf node's QN so res.qualified_name matches
            // the gbuf even when the cross-file fallback had to prefix the
            // project name.
            lsp_target = c.cbm_pipeline_lsp_target_node(rc.main_gbuf, rc.project_name, l.callee_qn, allow_tail);
            if (lsp_target) |target| {
                res.qualified_name = target.qualified_name;
                res.strategy = if (l.strategy != null) l.strategy else "lsp_override";
                res.confidence = l.confidence;
                res.candidate_count = 1;
                ws.lsp_overrides += 1;
            }
        }
        // #1085: fall back to the registry resolver whenever the LSP did not
        // yield a gbuf-resolvable target — whether no LSP resolution existed,
        // OR the LSP was confident but its callee_qn isn't a node in the gbuf
        // (the JSX-via-tsconfig-alias case: the TS LSP resolves the element
        // ref to an alias-path QN that never matches a def node, so lsp_target
        // is NULL). The old `else` ran the registry ONLY when lsp was null, so
        // an LSP-with-unresolvable-target dropped the edge outright — silently
        // losing every alias-imported JSX component edge (~21% of a Next.js
        // call graph). Falling THROUGH to the registry keeps them via the
        // import_map / unique_name resolution.
        if (res.qualified_name == null or res.qualified_name[0] == 0) {
            res = c.cbm_registry_resolve(rc.registry, call.callee_name, module_qn, imp_keys, imp_vals, imp_count);
        }
        addElapsed(&rc.time_ns_rc_resolve, t0);

        t0 = extractNowNs();
        tryFieldTypeHint(rc, &res, call.callee_name, source_node.id);
        addElapsed(&rc.time_ns_rc_hint, t0);

        // Perl call-graph noise guard (#476). Perl has no LSP resolver; for
        // builtins (push/shift/
        // keys/...) and method calls ($obj->m, unresolved receiver), suppress
        // only WEAK cross-file short-name matches and keep the high-confidence
        // same_module / import_map strategies so a genuine same-file or
        // imported call to a builtin-named sub still resolves. Placed after the
        // field-type hint so a hint cannot re-introduce a suppressed edge.
        // Gated to Perl — other languages are unaffected.
        if (c.cbm_perl_suppress_generic_match(lang == c.CBM_LANG_PERL, call.is_method, call.callee_name, res.strategy)) {
            continue;
        }

        // Dynamic-language weak-member suppression (#592/#606/#1276). The
        // receiver-aware guard must NOT drop this call here: doing so would also
        // skip the #523 callee-name service bypass below, emit_service_edge's
        // route/gRPC/config branches, and its unconditional detect_url_in_args
        // (which classifies verb-suffix HTTP clients like api.patch('/x')).
        // Instead, defer to the emit path and suppress ONLY the plain-CALLS
        // fall-through (emit_normal_calls_edge), so every service edge stays
        // main-identical by construction. res.strategy may carry an lsp_* value
        // here (LSP-resolved calls keep res through this point); the helper's
        // EXPLICIT drop-list leaves lsp_ts_method / lsp_cross untouched. See
        // #606 direction.
        const suppress_weak_member = lang == c.CBM_LANG_PYTHON or lang == c.CBM_LANG_JAVASCRIPT or
            lang == c.CBM_LANG_TYPESCRIPT or lang == c.CBM_LANG_TSX or lang == c.CBM_LANG_ZIG;
        // Bare-call local-binding suppression. A member call has a receiver the
        // guard above can reason about; a bare `run()` has none, so that guard
        // cannot see this class at all. Python-only today because the
        // extraction flag is set only for Python.
        const suppress_weak_local_binding = lang == c.CBM_LANG_PYTHON;
        const drop_plain_call =
            c.cbm_suppress_weak_member_match(suppress_weak_member, call.is_method, res.strategy) or
            c.cbm_suppress_zig_qualified_member(lang == c.CBM_LANG_ZIG, call.is_method, res.strategy) or
            c.cbm_suppress_weak_local_binding_call(suppress_weak_local_binding, call.callee_is_locally_bound, res.strategy);

        // First: a call on an axios.create() instance (#1916) — decided by the
        // receiver's binding, before the #523 spelling check and the
        // route-registration suffix fallback.
        var client_url: [c.CBM_SZ_512]u8 = undefined;
        if (c.cbm_pipeline_http_client_call_url(rc.main_gbuf, rc.project_name, rel, result, imp_keys, imp_vals, imp_count, call, &client_url, client_url.len)) {
            const svc_res = nameResolution(call.callee_name, "http_client_instance");
            emitHttpAsyncServiceEdge(ws.local_edge_buf, source_node, call, &svc_res, c.CBM_SVC_HTTP, &client_url);
            continue;
        }
        // Service-pattern HTTP/ASYNC client call (`requests.get(url)`): the
        // service signal lives in the callee_name. The registry can mis-resolve
        // it to a spurious builtin short-name match (`requests.get` ->
        // `builtins.dict.get` via "get"), which is non-empty and not an HTTP
        // pattern, so the resolved-QN service checks below miss it and the call
        // is dropped. Detect it on the callee_name FIRST so the HTTP_CALLS/
        // ASYNC_CALLS edge is emitted regardless (target is a synthesized route
        // node, not the unindexed library). (#523)
        const csvc = c.cbm_service_pattern_match(call.callee_name);
        if (csvc == c.CBM_SVC_HTTP or csvc == c.CBM_SVC_ASYNC) {
            if (isUrlOrTopicArg(call.first_string_arg, csvc)) {
                const svc_res = nameResolution(call.callee_name, "service_pattern");
                emitServiceEdge(ws.local_edge_buf, source_node, source_node, call, &svc_res, module_qn, rc.registry, rc.main_gbuf, imp_keys, imp_vals, imp_count, false, lang, &route_mount);
                continue;
            }
        }

        // A call that starts another program (subprocess.run, exec.Command,
        // posix_spawn): a SPAWNS edge to its Process node instead of a CALLS
        // edge. `res` holds the LSP answer when there is one.
        var spawn: spawns.Spawn = undefined;
        if (spawns.site(rc.main_gbuf, lang, call, &result.imports, &res, &spawn)) {
            spawns.emit(ws.local_edge_buf, source_node, call, &spawn);
            continue;
        }

        const unresolved = res.qualified_name == null or res.qualified_name[0] == 0;
        if (unresolved or c.cbm_registry_strategy_is_weak(res.strategy)) {
            // A callee an import binds to an external client library
            // (`send` under `from paho.mqtt import publish as send`):
            // spelled through its import, then classified. The binding
            // outranks a weak name match.
            var spelled: [c.CBM_SZ_512]u8 = undefined;
            const isvc = c.cbm_pipeline_external_import_service(lang, &result.imports, imp_keys, imp_count, call, &spelled, spelled.len);
            if (isvc != c.CBM_SVC_NONE) {
                var through = call.*;
                through.callee_name = &spelled;
                const imp_res = nameResolution(&spelled, "import_binding");
                emitHttpAsyncServiceEdge(ws.local_edge_buf, source_node, &through, &imp_res, isvc, through.first_string_arg);
                ws.calls_resolved += 1;
                continue;
            }
        }
        if (unresolved) {
            if (c.cbm_service_pattern_route_method(call.callee_name) != null) {
                const fake_res = nameResolution(call.callee_name, "callee_suffix");
                // The caller stands in for the unknown target, so a plain
                // CALLS fall-through would be a self-loop.
                emitServiceEdge(ws.local_edge_buf, source_node, source_node, call, &fake_res, module_qn, rc.registry, rc.main_gbuf, imp_keys, imp_vals, imp_count, true, lang, &route_mount);
            } else if (c.cbm_service_pattern_is_global_fetch(call.callee_name)) {
                // Native `fetch()` (#856): only the global API once resolution
                // has failed to find a local/imported `fetch`. Call the low-level
                // emitter directly — emit_service_edge re-derives its own kind
                // from res->qualified_name via cbm_service_pattern_match, which
                // "fetch" deliberately never matches.
                const u: CStr = call.first_string_arg;
                if (isUrlArg(u)) {
                    const fake_res = nameResolution(call.callee_name, "service_pattern");
                    emitHttpAsyncServiceEdge(ws.local_edge_buf, source_node, call, &fake_res, c.CBM_SVC_HTTP, u);
                }
            }
            continue;
        }
        // Reuse lsp_target as target_node when LSP resolved — avoids a
        // second cbm_gbuf_find_by_qn lookup. try_field_type_hint may have
        // upgraded res.qualified_name to a different candidate, in which
        // case we must re-resolve.
        t0 = extractNowNs();
        const target_node: ?*const Node =
            if (lsp_target != null and res.qualified_name == lsp_target.?.qualified_name)
                lsp_target
            else
                c.cbm_gbuf_find_by_qn(rc.main_gbuf, res.qualified_name);
        addElapsed(&rc.time_ns_rc_target, t0);
        if (target_node != null and source_node.id != target_node.?.id and
            c.cbm_suppress_cross_language_suffix_match(lang, target_node.?.file_path, res.strategy))
        {
            // #725: do not emit a suffix_match
            // CALLS edge across a language boundary. A route registration
            // behind the refused binding still gets its Route.
            emitXlangRefusedRoute(ws.local_edge_buf, source_node, call, module_qn, rc.registry, rc.main_gbuf, imp_keys, imp_vals, imp_count, &route_mount);
            continue;
        }
        if (target_node == null or source_node.id == target_node.?.id) {
            // HTTP/ASYNC calls to an EXTERNAL client library (`requests.get(url)`)
            // resolve to an unindexed QN (target_node == NULL), but their edge
            // target is a synthesized route node, not the library — emit them
            // anyway so cross-repo matching has an HTTP_CALLS edge to work with
            // (#523).
            const psvc = c.cbm_service_pattern_match(res.qualified_name);
            if ((psvc == c.CBM_SVC_HTTP or psvc == c.CBM_SVC_ASYNC) and target_node == null) {
                if (isUrlOrTopicArg(call.first_string_arg, psvc)) {
                    emitServiceEdge(ws.local_edge_buf, source_node, null, call, &res, module_qn, rc.registry, rc.main_gbuf, imp_keys, imp_vals, imp_count, false, lang, &route_mount);
                    ws.calls_resolved += 1;
                }
            }
            continue;
        }
        t0 = extractNowNs();
        emitServiceEdge(ws.local_edge_buf, source_node, target_node, call, &res, module_qn, rc.registry, rc.main_gbuf, imp_keys, imp_vals, imp_count, drop_plain_call, lang, &route_mount);
        addElapsed(&rc.time_ns_rc_emit, t0);
        ws.calls_resolved += 1;
    }
    if (lsp_idx != null) {
        c.cbm_ht_foreach(lsp_idx, lspIdxFreeKey, null);
        c.cbm_ht_free(lsp_idx);
    }
}

/// Resolve USAGE and CALL_REFERENCE edges for one file. CALL_REFERENCE only
/// through the exact LSP join, never from the textual registry.
fn resolveFileUsages(
    rc: *ResolveCtx,
    ws: *ResolveWorkerState,
    result: *FileResult,
    rel: CStr,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
    lang: Language,
) void {
    var ref_index = lsp_resolve.RefIndex.build(&result.resolved_calls);
    defer ref_index.deinit();
    const allow_tail = c.cbm_pipeline_lsp_allow_tail_match(lang);
    for (0..ix(result.usages.count)) |u| {
        const usage = util.at(result.usages.items, u);
        if (usage.ref_name == null) continue;
        const src = findSourceNode(rc.main_gbuf, rc.project_name, rel, usage.enclosing_func_qn) orelse continue;
        const join = lsp_resolve.joinLspReference(rc.main_gbuf, rc.project_name, ref_index, usage, allow_tail);
        if (join.kind == .skip or (join.kind == .value and usage.reference_only)) {
            continue;
        }
        var tgt = join.target;
        if (join.kind == .none) {
            const res = c.cbm_registry_resolve(rc.registry, usage.ref_name, module_qn, imp_keys, imp_vals, imp_count);
            if (res.qualified_name == null or res.qualified_name[0] == 0) continue;
            if (!lsp_resolve.referenceFallbackAllowed(lang, usage, res.strategy, imp_keys, imp_count)) {
                continue;
            }
            tgt = c.cbm_gbuf_find_by_qn(rc.main_gbuf, res.qualified_name);
            // #1928: the registry fallback is a bare-name guess — never let it
            // bind a reference across a language boundary.
            if (tgt != null and c.cbm_suppress_cross_language_ref(lang, tgt.?.file_path)) continue;
            // #1942/#1962: a bare Go reference can never denote a struct
            // field; the member half of a selector may.
            if (tgt != null and c.cbm_go_suppress_bare_field_ref(lang == c.CBM_LANG_GO, usage.is_member_access, tgt.?.label)) {
                continue;
            }
        }
        const target = tgt orelse continue;
        if (src.id == target.id) continue;
        var uprops: [c.CBM_SZ_512]u8 = undefined;
        var esc_ref: [c.CBM_SZ_256]u8 = undefined; // sliced source text: escape quotes/newlines
        _ = c.cbm_json_escape(&esc_ref, esc_ref.len, usage.ref_name);
        const callable = join.kind == .callable;
        lsp_resolve.referenceProps(&uprops, &esc_ref, if (callable) join.row else null);
        _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, src.id, target.id, if (callable) "CALL_REFERENCE" else "USAGE", &uprops);
        ws.usages_resolved += 1;
    }
}

/// Resolve throws/raises for one file.
fn resolveFileThrows(
    rc: *ResolveCtx,
    ws: *ResolveWorkerState,
    result: *FileResult,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
) void {
    for (0..ix(result.throws.count)) |t| {
        const thr = util.at(result.throws.items, t);
        if (thr.exception_name == null or thr.enclosing_func_qn == null) continue;
        const src: *const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, thr.enclosing_func_qn) orelse continue;
        const edge_type: CStr = if (isCheckedException(thr.exception_name)) "THROWS" else "RAISES";
        const res = c.cbm_registry_resolve(rc.registry, thr.exception_name, module_qn, imp_keys, imp_vals, imp_count);
        if (res.qualified_name == null or res.qualified_name[0] == 0) continue;
        const tgt: *const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, res.qualified_name) orelse continue;
        if (src.id == tgt.id) continue;
        _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, src.id, tgt.id, edge_type, "{}");
    }
}

/// Resolve reads/writes for one file.
fn resolveFileRw(
    rc: *ResolveCtx,
    ws: *ResolveWorkerState,
    result: *FileResult,
    rel: CStr,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
    lang: Language,
) void {
    for (0..ix(result.rw.count)) |r| {
        const rw = util.at(result.rw.items, r);
        if (rw.var_name == null) continue;
        const src = findSourceNode(rc.main_gbuf, rc.project_name, rel, rw.enclosing_func_qn) orelse continue;
        const res = c.cbm_registry_resolve(rc.registry, rw.var_name, module_qn, imp_keys, imp_vals, imp_count);
        if (res.qualified_name == null or res.qualified_name[0] == 0) continue;
        const tgt: *const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, res.qualified_name) orelse continue;
        if (src.id == tgt.id) continue;
        // #1928: every resolution here is a bare-name registry guess — never
        // let it bind a read/write across a language boundary.
        if (c.cbm_suppress_cross_language_ref(lang, tgt.file_path)) continue;
        // #1942/#1962: a bare Go reference can never denote a struct field;
        // a selector-LHS write (`t.err = x`) may bind it.
        if (c.cbm_go_suppress_bare_field_ref(lang == c.CBM_LANG_GO, rw.is_member_access, tgt.label)) continue;
        const etype: CStr = if (rw.is_write) "WRITES" else "READS";
        _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, src.id, tgt.id, etype, "{}");
    }
}

/// Resolve base_classes → INHERITS / IMPLEMENTS edges for one definition.
fn resolveDefInherits(rc: *ResolveCtx, ws: *ResolveWorkerState, def: *const Definition, node: *const Node, mq: CStr, ik: StrList, iv: StrList, ic: c_int) void {
    if (def.base_classes == null) return;
    var b: usize = 0;
    while (def.base_classes[b] != null) : (b += 1) {
        const bqn = resolveAsClass(rc.registry, def.base_classes[b], mq, ik, iv, ic);
        if (bqn == null) continue;
        const bn: ?*const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, bqn);
        if (bn != null and node.id != bn.?.id) {
            // A base that resolves to an Interface is an IMPLEMENTS relation
            // (Java `implements`, C# `: IFace`, TS `implements`); a Class/
            // Type/Enum base is plain INHERITS.
            const base_label: CStr = c.cbm_registry_label_of(rc.registry, bqn);
            const edge_type: CStr =
                if (base_label != null and streq(base_label, "Interface")) "IMPLEMENTS" else "INHERITS";
            _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, node.id, bn.?.id, edge_type, "{}");
            ws.semantic_resolved += 1;
        }
    }
}

/// Resolve decorators → DECORATES edges for one definition.
fn resolveDefDecorators(rc: *ResolveCtx, ws: *ResolveWorkerState, def: *const Definition, node: *const Node, mq: CStr, ik: StrList, iv: StrList, ic: c_int) void {
    if (def.decorators == null) return;
    var dc: usize = 0;
    while (def.decorators[dc] != null) : (dc += 1) {
        var func: [c.CBM_SZ_256]u8 = undefined;
        extractDecoratorFunc(def.decorators[dc], &func, func.len);
        if (func[0] == 0) continue;
        const func_name: CStr = &func;
        var res = c.cbm_registry_resolve(rc.registry, func_name, mq, ik, iv, ic);
        if ((res.qualified_name == null or res.qualified_name[0] == 0) and c.strchr(func_name, '.') == null) {
            // C# attributes are referenced by their short name (`[Log]`) but
            // declared with an `Attribute` suffix (`class LogAttribute`).
            var with_suffix: [c.CBM_SZ_256]u8 = undefined;
            const wn = c.snprintf(&with_suffix, with_suffix.len, "%sAttribute", func_name);
            if (wn > 0 and ix(wn) < with_suffix.len) {
                res = c.cbm_registry_resolve(rc.registry, &with_suffix, mq, ik, iv, ic);
            }
        }
        var dn: ?*const Node = null;
        if (res.qualified_name != null and res.qualified_name[0] != 0) {
            dn = c.cbm_gbuf_find_by_qn(rc.main_gbuf, res.qualified_name);
        }
        var dn_id: i64 = 0;
        if (dn) |decorator| {
            dn_id = decorator.id;
        } else {
            // External/stdlib decorator (Rust `#[derive(Debug)]`, Swift
            // `@discardableResult`, Scala `@deprecated`, Python `@cache`,
            // Java `@Override`, ...): no local symbol resolves.  Materialise a
            // synthetic "Decorator" node so the DECORATES relation is recorded.
            // The node is created in the per-worker local_edge_buf (shared-ID
            // gbuf); the sequential merge dedupes by QN across workers, so all
            // uses of the same decorator name collapse to one node project-wide
            // and the edge target IDs are remapped consistently.
            var syn_qn: [c.CBM_SZ_512]u8 = undefined;
            _ = c.snprintf(&syn_qn, syn_qn.len, "<decorator:%s>", func_name);
            dn_id = c.cbm_gbuf_upsert_node(ws.local_edge_buf, "Decorator", func_name, &syn_qn, "", 0, 0, "{}");
        }
        if (dn_id != 0 and node.id != dn_id) {
            // Decorator SOURCE TEXT can contain quotes and raw newlines
            // (e.g. @register.tag("block"), multi-line @override_settings) —
            // interpolating it raw produced malformed properties JSON that
            // aborts every json_extract consumer (django: 3826 such edges).
            var esc_dec: [c.CBM_SZ_256]u8 = undefined;
            _ = c.cbm_json_escape(&esc_dec, esc_dec.len, def.decorators[dc]);
            var dp: [c.CBM_SZ_512]u8 = undefined;
            _ = c.snprintf(&dp, dp.len, "{\"decorator\":\"%s\"}", @as(CStr, &esc_dec));
            _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, node.id, dn_id, "DECORATES", &dp);
            // Ensure a reference-style edge exists so the decorator appears in queries
            // without being misclassified as a real call by downstream passes.
            _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, node.id, dn_id, "USAGE", "{}");
            ws.semantic_resolved += 1;
        }
    }
}

/// Resolve INHERITS + DECORATES + IMPLEMENTS for one file.
fn resolveFileSemantic(rc: *ResolveCtx, ws: *ResolveWorkerState, result: *FileResult, module_qn: CStr, imp_keys: StrList, imp_vals: StrList, imp_count: c_int) void {
    for (0..ix(result.defs.count)) |d| {
        const def = util.at(result.defs.items, d);
        if (def.qualified_name == null) continue;
        const node: *const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, def.qualified_name) orelse continue;
        resolveDefInherits(rc, ws, def, node, module_qn, imp_keys, imp_vals, imp_count);
        resolveDefDecorators(rc, ws, def, node, module_qn, imp_keys, imp_vals, imp_count);
    }
    for (0..ix(result.impl_traits.count)) |t| {
        const it = util.at(result.impl_traits.items, t);
        if (it.trait_name == null or it.struct_name == null) continue;
        const tqn = resolveAsClass(rc.registry, it.trait_name, module_qn, imp_keys, imp_vals, imp_count);
        const sqn = resolveAsClass(rc.registry, it.struct_name, module_qn, imp_keys, imp_vals, imp_count);
        if (tqn == null or sqn == null) continue;
        const tn: ?*const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, tqn);
        const sn: ?*const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, sqn);
        if (tn != null and sn != null and tn.?.id != sn.?.id) {
            _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, sn.?.id, tn.?.id, "IMPLEMENTS", "{}");
            ws.semantic_resolved += 1;
        }
    }
}

// ── FastAPI Depends() tracking ──────────────────────────────────────
// Scans Python function signatures for Depends(func_ref) and creates CALLS
// edges from the endpoint to the dependency function. Without this, FastAPI
// auth/DI functions appear as dead code (in_degree=0).

const PP_SIG_SCAN_LINES = 15;
const PP_REGEX_GROUPS = 2;

/// Python signature text of the def starting at start_line. Caller frees.
fn extractPySignature(source: CStr, start_line: c_int, end_line: c_int) MutStr {
    var sig_end = start_line + PP_SIG_SCAN_LINES;
    if (end_line > 0 and sig_end > end_line) sig_end = end_line;
    var p = source;
    var line: c_int = 1;
    while (p[0] != 0 and line < start_line) {
        if (p[0] == '\n') line += 1;
        p += 1;
    }
    const sig_start = p;
    while (p[0] != 0 and line < sig_end) {
        if (p[0] == '\n') line += 1;
        p += 1;
        if (@intFromPtr(p) > @intFromPtr(sig_start) + 1 and (p - 1)[0] == ':' and (p - 2)[0] == ')') {
            break;
        }
    }
    const sig_len = ptrDiff(p, sig_start);
    const sig: MutStr = @ptrCast(c.malloc(sig_len + 1));
    if (sig == null) return null;
    @memcpy(sig[0..sig_len], sig_start[0..sig_len]);
    sig[sig_len] = 0;
    return sig;
}

/// CALLS edges for each Depends(func_ref) in one def's signature.
fn scanDependsInSig(
    rc: *ResolveCtx,
    ws: *ResolveWorkerState,
    re: *const c.cbm_regex_t,
    sig: CStr,
    source_node: *const Node,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
) void {
    var match: [PP_REGEX_GROUPS]c.cbm_regmatch_t = undefined;
    var scan = sig;
    while (c.cbm_regexec(re, scan, PP_REGEX_GROUPS, &match, 0) == 0) {
        var ref_len: c_int = match[1].rm_eo - match[1].rm_so;
        var func_ref: [c.CBM_SZ_256]u8 = undefined;
        if (ref_len >= @as(c_int, func_ref.len)) ref_len = @as(c_int, func_ref.len) - 1;
        const len = ix(ref_len);
        @memcpy(func_ref[0..len], (scan + ix(match[1].rm_so))[0..len]);
        func_ref[len] = 0;
        const res = c.cbm_registry_resolve(rc.registry, &func_ref, module_qn, imp_keys, imp_vals, imp_count);
        if (res.qualified_name != null and res.qualified_name[0] != 0) {
            const tn: ?*const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, res.qualified_name);
            if (tn != null and source_node.id != tn.?.id) {
                _ = c.cbm_gbuf_insert_edge(ws.local_edge_buf, source_node.id, tn.?.id, "CALLS", "{\"confidence\":0.95,\"strategy\":\"fastapi_depends\"}");
                ws.calls_resolved += 1;
            }
        }
        scan += ix(match[0].rm_eo);
    }
}

fn resolveFileFastapiDepends(
    rc: *ResolveCtx,
    ws: *ResolveWorkerState,
    result: *const FileResult,
    path: CStr,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
) void {
    var has_depends = false;
    for (0..ix(result.calls.count)) |ci| {
        if (has_depends) break;
        const callee: CStr = result.calls.items[ci].callee_name;
        has_depends = callee != null and streq(callee, "Depends");
    }
    if (!has_depends) return;
    var depends_re: c.cbm_regex_t = undefined;
    if (c.cbm_regcomp(&depends_re, "Depends\\(([A-Za-z_][A-Za-z0-9_.]*)", c.CBM_REG_EXTENDED) != 0) {
        return;
    }
    var source_owned: MutStr = null;
    var source: CStr = result.source;
    if (source == null or result.source_len <= 0) {
        var source_len: c_int = 0;
        source_owned = if (path != null) readFile(path, &source_len, null, null) else null;
        source = source_owned;
    }
    if (source != null) for (0..ix(result.defs.count)) |d| {
        const def = util.at(result.defs.items, d);
        if (def.qualified_name == null or def.start_line <= 0 or def.label == null or
            (!streq(def.label, "Function") and !streq(def.label, "Method")))
        {
            continue;
        }
        const source_node: *const Node = c.cbm_gbuf_find_by_qn(rc.main_gbuf, def.qualified_name) orelse continue;
        const sig = extractPySignature(source, @bitCast(def.start_line), @bitCast(def.end_line));
        if (sig == null) continue;
        scanDependsInSig(rc, ws, &depends_re, sig, source_node, module_qn, imp_keys, imp_vals, imp_count);
        c.free(sig);
    };
    freeSource(source_owned);
    c.cbm_regfree(&depends_re);
}

/// F4: get (or lazily build ONCE) the shared all_defs Rust registry. Called by the
/// first worker that hits a NULL-filter rust file; later null-files reuse it. Fast
/// path is a lock-free atomic load; the build happens under rust_shared_mu into the
/// dedicated rust_shared_arena (never a shared pipeline arena from a worker thread).
/// Returns NULL if there are no defs (caller falls back to the per-file build).
fn ppRustSharedRegistry(rc: *ResolveCtx) ?*c.CBMTypeRegistry {
    var p = rc.rust_shared_reg.load(.acquire);
    if (p != null) return p;
    if (rc.all_defs == null or rc.def_count <= 0) return null;
    c.cbm_mutex_lock(&rc.rust_shared_mu);
    p = rc.rust_shared_reg.load(.monotonic);
    if (p == null) {
        c.cbm_arena_init(&rc.rust_shared_arena);
        rc.rust_shared_arena_live = true;
        p = c.cbm_rust_build_cross_registry(&rc.rust_shared_arena, rc.all_defs, rc.def_count);
        if (p) |reg| {
            var sb: [96]u8 = undefined;
            _ = c.snprintf(&sb, sb.len, "types=%d funcs=%d", reg.type_count, reg.func_count);
            interop.logInfo("cross_lsp.rust_registry", .{ "scale", @as(CStr, &sb) });
        }
        rc.rust_shared_reg.store(p, .release);
    }
    c.cbm_mutex_unlock(&rc.rust_shared_mu);
    return p;
}

/// void*-typed adapter so the shared dispatch helper (pass_lsp_cross) can
/// borrow the lazily-built shared Rust registry without knowing ResolveCtx.
fn ppRustSharedRegistryGet(ctx: ?*anyopaque) callconv(.c) [*c]c.CBMTypeRegistry {
    return ppRustSharedRegistry(@ptrCast(@alignCast(ctx)));
}

fn resolveWorker(worker_id: c_int, ctx_ptr: ?*anyopaque) callconv(.c) void {
    const rc: *ResolveCtx = @ptrCast(@alignCast(ctx_ptr));
    const ws = util.at(rc.workers, ix(worker_id));

    c.cbm_pxc_set_rust_manifest(rc.rust_manifest);

    if (ws.local_edge_buf == null) {
        ws.local_edge_buf = c.cbm_gbuf_new_shared_ids(rc.project_name, rc.repo_path, rc.shared_ids);
    }

    // Per-worker service-pattern result cache. The same resolved QN
    // (e.g. "fmt.Errorf", "context.Context.Done") appears in many
    // call edges across many files within a project — caching turns
    // cbm_service_pattern_match's 6 × 30 × strstr scan into one hash
    // lookup after the first miss for each QN. Scoped to the worker's
    // lifetime in the parallel_resolve phase.
    c.cbm_service_pattern_cache_begin();

    // A result loaded from the spill store lives for one iteration; it is
    // released at the top of the next one (every `continue` passes there)
    // and after the loop.
    var loaded: ?*FileResult = null;
    var loaded_idx: c_int = -1;
    while (true) {
        c.cbm_result_cache_release(rc.result_cache, loaded_idx, loaded);
        loaded = null;
        loaded_idx = -1;
        const file_idx = rc.next_file_idx.fetchAdd(1, .monotonic);
        if (file_idx >= rc.file_count) break;
        if (loadInt(rc.cancelled) != 0) break;
        const fidx = ix(file_idx);

        const loop_t0 = extractNowNs();

        const result_opt: ?*FileResult = c.cbm_result_cache_acquire(rc.result_cache, rc.spill, file_idx);
        if (result_opt != null and result_opt != @as(?*FileResult, rc.result_cache[fidx])) {
            loaded = result_opt;
            loaded_idx = file_idx;
            // cbm_pxc_collect_all_defs stored the inferred JVM namespace on
            // the in-memory result; a loaded copy needs it again.
            if (rc.all_defs != null) {
                _ = c.cbm_pxc_apply_inferred_namespace(result_opt, util.at(rc.files, fidx));
            }
        }
        const result = result_opt orelse {
            addElapsed(&rc.time_ns_total_loop, loop_t0);
            continue;
        };
        _ = rc.total_files_visited.fetchAdd(1, .monotonic);

        const lang: Language = rc.files[fidx].language;
        const rel: CStr = rc.files[fidx].rel_path;

        // Skip cross-LSP for machine-generated files — they're huge (10k-
        // 70k lines for k8s protobuf/openapi), have low semantic value for
        // graph navigation (boilerplate getters/setters/marshal), and
        // dominate the cross-LSP wall time when they have even one
        // unresolved call (tree-sitter parse on a 70k-line file is ~1-2s).
        // The per-file LSP during extract still indexes their defs/calls
        // normally — only the cross-file resolution refinement is skipped.
        var is_generated = false;
        if (rel != null) {
            inline for (.{
                ".pb.go",  "zz_generated", "_generated.go", ".gen.go", "/applyconfigurations/",
                "_pb2.py", "_pb2_grpc.py", ".pb.cc",        ".pb.h",   ".pb-c.c",
                ".pb-c.h",
            }) |marker| {
                is_generated = is_generated or c.strstr(rel, marker) != null;
            }
        }

        // Cross-file LSP is a per-file tree-sitter re-parse + AST walk +
        // registry lookups — ~50-150ms per file. It can ONLY find calls
        // that exist in the AST. If the per-file extract found zero calls,
        // cross-LSP will too: the AST is the same. For non-JVM languages,
        // skip when per-file LSP already produced at least as many resolved
        // entries as textual calls. Java/Kotlin per-file LSP can fill the
        // count with constructors or same-file calls while a mixed-source-root
        // Java↔Kotlin call remains unresolved, so JVM callers run whenever
        // calls exist. Callable-reference candidates (`accept(handler)` with
        // an imported `handler`) count the same way against reference rows.
        const jvm_cross_lsp = lang == c.CBM_LANG_JAVA or lang == c.CBM_LANG_KOTLIN;
        const rust_workspace_cross_lsp =
            lang == c.CBM_LANG_RUST and rc.rust_manifest != null and rc.rust_manifest.?.member_count > 0;
        const invocation_rows = lsp_resolve.invocationRowCount(&result.resolved_calls);
        var ref_candidates: c_int = 0;
        for (0..ix(result.usages.count)) |u| {
            ref_candidates += @intFromBool(result.usages.items[u].may_be_call_reference);
        }
        const cross_lsp_eligible =
            rc.all_defs != null and rc.def_count > 0 and c.cbm_pxc_has_cross_lsp(lang) and
            (result.calls.count > 0 or ref_candidates > 0) and
            (jvm_cross_lsp or rust_workspace_cross_lsp or invocation_rows < result.calls.count or
                lsp_resolve.referenceRowCount(&result.resolved_calls) < ref_candidates or
                (lang == c.CBM_LANG_ZIG and rc.def_modules != null and
                    c.cbm_pxc_zig_args_in_module(rc.cross_registries, rc.def_modules[fidx]))) and
            !is_generated;

        // Skip files with nothing else to resolve and no cross-LSP work.
        if (result.calls.count == 0 and result.usages.count == 0 and result.throws.count == 0 and
            result.rw.count == 0 and result.defs.count == 0 and result.impl_traits.count == 0 and
            !cross_lsp_eligible)
        {
            continue;
        }

        // Build import map ONCE (read-only access to main_gbuf). The
        // same imp_keys/imp_vals feed both the fused cross-file LSP
        // step below AND the resolve_file_* chain — no duplicate build.
        var imp_keys: StrList = null;
        var imp_vals: StrList = null;
        var imp_count: c_int = 0;
        const imp_t0 = extractNowNs();
        _ = buildImportMap(rc.main_gbuf, rc.project_name, rel, &imp_keys, &imp_vals, &imp_count);
        addElapsed(&rc.time_ns_import_map, imp_t0);

        // Per-file is_import_reachable memoization. Spans all 5 resolve
        // sub-passes (calls/usages/throws/rw/semantic) which all flow
        // through cbm_registry_resolve. Same callee_name appears in
        // many call sites — first eval pays the strstr cost, repeats
        // are O(1) hash. Imports are constant within a file so the
        // cache is sound; invalidated at file exit.
        c.cbm_registry_reach_cache_begin(result.calls.count + result.usages.count + 64);

        // Per-file import-map prefix → module-qn hash. resolve_import_map
        // was doing O(imports) linear strcmp per call; with this it
        // becomes O(1). Keys/values borrowed from imp_keys/imp_vals
        // which outlive this scope.
        c.cbm_registry_import_map_cache_begin(imp_keys, imp_vals, imp_count);

        // THE BIG ONE: per-file cache of cbm_registry_resolve results.
        // Same callee_name in multiple call sites resolves identically
        // within a file (module_qn is fixed) — first call walks the
        // strategy chain, repeats are O(1). On K8s this targets the
        // 98.7% hot spot in resolve_file_calls (881 of 893s CPU).
        c.cbm_registry_resolve_cache_begin(result.calls.count + result.usages.count + 64);

        const module_qn: MutStr = c.cbm_pipeline_fqn_module_dir(rc.project_name, rel, ppModuleIsDir(lang));

        // ── Cross-file LSP (FUSED) ─────────────────────────────
        // Runs BEFORE resolve_file_calls so its additions to
        // result->resolved_calls are picked up by
        // cbm_pipeline_find_lsp_resolution when calls become CALLS
        // edges. Prefers source bytes retained in result->arena during
        // extract; when the low-RAM retention cap dropped this file
        // (result->source==NULL) it FALLS BACK to a bounded per-file
        // read from disk, freed immediately after the LSP call. This is
        // the correctness guarantee: lowering the retention cap trades
        // retained RAM for a bounded re-read, it NEVER drops a cross-file
        // edge. Only a genuine read failure (deleted/unreadable/oversized)
        // leaves source NULL and is counted as skipped_no_source; defs/calls
        // already in the extract are unaffected either way.
        //
        // Slab reclaim afterward: the LSP re-parses via tree-sitter,
        // which allocates through this worker's TLS slab. Reclaiming
        // here keeps the slab high-water bounded as the resolve phase
        // walks across thousands of files in a single worker thread.
        if (cross_lsp_eligible) {
            var lsp_source_owned: MutStr = null;
            var lsp_source: CStr = result.source;
            var lsp_source_len: c_int = result.source_len;
            if ((lsp_source == null or lsp_source_len <= 0) and rc.files[fidx].path != null) {
                // Retention cap skipped this file — re-read on demand (bounded
                // by readFile's cbm_max_file_bytes cap), freed below.
                lsp_source_owned = readFile(rc.files[fidx].path, &lsp_source_len, null, null);
                lsp_source = lsp_source_owned;
            }
            if (lsp_source != null and lsp_source_len > 0) {
                const def_module: CStr = if (rc.def_modules != null) rc.def_modules[fidx] else module_qn;

                const lsp_t0 = extractNowNs();

                // Shared per-file dispatch (pass_lsp_cross): module-def
                // filter → shared prebuilt registry (overlay pattern) →
                // filtered per-file fallback. Journal the
                // file around the resolve so a hang HERE is attributed to
                // this file, not to a stale extraction marker.
                c.cbm_index_mark_start(rel);
                const had_error = result.has_error;
                c.cbm_pxc_dispatch_file(lang, result, lsp_source, lsp_source_len, rel, def_module, rc.cross_registries, rc.module_def_index, rc.all_defs, rc.def_count, imp_keys, imp_vals, imp_count, ppRustSharedRegistryGet, rc);
                c.cbm_index_mark_done(rel);
                if (!had_error and result.has_error) {
                    rc.lsp_failures[fidx] = result.error_msg;
                }
                // Free the on-demand re-read (no-op when source was retained).
                freeSource(lsp_source_owned);
                // Contract: cbm_slab_reclaim() requires the thread parser to be
                // destroyed first; otherwise its lexer holds slab pointers
                // (lexer.included_ranges) that get freed underneath it, causing
                // a heap-use-after-free on the next ts_lexer_goto. The next
                // cbm_extract_file on this thread will recreate the parser.
                c.cbm_destroy_thread_parser();
                c.cbm_slab_reclaim();
                const lsp_elapsed_ns = extractNowNs() -% lsp_t0;
                _ = rc.time_ns_cross_lsp.fetchAdd(lsp_elapsed_ns, .monotonic);
                const lsp_elapsed_ms = lsp_elapsed_ns / PP_USEC_PER_MS;
                if (lsp_elapsed_ms > PP_TIMER_THRESH) {
                    interop.logInfo("parallel.resolve.lsp_cross.slow", .{
                        "elapsed_ms", itoa(msInt(lsp_elapsed_ms)),
                        "path",       rel,
                    });
                }
                _ = rc.lsp_cross_processed.fetchAdd(1, .monotonic);
            } else {
                // Source unavailable even after the re-read fallback (file
                // deleted / unreadable / oversized) → the cross-file LSP
                // refinement no-ops for this file. This is a bounded skip, NOT
                // a file failure: defs/calls were already extracted and are
                // unaffected. Deliberately NOT recorded as a cbm_file_error —
                // doing so would flood skipped[] with false positives (itself a
                // false-guard bug). The "cross_lsp" phase string is reserved for
                // Track C's real crash-attribution signal; leave it unwired.
                _ = rc.lsp_cross_skipped_no_source.fetchAdd(1, .monotonic);
            }
        }

        // Per-sub-phase wall-clock so we can attribute the dominant cost.
        var ph_t0: u64 = undefined;

        // ── CALLS resolution ────────────────────────────────────
        ph_t0 = extractNowNs();
        resolveFileCalls(rc, ws, result, rel, module_qn, imp_keys, imp_vals, imp_count, lang);
        if (lang == c.CBM_LANG_PYTHON) {
            resolveFileFastapiDepends(rc, ws, result, rc.files[fidx].path, module_qn, imp_keys, imp_vals, imp_count);
        }
        addElapsed(&rc.time_ns_calls, ph_t0);

        // ── USAGE resolution ────────────────────────────────────
        ph_t0 = extractNowNs();
        resolveFileUsages(rc, ws, result, rel, module_qn, imp_keys, imp_vals, imp_count, lang);
        addElapsed(&rc.time_ns_usages, ph_t0);

        // ── THROWS / RAISES ─────────────────────────────────────
        ph_t0 = extractNowNs();
        resolveFileThrows(rc, ws, result, module_qn, imp_keys, imp_vals, imp_count);
        addElapsed(&rc.time_ns_throws, ph_t0);

        // ── READS / WRITES ──────────────────────────────────────
        ph_t0 = extractNowNs();
        resolveFileRw(rc, ws, result, rel, module_qn, imp_keys, imp_vals, imp_count, lang);
        addElapsed(&rc.time_ns_rw, ph_t0);

        // ── INHERITS + DECORATES + IMPLEMENTS ────────────────────
        ph_t0 = extractNowNs();
        resolveFileSemantic(rc, ws, result, module_qn, imp_keys, imp_vals, imp_count);
        addElapsed(&rc.time_ns_semantic, ph_t0);

        c.cbm_registry_reach_cache_end();
        c.cbm_registry_import_map_cache_end();
        c.cbm_registry_resolve_cache_end();

        c.free(module_qn);
        freeImportMap(imp_keys, imp_vals, imp_count);

        addElapsed(&rc.time_ns_total_loop, loop_t0);
    }
    c.cbm_result_cache_release(rc.result_cache, loaded_idx, loaded);

    // Tear down this worker's thread-local parser + slab state, mirroring
    // extractWorker. Without this, resolve-worker pages keep an owner pointer
    // into dead TLS and are never retired, so a later cross-thread free can
    // never bring their refcount to zero (leak). Retiring them here releases
    // each page as its final chunk returns.
    c.cbm_pxc_set_rust_manifest(null);
    c.cbm_destroy_thread_parser();
    c.cbm_slab_destroy_thread();
    c.cbm_service_pattern_cache_end();
}

/// snprintf("%llu") of `v` into `buf`.
fn u64Text(buf: *[32]u8, v: u64) CStr {
    _ = c.snprintf(buf, buf.len, "%llu", @as(c_ulonglong, v));
    return buf;
}

/// A nanosecond counter in whole milliseconds, as text in `buf`.
fn nsAsMsText(buf: *[32]u8, counter: *const AtomicU64) CStr {
    return u64Text(buf, counter.load(.monotonic) / 1000000);
}

pub export fn cbm_parallel_resolve(
    ctx: *Ctx,
    files: [*c]const FileInfo,
    file_count: c_int,
    result_cache: [*c][*c]FileResult,
    shared_ids: [*c]i64,
    worker_count: c_int,
    all_defs: [*c]LspDef,
    def_count: c_int,
    def_modules: [*c]const MutStr,
    module_def_index: ?*c.struct_CBMModuleDefIndex,
    cross_registries_v: ?*anyopaque,
) c_int {
    // See header: typed as void* across the TU boundary; cast back here.
    const cross_registries: ?*c.CBMCrossLspRegistries = @ptrCast(@alignCast(cross_registries_v));
    if (file_count == 0) return 0;

    interop.logInfo("parallel.resolve.start", .{
        "files",   itoa(file_count),
        "workers", itoa(worker_count),
    });

    var workers_raw: ?*anyopaque = null;
    const workers_bytes = ix(worker_count) * @sizeOf(ResolveWorkerState);
    if (c.cbm_aligned_alloc(&workers_raw, CBM_CACHE_LINE, workers_bytes) != 0) return c.CBM_NOT_FOUND;
    const workers: [*c]ResolveWorkerState = @ptrCast(@alignCast(workers_raw));
    @memset(@as([*]u8, @ptrCast(workers))[0..workers_bytes], 0);
    const lsp_failures = interop.callocN(CStr, ix(file_count));
    if (lsp_failures == null) {
        c.cbm_aligned_free(workers);
        return c.CBM_NOT_FOUND;
    }

    var have_rust = false;
    for (0..ix(file_count)) |i| {
        if (files[i].language == c.CBM_LANG_RUST and
            c.cbm_result_cache_present(result_cache, ctx.spill, @intCast(i)))
        {
            have_rust = true;
            break;
        }
    }
    var rust_manifest_arena: Arena = undefined;
    var rust_manifest: c.CBMCargoManifest = undefined;
    var rust_manifest_arena_live = false;
    var rust_manifest_ptr: ?*const c.CBMCargoManifest = null;
    if (have_rust) {
        c.cbm_arena_init(&rust_manifest_arena);
        rust_manifest_arena_live = true;
        if (c.cbm_pxc_build_rust_manifest(ctx, &rust_manifest_arena, &rust_manifest)) {
            rust_manifest_ptr = &rust_manifest;
        }
    }

    var rc: ResolveCtx = .{
        .files = files,
        .file_count = file_count,
        .project_name = ctx.project_name,
        .repo_path = ctx.repo_path,
        .workers = workers,
        .max_workers = worker_count,
        .result_cache = result_cache,
        .spill = ctx.spill,
        .main_gbuf = ctx.gbuf,
        .registry = ctx.registry,
        .shared_ids = shared_ids,
        .cancelled = ctx.cancelled,
        .all_defs = all_defs,
        .def_count = def_count,
        .def_modules = def_modules,
        .module_def_index = module_def_index,
        .cross_registries = cross_registries,
        .rust_manifest = rust_manifest_ptr,
        .lsp_failures = lsp_failures,
    };
    lsp_resolve.tail_lookups.store(0, .monotonic);
    lsp_resolve.tail_candidates.store(0, .monotonic);
    c.cbm_pxc_filter_stats_reset();
    // F4 lazy shared Rust registry: mutex up before workers spawn.
    c.cbm_mutex_init(&rc.rust_shared_mu);
    rc.rust_shared_arena_live = false;

    // Sub-phase: Dispatch resolve workers (per-file call/usage resolution, PARALLEL)
    const t_resolve_dispatch = util.profStart();
    var opts = std.mem.zeroes(c.cbm_parallel_for_opts_t);
    opts.max_workers = worker_count;
    opts.force_pthreads = false;
    c.cbm_parallel_for(worker_count, resolveWorker, &rc, opts);
    util.profEnd("parallel_resolve", "1_dispatch_workers_parallel", &t_resolve_dispatch, file_count);
    for (0..ix(file_count)) |i| {
        if (lsp_failures[i] != null and ctx.pipeline != null) {
            c.cbm_pipeline_add_file_error(ctx.pipeline, files[i].rel_path, lsp_failures[i], "lsp_incomplete");
        }
    }
    c.free(@ptrCast(lsp_failures));
    // Workers joined: the shared Rust registry (if built) is no longer read.
    // Free its dedicated arena + the lock (registry was self-contained: it strdup'd
    // all QNs, so freeing all_defs afterward is safe).
    if (rc.rust_shared_arena_live) {
        c.cbm_arena_destroy(&rc.rust_shared_arena);
        rc.rust_shared_arena_live = false;
    }
    c.cbm_mutex_destroy(&rc.rust_shared_mu);
    if (rust_manifest_arena_live) c.cbm_arena_destroy(&rust_manifest_arena);

    // Sub-phase: Merge all local edge bufs into main gbuf (SEQUENTIAL)
    const t_resolve_merge = util.profStart();
    var total_calls: c_int = 0;
    var total_usages: c_int = 0;
    var total_semantic: c_int = 0;
    var total_lsp_overrides: c_int = 0;
    for (0..ix(worker_count)) |i| {
        if (workers[i].local_edge_buf != null) {
            _ = c.cbm_gbuf_merge(ctx.gbuf, workers[i].local_edge_buf);
            total_calls += workers[i].calls_resolved;
            total_usages += workers[i].usages_resolved;
            total_semantic += workers[i].semantic_resolved;
            total_lsp_overrides += workers[i].lsp_overrides;
            c.cbm_gbuf_free(workers[i].local_edge_buf);
        }
    }
    util.profEnd("parallel_resolve", "2_merge_edge_bufs_seq", &t_resolve_merge, total_calls + total_usages);

    c.cbm_aligned_free(workers);

    // Go-style implicit interface satisfaction (needs full graph, serial)
    const go_impl = c.cbm_pipeline_implements_go(ctx);

    if (@atomicLoad(c_int, ctx.cancelled, .seq_cst) != 0) return c.CBM_NOT_FOUND;

    // Summary metric that replaces the removed `pass.timing pass=lsp_cross`
    // log line — surfaces how many files the fused cross-file LSP step
    // actually processed vs skipped (e.g. because their source bytes
    // were not retained at extract time due to the per-file/total cap).
    interop.logInfo("parallel.resolve.lsp_cross_done", .{
        "files_processed",         itoa(rc.lsp_cross_processed.load(.monotonic)),
        "files_skipped_no_source", itoa(rc.lsp_cross_skipped_no_source.load(.monotonic)),
        "defs_total",              itoa(def_count),
    });

    // Cross-LSP cost normalised per file (upstream f6e3af43/cffb4e37). Wall
    // time cannot tell "big repo" from "superlinear pass"; us_per_file can:
    // for a pass linear in file count it stays roughly constant across repo
    // sizes. defs_per_file is the lever behind it: if it tracks defs_total
    // rather than the file's own module plus imports, the module filter is
    // not containing the work and cross-file LSP is O(files x corpus_defs).
    const cross_files = rc.lsp_cross_processed.load(.monotonic);
    const cross_us = rc.time_ns_cross_lsp.load(.monotonic) / 1000;
    if (cross_files > 0) {
        const cross_files_u: u64 = @intCast(cross_files);
        var cf_buf: [32]u8 = undefined;
        var nf_buf: [32]u8 = undefined;
        var cu_buf: [32]u8 = undefined;
        var pk_buf: [32]u8 = undefined;
        _ = c.snprintf(&nf_buf, nf_buf.len, "%d", cross_files);
        const per_kdef: u64 = if (def_count > 0)
            (cross_us *% 1000) / (cross_files_u *% @as(u64, @intCast(def_count)))
        else
            0;
        interop.logInfo("parallel.resolve.cross_lsp_cost", .{
            "cross_lsp_ms",         u64Text(&cu_buf, cross_us / 1000),
            "files",                @as(CStr, &nf_buf),
            "us_per_file",          u64Text(&cf_buf, cross_us / cross_files_u),
            "defs_total",           itoa(def_count),
            "us_per_file_per_kdef", u64Text(&pk_buf, per_kdef),
        });

        var reg_defs: u64 = 0;
        var reg_files: u64 = 0;
        var flt_files: u64 = 0;
        var flt_failed: u64 = 0;
        c.cbm_pxc_filter_stats(&reg_defs, &reg_files, &flt_files, &flt_failed);
        if (reg_files > 0) {
            var rd_buf: [32]u8 = undefined;
            var ff_buf: [32]u8 = undefined;
            var fp_buf: [32]u8 = undefined;
            interop.logInfo("parallel.resolve.perfile_registry", .{
                "defs_per_file",  u64Text(&rd_buf, reg_defs / reg_files),
                "defs_total",     itoa(def_count),
                "filtered_files", u64Text(&fp_buf, flt_files),
                "filter_failed",  u64Text(&ff_buf, flt_failed),
            });
        }
    }

    interop.logInfo("parallel.resolve.done", .{
        "calls",         itoa(total_calls),
        "usages",        itoa(total_usages),
        "semantic",      itoa(total_semantic + go_impl),
        "lsp_overrides", itoa(total_lsp_overrides),
    });

    // Per-sub-phase breakdown so we stop guessing about hot paths.
    // Numbers are summed across workers (total CPU-ms, not wall-time).
    var loop_buf: [32]u8 = undefined;
    var visits_buf: [32]u8 = undefined;
    _ = c.snprintf(&visits_buf, visits_buf.len, "%d", rc.total_files_visited.load(.monotonic));
    interop.logInfo("parallel.resolve.phase_summary", .{
        "total_loop_cpu_ms", nsAsMsText(&loop_buf, &rc.time_ns_total_loop),
        "files_visited",     @as(CStr, &visits_buf),
    });

    var imp_buf: [32]u8 = undefined;
    var xls_buf: [32]u8 = undefined;
    var cal_buf: [32]u8 = undefined;
    interop.logInfo("parallel.resolve.phase_ms_a", .{
        "import_map",    nsAsMsText(&imp_buf, &rc.time_ns_import_map),
        "cross_lsp",     nsAsMsText(&xls_buf, &rc.time_ns_cross_lsp),
        "resolve_calls", nsAsMsText(&cal_buf, &rc.time_ns_calls),
    });

    var use_buf: [32]u8 = undefined;
    var thr_buf: [32]u8 = undefined;
    var rw_buf: [32]u8 = undefined;
    var sem_buf: [32]u8 = undefined;
    interop.logInfo("parallel.resolve.phase_ms_b", .{
        "resolve_usages",   nsAsMsText(&use_buf, &rc.time_ns_usages),
        "resolve_throws",   nsAsMsText(&thr_buf, &rc.time_ns_throws),
        "resolve_rw",       nsAsMsText(&rw_buf, &rc.time_ns_rw),
        "resolve_semantic", nsAsMsText(&sem_buf, &rc.time_ns_semantic),
    });

    var src_buf: [32]u8 = undefined;
    var lsp_buf: [32]u8 = undefined;
    var rsv_buf: [32]u8 = undefined;
    var hnt_buf: [32]u8 = undefined;
    var tgt_buf: [32]u8 = undefined;
    var emt_buf: [32]u8 = undefined;
    interop.logInfo("parallel.resolve.calls_breakdown", .{
        "find_source", nsAsMsText(&src_buf, &rc.time_ns_rc_source),
        "lsp_lookup",  nsAsMsText(&lsp_buf, &rc.time_ns_rc_lsp_lookup),
        "resolve",     nsAsMsText(&rsv_buf, &rc.time_ns_rc_resolve),
    });
    interop.logInfo("parallel.resolve.calls_breakdown2", .{
        "field_hint",  nsAsMsText(&hnt_buf, &rc.time_ns_rc_hint),
        "find_target", nsAsMsText(&tgt_buf, &rc.time_ns_rc_target),
        "emit_edge",   nsAsMsText(&emt_buf, &rc.time_ns_rc_emit),
    });

    // Candidate-scan cost (upstream f6e3af43). per_lookup is a property of
    // the corpus, not the machine; if it grows with repo size the tail-match
    // scan is turning resolve superlinear.
    const tail_lookups = lsp_resolve.tail_lookups.load(.monotonic);
    const tail_cands = lsp_resolve.tail_candidates.load(.monotonic);
    var tl_buf: [32]u8 = undefined;
    var tc_buf: [32]u8 = undefined;
    var tp_buf: [32]u8 = undefined;
    interop.logInfo("parallel.resolve.scan_cost", .{
        "tail_lookups",    u64Text(&tl_buf, tail_lookups),
        "tail_candidates", u64Text(&tc_buf, tail_cands),
        "per_lookup",      u64Text(&tp_buf, if (tail_lookups != 0) tail_cands / tail_lookups else 0),
    });
    return 0;
}

pub export fn cbm_parallel_resolve_calls(ctx_c: ?*Ctx, files: [*c]const FileInfo, file_count: c_int) void {
    const ctx = ctx_c orelse return;
    if (files == null or file_count <= 0) return;
    var shared_ids: i64 = c.cbm_gbuf_next_id(ctx.gbuf);
    var rc: ResolveCtx = .{
        .files = files,
        .file_count = file_count,
        .project_name = ctx.project_name,
        .repo_path = ctx.repo_path,
        .workers = null,
        .max_workers = 0,
        .result_cache = null,
        .spill = null,
        .main_gbuf = ctx.gbuf,
        .registry = ctx.registry,
        .shared_ids = &shared_ids,
        .cancelled = ctx.cancelled,
        .all_defs = null,
        .def_count = 0,
        .def_modules = null,
        .module_def_index = null,
        .cross_registries = null,
    };
    var ws = std.mem.zeroes(ResolveWorkerState);
    ws.local_edge_buf = c.cbm_gbuf_new_shared_ids(ctx.project_name, ctx.repo_path, &shared_ids);
    if (ws.local_edge_buf == null) return;
    for (0..ix(file_count)) |i| {
        if (util.checkCancel(ctx) != 0) break;
        const fi = util.at(files, i);
        var source_len: c_int = 0;
        const source = readFile(fi.path, &source_len, null, null);
        if (source == null) continue;
        const result_opt: ?*FileResult = c.cbm_extract_file(source, source_len, fi.language, ctx.project_name, fi.rel_path, c.CBM_EXTRACT_BUDGET, null, null);
        freeSource(source);
        const result = result_opt orelse continue;
        if (result.calls.count > 0) {
            var imp_keys: StrList = null;
            var imp_vals: StrList = null;
            var imp_count: c_int = 0;
            _ = buildImportMap(ctx.gbuf, ctx.project_name, fi.rel_path, &imp_keys, &imp_vals, &imp_count);
            const module_qn: MutStr = c.cbm_pipeline_fqn_module_dir(ctx.project_name, fi.rel_path, ppModuleIsDir(fi.language));
            resolveFileCalls(&rc, &ws, result, fi.rel_path, module_qn, imp_keys, imp_vals, imp_count, fi.language);
            if (fi.language == c.CBM_LANG_PYTHON) {
                resolveFileFastapiDepends(&rc, &ws, result, fi.path, module_qn, imp_keys, imp_vals, imp_count);
            }
            c.free(module_qn);
            freeImportMap(imp_keys, imp_vals, imp_count);
        }
        c.cbm_free_result(result);
    }
    _ = c.cbm_gbuf_merge(ctx.gbuf, ws.local_edge_buf);
    c.cbm_gbuf_free(ws.local_edge_buf);
    c.cbm_pipeline_sync_next_id(ctx.gbuf, &shared_ids);
    interop.logInfo("parallel.resolve_calls.done", .{
        "files", itoa(file_count),
        "calls", itoa(ws.calls_resolved),
    });
}
