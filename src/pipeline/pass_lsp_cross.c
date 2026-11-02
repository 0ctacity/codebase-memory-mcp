/*
 * pass_lsp_cross.c — Cross-file LSP type-aware call resolution pass.
 *
 * See pass_lsp_cross.h for the high-level contract. This file is the
 * pipeline glue that converts the existing per-file extraction state
 * (CBMDefinition / CBMImport / IMPORTS-edge gbuf state) into the input
 * shape each language LSP's cbm_run_X_lsp_cross expects, then merges
 * the resulting CBMResolvedCall entries back into per-file results.
 *
 * The pass is a no-op for any file whose CBMFileResult is missing or
 * whose language has no cross-file LSP entry registered (e.g. Rust /
 * Java today). Per-LSP emit functions dedup against entries already in
 * resolved_calls, so this pass is also idempotent — safe to invoke
 * multiple times if the pipeline gains a re-run path later.
 */
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/pipeline.h" // cbm_pipeline_add_file_error
#include "pipeline/pipeline_internal.h"
#include "cbm.h" // CBMLSPStatus
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/rust_lsp.h"
#include "lsp/rust_cargo.h"
#include "lsp/zig_lsp.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/compat_fs.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────── */

enum {
    PXC_MAX_FILE_BYTES_FACTOR = 100, /* same cap pass_calls.c uses for source size */
    PXC_ITOA_BUF = 16,
};

/* Format an int into a thread-local rotating buffer for log key=value emission.
 * Mirrors the itoa_log helper in pass_calls.c — kept local so passes don't
 * grow a foundation-wide formatting API just for log output. */
static const char *itoa_buf(int val) {
    static _Thread_local char bufs[PXC_ITOA_BUF][PXC_ITOA_BUF];
    static _Thread_local int slot = 0;
    char *out = bufs[slot];
    slot = (slot + 1) & (PXC_ITOA_BUF - 1);
    snprintf(out, PXC_ITOA_BUF, "%d", val);
    return out;
}

/* ── Local helpers ─────────────────────────────────────────────── */

/* True for languages whose module QN is derived from the CONTAINING DIRECTORY
 * (Java package, Go package) rather than the filename stem. MUST match the
 * extraction-side cbm_lang_module_is_dir() in internal/cbm/helpers.c so the
 * cross-file LSP caller_qn agrees with the def-node QN (the lsp_resolve join
 * keys on exact equality). */
static bool pxc_module_is_dir(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_GO;
}

/* Slurp a file into a malloc'd, NUL-terminated buffer. Mirrors the
 * read_file helper in pass_calls.c / pass_parallel.c (kept local so the
 * pipeline doesn't grow a public read-file API just for this pass). */
static char *pxc_read_file(const char *path, int *out_len) {
    FILE *f = cbm_fopen(path, "rb");
    if (!f)
        return NULL;
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (long)PXC_MAX_FILE_BYTES_FACTOR * (long)CBM_SZ_1K * (long)CBM_SZ_1K) {
        (void)fclose(f);
        return NULL;
    }
    /* +pad: tree-sitter lexer lookahead reads past EOF; keep it in-bounds */
    enum { CBM_TS_LOOKAHEAD_PAD = 16 };
    char *buf = (char *)malloc((size_t)size + CBM_TS_LOOKAHEAD_PAD);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    if (nread > (size_t)size)
        nread = (size_t)size;
    memset(buf + nread, 0, CBM_TS_LOOKAHEAD_PAD);
    *out_len = (int)nread;
    return buf;
}

/* Map a CBMDefinition.label to a CBMLSPDef.label. Per-language LSP registrars
 * only care about type-like containers (Class/Struct/Interface/Trait/Enum/Type)
 * plus Protocol/Function/Method — variables, modules, decorators, etc. are
 * skipped. Struct passes through so Rust/Go struct type-registration via the
 * cross-file LSP path is not dropped. */
static const char *pxc_map_label(const char *label) {
    if (!label)
        return NULL;
    if (cbm_label_is_type_like(label) || strcmp(label, "Protocol") == 0 ||
        strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0) {
        return label;
    }
    return NULL;
}

/* Build the embedded_types "|"-separated string from base_classes[].
 * Returns NULL when there are no bases. Allocated in the supplied arena. */
static const char *pxc_join_pipe(CBMArena *arena, const char *const *items) {
    if (!items || !items[0])
        return NULL;
    int count = 0;
    size_t total = 0;
    for (int i = 0; items[i]; i++) {
        count++;
        total += strlen(items[i]);
    }
    if (count == 0)
        return NULL;
    /* count - 1 separators + NUL. */
    size_t bufsz = total + (size_t)(count - 1) + 1;
    char *buf = (char *)cbm_arena_alloc(arena, bufsz);
    if (!buf)
        return NULL;
    char *p = buf;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        memcpy(p, items[i], n);
        p += n;
        if (i + 1 < count)
            *p++ = '|';
    }
    *p = '\0';
    return buf;
}

/* ── Cross-file base-class QN resolution ──────────────────────────
 *
 * CBMDefinition.base_classes carries the SOURCE SPELLING of each base
 * ("Base", "django.db.Model", "React.Component"): extraction strips
 * keywords and generic arguments, but it cannot know WHERE the name is
 * declared. The Python and TS cross-file registrars, however, consume
 * CBMLSPDef.embedded_types as fully-qualified names — py_lookup_attribute
 * and ts_lookup_member feed each entry straight into
 * cbm_registry_lookup_type. An unqualified spelling therefore matched
 * nothing declared in ANOTHER file: `class Child(Base)` in child.py never
 * saw Base in base.py, so a call to an inherited method through a typed
 * receiver had no member to find and fell through to the weak textual
 * cascade (where the #592/#606 guard correctly kills it).
 *
 * Resolve each base name ONCE per definition here, from exactly the
 * inputs pass_semantic uses to draw its INHERITS edge: the project
 * registry, the declaring module, and the file's import map. Two
 * properties follow. The LSP's inheritance view is the same relation the
 * graph records, so the two cannot diverge. And the binding is
 * import- or same-module-backed, not a short-name guess, so the CALLS
 * edge it enables is a supported fact that the weak-member guard keeps.
 *
 * Cost: O(defs) hash lookups + ONE import map per file. No per-call-site
 * hierarchy walk, no registry scan, no per-file registry rebuild.
 */
static bool pxc_lang_resolves_base_qns(CBMLanguage lang) {
    switch (lang) {
    case CBM_LANG_PYTHON:
    case CBM_LANG_JAVASCRIPT:
    case CBM_LANG_TYPESCRIPT:
    case CBM_LANG_TSX:
        return true;
    default:
        /* Go / JVM / C# / C++ / Rust registrars qualify their own embedded
         * types already (struct embedding, parent_class chains, impl-trait
         * provenance). Re-resolving here would fight those paths, so they
         * keep the raw join. */
        return false;
    }
}

/* True for the registry strategies that are pure short-name guesses.
 * EXPLICIT drop-list, mirroring cbm_tsjs_suppress_weak_method_match: a
 * base class bound by "some project type happens to share this name" is
 * exactly the fabricated relation #606 removed, and inheritance
 * multiplies it — every inherited member of the wrong base would become
 * a callable target. Import-, module- and suffix-aware strategies are
 * kept; anything unresolved simply retains its source spelling and the
 * behaviour that predates this resolution. */
static bool pxc_base_strategy_is_weak(const char *strategy) {
    if (!strategy || !strategy[0]) {
        return true;
    }
    return strcmp(strategy, "suffix_match") == 0 || strcmp(strategy, "unique_name") == 0 ||
           strcmp(strategy, "field_type_hint") == 0 || strcmp(strategy, "fuzzy") == 0;
}

/* Resolve one base-class spelling to a project QN. Mirrors
 * pass_semantic.c::resolve_as_class — same registry, same type-like veto —
 * then additionally rejects weak short-name strategies (see above).
 * Returns NULL when the base is not a confidently-known project type;
 * stdlib and third-party bases land here and keep their raw spelling. */
static const char *pxc_resolve_base_qn(const cbm_registry_t *reg, const char *raw,
                                       const char *module_qn, const char **imp_keys,
                                       const char **imp_vals, int imp_count) {
    if (!reg || !raw || !raw[0]) {
        return NULL;
    }
    cbm_resolution_t res = cbm_registry_resolve(reg, raw, module_qn, imp_keys, imp_vals, imp_count);
    if (!res.qualified_name || !res.qualified_name[0]) {
        return NULL;
    }
    if (pxc_base_strategy_is_weak(res.strategy)) {
        return NULL;
    }
    if (!cbm_label_is_type_like(cbm_registry_label_of(reg, res.qualified_name))) {
        return NULL;
    }
    return res.qualified_name;
}

/* pxc_join_pipe over base_classes, substituting each resolved QN for its
 * source spelling. Unresolved entries pass through verbatim so a base the
 * registry does not know keeps working exactly as before. */
static const char *pxc_join_base_qns(CBMArena *arena, const char *const *bases,
                                     const cbm_registry_t *reg, const char *module_qn,
                                     const char **imp_keys, const char **imp_vals, int imp_count) {
    if (!bases || !bases[0]) {
        return NULL;
    }
    int count = 0;
    while (bases[count]) {
        count++;
    }
    const char **resolved =
        (const char **)cbm_arena_alloc(arena, (size_t)(count + 1) * sizeof(const char *));
    if (!resolved) {
        return pxc_join_pipe(arena, bases);
    }
    for (int i = 0; i < count; i++) {
        const char *qn =
            pxc_resolve_base_qn(reg, bases[i], module_qn, imp_keys, imp_vals, imp_count);
        resolved[i] = qn ? qn : bases[i];
    }
    resolved[count] = NULL;
    return pxc_join_pipe(arena, resolved);
}

static bool pxc_is_jvm_lang(CBMLanguage lang);

static const char *pxc_last_component(const char *qn) {
    if (!qn) {
        return NULL;
    }
    const char *dot = strrchr(qn, '.');
    return dot ? dot + 1 : qn;
}

static const char *pxc_jvm_type_qn(CBMArena *arena, const char *namespace_name,
                                   const char *type_qn_or_name) {
    if (!arena || !namespace_name || !namespace_name[0] || !type_qn_or_name) {
        return type_qn_or_name;
    }
    const char *short_name = pxc_last_component(type_qn_or_name);
    if (!short_name || !short_name[0]) {
        return type_qn_or_name;
    }
    return cbm_arena_sprintf(arena, "%s.%s", namespace_name, short_name);
}

static const char *pxc_jvm_def_qn(CBMArena *arena, const CBMDefinition *src,
                                  const char *namespace_name, const char *label) {
    if (!arena || !src || !namespace_name || !namespace_name[0]) {
        return src ? src->qualified_name : NULL;
    }
    if (strcmp(label, "Method") == 0 || strcmp(label, "Function") == 0 ||
        strcmp(label, "Constructor") == 0) {
        if (src->parent_class && src->parent_class[0]) {
            return cbm_arena_sprintf(arena, "%s.%s.%s", namespace_name,
                                     pxc_last_component(src->parent_class), src->name);
        }
        return cbm_arena_sprintf(arena, "%s.%s", namespace_name, src->name);
    }
    return cbm_arena_sprintf(arena, "%s.%s", namespace_name, src->name);
}

static const char *pxc_infer_jvm_namespace(CBMArena *arena, const char *rel_path,
                                           CBMLanguage lang) {
    if (!arena || !rel_path || !pxc_is_jvm_lang(lang)) {
        return NULL;
    }
    const char *root = NULL;
    const char *lang_root = lang == CBM_LANG_KOTLIN ? "kotlin/" : "java/";
    if (strncmp(rel_path, "src/main/", 9) == 0 &&
        strncmp(rel_path + 9, lang_root, strlen(lang_root)) == 0) {
        root = rel_path + 9 + strlen(lang_root);
    } else if (strncmp(rel_path, "src/test/", 9) == 0 &&
               strncmp(rel_path + 9, lang_root, strlen(lang_root)) == 0) {
        root = rel_path + 9 + strlen(lang_root);
    } else {
        const char *needle = lang == CBM_LANG_KOTLIN ? "/kotlin/" : "/java/";
        root = strstr(rel_path, needle);
        if (root) {
            root += strlen(needle);
        } else if (strncmp(rel_path, "src/", 4) == 0) {
            root = rel_path + 4;
        } else {
            root = strstr(rel_path, "/src/");
            if (root) {
                root += strlen("/src/");
            }
        }
    }
    if (!root || !root[0]) {
        return NULL;
    }
    if (strncmp(root, "main/", 5) == 0 || strncmp(root, "test/", 5) == 0) {
        root += 5;
    }
    if (strncmp(root, "java/", 5) == 0) {
        root += 5;
    } else if (strncmp(root, "kotlin/", 7) == 0) {
        root += 7;
    }
    const char *slash = strrchr(root, '/');
    if (!slash || slash <= root) {
        return NULL;
    }
    size_t len = (size_t)(slash - root);
    char *ns = (char *)cbm_arena_alloc(arena, len + 1);
    if (!ns) {
        return NULL;
    }
    memcpy(ns, root, len);
    ns[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (ns[i] == '/') {
            ns[i] = '.';
        }
    }
    return ns;
}

static const char *pxc_import_lookup(const char *local, size_t len, const char **imp_keys,
                                     const char **imp_vals, int imp_count) {
    for (int i = 0; i < imp_count; i++) {
        if (imp_keys[i] && imp_vals[i] && strlen(imp_keys[i]) == len &&
            strncmp(imp_keys[i], local, len) == 0) {
            return imp_vals[i];
        }
    }
    return NULL;
}

/* The innermost container enclosing a container-level Zig alias (a QN prefix
 * of its parent_class, above the module) that declares `head` in this file,
 * as `<container>.<head>`. NULL when none does. */
static const char *pxc_zig_container_head(CBMArena *arena, const CBMFileResult *file,
                                          const char *scope, const char *module_qn,
                                          const char *head, size_t len) {
    size_t mlen = strlen(module_qn);
    size_t slen = strlen(scope);
    while (slen > mlen) {
        for (int i = 0; i < file->defs.count; i++) {
            const char *qn = file->defs.items[i].qualified_name;
            if (qn && strlen(qn) == slen + 1 + len && strncmp(qn, scope, slen) == 0 &&
                qn[slen] == '.' && strncmp(qn + slen + 1, head, len) == 0) {
                return cbm_arena_strdup(arena, qn);
            }
        }
        while (slen > mlen && scope[slen - 1] != '.') {
            slen--;
        }
        slen = slen > 0 ? slen - 1 : 0;
    }
    return NULL;
}

/* Zig: an alias (`const db = @import("db.zig");`, `pub const Database =
 * zova.Database;`, alias_value = the value text), at module or container
 * level, becomes a "Variable" def whose return_types is the QN it names. The
 * head resolves lexically: a declaration in an enclosing container, then the
 * declaring file's imports (`const v = @import(..)` is imported as `v`,
 * `@import("x.zig").C` as `x`); any other head is a path from the declaring
 * module. Returns -1 for anything else. */
static int pxc_build_zig_alias(CBMArena *arena, const CBMFileResult *file,
                               const CBMDefinition *src, const char *module_qn, CBMLSPDef *dst,
                               const char **imp_keys, const char **imp_vals, int imp_count) {
    const char *text = src->alias_value;
    if (!text || !text[0] || !module_qn || strpbrk(text, " \t\r\n")) {
        return -1;
    }
    const char *target = NULL;
    if (strncmp(text, "@import(\"", strlen("@import(\"")) == 0) {
        const char *path = text + strlen("@import(\"");
        const char *close = strchr(path, '"');
        if (!close || close[1] != ')') {
            return -1;
        }
        const char *rest = close + 2;
        if ((size_t)(close - path) == strlen("std") && strncmp(path, "std", 3) == 0) {
            target = cbm_arena_sprintf(arena, "std%s", rest); /* the opaque std namespace */
        } else {
            const char *local = src->name;
            size_t len = strlen(local);
            if (rest[0]) {
                local = path;
                for (const char *p = path; p < close; p++) {
                    if (*p == '/') {
                        local = p + 1;
                    }
                }
                len = (size_t)(close - local);
                if (len > 4 && strncmp(local + len - 4, ".zig", 4) == 0) {
                    len -= 4;
                }
            }
            const char *head = pxc_import_lookup(local, len, imp_keys, imp_vals, imp_count);
            target = head ? cbm_arena_sprintf(arena, "%s%s", head, rest) : NULL;
        }
    } else {
        size_t len = strcspn(text, ".");
        const char *head = src->parent_class ? pxc_zig_container_head(arena, file, src->parent_class,
                                                                      module_qn, text, len)
                                             : NULL;
        if (!head) {
            head = pxc_import_lookup(text, len, imp_keys, imp_vals, imp_count);
        }
        target = head ? cbm_arena_sprintf(arena, "%s%s", head, text + len)
                      : cbm_arena_sprintf(arena, "%s.%s", module_qn, text);
    }
    if (!target) {
        return -1;
    }
    memset(dst, 0, sizeof(*dst));
    dst->qualified_name = src->qualified_name;
    dst->short_name = src->name;
    dst->label = "Variable";
    dst->def_module_qn = module_qn;
    dst->return_types = target;
    dst->lang = CBM_LANG_ZIG;
    return 0;
}

/* Zig: a type function (`fn List(comptime T: type) type`) names the
 * containers it returns, whose members extraction scopes under its QN. The
 * resolver sees it as a type too: a "Struct" def of the same QN (its fields
 * fold in like any struct's) whose type_params lists its parameters'
 * kinds (cbm_zig_param_kinds of the signature text). */
static bool pxc_is_zig_type_fn(const CBMDefinition *src) {
    return src->label && (strcmp(src->label, "Function") == 0 || strcmp(src->label, "Method") == 0) &&
           src->return_type &&
           strcmp(src->return_type, "type") == 0 && src->qualified_name && src->name;
}

static int pxc_build_zig_type_fn(CBMArena *arena, const CBMDefinition *src, const char *module_qn,
                                 CBMLSPDef *dst) {
    const char *out = cbm_zig_param_kinds(arena, src->signature ? src->signature : "");
    if (!out) {
        return -1;
    }
    memset(dst, 0, sizeof(*dst));
    dst->qualified_name = src->qualified_name;
    dst->short_name = src->name;
    dst->label = "Struct";
    dst->def_module_qn = module_qn;
    dst->type_params = out;
    dst->lang = CBM_LANG_ZIG;
    return 0;
}

/* Convert one CBMDefinition into a CBMLSPDef. Returns 0 on success, -1
 * to skip (unsupported label or missing required field). dst gets borrowed
 * pointers into src and into `arena` for synthesised composites. */
static int pxc_build_lsp_def(CBMArena *arena, const CBMDefinition *src, const char *module_qn,
                             const char *namespace_name, CBMLanguage lang, CBMLSPDef *dst,
                             const cbm_registry_t *reg, const char **imp_keys,
                             const char **imp_vals, int imp_count) {
    const char *label = pxc_map_label(src->label);
    if (!label || !src->qualified_name || !src->name)
        return -1;
    memset(dst, 0, sizeof(*dst));
    if (pxc_is_jvm_lang(lang) && namespace_name && namespace_name[0]) {
        dst->qualified_name = pxc_jvm_def_qn(arena, src, namespace_name, label);
        dst->receiver_type = pxc_jvm_type_qn(arena, namespace_name, src->parent_class);
    } else {
        dst->qualified_name = src->qualified_name;
        dst->receiver_type = src->parent_class;
    }
    dst->short_name = src->name;
    dst->label = label;
    dst->def_module_qn = module_qn;
    dst->namespace_name = namespace_name;
    dst->is_interface = (strcmp(label, "Interface") == 0 || strcmp(label, "Protocol") == 0);
    /* Single return-type string. The per-language registrars split on '|'
     * for multi-return languages (Go); single-return languages just see one
     * piece, which is what's already stored. */
    dst->return_types = src->return_type;
    /* Languages whose cross registrars read embedded_types as QNs get their
     * bases resolved against the project registry; everyone else keeps the
     * raw source spelling their own registrar already knows how to handle. */
    dst->embedded_types = (reg && pxc_lang_resolves_base_qns(lang))
                              ? pxc_join_base_qns(arena, src->base_classes, reg, module_qn,
                                                  imp_keys, imp_vals, imp_count)
                              : pxc_join_pipe(arena, src->base_classes);
    dst->lang = lang;
    return 0;
}

/* Go and Zig: fold per-field "Field" definitions into their owning struct's
 * field_defs. extract_defs.c emits one flat CBMDefinition per struct field
 * (label "Field", parent_class = owning struct QN, name = field name,
 * return_type = raw type text). Those rows are dropped by pxc_build_lsp_def
 * (pxc_map_label excludes "Field"), so without this fold every Go struct
 * registers with zero fields and field-chain calls (h.svc.Handle) can
 * never resolve. Fields are always declared in the same file as their struct,
 * so scanning the file's own defs covers every case. Runs inside
 * cbm_pxc_collect_all_defs — one site covers both the prebuilt-registry path
 * and the per-file fallback, since both consume all_defs. */
static void pxc_fold_go_struct_fields(CBMArena *arena, const CBMFileResult *result,
                                      CBMLSPDef *defs, int start, int end) {
    if (!arena || !result || !defs || start >= end) {
        return;
    }
    for (int si = start; si < end; si++) {
        CBMLSPDef *dst = &defs[si];
        if (!dst->label || strcmp(dst->label, "Struct") != 0 || !dst->qualified_name) {
            continue;
        }
        int count = 0;
        size_t total = 0; /* "name:type" bytes; separators and NUL added below */
        for (int di = 0; di < result->defs.count; di++) {
            const CBMDefinition *fd = &result->defs.items[di];
            if (!fd->label || !fd->parent_class || !fd->name || !fd->name[0] || !fd->return_type ||
                !fd->return_type[0] || strcmp(fd->label, "Field") != 0 ||
                strcmp(fd->parent_class, dst->qualified_name) != 0) {
                continue;
            }
            total += strlen(fd->name) + 1 + strlen(fd->return_type);
            count++;
        }
        if (count == 0) {
            continue;
        }
        /* count - 1 separators + NUL. */
        size_t bufsz = total + (size_t)(count - 1) + 1;
        char *buf = (char *)cbm_arena_alloc(arena, bufsz);
        if (!buf) {
            continue;
        }
        char *p = buf;
        int written = 0;
        for (int di = 0; di < result->defs.count; di++) {
            const CBMDefinition *fd = &result->defs.items[di];
            if (!fd->label || !fd->parent_class || !fd->name || !fd->name[0] || !fd->return_type ||
                !fd->return_type[0] || strcmp(fd->label, "Field") != 0 ||
                strcmp(fd->parent_class, dst->qualified_name) != 0) {
                continue;
            }
            size_t n = strlen(fd->name);
            memcpy(p, fd->name, n);
            p += n;
            *p++ = ':';
            n = strlen(fd->return_type);
            memcpy(p, fd->return_type, n);
            p += n;
            if (written + 1 < count) {
                *p++ = '|';
            }
            written++;
        }
        *p = '\0';
        dst->field_defs = buf;
    }
}

static int pxc_build_import_map(const cbm_gbuf_t *gbuf, const char *project_name,
                                const char *rel_path, const char ***out_keys,
                                const char ***out_vals, int *out_count);
static void pxc_free_import_map(const char **keys, const char **vals, int count);

/* A file's declared namespace, or for Java/Kotlin the one inferred from its
 * source-root path, stored back on the result (in its arena) so later readers
 * of result->namespace_name see the same value. NULL when there is none. */
const char *cbm_pxc_apply_inferred_namespace(CBMFileResult *result, const cbm_file_info_t *file) {
    const char *namespace_name = result->namespace_name;
    if ((!namespace_name || !namespace_name[0]) && file->rel_path) {
        namespace_name = pxc_infer_jvm_namespace(&result->arena, file->rel_path, file->language);
        if (namespace_name && namespace_name[0]) {
            result->namespace_name = namespace_name;
        }
    }
    return namespace_name;
}

/* Re-home defs[start, end) into `arena`: they borrow strings from a loaded
 * (spilled) result that is freed right after collection. def_module_qn is
 * borrowed from def_modules[] and stays. */
static void pxc_own_def_strings(CBMArena *arena, CBMLSPDef *defs, int start, int end) {
    for (int i = start; i < end; i++) {
        CBMLSPDef *d = &defs[i];
        d->qualified_name = cbm_arena_strdup(arena, d->qualified_name);
        d->short_name = cbm_arena_strdup(arena, d->short_name);
        d->label = cbm_arena_strdup(arena, d->label);
        d->receiver_type = cbm_arena_strdup(arena, d->receiver_type);
        d->return_types = cbm_arena_strdup(arena, d->return_types);
        d->embedded_types = cbm_arena_strdup(arena, d->embedded_types);
        d->field_defs = cbm_arena_strdup(arena, d->field_defs);
        d->method_names_str = cbm_arena_strdup(arena, d->method_names_str);
        d->namespace_name = cbm_arena_strdup(arena, d->namespace_name);
        d->type_params = cbm_arena_strdup(arena, d->type_params);
    }
}

/* Collect a project-wide CBMLSPDef[] from all cached results. Returns a
 * malloc'd array (caller frees) of length *out_count. String fields are
 * borrowed from cache[i]->arena and from def_modules[i] (also borrowed);
 * defs of a spilled result (ctx->spill) are copied into owned_arena, and
 * spilled results are skipped when owned_arena is NULL. */
CBMLSPDef *cbm_pxc_collect_all_defs(const cbm_pipeline_ctx_t *ctx, CBMFileResult **cache,
                                    const cbm_file_info_t *files, int file_count,
                                    const char *project_name, char **def_modules,
                                    CBMArena *owned_arena, int *out_count) {
    cbm_result_spill_t *spill = ctx && owned_arena ? ctx->spill : NULL;
    int cap = 0;
    for (int i = 0; i < file_count; i++) {
        if (cache[i])
            cap += cache[i]->defs.count;
    }
    if (cap == 0 && !spill) {
        *out_count = 0;
        return NULL;
    }
    CBMLSPDef *defs = cap > 0 ? (CBMLSPDef *)calloc((size_t)cap, sizeof(CBMLSPDef)) : NULL;
    if (cap > 0 && !defs) {
        *out_count = 0;
        return NULL;
    }
    int idx = 0;
    for (int fi = 0; fi < file_count; fi++) {
        const int file_start = idx;
        CBMFileResult *result = cbm_result_cache_acquire(cache, spill, fi);
        if (!result)
            continue;
        const bool loaded = result != cache[fi];
        int need = result->defs.count;
        for (int di = 0; files[fi].language == CBM_LANG_ZIG && di < result->defs.count; di++) {
            need += pxc_is_zig_type_fn(&result->defs.items[di]) ? 1 : 0;
        }
        if (idx + need > cap) {
            int grown = cap * 2 > idx + need ? cap * 2 : idx + need;
            CBMLSPDef *more = (CBMLSPDef *)realloc(defs, (size_t)grown * sizeof(CBMLSPDef));
            if (!more) {
                cbm_result_cache_release(cache, fi, result);
                continue;
            }
            defs = more;
            cap = grown;
        }
        if (!def_modules[fi]) {
            def_modules[fi] = cbm_pipeline_fqn_module_dir(project_name, files[fi].rel_path,
                                                          pxc_module_is_dir(files[fi].language));
        }
        const char *namespace_name = cbm_pxc_apply_inferred_namespace(result, &files[fi]);
        /* One import map per FILE (not per def, and not per base name): the
         * cross-file base-class resolution needs the same local-name →
         * import-QN view pass_semantic uses. Both drivers have already
         * created this file's IMPORTS edges and filled the registry here.
         * Built only for the languages that consume resolved base QNs (and
         * for Zig, whose aliases resolve through it), and only when a caller
         * supplied the pipeline context. */
        const cbm_registry_t *base_reg = NULL;
        const char **imp_keys = NULL;
        const char **imp_vals = NULL;
        int imp_count = 0;
        if (ctx && ctx->registry && ctx->gbuf && pxc_lang_resolves_base_qns(files[fi].language)) {
            base_reg = ctx->registry;
        }
        if (ctx && ctx->gbuf && (base_reg || files[fi].language == CBM_LANG_ZIG)) {
            pxc_build_import_map(ctx->gbuf, project_name, files[fi].rel_path, &imp_keys,
                                 &imp_vals, &imp_count);
        }
        for (int di = 0; di < result->defs.count; di++) {
            const CBMDefinition *src = &result->defs.items[di];
            int rc = files[fi].language == CBM_LANG_ZIG && src->label &&
                             strcmp(src->label, "Variable") == 0 && src->qualified_name &&
                             src->name
                         ? pxc_build_zig_alias(&result->arena, result, src, def_modules[fi],
                                               &defs[idx], imp_keys, imp_vals, imp_count)
                         : pxc_build_lsp_def(&result->arena, src, def_modules[fi],
                                             namespace_name, files[fi].language, &defs[idx],
                                             base_reg, imp_keys, imp_vals, imp_count);
            if (rc == 0) {
                /* Zig: a function taking `anytype` carries its parameter kinds,
                 * so callers' argument types reach it (cbm_pxc_collect_zig_args). */
                if (files[fi].language == CBM_LANG_ZIG && src->signature &&
                    strstr(src->signature, "anytype") &&
                    (strcmp(defs[idx].label, "Function") == 0 ||
                     strcmp(defs[idx].label, "Method") == 0)) {
                    defs[idx].type_params = cbm_zig_param_kinds(&result->arena, src->signature);
                }
                idx++;
            }
            if (files[fi].language == CBM_LANG_ZIG && pxc_is_zig_type_fn(src) &&
                pxc_build_zig_type_fn(&result->arena, src, def_modules[fi], &defs[idx]) == 0) {
                idx++;
            }
        }
        pxc_free_import_map(imp_keys, imp_vals, imp_count); /* NULL-safe */
        if (files[fi].language == CBM_LANG_GO || files[fi].language == CBM_LANG_ZIG) {
            pxc_fold_go_struct_fields(&result->arena, result, defs, file_start, idx);
        }
        if (loaded) {
            pxc_own_def_strings(owned_arena, defs, file_start, idx);
        }
        cbm_result_cache_release(cache, fi, result);
    }
    *out_count = idx;
    return defs;
}

/* Build per-file import map (local_name -> resolved module QN) from gbuf
 * IMPORTS edges. Mirrors build_import_map() in pass_parallel.c. Returns 0
 * with *out_count = 0 when the file has no IMPORTS edges. Caller frees keys
 * with pxc_free_import_map. */
static int pxc_build_import_map(const cbm_gbuf_t *gbuf, const char *project_name,
                                const char *rel_path, const char ***out_keys,
                                const char ***out_vals, int *out_count) {
    *out_keys = NULL;
    *out_vals = NULL;
    *out_count = 0;

    char *file_qn = cbm_pipeline_fqn_compute(project_name, rel_path, "__file__");
    if (!file_qn)
        return 0;
    const cbm_gbuf_node_t *file_node = cbm_gbuf_find_by_qn(gbuf, file_qn);
    free(file_qn);
    if (!file_node)
        return 0;

    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    int rc =
        cbm_gbuf_find_edges_by_source_type(gbuf, file_node->id, "IMPORTS", &edges, &edge_count);
    if (rc != 0 || edge_count == 0)
        return 0;

    const char **keys = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    const char **vals = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return 0;
    }
    int count = 0;
    for (int i = 0; i < edge_count; i++) {
        const cbm_gbuf_edge_t *e = edges[i];
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, e->target_id);
        if (!target || !e->properties_json)
            continue;
        const char *start = strstr(e->properties_json, "\"local_name\":\"");
        if (!start)
            continue;
        start += strlen("\"local_name\":\"");
        const char *end = strchr(start, '"');
        if (!end || end <= start)
            continue;
        size_t n = (size_t)(end - start);
        char *local = (char *)malloc(n + 1);
        if (!local)
            continue;
        memcpy(local, start, n);
        local[n] = '\0';
        keys[count] = local;
        vals[count] = target->qualified_name; /* borrowed from gbuf */
        count++;
    }
    *out_keys = keys;
    *out_vals = vals;
    *out_count = count;
    return 0;
}

static void pxc_free_import_map(const char **keys, const char **vals, int count) {
    if (keys) {
        for (int i = 0; i < count; i++)
            free((void *)keys[i]);
        free((void *)keys);
    }
    free((void *)vals); /* vals strings borrowed from gbuf — don't free elements */
}

/* Detect TS dialect flags from a relative path. */
void cbm_pxc_ts_modes(CBMLanguage lang, const char *rel_path, bool *out_js, bool *out_jsx,
                      bool *out_dts) {
    *out_js = (lang == CBM_LANG_JAVASCRIPT);
    *out_jsx = (lang == CBM_LANG_TSX);
    *out_dts = false;
    if (!rel_path)
        return;
    size_t rl = strlen(rel_path);
    if (lang == CBM_LANG_JAVASCRIPT && rl >= 4 && strcmp(rel_path + rl - 4, ".jsx") == 0) {
        *out_jsx = true;
    }
    if (lang == CBM_LANG_TYPESCRIPT && rl >= 5 && strcmp(rel_path + rl - 5, ".d.ts") == 0) {
        *out_dts = true;
    }
}

/* Returns true when this language has a cross-file LSP wired up. */
bool cbm_pxc_has_cross_lsp(CBMLanguage lang) {
    switch (lang) {
    case CBM_LANG_GO:
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA:
    case CBM_LANG_PYTHON:
    case CBM_LANG_JAVASCRIPT:
    case CBM_LANG_TYPESCRIPT:
    case CBM_LANG_TSX:
    case CBM_LANG_PHP:
    case CBM_LANG_CSHARP: /* tier-2 prebuilt registry path (pass_parallel.c) */
    case CBM_LANG_JAVA:   /* fallback cbm_pxc_run_one path */
    case CBM_LANG_KOTLIN: /* fallback cbm_pxc_run_one path */
    case CBM_LANG_RUST:   /* fallback cbm_pxc_run_one path (manifest-aware) */
    case CBM_LANG_ZIG:    /* fallback cbm_pxc_run_one path */
        return true;
    default:
        return false;
    }
}

/* Append cross-file results from `src_out` (allocated in a scratch arena
 * about to be destroyed) into `dst_calls` (lives in cache_entry->arena),
 * copying every string field into dst_arena. Skips entries whose
 * (caller_qn, callee_qn) is already present — avoids inflating the array with
 * cross-file duplicates of per-file LSP output.
 *
 * Dedup uses a hash set keyed on "caller\x1fcallee", giving O(1) membership.
 * The previous linear strcmp scan made each append O(n), so a file that
 * resolved very many cross-calls turned the whole append into O(n^2) and could
 * peg a core for minutes (observed: an index hung in pxc_append_results/strcmp).
 * The key strings live in a scratch arena that is destroyed after the table. */
/* Invocation rows dedup on (caller, callee). A reference row is tied to one
 * source occurrence, so its site is part of the key and it never collides
 * with an invocation of the same callee. An invocation row with a site span
 * proves one call site (Zig), so its site is part of the key too. */
static char *pxc_result_key(CBMArena *keys, const CBMResolvedCall *rc) {
    if (rc->kind == CBM_RESOLVED_CALL_REFERENCE) {
        return cbm_arena_sprintf(keys, "%s\x1f%s\x1fR%u:%u", rc->caller_qn, rc->callee_qn,
                                 rc->site_start_byte, rc->site_end_byte);
    }
    if (rc->site_end_byte > rc->site_start_byte) {
        return cbm_arena_sprintf(keys, "%s\x1f%s\x1fI%u", rc->caller_qn, rc->callee_qn,
                                 rc->site_start_byte);
    }
    return cbm_arena_sprintf(keys, "%s\x1f%s", rc->caller_qn, rc->callee_qn);
}

static void pxc_append_results(CBMArena *dst_arena, CBMResolvedCallArray *dst_calls,
                               const CBMResolvedCallArray *src_out) {
    if (!dst_calls || !src_out)
        return;

    CBMArena keys;
    cbm_arena_init(&keys);
    CBMHashTable *seen = cbm_ht_create((uint32_t)(dst_calls->count + src_out->count + 1));

    for (int i = 0; i < dst_calls->count; i++) {
        const CBMResolvedCall *rc = &dst_calls->items[i];
        if (rc->caller_qn && rc->callee_qn) {
            char *k = pxc_result_key(&keys, rc);
            if (k) {
                cbm_ht_set(seen, k, (void *)1);
            }
        }
    }

    for (int j = 0; j < src_out->count; j++) {
        const CBMResolvedCall *src = &src_out->items[j];
        if (!src->caller_qn || !src->callee_qn)
            continue;
        char *k = pxc_result_key(&keys, src);
        if (k && cbm_ht_has(seen, k))
            continue;
        if (k) {
            cbm_ht_set(seen, k, (void *)1);
        }
        CBMResolvedCall dst;
        memset(&dst, 0, sizeof(dst));
        dst.caller_qn = cbm_arena_strdup(dst_arena, src->caller_qn);
        dst.callee_qn = cbm_arena_strdup(dst_arena, src->callee_qn);
        dst.strategy = src->strategy ? cbm_arena_strdup(dst_arena, src->strategy) : NULL;
        dst.confidence = src->confidence;
        dst.reason = src->reason ? cbm_arena_strdup(dst_arena, src->reason) : NULL;
        dst.kind = src->kind;
        dst.site_start_byte = src->site_start_byte;
        dst.site_end_byte = src->site_end_byte;
        cbm_resolvedcall_push(dst_calls, dst_arena, dst);
    }

    cbm_ht_free(seen);
    cbm_arena_destroy(&keys);
}

/* ── Rust workspace manifest (Cargo.toml) for cross-CRATE resolution ──
 *
 * cbm_pxc_run_one's signature is shared with the parallel pass
 * (pass_parallel.c) and cannot grow a manifest parameter without touching
 * that file. We therefore pass the parsed workspace manifest to the Rust
 * cross-file resolver through a file-static borrowed pointer that the
 * sequential driver (cbm_pipeline_pass_lsp_cross, below) sets up once per
 * pass run from the project's root Cargo.toml. The manifest's strings are
 * owned by `g_pxc_rust_manifest_arena`; the pointer is borrowed (NULL when
 * the project has no Cargo.toml — single-crate / non-workspace projects,
 * where in-file resolution needs no workspace metadata). */
static _Thread_local const CBMCargoManifest *g_pxc_rust_manifest = NULL;

void cbm_pxc_set_rust_manifest(const CBMCargoManifest *m) {
    g_pxc_rust_manifest = m;
}

const struct CBMCargoManifest *cbm_pxc_get_rust_manifest(void) {
    return g_pxc_rust_manifest;
}

/* Convert a CBMLSPDef array (the pipeline's lingua franca, go_lsp.h:73)
 * into a CBMRustLSPDef array (rust_lsp.h) inside `arena`. The two structs
 * share their first 9 string fields; CBMRustLSPDef adds `trait_qn` before
 * `is_interface` whereas CBMLSPDef has `is_interface` followed by `lang`,
 * so a memcpy is unsafe — copy field-by-field. trait_qn is left NULL
 * because the pipeline's collect-all-defs step does not carry the
 * impl-Trait-for-Type linkage; the resolver still recovers trait dispatch
 * from the in-file walk (the cross-file path only needs receiver_type). */
static CBMRustLSPDef *pxc_lspdefs_to_rust(CBMArena *arena, const CBMLSPDef *defs, int def_count) {
    if (!defs || def_count <= 0)
        return NULL;
    CBMRustLSPDef *out =
        (CBMRustLSPDef *)cbm_arena_alloc(arena, (size_t)def_count * sizeof(CBMRustLSPDef));
    if (!out)
        return NULL;
    for (int i = 0; i < def_count; i++) {
        out[i].qualified_name = defs[i].qualified_name;
        out[i].short_name = defs[i].short_name;
        out[i].label = defs[i].label;
        out[i].receiver_type = defs[i].receiver_type;
        out[i].def_module_qn = defs[i].def_module_qn;
        out[i].return_types = defs[i].return_types;
        out[i].embedded_types = defs[i].embedded_types;
        out[i].field_defs = defs[i].field_defs;
        out[i].method_names_str = defs[i].method_names_str;
        out[i].trait_qn = NULL;
        out[i].is_interface = defs[i].is_interface;
    }
    return out;
}

/* Run cross-file LSP for a single file inside a scratch arena that gets
 * freed when the call returns. The LSP would otherwise allocate a fresh
 * type registry + stdlib + all project defs into the supplied arena, and
 * that adds up to O(N×project_size) memory if we used cache[i]->arena
 * directly across N files (test_incremental.c saw 3.5 GB peak on a
 * 1100-file repo before this fix). Output gets copied into the file's own
 * arena and merged into result->resolved_calls. */
void cbm_pxc_run_one(CBMLanguage lang, CBMFileResult *r, const char *source, int source_len,
                     const char *module_qn, CBMLSPDef *defs, int def_count, const char **imp_names,
                     const char **imp_qns, int imp_count) {
    TSTree *tree = r->cached_tree; /* may be NULL — LSP re-parses then */

    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    switch (lang) {
    case CBM_LANG_GO:
        cbm_run_go_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                             imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA: {
        bool cpp_mode = (lang != CBM_LANG_C);
        /* C/C++ cross LSP takes include_paths/include_ns_qns instead of
         * imports — the existing pipeline doesn't carry C-style include
         * resolution as a separate map, so pass NULL/0 and let the LSP
         * fall back to its own #include scan. */
        CBMLSPStatus status =
            cbm_run_c_lsp_cross_status(&scratch, source, source_len, module_qn, cpp_mode, defs,
                                       def_count, NULL, NULL, 0, tree, &out);
        if (status != CBM_LSP_COMPLETE) {
            cbm_c_lsp_record_failure(r, status);
        }
        break;
    }
    case CBM_LANG_PYTHON: {
        CBMLSPStatus status =
            cbm_run_py_lsp_cross_status(&scratch, source, source_len, module_qn, defs, def_count,
                                        imp_names, imp_qns, imp_count, tree, &out);
        if (status != CBM_LSP_COMPLETE) {
            cbm_py_lsp_record_failure(r, status);
        }
        break;
    }
    case CBM_LANG_PHP:
        cbm_run_php_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                              imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_JAVA:
        cbm_run_java_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                               imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_KOTLIN:
        cbm_run_kotlin_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count,
                                 imp_names, imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_RUST: {
        /* The Rust resolver wants CBMRustLSPDef (rust_lsp.h), not the
         * pipeline's CBMLSPDef — the structs share their first 9 fields
         * but diverge after, so convert into the scratch arena. The
         * workspace manifest (set once by the sequential driver) lets
         * `crate_a::foo` route across the crate boundary (#56). */
        CBMRustLSPDef *rdefs = pxc_lspdefs_to_rust(&scratch, defs, def_count);
        cbm_run_rust_lsp_cross_with_manifest(&scratch, source, source_len, module_qn, rdefs,
                                             def_count, imp_names, imp_qns, imp_count, tree,
                                             g_pxc_rust_manifest, &out);
        break;
    }
    case CBM_LANG_ZIG:
        cbm_run_zig_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                              imp_qns, imp_count, tree, &out);
        break;
    default:
        break;
    }

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

/* cbm_pxc_run_one for Zig with the project's `anytype` argument types (args)
 * and, when collect is set, collecting them instead: the file's resolved
 * calls are then discarded. */
static void pxc_run_one_zig(CBMFileResult *r, const char *source, int source_len,
                            const char *module_qn, CBMLSPDef *defs, int def_count,
                            const char **imp_names, const char **imp_qns, int imp_count,
                            const CBMZigArgType *args, int arg_count,
                            CBMZigArgTypeArray *collect) {
    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross_ex(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                             imp_qns, imp_count, r->cached_tree, &out, args, arg_count, collect);
    if (!collect) {
        pxc_append_results(&r->arena, &r->resolved_calls, &out);
    }
    cbm_arena_destroy(&scratch);
}

/* Variant of cbm_pxc_run_one for TS/JS/JSX/TSX with explicit dialect
 * flags. Same scratch-arena lifecycle as cbm_pxc_run_one. */
void cbm_pxc_run_one_ts(CBMFileResult *r, const char *source, int source_len, const char *module_qn,
                        CBMLSPDef *defs, int def_count, const char **imp_names,
                        const char **imp_qns, int imp_count, bool js_mode, bool jsx_mode,
                        bool dts_mode) {
    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    cbm_run_ts_lsp_cross(&scratch, source, source_len, module_qn, js_mode, jsx_mode, dts_mode, defs,
                         def_count, imp_names, imp_qns, imp_count, r->cached_tree, &out);

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

/* Parse the project's root Cargo.toml (if present) into `out_m`, using
 * `marena` for the manifest's owned strings. Returns true when a manifest
 * was parsed (a workspace root or any [package]/[dependencies]); false when
 * there is no readable Cargo.toml, leaving *out_m untouched. The resulting
 * manifest feeds cross-CRATE Rust resolution (#56): its [workspace].members
 * map lets `crate_a::foo` route to the member crate's def. */
/* Per-file cross-LSP dispatch, shared by the PARALLEL resolve worker and the
 * SEQUENTIAL driver. One code path = one semantics: filter the global defs
 * down to the file's own+imported modules via the module-def index, resolve
 * through the shared prebuilt registry when the language has one (per-file
 * OVERLAY pattern — no registry build, no finalize), and only fall back to
 * the per-file registry build (with the FILTERED defs) for languages without
 * a shared-registry variant. Before this helper existed the sequential
 * driver fed the FULL def list into full per-file registry builds —
 * O(files x defs), which ground an 81k-file TS corpus for hours.
 *
 * `rust_shared_get` supplies the lazily-built shared Rust all-defs registry
 * (the parallel resolver owns its once-guard); NULL means "no shared rust
 * registry available" and rust NULL-filter files take the per-file build. */
/* Per-file registry-build cost counters (upstream cffb4e37), logged by
 * pass_parallel at the end of resolve. */
static _Atomic uint64_t g_pxc_defs_registered = 0;
static _Atomic uint64_t g_pxc_build_files = 0;
static _Atomic uint64_t g_pxc_filter_files = 0;
static _Atomic uint64_t g_pxc_filter_failed = 0;

void cbm_pxc_filter_stats(uint64_t *defs_registered, uint64_t *build_files, uint64_t *filter_files,
                          uint64_t *filter_failed) {
    *defs_registered = atomic_load_explicit(&g_pxc_defs_registered, memory_order_relaxed);
    *build_files = atomic_load_explicit(&g_pxc_build_files, memory_order_relaxed);
    *filter_files = atomic_load_explicit(&g_pxc_filter_files, memory_order_relaxed);
    *filter_failed = atomic_load_explicit(&g_pxc_filter_failed, memory_order_relaxed);
}

void cbm_pxc_filter_stats_reset(void) {
    atomic_store_explicit(&g_pxc_defs_registered, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pxc_build_files, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pxc_filter_files, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pxc_filter_failed, 0, memory_order_relaxed);
}

/* imp_vals plus every dotted prefix of each QN in the types bound to
 * module_qn's `anytype` parameters (in arena): the modules among them hold
 * the defs a bound walk resolves against. */
static const char **pxc_zig_arg_modules(CBMArena *arena, const CBMCrossLspRegistries *r,
                                        const char *module_qn, const char **imp_vals,
                                        int imp_count, int *out_count) {
    size_t mlen = strlen(module_qn);
    int cap = imp_count;
    for (int i = 0; i < r->zig_arg_count; i++) {
        const char *q = r->zig_args[i].callee_qn;
        if (strncmp(q, module_qn, mlen) == 0 && q[mlen] == '.') {
            for (const char *t = r->zig_args[i].type; *t; t++) {
                cap += *t == '.' ? 1 : 0;
            }
        }
    }
    const char **out = (const char **)cbm_arena_alloc(arena, (size_t)(cap + 1) * sizeof(char *));
    if (!out) {
        *out_count = imp_count;
        return imp_vals;
    }
    int n = 0;
    for (int i = 0; i < imp_count; i++) {
        out[n++] = imp_vals[i];
    }
    for (int i = 0; i < r->zig_arg_count; i++) {
        const char *q = r->zig_args[i].callee_qn;
        if (strncmp(q, module_qn, mlen) != 0 || q[mlen] != '.') {
            continue;
        }
        const char *t = r->zig_args[i].type;
        size_t start = 0;
        for (size_t j = 0;; j++) {
            char ch = t[j];
            bool qn_char = ch && !strchr("[]()*?, ", ch);
            if (ch == '.' && j > start && n < cap) {
                out[n++] = cbm_arena_strndup(arena, t + start, j - start);
            }
            if (!qn_char) {
                start = j + 1;
            }
            if (!ch) {
                break;
            }
        }
    }
    *out_count = n;
    return out;
}

void cbm_pxc_dispatch_file(CBMLanguage lang, CBMFileResult *result, const char *source,
                           int source_len, const char *rel, const char *def_module,
                           const CBMCrossLspRegistries *cross_registries,
                           const CBMModuleDefIndex *module_def_index, CBMLSPDef *all_defs,
                           int all_def_count, const char **imp_keys, const char **imp_vals,
                           int imp_count, CBMTypeRegistry *(*rust_shared_get)(void *),
                           void *rust_shared_ctx) {
    if (!result) {
        return;
    }
    bool used_prebuilt = false;
    CBMTypeRegistry *prebuilt =
        cross_registries ? cbm_pxc_registry_for_lang(cross_registries, lang) : NULL;
    if (prebuilt) {
        switch (lang) {
        case CBM_LANG_GO:
            /* Tier 3 (metadata-driven): pure lookup over the Tier-1
             * lsp_unresolved entries — no parse, no AST walk. Then the
             * AST walk on the shared Tier-2 registry (mirroring every
             * other language) so NAMED receivers evaluated against
             * project-wide defs also resolve. The walk variant below is
             * read-only — the sealed registry is safe for parallel
             * workers. */
            cbm_go_fast_resolve_qualified_calls(result, prebuilt, imp_keys, imp_vals, imp_count);
            cbm_run_go_lsp_cross_with_registry(&result->arena, source, source_len, def_module,
                                               prebuilt, imp_keys, imp_vals, imp_count,
                                               result->cached_tree, &result->resolved_calls);
            used_prebuilt = true;
            break;
        case CBM_LANG_PYTHON: {
            CBMLSPStatus status = cbm_run_py_lsp_cross_with_registry_status(
                &result->arena, source, source_len, def_module, prebuilt, imp_keys, imp_vals,
                imp_count, result->cached_tree, &result->resolved_calls);
            if (status != CBM_LSP_COMPLETE) {
                cbm_py_lsp_record_failure(result, status);
            }
            used_prebuilt = true;
            break;
        }
        case CBM_LANG_C:
        case CBM_LANG_CPP:
        case CBM_LANG_CUDA: {
            CBMLSPStatus status = cbm_run_c_lsp_cross_with_registry_status(
                &result->arena, source, source_len, def_module, (lang != CBM_LANG_C), prebuilt,
                imp_keys, imp_vals, imp_count, result->cached_tree, &result->resolved_calls);
            if (status != CBM_LSP_COMPLETE) {
                cbm_c_lsp_record_failure(result, status);
            }
            used_prebuilt = true;
            break;
        }
        case CBM_LANG_CSHARP:
            cbm_run_cs_lsp_cross_with_registry(&result->arena, source, source_len, def_module,
                                               prebuilt, imp_vals, imp_count, result->cached_tree,
                                               &result->resolved_calls);
            used_prebuilt = true;
            break;
        case CBM_LANG_JAVASCRIPT:
        case CBM_LANG_TYPESCRIPT:
        case CBM_LANG_TSX: {
            /* TS: per-file OVERLAY chained to the shared base. Filter to
             * own+imports so the overlay builder can pick out own-module
             * defs without scanning the whole project. */
            bool js;
            bool jsx;
            bool dts;
            cbm_pxc_ts_modes(lang, rel, &js, &jsx, &dts);
            CBMLSPDef *ts_defs = all_defs;
            int ts_def_count = all_def_count;
            CBMLSPDef *ts_filtered = NULL;
            if (module_def_index) {
                int fc = 0;
                ts_filtered = cbm_pxc_filter_defs_for_file(module_def_index, all_defs, lang,
                                                           result->namespace_name, def_module,
                                                           imp_vals, imp_count, &fc);
                if (ts_filtered) {
                    ts_defs = ts_filtered;
                    ts_def_count = fc;
                }
            }
            cbm_run_ts_lsp_cross_with_registry(&result->arena, source, source_len, def_module, js,
                                               jsx, dts, prebuilt, ts_defs, ts_def_count, imp_keys,
                                               imp_vals, imp_count, result->cached_tree,
                                               &result->resolved_calls);
            free(ts_filtered);
            used_prebuilt = true;
            break;
        }
        /* PHP falls through to the per-file build path below until its
         * overlay variant lands. */
        default:
            break;
        }
    }

    if (used_prebuilt) {
        return;
    }
    /* Fallback: gopls per-file filter + per-file registry build. RUST is
     * exempt from the module filter: its resolution is Cargo-manifest-aware
     * and a `crate_a::foo` reference routes to defs in ANOTHER workspace
     * crate — a module that is in neither own_module nor the import map, so
     * the filter starves cross-crate resolution (#56 repro red). Rust
     * therefore always resolves against the FULL def universe: the lazily
     * built shared registry when available, else a full per-file build. */
    CBMLSPDef *filtered = NULL;
    CBMLSPDef *file_defs = all_defs;
    int file_def_count = all_def_count;
    /* Zig: a function walked with an `anytype` parameter bound needs the defs
     * of the bound types' modules too, which it need not import. */
    const char **filter_qns = imp_vals;
    int filter_count = imp_count;
    CBMArena zig_arena;
    bool zig_arena_live = false;
    if (lang == CBM_LANG_ZIG && cbm_pxc_zig_args_in_module(cross_registries, def_module)) {
        cbm_arena_init(&zig_arena);
        zig_arena_live = true;
        filter_qns = pxc_zig_arg_modules(&zig_arena, cross_registries, def_module, imp_vals,
                                         imp_count, &filter_count);
    }
    if (module_def_index && lang != CBM_LANG_RUST) {
        int filtered_count = 0;
        filtered =
            cbm_pxc_filter_defs_for_file(module_def_index, all_defs, lang, result->namespace_name,
                                         def_module, filter_qns, filter_count, &filtered_count);
        if (filtered) {
            file_defs = filtered;
            file_def_count = filtered_count;
        }
        atomic_fetch_add_explicit(&g_pxc_filter_files, 1, memory_order_relaxed);
        if (!filtered) {
            atomic_fetch_add_explicit(&g_pxc_filter_failed, 1, memory_order_relaxed);
        }
    }
    /* Per-file registry build cost is driven by this count. */
    atomic_fetch_add_explicit(&g_pxc_defs_registered, (uint64_t)file_def_count,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pxc_build_files, 1, memory_order_relaxed);
    if (lang == CBM_LANG_RUST) {
        CBMTypeRegistry *shared = rust_shared_get ? rust_shared_get(rust_shared_ctx) : NULL;
        if (shared) {
            cbm_run_rust_lsp_cross_with_registry(&result->arena, source, source_len, def_module,
                                                 shared, imp_keys, imp_vals, imp_count,
                                                 result->cached_tree, cbm_pxc_get_rust_manifest(),
                                                 &result->resolved_calls,
                                                 /*result=*/NULL);
        } else {
            cbm_pxc_run_one(lang, result, source, source_len, def_module, file_defs, file_def_count,
                            imp_keys, imp_vals, imp_count);
        }
    } else if (lang == CBM_LANG_JAVASCRIPT || lang == CBM_LANG_TYPESCRIPT || lang == CBM_LANG_TSX) {
        bool js;
        bool jsx;
        bool dts;
        cbm_pxc_ts_modes(lang, rel, &js, &jsx, &dts);
        cbm_pxc_run_one_ts(result, source, source_len, def_module, file_defs, file_def_count,
                           imp_keys, imp_vals, imp_count, js, jsx, dts);
    } else if (lang == CBM_LANG_ZIG) {
        pxc_run_one_zig(result, source, source_len, def_module, file_defs, file_def_count,
                        imp_keys, imp_vals, imp_count,
                        cross_registries ? cross_registries->zig_args : NULL,
                        cross_registries ? cross_registries->zig_arg_count : 0, NULL);
    } else {
        cbm_pxc_run_one(lang, result, source, source_len, def_module, file_defs, file_def_count,
                        imp_keys, imp_vals, imp_count);
    }
    free(filtered);
    if (zig_arena_live) {
        cbm_arena_destroy(&zig_arena);
    }
}

static int pxc_zig_arg_cmp(const void *a, const void *b) {
    const CBMZigArgType *x = (const CBMZigArgType *)a;
    const CBMZigArgType *y = (const CBMZigArgType *)b;
    int c = strcmp(x->callee_qn, y->callee_qn);
    if (c == 0) {
        c = x->param - y->param;
    }
    return c != 0 ? c : strcmp(x->type, y->type);
}

CBMZigArgType *cbm_pxc_collect_zig_args(const cbm_pipeline_ctx_t *ctx, CBMFileResult **cache,
                                        const cbm_file_info_t *files, int file_count,
                                        char *const *def_modules,
                                        const CBMModuleDefIndex *module_def_index,
                                        CBMLSPDef *all_defs, int def_count, CBMArena *arena,
                                        int *out_count) {
    *out_count = 0;
    bool any = false;
    for (int i = 0; i < def_count && !any; i++) {
        any = all_defs[i].lang == CBM_LANG_ZIG && all_defs[i].type_params &&
              strchr(all_defs[i].type_params, '*') && all_defs[i].label &&
              (strcmp(all_defs[i].label, "Function") == 0 ||
               strcmp(all_defs[i].label, "Method") == 0);
    }
    if (!any || !ctx || !cache || !def_modules) {
        return NULL;
    }
    CBMZigArgTypeArray collect = {NULL, 0, 0, arena};
    for (int fi = 0; fi < file_count; fi++) {
        if (files[fi].language != CBM_LANG_ZIG || !def_modules[fi]) {
            continue;
        }
        CBMFileResult *result = cbm_result_cache_acquire(cache, ctx->spill, fi);
        if (!result) {
            continue;
        }
        char *owned = NULL;
        const char *source = result->source;
        int source_len = result->source_len;
        if ((!source || source_len <= 0) && result->calls.count > 0) {
            owned = pxc_read_file(files[fi].path, &source_len);
            source = owned;
        }
        if (source && source_len > 0 && result->calls.count > 0) {
            const char **imp_keys = NULL;
            const char **imp_vals = NULL;
            int imp_count = 0;
            pxc_build_import_map(ctx->gbuf, ctx->project_name, files[fi].rel_path, &imp_keys,
                                 &imp_vals, &imp_count);
            int fc = 0;
            CBMLSPDef *filtered =
                module_def_index ? cbm_pxc_filter_defs_for_file(
                                       module_def_index, all_defs, CBM_LANG_ZIG,
                                       result->namespace_name, def_modules[fi], imp_vals,
                                       imp_count, &fc)
                                 : NULL;
            pxc_run_one_zig(result, source, source_len, def_modules[fi],
                            filtered ? filtered : all_defs, filtered ? fc : def_count, imp_keys,
                            imp_vals, imp_count, NULL, 0, &collect);
            free(filtered);
            pxc_free_import_map(imp_keys, imp_vals, imp_count);
        }
        free(owned);
        cbm_result_cache_release(cache, fi, result);
    }
    if (collect.count == 0) {
        free(collect.items);
        return NULL;
    }
    qsort(collect.items, (size_t)collect.count, sizeof(CBMZigArgType), pxc_zig_arg_cmp);
    int n = 1;
    for (int i = 1; i < collect.count; i++) {
        if (pxc_zig_arg_cmp(&collect.items[i], &collect.items[n - 1]) != 0) {
            collect.items[n++] = collect.items[i];
        }
    }
    cbm_log_info("lsp_cross.zig_anytype", "arg_types", itoa_buf(n));
    *out_count = n;
    return collect.items;
}

bool cbm_pxc_zig_args_in_module(const CBMCrossLspRegistries *r, const char *module_qn) {
    size_t n = module_qn ? strlen(module_qn) : 0;
    for (int i = 0; r && n > 0 && i < r->zig_arg_count; i++) {
        const char *q = r->zig_args[i].callee_qn;
        if (strncmp(q, module_qn, n) == 0 && q[n] == '.') {
            return true;
        }
    }
    return false;
}

bool cbm_pxc_build_rust_manifest(const cbm_pipeline_ctx_t *ctx, CBMArena *marena,
                                 CBMCargoManifest *out_m) {
    if (!ctx || !ctx->repo_path || !marena || !out_m)
        return false;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/Cargo.toml", ctx->repo_path);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    int toml_len = 0;
    char *toml = pxc_read_file(path, &toml_len);
    if (!toml || toml_len <= 0) {
        free(toml);
        return false;
    }
    memset(out_m, 0, sizeof(*out_m));
    cbm_cargo_parse(marena, toml, toml_len, out_m);
    free(toml); /* cargo parser copies into marena */
    return true;
}

int cbm_pipeline_pass_lsp_cross(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                                int file_count, CBMFileResult **cache) {
    if (!ctx || !files || file_count <= 0 || !cache)
        return 0;

    cbm_log_info("pass.start", "pass", "lsp_cross", "files", itoa_buf(file_count));

    /* Build the Rust workspace manifest once (only when the project has at
     * least one Rust file, to avoid an unconditional Cargo.toml read).
     * The manifest's strings live in `cargo_arena`; the resolver borrows
     * the pointer through the file-static set below. */
    bool have_rust = false;
    for (int i = 0; i < file_count; i++) {
        if (cache[i] && files[i].language == CBM_LANG_RUST) {
            have_rust = true;
            break;
        }
    }
    CBMArena cargo_arena;
    CBMCargoManifest cargo_manifest;
    bool have_manifest = false;
    if (have_rust) {
        cbm_arena_init(&cargo_arena);
        have_manifest = cbm_pxc_build_rust_manifest(ctx, &cargo_arena, &cargo_manifest);
        cbm_pxc_set_rust_manifest(have_manifest ? &cargo_manifest : NULL);
    }

    /* Per-file module QN cache so we don't recompute it once per def + once
     * per call. cbm_pipeline_fqn_module mallocs; freed at end. */
    char **def_modules = (char **)calloc((size_t)file_count, sizeof(char *));
    if (!def_modules) {
        cbm_log_error("pass.err", "pass", "lsp_cross", "phase", "alloc");
        return 0;
    }

    int def_count = 0;
    CBMLSPDef *all_defs = cbm_pxc_collect_all_defs(ctx, cache, files, file_count,
                                                   ctx->project_name, def_modules, NULL,
                                                   &def_count);

    /* Shared prepare (mirrors run_parallel_pipeline): inverted module-def
     * index + per-language shared registries, built ONCE for the whole pass.
     * The per-file loop below then dispatches through the SAME helper the
     * parallel resolve worker uses — previously this driver handed the FULL
     * def list to full per-file registry builds (O(files x defs); the
     * ms-typescript sequential crawl). The registries live in the
     * CALLER-OWNED ctx->seq_cross_arena: resolved_calls may borrow registry
     * strings that the later calls pass still reads, so the arena must
     * outlive this pass (run_sequential_pipeline destroys it after all
     * passes; freeing here was a pass_calls use-after-free). */
    CBMModuleDefIndex *module_def_index =
        all_defs ? cbm_pxc_build_module_def_index(all_defs, def_count) : NULL;
    CBMCrossLspRegistries cross_registries = {0};
    if (all_defs) {
        CBMArena *xa = &ctx->seq_cross_arena;
        if (!ctx->seq_cross_arena_live) {
            cbm_arena_init(xa);
            ctx->seq_cross_arena_live = true;
        }
        cross_registries.go = cbm_go_build_cross_registry(xa, all_defs, def_count);
        cross_registries.python = cbm_py_build_cross_registry(xa, all_defs, def_count);
        cross_registries.c = cbm_c_build_cross_registry(xa, all_defs, def_count);
        cross_registries.cs = cbm_cs_build_cross_registry(xa, all_defs, def_count);
        cross_registries.ts = cbm_ts_build_cross_registry(xa, all_defs, def_count);
        cross_registries.zig_args = cbm_pxc_collect_zig_args(
            ctx, cache, files, file_count, def_modules, module_def_index, all_defs, def_count, xa,
            &cross_registries.zig_arg_count);
    }

    int processed = 0;
    int skipped_no_lsp = 0;
    int skipped_no_source = 0;
    int per_lang_calls = 0;

    for (int i = 0; i < file_count; i++) {
        if (!cache[i])
            continue;
        CBMLanguage lang = files[i].language;
        if (!cbm_pxc_has_cross_lsp(lang)) {
            skipped_no_lsp++;
            continue;
        }

        int source_len = 0;
        char *source = pxc_read_file(files[i].path, &source_len);
        if (!source || source_len <= 0) {
            free(source);
            skipped_no_source++;
            continue;
        }

        if (!def_modules[i]) {
            def_modules[i] = cbm_pipeline_fqn_module_dir(ctx->project_name, files[i].rel_path,
                                                         pxc_module_is_dir(files[i].language));
        }

        const char **imp_keys = NULL;
        const char **imp_vals = NULL;
        int imp_count = 0;
        pxc_build_import_map(ctx->gbuf, ctx->project_name, files[i].rel_path, &imp_keys, &imp_vals,
                             &imp_count);

        /* Journal around the resolve: a hang here must be attributed to THIS
         * file, not to a stale extraction marker (the innocent-quarantine
         * failure mode). */
        cbm_index_mark_start(files[i].rel_path);
        bool had_error = cache[i]->has_error;
        cbm_pxc_dispatch_file(lang, cache[i], source, source_len, files[i].rel_path, def_modules[i],
                              &cross_registries, module_def_index, all_defs, def_count, imp_keys,
                              imp_vals, imp_count, NULL, NULL);
        cbm_index_mark_done(files[i].rel_path);
        /* A cross-file walk that stopped incomplete kept only the calls it
         * had before; the file is indexed with less resolution than it
         * should have, so the gap is reported rather than silent. */
        if (!had_error && cache[i]->has_error && ctx->pipeline) {
            cbm_pipeline_add_file_error(ctx->pipeline, files[i].rel_path, cache[i]->error_msg,
                                        "lsp_incomplete");
        }
        per_lang_calls++;
        processed++;

        pxc_free_import_map(imp_keys, imp_vals, imp_count);
        free(source);
    }

    cbm_pxc_free_module_def_index(module_def_index);
    free((void *)cross_registries.zig_args);
    free(all_defs);
    for (int i = 0; i < file_count; i++)
        free(def_modules[i]);
    free(def_modules);

    /* Drop the borrowed manifest pointer before its arena dies, so a later
     * pass (or a stale thread-local) can never read freed manifest memory. */
    if (have_rust) {
        cbm_pxc_set_rust_manifest(NULL);
        cbm_arena_destroy(&cargo_arena);
    }
    (void)have_manifest;

    cbm_log_info("pass.done", "pass", "lsp_cross", "files_processed", itoa_buf(processed),
                 "files_skipped_no_lsp", itoa_buf(skipped_no_lsp), "files_skipped_no_source",
                 itoa_buf(skipped_no_source), "defs_total", itoa_buf(def_count), "lsp_calls",
                 itoa_buf(per_lang_calls));
    return 0;
}

/* ── Per-module def index (gopls "package summary" pattern) ──── */

typedef struct {
    int count;
    int cap;
    int *indices; /* malloc'd; indices into the caller's all_defs[] */
} pxc_module_entry_t;

struct CBMModuleDefIndex {
    CBMHashTable *ht;           /* module_qn → pxc_module_entry_t* */
    CBMHashTable *namespace_ht; /* declared package/namespace → pxc_module_entry_t* */
    int def_count;              /* total entries in the all_defs[] array */
};

/* cbm_ht_foreach callback: free each pxc_module_entry_t. */
static void pxc_module_entry_free_cb(const char *key, void *value, void *userdata) {
    (void)key;
    (void)userdata;
    pxc_module_entry_t *e = (pxc_module_entry_t *)value;
    if (!e)
        return;
    free(e->indices);
    free(e);
}
static pxc_module_entry_t *pxc_module_entry_get_or_create(CBMHashTable *ht, const char *key) {
    if (!ht || !key || !key[0]) {
        return NULL;
    }
    pxc_module_entry_t *e = (pxc_module_entry_t *)cbm_ht_get(ht, key);
    if (e) {
        return e;
    }
    e = (pxc_module_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->cap = 8;
    e->indices = (int *)calloc((size_t)e->cap, sizeof(*e->indices));
    if (!e->indices) {
        free(e);
        return NULL;
    }
    cbm_ht_set(ht, key, e);
    return e;
}

static void pxc_module_entry_add_index(pxc_module_entry_t *e, int index) {
    if (!e) {
        return;
    }
    if (e->count >= e->cap) {
        int new_cap = e->cap * 2;
        int *new_indices = (int *)realloc(e->indices, (size_t)new_cap * sizeof(*new_indices));
        if (!new_indices) {
            return;
        }
        e->indices = new_indices;
        e->cap = new_cap;
    }
    e->indices[e->count++] = index;
}

static bool pxc_is_jvm_lang(CBMLanguage lang);
static bool pxc_def_lang_matches(CBMLanguage caller_lang, CBMLanguage def_lang);

static int pxc_mark_entry_defs(bool *selected, const pxc_module_entry_t *e,
                               const CBMLSPDef *all_defs, CBMLanguage caller_lang) {
    if (!selected || !e) {
        return 0;
    }
    int added = 0;
    for (int j = 0; j < e->count; j++) {
        int idx = e->indices[j];
        const CBMLSPDef *def = &all_defs[idx];
        if (!pxc_def_lang_matches(caller_lang, def->lang) || selected[idx]) {
            continue;
        }
        selected[idx] = true;
        added++;
    }
    return added;
}

static bool pxc_is_jvm_lang(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_KOTLIN;
}

static bool pxc_def_lang_matches(CBMLanguage caller_lang, CBMLanguage def_lang) {
    if (pxc_is_jvm_lang(caller_lang)) {
        return pxc_is_jvm_lang(def_lang);
    }
    return true;
}

static void pxc_mark_module_defs(const CBMModuleDefIndex *idx, bool *selected,
                                 const CBMLSPDef *all_defs, CBMLanguage caller_lang,
                                 const char *module_qn, int *total) {
    if (!idx || !idx->ht || !module_qn || !module_qn[0]) {
        return;
    }
    pxc_module_entry_t *e = (pxc_module_entry_t *)cbm_ht_get(idx->ht, module_qn);
    int added = pxc_mark_entry_defs(selected, e, all_defs, caller_lang);
    if (total) {
        *total += added;
    }
}

/* Zig: an alias names a module other than the file's imports (a re-export
 * `pub const Database = zova.Database;`, or `const sqlite = @import(..)` that a
 * return type text spells), so a Zig file also gets the defs of each module a
 * selected alias names, transitively. Returns the number of defs added. */
enum { PXC_ZIG_ALIAS_ROUNDS = 8 };
static int pxc_mark_zig_alias_modules(const CBMModuleDefIndex *idx, bool *selected,
                                      const CBMLSPDef *all_defs) {
    int added_total = 0;
    for (int round = 0; round < PXC_ZIG_ALIAS_ROUNDS; round++) {
        int added = 0;
        for (int i = 0; i < idx->def_count; i++) {
            const CBMLSPDef *d = &all_defs[i];
            if (!selected[i] || d->lang != CBM_LANG_ZIG || !d->return_types || !d->label ||
                strcmp(d->label, "Variable") != 0) {
                continue;
            }
            char buf[CBM_SZ_512];
            snprintf(buf, sizeof(buf), "%s", d->return_types);
            for (char *dot = buf; dot;) {
                pxc_module_entry_t *e = (pxc_module_entry_t *)cbm_ht_get(idx->ht, buf);
                if (e) {
                    added += pxc_mark_entry_defs(selected, e, all_defs, CBM_LANG_ZIG);
                    break;
                }
                dot = strrchr(buf, '.');
                if (dot) {
                    *dot = '\0';
                }
            }
        }
        added_total += added;
        if (added == 0) {
            break;
        }
    }
    return added_total;
}

CBMModuleDefIndex *cbm_pxc_build_module_def_index(CBMLSPDef *all_defs, int def_count) {
    if (!all_defs || def_count <= 0) {
        return NULL;
    }

    CBMHashTable *ht = cbm_ht_create(64);
    CBMHashTable *namespace_ht = cbm_ht_create(64);
    if (!ht || !namespace_ht) {
        cbm_ht_free(ht);
        cbm_ht_free(namespace_ht);
        return NULL;
    }

    /* Single pass: index each def by file module and by declared package.
     * JVM mixed roots (`src/main/java` + `src/main/kotlin`) share the
     * declared package, not the path-derived module prefix. */
    for (int i = 0; i < def_count; i++) {
        pxc_module_entry_add_index(pxc_module_entry_get_or_create(ht, all_defs[i].def_module_qn),
                                   i);
        pxc_module_entry_add_index(
            pxc_module_entry_get_or_create(namespace_ht, all_defs[i].namespace_name), i);
    }

    CBMModuleDefIndex *idx = (CBMModuleDefIndex *)calloc(1, sizeof(*idx));
    if (!idx) {
        cbm_ht_foreach(ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(ht);
        cbm_ht_foreach(namespace_ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(namespace_ht);
        return NULL;
    }
    idx->ht = ht;
    idx->namespace_ht = namespace_ht;
    idx->def_count = def_count;
    return idx;
}

void cbm_pxc_free_module_def_index(CBMModuleDefIndex *idx) {
    if (!idx) {
        return;
    }
    if (idx->ht) {
        cbm_ht_foreach(idx->ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(idx->ht);
    }
    if (idx->namespace_ht) {
        cbm_ht_foreach(idx->namespace_ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(idx->namespace_ht);
    }
    free(idx);
}

CBMLSPDef *cbm_pxc_filter_defs_for_file(const CBMModuleDefIndex *idx, CBMLSPDef *all_defs,
                                        CBMLanguage caller_lang, const char *caller_namespace,
                                        const char *own_module, const char *const *imp_qns,
                                        int imp_count, int *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    if (!idx || !idx->ht || !all_defs || !out_count || idx->def_count <= 0) {
        return NULL;
    }

    bool *selected = (bool *)calloc((size_t)idx->def_count, sizeof(*selected));
    if (!selected) {
        return NULL;
    }

    int total = 0;
    pxc_mark_module_defs(idx, selected, all_defs, caller_lang, own_module, &total);
    for (int i = 0; i < imp_count; i++) {
        pxc_mark_module_defs(idx, selected, all_defs, caller_lang, imp_qns[i], &total);
    }
    if (caller_lang == CBM_LANG_ZIG) {
        total += pxc_mark_zig_alias_modules(idx, selected, all_defs);
    }
    if (pxc_is_jvm_lang(caller_lang) && caller_namespace && caller_namespace[0] &&
        idx->namespace_ht) {
        pxc_module_entry_t *e =
            (pxc_module_entry_t *)cbm_ht_get(idx->namespace_ht, caller_namespace);
        total += pxc_mark_entry_defs(selected, e, all_defs, caller_lang);
    }

    if (total == 0) {
        free(selected);
        return NULL;
    }

    CBMLSPDef *out = (CBMLSPDef *)malloc((size_t)total * sizeof(CBMLSPDef));
    if (!out) {
        free(selected);
        return NULL;
    }

    int n = 0;
    for (int i = 0; i < idx->def_count; i++) {
        if (selected[i]) {
            out[n++] = all_defs[i];
        }
    }
    *out_count = n;
    free(selected);
    return out;
}
