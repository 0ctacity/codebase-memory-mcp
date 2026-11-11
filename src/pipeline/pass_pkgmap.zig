//! pass_pkgmap.zig — Generic package/module manifest resolution.
//!
//! Scans discovered files for manifest files (package.json, go.mod, Cargo.toml,
//! pyproject.toml, composer.json, pubspec.yaml, pom.xml, build.gradle, mix.exs,
//! *.gemspec) and builds a hash table mapping bare package specifiers to resolved
//! module QNs. This enables IMPORTS edges for non-relative imports like
//! "@myorg/pkg", "github.com/foo/bar", "use my_crate::foo".
//!
//! Integration: called from parallel extract workers (per-worker local arrays)
//! and merged sequentially before registry build.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const ptrDiff = util.ptrDiff;
const HashTable = c.CBMHashTable;
const PkgEntries = c.cbm_pkg_entries_t;
const PkgEntry = c.cbm_pkg_entry_t;
const Node = c.cbm_gbuf_node_t;
const Gbuf = c.cbm_gbuf_t;
const Ctx = c.cbm_pipeline_ctx_t;
const JsonVal = ?*c.yyjson_val;

/// Read an entire file into a malloc'd buffer. Returns NULL on failure.
fn pkgmapReadFile(path: CStr, out_len: *c_int) MutStr {
    const f = c.cbm_fopen(path, "rb") orelse return null;
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    if (size <= 0 or size > c.CBM_SZ_1K * c.CBM_SZ_1K) { // 1MB cap for manifests
        _ = c.fclose(f);
        return null;
    }
    const buf: MutStr = @ptrCast(c.malloc(@as(usize, @intCast(size)) + 1));
    if (buf == null) {
        _ = c.fclose(f);
        return null;
    }
    const nread = c.fread(buf, 1, @intCast(size), f);
    _ = c.fclose(f);
    buf[nread] = 0;
    out_len.* = @intCast(nread);
    return buf;
}

// ── Constants ───────────────────────────────────────────────────────

const PKGMAP_INIT_CAP = 16;
const PKGMAP_PATH_BUF = 1024;
const PKGMAP_LINE_BUF = 512;
const PKGMAP_HT_INIT = 64;
/// Hard ceiling on recursive directory descent. This is the
/// non-negotiable termination guarantee for the manifest walk: even
/// if the filesystem contains directory junctions / symlink cycles
/// (the documented reason the walk was once disabled on Windows),
/// descent stops at this depth so the walk can never hang. 64 is far
/// deeper than any real source tree.
const PKGMAP_WALK_MAX_DEPTH = 64;

/// Check if the text at p (bounded by end) starts with prefix.
fn atPrefix(p: [*]const u8, end: [*]const u8, comptime prefix: []const u8) bool {
    return @intFromPtr(p) + prefix.len <= @intFromPtr(end) and
        std.mem.eql(u8, p[0..prefix.len], prefix);
}

/// p < end, for the pointer scans below.
inline fn before(p: [*]const u8, end: [*]const u8) bool {
    return @intFromPtr(p) < @intFromPtr(end);
}

fn isBlank(ch: u8) bool {
    return ch == ' ' or ch == '\t';
}

// ── Per-worker collection ───────────────────────────────────────────

pub export fn cbm_pkg_entries_init(e: *PkgEntries) void {
    e.items = null;
    e.count = 0;
    e.cap = 0;
}

fn pkgEntriesPush(e: *PkgEntries, pkg_name: MutStr, entry_rel: MutStr) void {
    if (e.count >= e.cap) {
        const new_cap: c_int = if (e.cap == 0) PKGMAP_INIT_CAP else e.cap * 2;
        const tmp = interop.reallocN(PkgEntry, e.items, ix(new_cap));
        if (tmp == null) {
            c.free(pkg_name);
            c.free(entry_rel);
            return;
        }
        e.items = tmp;
        e.cap = new_cap;
    }
    e.items[ix(e.count)].pkg_name = pkg_name;
    e.items[ix(e.count)].entry_rel = entry_rel;
    e.count += 1;
}

pub export fn cbm_pkg_entries_free(e: *PkgEntries) void {
    for (0..ix(e.count)) |i| {
        c.free(e.items[i].pkg_name);
        c.free(e.items[i].entry_rel);
    }
    c.free(e.items);
    e.items = null;
    e.count = 0;
    e.cap = 0;
}

// ── Helpers ─────────────────────────────────────────────────────────

/// Get the directory part of a relative path (without trailing slash).
/// Returns heap-allocated string. For "foo.json" returns "".
fn pathDirname(rel_path: CStr) MutStr {
    const last: CStr = c.strrchr(rel_path, '/');
    if (last == null) return c.strdup("");
    return c.cbm_strndup(rel_path, ptrDiff(last, rel_path));
}

/// Strip file extension from a path. Returns heap-allocated string.
/// "src/index.ts" → "src/index", "lib/main" → "lib/main"
fn stripExtension(path: CStr) MutStr {
    var i = c.strlen(path);
    while (i > 0) : (i -= 1) {
        if (path[i - 1] == '.') return c.cbm_strndup(path, i - 1);
        if (path[i - 1] == '/') break;
    }
    return c.strdup(path);
}

/// Join directory + relative entry path, normalize.
/// "packages/foo" + "src/index.ts" → "packages/foo/src/index" (stripped ext)
fn joinAndStrip(dir: CStr, entry_in: CStr) MutStr {
    var entry = entry_in;
    if (entry == null or entry[0] == 0) return null;
    // Skip leading ./ from entry
    if (entry[0] == '.' and entry[1] == '/') entry += 2;
    var buf: [PKGMAP_PATH_BUF]u8 = undefined;
    if (dir[0] == 0) {
        _ = c.snprintf(&buf, buf.len, "%s", entry);
    } else {
        _ = c.snprintf(&buf, buf.len, "%s/%s", dir, entry);
    }
    return stripExtension(&buf);
}

/// Find a line starting with a prefix in source. Returns pointer to first char
/// after prefix, or NULL. Handles leading whitespace.
fn findLineValue(src: [*]const u8, src_len: c_int, prefix: []const u8) ?[*]const u8 {
    var p = src;
    const end = src + ix(src_len);
    while (before(p, end)) {
        // Skip leading whitespace
        while (before(p, end) and isBlank(p[0])) p += 1;
        if (@intFromPtr(p) + prefix.len <= @intFromPtr(end) and std.mem.eql(u8, p[0..prefix.len], prefix)) {
            return p + prefix.len;
        }
        // Skip to next line
        while (before(p, end) and p[0] != '\n') p += 1;
        if (before(p, end)) p += 1; // skip \n
    }
    return null;
}

/// Extract a quoted string value from position. Handles both "..." and '...'
/// Returns heap-allocated string, or NULL.
fn extractQuoted(p_in: [*]const u8, end: [*]const u8) MutStr {
    var p = p_in;
    // Skip whitespace and = sign
    while (before(p, end) and (isBlank(p[0]) or p[0] == '=')) p += 1;
    if (!before(p, end)) return null;
    const quote = p[0];
    if (quote != '"' and quote != '\'') return null;
    p += 1;
    const start = p;
    while (before(p, end) and p[0] != quote and p[0] != '\n') p += 1;
    if (!before(p, end) or p[0] != quote) return null;
    return c.cbm_strndup(start, ptrDiff(p, start));
}

// ── Language-specific manifest parsers ──────────────────────────────

/// Resolve JS/TS entry point from exports["."] object.
fn resolveExportsDot(dot: JsonVal) CStr {
    @setEvalBranchQuota(10000);
    if (c.yyjson_is_str(dot)) return c.yyjson_get_str(dot);
    if (!c.yyjson_is_obj(dot)) return null;
    for ([_][*:0]const u8{ "import", "default", "require" }) |key| {
        const v = c.yyjson_obj_get(dot, key);
        if (c.yyjson_is_str(v)) return c.yyjson_get_str(v);
    }
    return null;
}

/// Resolve JS/TS entry point from package.json root.
fn resolvePkgEntry(root: JsonVal) CStr {
    @setEvalBranchQuota(10000);
    const exports = c.yyjson_obj_get(root, "exports");
    if (c.yyjson_is_obj(exports)) {
        const e = resolveExportsDot(c.yyjson_obj_get(exports, "."));
        if (e != null) return e;
    }
    for ([_][*:0]const u8{ "main", "module" }) |key| {
        const v = c.yyjson_obj_get(root, key);
        if (c.yyjson_is_str(v)) return c.yyjson_get_str(v);
    }
    return "src/index.ts"; // last resort default
}

/// JS/TS: package.json — name + entry point resolution
fn parsePackageJson(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    @setEvalBranchQuota(10000);
    const doc = c.yyjson_read(source, ix(source_len), 0);
    if (doc == null) return;
    defer c.yyjson_doc_free(doc);
    const root = c.yyjson_doc_get_root(doc);
    if (!c.yyjson_is_obj(root)) return;

    const name_val = c.yyjson_obj_get(root, "name");
    if (!c.yyjson_is_str(name_val)) return;
    const name: CStr = c.yyjson_get_str(name_val);
    if (name == null or name[0] == 0) return;

    const entry = resolvePkgEntry(root);
    if (entry != null) {
        const dir = pathDirname(rel_path);
        const resolved = joinAndStrip(dir, entry);
        if (resolved != null) {
            pkgEntriesPush(entries, c.strdup(name), resolved);
        }
        c.free(dir);
    }
}

/// Go: go.mod — module directive
fn parseGoMod(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const end = source + ix(source_len);
    var val = findLineValue(source, source_len, "module ") orelse return;
    // Extract module path (rest of line, trimmed)
    while (before(val, end) and isBlank(val[0])) val += 1;
    const start = val;
    while (before(val, end) and val[0] != '\n' and val[0] != '\r' and val[0] != ' ') val += 1;
    if (@intFromPtr(val) <= @intFromPtr(start)) return;
    const module_path = c.cbm_strndup(start, ptrDiff(val, start));
    const dir = pathDirname(rel_path);

    // The module path maps to the directory containing go.mod.
    // For "." dir, use empty string.
    pkgEntriesPush(entries, module_path, c.strdup(dir));
    c.free(dir);
}

/// Extract "name" value from a TOML section. Scans from section_start until
/// the next [...] header or EOF. Returns heap string or NULL.
fn tomlExtractName(section_start: [*]const u8, end: [*]const u8) MutStr {
    var p = section_start;
    while (before(p, end)) {
        while (before(p, end) and isBlank(p[0])) p += 1;
        if (before(p, end) and p[0] == '[') break;
        if (atPrefix(p, end, "name ")) return extractQuoted(p + "name ".len, end);
        if (atPrefix(p, end, "name=")) return extractQuoted(p + "name=".len, end);
        while (before(p, end) and p[0] != '\n') p += 1;
        if (before(p, end)) p += 1;
    }
    return null;
}

/// "<dir>/" for a non-empty dir, "" otherwise: the two snprintf arguments the
/// C code wrote as `dir[0] ? dir : "", dir[0] ? "/" : ""`.
const DirPrefix = struct {
    dir: CStr,
    sep: CStr,

    fn of(dir: CStr) DirPrefix {
        return if (dir[0] != 0) .{ .dir = dir, .sep = "/" } else .{ .dir = "", .sep = "" };
    }
};

/// Build entry path: dir/suffix or just suffix if dir is empty.
fn buildEntryPath(rel_path: CStr, suffix: CStr) MutStr {
    const dir = pathDirname(rel_path);
    const prefix = DirPrefix.of(dir);
    var buf: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&buf, buf.len, "%s%s%s", prefix.dir, prefix.sep, suffix);
    c.free(dir);
    return c.strdup(&buf);
}

/// Rust: Cargo.toml — [package] name
fn parseCargoToml(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const section = findLineValue(source, source_len, "[package]") orelse return;
    const name = tomlExtractName(section, source + ix(source_len));
    if (name == null) return;
    const entry = buildEntryPath(rel_path, "src/lib");
    pkgEntriesPush(entries, name, entry);
}

/// Normalize Python package name: hyphens → underscores (PEP 503).
fn pyNormalizeName(name: [*:0]u8) void {
    var p = name;
    while (p[0] != 0) : (p += 1) {
        if (p[0] == '-') p[0] = '_';
    }
}

/// Python: pyproject.toml — [project] name
fn parsePyprojectToml(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const section = findLineValue(source, source_len, "[project]") orelse return;
    const name = tomlExtractName(section, source + ix(source_len));
    if (name == null) return;
    pyNormalizeName(name);

    // Register src/<name>/__init__ as primary entry
    var suffix: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&suffix, suffix.len, "src/%s/__init__", name);
    const entry = buildEntryPath(rel_path, &suffix);
    const name_copy: MutStr = c.strdup(name);
    pkgEntriesPush(entries, name, entry);

    // Also register <name>/__init__ as alternative (no src/ prefix)
    _ = c.snprintf(&suffix, suffix.len, "%s/__init__", name_copy);
    const alt_entry = buildEntryPath(rel_path, &suffix);
    if (name_copy != null and alt_entry != null) {
        pkgEntriesPush(entries, name_copy, alt_entry);
    } else {
        c.free(name_copy);
        c.free(alt_entry);
    }
}

/// Extract PSR-4 entries from one composer.json autoload section ("autoload"
/// or "autoload-dev": composer registers both, so test namespaces such as
/// Tests\ map to their directory as well).
fn extractPsr4(root: JsonVal, section: [*:0]const u8, dir: CStr, entries: *PkgEntries) void {
    @setEvalBranchQuota(10000);
    const autoload = c.yyjson_obj_get(root, section);
    if (!c.yyjson_is_obj(autoload)) return;
    const psr4 = c.yyjson_obj_get(autoload, "psr-4");
    if (!c.yyjson_is_obj(psr4)) return;
    var iter = c.yyjson_obj_iter_with(psr4);
    while (true) {
        const key = c.yyjson_obj_iter_next(&iter);
        if (key == null) break;
        const val = c.yyjson_obj_iter_get_val(key);
        if (!c.yyjson_is_str(key) or !c.yyjson_is_str(val)) continue;
        const ns_prefix: CStr = c.yyjson_get_str(key);
        var ns_dir: CStr = c.yyjson_get_str(val);
        while (ns_dir[0] == '.' and ns_dir[1] == '/') {
            ns_dir += 2; // "./src/" names src/
        }
        if (streq(ns_dir, ".")) {
            ns_dir = ""; // "." names the manifest's own directory
        }
        var ns_entry: [PKGMAP_PATH_BUF]u8 = undefined;
        if (dir[0] != 0) {
            _ = c.snprintf(&ns_entry, ns_entry.len, "%s/%s", dir, ns_dir);
        } else {
            _ = c.snprintf(&ns_entry, ns_entry.len, "%s", ns_dir);
        }
        const nelen = c.strlen(&ns_entry);
        if (nelen > 0 and ns_entry[nelen - 1] == '/') ns_entry[nelen - 1] = 0;
        pkgEntriesPush(entries, c.strdup(ns_prefix), c.strdup(&ns_entry));
    }
}

/// PHP: composer.json — name + PSR-4 autoload
fn parseComposerJson(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    @setEvalBranchQuota(10000);
    const doc = c.yyjson_read(source, ix(source_len), 0);
    if (doc == null) return;
    defer c.yyjson_doc_free(doc);
    const root = c.yyjson_doc_get_root(doc);
    if (!c.yyjson_is_obj(root)) return;

    const dir = pathDirname(rel_path);

    // Register package name → directory
    const name_val = c.yyjson_obj_get(root, "name");
    if (c.yyjson_is_str(name_val)) {
        const name: CStr = c.yyjson_get_str(name_val);
        if (name != null and name[0] != 0) {
            pkgEntriesPush(entries, c.strdup(name), c.strdup(dir));
        }
    }

    extractPsr4(root, "autoload", dir, entries);
    extractPsr4(root, "autoload-dev", dir, entries);

    c.free(dir);
}

/// Dart: pubspec.yaml — name
fn parsePubspecYaml(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const root = c.cbm_yaml_parse(source, source_len);
    if (root == null) return;
    const name: CStr = c.cbm_yaml_get_str(root, "name");
    if (name != null and name[0] != 0) {
        const dir = pathDirname(rel_path);
        const prefix = DirPrefix.of(dir);
        var entry: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&entry, entry.len, "%s%slib", prefix.dir, prefix.sep);
        pkgEntriesPush(entries, c.strdup(name), c.strdup(&entry));
        c.free(dir);
    }
    c.cbm_yaml_free(root);
}

/// Extract text content of an XML tag at position p. Returns heap string or NULL.
/// p must point to the char after the opening tag's '>'.
fn xmlTagContent(p_in: [*]const u8, end: [*]const u8) MutStr {
    var p = p_in;
    const s = p;
    while (before(p, end) and p[0] != '<') p += 1;
    if (@intFromPtr(p) <= @intFromPtr(s)) return null;
    return c.cbm_strndup(s, ptrDiff(p, s));
}

/// Scan pom.xml for a top-level XML tag (outside <parent>). Returns heap string or NULL.
fn pomFindTag(source: [*]const u8, end: [*]const u8, comptime tag: []const u8) MutStr {
    var p = source;
    var in_parent = false;
    while (before(p, end)) : (p += 1) {
        if (atPrefix(p, end, "<parent>")) in_parent = true;
        if (atPrefix(p, end, "</parent>")) in_parent = false;
        if (!in_parent and atPrefix(p, end, tag)) {
            return xmlTagContent(p + tag.len, end);
        }
    }
    return null;
}

/// Java: pom.xml — <groupId> + <artifactId>
fn parsePomXml(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const end = source + ix(source_len);
    const group_id = pomFindTag(source, end, "<groupId>");
    const artifact_id = pomFindTag(source, end, "<artifactId>");

    if (group_id != null and artifact_id != null) {
        // Map: "com.myorg.myapp" → src/main/java directory
        var pkg_name: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&pkg_name, pkg_name.len, "%s.%s", group_id, artifact_id);
        const dir = pathDirname(rel_path);
        const prefix = DirPrefix.of(dir);
        var entry: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&entry, entry.len, "%s%ssrc/main/java", prefix.dir, prefix.sep);
        pkgEntriesPush(entries, c.strdup(&pkg_name), c.strdup(&entry));

        // Also register just the groupId for package-level imports
        var grp_entry: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&grp_entry, grp_entry.len, "%s%ssrc/main/java", prefix.dir, prefix.sep);
        pkgEntriesPush(entries, c.strdup(group_id), c.strdup(&grp_entry));
        c.free(dir);
    }

    c.free(group_id);
    c.free(artifact_id);
}

/// Gradle: build.gradle / build.gradle.kts — group = '...'
fn parseBuildGradle(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const end = source + ix(source_len);
    // Look for group = '...' or group '...' or group = "..."
    const val = findLineValue(source, source_len, "group") orelse return;
    const group = extractQuoted(val, end);
    if (group == null) return;
    const dir = pathDirname(rel_path);
    const prefix = DirPrefix.of(dir);
    // Check for src/main/java or src/main/kotlin
    var entry: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&entry, entry.len, "%s%ssrc/main/java", prefix.dir, prefix.sep);
    pkgEntriesPush(entries, group, c.strdup(&entry));
    c.free(dir);
}

/// Elixir: mix.exs — app: :name
fn parseMixExs(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const end = source + ix(source_len);
    // Look for app: :app_name
    var val = findLineValue(source, source_len, "app:") orelse return;
    while (before(val, end) and isBlank(val[0])) val += 1;
    if (!before(val, end) or val[0] != ':') return;
    val += 1; // skip :
    const start = val;
    while (before(val, end) and val[0] != ',' and val[0] != '\n' and val[0] != ' ' and val[0] != ')') {
        val += 1;
    }
    if (@intFromPtr(val) <= @intFromPtr(start)) return;
    const app_name = c.cbm_strndup(start, ptrDiff(val, start));
    const dir = pathDirname(rel_path);
    const prefix = DirPrefix.of(dir);
    var entry: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&entry, entry.len, "%s%slib/%s", prefix.dir, prefix.sep, app_name);
    // Register with colon prefix as Elixir uses :atom syntax
    var atom_name: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&atom_name, atom_name.len, "%s", app_name);
    pkgEntriesPush(entries, c.strdup(&atom_name), c.strdup(&entry));
    c.free(app_name);
    c.free(dir);
}

/// Ruby: *.gemspec — spec.name = '...'
fn parseGemspec(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const end = source + ix(source_len);
    // Try spec.name, s.name, gem.name patterns
    const pattern = ".name";
    var p = source;
    while (before(p, end)) {
        const found: CStr = c.strstr(p, pattern);
        if (found == null or !before(found, end)) break;
        const name = extractQuoted(found + pattern.len, end);
        if (name != null) {
            const dir = pathDirname(rel_path);
            const prefix = DirPrefix.of(dir);
            var entry: [PKGMAP_PATH_BUF]u8 = undefined;
            _ = c.snprintf(&entry, entry.len, "%s%slib/%s", prefix.dir, prefix.sep, name);
            pkgEntriesPush(entries, name, c.strdup(&entry));
            c.free(dir);
            return;
        }
        p = found + 1;
    }
}

// Zig: build.zig — named modules for `@import("name")`. A module is declared
// with `b.addModule("name", .{ .root_source_file = b.path("src/root.zig") })`,
// or created into a variable (`const m = b.createModule(.{ .root_source_file =
// b.path(...) })`) that is registered under a name by `x.addImport("name", m)`
// or `.imports = &.{ .{ .name = "name", .module = m } }`. Keys carry a "zig:"
// prefix so a Zig module never collides with another ecosystem's package.
//
// A module of a dependency (`b.dependency("pkg", .{}).module("x")`) resolves
// when build.zig.zon declares the dependency by an in-repo `.path`: the
// dependency's build.zig `addModule("x", ...)` names its root. A dependency
// fetched by URL has no source in the repo and stays unmapped. Entries for
// this: "zigdep:<dir>|<pkg>" -> the dependency's directory (build.zig.zon in
// <dir>), "zigmod:<dir>|<x>" -> module x of the build.zig in <dir>, and
// "zig:<name>" -> "@<dir>|<pkg>|<x>", resolved by cbm_pkgmap_build.

const Text = ?[*]const u8;

fn bzFind(p_in: Text, end: [*]const u8, needle: []const u8) Text {
    var p = p_in orelse return null;
    while (@intFromPtr(p) + needle.len <= @intFromPtr(end)) : (p += 1) {
        if (std.mem.eql(u8, p[0..needle.len], needle)) return p;
    }
    return null;
}

fn bzSkipWs(p_in: [*]const u8, end: [*]const u8) [*]const u8 {
    var p = p_in;
    while (before(p, end) and (p[0] == ' ' or p[0] == '\t' or p[0] == '\n' or p[0] == '\r')) p += 1;
    return p;
}

fn bzIdentLen(p: [*]const u8, end: [*]const u8) usize {
    var n: usize = 0;
    while (before(p + n, end) and (p[n] == '_' or (p[n] >= 'a' and p[n] <= 'z') or
        (p[n] >= 'A' and p[n] <= 'Z') or (n > 0 and p[n] >= '0' and p[n] <= '9')))
    {
        n += 1;
    }
    return n;
}

/// memchr over [p, end).
fn bzMemchr(p: [*]const u8, ch: u8, end: [*]const u8) Text {
    return @ptrCast(c.memchr(p, ch, ptrDiff(end, p)));
}

/// A "..." string at p (after whitespace): heap copy, after.* past the quote.
fn bzQuoted(p_in: [*]const u8, end: [*]const u8, after: *[*]const u8) MutStr {
    const p = bzSkipWs(p_in, end);
    if (!before(p, end) or p[0] != '"') return null;
    const close = bzMemchr(p + 1, '"', end) orelse return null;
    after.* = close + 1;
    return c.cbm_strndup(p + 1, ptrDiff(close, p) - 1);
}

/// The `.root_source_file = x.path("...zig")` of the statement starting at p.
fn bzRootSource(p: [*]const u8, end: [*]const u8) MutStr {
    const stop = bzMemchr(p, ';', end) orelse end;
    const r = bzFind(p, stop, "root_source_file");
    const call = if (r != null) bzFind(r, stop, ".path(") else null;
    var after: [*]const u8 = undefined;
    var path: MutStr = if (call) |found| bzQuoted(found + ".path(".len, stop, &after) else null;
    if (path != null and (!c.cbm_str_ends_with(path, ".zig") or c.strstr(path, "..") != null)) {
        c.free(path);
        path = null;
    }
    return path;
}

/// The text after `const ident =` for the declaration of `ident`, or null.
fn bzConstValue(src: [*]const u8, end: [*]const u8, ident: []const u8) Text {
    var p = bzFind(src, end, "const ");
    while (p) |decl| : (p = bzFind(decl + 1, end, "const ")) {
        var q = bzSkipWs(decl + "const ".len, end);
        if (bzIdentLen(q, end) != ident.len or !std.mem.eql(u8, q[0..ident.len], ident)) continue;
        q = bzSkipWs(q + ident.len, end);
        if (before(q, end) and q[0] == '=') return q;
    }
    return null;
}

/// Root source of module variable `ident` (from its `const ident = ...;`).
fn bzVarSource(src: [*]const u8, end: [*]const u8, ident: []const u8) MutStr {
    const eq = bzConstValue(src, end, ident) orelse return null;
    return bzRootSource(eq, end);
}

/// The module variable at p (`m` followed by `)`/`,`/`}`): its root source.
fn bzModuleArgSource(src: [*]const u8, p_in: [*]const u8, end: [*]const u8) MutStr {
    const p = bzSkipWs(p_in, end);
    const len = bzIdentLen(p, end);
    const q = bzSkipWs(p + len, end);
    if (len == 0 or !before(q, end) or (q[0] != ')' and q[0] != ',' and q[0] != '}')) return null;
    return bzVarSource(src, end, p[0..len]);
}

fn bzPush(entries: *PkgEntries, rel_path: CStr, name: CStr, path: CStr, exported: bool) void {
    var key: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&key, key.len, "zig:%s", name);
    const dir = pathDirname(rel_path);
    var mod_key: [PKGMAP_PATH_BUF]u8 = undefined;
    _ = c.snprintf(&mod_key, mod_key.len, "zigmod:%s|%s", dir, name);
    const mod_entry: MutStr = if (exported) joinAndStrip(dir, path) else null;
    if (mod_entry != null) {
        pkgEntriesPush(entries, c.strdup(&mod_key), mod_entry);
    }
    const entry = joinAndStrip(dir, path);
    c.free(dir);
    if (entry != null) {
        pkgEntriesPush(entries, c.strdup(&key), entry);
    }
}

/// End of the expression at p: the `,` `)` `}` or `;` that closes it.
fn bzExprEnd(p_in: [*]const u8, end: [*]const u8) [*]const u8 {
    var nest: c_int = 0;
    var p = p_in;
    while (before(p, end)) : (p += 1) {
        if (p[0] == '"') {
            p = bzMemchr(p + 1, '"', end) orelse end;
        } else if (p[0] == '(' or p[0] == '{' or p[0] == '[') {
            nest += 1;
        } else if ((p[0] == ')' or p[0] == '}' or p[0] == ']') and nest > 0) {
            nest -= 1;
        } else if (nest == 0 and (p[0] == ',' or p[0] == ')' or p[0] == '}' or p[0] == ';')) {
            return p;
        }
    }
    return end;
}

/// The expression [b, e), or a bare identifier's `const` value.
fn bzDeref(src: [*]const u8, end: [*]const u8, b: *[*]const u8, e: *[*]const u8) void {
    const len = bzIdentLen(b.*, e.*);
    if (len > 0 and bzSkipWs(b.* + len, e.*) == e.*) {
        if (bzConstValue(src, end, b.*[0..len])) |eq| {
            const vb = bzSkipWs(eq + 1, end);
            b.* = vb;
            e.* = bzExprEnd(vb, end);
        }
    }
}

/// A module argument `dep.module("x")` / `b.dependency("pkg", ..).module("x")`
/// (or a `const` holding one): push "zig:<name>" -> "@<dir>|<pkg>|<x>".
fn bzPushDepModule(entries: *PkgEntries, rel_path: CStr, name: CStr, src: [*]const u8, p: [*]const u8, end: [*]const u8) void {
    var b = bzSkipWs(p, end);
    var e = bzExprEnd(b, end);
    bzDeref(src, end, &b, &e);
    var m: Text = null;
    var q = bzFind(b, e, ".module(");
    while (q) |found| : (q = bzFind(found + 1, e, ".module(")) {
        m = found;
    }
    var after: [*]const u8 = undefined;
    const mod: MutStr = if (m) |found| bzQuoted(found + ".module(".len, e, &after) else null;
    var db = b;
    var de: Text = m;
    if (mod != null) {
        var de_end = m.?;
        bzDeref(src, end, &db, &de_end);
        de = de_end;
    }
    const d: Text = if (mod != null) bzFind(db, de.?, ".dependency(") else null;
    const pkg: MutStr = if (d) |found| bzQuoted(found + ".dependency(".len, de.?, &after) else null;
    if (pkg != null) {
        var key: [PKGMAP_PATH_BUF]u8 = undefined;
        var val: [PKGMAP_PATH_BUF]u8 = undefined;
        const dir = pathDirname(rel_path);
        _ = c.snprintf(&key, key.len, "zig:%s", name);
        _ = c.snprintf(&val, val.len, "@%s|%s|%s", dir, pkg, mod);
        c.free(dir);
        pkgEntriesPush(entries, c.strdup(&key), c.strdup(&val));
    }
    c.free(mod);
    c.free(pkg);
}

/// build.zig.zon: `.dependencies = .{ .pkg = .{ .path = "libs/pkg" }, ... }`.
fn parseBuildZigZon(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    // Blank `//` comments (outside strings), so a commented-out dependency
    // is not one.
    const len = ix(source_len);
    const text: MutStr = c.cbm_strndup(source, len);
    if (text == null) return;
    var in_str = false;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        if (in_str) {
            in_str = !(text[i] == '"' and text[i - 1] != '\\');
        } else if (text[i] == '"') {
            in_str = true;
        } else if (text[i] == '/' and i + 1 < len and text[i + 1] == '/') {
            while (i < len and text[i] != '\n') : (i += 1) text[i] = ' ';
        }
    }
    const end: [*]const u8 = @as([*]const u8, text) + len;
    const deps = bzFind(text, end, ".dependencies");
    const open = if (deps != null) bzFind(deps, end, ".{") else null;
    const dir = pathDirname(rel_path);
    if (open) |brace| {
        const close = bzExprEnd(brace + 2, end);
        var p: [*]const u8 = brace + 2;
        while (before(p, close)) {
            p = bzSkipWs(p, close);
            if (!before(p, close) or p[0] != '.') break;
            p += 1;
            var after = p;
            var name: MutStr = null;
            if (p[0] == '@') {
                name = bzQuoted(p + 1, close, &after);
            } else {
                const n = bzIdentLen(p, close);
                name = if (n > 0) c.cbm_strndup(p, n) else null;
                after = p + n;
            }
            const eq = bzSkipWs(after, close);
            const body = if (before(eq, close) and eq[0] == '=') bzSkipWs(eq + 1, close) else close;
            const body_end = bzExprEnd(body, close);
            const path: Text = if (name != null) bzFind(body, body_end, ".path") else null;
            const peq: Text = if (path) |found| bzSkipWs(found + ".path".len, body_end) else null;
            var rel: MutStr = null;
            if (peq) |assign| {
                if (before(assign, body_end) and assign[0] == '=') {
                    rel = bzQuoted(assign + 1, body_end, &after);
                }
            }
            if (rel != null) {
                var rl = c.strlen(rel);
                while (rl > 0 and rel[rl - 1] == '/') {
                    rl -= 1;
                    rel[rl] = 0;
                }
                const r: CStr = if (rel[0] == '.' and rel[1] == '/') rel + 2 else rel;
                var key: [PKGMAP_PATH_BUF]u8 = undefined;
                var val: [PKGMAP_PATH_BUF]u8 = undefined;
                _ = c.snprintf(&key, key.len, "zigdep:%s|%s", dir, name);
                if (dir[0] == 0) {
                    _ = c.snprintf(&val, val.len, "%s", r);
                } else {
                    _ = c.snprintf(&val, val.len, "%s/%s", dir, r);
                }
                pkgEntriesPush(entries, c.strdup(&key), c.strdup(&val));
            }
            c.free(name);
            c.free(rel);
            p = if (before(body_end, close) and body_end[0] == ',') body_end + 1 else close;
        }
    }
    c.free(dir);
    c.free(text);
}

fn parseBuildZig(source: [*]const u8, source_len: c_int, rel_path: CStr, entries: *PkgEntries) void {
    const end = source + ix(source_len);
    var after: [*]const u8 = undefined;
    var p = bzFind(source, end, "addModule(");
    while (p) |found| : (p = bzFind(found + 1, end, "addModule(")) {
        const name = bzQuoted(found + "addModule(".len, end, &after);
        const path: MutStr = if (name != null) bzRootSource(after, end) else null;
        if (path != null) bzPush(entries, rel_path, name, path, true);
        c.free(name);
        c.free(path);
    }
    p = bzFind(source, end, "addImport(");
    while (p) |found| : (p = bzFind(found + 1, end, "addImport(")) {
        const name = bzQuoted(found + "addImport(".len, end, &after);
        const comma: Text = if (name != null) bzSkipWs(after, end) else null;
        const has_arg = if (comma) |sep| before(sep, end) and sep[0] == ',' else false;
        const path: MutStr = if (has_arg) bzModuleArgSource(source, comma.? + 1, end) else null;
        if (path != null) {
            bzPush(entries, rel_path, name, path, false);
        } else if (has_arg) {
            bzPushDepModule(entries, rel_path, name, source, comma.? + 1, end);
        }
        c.free(name);
        c.free(path);
    }
    p = bzFind(source, end, ".name");
    while (p) |found| : (p = bzFind(found + 1, end, ".name")) {
        const eq = bzSkipWs(found + ".name".len, end);
        const name: MutStr = if (before(eq, end) and eq[0] == '=') bzQuoted(eq + 1, end, &after) else null;
        const close: Text = if (name != null) bzMemchr(after, '}', end) else null;
        const mod: Text = if (close) |brace| bzFind(after, brace, ".module") else null;
        const meq: Text = if (mod) |m| bzSkipWs(m + ".module".len, close.?) else null;
        const has_arg = if (meq) |assign| before(assign, close.?) and assign[0] == '=' else false;
        const path: MutStr = if (has_arg) bzModuleArgSource(source, meq.? + 1, end) else null;
        if (path != null) {
            bzPush(entries, rel_path, name, path, false);
        } else if (has_arg) {
            bzPushDepModule(entries, rel_path, name, source, meq.? + 1, end);
        }
        c.free(name);
        c.free(path);
    }
}

// ── Public: manifest detection + parsing ────────────────────────────

pub export fn cbm_pkgmap_try_parse(basename: CStr, rel_path: CStr, source_c: CStr, source_len: c_int, entries: *PkgEntries) bool {
    if (basename == null or source_c == null or source_len <= 0) return false;
    const source: [*]const u8 = source_c;

    const Parser = *const fn ([*]const u8, c_int, CStr, *PkgEntries) void;
    const by_name = [_]struct { [*:0]const u8, Parser }{
        .{ "package.json", parsePackageJson },
        .{ "go.mod", parseGoMod },
        .{ "Cargo.toml", parseCargoToml },
        .{ "pyproject.toml", parsePyprojectToml },
        .{ "composer.json", parseComposerJson },
        .{ "pubspec.yaml", parsePubspecYaml },
        .{ "pom.xml", parsePomXml },
        .{ "build.gradle", parseBuildGradle },
        .{ "build.gradle.kts", parseBuildGradle },
        .{ "mix.exs", parseMixExs },
    };
    for (by_name) |entry| {
        if (streq(basename, entry[0])) {
            entry[1](source, source_len, rel_path, entries);
            return true;
        }
    }
    if (c.cbm_str_ends_with(basename, ".gemspec")) {
        parseGemspec(source, source_len, rel_path, entries);
        return true;
    }
    if (streq(basename, "build.zig")) {
        parseBuildZig(source, source_len, rel_path, entries);
        return true;
    }
    if (streq(basename, "build.zig.zon")) {
        parseBuildZigZon(source, source_len, rel_path, entries);
        return true;
    }
    return false;
}

// ── Merge: per-worker entries → hash table ──────────────────────────

/// A composer.json `autoload.psr-4` key: a namespace prefix ending in '\\'.
fn pkgmapIsPsr4Prefix(pkg_name: CStr) bool {
    const n: usize = if (pkg_name != null) c.strlen(pkg_name) else 0;
    return n > 0 and pkg_name[n - 1] == '\\';
}

/// The entry_rel recorded for key, or NULL.
fn pkgmapRawEntry(worker_entries: [*c]PkgEntries, worker_count: c_int, key: CStr) CStr {
    for (0..ix(worker_count)) |w| {
        for (0..ix(worker_entries[w].count)) |i| {
            if (streq(worker_entries[w].items[i].pkg_name, key)) {
                return worker_entries[w].items[i].entry_rel;
            }
        }
    }
    return null;
}

/// Zig "zig:<name>" -> "@<dir>|<pkg>|<x>": module x of the build.zig in the
/// directory build.zig.zon in <dir> gives dependency pkg. Returns the number
/// of entries added.
fn pkgmapResolveZigDeps(map: ?*HashTable, worker_entries: [*c]PkgEntries, worker_count: c_int) c_int {
    var added: c_int = 0;
    for (0..ix(worker_count)) |w| {
        for (0..ix(worker_entries[w].count)) |i| {
            const name: CStr = worker_entries[w].items[i].pkg_name;
            const v: CStr = worker_entries[w].items[i].entry_rel;
            const bar1: CStr = if (v[0] == '@') c.strchr(v, '|') else null;
            const bar2: CStr = if (bar1 != null) c.strchr(bar1 + 1, '|') else null;
            if (bar2 == null or c.cbm_ht_has(map, name)) continue;
            var key: [PKGMAP_PATH_BUF]u8 = undefined;
            _ = c.snprintf(&key, key.len, "zigdep:%.*s", @as(c_int, @intCast(ptrDiff(bar2, v) - 1)), v + 1);
            const dep_dir = pkgmapRawEntry(worker_entries, worker_count, &key);
            if (dep_dir == null) continue;
            _ = c.snprintf(&key, key.len, "zigmod:%s|%s", dep_dir, bar2 + 1);
            const qn: CStr = @ptrCast(c.cbm_ht_get(map, &key));
            if (qn != null) {
                _ = c.cbm_ht_set(map, c.strdup(name), c.strdup(qn));
                added += 1;
            }
        }
    }
    return added;
}

pub export fn cbm_pkgmap_build(worker_entries: [*c]PkgEntries, worker_count: c_int, project_name: CStr) ?*HashTable {
    // Count total entries
    var total: c_int = 0;
    for (0..ix(worker_count)) |w| total += worker_entries[w].count;
    if (total == 0) return null;

    const map = c.cbm_ht_create(PKGMAP_HT_INIT);
    var merged: c_int = 0;

    for (0..ix(worker_count)) |w| {
        const we = util.at(worker_entries, w);
        for (0..ix(we.count)) |i| {
            const item = util.at(we.items, i);
            // Zig dependency entries are resolved below.
            if (item.entry_rel[0] == '@' or c.strncmp(item.pkg_name, "zigdep:", "zigdep:".len) == 0) {
                continue;
            }
            // Convert entry_rel to QN: project.dir.parts. A PSR-4 namespace
            // prefix (key ends in '\\') keeps its repo-relative DIRECTORY
            // instead: its readers map `Prefix\Sub\Class` onto a file path
            // (<dir>/Sub/Class.php), which a dotted QN cannot express (#1186).
            const entry_rel: CStr = item.entry_rel;
            const qn: MutStr = if (pkgmapIsPsr4Prefix(item.pkg_name))
                c.cbm_strndup(entry_rel, c.strlen(entry_rel))
            else
                c.cbm_pipeline_fqn_module(project_name, entry_rel);
            if (qn == null) continue;

            // Check for duplicate — first wins
            if (c.cbm_ht_has(map, item.pkg_name)) {
                c.free(qn);
                continue;
            }

            // Transfer ownership: key = strdup'd pkg_name, value = qn
            const key: MutStr = c.strdup(item.pkg_name);
            _ = c.cbm_ht_set(map, key, qn);
            merged += 1;
        }
    }

    merged += pkgmapResolveZigDeps(map, worker_entries, worker_count);
    if (merged == 0) {
        c.cbm_ht_free(map);
        return null;
    }
    interop.logInfo("pkgmap.build", .{ @as(CStr, "entries"), @as(CStr, c.cbm_log_itoa(merged)) });
    return map;
}

/// Returns true if basename is a package manifest we know how to parse.
/// Used by the filesystem walker; cbm_pkgmap_try_parse is the source of
/// truth for which basenames produce entries.
fn isPkgmapManifestBasename(basename: CStr) bool {
    if (basename == null) return false;
    const names = [_][*:0]const u8{
        "package.json",     "go.mod",       "Cargo.toml", "pyproject.toml",
        "composer.json",    "pubspec.yaml", "pom.xml",    "build.gradle",
        "build.gradle.kts", "mix.exs",      "build.zig",  "build.zig.zon",
    };
    for (names) |name| {
        if (streq(basename, name)) return true;
    }
    return c.cbm_str_ends_with(basename, ".gemspec");
}

const is_windows = builtin.os.tag == .windows;

/// Stat a path, skipping symlinks. Returns 0 on success, -1 to skip.
/// On POSIX, lstat + S_ISLNK avoids following symlink cycles. On Windows
/// the UTF-8-safe wide stat is used (cbm_stat_utf8: _wstat64 by wide path,
/// filling st_mode, st_size and st_mtime); reparse points (junctions/symlinks)
/// are detected separately by pkgmapIsReparsePoint below before we descend.
fn pkgmapSafeStat(abs_path: CStr, st: *c.struct_stat) c_int {
    if (is_windows) {
        return if (c.cbm_stat_utf8(abs_path, st) != 0) c.CBM_NOT_FOUND else 0;
    }
    if (c.lstat(abs_path, st) != 0) return c.CBM_NOT_FOUND;
    if ((st.st_mode & c.S_IFMT) == c.S_IFLNK) return c.CBM_NOT_FOUND;
    return 0;
}

/// True if abs_path is a Windows reparse point (directory junction or
/// symlink). Following these is the documented cause of the historic CI
/// hang, so we skip them before recursing. On POSIX this is a no-op
/// (symlinks are already filtered by pkgmapSafeStat's S_ISLNK check).
/// Note: the PKGMAP_WALK_MAX_DEPTH bound below is the hard termination
/// guarantee; this check is a best-effort early skip on top of it.
///
/// The C code asked GetFileAttributesW for FILE_ATTRIBUTE_REPARSE_POINT.
/// cbm_stat_no_follow asks the same question on Windows: it fails for a
/// reparse point, and the path is already known to stat successfully.
fn pkgmapIsReparsePoint(abs_path: CStr) bool {
    if (!is_windows) return false;
    var st: c.struct_stat = undefined;
    return c.cbm_stat_no_follow(abs_path, &st) != 0;
}

/// Recursive filesystem walker that finds and parses package manifest
/// files independently of the main discovery filter. The main discovery
/// filter intentionally hides package.json / composer.json etc. from
/// code indexing (they're config, not source), but pass_pkgmap still
/// needs to read them to resolve workspace imports. Skips directories
/// matched by the shared cbm_should_skip_dir helper so we don't walk
/// node_modules, .git, build, etc. Returns the number of manifests
/// parsed, accumulated across the whole walk.
///
/// Cross-platform: uses the portable cbm_opendir/cbm_readdir/cbm_closedir
/// API (same as src/discover) and the symlink-skipping pkgmapSafeStat.
/// Termination is guaranteed by the PKGMAP_WALK_MAX_DEPTH recursion bound —
/// even directory junctions / symlink cycles cannot make it hang. On Windows
/// we additionally skip reparse points before descending as a best-effort
/// early-out.
fn pkgmapWalkDir(abs_dir: CStr, rel_dir: CStr, entries: *PkgEntries, depth: c_int, excluded_dirs: [*c]MutStr, excluded_count: c_int) c_int {
    if (depth >= PKGMAP_WALK_MAX_DEPTH) {
        interop.logInfo("pkgmap.walk", .{
            @as(CStr, "depth_cap"), if (rel_dir != null and rel_dir[0] != 0) rel_dir else @as(CStr, "."),
        });
        return 0;
    }
    const dir = c.cbm_opendir(abs_dir) orelse return 0;
    var parsed: c_int = 0;
    while (true) {
        const entry = c.cbm_readdir(dir) orelse break;
        const name: CStr = &entry.*.name;
        if (name[0] == '.' and (name[1] == 0 or (name[1] == '.' and name[2] == 0))) continue;
        var abs_path: [PKGMAP_PATH_BUF]u8 = undefined;
        var rel_path: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&abs_path, abs_path.len, "%s/%s", abs_dir, name);
        if (rel_dir != null and rel_dir[0] != 0) {
            _ = c.snprintf(&rel_path, rel_path.len, "%s/%s", rel_dir, name);
        } else {
            _ = c.snprintf(&rel_path, rel_path.len, "%s", name);
        }
        var st: c.struct_stat = undefined;
        if (pkgmapSafeStat(&abs_path, &st) != 0) continue;
        if (util.isDirMode(&st)) {
            if (c.cbm_should_skip_dir(name, c.CBM_MODE_FULL) or
                c.cbm_pipeline_relpath_is_excluded(&rel_path, excluded_dirs, excluded_count) != 0)
            {
                continue;
            }
            // Don't follow Windows directory junctions — they can form
            // cycles. The depth bound is the hard guarantee; this just
            // avoids wasted descent. (POSIX symlinks are already skipped by
            // pkgmapSafeStat's S_ISLNK check.)
            if (pkgmapIsReparsePoint(&abs_path)) continue;
            parsed += pkgmapWalkDir(&abs_path, &rel_path, entries, depth + 1, excluded_dirs, excluded_count);
            continue;
        }
        if ((st.st_mode & c.S_IFMT) != c.S_IFREG) continue;
        if (!isPkgmapManifestBasename(name)) continue;
        var source_len: c_int = 0;
        const source = pkgmapReadFile(&abs_path, &source_len);
        if (source == null) continue;
        if (cbm_pkgmap_try_parse(name, &rel_path, source, source_len, entries)) parsed += 1;
        c.free(source);
    }
    _ = c.cbm_closedir(dir);
    return parsed;
}

/// Scan a repository for package manifest files via the filesystem
/// walker above. Always-available companion to the parallel path's
/// per-worker manifest parsing, which is bound to whatever `files[]`
/// the discoverer produces and therefore misses ignored manifests like
/// package.json. NULL-safe; returns 0 entries when repo_path is unset.
///
/// Cross-platform: the walk runs on every platform via pkgmapWalkDir,
/// which is depth-bounded (PKGMAP_WALK_MAX_DEPTH) and skips symlinks /
/// Windows reparse points, so it cannot hang on directory junctions.
/// This is what lets bare workspace imports (e.g. "@org/pkg" declared in
/// an ignored package.json) resolve on Windows as well as POSIX.
pub export fn cbm_pkgmap_scan_repo(repo_path: CStr, entries_c: ?*PkgEntries, excluded_dirs: [*c]MutStr, excluded_count: c_int) c_int {
    const entries = entries_c orelse return 0;
    if (repo_path == null) return 0;
    const parsed = pkgmapWalkDir(repo_path, "", entries, 0, excluded_dirs, excluded_count);
    interop.logInfo("pkgmap.scan_repo", .{ @as(CStr, "manifests"), @as(CStr, c.cbm_log_itoa(parsed)) });
    return parsed;
}

fn pkgmapFreeEntry(key: CStr, value: ?*anyopaque, userdata: ?*anyopaque) callconv(.c) void {
    _ = userdata;
    c.free(@constCast(key));
    c.free(value);
}

pub export fn cbm_pkgmap_free(pkgmap_c: ?*HashTable) void {
    const pkgmap = pkgmap_c orelse return;
    c.cbm_ht_foreach(pkgmap, pkgmapFreeEntry, null);
    c.cbm_ht_free(pkgmap);
}

// ── Resolver ────────────────────────────────────────────────────────

/// Try slash-based prefix matching (Go: github.com/foo/bar/pkg/utils).
/// Returns heap QN or NULL.
fn resolveSlashPrefix(map: ?*HashTable, module_path: CStr) MutStr {
    const buf: MutStr = c.strdup(module_path);
    if (buf == null) return null;
    var i = c.strlen(buf);
    // for (slash = buf + strlen(buf) - 1; slash > buf; slash--)
    while (i > 1) {
        i -= 1;
        if (buf[i] != '/') continue;
        buf[i] = 0;
        const base_qn: CStr = @ptrCast(c.cbm_ht_get(map, buf));
        if (base_qn == null) continue;
        const subpath: CStr = module_path + i + 1;
        var result: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&result, result.len, "%s.%s", base_qn, subpath);
        // Replace / with . in the appended part
        var j = c.strlen(base_qn) + 1;
        while (result[j] != 0) : (j += 1) {
            if (result[j] == '/') result[j] = '.';
        }
        c.free(buf);
        return c.strdup(&result);
    }
    c.free(buf);
    return null;
}

/// Try dot-based prefix matching (Java: com.myorg.pkg.Foo).
/// Returns heap QN or NULL.
fn resolveDotPrefix(map: ?*HashTable, module_path: CStr, project_name: CStr) MutStr {
    const buf: MutStr = c.strdup(module_path);
    if (buf == null) return null;
    var i = c.strlen(buf);
    while (i > 1) {
        i -= 1;
        if (buf[i] != '.') continue;
        buf[i] = 0;
        const base_qn: CStr = @ptrCast(c.cbm_ht_get(map, buf));
        if (base_qn == null) continue;
        const subpath: CStr = module_path + i + 1;
        var subpath_slashed: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&subpath_slashed, subpath_slashed.len, "%s", subpath);
        var j: usize = 0;
        while (subpath_slashed[j] != 0) : (j += 1) {
            if (subpath_slashed[j] == '.') subpath_slashed[j] = '/';
        }
        var result: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&result, result.len, "%s/%s", base_qn, @as(CStr, &subpath_slashed));
        c.free(buf);
        return c.cbm_pipeline_fqn_module(project_name, &result);
    }
    c.free(buf);
    return null;
}

/// Try backslash-based prefix matching (PHP PSR-4: App\\Controllers\\Foo).
/// Returns heap QN or NULL.
fn resolveBackslashPrefix(map: ?*HashTable, module_path: CStr, project_name: CStr) MutStr {
    const buf: MutStr = c.strdup(module_path);
    if (buf == null) return null;
    var i = c.strlen(buf);
    while (i > 1) {
        i -= 1;
        if (buf[i] != '\\') continue;
        buf[i] = 0;
        var prefix: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&prefix, prefix.len, "%s\\", buf);
        const base_dir: CStr = @ptrCast(c.cbm_ht_get(map, &prefix));
        if (base_dir == null) continue;
        const subpath: CStr = module_path + i + 1;
        var path_result: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(
            &path_result,
            path_result.len,
            "%s%s%s",
            base_dir,
            @as(CStr, if (base_dir[0] != 0) "/" else ""),
            subpath,
        );
        var j: usize = 0;
        while (path_result[j] != 0) : (j += 1) {
            if (path_result[j] == '\\') path_result[j] = '/';
        }
        c.free(buf);
        return c.cbm_pipeline_fqn_module(project_name, &path_result);
    }
    c.free(buf);
    return null;
}

pub export fn cbm_pipeline_resolve_module(ctx: ?*const Ctx, source_rel: CStr, module_path: CStr) MutStr {
    const cx = ctx orelse return c.cbm_pipeline_fqn_module(null, module_path);
    if (module_path == null) return c.cbm_pipeline_fqn_module(cx.project_name, module_path);

    // 1. Try relative import resolution (existing logic)
    const resolved: MutStr = c.cbm_pipeline_resolve_relative_import(source_rel, module_path);
    if (resolved != null) {
        // The relative resolver has already removed an explicit JS/TS file
        // extension.  Treat the remaining path as a module path verbatim so a
        // dotted extensionless basename such as `featureX.engine` is not
        // stripped a second time by cbm_pipeline_fqn_module.
        const qn = c.cbm_pipeline_fqn_folder(cx.project_name, resolved);
        c.free(resolved);
        return qn;
    }

    // 1b. Try build-tool path aliases (tsconfig/jsconfig paths today;
    //     other loaders can register here later). Independent of pkgmap.
    if (cx.path_aliases != null and source_rel != null) {
        const amap = c.cbm_path_alias_find_for_file(cx.path_aliases, source_rel);
        if (amap != null) {
            const aliased: MutStr = c.cbm_path_alias_resolve(amap, module_path);
            if (aliased != null) {
                const qn = c.cbm_pipeline_fqn_module(cx.project_name, aliased);
                c.free(aliased);
                return qn;
            }
        }
    }

    // 2. No pkgmap → fall through immediately
    const pkgmap = cx.pkgmap;
    if (pkgmap == null) return c.cbm_pipeline_fqn_module(cx.project_name, module_path);

    // 2b. Zig: a named `@import("name")` is a build.zig module, never another
    //     ecosystem's package of the same name.
    if (source_rel != null and c.cbm_str_ends_with(source_rel, ".zig")) {
        var key: [PKGMAP_PATH_BUF]u8 = undefined;
        _ = c.snprintf(&key, key.len, "zig:%s", module_path);
        const zig_qn: CStr = @ptrCast(c.cbm_ht_get(pkgmap, &key));
        return if (zig_qn != null)
            c.strdup(zig_qn)
        else
            c.cbm_pipeline_fqn_module(cx.project_name, module_path);
    }

    // 3. Exact lookup
    const mapped_qn: CStr = @ptrCast(c.cbm_ht_get(pkgmap, module_path));
    if (mapped_qn != null) return c.strdup(mapped_qn);

    // 4. Prefix matching by separator type
    var result = resolveSlashPrefix(pkgmap, module_path);
    if (result == null) result = resolveDotPrefix(pkgmap, module_path, cx.project_name);
    if (result == null) result = resolveBackslashPrefix(pkgmap, module_path, cx.project_name);
    if (result != null) return result;

    // 5. Fallthrough to default resolution
    return c.cbm_pipeline_fqn_module(cx.project_name, module_path);
}

// ── Import-target node resolver ─────────────────────────────────────

/// Return the last path segment of an import string, recognizing the separators
/// used across languages: '.', '/', '\\', and "::".  Returns a pointer into
/// `path` (no allocation).
fn importLastSegment(path: CStr) CStr {
    var seg = path;
    var p = path;
    while (p[0] != 0) : (p += 1) {
        if (p[0] == '.' or p[0] == '/' or p[0] == '\\' or p[0] == ':') seg = p + 1;
    }
    return seg;
}

/// Derive a representative imported symbol name from a raw module/use path,
/// stripping language decorations so it can be matched against an in-graph node:
///   - trailing alias " as X"      (Rust/Kotlin)
///   - trailing glob "::*" / ".*"  (Rust/Kotlin/Java wildcard)
///   - brace groups "{a, b, ...}"  (Rust grouped use) → first member
/// Writes the result into `out` (size `outsz`) and returns it, or NULL if none.
/// For grouped/braced forms the first listed symbol is used as the representative
/// (the tests only require at least one resolved IMPORTS edge per statement).
fn importCandidateSymbol(module_path: CStr, out: MutStr, outsz: usize) CStr {
    if (module_path == null or module_path[0] == 0) return null;
    var buf: [1024]u8 = undefined;
    _ = c.snprintf(&buf, buf.len, "%s", module_path);

    // Brace group: `prefix::{a, b}` → take first member `a`.
    const brace: MutStr = c.strchr(&buf, '{');
    if (brace != null) {
        var first: MutStr = brace + 1;
        var end: MutStr = first;
        while (end[0] != 0 and end[0] != ',' and end[0] != '}') end += 1;
        end[0] = 0;
        // Trim whitespace.
        while (isBlank(first[0])) first += 1;
        var t: MutStr = first + c.strlen(first);
        while (@intFromPtr(t) > @intFromPtr(first) and isBlank((t - 1)[0])) {
            t -= 1;
            t[0] = 0;
        }
        if (first[0] != 0 and !streq(first, "self")) {
            _ = c.snprintf(out, outsz, "%s", first);
            return out;
        }
        // `{self, ...}` → fall back to the path before the brace group.
        brace[0] = 0;
    }

    // Strip trailing " as <alias>".
    const as: MutStr = c.strstr(&buf, " as ");
    if (as != null) as[0] = 0;

    // Strip trailing glob and separator noise.
    var len = c.strlen(&buf);
    while (len > 0 and (buf[len - 1] == '*' or buf[len - 1] == ':' or buf[len - 1] == '.' or
        buf[len - 1] == '/' or buf[len - 1] == '\\' or buf[len - 1] == ' '))
    {
        len -= 1;
        buf[len] = 0;
    }
    if (buf[0] == 0) return null;
    const seg = importLastSegment(&buf);
    if (seg == null or seg[0] == 0 or streq(seg, "*")) return null;
    _ = c.snprintf(out, outsz, "%s", seg);
    return out;
}

/// True for node labels that represent an importable definition (so a symbol-name
/// fallback does not link to, e.g., a Variable or Field).
fn importTargetableLabel(label: CStr) bool {
    if (label == null) return false;
    const ok = [_][*:0]const u8{
        "Class",  "Interface", "Function", "Method", "Module",
        "Struct", "Enum",      "Trait",    "Type",   "File",
    };
    for (ok) |l| {
        if (streq(l, label)) return true;
    }
    return false;
}

/// #1934: whether the name-guess import fallbacks — Strategy 1b (sibling file,
/// whose label filter admits symbols) and Strategy 3 (symbol name) — may run
/// for imports from this language. A Go import path names a package — never a
/// function, method
/// or field — and every correct Go import resolves in Strategy 1 (module path
/// → the package's Folder node); when that misses the import is external and
/// the correct result is NO edge. The fallback instead bound the last path
/// segment to an arbitrary same-named project symbol (`import "os/exec"` → a
/// test harness's exec() method, two imports → a Makefile target). Languages
/// whose import genuinely can name a member (Python `from m import f`, Java
/// `import com.example.Foo`, Rust `use crate::ops::helper`) keep it.
pub export fn cbm_import_symbol_fallback_allowed(lang: c.CBMLanguage) bool {
    return lang != c.CBM_LANG_GO;
}

/// "/" after a non-empty directory, "" after an empty one.
fn dirSep(dir: CStr) CStr {
    return if (dir[0] != 0) "/" else "";
}

/// True when `n` may be an import target of the file `source_file_qn`: it is
/// not that file itself.
fn notSelf(n: *const Node, source_file_qn: CStr) bool {
    return source_file_qn == null or n.qualified_name == null or
        !streq(n.qualified_name, source_file_qn);
}

/// Resolve a sibling-file import: a bare path/name (no leading "./") that names
/// a file relative to the importer's directory.  This covers build/markup
/// grammars whose import string is a sibling filename or directory rather than a
/// dotted module path:
///   - SCSS  `@use 'vars'`              → sibling `_vars.scss` (partial underscore)
///   - Just  `import 'common.just'`     → sibling `common.just`
///   - BitBake `require mypackage.inc`  → sibling `mypackage.inc`
///   - Meson `subdir('lib')`            → `lib/meson.build`
///   - func  `#include "utils.fc"`      → sibling `utils.fc`
///   - Pony  `use "util"`               → sibling `util.pony`
/// Builds a path relative to source_rel's directory, then looks up the resulting
/// File/Module-node QN (extension is stripped by fqn_module).  Returns a borrowed
/// node or NULL.  Several filename conventions are tried in turn.
fn resolveSiblingFile(ctx: *const Ctx, source_rel: CStr, source_file_qn: CStr, module_path: CStr) ?*const Node {
    if (module_path == null or module_path[0] == 0) return null;
    // Directory of the importing file (empty for repo-root files).
    const dir: MutStr = pathDirname(if (source_rel != null) source_rel else "");
    if (dir == null) return null;

    // Candidate relative paths, in priority order.
    var cands: [5][PKGMAP_PATH_BUF]u8 = undefined;
    var ncand: usize = 0;
    var base = module_path;
    // Skip a leading "./".
    if (base[0] == '.' and base[1] == '/') base += 2;
    // 1. Direct sibling: dir/<module_path>.
    _ = c.snprintf(&cands[ncand], PKGMAP_PATH_BUF, "%s%s%s", dir, dirSep(dir), base);
    ncand += 1;
    // 2. SCSS partial: dir/[subdir/]_<basename>.scss (underscore-prefixed).
    {
        const slash: CStr = c.strrchr(base, '/');
        const bn: CStr = if (slash != null) slash + 1 else base;
        const dpart: MutStr = if (slash != null)
            c.cbm_strndup(base, ptrDiff(slash, base))
        else
            c.strdup("");
        if (dpart != null and bn[0] != '_') {
            _ = c.snprintf(
                &cands[ncand],
                PKGMAP_PATH_BUF,
                "%s%s%s%s_%s.scss",
                dir,
                dirSep(dir),
                @as(CStr, if (dpart[0] != 0) dpart else ""),
                dirSep(dpart),
                bn,
            );
            ncand += 1;
        }
        c.free(dpart);
    }
    // 3. Meson subdir: dir/<module_path>/meson.build.
    _ = c.snprintf(&cands[ncand], PKGMAP_PATH_BUF, "%s%s%s/meson.build", dir, dirSep(dir), base);
    ncand += 1;
    // 4. Basename sibling: dir/<basename(module_path)>.  Covers include paths
    //    that carry a non-relative prefix (Hyprlang `source = ~/.config/.../x.conf`,
    //    absolute include paths) but reference a file sitting beside the importer.
    {
        const slash: CStr = c.strrchr(base, '/');
        if (slash != null and slash[1] != 0) {
            _ = c.snprintf(&cands[ncand], PKGMAP_PATH_BUF, "%s%s%s", dir, dirSep(dir), slash + 1);
            ncand += 1;
        }
    }

    var found: ?*const Node = null;
    for (cands[0..ncand]) |*cand| {
        const qn: MutStr = c.cbm_pipeline_fqn_module(ctx.project_name, cand);
        if (qn == null) continue;
        const n: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, qn);
        c.free(qn);
        if (n) |node| {
            if (importTargetableLabel(node.label) and notSelf(node, source_file_qn)) {
                found = node;
                break;
            }
        }
    }
    c.free(dir);
    return found;
}

// ── PHP PSR-4 class imports (#1186) ─────────────────────────────────

const Psr4Outcome = enum {
    /// no composer psr-4 prefix covers the import
    not_applicable,
    /// the class file exists: *out is its File node
    resolved,
    /// a prefix covers it but no class file exists
    unresolved,
};

fn pathLeaf(path: CStr) CStr {
    var leaf = path;
    var p = path;
    while (p != null and p[0] != 0) : (p += 1) {
        if (p[0] == '/' or p[0] == '\\') leaf = p + 1;
    }
    return leaf;
}

/// The File node at exactly `rel_path`, or NULL. Looked up through the name
/// index (File nodes are named by basename) and matched on the full path, so
/// a same-named file in another directory never qualifies.
fn psr4FileNode(gbuf: ?*const Gbuf, rel_path: CStr) ?*const Node {
    var hits: [*c][*c]const Node = null;
    var hit_count: c_int = 0;
    if (c.cbm_gbuf_find_by_name(gbuf, pathLeaf(rel_path), &hits, &hit_count) != 0 or hits == null) {
        return null;
    }
    for (0..ix(hit_count)) |i| {
        const n: *const Node = hits[i] orelse continue;
        if (n.label != null and streq(n.label, "File") and n.file_path != null and
            streq(n.file_path, rel_path))
        {
            return n;
        }
    }
    return null;
}

/// Resolve a PHP class import `use A\B\C;` the way composer's PSR-4 autoloader
/// does: for every autoload.psr-4 prefix covering the name, longest first, the
/// class lives in exactly <prefix-dir>/<rest>.php with sub-namespaces as
/// subdirectories; the first existing file wins. When a prefix covers the name
/// but no such file exists the import is UNRESOLVED: it must never fall through
/// to the namespace bucket, which binds it to whichever file of the namespace
/// came first. `use function` / `use const` name namespace members, not class
/// files, so they are not applicable.
fn resolvePhpPsr4Class(gbuf: ?*const Gbuf, source_file_qn: CStr, imp: *const c.CBMImport, out: *?*const Node) Psr4Outcome {
    out.* = null;
    const pkgmap = c.cbm_pipeline_get_pkgmap();
    if (pkgmap == null or imp.kind != c.CBM_IMPORT_KIND_DEFAULT) return .not_applicable;
    var name: CStr = imp.module_path;
    if (name[0] == '\\') name += 1; // fully qualified `use \App\X;`
    var key: [PKGMAP_PATH_BUF]u8 = undefined;
    const name_len = c.strlen(name);
    if (name_len == 0 or name_len >= key.len or c.strchr(name, '\\') == null) {
        return .not_applicable;
    }
    var covered = false;
    // Right to left: each '\' ends a candidate prefix, longest first.
    var cut = name_len - 1;
    while (cut > 0) : (cut -= 1) {
        if (name[cut - 1] != '\\') continue;
        @memcpy(key[0..cut], name[0..cut]);
        key[cut] = 0;
        const dir: CStr = @ptrCast(c.cbm_ht_get(pkgmap, &key));
        if (dir == null) continue;
        covered = true;
        var rel: [PKGMAP_PATH_BUF]u8 = undefined;
        const w = c.snprintf(&rel, rel.len, "%s%s%s.php", dir, dirSep(dir), name + cut);
        if (w <= 0 or ix(w) >= rel.len) continue;
        var j: usize = 0;
        while (rel[j] != 0) : (j += 1) {
            if (rel[j] == '\\') rel[j] = '/';
        }
        if (psr4FileNode(gbuf, &rel)) |file| {
            if (notSelf(file, source_file_qn)) {
                out.* = file;
                return .resolved;
            }
        }
    }
    return if (covered) .unresolved else .not_applicable;
}

fn psr4ClassLabel(label: CStr) bool {
    return label != null and (streq(label, "Class") or streq(label, "Interface") or
        streq(label, "Trait") or streq(label, "Enum"));
}

/// The QN of `member` on the class `cls` defined in `file_path`, or NULL when
/// that file defines no such class or the class has no such member.
fn psr4MemberInFile(gbuf: ?*const Gbuf, file_path: CStr, cls: CStr, member: CStr) CStr {
    var hits: [*c][*c]const Node = null;
    var hit_count: c_int = 0;
    if (c.cbm_gbuf_find_by_name(gbuf, cls, &hits, &hit_count) != 0 or hits == null) return null;
    for (0..ix(hit_count)) |i| {
        const cn: *const Node = hits[i] orelse continue;
        if (!psr4ClassLabel(cn.label) or cn.qualified_name == null or cn.file_path == null or
            !streq(cn.file_path, file_path))
        {
            continue;
        }
        var qn: [c.CBM_SZ_1K]u8 = undefined;
        const w = c.snprintf(&qn, qn.len, "%s.%s", cn.qualified_name, member);
        if (w <= 0 or ix(w) >= qn.len) return null;
        const m: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, &qn);
        return if (m) |node| node.qualified_name else null;
    }
    return null;
}

pub export fn cbm_pipeline_psr4_member_qn(gbuf: ?*const Gbuf, class_fqn: CStr, member: CStr, out_qn: *CStr) c.cbm_psr4_member_t {
    out_qn.* = null;
    if (gbuf == null or class_fqn == null or member == null or member[0] == 0) {
        return c.CBM_PSR4_MEMBER_NOT_COVERED;
    }
    var imp = std.mem.zeroes(c.CBMImport);
    imp.module_path = class_fqn;
    imp.kind = c.CBM_IMPORT_KIND_DEFAULT;
    var file: ?*const Node = null;
    const placed = resolvePhpPsr4Class(gbuf, null, &imp, &file);
    if (placed == .not_applicable) return c.CBM_PSR4_MEMBER_NOT_COVERED;
    // Covered from here on: composer would load this file or none, so a
    // missing file, class or member leaves the member unresolved.
    if (placed != .resolved or file.?.file_path == null) return c.CBM_PSR4_MEMBER_UNRESOLVED;
    var cls: CStr = c.strrchr(class_fqn, '\\');
    cls = if (cls != null) cls + 1 else class_fqn;
    out_qn.* = psr4MemberInFile(gbuf, file.?.file_path, cls, member);
    return if (out_qn.* != null) c.CBM_PSR4_MEMBER_RESOLVED else c.CBM_PSR4_MEMBER_UNRESOLVED;
}

/// Copies `module_path` into `buf` and cuts it at a brace group and at a
/// trailing " as <alias>".
fn copyUndecorated(buf: *[1024]u8, module_path: CStr) void {
    _ = c.snprintf(buf, buf.len, "%s", module_path);
    const brace: MutStr = c.strchr(buf, '{');
    if (brace != null) brace[0] = 0;
    const as: MutStr = c.strstr(buf, " as ");
    if (as != null) as[0] = 0;
}

/// Turns the '\\', ':' and '/' separators of a NUL-terminated path into '.'.
fn dotSeparators(buf: [*]u8) void {
    var i: usize = 0;
    while (buf[i] != 0) : (i += 1) {
        if (buf[i] == '\\' or buf[i] == ':' or buf[i] == '/') buf[i] = '.';
    }
}

pub export fn cbm_pipeline_resolve_import_node(
    ctx_c: ?*const Ctx,
    source_rel: CStr,
    source_file_qn: CStr,
    imp_c: ?*const c.CBMImport,
    namespace_map: ?*HashTable,
) ?*const Node {
    const ctx = ctx_c orelse return null;
    const imp = imp_c orelse return null;
    if (imp.module_path == null) return null;

    // PHP class imports covered by a composer psr-4 prefix name exactly one
    // file; when it is absent the import stays unresolved (#1186).
    var psr4_target: ?*const Node = null;
    switch (resolvePhpPsr4Class(ctx.gbuf, source_file_qn, imp, &psr4_target)) {
        .resolved => return psr4_target,
        .unresolved => return null,
        .not_applicable => {},
    }

    // Strategy 1: module-path resolution → existing node (Python/TS/Go).
    // No label filter here: directory-module languages (Go/Java packages)
    // legitimately resolve straight to a Folder node -- that's the intended,
    // correct import target, not a collision. The Folder-collision problem
    // (#767) only shows up downstream, in Strategy 4's retry-with-truncated-
    // path loop, which re-enters resolve_module with a DIFFERENT, shortened
    // string that the original import never named.
    const target_qn = cbm_pipeline_resolve_module(ctx, source_rel, imp.module_path);
    const target_opt: ?*const Node =
        if (target_qn != null) c.cbm_gbuf_find_by_qn(ctx.gbuf, target_qn) else null;
    c.free(target_qn);
    if (target_opt) |target| {
        // Python/TS from-import of a member: module_path is often
        // "pkg.mod.symbol" while resolve_module lands on the Module node
        // "pkg.mod". Prefer the member Function/Class when it exists —
        // required for `from M import f as g` so CALLS can import_map to f
        // (local_name g ≠ f).
        if (target.label != null and (streq(target.label, "Module") or streq(target.label, "File"))) {
            var symbuf: [256]u8 = undefined;
            const sym = importCandidateSymbol(imp.module_path, &symbuf, symbuf.len);
            if (sym != null and sym[0] != 0 and !streq(sym, "*")) {
                const mod_tail = importLastSegment(
                    if (target.qualified_name != null) target.qualified_name else "",
                );
                if (mod_tail == null or !streq(mod_tail, sym)) {
                    var member_qn: [c.CBM_SZ_512]u8 = undefined;
                    _ = c.snprintf(&member_qn, member_qn.len, "%s.%s", target.qualified_name, sym);
                    const member: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, &member_qn);
                    if (member) |m| {
                        if (importTargetableLabel(m.label) and !streq(m.label, "Module") and
                            !streq(m.label, "File"))
                        {
                            return m;
                        }
                    }
                }
            }
        }
        return target;
    }

    // Name-guess fallbacks below (Strategy 1b sibling-file, Strategy 3
    // symbol-name) are gated per importing-file language — see
    // cbm_import_symbol_fallback_allowed (#1934).
    var src_base: CStr = if (source_rel != null) source_rel else "";
    {
        var pb = src_base;
        while (pb[0] != 0) : (pb += 1) {
            if (pb[0] == '/' or pb[0] == '\\') src_base = pb + 1;
        }
    }
    const symbol_fallback_allowed =
        cbm_import_symbol_fallback_allowed(c.cbm_language_for_filename(src_base));

    // Strategy 1b: sibling-file resolution for build/markup grammars whose
    // import string is a sibling filename or directory (SCSS partials, Just/
    // BitBake/func includes, Meson subdir, Pony use). Its label filter admits
    // symbols too, so for Go it re-creates the Strategy-3 bug one directory
    // closer (`os/exec` → a same-package exec() method) — gated the same.
    if (symbol_fallback_allowed) {
        if (resolveSiblingFile(ctx, source_rel, source_file_qn, imp.module_path)) |sib| return sib;
    }

    // Strategy 2: namespace map.  `using App.Utils`, `import com.example.Foo`,
    // `use App\Utils\Helper` name a NAMESPACE (or a member of it) that the
    // path-based QN cannot express.  Try the full module path and progressively
    // shorter prefixes (dropping the trailing member segment) so both a bare
    // namespace import and a member import resolve to the declaring file.
    if (namespace_map != null) {
        // Normalize separators to '.' for namespace keys (PHP uses '\\').
        // Strip decorations first so `com.example.Util as U`, `crate::ops::*`
        // and `App\Utils\{A, B}` reduce to a clean dotted path.
        var norm: [1024]u8 = undefined;
        copyUndecorated(&norm, imp.module_path);
        dotSeparators(&norm);
        // Strip trailing glob/separator noise.
        var nl = c.strlen(&norm);
        while (nl > 0 and (norm[nl - 1] == '*' or norm[nl - 1] == '.' or norm[nl - 1] == ' ')) {
            nl -= 1;
            norm[nl] = 0;
        }
        // Collapse any "::"-induced empty segments ("a..b" → "a.b").
        while (true) {
            const dd: MutStr = c.strstr(&norm, "..");
            if (dd == null) break;
            _ = c.memmove(dd, dd + 1, c.strlen(dd + 1) + 1);
        }
        while (true) {
            // The map value is a '\n'-delimited list of __file__ QNs declaring
            // this namespace. Return the FIRST that is not the importing file,
            // so a same-package wildcard import (`import com.example.*` from a
            // file that is itself in com.example) resolves to a sibling — and
            // deterministically, independent of file-iteration order across
            // platforms (amd64 vs arm64).
            var seg: CStr = @ptrCast(c.cbm_ht_get(namespace_map, &norm));
            while (seg != null and seg[0] != 0) {
                const eol: CStr = c.strchr(seg, '\n');
                const len = if (eol != null) ptrDiff(eol, seg) else c.strlen(seg);
                var qbuf: [1024]u8 = undefined;
                if (len > 0 and len < qbuf.len) {
                    @memcpy(qbuf[0..len], seg[0..len]);
                    qbuf[len] = 0;
                    const n: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, &qbuf);
                    if (n) |node| {
                        if (source_file_qn == null or !streq(node.qualified_name, source_file_qn)) {
                            return node;
                        }
                    }
                }
                seg = if (eol != null) eol + 1 else null;
            }
            const dot: MutStr = c.strrchr(&norm, '.');
            if (dot == null) break;
            dot[0] = 0;
        }
    }

    // Strategy 3: symbol-name fallback.  Derive a representative imported
    // symbol (handling alias / glob / grouped forms) and match it against an
    // in-graph definition of the same simple name in another file
    // (Rust `helper`, Java `Util`, Kotlin grouped, ...).
    // Gated per importing-file language, like Strategy 1b above (#1934).
    var symbuf: [256]u8 = undefined;
    // Prefer the clean candidate from the module path; the local_name may be an
    // alias (Rust `as h`, Kotlin `as U`) that names no real symbol.
    var seg = importCandidateSymbol(imp.module_path, &symbuf, symbuf.len);
    if (seg == null and imp.local_name != null and imp.local_name[0] != 0 and
        !streq(imp.local_name, "*"))
    {
        seg = imp.local_name;
    }
    // Build the candidate symbol list: the derived symbol plus, when the import
    // is a dotted member path like `com.example.Config.DEFAULT`, the enclosing
    // type segments (`Config`).  This resolves object/class-member imports to
    // the declaring type when the leaf member isn't an importable node.
    var cands: [8]CStr = undefined;
    var ncands: usize = 0;
    if (seg != null and seg[0] != 0 and !streq(seg, "*")) {
        cands[ncands] = seg;
        ncands += 1;
    }
    {
        // Strip decorations, normalize separators to '.', and collect the
        // trailing path segments (last first) as fallback candidates.
        var mp: [1024]u8 = undefined;
        copyUndecorated(&mp, imp.module_path);
        dotSeparators(&mp);
        // Walk segments from the end.
        var end = c.strlen(&mp);
        while (end > 0 and ncands < cands.len) {
            var dot: ?usize = null;
            var p = end;
            while (p > 0) {
                p -= 1;
                if (mp[p] == '.') {
                    dot = p;
                    break;
                }
            }
            const s: CStr = if (dot) |d| @as(CStr, &mp) + d + 1 else &mp;
            if (s[0] != 0 and !streq(s, "*")) {
                var dup = false;
                for (cands[0..ncands]) |cand| {
                    if (streq(cand, s)) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    cands[ncands] = s;
                    ncands += 1;
                }
            }
            const d = dot orelse break;
            mp[d] = 0;
            end = d;
        }
        if (symbol_fallback_allowed) for (cands[0..ncands]) |name| {
            var hits: [*c][*c]const Node = null;
            var n: c_int = 0;
            if (c.cbm_gbuf_find_by_name(ctx.gbuf, name, &hits, &n) == 0 and hits != null) {
                // Deterministic winner: hits[] is in node-registration order,
                // which under parallel extraction varies run to run — taking
                // the FIRST targetable hit made the same import resolve to
                // different same-named nodes across runs (xfs: 360 flickering
                // IMPORTS diff lines between two MT runs). Pick the candidate
                // with the lexicographically smallest qualified name instead:
                // stable, content-derived, identical for ST and MT.
                var best: ?*const Node = null;
                for (0..ix(n)) |i| {
                    const cand: *const Node = hits[i] orelse continue;
                    if (!importTargetableLabel(cand.label)) continue;
                    if (source_file_qn != null and cand.qualified_name != null and
                        streq(cand.qualified_name, source_file_qn))
                    {
                        continue; // self
                    }
                    if (best == null or (cand.qualified_name != null and
                        best.?.qualified_name != null and
                        c.strcmp(cand.qualified_name, best.?.qualified_name) < 0))
                    {
                        best = cand;
                    }
                }
                if (best) |b| return b;
            }
        };
    }

    // Strategy 4: crate-relative module path → File/Module node.  Rust glob
    // `use crate::ops::*` names a module, not a symbol; strip the glob and the
    // `crate::`/`self::`/`super::` prefix, convert `::`→`/`, then resolve the
    // remaining path (and successive prefixes) to a Module/File node.
    {
        var mp: [1024]u8 = undefined;
        copyUndecorated(&mp, imp.module_path);
        // Convert "::" → "/" (drop the doubled colon cleanly).
        var clean: [1024]u8 = undefined;
        var ci: usize = 0;
        var pi: usize = 0;
        while (mp[pi] != 0 and ci + 1 < clean.len) : (pi += 1) {
            if (mp[pi] == ':') {
                if (ci > 0 and clean[ci - 1] == '/') continue; // collapse "::" → single "/"
                clean[ci] = '/';
                ci += 1;
            } else if (mp[pi] == '*' or mp[pi] == ' ') {
                continue;
            } else {
                clean[ci] = mp[pi];
                ci += 1;
            }
        }
        clean[ci] = 0;
        // Drop trailing slashes.
        while (ci > 0 and clean[ci - 1] == '/') {
            ci -= 1;
            clean[ci] = 0;
        }
        // Strip a leading crate-relative prefix.
        var body: CStr = &clean;
        for ([_][]const u8{ "crate/", "self/", "super/" }) |pf| {
            if (c.strncmp(body, pf.ptr, pf.len) == 0) {
                body += pf.len;
                break;
            }
        }
        if (body[0] != 0) {
            var work: [1024]u8 = undefined;
            _ = c.snprintf(&work, work.len, "%s", body);
            while (true) {
                const rqn = cbm_pipeline_resolve_module(ctx, source_rel, &work);
                const n: ?*const Node = if (rqn != null) c.cbm_gbuf_find_by_qn(ctx.gbuf, rqn) else null;
                c.free(rqn);
                if (n) |node| {
                    if (importTargetableLabel(node.label) and notSelf(node, source_file_qn)) {
                        return node;
                    }
                }
                const sl: MutStr = c.strrchr(&work, '/');
                if (sl == null) break;
                sl[0] = 0;
            }
        }
    }

    return null;
}

// ── Namespace map ───────────────────────────────────────────────────

pub export fn cbm_pipeline_namespace_map_build(
    project_name: CStr,
    results: [*c][*c]c.CBMFileResult,
    spill: ?*c.cbm_result_spill_t,
    rels: [*c]const CStr,
    count: c_int,
) ?*HashTable {
    var map: ?*HashTable = null;
    for (0..ix(count)) |i| {
        if (rels[i] == null) continue;
        const r: ?*c.CBMFileResult = c.cbm_result_cache_acquire(results, spill, @intCast(i));
        const key: MutStr = if (r != null and r.?.namespace_name != null and r.?.namespace_name[0] != 0)
            c.strdup(r.?.namespace_name)
        else
            null;
        c.cbm_result_cache_release(results, @intCast(i), r);
        if (key == null) continue;
        if (map == null) {
            map = c.cbm_ht_create(c.CBM_SZ_64);
            if (map == null) {
                c.free(key);
                return null;
            }
        }
        const file_qn: MutStr = c.cbm_pipeline_fqn_compute(project_name, rels[i], "__file__");
        if (file_qn == null) {
            c.free(key);
            continue;
        }
        // Normalize the namespace key to dot-separated form so it matches the
        // dot-normalized lookups in cbm_pipeline_resolve_import_node (PHP uses
        // '\\', some grammars '::' or '/').
        dotSeparators(key);
        // Store ALL files declaring a namespace as a '\n'-delimited list so the
        // resolver can pick a non-importer sibling (see resolve loop). The hash
        // table does not copy keys, so the strdup'd key is owned by the map and
        // freed in nsMapFreeEntry.
        if (!c.cbm_ht_has(map, key)) {
            _ = c.cbm_ht_set(map, key, file_qn); // map owns key + file_qn
        } else {
            // Append to the existing list. Re-key with the STORED key pointer
            // (not our fresh strdup) so the map's key pointer never changes —
            // otherwise Verstable would adopt the new key and our free(key)
            // below would free the live key (use-after-free).
            const stored_key: CStr = c.cbm_ht_get_key(map, key);
            const cur: CStr = @ptrCast(c.cbm_ht_get(map, key));
            if (stored_key != null and cur != null) {
                const need = c.strlen(cur) + 1 + c.strlen(file_qn) + 1;
                const combined: MutStr = @ptrCast(c.malloc(need));
                if (combined != null) {
                    _ = c.snprintf(combined, need, "%s\n%s", cur, file_qn);
                    const prev = c.cbm_ht_set(map, stored_key, combined);
                    c.free(prev); // old value string
                }
            }
            c.free(key); // our fresh strdup — never stored
            c.free(file_qn); // content copied into combined
        }
    }
    return map;
}

fn nsMapFreeEntry(key: CStr, value: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    c.free(@constCast(key)); // strdup'd in cbm_pipeline_namespace_map_build
    c.free(value);
}

pub export fn cbm_pipeline_namespace_map_free(map: ?*HashTable) void {
    if (map == null) return;
    c.cbm_ht_foreach(map, nsMapFreeEntry, null);
    c.cbm_ht_free(map);
}
