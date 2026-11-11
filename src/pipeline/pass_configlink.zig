//! pass_configlink.zig — Config ↔ Code linking strategies (pre-dump pass).
//!
//! Three strategies link config files to code symbols:
//!   1. Key→Symbol: normalized config key matches code function/variable name
//!   2. Dep→Import: package manifest dependency matches IMPORTS edge target
//!   3. File→Ref: source code string literal references config file path
//!
//! Operates on the graph buffer before dump to .db file.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;
const NodeRefs = [*c]const [*c]const Node;

// ── Config link confidence scores ───────────────────────────────────
// Strategy 1: Key→Symbol matching
const CONF_KEY_EXACT: f64 = 0.85;
const CONF_KEY_SUBSTRING: f64 = 0.75;
// Strategy 2: Dep→Import matching
const CONF_DEP_EXACT: f64 = 0.95;
const CONF_DEP_QN_SUBSTR: f64 = 0.80;

// ── Manifest / dep section tables ───────────────────────────────────

fn isManifestFile(basename: CStr) bool {
    const names = [_][*:0]const u8{
        "Cargo.toml",       "package.json",  "go.mod",
        "requirements.txt", "Gemfile",       "build.gradle",
        "pom.xml",          "composer.json",
    };
    for (names) |name| {
        if (streq(basename, name)) return true;
    }
    return false;
}

const DEP_SECTIONS = [_][*:0]const u8{
    "dependencies", "devdependencies", "peerdependencies", "dev-dependencies", "build-dependencies",
};

fn isDepSection(s: CStr) bool {
    for (DEP_SECTIONS) |sec| {
        if (c.cbm_strcasestr(s, sec) != null) return true;
    }
    return false;
}

// ── Strategy 1: Config Key → Code Symbol ────────────────────────────

const ConfigEntry = extern struct {
    node_id: i64,
    normalized: [c.CBM_SZ_256]u8,
    name: [c.CBM_SZ_256]u8,
};

fn nodeRefQualifiedNameCompare(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const left: *const Node = @as(*const *const Node, @ptrCast(@alignCast(a))).*;
    const right: *const Node = @as(*const *const Node, @ptrCast(@alignCast(b))).*;
    return c.strcmp(left.qualified_name, right.qualified_name);
}

fn sortedNodeRefs(nodes: NodeRefs, count: c_int) [*c][*c]const Node {
    if (nodes == null or count <= 0) return null;
    const sorted = interop.mallocN([*c]const Node, ix(count));
    if (sorted == null) return null;
    @memcpy(sorted[0..ix(count)], nodes[0..ix(count)]);
    if (count > 1) {
        c.qsort(@ptrCast(sorted), ix(count), @sizeOf([*c]const Node), nodeRefQualifiedNameCompare);
    }
    return sorted;
}

/// Collect config Variable nodes with ≥2 tokens, each ≥3 chars.
fn collectConfigEntries(vars: NodeRefs, var_count: c_int, out: []ConfigEntry) c_int {
    const sorted = sortedNodeRefs(vars, var_count);
    if (var_count > 0 and sorted == null) return 0;
    var n: usize = 0;
    var i: usize = 0;
    while (i < var_count and n < out.len) : (i += 1) {
        const node: *const Node = sorted[i];
        if (!c.cbm_has_config_extension(node.file_path)) continue;

        var norm: [c.CBM_SZ_256]u8 = undefined;
        const tokens = c.cbm_normalize_config_key(node.name, &norm, norm.len);
        if (tokens < 2) continue;

        // Check all tokens ≥3 chars
        var all_long = true;
        var p: [*]const u8 = &norm;
        while (p[0] != 0) {
            const end: CStr = c.strchr(p, '_');
            const tlen: usize = if (end != null) util.ptrDiff(end, p) else c.strlen(p);
            if (tlen < c.CBM_SZ_3) {
                all_long = false;
                break;
            }
            p = if (end != null) end + 1 else p + tlen;
        }
        if (!all_long) continue;

        out[n].node_id = node.id;
        _ = c.snprintf(&out[n].normalized, out[n].normalized.len, "%s", @as(CStr, &norm));
        _ = c.snprintf(&out[n].name, out[n].name.len, "%s", node.name);
        n += 1;
    }
    c.free(@ptrCast(sorted));
    return @intCast(n);
}

/// Collect code nodes (Function/Variable/Class/Struct) not from config files.
const CodeEntry = extern struct {
    node_id: i64,
    normalized: [c.CBM_SZ_256]u8,
};

fn collectCodeEntries(gb: ?*c.cbm_gbuf_t, out: []CodeEntry) c_int {
    var n: usize = 0;
    // "Struct" alongside "Class": a config key may name a Go/Rust/Swift/D struct
    // type, which is now labelled "Struct" — keep it linkable.
    const labels = [_][*:0]const u8{ "Function", "Variable", "Class", "Struct" };

    for (labels) |label| {
        if (n >= out.len) break;
        var nodes: [*c][*c]const Node = null;
        var count: c_int = 0;
        if (c.cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) continue;

        const sorted = sortedNodeRefs(nodes, count);
        if (count > 0 and sorted == null) return @intCast(n);

        var i: usize = 0;
        while (i < count and n < out.len) : (i += 1) {
            const node: *const Node = sorted[i];
            if (c.cbm_has_config_extension(node.file_path)) continue;

            var norm: [c.CBM_SZ_256]u8 = undefined;
            const tokens = c.cbm_normalize_config_key(node.name, &norm, norm.len);
            if (tokens == 0 or norm[0] == 0) continue;

            out[n].node_id = node.id;
            _ = c.snprintf(&out[n].normalized, out[n].normalized.len, "%s", @as(CStr, &norm));
            n += 1;
        }
        c.free(@ptrCast(sorted));
    }
    return @intCast(n);
}

fn strategyKeySymbols(gb: ?*c.cbm_gbuf_t) c_int {
    // Get all Variable nodes
    var vars: [*c][*c]const Node = null;
    var var_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, "Variable", &vars, &var_count) != 0) return 0;

    var config_entries: [c.CBM_SZ_4K]ConfigEntry = undefined;
    const config_count = collectConfigEntries(vars, var_count, &config_entries);

    if (config_count == 0) return 0;

    var code_entries: [c.CBM_SZ_8K]CodeEntry = undefined;
    const code_count = collectCodeEntries(gb, &code_entries);

    var edge_count: c_int = 0;

    for (config_entries[0..ix(config_count)]) |*config| {
        for (code_entries[0..ix(code_count)]) |*code| {
            var confidence: f64 = 0.0;

            if (streq(&config.normalized, &code.normalized)) {
                // Exact match
                confidence = CONF_KEY_EXACT;
            } else if (c.strstr(&code.normalized, &config.normalized) != null) {
                // Substring match
                confidence = CONF_KEY_SUBSTRING;
            }

            if (confidence > 0.0) {
                var props: [c.CBM_SZ_512]u8 = undefined;
                _ = c.snprintf(
                    &props,
                    props.len,
                    "{\"strategy\":\"key_symbol\",\"confidence\":%.2f,\"config_key\":\"%s\"}",
                    confidence,
                    @as(CStr, &config.name),
                );

                _ = c.cbm_gbuf_insert_edge(gb, code.node_id, config.node_id, "CONFIGURES", &props);
                edge_count += 1;
            }
        }
    }

    return edge_count;
}

// ── Strategy 2: Dependency → Import ─────────────────────────────────

const DepEntry = extern struct {
    node_id: i64,
    name: [c.CBM_SZ_256]u8,
};

/// Check if a Cargo.toml QN contains a dependency section in any dotted part.
fn isCargoDepSection(qn: CStr) bool {
    var qn_copy: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(&qn_copy, qn_copy.len, "%s", qn);
    var saveptr: MutStr = null;
    var part: MutStr = c.strtok_r(&qn_copy, ".", &saveptr);
    while (part != null) {
        var lower: [c.CBM_SZ_128]u8 = undefined;
        var plen = c.strlen(part);
        if (plen >= lower.len) plen = lower.len - 1;
        for (0..plen) |j| lower[j] = std.ascii.toLower(part[j]);
        lower[plen] = 0;

        for (DEP_SECTIONS) |sec| {
            if (streq(&lower, sec)) return true;
        }
        part = c.strtok_r(null, ".", &saveptr);
    }
    return false;
}

fn collectManifestDeps(vars: NodeRefs, var_count: c_int, out: []DepEntry) c_int {
    var n: usize = 0;
    var i: usize = 0;
    while (i < var_count and n < out.len) : (i += 1) {
        const node: *const Node = vars[i];
        const base: CStr = c.cbm_path_base(node.file_path);
        if (!isManifestFile(base)) continue;

        var is_dep = node.qualified_name != null and isDepSection(node.qualified_name);

        if (!is_dep and streq(base, "Cargo.toml") and node.qualified_name != null) {
            is_dep = isCargoDepSection(node.qualified_name);
        }

        if (is_dep) {
            out[n].node_id = node.id;
            _ = c.snprintf(&out[n].name, out[n].name.len, "%s", node.name);
            n += 1;
        }
    }
    return @intCast(n);
}

/// Lowercase a string into buf.
fn lowercaseInto(buf: []u8, src: CStr) void {
    const len: usize = if (src != null) c.strlen(src) else 0;
    var j: usize = 0;
    while (j < len and j < buf.len - 1) : (j += 1) {
        buf[j] = std.ascii.toLower(src[j]);
    }
    buf[if (len < buf.len) len else buf.len - 1] = 0;
}

/// Match a dep name (lowercased) against an import target node.
/// Returns confidence > 0 on match, 0 on no match.
fn matchDepToImport(target: *const Node, dep_lower: CStr) f64 {
    var target_lower: [c.CBM_SZ_256]u8 = undefined;
    lowercaseInto(&target_lower, target.name);

    if (streq(&target_lower, dep_lower)) return CONF_DEP_EXACT;
    if (target.qualified_name != null) {
        var qn_lower: [c.CBM_SZ_512]u8 = undefined;
        lowercaseInto(&qn_lower, target.qualified_name);
        if (c.strstr(&qn_lower, dep_lower) != null) return CONF_DEP_QN_SUBSTR;
    }
    return 0.0;
}

fn strategyDepImports(gb: ?*c.cbm_gbuf_t) c_int {
    var vars: [*c][*c]const Node = null;
    var var_count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, "Variable", &vars, &var_count) != 0) return 0;

    var deps: [c.CBM_SZ_2K]DepEntry = undefined;
    const dep_count = collectManifestDeps(vars, var_count, &deps);

    if (dep_count == 0) return 0;

    // Get all IMPORTS edges
    var imports: [*c][*c]const Edge = null;
    var import_count: c_int = 0;
    if (c.cbm_gbuf_find_edges_by_type(gb, "IMPORTS", &imports, &import_count) != 0) return 0;

    var edge_count: c_int = 0;

    for (deps[0..ix(dep_count)]) |*dep| {
        var dep_lower: [c.CBM_SZ_256]u8 = undefined;
        lowercaseInto(&dep_lower, &dep.name);

        var ii: usize = 0;
        while (ii < import_count) : (ii += 1) {
            const target: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_id(gb, imports[ii].*.target_id)) orelse continue;
            const source: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_id(gb, imports[ii].*.source_id)) orelse continue;

            const confidence = matchDepToImport(target, &dep_lower);
            if (confidence > 0.0) {
                var props: [c.CBM_SZ_512]u8 = undefined;
                _ = c.snprintf(
                    &props,
                    props.len,
                    "{\"strategy\":\"dependency_import\",\"confidence\":%.2f,\"dep_name\":\"%s\"}",
                    confidence,
                    @as(CStr, &dep.name),
                );

                _ = c.cbm_gbuf_insert_edge(gb, source.id, dep.node_id, "CONFIGURES", &props);
                edge_count += 1;
            }
        }
    }

    // gbuf data is borrowed — no free
    return edge_count;
}

// ── Strategy 3: Config File Path → Code String Reference ────────────

/// True when a node of this label sits in a config file.
fn labelHasConfigFile(gb: ?*c.cbm_gbuf_t, label: CStr) bool {
    var nodes: [*c][*c]const Node = null;
    var count: c_int = 0;
    if (c.cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) return false;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        if (c.cbm_has_config_extension(nodes[i].*.file_path)) return true;
    }
    return false;
}

fn logStrategy(name: CStr, edges: c_int, buf: *[c.CBM_SZ_16]u8) void {
    _ = c.snprintf(buf, buf.len, "%d", edges);
    interop.logInfo("configlinker.strategy", .{
        @as(CStr, "name"), name, @as(CStr, "edges"), @as(CStr, buf),
    });
}

pub export fn cbm_pipeline_pass_configlink(ctx: *c.cbm_pipeline_ctx_t) c_int {
    const gb = ctx.gbuf;
    // Early exit: check if any config files exist in the project.
    const has_config = labelHasConfigFile(gb, "Variable") or labelHasConfigFile(gb, "Module");

    if (!has_config) {
        interop.logInfo("configlinker.skip", .{ @as(CStr, "reason"), @as(CStr, "no_config_files") });
        return 0;
    }

    var buf1: [c.CBM_SZ_16]u8 = undefined;
    var buf2: [c.CBM_SZ_16]u8 = undefined;
    var buf3: [c.CBM_SZ_16]u8 = undefined;
    var buf4: [c.CBM_SZ_16]u8 = undefined;

    const key_edges = strategyKeySymbols(gb);
    logStrategy("key_symbol", key_edges, &buf1);

    const dep_edges = strategyDepImports(gb);
    logStrategy("dep_import", dep_edges, &buf2);

    // File refs: no longer reads from disk — config file path matching
    // is handled by CONFIGURES edges created during resolution.
    const ref_edges: c_int = 0;
    logStrategy("file_ref", ref_edges, &buf3);

    _ = c.snprintf(&buf4, buf4.len, "%d", key_edges + dep_edges + ref_edges);
    interop.logInfo("configlinker.done", .{ @as(CStr, "total"), @as(CStr, &buf4) });

    return key_edges + dep_edges + ref_edges;
}
