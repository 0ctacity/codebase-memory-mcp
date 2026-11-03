//! userconfig.zig — User-defined extension→language mappings.
//!
//! Implements src/discover/userconfig.h.
//!
//! Reads extra_extensions from:
//!   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
//!            (falls back to ~/.config/codebase-memory-mcp/config.json)
//!   Project: {repo_root}/.codebase-memory.json
//!
//! Project config wins over global. Unknown language values warn and are
//! skipped (fail-open). Missing files are silently ignored.
//!
//! The config and its strings are malloc'd: C code reads `entries`, and
//! cbm_userconfig_free releases them.

const std = @import("std");
const c = @import("c");

const Str = [*:0]const u8;
const Lang = c.CBMLanguage;

const MAX_CONFIG_SIZE = 65536;
const LANG_UNKNOWN: Lang = c.CBM_LANG_COUNT;

// ── Process-global user config pointer ─────────────────────────────

var g_userconfig: ?*const c.cbm_userconfig_t = null;

pub export fn cbm_set_user_lang_config(cfg: ?*const c.cbm_userconfig_t) void {
    g_userconfig = cfg;
}

pub export fn cbm_get_user_lang_config() ?*const c.cbm_userconfig_t {
    return g_userconfig;
}

// ── Language name → enum table ─────────────────────────────────────

/// Reverse mapping from lowercase language names to CBMLanguage. Covers all
/// names exposed by cbm_language_name() plus common aliases.
const LangName = struct { name: []const u8, lang: Lang };

const LANG_NAME_TABLE = [_]LangName{
    .{ .name = "go", .lang = c.CBM_LANG_GO },
    .{ .name = "python", .lang = c.CBM_LANG_PYTHON },
    .{ .name = "javascript", .lang = c.CBM_LANG_JAVASCRIPT },
    .{ .name = "typescript", .lang = c.CBM_LANG_TYPESCRIPT },
    .{ .name = "tsx", .lang = c.CBM_LANG_TSX },
    .{ .name = "rust", .lang = c.CBM_LANG_RUST },
    .{ .name = "java", .lang = c.CBM_LANG_JAVA },
    .{ .name = "c++", .lang = c.CBM_LANG_CPP },
    .{ .name = "cpp", .lang = c.CBM_LANG_CPP },
    .{ .name = "c#", .lang = c.CBM_LANG_CSHARP },
    .{ .name = "csharp", .lang = c.CBM_LANG_CSHARP },
    .{ .name = "php", .lang = c.CBM_LANG_PHP },
    .{ .name = "lua", .lang = c.CBM_LANG_LUA },
    .{ .name = "scala", .lang = c.CBM_LANG_SCALA },
    .{ .name = "kotlin", .lang = c.CBM_LANG_KOTLIN },
    .{ .name = "ruby", .lang = c.CBM_LANG_RUBY },
    .{ .name = "c", .lang = c.CBM_LANG_C },
    .{ .name = "bash", .lang = c.CBM_LANG_BASH },
    .{ .name = "sh", .lang = c.CBM_LANG_BASH },
    .{ .name = "zig", .lang = c.CBM_LANG_ZIG },
    .{ .name = "elixir", .lang = c.CBM_LANG_ELIXIR },
    .{ .name = "haskell", .lang = c.CBM_LANG_HASKELL },
    .{ .name = "ocaml", .lang = c.CBM_LANG_OCAML },
    .{ .name = "objective-c", .lang = c.CBM_LANG_OBJC },
    .{ .name = "objc", .lang = c.CBM_LANG_OBJC },
    .{ .name = "swift", .lang = c.CBM_LANG_SWIFT },
    .{ .name = "dart", .lang = c.CBM_LANG_DART },
    .{ .name = "perl", .lang = c.CBM_LANG_PERL },
    .{ .name = "groovy", .lang = c.CBM_LANG_GROOVY },
    .{ .name = "erlang", .lang = c.CBM_LANG_ERLANG },
    .{ .name = "r", .lang = c.CBM_LANG_R },
    .{ .name = "html", .lang = c.CBM_LANG_HTML },
    .{ .name = "css", .lang = c.CBM_LANG_CSS },
    .{ .name = "scss", .lang = c.CBM_LANG_SCSS },
    .{ .name = "yaml", .lang = c.CBM_LANG_YAML },
    .{ .name = "toml", .lang = c.CBM_LANG_TOML },
    .{ .name = "hcl", .lang = c.CBM_LANG_HCL },
    .{ .name = "terraform", .lang = c.CBM_LANG_HCL },
    .{ .name = "sql", .lang = c.CBM_LANG_SQL },
    .{ .name = "dockerfile", .lang = c.CBM_LANG_DOCKERFILE },
    .{ .name = "clojure", .lang = c.CBM_LANG_CLOJURE },
    .{ .name = "f#", .lang = c.CBM_LANG_FSHARP },
    .{ .name = "fsharp", .lang = c.CBM_LANG_FSHARP },
    .{ .name = "julia", .lang = c.CBM_LANG_JULIA },
    .{ .name = "vimscript", .lang = c.CBM_LANG_VIMSCRIPT },
    .{ .name = "nix", .lang = c.CBM_LANG_NIX },
    .{ .name = "common lisp", .lang = c.CBM_LANG_COMMONLISP },
    .{ .name = "commonlisp", .lang = c.CBM_LANG_COMMONLISP },
    .{ .name = "lisp", .lang = c.CBM_LANG_COMMONLISP },
    .{ .name = "elm", .lang = c.CBM_LANG_ELM },
    .{ .name = "fortran", .lang = c.CBM_LANG_FORTRAN },
    .{ .name = "cuda", .lang = c.CBM_LANG_CUDA },
    .{ .name = "cobol", .lang = c.CBM_LANG_COBOL },
    .{ .name = "verilog", .lang = c.CBM_LANG_VERILOG },
    .{ .name = "emacs lisp", .lang = c.CBM_LANG_EMACSLISP },
    .{ .name = "emacslisp", .lang = c.CBM_LANG_EMACSLISP },
    .{ .name = "json", .lang = c.CBM_LANG_JSON },
    .{ .name = "xml", .lang = c.CBM_LANG_XML },
    .{ .name = "markdown", .lang = c.CBM_LANG_MARKDOWN },
    .{ .name = "makefile", .lang = c.CBM_LANG_MAKEFILE },
    .{ .name = "cmake", .lang = c.CBM_LANG_CMAKE },
    .{ .name = "protobuf", .lang = c.CBM_LANG_PROTOBUF },
    .{ .name = "graphql", .lang = c.CBM_LANG_GRAPHQL },
    .{ .name = "vue", .lang = c.CBM_LANG_VUE },
    .{ .name = "svelte", .lang = c.CBM_LANG_SVELTE },
    .{ .name = "meson", .lang = c.CBM_LANG_MESON },
    .{ .name = "glsl", .lang = c.CBM_LANG_GLSL },
    .{ .name = "ini", .lang = c.CBM_LANG_INI },
    .{ .name = "matlab", .lang = c.CBM_LANG_MATLAB },
    .{ .name = "lean", .lang = c.CBM_LANG_LEAN },
    .{ .name = "form", .lang = c.CBM_LANG_FORM },
    .{ .name = "magma", .lang = c.CBM_LANG_MAGMA },
    .{ .name = "wolfram", .lang = c.CBM_LANG_WOLFRAM },
};

/// Parse a language string (case-insensitive) to a CBMLanguage. Returns
/// CBM_LANG_COUNT if the string is not recognized.
fn langFromString(s: Str) Lang {
    if (s[0] == 0) return LANG_UNKNOWN;

    // Lowercase copy for comparison, truncated like the C 64-byte buffer
    var lower: [64]u8 = undefined;
    var i: usize = 0;
    while (i < lower.len - 1 and s[i] != 0) : (i += 1) lower[i] = std.ascii.toLower(s[i]);

    for (LANG_NAME_TABLE) |entry| {
        if (std.mem.eql(u8, entry.name, lower[0..i])) return entry.lang;
    }
    return LANG_UNKNOWN;
}

// ── Logging ────────────────────────────────────────────────────────

/// cbm_log_warn: cbm_log with the NULL key terminator the C macro appends.
fn logWarn(msg: Str, kvs: anytype) void {
    @call(.auto, c.cbm_log, .{ c.CBM_LOG_WARN, msg } ++ kvs ++ .{@as(?Str, null)});
}

// ── JSON parsing ───────────────────────────────────────────────────

const EntryList = struct {
    entries: ?[*]c.cbm_userext_t = null,
    count: usize = 0,

    fn freeAll(list: *EntryList) void {
        if (list.entries) |es| {
            for (es[0..list.count]) |e| c.free(e.ext);
        }
        c.free(@ptrCast(list.entries));
    }
};

/// Parse extra_extensions from a yyjson object root, appending valid entries
/// to `list`. Project entries are appended after global ones so the dedup
/// pass can prefer them. Returns false on allocation failure.
fn parseExtraExtensions(root: ?*c.yyjson_val, list: *EntryList, source_label: Str) bool {
    if (!c.yyjson_is_obj(root)) {
        logWarn("userconfig.bad_root", .{ "file", source_label });
        return true;
    }

    const extra = c.yyjson_obj_get(root, "extra_extensions");
    if (extra == null) return true; // key absent — fine
    if (!c.yyjson_is_obj(extra)) {
        logWarn("userconfig.bad_extra_extensions", .{ "file", source_label });
        return true;
    }

    var iter: c.yyjson_obj_iter = undefined;
    _ = c.yyjson_obj_iter_init(extra, &iter);
    while (true) {
        const key = c.yyjson_obj_iter_next(&iter);
        if (key == null) break;
        const val = c.yyjson_obj_iter_get_val(key);

        const ext_c = c.yyjson_get_str(key);
        const lang_c = c.yyjson_get_str(val);
        if (ext_c == null or lang_c == null) {
            logWarn("userconfig.skip_non_string", .{ "file", source_label });
            continue;
        }
        const ext_str: Str = @ptrCast(ext_c);
        const lang_str: Str = @ptrCast(lang_c);

        // Extension must start with '.'
        if (ext_str[0] != '.') {
            logWarn("userconfig.skip_bad_ext", .{ "file", source_label, "ext", ext_str });
            continue;
        }

        const lang = langFromString(lang_str);
        if (lang == LANG_UNKNOWN) {
            logWarn("userconfig.unknown_lang", .{ "file", source_label, "lang", lang_str });
            continue; // fail-open: skip unknown languages
        }

        // Grow the array
        const grown = c.realloc(@ptrCast(list.entries), (list.count + 1) * @sizeOf(c.cbm_userext_t)) orelse
            return false;
        list.entries = @ptrCast(@alignCast(grown));

        const ext_copy = c.strdup(ext_str) orelse return false;
        list.entries.?[list.count] = .{ .ext = ext_copy, .lang = lang };
        list.count += 1;
    }
    return true;
}

/// Read a JSON file and parse extra_extensions from it. Silently ignores
/// missing files and logs warnings for corrupt JSON. Returns false on
/// allocation failure.
fn loadConfigFile(path: Str, list: *EntryList) bool {
    const f = c.cbm_fopen(path, "rb") orelse return true; // file absent — silently ignore

    if (c.fseek(f, 0, c.SEEK_END) != 0) {
        _ = c.fclose(f);
        return true;
    }
    const len = c.ftell(f);
    if (c.fseek(f, 0, c.SEEK_SET) != 0) {
        _ = c.fclose(f);
        return true;
    }

    if (len <= 0 or len > MAX_CONFIG_SIZE) {
        _ = c.fclose(f);
        if (len > MAX_CONFIG_SIZE) logWarn("userconfig.file_too_large", .{ "path", path });
        return true;
    }

    const size: usize = @intCast(len);
    const buf: [*]u8 = @ptrCast(c.malloc(size + 1) orelse {
        _ = c.fclose(f);
        return false;
    });

    const nread = @min(c.fread(buf, 1, size, f), size);
    _ = c.fclose(f);
    buf[nread] = 0;

    const doc = c.yyjson_read(buf, nread, 0);
    c.free(buf);

    if (doc == null) {
        logWarn("userconfig.corrupt_json", .{ "path", path });
        return true; // corrupt JSON — silently ignore (fail-open)
    }

    const root = c.yyjson_doc_get_root(doc);
    const ok = parseExtraExtensions(root, list, path);
    c.yyjson_doc_free(doc);
    return ok;
}

// ── Public API ─────────────────────────────────────────────────────

pub export fn cbm_userconfig_load(repo_path: ?Str) ?*c.cbm_userconfig_t {
    const cfg: *c.cbm_userconfig_t = @ptrCast(@alignCast(c.calloc(1, @sizeOf(c.cbm_userconfig_t)) orelse return null));

    var list: EntryList = .{};

    // ── Step 1: Load global config ──
    var global_path: [1280]u8 = undefined;
    const cfg_base: Str = if (c.cbm_app_config_dir()) |d| @ptrCast(d) else "/tmp";
    formatPath(&global_path, "{s}/codebase-memory-mcp/config.json", cfg_base);

    if (!loadConfigFile(@ptrCast(&global_path), &list)) {
        list.freeAll();
        c.free(cfg);
        return null;
    }

    var global_count = list.count; // entries[0..global_count) are from global

    // ── Step 2: Load project config ──
    if (repo_path) |repo| {
        if (repo[0] != 0) {
            var project_path: [1280]u8 = undefined;
            formatPath(&project_path, "{s}/.codebase-memory.json", repo);

            if (!loadConfigFile(@ptrCast(&project_path), &list)) {
                list.freeAll();
                c.free(cfg);
                return null;
            }
        }
    }

    const entries = list.entries orelse return cfg; // no entries: empty config

    // ── Step 3: Dedup — project entries win over global ──
    //
    // For any extension that appears in both global (0..global_count) and
    // project (global_count..count), remove the global entry by replacing it
    // with the last global entry (order-insensitive dedup).
    var p = global_count;
    while (p < list.count) : (p += 1) {
        var g: usize = 0;
        while (g < global_count) : (g += 1) {
            if (entries[g].ext != null and c.strcmp(entries[g].ext, entries[p].ext) == 0) {
                // Remove global entry: overwrite with last global entry
                c.free(entries[g].ext);
                entries[g] = entries[global_count - 1];
                entries[global_count - 1].ext = null; // mark as consumed
                global_count -= 1;
                break;
            }
        }
    }

    // Compact: remove the NULL-ext slots (consumed "last global" entries)
    // left by the dedup step.
    var write_idx: usize = 0;
    for (0..list.count) |i| {
        if (entries[i].ext != null) {
            entries[write_idx] = entries[i];
            write_idx += 1;
        }
    }

    cfg.entries = entries;
    cfg.count = @intCast(write_idx);
    return cfg;
}

/// snprintf(buf, sizeof(buf), fmt, s): truncating, always NUL-terminated.
fn formatPath(buf: []u8, comptime fmt: []const u8, s: Str) void {
    var w: std.Io.Writer = .fixed(buf[0 .. buf.len - 1]);
    w.print(fmt, .{std.mem.span(s)}) catch {};
    buf[w.end] = 0;
}

pub export fn cbm_userconfig_lookup(cfg_c: ?*const c.cbm_userconfig_t, ext_c: ?Str) Lang {
    const cfg = cfg_c orelse return LANG_UNKNOWN;
    const ext = ext_c orelse return LANG_UNKNOWN;
    if (ext[0] == 0) return LANG_UNKNOWN;
    return lookup(cfg, ext);
}

/// cbm_userconfig_lookup for a non-empty extension.
pub fn lookup(cfg: *const c.cbm_userconfig_t, ext: Str) Lang {
    if (cfg.count <= 0) return LANG_UNKNOWN;
    for (cfg.entries[0..@intCast(cfg.count)]) |e| {
        if (e.ext != null and c.strcmp(e.ext, ext) == 0) return e.lang;
    }
    return LANG_UNKNOWN;
}

pub export fn cbm_userconfig_free(cfg_c: ?*c.cbm_userconfig_t) void {
    const cfg = cfg_c orelse return;
    if (cfg.count > 0) {
        for (cfg.entries[0..@intCast(cfg.count)]) |e| c.free(e.ext);
    }
    c.free(cfg.entries);
    c.free(cfg);
}
