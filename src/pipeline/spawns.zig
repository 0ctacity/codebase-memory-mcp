//! spawns.zig — SPAWNS edges: a call that starts another program, to the
//! Process node of that program (internal/cbm/spawn_patterns.h).
//!
//! Hooked into the call resolver (pass_parallel.zig resolveFileCalls) before
//! empty-resolution fallbacks, so launch APIs emit SPAWNS edges instead of
//! heuristic guessed CALLS edges.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const streq = util.streq;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Call = c.CBMCall;
const Language = c.CBMLanguage;
const ImportArray = c.CBMImportArray;
const Resolution = c.cbm_resolution_t;

/// Edge confidence: a site read from the source.
const spawn_conf_source: f64 = 1.0;

pub const Spawn = c.cbm_pipeline_spawn_t;

/// Strategies that only guess by name: they never make a spawn spelling an
/// ordinary call (`subprocess.run` must not become the project's `run`, nor
/// a call of the C library's `system` the static `system` of another file).
fn spawnNameGuess(strategy: CStr) bool {
    if (strategy == null) return false;
    const guesses = [_][]const u8{
        "unique_name",
        "suffix_match",
        "qualified_suffix",
        "field_type_hint",
        "fuzzy",
    };
    const s = std.mem.span(strategy);
    for (guesses) |g| {
        if (std.mem.eql(u8, s, g)) return true;
    }
    return false;
}

/// Labels of code a call can run: a function, or a type it constructs.
fn spawnCodeLabel(label: CStr) bool {
    if (label == null) return false;
    const labels = [_][]const u8{
        "Function",
        "Method",
        "Class",
        "Struct",
        "Interface",
        "Trait",
        "Enum",
        "Type",
        "Macro",
    };
    const l = std.mem.span(label);
    for (labels) |lbl| {
        if (std.mem.eql(u8, l, lbl)) return true;
    }
    return false;
}

/// The call reached code in the project's own sources. A synthetic builtin
/// (a `<python-builtins>` node), a QN without a node, and a Variable are not:
/// `const { spawn } = require("child_process")` mints a Variable `spawn` that
/// the call resolves to by same module, and it is the import binding. For a
/// qualified spelling, same_module is a guess too: it matched the suffix
/// alone, so `subprocess.run` inside a module's own `def run` found that
/// `run`.
fn spawnResolvedToProject(gbuf: ?*const Gbuf, res: ?*const Resolution, qualified: bool) bool {
    const r = res orelse return false;
    if (r.qualified_name == null or r.qualified_name[0] == 0) return false;
    const gb = gbuf orelse return false;
    const node = c.cbm_gbuf_find_by_qn(gb, r.qualified_name) orelse return false;
    if (node.*.file_path == null or node.*.file_path[0] == 0 or node.*.file_path[0] == '<' or
        !spawnCodeLabel(node.*.label))
    {
        return false;
    }
    const guess = spawnNameGuess(r.strategy) or
        (qualified and r.strategy != null and streq(r.strategy, "same_module"));
    return !guess;
}

/// True when `call` starts another program (spawn_patterns.h) and `res` does
/// not make it a call into the project's own code. `imports` are the file's
/// extracted imports. Emits emit() instead of a CALLS edge.
pub fn site(
    gbuf: ?*const Gbuf,
    lang: Language,
    call: *const Call,
    imports: ?*const ImportArray,
    res: ?*const Resolution,
    out: *Spawn,
) bool {
    const import_items = if (imports) |imp| imp.items else null;
    const import_count: c_int = if (imports) |imp| imp.count else 0;
    const api = c.cbm_spawn_match(lang, call.callee_name, import_items, import_count) orelse return false;

    // A bare launch API (e.g. PHP exec, Ruby exec) called as a method on a receiver
    // (such as $pdo->exec("DELETE FROM ...") or obj.exec(...)) is not a process spawn.
    if (call.is_method and (api.*.rule == c.CBM_SPAWN_BARE or api.*.rule == c.CBM_SPAWN_BARE_IMPORTED)) {
        return false;
    }

    var qualified = false;
    if (call.callee_name) |name| {
        var p = name;
        while (p[0] != 0) : (p += 1) {
            if (p[0] == '.' or p[0] == ':' or p[0] == '/' or p[0] == '\\') {
                qualified = true;
                break;
            }
        }
    }

    if (spawnResolvedToProject(gbuf, res, qualified)) {
        return false;
    }

    out.api = api.*.api;
    c.cbm_spawn_program(api, lang, call, &out.program, out.program.len);
    return true;
}

/// The Process node of the spawned program (QN __process__<program>, one per
/// program) and the SPAWNS edge from `source` to it.
pub fn emit(
    gbuf: ?*Gbuf,
    source: *const Node,
    call: *const Call,
    spawn: *const Spawn,
) void {
    const gb = gbuf orelse return;

    var program_escaped: [c.CBM_SZ_128]u8 = undefined;
    var api_escaped: [c.CBM_SZ_256]u8 = undefined;
    _ = c.cbm_json_escape(&program_escaped, program_escaped.len, &spawn.program);
    _ = c.cbm_json_escape(&api_escaped, api_escaped.len, spawn.api);

    var qn: [c.CBM_SZ_128]u8 = undefined;
    _ = c.snprintf(&qn, qn.len, "__process__%s", @as(CStr, &spawn.program));

    var node_props: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&node_props, node_props.len, "{\"program\":\"%s\"}", @as(CStr, &program_escaped));

    const process_id = c.cbm_gbuf_upsert_node(
        gb,
        "Process",
        &spawn.program,
        &qn,
        "",
        0,
        0,
        &node_props,
    );
    if (process_id <= 0) return;

    var edge_props: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(
        &edge_props,
        edge_props.len,
        "{\"api\":\"%s\",\"program\":\"%s\",\"line\":%d,\"confidence\":%.2f}",
        @as(CStr, &api_escaped),
        @as(CStr, &program_escaped),
        call.start_line,
        spawn_conf_source,
    );

    _ = c.cbm_gbuf_insert_edge(gb, source.*.id, process_id, "SPAWNS", &edge_props);
}

// ── C-ABI Exports (matching lsp_resolve.zig pattern) ─────────────────

pub export fn cbm_pipeline_spawn_site(
    gbuf: ?*const Gbuf,
    lang: Language,
    call: ?*const Call,
    imports: ?*const ImportArray,
    res: ?*const Resolution,
    out: ?*Spawn,
) bool {
    const cl = call orelse return false;
    const o = out orelse return false;
    return site(gbuf, lang, cl, imports, res, o);
}

pub export fn cbm_pipeline_emit_spawn(
    gbuf: ?*Gbuf,
    source: ?*const Node,
    call: ?*const Call,
    spawn: ?*const Spawn,
) void {
    const gb = gbuf orelse return;
    const src = source orelse return;
    const cl = call orelse return;
    const sp = spawn orelse return;
    emit(gb, src, cl, sp);
}
