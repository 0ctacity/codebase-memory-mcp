//! pass_lsp_cross.zig — Cross-file LSP type-aware call resolution pass.
//!
//! See pass_lsp_cross.h for the high-level contract. This file is the
//! pipeline glue that converts the existing per-file extraction state
//! (CBMDefinition / CBMImport / IMPORTS-edge gbuf state) into the input
//! shape each language LSP's cbm_run_X_lsp_cross expects, then merges
//! the resulting CBMResolvedCall entries back into per-file results.
//!
//! The pass is a no-op for any file whose CBMFileResult is missing or
//! whose language has no cross-file LSP entry registered (e.g. Rust /
//! Java today). Per-LSP emit functions dedup against entries already in
//! resolved_calls, so this pass is also idempotent — safe to invoke
//! multiple times if the pipeline gains a re-run path later.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

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
const FileResult = c.CBMFileResult;
const Definition = c.CBMDefinition;
const LspDef = c.CBMLSPDef;
const Registry = c.cbm_registry_t;
const Language = c.CBMLanguage;
/// A NULL-terminated or counted array of borrowed C strings.
const StrList = [*c]CStr;

// ── Constants ───────────────────────────────────────────────────────

/// source size cap, in MiB
const PXC_MAX_FILE_BYTES_FACTOR = 100;

// ── Local helpers ───────────────────────────────────────────────────

/// strncmp(s, prefix, strlen(prefix)) == 0.
fn hasPrefix(s: CStr, comptime prefix: [:0]const u8) bool {
    return c.strncmp(s, prefix.ptr, prefix.len) == 0;
}

/// True for languages whose module QN is derived from the CONTAINING DIRECTORY
/// (Java package, Go package) rather than the filename stem. MUST match the
/// extraction-side cbm_lang_module_is_dir() in internal/cbm/helpers.c so the
/// cross-file LSP caller_qn agrees with the def-node QN (the lsp_resolve join
/// keys on exact equality).
fn pxcModuleIsDir(lang: Language) bool {
    return lang == c.CBM_LANG_JAVA or lang == c.CBM_LANG_GO;
}

/// Slurp a file into a malloc'd, NUL-terminated buffer. Mirrors the
/// read_file helper in pass_parallel (kept local so the
/// pipeline doesn't grow a public read-file API just for this pass).
fn pxcReadFile(path: CStr, out_len: *c_int) MutStr {
    const f = c.cbm_fopen(path, "rb") orelse return null;
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    if (size <= 0 or
        size > @as(c_long, PXC_MAX_FILE_BYTES_FACTOR) * @as(c_long, c.CBM_SZ_1K) * @as(c_long, c.CBM_SZ_1K))
    {
        _ = c.fclose(f);
        return null;
    }
    // +pad: tree-sitter lexer lookahead reads past EOF; keep it in-bounds
    const CBM_TS_LOOKAHEAD_PAD = 16;
    const usize_size: usize = @intCast(size);
    const buf: MutStr = @ptrCast(c.malloc(usize_size + CBM_TS_LOOKAHEAD_PAD));
    if (buf == null) {
        _ = c.fclose(f);
        return null;
    }
    var nread = c.fread(buf, 1, usize_size, f);
    _ = c.fclose(f);
    if (nread > usize_size) nread = usize_size;
    @memset(buf[nread .. nread + CBM_TS_LOOKAHEAD_PAD], 0);
    out_len.* = @intCast(nread);
    return buf;
}

/// Map a CBMDefinition.label to a CBMLSPDef.label. Per-language LSP registrars
/// only care about type-like containers (Class/Struct/Interface/Trait/Enum/Type)
/// plus Protocol/Function/Method — variables, modules, decorators, etc. are
/// skipped. Struct passes through so Rust/Go struct type-registration via the
/// cross-file LSP path is not dropped.
fn pxcMapLabel(label: CStr) CStr {
    if (label == null) return null;
    if (c.cbm_label_is_type_like(label) or streq(label, "Protocol") or
        streq(label, "Function") or streq(label, "Method"))
    {
        return label;
    }
    return null;
}

/// Build the embedded_types "|"-separated string from base_classes[].
/// Returns NULL when there are no bases. Allocated in the supplied arena.
fn pxcJoinPipe(arena: *Arena, items: [*c]const CStr) CStr {
    if (items == null or items[0] == null) return null;
    var count: usize = 0;
    var total: usize = 0;
    while (items[count] != null) : (count += 1) {
        total += c.strlen(items[count]);
    }
    if (count == 0) return null;
    // count - 1 separators + NUL.
    const bufsz = total + (count - 1) + 1;
    const buf: MutStr = @ptrCast(c.cbm_arena_alloc(arena, bufsz));
    if (buf == null) return null;
    var p: usize = 0;
    for (0..count) |i| {
        const n = c.strlen(items[i]);
        @memcpy(buf[p .. p + n], items[i][0..n]);
        p += n;
        if (i + 1 < count) {
            buf[p] = '|';
            p += 1;
        }
    }
    buf[p] = 0;
    return buf;
}

// ── Cross-file base-class QN resolution ─────────────────────────────
//
// CBMDefinition.base_classes carries the SOURCE SPELLING of each base
// ("Base", "django.db.Model", "React.Component"): extraction strips
// keywords and generic arguments, but it cannot know WHERE the name is
// declared. The Python and TS cross-file registrars, however, consume
// CBMLSPDef.embedded_types as fully-qualified names — py_lookup_attribute
// and ts_lookup_member feed each entry straight into
// cbm_registry_lookup_type. An unqualified spelling therefore matched
// nothing declared in ANOTHER file: `class Child(Base)` in child.py never
// saw Base in base.py, so a call to an inherited method through a typed
// receiver had no member to find and fell through to the weak textual
// cascade (where the #592/#606 guard correctly kills it).
//
// Resolve each base name ONCE per definition here, from exactly the
// inputs pass_semantic uses to draw its INHERITS edge: the project
// registry, the declaring module, and the file's import map. Two
// properties follow. The LSP's inheritance view is the same relation the
// graph records, so the two cannot diverge. And the binding is
// import- or same-module-backed, not a short-name guess, so the CALLS
// edge it enables is a supported fact that the weak-member guard keeps.
//
// Cost: O(defs) hash lookups + ONE import map per file. No per-call-site
// hierarchy walk, no registry scan, no per-file registry rebuild.
fn pxcLangResolvesBaseQns(lang: Language) bool {
    return switch (lang) {
        c.CBM_LANG_PYTHON, c.CBM_LANG_JAVASCRIPT, c.CBM_LANG_TYPESCRIPT, c.CBM_LANG_TSX => true,
        // Go / JVM / C# / C++ / Rust registrars qualify their own embedded
        // types already (struct embedding, parent_class chains, impl-trait
        // provenance). Re-resolving here would fight those paths, so they
        // keep the raw join.
        else => false,
    };
}

/// True for the registry strategies that are pure short-name guesses.
/// EXPLICIT drop-list, mirroring cbm_tsjs_suppress_weak_method_match: a
/// base class bound by "some project type happens to share this name" is
/// exactly the fabricated relation #606 removed, and inheritance
/// multiplies it — every inherited member of the wrong base would become
/// a callable target. Import-, module- and suffix-aware strategies are
/// kept; anything unresolved simply retains its source spelling and the
/// behaviour that predates this resolution.
fn pxcBaseStrategyIsWeak(strategy: CStr) bool {
    if (strategy == null or strategy[0] == 0) return true;
    return streq(strategy, "suffix_match") or streq(strategy, "unique_name") or
        streq(strategy, "field_type_hint") or streq(strategy, "fuzzy");
}

/// Resolve one base-class spelling to a project QN. Mirrors
/// pass_semantic's resolve_as_class — same registry, same type-like veto —
/// then additionally rejects weak short-name strategies (see above).
/// Returns NULL when the base is not a confidently-known project type;
/// stdlib and third-party bases land here and keep their raw spelling.
fn pxcResolveBaseQn(reg: ?*const Registry, raw: CStr, module_qn: CStr, imp_keys: StrList, imp_vals: StrList, imp_count: c_int) CStr {
    if (reg == null or raw == null or raw[0] == 0) return null;
    const res = c.cbm_registry_resolve(reg, raw, module_qn, imp_keys, imp_vals, imp_count);
    if (res.qualified_name == null or res.qualified_name[0] == 0) return null;
    if (pxcBaseStrategyIsWeak(res.strategy)) return null;
    if (!c.cbm_label_is_type_like(c.cbm_registry_label_of(reg, res.qualified_name))) return null;
    return res.qualified_name;
}

/// pxcJoinPipe over base_classes, substituting each resolved QN for its
/// source spelling. Unresolved entries pass through verbatim so a base the
/// registry does not know keeps working exactly as before.
fn pxcJoinBaseQns(
    arena: *Arena,
    bases: [*c]const CStr,
    reg: ?*const Registry,
    module_qn: CStr,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
) CStr {
    if (bases == null or bases[0] == null) return null;
    var count: usize = 0;
    while (bases[count] != null) count += 1;
    const resolved: StrList = @ptrCast(@alignCast(c.cbm_arena_alloc(arena, (count + 1) * @sizeOf(CStr))));
    if (resolved == null) return pxcJoinPipe(arena, bases);
    for (0..count) |i| {
        const qn = pxcResolveBaseQn(reg, bases[i], module_qn, imp_keys, imp_vals, imp_count);
        resolved[i] = if (qn != null) qn else bases[i];
    }
    resolved[count] = null;
    return pxcJoinPipe(arena, resolved);
}

fn pxcLastComponent(qn: CStr) CStr {
    if (qn == null) return null;
    const dot: CStr = c.strrchr(qn, '.');
    return if (dot != null) dot + 1 else qn;
}

fn pxcJvmTypeQn(arena: ?*Arena, namespace_name: CStr, type_qn_or_name: CStr) CStr {
    if (arena == null or namespace_name == null or namespace_name[0] == 0 or type_qn_or_name == null) {
        return type_qn_or_name;
    }
    const short_name = pxcLastComponent(type_qn_or_name);
    if (short_name == null or short_name[0] == 0) return type_qn_or_name;
    return c.cbm_arena_sprintf(arena, "%s.%s", namespace_name, short_name);
}

fn pxcJvmDefQn(arena: ?*Arena, src: ?*const Definition, namespace_name: CStr, label: CStr) CStr {
    const s = src orelse return null;
    if (arena == null or namespace_name == null or namespace_name[0] == 0) return s.qualified_name;
    if (streq(label, "Method") or streq(label, "Function") or streq(label, "Constructor")) {
        if (s.parent_class != null and s.parent_class[0] != 0) {
            return c.cbm_arena_sprintf(arena, "%s.%s.%s", namespace_name, pxcLastComponent(s.parent_class), s.name);
        }
        return c.cbm_arena_sprintf(arena, "%s.%s", namespace_name, s.name);
    }
    return c.cbm_arena_sprintf(arena, "%s.%s", namespace_name, s.name);
}

fn pxcInferJvmNamespace(arena: ?*Arena, rel_path: CStr, lang: Language) CStr {
    if (arena == null or rel_path == null or !pxcIsJvmLang(lang)) return null;
    var root: CStr = null;
    const lang_root: CStr = if (lang == c.CBM_LANG_KOTLIN) "kotlin/" else "java/";
    const lang_root_len = c.strlen(lang_root);
    if (hasPrefix(rel_path, "src/main/") and c.strncmp(rel_path + 9, lang_root, lang_root_len) == 0) {
        root = rel_path + 9 + lang_root_len;
    } else if (hasPrefix(rel_path, "src/test/") and
        c.strncmp(rel_path + 9, lang_root, lang_root_len) == 0)
    {
        root = rel_path + 9 + lang_root_len;
    } else {
        const needle: CStr = if (lang == c.CBM_LANG_KOTLIN) "/kotlin/" else "/java/";
        root = c.strstr(rel_path, needle);
        if (root != null) {
            root += c.strlen(needle);
        } else if (hasPrefix(rel_path, "src/")) {
            root = rel_path + 4;
        } else {
            root = c.strstr(rel_path, "/src/");
            if (root != null) root += "/src/".len;
        }
    }
    if (root == null or root[0] == 0) return null;
    if (hasPrefix(root, "main/") or hasPrefix(root, "test/")) root += 5;
    if (hasPrefix(root, "java/")) {
        root += 5;
    } else if (hasPrefix(root, "kotlin/")) {
        root += 7;
    }
    const slash: CStr = c.strrchr(root, '/');
    if (slash == null or @intFromPtr(slash) <= @intFromPtr(root)) return null;
    const len = ptrDiff(slash, root);
    const ns: MutStr = @ptrCast(c.cbm_arena_alloc(arena, len + 1));
    if (ns == null) return null;
    @memcpy(ns[0..len], root[0..len]);
    ns[len] = 0;
    for (0..len) |i| {
        if (ns[i] == '/') ns[i] = '.';
    }
    return ns;
}

fn pxcImportLookup(local: CStr, len: usize, imp_keys: StrList, imp_vals: StrList, imp_count: c_int) CStr {
    for (0..ix(imp_count)) |i| {
        if (imp_keys[i] != null and imp_vals[i] != null and c.strlen(imp_keys[i]) == len and
            c.strncmp(imp_keys[i], local, len) == 0)
        {
            return imp_vals[i];
        }
    }
    return null;
}

/// The innermost container enclosing a container-level Zig alias (a QN prefix
/// of its parent_class, above the module) that declares `head` in this file,
/// as `<container>.<head>`. NULL when none does.
fn pxcZigContainerHead(arena: *Arena, file: *const FileResult, scope: CStr, module_qn: CStr, head: CStr, len: usize) CStr {
    const mlen = c.strlen(module_qn);
    var slen = c.strlen(scope);
    while (slen > mlen) {
        for (0..ix(file.defs.count)) |i| {
            const qn: CStr = file.defs.items[i].qualified_name;
            if (qn != null and c.strlen(qn) == slen + 1 + len and c.strncmp(qn, scope, slen) == 0 and
                qn[slen] == '.' and c.strncmp(qn + slen + 1, head, len) == 0)
            {
                return c.cbm_arena_strdup(arena, qn);
            }
        }
        while (slen > mlen and scope[slen - 1] != '.') slen -= 1;
        slen = if (slen > 0) slen - 1 else 0;
    }
    return null;
}

/// Zig: an alias (`const db = @import(..);` of "db.zig", `pub const Database =
/// zova.Database;`, alias_value = the value text), at module or container
/// level, becomes a "Variable" def whose return_types is the QN it names. The
/// head resolves lexically: a declaration in an enclosing container, then the
/// declaring file's imports (`const v = @import(..)` is imported as `v`,
/// the `.C` of an import of "x.zig" as `x`); any other head is a path from the declaring
/// module. Returns -1 for anything else.
fn pxcBuildZigAlias(
    arena: *Arena,
    file: *const FileResult,
    src: *const Definition,
    module_qn: CStr,
    dst: *LspDef,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
) c_int {
    const text: CStr = src.alias_value;
    if (text == null or text[0] == 0 or module_qn == null or c.strpbrk(text, " \t\r\n") != null) {
        return -1;
    }
    var target: CStr = null;
    const import_open = "@import(\"";
    if (hasPrefix(text, import_open)) {
        const path: CStr = text + import_open.len;
        const close: CStr = c.strchr(path, '"');
        if (close == null or close[1] != ')') return -1;
        const rest: CStr = close + 2;
        if (ptrDiff(close, path) == "std".len and c.strncmp(path, "std", 3) == 0) {
            target = c.cbm_arena_sprintf(arena, "std%s", rest); // the opaque std namespace
        } else {
            var local: CStr = src.name;
            var len = c.strlen(local);
            if (rest[0] != 0) {
                local = path;
                var p = path;
                while (@intFromPtr(p) < @intFromPtr(close)) : (p += 1) {
                    if (p[0] == '/') local = p + 1;
                }
                len = ptrDiff(close, local);
                if (len > 4 and c.strncmp(local + len - 4, ".zig", 4) == 0) len -= 4;
            }
            const head = pxcImportLookup(local, len, imp_keys, imp_vals, imp_count);
            target = if (head != null) c.cbm_arena_sprintf(arena, "%s%s", head, rest) else null;
        }
    } else {
        const len = c.strcspn(text, ".");
        var head: CStr = if (src.parent_class != null)
            pxcZigContainerHead(arena, file, src.parent_class, module_qn, text, len)
        else
            null;
        if (head == null) head = pxcImportLookup(text, len, imp_keys, imp_vals, imp_count);
        target = if (head != null)
            c.cbm_arena_sprintf(arena, "%s%s", head, text + len)
        else
            c.cbm_arena_sprintf(arena, "%s.%s", module_qn, text);
    }
    if (target == null) return -1;
    dst.* = std.mem.zeroes(LspDef);
    dst.qualified_name = src.qualified_name;
    dst.short_name = src.name;
    dst.label = "Variable";
    dst.def_module_qn = module_qn;
    dst.return_types = target;
    dst.lang = c.CBM_LANG_ZIG;
    return 0;
}

/// Zig: a type function (`fn List(comptime T: type) type`) names the
/// containers it returns, whose members extraction scopes under its QN. The
/// resolver sees it as a type too: a "Struct" def of the same QN (its fields
/// fold in like any struct's) whose type_params lists its parameters'
/// kinds (cbm_zig_param_kinds of the signature text).
fn pxcIsZigTypeFn(src: *const Definition) bool {
    return src.label != null and (streq(src.label, "Function") or streq(src.label, "Method")) and
        src.return_type != null and streq(src.return_type, "type") and
        src.qualified_name != null and src.name != null;
}

fn pxcBuildZigTypeFn(arena: *Arena, src: *const Definition, module_qn: CStr, dst: *LspDef) c_int {
    const out: CStr = c.cbm_zig_param_kinds(arena, if (src.signature != null) src.signature else "");
    if (out == null) return -1;
    dst.* = std.mem.zeroes(LspDef);
    dst.qualified_name = src.qualified_name;
    dst.short_name = src.name;
    dst.label = "Struct";
    dst.def_module_qn = module_qn;
    dst.type_params = out;
    dst.lang = c.CBM_LANG_ZIG;
    return 0;
}

/// Convert one CBMDefinition into a CBMLSPDef. Returns 0 on success, -1
/// to skip (unsupported label or missing required field). dst gets borrowed
/// pointers into src and into `arena` for synthesised composites.
fn pxcBuildLspDef(
    arena: *Arena,
    src: *const Definition,
    module_qn: CStr,
    namespace_name: CStr,
    lang: Language,
    dst: *LspDef,
    reg: ?*const Registry,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
) c_int {
    const label = pxcMapLabel(src.label);
    if (label == null or src.qualified_name == null or src.name == null) return -1;
    dst.* = std.mem.zeroes(LspDef);
    if (pxcIsJvmLang(lang) and namespace_name != null and namespace_name[0] != 0) {
        dst.qualified_name = pxcJvmDefQn(arena, src, namespace_name, label);
        dst.receiver_type = pxcJvmTypeQn(arena, namespace_name, src.parent_class);
    } else {
        dst.qualified_name = src.qualified_name;
        dst.receiver_type = src.parent_class;
    }
    dst.short_name = src.name;
    dst.label = label;
    dst.def_module_qn = module_qn;
    dst.namespace_name = namespace_name;
    dst.is_interface = streq(label, "Interface") or streq(label, "Protocol");
    // Single return-type string. The per-language registrars split on '|'
    // for multi-return languages (Go); single-return languages just see one
    // piece, which is what's already stored.
    dst.return_types = src.return_type;
    // Languages whose cross registrars read embedded_types as QNs get their
    // bases resolved against the project registry; everyone else keeps the
    // raw source spelling their own registrar already knows how to handle.
    dst.embedded_types = if (reg != null and pxcLangResolvesBaseQns(lang))
        pxcJoinBaseQns(arena, src.base_classes, reg, module_qn, imp_keys, imp_vals, imp_count)
    else
        pxcJoinPipe(arena, src.base_classes);
    dst.lang = lang;
    return 0;
}

/// A "Field" definition of the struct `struct_qn` that has a name and a type.
fn isFieldOf(fd: *const Definition, struct_qn: CStr) bool {
    return !(fd.label == null or fd.parent_class == null or fd.name == null or fd.name[0] == 0 or
        fd.return_type == null or fd.return_type[0] == 0 or !streq(fd.label, "Field") or
        !streq(fd.parent_class, struct_qn));
}

/// Go and Zig: fold per-field "Field" definitions into their owning struct's
/// field_defs. extract_defs.c emits one flat CBMDefinition per struct field
/// (label "Field", parent_class = owning struct QN, name = field name,
/// return_type = raw type text). Those rows are dropped by pxcBuildLspDef
/// (pxcMapLabel excludes "Field"), so without this fold every Go struct
/// registers with zero fields and field-chain calls (h.svc.Handle) can
/// never resolve. Fields are always declared in the same file as their struct,
/// so scanning the file's own defs covers every case. Runs inside
/// cbm_pxc_collect_all_defs — one site covers both the prebuilt-registry path
/// and the per-file fallback, since both consume all_defs.
fn pxcFoldGoStructFields(arena: ?*Arena, result: ?*const FileResult, defs: [*c]LspDef, start: c_int, end: c_int) void {
    const res = result orelse return;
    if (arena == null or defs == null or start >= end) return;
    for (ix(start)..ix(end)) |si| {
        const dst = util.at(defs, si);
        if (dst.label == null or !streq(dst.label, "Struct") or dst.qualified_name == null) continue;
        var count: usize = 0;
        var total: usize = 0; // "name:type" bytes; separators and NUL added below
        for (0..ix(res.defs.count)) |di| {
            const fd = util.at(res.defs.items, di);
            if (!isFieldOf(fd, dst.qualified_name)) continue;
            total += c.strlen(fd.name) + 1 + c.strlen(fd.return_type);
            count += 1;
        }
        if (count == 0) continue;
        // count - 1 separators + NUL.
        const bufsz = total + (count - 1) + 1;
        const buf: MutStr = @ptrCast(c.cbm_arena_alloc(arena, bufsz));
        if (buf == null) continue;
        var p: usize = 0;
        var written: usize = 0;
        for (0..ix(res.defs.count)) |di| {
            const fd = util.at(res.defs.items, di);
            if (!isFieldOf(fd, dst.qualified_name)) continue;
            var n = c.strlen(fd.name);
            @memcpy(buf[p .. p + n], fd.name[0..n]);
            p += n;
            buf[p] = ':';
            p += 1;
            n = c.strlen(fd.return_type);
            @memcpy(buf[p .. p + n], fd.return_type[0..n]);
            p += n;
            if (written + 1 < count) {
                buf[p] = '|';
                p += 1;
            }
            written += 1;
        }
        buf[p] = 0;
        dst.field_defs = buf;
    }
}

/// A file's declared namespace, or for Java/Kotlin the one inferred from its
/// source-root path, stored back on the result (in its arena) so later readers
/// of result->namespace_name see the same value. NULL when there is none.
pub export fn cbm_pxc_apply_inferred_namespace(result: *FileResult, file: *const FileInfo) CStr {
    var namespace_name: CStr = result.namespace_name;
    if ((namespace_name == null or namespace_name[0] == 0) and file.rel_path != null) {
        namespace_name = pxcInferJvmNamespace(&result.arena, file.rel_path, file.language);
        if (namespace_name != null and namespace_name[0] != 0) {
            result.namespace_name = namespace_name;
        }
    }
    return namespace_name;
}

/// Re-home defs[start, end) into `arena`: they borrow strings from a loaded
/// (spilled) result that is freed right after collection. def_module_qn is
/// borrowed from def_modules[] and stays.
fn pxcOwnDefStrings(arena: ?*Arena, defs: [*c]LspDef, start: c_int, end: c_int) void {
    for (ix(start)..ix(end)) |i| {
        const d = util.at(defs, i);
        inline for (.{
            "qualified_name", "short_name",     "label",      "receiver_type",
            "return_types",   "embedded_types", "field_defs", "method_names_str",
            "namespace_name", "type_params",
        }) |field| {
            @field(d, field) = c.cbm_arena_strdup(arena, @field(d, field));
        }
    }
}

/// Collect a project-wide CBMLSPDef[] from all cached results. Returns a
/// malloc'd array (caller frees) of length *out_count. String fields are
/// borrowed from cache[i]->arena and from def_modules[i] (also borrowed);
/// defs of a spilled result (ctx->spill) are copied into owned_arena, and
/// spilled results are skipped when owned_arena is NULL.
pub export fn cbm_pxc_collect_all_defs(
    ctx: ?*const Ctx,
    cache: [*c][*c]FileResult,
    files: [*c]const FileInfo,
    file_count: c_int,
    project_name: CStr,
    def_modules: [*c]MutStr,
    owned_arena: ?*Arena,
    out_count: *c_int,
) [*c]LspDef {
    const spill = if (ctx != null and owned_arena != null) ctx.?.spill else null;
    var cap: c_int = 0;
    for (0..ix(file_count)) |i| {
        if (cache[i] != null) cap += cache[i].*.defs.count;
    }
    if (cap == 0 and spill == null) {
        out_count.* = 0;
        return null;
    }
    var defs: [*c]LspDef = if (cap > 0) interop.callocN(LspDef, ix(cap)) else null;
    if (cap > 0 and defs == null) {
        out_count.* = 0;
        return null;
    }
    var idx: c_int = 0;
    for (0..ix(file_count)) |fi| {
        const file_start = idx;
        const file = util.at(files, fi);
        const is_zig = file.language == c.CBM_LANG_ZIG;
        const result: *FileResult = c.cbm_result_cache_acquire(cache, spill, @intCast(fi)) orelse continue;
        const loaded = result != @as(?*FileResult, cache[fi]);
        var need: c_int = result.defs.count;
        if (is_zig) for (0..ix(result.defs.count)) |di| {
            need += @intFromBool(pxcIsZigTypeFn(util.at(result.defs.items, di)));
        };
        if (idx + need > cap) {
            const grown: c_int = if (cap * 2 > idx + need) cap * 2 else idx + need;
            const more = interop.reallocN(LspDef, defs, ix(grown));
            if (more == null) {
                c.cbm_result_cache_release(cache, @intCast(fi), result);
                continue;
            }
            defs = more;
            cap = grown;
        }
        if (def_modules[fi] == null) {
            def_modules[fi] = c.cbm_pipeline_fqn_module_dir(project_name, file.rel_path, pxcModuleIsDir(file.language));
        }
        const namespace_name = cbm_pxc_apply_inferred_namespace(result, file);
        // One import map per FILE (not per def, and not per base name): the
        // cross-file base-class resolution needs the same local-name →
        // import-QN view pass_semantic uses. Both drivers have already
        // created this file's IMPORTS edges and filled the registry here.
        // Built only for the languages that consume resolved base QNs (and
        // for Zig, whose aliases resolve through it), and only when a caller
        // supplied the pipeline context.
        var base_reg: ?*const Registry = null;
        var imp_keys: StrList = null;
        var imp_vals: StrList = null;
        var imp_count: c_int = 0;
        if (ctx) |cx| {
            if (cx.registry != null and cx.gbuf != null and pxcLangResolvesBaseQns(file.language)) {
                base_reg = cx.registry;
            }
            if (cx.gbuf != null and (base_reg != null or is_zig)) {
                _ = pxcBuildImportMap(cx.gbuf, project_name, file.rel_path, &imp_keys, &imp_vals, &imp_count);
            }
        }
        for (0..ix(result.defs.count)) |di| {
            const src = util.at(result.defs.items, di);
            const dst = util.at(defs, ix(idx));
            const rc = if (is_zig and src.label != null and streq(src.label, "Variable") and
                src.qualified_name != null and src.name != null)
                pxcBuildZigAlias(&result.arena, result, src, def_modules[fi], dst, imp_keys, imp_vals, imp_count)
            else
                pxcBuildLspDef(&result.arena, src, def_modules[fi], namespace_name, file.language, dst, base_reg, imp_keys, imp_vals, imp_count);
            if (rc == 0) {
                // Zig: a function taking `anytype` carries its parameter kinds,
                // so callers' argument types reach it (cbm_pxc_collect_zig_args).
                if (is_zig and src.signature != null and c.strstr(src.signature, "anytype") != null and
                    (streq(dst.label, "Function") or streq(dst.label, "Method")))
                {
                    dst.type_params = c.cbm_zig_param_kinds(&result.arena, src.signature);
                }
                idx += 1;
            }
            if (is_zig and pxcIsZigTypeFn(src) and
                pxcBuildZigTypeFn(&result.arena, src, def_modules[fi], util.at(defs, ix(idx))) == 0)
            {
                idx += 1;
            }
        }
        pxcFreeImportMap(imp_keys, imp_vals, imp_count); // NULL-safe
        if (file.language == c.CBM_LANG_GO or is_zig) {
            pxcFoldGoStructFields(&result.arena, result, defs, file_start, idx);
        }
        if (loaded) pxcOwnDefStrings(owned_arena, defs, file_start, idx);
        c.cbm_result_cache_release(cache, @intCast(fi), result);
    }
    out_count.* = idx;
    return defs;
}

/// Build per-file import map (local_name -> resolved module QN) from gbuf
/// IMPORTS edges. Mirrors build_import_map() in pass_parallel. Returns 0
/// with *out_count = 0 when the file has no IMPORTS edges. Caller frees keys
/// with pxcFreeImportMap.
fn pxcBuildImportMap(
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
    if (file_qn == null) return 0;
    const file_node: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, file_qn);
    c.free(file_qn);
    const fnode = file_node orelse return 0;

    var edges: [*c][*c]const Edge = null;
    var edge_count: c_int = 0;
    const rc = c.cbm_gbuf_find_edges_by_source_type(gbuf, fnode.id, "IMPORTS", &edges, &edge_count);
    if (rc != 0 or edge_count == 0) return 0;

    const keys = interop.callocN(CStr, ix(edge_count));
    const vals = interop.callocN(CStr, ix(edge_count));
    if (keys == null or vals == null) {
        c.free(@ptrCast(keys));
        c.free(@ptrCast(vals));
        return 0;
    }
    const marker = "\"local_name\":\"";
    var count: usize = 0;
    for (0..ix(edge_count)) |i| {
        const e: *const Edge = edges[i];
        const target: ?*const Node = c.cbm_gbuf_find_by_id(gbuf, e.target_id);
        if (target == null or e.properties_json == null) continue;
        var start: CStr = c.strstr(e.properties_json, marker);
        if (start == null) continue;
        start += marker.len;
        const end: CStr = c.strchr(start, '"');
        if (end == null or @intFromPtr(end) <= @intFromPtr(start)) continue;
        const n = ptrDiff(end, start);
        const local: MutStr = @ptrCast(c.malloc(n + 1));
        if (local == null) continue;
        @memcpy(local[0..n], start[0..n]);
        local[n] = 0;
        keys[count] = local;
        vals[count] = target.?.qualified_name; // borrowed from gbuf
        count += 1;
    }
    out_keys.* = keys;
    out_vals.* = vals;
    out_count.* = @intCast(count);
    return 0;
}

fn pxcFreeImportMap(keys: StrList, vals: StrList, count: c_int) void {
    if (keys != null) {
        for (0..ix(count)) |i| c.free(@constCast(keys[i]));
        c.free(@ptrCast(keys));
    }
    c.free(@ptrCast(vals)); // vals strings borrowed from gbuf — don't free elements
}

/// Detect TS dialect flags from a relative path.
pub export fn cbm_pxc_ts_modes(lang: Language, rel_path: CStr, out_js: *bool, out_jsx: *bool, out_dts: *bool) void {
    out_js.* = lang == c.CBM_LANG_JAVASCRIPT;
    out_jsx.* = lang == c.CBM_LANG_TSX;
    out_dts.* = false;
    if (rel_path == null) return;
    const rl = c.strlen(rel_path);
    if (lang == c.CBM_LANG_JAVASCRIPT and rl >= 4 and streq(rel_path + rl - 4, ".jsx")) {
        out_jsx.* = true;
    }
    if (lang == c.CBM_LANG_TYPESCRIPT and rl >= 5 and streq(rel_path + rl - 5, ".d.ts")) {
        out_dts.* = true;
    }
}

/// Returns true when this language has a cross-file LSP wired up.
pub export fn cbm_pxc_has_cross_lsp(lang: Language) bool {
    return switch (lang) {
        c.CBM_LANG_GO,
        c.CBM_LANG_C,
        c.CBM_LANG_CPP,
        c.CBM_LANG_CUDA,
        c.CBM_LANG_PYTHON,
        c.CBM_LANG_JAVASCRIPT,
        c.CBM_LANG_TYPESCRIPT,
        c.CBM_LANG_TSX,
        c.CBM_LANG_PHP,
        c.CBM_LANG_CSHARP, // tier-2 prebuilt registry path (pass_parallel)
        c.CBM_LANG_JAVA, // fallback cbm_pxc_run_one path
        c.CBM_LANG_KOTLIN, // fallback cbm_pxc_run_one path
        c.CBM_LANG_RUST, // fallback cbm_pxc_run_one path (manifest-aware)
        c.CBM_LANG_ZIG, // fallback cbm_pxc_run_one path
        => true,
        else => false,
    };
}

// Append cross-file results from `src_out` (allocated in a scratch arena
// about to be destroyed) into `dst_calls` (lives in cache_entry->arena),
// copying every string field into dst_arena. Skips entries whose
// (caller_qn, callee_qn) is already present — avoids inflating the array with
// cross-file duplicates of per-file LSP output.
//
// Dedup uses a hash set keyed on "caller\x1fcallee", giving O(1) membership.
// The previous linear strcmp scan made each append O(n), so a file that
// resolved very many cross-calls turned the whole append into O(n^2) and could
// peg a core for minutes (observed: an index hung in pxc_append_results/strcmp).
// The key strings live in a scratch arena that is destroyed after the table.
/// Invocation rows dedup on (caller, callee). A reference row is tied to one
/// source occurrence, so its site is part of the key and it never collides
/// with an invocation of the same callee. An invocation row with a site span
/// proves one call site (Zig), so its site is part of the key too.
fn pxcResultKey(keys: *Arena, rc: *const c.CBMResolvedCall) MutStr {
    if (rc.kind == c.CBM_RESOLVED_CALL_REFERENCE) {
        return c.cbm_arena_sprintf(keys, "%s\x1f%s\x1fR%u:%u", rc.caller_qn, rc.callee_qn, rc.site_start_byte, rc.site_end_byte);
    }
    if (rc.site_end_byte > rc.site_start_byte) {
        return c.cbm_arena_sprintf(keys, "%s\x1f%s\x1fI%u", rc.caller_qn, rc.callee_qn, rc.site_start_byte);
    }
    return c.cbm_arena_sprintf(keys, "%s\x1f%s", rc.caller_qn, rc.callee_qn);
}

fn pxcAppendResults(dst_arena: *Arena, dst_calls: *c.CBMResolvedCallArray, src_out: *const c.CBMResolvedCallArray) void {
    var keys: Arena = undefined;
    c.cbm_arena_init(&keys);
    const seen = c.cbm_ht_create(@as(u32, @bitCast(dst_calls.count + src_out.count + 1)));

    for (0..ix(dst_calls.count)) |i| {
        const rc = util.at(dst_calls.items, i);
        if (rc.caller_qn != null and rc.callee_qn != null) {
            const k = pxcResultKey(&keys, rc);
            if (k != null) _ = c.cbm_ht_set(seen, k, @ptrFromInt(1));
        }
    }

    for (0..ix(src_out.count)) |j| {
        const src = util.at(src_out.items, j);
        if (src.caller_qn == null or src.callee_qn == null) continue;
        const k = pxcResultKey(&keys, src);
        if (k != null and c.cbm_ht_has(seen, k)) continue;
        if (k != null) _ = c.cbm_ht_set(seen, k, @ptrFromInt(1));
        var dst = std.mem.zeroes(c.CBMResolvedCall);
        dst.caller_qn = c.cbm_arena_strdup(dst_arena, src.caller_qn);
        dst.callee_qn = c.cbm_arena_strdup(dst_arena, src.callee_qn);
        dst.strategy = if (src.strategy != null) c.cbm_arena_strdup(dst_arena, src.strategy) else null;
        dst.confidence = src.confidence;
        dst.reason = if (src.reason != null) c.cbm_arena_strdup(dst_arena, src.reason) else null;
        dst.kind = src.kind;
        dst.site_start_byte = src.site_start_byte;
        dst.site_end_byte = src.site_end_byte;
        c.cbm_resolvedcall_push(dst_calls, dst_arena, dst);
    }

    c.cbm_ht_free(seen);
    c.cbm_arena_destroy(&keys);
}

// ── Rust workspace manifest (Cargo.toml) for cross-CRATE resolution ──
//
// cbm_pxc_run_one's signature is shared with the parallel pass
// (pass_parallel) and cannot grow a manifest parameter without touching
// that file. We therefore pass the parsed workspace manifest to the Rust
// cross-file resolver through a thread-local borrowed pointer that each
// resolve worker sets from the manifest cbm_pxc_build_rust_manifest read
// once from the project's root Cargo.toml. The manifest's strings are
// owned by the caller's arena; the pointer is borrowed (NULL when
// the project has no Cargo.toml — single-crate / non-workspace projects,
// where in-file resolution needs no workspace metadata).
threadlocal var g_pxc_rust_manifest: ?*const c.CBMCargoManifest = null;

pub export fn cbm_pxc_set_rust_manifest(m: ?*const c.CBMCargoManifest) void {
    g_pxc_rust_manifest = m;
}

pub export fn cbm_pxc_get_rust_manifest() ?*const c.CBMCargoManifest {
    return g_pxc_rust_manifest;
}

/// Convert a CBMLSPDef array (the pipeline's lingua franca, go_lsp.h:73)
/// into a CBMRustLSPDef array (rust_lsp.h) inside `arena`. The two structs
/// share their first 9 string fields; CBMRustLSPDef adds `trait_qn` before
/// `is_interface` whereas CBMLSPDef has `is_interface` followed by `lang`,
/// so a memcpy is unsafe — copy field-by-field. trait_qn is left NULL
/// because the pipeline's collect-all-defs step does not carry the
/// impl-Trait-for-Type linkage; the resolver still recovers trait dispatch
/// from the in-file walk (the cross-file path only needs receiver_type).
fn pxcLspdefsToRust(arena: *Arena, defs: [*c]const LspDef, def_count: c_int) [*c]c.CBMRustLSPDef {
    if (defs == null or def_count <= 0) return null;
    const out: [*c]c.CBMRustLSPDef =
        @ptrCast(@alignCast(c.cbm_arena_alloc(arena, ix(def_count) * @sizeOf(c.CBMRustLSPDef))));
    if (out == null) return null;
    for (0..ix(def_count)) |i| {
        inline for (.{
            "qualified_name", "short_name",     "label",      "receiver_type",    "def_module_qn",
            "return_types",   "embedded_types", "field_defs", "method_names_str",
        }) |field| {
            @field(out[i], field) = @field(defs[i], field);
        }
        out[i].trait_qn = null;
        out[i].is_interface = defs[i].is_interface;
    }
    return out;
}

/// Run cross-file LSP for a single file inside a scratch arena that gets
/// freed when the call returns. The LSP would otherwise allocate a fresh
/// type registry + stdlib + all project defs into the supplied arena, and
/// that adds up to O(N×project_size) memory if we used cache[i]->arena
/// directly across N files (test_incremental.c saw 3.5 GB peak on a
/// 1100-file repo before this fix). Output gets copied into the file's own
/// arena and merged into result->resolved_calls.
pub export fn cbm_pxc_run_one(
    lang: Language,
    r: *FileResult,
    source: CStr,
    source_len: c_int,
    module_qn: CStr,
    defs: [*c]LspDef,
    def_count: c_int,
    imp_names: StrList,
    imp_qns: StrList,
    imp_count: c_int,
) void {
    const tree = r.cached_tree; // may be NULL — LSP re-parses then

    var scratch: Arena = undefined;
    c.cbm_arena_init(&scratch);
    var out = std.mem.zeroes(c.CBMResolvedCallArray);

    switch (lang) {
        c.CBM_LANG_GO => c.cbm_run_go_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, tree, &out),
        c.CBM_LANG_C, c.CBM_LANG_CPP, c.CBM_LANG_CUDA => {
            const cpp_mode = lang != c.CBM_LANG_C;
            // C/C++ cross LSP takes include_paths/include_ns_qns instead of
            // imports — the existing pipeline doesn't carry C-style include
            // resolution as a separate map, so pass NULL/0 and let the LSP
            // fall back to its own #include scan.
            const status = c.cbm_run_c_lsp_cross_status(&scratch, source, source_len, module_qn, cpp_mode, defs, def_count, null, null, 0, tree, &out);
            if (status != c.CBM_LSP_COMPLETE) c.cbm_c_lsp_record_failure(r, status);
        },
        c.CBM_LANG_PYTHON => {
            const status = c.cbm_run_py_lsp_cross_status(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, tree, &out);
            if (status != c.CBM_LSP_COMPLETE) c.cbm_py_lsp_record_failure(r, status);
        },
        c.CBM_LANG_PHP => c.cbm_run_php_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, tree, &out),
        c.CBM_LANG_JAVA => c.cbm_run_java_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, tree, &out),
        c.CBM_LANG_KOTLIN => c.cbm_run_kotlin_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, tree, &out),
        c.CBM_LANG_RUST => {
            // The Rust resolver wants CBMRustLSPDef (rust_lsp.h), not the
            // pipeline's CBMLSPDef — the structs share their first 9 fields
            // but diverge after, so convert into the scratch arena. The
            // workspace manifest (set by each resolve worker) lets
            // `crate_a::foo` route across the crate boundary (#56).
            const rdefs = pxcLspdefsToRust(&scratch, defs, def_count);
            c.cbm_run_rust_lsp_cross_with_manifest(&scratch, source, source_len, module_qn, rdefs, def_count, imp_names, imp_qns, imp_count, tree, g_pxc_rust_manifest, &out);
        },
        c.CBM_LANG_ZIG => c.cbm_run_zig_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, tree, &out),
        else => {},
    }

    pxcAppendResults(&r.arena, &r.resolved_calls, &out);
    c.cbm_arena_destroy(&scratch);
}

/// cbm_pxc_run_one for Zig with the project's `anytype` argument types (args)
/// and, when collect is set, collecting them instead: the file's resolved
/// calls are then discarded.
fn pxcRunOneZig(
    r: *FileResult,
    source: CStr,
    source_len: c_int,
    module_qn: CStr,
    defs: [*c]LspDef,
    def_count: c_int,
    imp_names: StrList,
    imp_qns: StrList,
    imp_count: c_int,
    args: [*c]const c.CBMZigArgType,
    arg_count: c_int,
    collect: ?*c.CBMZigArgTypeArray,
) void {
    var scratch: Arena = undefined;
    c.cbm_arena_init(&scratch);
    var out = std.mem.zeroes(c.CBMResolvedCallArray);
    c.cbm_run_zig_lsp_cross_ex(&scratch, source, source_len, module_qn, defs, def_count, imp_names, imp_qns, imp_count, r.cached_tree, &out, args, arg_count, collect);
    if (collect == null) pxcAppendResults(&r.arena, &r.resolved_calls, &out);
    c.cbm_arena_destroy(&scratch);
}

/// Variant of cbm_pxc_run_one for TS/JS/JSX/TSX with explicit dialect
/// flags. Same scratch-arena lifecycle as cbm_pxc_run_one.
pub export fn cbm_pxc_run_one_ts(
    r: *FileResult,
    source: CStr,
    source_len: c_int,
    module_qn: CStr,
    defs: [*c]LspDef,
    def_count: c_int,
    imp_names: StrList,
    imp_qns: StrList,
    imp_count: c_int,
    js_mode: bool,
    jsx_mode: bool,
    dts_mode: bool,
) void {
    var scratch: Arena = undefined;
    c.cbm_arena_init(&scratch);
    var out = std.mem.zeroes(c.CBMResolvedCallArray);

    c.cbm_run_ts_lsp_cross(&scratch, source, source_len, module_qn, js_mode, jsx_mode, dts_mode, defs, def_count, imp_names, imp_qns, imp_count, r.cached_tree, &out);

    pxcAppendResults(&r.arena, &r.resolved_calls, &out);
    c.cbm_arena_destroy(&scratch);
}

// Per-file cross-LSP dispatch for the parallel resolve worker: filter the global defs
// down to the file's own+imported modules via the module-def index, resolve
// through the shared prebuilt registry when the language has one (per-file
// OVERLAY pattern — no registry build, no finalize), and only fall back to
// the per-file registry build (with the FILTERED defs) for languages without
// a shared-registry variant. Before this helper existed the sequential
// driver fed the FULL def list into full per-file registry builds —
// O(files x defs), which ground an 81k-file TS corpus for hours.
//
// `rust_shared_get` supplies the lazily-built shared Rust all-defs registry
// (the parallel resolver owns its once-guard); NULL means "no shared rust
// registry available" and rust NULL-filter files take the per-file build.
// Per-file registry-build cost counters (upstream cffb4e37), logged by
// pass_parallel at the end of resolve.
var g_pxc_defs_registered: std.atomic.Value(u64) = .init(0);
var g_pxc_build_files: std.atomic.Value(u64) = .init(0);
var g_pxc_filter_files: std.atomic.Value(u64) = .init(0);
var g_pxc_filter_failed: std.atomic.Value(u64) = .init(0);

pub export fn cbm_pxc_filter_stats(defs_registered: *u64, build_files: *u64, filter_files: *u64, filter_failed: *u64) void {
    defs_registered.* = g_pxc_defs_registered.load(.monotonic);
    build_files.* = g_pxc_build_files.load(.monotonic);
    filter_files.* = g_pxc_filter_files.load(.monotonic);
    filter_failed.* = g_pxc_filter_failed.load(.monotonic);
}

pub export fn cbm_pxc_filter_stats_reset() void {
    g_pxc_defs_registered.store(0, .monotonic);
    g_pxc_build_files.store(0, .monotonic);
    g_pxc_filter_files.store(0, .monotonic);
    g_pxc_filter_failed.store(0, .monotonic);
}

/// imp_vals plus every dotted prefix of each QN in the types bound to
/// module_qn's `anytype` parameters (in arena): the modules among them hold
/// the defs a bound walk resolves against.
fn pxcZigArgModules(
    arena: *Arena,
    r: *const c.CBMCrossLspRegistries,
    module_qn: CStr,
    imp_vals: StrList,
    imp_count: c_int,
    out_count: *c_int,
) StrList {
    const mlen = c.strlen(module_qn);
    var cap: c_int = imp_count;
    for (0..ix(r.zig_arg_count)) |i| {
        const q: CStr = r.zig_args[i].callee_qn;
        if (c.strncmp(q, module_qn, mlen) == 0 and q[mlen] == '.') {
            var t: CStr = r.zig_args[i].type;
            while (t[0] != 0) : (t += 1) {
                cap += @intFromBool(t[0] == '.');
            }
        }
    }
    const out: StrList = @ptrCast(@alignCast(c.cbm_arena_alloc(arena, ix(cap + 1) * @sizeOf(CStr))));
    if (out == null) {
        out_count.* = imp_count;
        return imp_vals;
    }
    var n: c_int = 0;
    for (0..ix(imp_count)) |i| {
        out[ix(n)] = imp_vals[i];
        n += 1;
    }
    for (0..ix(r.zig_arg_count)) |i| {
        const q: CStr = r.zig_args[i].callee_qn;
        if (c.strncmp(q, module_qn, mlen) != 0 or q[mlen] != '.') continue;
        const t: CStr = r.zig_args[i].type;
        var start: usize = 0;
        var j: usize = 0;
        while (true) : (j += 1) {
            const ch = t[j];
            const qn_char = ch != 0 and c.strchr("[]()*?, ", ch) == null;
            if (ch == '.' and j > start and n < cap) {
                out[ix(n)] = c.cbm_arena_strndup(arena, t + start, j - start);
                n += 1;
            }
            if (!qn_char) start = j + 1;
            if (ch == 0) break;
        }
    }
    out_count.* = n;
    return out;
}

const RustSharedGet = ?*const fn (?*anyopaque) callconv(.c) [*c]c.CBMTypeRegistry;

pub export fn cbm_pxc_dispatch_file(
    lang: Language,
    result_c: ?*FileResult,
    source: CStr,
    source_len: c_int,
    rel: CStr,
    def_module: CStr,
    cross_registries: ?*const c.CBMCrossLspRegistries,
    module_def_index: ?*const ModuleDefIndex,
    all_defs: [*c]LspDef,
    all_def_count: c_int,
    imp_keys: StrList,
    imp_vals: StrList,
    imp_count: c_int,
    rust_shared_get: RustSharedGet,
    rust_shared_ctx: ?*anyopaque,
) void {
    const result = result_c orelse return;
    var used_prebuilt = false;
    const prebuilt: ?*c.CBMTypeRegistry =
        if (cross_registries != null) c.cbm_pxc_registry_for_lang(cross_registries, lang) else null;
    if (prebuilt != null) {
        switch (lang) {
            c.CBM_LANG_GO => {
                // Tier 3 (metadata-driven): pure lookup over the Tier-1
                // lsp_unresolved entries — no parse, no AST walk. Then the
                // AST walk on the shared Tier-2 registry (mirroring every
                // other language) so NAMED receivers evaluated against
                // project-wide defs also resolve. The walk variant below is
                // read-only — the sealed registry is safe for parallel
                // workers.
                _ = c.cbm_go_fast_resolve_qualified_calls(result, prebuilt, imp_keys, imp_vals, imp_count);
                c.cbm_run_go_lsp_cross_with_registry(&result.arena, source, source_len, def_module, prebuilt, imp_keys, imp_vals, imp_count, result.cached_tree, &result.resolved_calls);
                used_prebuilt = true;
            },
            c.CBM_LANG_PYTHON => {
                const status = c.cbm_run_py_lsp_cross_with_registry_status(&result.arena, source, source_len, def_module, prebuilt, imp_keys, imp_vals, imp_count, result.cached_tree, &result.resolved_calls);
                if (status != c.CBM_LSP_COMPLETE) c.cbm_py_lsp_record_failure(result, status);
                used_prebuilt = true;
            },
            c.CBM_LANG_C, c.CBM_LANG_CPP, c.CBM_LANG_CUDA => {
                const status = c.cbm_run_c_lsp_cross_with_registry_status(&result.arena, source, source_len, def_module, lang != c.CBM_LANG_C, prebuilt, imp_keys, imp_vals, imp_count, result.cached_tree, &result.resolved_calls);
                if (status != c.CBM_LSP_COMPLETE) c.cbm_c_lsp_record_failure(result, status);
                used_prebuilt = true;
            },
            c.CBM_LANG_CSHARP => {
                c.cbm_run_cs_lsp_cross_with_registry(&result.arena, source, source_len, def_module, prebuilt, imp_vals, imp_count, result.cached_tree, &result.resolved_calls);
                used_prebuilt = true;
            },
            c.CBM_LANG_JAVASCRIPT, c.CBM_LANG_TYPESCRIPT, c.CBM_LANG_TSX => {
                // TS: per-file OVERLAY chained to the shared base. Filter to
                // own+imports so the overlay builder can pick out own-module
                // defs without scanning the whole project.
                var js: bool = undefined;
                var jsx: bool = undefined;
                var dts: bool = undefined;
                cbm_pxc_ts_modes(lang, rel, &js, &jsx, &dts);
                var ts_defs = all_defs;
                var ts_def_count = all_def_count;
                var ts_filtered: [*c]LspDef = null;
                if (module_def_index != null) {
                    var fc: c_int = 0;
                    ts_filtered = cbm_pxc_filter_defs_for_file(module_def_index, all_defs, lang, result.namespace_name, def_module, imp_vals, imp_count, &fc);
                    if (ts_filtered != null) {
                        ts_defs = ts_filtered;
                        ts_def_count = fc;
                    }
                }
                c.cbm_run_ts_lsp_cross_with_registry(&result.arena, source, source_len, def_module, js, jsx, dts, prebuilt, ts_defs, ts_def_count, imp_keys, imp_vals, imp_count, result.cached_tree, &result.resolved_calls);
                c.free(ts_filtered);
                used_prebuilt = true;
            },
            // PHP falls through to the per-file build path below until its
            // overlay variant lands.
            else => {},
        }
    }

    if (used_prebuilt) return;
    // Fallback: gopls per-file filter + per-file registry build. RUST is
    // exempt from the module filter: its resolution is Cargo-manifest-aware
    // and a `crate_a::foo` reference routes to defs in ANOTHER workspace
    // crate — a module that is in neither own_module nor the import map, so
    // the filter starves cross-crate resolution (#56 repro red). Rust
    // therefore always resolves against the FULL def universe: the lazily
    // built shared registry when available, else a full per-file build.
    var filtered: [*c]LspDef = null;
    var file_defs = all_defs;
    var file_def_count = all_def_count;
    // Zig: a function walked with an `anytype` parameter bound needs the defs
    // of the bound types' modules too, which it need not import.
    var filter_qns = imp_vals;
    var filter_count = imp_count;
    var zig_arena: Arena = undefined;
    var zig_arena_live = false;
    if (lang == c.CBM_LANG_ZIG and cbm_pxc_zig_args_in_module(cross_registries, def_module)) {
        c.cbm_arena_init(&zig_arena);
        zig_arena_live = true;
        filter_qns = pxcZigArgModules(&zig_arena, cross_registries.?, def_module, imp_vals, imp_count, &filter_count);
    }
    if (module_def_index != null and lang != c.CBM_LANG_RUST) {
        var filtered_count: c_int = 0;
        filtered = cbm_pxc_filter_defs_for_file(module_def_index, all_defs, lang, result.namespace_name, def_module, filter_qns, filter_count, &filtered_count);
        if (filtered != null) {
            file_defs = filtered;
            file_def_count = filtered_count;
        }
        _ = g_pxc_filter_files.fetchAdd(1, .monotonic);
        if (filtered == null) _ = g_pxc_filter_failed.fetchAdd(1, .monotonic);
    }
    // Per-file registry build cost is driven by this count.
    _ = g_pxc_defs_registered.fetchAdd(@bitCast(@as(i64, file_def_count)), .monotonic);
    _ = g_pxc_build_files.fetchAdd(1, .monotonic);
    if (lang == c.CBM_LANG_RUST) {
        const shared: ?*c.CBMTypeRegistry = if (rust_shared_get) |get| get(rust_shared_ctx) else null;
        if (shared != null) {
            c.cbm_run_rust_lsp_cross_with_registry(&result.arena, source, source_len, def_module, shared, imp_keys, imp_vals, imp_count, result.cached_tree, cbm_pxc_get_rust_manifest(), &result.resolved_calls, null);
        } else {
            cbm_pxc_run_one(lang, result, source, source_len, def_module, file_defs, file_def_count, imp_keys, imp_vals, imp_count);
        }
    } else if (lang == c.CBM_LANG_JAVASCRIPT or lang == c.CBM_LANG_TYPESCRIPT or lang == c.CBM_LANG_TSX) {
        var js: bool = undefined;
        var jsx: bool = undefined;
        var dts: bool = undefined;
        cbm_pxc_ts_modes(lang, rel, &js, &jsx, &dts);
        cbm_pxc_run_one_ts(result, source, source_len, def_module, file_defs, file_def_count, imp_keys, imp_vals, imp_count, js, jsx, dts);
    } else if (lang == c.CBM_LANG_ZIG) {
        pxcRunOneZig(
            result,
            source,
            source_len,
            def_module,
            file_defs,
            file_def_count,
            imp_keys,
            imp_vals,
            imp_count,
            if (cross_registries) |r| r.zig_args else null,
            if (cross_registries) |r| r.zig_arg_count else 0,
            null,
        );
    } else {
        cbm_pxc_run_one(lang, result, source, source_len, def_module, file_defs, file_def_count, imp_keys, imp_vals, imp_count);
    }
    c.free(filtered);
    if (zig_arena_live) c.cbm_arena_destroy(&zig_arena);
}

fn pxcZigArgCmp(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x: *const c.CBMZigArgType = @ptrCast(@alignCast(a));
    const y: *const c.CBMZigArgType = @ptrCast(@alignCast(b));
    var cmp = c.strcmp(x.callee_qn, y.callee_qn);
    if (cmp == 0) cmp = x.param - y.param;
    return if (cmp != 0) cmp else c.strcmp(x.type, y.type);
}

pub export fn cbm_pxc_collect_zig_args(
    ctx_c: ?*const Ctx,
    cache: [*c][*c]FileResult,
    files: [*c]const FileInfo,
    file_count: c_int,
    def_modules: [*c]const MutStr,
    module_def_index: ?*const ModuleDefIndex,
    all_defs: [*c]LspDef,
    def_count: c_int,
    arena: ?*Arena,
    out_count: *c_int,
) [*c]c.CBMZigArgType {
    out_count.* = 0;
    var any = false;
    for (0..ix(def_count)) |i| {
        if (any) break;
        const d = util.at(all_defs, i);
        any = d.lang == c.CBM_LANG_ZIG and d.type_params != null and
            c.strchr(d.type_params, '*') != null and d.label != null and
            (streq(d.label, "Function") or streq(d.label, "Method"));
    }
    const ctx = ctx_c orelse return null;
    if (!any or cache == null or def_modules == null) return null;
    var collect = std.mem.zeroes(c.CBMZigArgTypeArray);
    collect.arena = arena;
    for (0..ix(file_count)) |fi| {
        if (files[fi].language != c.CBM_LANG_ZIG or def_modules[fi] == null) continue;
        const result: *FileResult = c.cbm_result_cache_acquire(cache, ctx.spill, @intCast(fi)) orelse continue;
        var owned: MutStr = null;
        var source: CStr = result.source;
        var source_len: c_int = result.source_len;
        if ((source == null or source_len <= 0) and result.calls.count > 0) {
            owned = pxcReadFile(files[fi].path, &source_len);
            source = owned;
        }
        if (source != null and source_len > 0 and result.calls.count > 0) {
            var imp_keys: StrList = null;
            var imp_vals: StrList = null;
            var imp_count: c_int = 0;
            _ = pxcBuildImportMap(ctx.gbuf, ctx.project_name, files[fi].rel_path, &imp_keys, &imp_vals, &imp_count);
            var fc: c_int = 0;
            const filtered: [*c]LspDef = if (module_def_index != null)
                cbm_pxc_filter_defs_for_file(module_def_index, all_defs, c.CBM_LANG_ZIG, result.namespace_name, def_modules[fi], imp_vals, imp_count, &fc)
            else
                null;
            pxcRunOneZig(
                result,
                source,
                source_len,
                def_modules[fi],
                if (filtered != null) filtered else all_defs,
                if (filtered != null) fc else def_count,
                imp_keys,
                imp_vals,
                imp_count,
                null,
                0,
                &collect,
            );
            c.free(filtered);
            pxcFreeImportMap(imp_keys, imp_vals, imp_count);
        }
        c.free(owned);
        c.cbm_result_cache_release(cache, @intCast(fi), result);
    }
    if (collect.count == 0) {
        c.free(collect.items);
        return null;
    }
    c.qsort(collect.items, ix(collect.count), @sizeOf(c.CBMZigArgType), pxcZigArgCmp);
    var n: usize = 1;
    for (1..ix(collect.count)) |i| {
        if (pxcZigArgCmp(util.at(collect.items, i), util.at(collect.items, n - 1)) != 0) {
            collect.items[n] = collect.items[i];
            n += 1;
        }
    }
    interop.logInfo("lsp_cross.zig_anytype", .{ "arg_types", c.cbm_log_itoa(@intCast(n)) });
    out_count.* = @intCast(n);
    return collect.items;
}

pub export fn cbm_pxc_zig_args_in_module(r_c: ?*const c.CBMCrossLspRegistries, module_qn: CStr) bool {
    const r = r_c orelse return false;
    const n = if (module_qn != null) c.strlen(module_qn) else 0;
    if (n == 0) return false;
    for (0..ix(r.zig_arg_count)) |i| {
        const q: CStr = r.zig_args[i].callee_qn;
        if (c.strncmp(q, module_qn, n) == 0 and q[n] == '.') return true;
    }
    return false;
}

/// Parse the project's root Cargo.toml (if present) into `out_m`, using
/// `marena` for the manifest's owned strings. Returns true when a manifest
/// was parsed (a workspace root or any [package]/[dependencies]); false when
/// there is no readable Cargo.toml, leaving *out_m untouched. The resulting
/// manifest feeds cross-CRATE Rust resolution (#56): its [workspace].members
/// map lets `crate_a::foo` route to the member crate's def.
pub export fn cbm_pxc_build_rust_manifest(ctx_c: ?*const Ctx, marena: ?*Arena, out_m: ?*c.CBMCargoManifest) bool {
    const ctx = ctx_c orelse return false;
    const out = out_m orelse return false;
    if (ctx.repo_path == null or marena == null) return false;
    var path: [1024]u8 = undefined;
    const n = c.snprintf(&path, path.len, "%s/Cargo.toml", ctx.repo_path);
    if (n <= 0 or ix(n) >= path.len) return false;
    var toml_len: c_int = 0;
    const toml = pxcReadFile(&path, &toml_len);
    if (toml == null or toml_len <= 0) {
        c.free(toml);
        return false;
    }
    out.* = std.mem.zeroes(c.CBMCargoManifest);
    c.cbm_cargo_parse(marena, toml, toml_len, out);
    c.free(toml); // cargo parser copies into marena
    return true;
}

// ── Per-module def index (gopls "package summary" pattern) ──────────

const ModuleEntry = extern struct {
    count: c_int,
    cap: c_int,
    /// malloc'd; indices into the caller's all_defs[]
    indices: [*c]c_int,
};

/// struct CBMModuleDefIndex. The header only names it as an opaque type.
const ModuleDefIndex = extern struct {
    /// module_qn → ModuleEntry*
    ht: ?*HashTable,
    /// declared package/namespace → ModuleEntry*
    namespace_ht: ?*HashTable,
    /// total entries in the all_defs[] array
    def_count: c_int,
};

/// cbm_ht_foreach callback: free each ModuleEntry.
fn pxcModuleEntryFreeCb(key: CStr, value: ?*anyopaque, userdata: ?*anyopaque) callconv(.c) void {
    _ = key;
    _ = userdata;
    const e: *ModuleEntry = @ptrCast(@alignCast(value orelse return));
    c.free(e.indices);
    c.free(e);
}

fn pxcModuleEntryGetOrCreate(ht: ?*HashTable, key: CStr) ?*ModuleEntry {
    if (ht == null or key == null or key[0] == 0) return null;
    if (c.cbm_ht_get(ht, key)) |found| return @ptrCast(@alignCast(found));
    const e: *ModuleEntry = @ptrCast(@alignCast(c.calloc(1, @sizeOf(ModuleEntry)) orelse return null));
    e.cap = 8;
    e.indices = interop.callocN(c_int, ix(e.cap));
    if (e.indices == null) {
        c.free(e);
        return null;
    }
    _ = c.cbm_ht_set(ht, key, e);
    return e;
}

fn pxcModuleEntryAddIndex(entry: ?*ModuleEntry, index: c_int) void {
    const e = entry orelse return;
    if (e.count >= e.cap) {
        const new_cap = e.cap * 2;
        const new_indices = interop.reallocN(c_int, e.indices, ix(new_cap));
        if (new_indices == null) return;
        e.indices = new_indices;
        e.cap = new_cap;
    }
    e.indices[ix(e.count)] = index;
    e.count += 1;
}

fn pxcMarkEntryDefs(selected: [*c]bool, entry: ?*const ModuleEntry, all_defs: [*c]const LspDef, caller_lang: Language) c_int {
    const e = entry orelse return 0;
    if (selected == null) return 0;
    var added: c_int = 0;
    for (0..ix(e.count)) |j| {
        const idx = ix(e.indices[j]);
        if (!pxcDefLangMatches(caller_lang, all_defs[idx].lang) or selected[idx]) continue;
        selected[idx] = true;
        added += 1;
    }
    return added;
}

fn pxcIsJvmLang(lang: Language) bool {
    return lang == c.CBM_LANG_JAVA or lang == c.CBM_LANG_KOTLIN;
}

fn pxcDefLangMatches(caller_lang: Language, def_lang: Language) bool {
    if (pxcIsJvmLang(caller_lang)) return pxcIsJvmLang(def_lang);
    return true;
}

fn pxcMarkModuleDefs(
    idx: *const ModuleDefIndex,
    selected: [*c]bool,
    all_defs: [*c]const LspDef,
    caller_lang: Language,
    module_qn: CStr,
    total: *c_int,
) void {
    if (idx.ht == null or module_qn == null or module_qn[0] == 0) return;
    const e: ?*ModuleEntry = @ptrCast(@alignCast(c.cbm_ht_get(idx.ht, module_qn)));
    total.* += pxcMarkEntryDefs(selected, e, all_defs, caller_lang);
}

/// Zig: an alias names a module other than the file's imports (a re-export
/// `pub const Database = zova.Database;`, or `const sqlite = @import(..)` that a
/// return type text spells), so a Zig file also gets the defs of each module a
/// selected alias names, transitively. Returns the number of defs added.
const PXC_ZIG_ALIAS_ROUNDS = 8;
fn pxcMarkZigAliasModules(idx: *const ModuleDefIndex, selected: [*c]bool, all_defs: [*c]const LspDef) c_int {
    var added_total: c_int = 0;
    for (0..PXC_ZIG_ALIAS_ROUNDS) |_| {
        var added: c_int = 0;
        for (0..ix(idx.def_count)) |i| {
            const d = util.at(all_defs, i);
            if (!selected[i] or d.lang != c.CBM_LANG_ZIG or d.return_types == null or
                d.label == null or !streq(d.label, "Variable"))
            {
                continue;
            }
            var buf: [c.CBM_SZ_512]u8 = undefined;
            _ = c.snprintf(&buf, buf.len, "%s", d.return_types);
            while (true) {
                const e: ?*ModuleEntry = @ptrCast(@alignCast(c.cbm_ht_get(idx.ht, &buf)));
                if (e != null) {
                    added += pxcMarkEntryDefs(selected, e, all_defs, c.CBM_LANG_ZIG);
                    break;
                }
                const dot: MutStr = c.strrchr(&buf, '.');
                if (dot == null) break;
                dot[0] = 0;
            }
        }
        added_total += added;
        if (added == 0) break;
    }
    return added_total;
}

pub export fn cbm_pxc_build_module_def_index(all_defs: [*c]LspDef, def_count: c_int) ?*ModuleDefIndex {
    if (all_defs == null or def_count <= 0) return null;

    const ht = c.cbm_ht_create(64);
    const namespace_ht = c.cbm_ht_create(64);
    if (ht == null or namespace_ht == null) {
        c.cbm_ht_free(ht);
        c.cbm_ht_free(namespace_ht);
        return null;
    }

    // Single pass: index each def by file module and by declared package.
    // JVM mixed roots (`src/main/java` + `src/main/kotlin`) share the
    // declared package, not the path-derived module prefix.
    for (0..ix(def_count)) |i| {
        pxcModuleEntryAddIndex(pxcModuleEntryGetOrCreate(ht, all_defs[i].def_module_qn), @intCast(i));
        pxcModuleEntryAddIndex(pxcModuleEntryGetOrCreate(namespace_ht, all_defs[i].namespace_name), @intCast(i));
    }

    const idx: ?*ModuleDefIndex = @ptrCast(@alignCast(c.calloc(1, @sizeOf(ModuleDefIndex))));
    if (idx == null) {
        c.cbm_ht_foreach(ht, pxcModuleEntryFreeCb, null);
        c.cbm_ht_free(ht);
        c.cbm_ht_foreach(namespace_ht, pxcModuleEntryFreeCb, null);
        c.cbm_ht_free(namespace_ht);
        return null;
    }
    idx.?.ht = ht;
    idx.?.namespace_ht = namespace_ht;
    idx.?.def_count = def_count;
    return idx;
}

pub export fn cbm_pxc_free_module_def_index(idx_c: ?*ModuleDefIndex) void {
    const idx = idx_c orelse return;
    if (idx.ht != null) {
        c.cbm_ht_foreach(idx.ht, pxcModuleEntryFreeCb, null);
        c.cbm_ht_free(idx.ht);
    }
    if (idx.namespace_ht != null) {
        c.cbm_ht_foreach(idx.namespace_ht, pxcModuleEntryFreeCb, null);
        c.cbm_ht_free(idx.namespace_ht);
    }
    c.free(idx);
}

pub export fn cbm_pxc_filter_defs_for_file(
    idx_c: ?*const ModuleDefIndex,
    all_defs: [*c]LspDef,
    caller_lang: Language,
    caller_namespace: CStr,
    own_module: CStr,
    imp_qns: [*c]const CStr,
    imp_count: c_int,
    out_count: ?*c_int,
) [*c]LspDef {
    if (out_count) |n| n.* = 0;
    const idx = idx_c orelse return null;
    const out_n = out_count orelse return null;
    if (idx.ht == null or all_defs == null or idx.def_count <= 0) return null;

    const selected = interop.callocN(bool, ix(idx.def_count));
    if (selected == null) return null;

    var total: c_int = 0;
    pxcMarkModuleDefs(idx, selected, all_defs, caller_lang, own_module, &total);
    for (0..ix(imp_count)) |i| {
        pxcMarkModuleDefs(idx, selected, all_defs, caller_lang, imp_qns[i], &total);
    }
    if (caller_lang == c.CBM_LANG_ZIG) total += pxcMarkZigAliasModules(idx, selected, all_defs);
    if (pxcIsJvmLang(caller_lang) and caller_namespace != null and caller_namespace[0] != 0 and
        idx.namespace_ht != null)
    {
        const e: ?*ModuleEntry = @ptrCast(@alignCast(c.cbm_ht_get(idx.namespace_ht, caller_namespace)));
        total += pxcMarkEntryDefs(selected, e, all_defs, caller_lang);
    }

    if (total == 0) {
        c.free(selected);
        return null;
    }

    const out = interop.mallocN(LspDef, ix(total));
    if (out == null) {
        c.free(selected);
        return null;
    }

    var n: usize = 0;
    for (0..ix(idx.def_count)) |i| {
        if (selected[i]) {
            out[n] = all_defs[i];
            n += 1;
        }
    }
    out_n.* = @intCast(n);
    c.free(selected);
    return out;
}
