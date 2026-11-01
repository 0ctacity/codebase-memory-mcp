/* zig_lsp.c — Type-aware call resolution for Zig. See zig_lsp.h. */
#include "zig_lsp.h"
#include "scope.h" /* cbm_lsp_max_walk_depth */
#include "../helpers.h"
#include "../cbm.h" // cbm_parse_source
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "tree_sitter/api.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const TSLanguage *tree_sitter_zig(void);

#define ZIG_CONFIDENCE 0.95f
#define ZIG_QN_BUF 1024
/* Alias chains (`const A = B; const B = C;`) and nested expressions are short in
 * real code; the cap only stops a cyclic or adversarial file. */
#define ZIG_MAX_EVAL_DEPTH 32

typedef enum { ZV_UNKNOWN = 0, ZV_TYPE, ZV_VALUE } ZigValKind;

/* What an expression denotes: a container type (struct/enum/union/opaque, or a
 * module — a Zig file is a struct), or a value of one. slice_depth counts the
 * slice/array layers around the container (`[]T` is 1). */
typedef struct {
    ZigValKind kind;
    const char *qn;
    int slice_depth;
} ZigVal;

typedef struct {
    const char *qn;
    const char *ret;       /* declared return type text, or NULL */
    const char *scope_qn;  /* container the function is declared in */
    const char *module_qn; /* module that declares it */
    const char *params;    /* cbm_zig_param_kinds of its signature when it takes an
                            * `anytype` parameter, else NULL */
} ZigCallable;

typedef struct {
    const char *qn;
    const char *module_qn;
    const char *fields; /* "name:type|..." (cross-file field types) */
    const char *params; /* type function: "|"-separated comptime type parameter
                         * names, "_" for any other parameter (cross-file) */
} ZigTypeInfo;

typedef struct {
    const char *name;
    ZigVal val;
} ZigLocal;

typedef struct {
    CBMArena *arena;
    const char *source;
    const char *module_qn;
    CBMHashTable *callables;  /* qn -> ZigCallable* */
    CBMHashTable *types;      /* qn -> ZigTypeInfo* */
    CBMHashTable *modules;    /* module qn -> non-NULL */
    CBMHashTable *containers; /* this file: container qn -> TSNode* */
    CBMHashTable *decls;      /* this file: "<container byte>:<name>" -> TSNode* */
    CBMHashTable *aliases;    /* other files: alias qn -> the QN it names */
    const char **imp_names;
    const char **imp_qns;
    int imp_count;
    ZigLocal *locals;
    int local_count;
    int local_cap;
    const char *caller_qn;
    CBMResolvedCallArray *out;
    int depth;      /* expression-evaluation recursion */
    int walk_depth; /* statement-walk recursion */
    /* Type parameters bound while a member of a generic instance is evaluated
     * (`T` -> "proj.Row" for a member of `List(Row)`). */
    const char *tp_names[16];
    const char *tp_vals[16];
    int tp_count;
    /* anytype: argument types to walk the file's functions with (sorted by
     * callee_qn), and where argument types of resolved calls are collected. */
    const CBMZigArgType *args;
    int arg_count;
    CBMZigArgTypeArray *collect;
    int site_from; /* >= 0 while a function is walked again with an anytype
                    * parameter bound: out rows from here on are its sites */
} ZigLSP;

static const ZigVal ZIG_UNKNOWN = {ZV_UNKNOWN, NULL, 0};

static ZigVal zig_val(ZigValKind kind, const char *qn, int slice_depth) {
    if (!qn) {
        return ZIG_UNKNOWN;
    }
    ZigVal v = {kind, qn, slice_depth};
    return v;
}

static const char *zig_text(ZigLSP *z, TSNode n) {
    return ts_node_is_null(n) ? NULL : cbm_node_text(z->arena, n, z->source);
}

static bool zig_kind_is(TSNode n, const char *kind) {
    return !ts_node_is_null(n) && strcmp(ts_node_type(n), kind) == 0;
}

static bool zig_is_container_decl(TSNode n) {
    if (ts_node_is_null(n)) {
        return false;
    }
    const char *k = ts_node_type(n);
    return strcmp(k, "struct_declaration") == 0 || strcmp(k, "enum_declaration") == 0 ||
           strcmp(k, "union_declaration") == 0 || strcmp(k, "opaque_declaration") == 0;
}

static bool zig_is_container(TSNode n) {
    return zig_is_container_decl(n) || zig_kind_is(n, "source_file");
}

static bool zig_is_if(TSNode n) {
    return zig_kind_is(n, "if_expression") || zig_kind_is(n, "if_type_expression");
}

/* Is n a branch of the if node p, not its condition? */
static bool zig_is_if_branch(TSNode p, TSNode n) {
    TSNode cond = ts_node_child_by_field_name(p, TS_FIELD("condition"));
    return ts_node_is_null(cond) || !ts_node_eq(cond, n);
}

enum { ZIG_MAX_BRANCHES = 8 };

/* Containers a declaration value defines: the container itself, or each
 * container branch of a compile-time `if` (`if (c) struct {..} else struct {..}`),
 * which all share the declaration's QN. Appends to out from n; returns the count. */
static int zig_value_containers(TSNode v, TSNode *out, int n) { // NOLINT(misc-no-recursion)
    if (ts_node_is_null(v) || n >= ZIG_MAX_BRANCHES) {
        return n;
    }
    if (zig_is_container_decl(v)) {
        out[n++] = v;
        return n;
    }
    if (!zig_is_if(v)) {
        return n;
    }
    uint32_t nc = ts_node_named_child_count(v);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode b = ts_node_named_child(v, i);
        if (zig_is_if_branch(v, b)) {
            n = zig_value_containers(b, out, n);
        }
    }
    return n;
}

/* Nearest container enclosing n (the lexical scope its declarations live in). */
static TSNode zig_enclosing_container(TSNode n) {
    for (TSNode cur = ts_node_parent(n); !ts_node_is_null(cur); cur = ts_node_parent(cur)) {
        if (zig_is_container(cur)) {
            return cur;
        }
    }
    TSNode null_node = {0};
    return null_node;
}

static TSNode *zig_node_copy(ZigLSP *z, TSNode n) {
    TSNode *p = (TSNode *)cbm_arena_alloc(z->arena, sizeof(TSNode));
    if (p) {
        *p = n;
    }
    return p;
}

/* QN of a container node, matching the def walk: module QN for the file, then
 * each named container (`const Name = struct {...}`) appended. NULL for an
 * anonymous container or one declared inside a function body. */
static const char *zig_container_qn(ZigLSP *z, TSNode c) {
    if (ts_node_is_null(c)) {
        return NULL;
    }
    if (zig_kind_is(c, "source_file")) {
        return z->module_qn;
    }
    TSNode name = cbm_zig_container_name_node(c);
    if (ts_node_is_null(name)) {
        return NULL;
    }
    /* The declaration must sit directly in a container, not in a function. */
    TSNode decl = cbm_zig_container_decl(c);
    TSNode outer = ts_node_parent(decl);
    if (!zig_is_container(outer)) {
        return NULL;
    }
    const char *outer_qn = zig_container_qn(z, outer);
    const char *nm = zig_text(z, name);
    if (!outer_qn || !nm || !nm[0]) {
        return NULL;
    }
    return cbm_arena_sprintf(z->arena, "%s.%s", outer_qn, nm);
}

/* Index a container's direct declarations by name, once. */
static void zig_index_container(ZigLSP *z, TSNode c) {
    char key[ZIG_QN_BUF];
    snprintf(key, sizeof(key), "%u:", ts_node_start_byte(c));
    if (cbm_ht_get(z->decls, key)) {
        return;
    }
    cbm_ht_set(z->decls, cbm_arena_strdup(z->arena, key), zig_node_copy(z, c));
    uint32_t nc = ts_node_named_child_count(c);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode d = ts_node_named_child(c, i);
        TSNode name = {0};
        if (zig_kind_is(d, "variable_declaration")) {
            name = ts_node_named_child(d, 0);
            if (!zig_kind_is(name, "identifier")) {
                continue;
            }
        } else if (zig_kind_is(d, "function_declaration")) {
            name = ts_node_child_by_field_name(d, TS_FIELD("name"));
        } else {
            continue;
        }
        const char *nm = zig_text(z, name);
        if (!nm || !nm[0]) {
            continue;
        }
        char *dkey = cbm_arena_sprintf(z->arena, "%u:%s", ts_node_start_byte(c), nm);
        if (dkey && !cbm_ht_get(z->decls, dkey)) {
            cbm_ht_set(z->decls, dkey, zig_node_copy(z, d));
        }
    }
}

/* Declaration `name` directly in container c, or NULL. */
static const TSNode *zig_find_decl(ZigLSP *z, TSNode c, const char *name) {
    zig_index_container(z, c);
    char key[ZIG_QN_BUF];
    int kn = snprintf(key, sizeof(key), "%u:%s", ts_node_start_byte(c), name);
    if (kn <= 0 || kn >= (int)sizeof(key)) {
        return NULL;
    }
    return (const TSNode *)cbm_ht_get(z->decls, key);
}

/* Lexical lookup: the declaration `name` visible from `from`, searching the
 * enclosing containers outward. *out_container receives where it was found. */
static const TSNode *zig_lookup_decl(ZigLSP *z, TSNode from, const char *name,
                                     TSNode *out_container) {
    TSNode c = zig_is_container(from) ? from : zig_enclosing_container(from);
    while (!ts_node_is_null(c)) {
        const TSNode *d = zig_find_decl(z, c, name);
        if (d) {
            if (out_container) {
                *out_container = c;
            }
            return d;
        }
        c = zig_enclosing_container(c);
    }
    return NULL;
}

static const char *zig_import_qn(ZigLSP *z, TSNode builtin) {
    TSNode args = cbm_find_child_by_kind(builtin, "arguments");
    TSNode str = ts_node_is_null(args) ? args : cbm_find_child_by_kind(args, "string");
    TSNode content = ts_node_is_null(str) ? str : cbm_find_child_by_kind(str, "string_content");
    const char *path = zig_text(z, content);
    if (path && strcmp(path, "std") == 0) {
        return "std"; /* the std namespace (see zig_is_std_ns) */
    }
    const char *local = cbm_zig_import_local_name(z->arena, builtin, z->source, path);
    if (!local) {
        return NULL;
    }
    for (int i = 0; i < z->imp_count; i++) {
        if (z->imp_names[i] && z->imp_qns[i] && strcmp(z->imp_names[i], local) == 0) {
            return z->imp_qns[i];
        }
    }
    return NULL;
}

static const char *zig_builtin_name(ZigLSP *z, TSNode builtin) {
    return zig_text(z, cbm_find_child_by_kind(builtin, "builtin_identifier"));
}

/* A path into the std namespace ("std", "std.mem.Allocator"). Project QNs
 * start with the project name, so they never collide with it. */
static bool zig_is_std_ns(const char *qn) {
    return qn && strncmp(qn, "std", 3) == 0 && (qn[3] == '\0' || qn[3] == '.') &&
           !strpbrk(qn, "($");
}

/* A std namespace path or a std generic instance ("std.$List(...)"). */
static bool zig_is_std_qn(const char *qn) {
    return qn && strncmp(qn, "std", 3) == 0 && (qn[3] == '\0' || qn[3] == '.');
}

static bool zig_is_known_container(ZigLSP *z, const char *qn) {
    return qn && (cbm_ht_get(z->types, qn) || cbm_ht_get(z->modules, qn) ||
                  cbm_ht_get(z->containers, qn) || zig_is_std_ns(qn));
}

/* The registered container or callable that qn names once the aliases other
 * files declare (`pub const Database = zova.Database;`) are followed, in qn or
 * in any prefix of it; NULL if none. Returns qn itself when it is registered. */
static const char *zig_canonical(ZigLSP *z, const char *qn, int depth) {
    if (!qn || depth >= ZIG_MAX_EVAL_DEPTH) {
        return NULL;
    }
    if (zig_is_known_container(z, qn) || cbm_ht_get(z->callables, qn)) {
        return qn;
    }
    const char *target = (const char *)cbm_ht_get(z->aliases, qn);
    if (target) {
        return zig_canonical(z, target, depth + 1);
    }
    const char *dot = strrchr(qn, '.');
    if (!dot || (size_t)(dot - qn) >= ZIG_QN_BUF) {
        return NULL;
    }
    char prefix[ZIG_QN_BUF];
    memcpy(prefix, qn, (size_t)(dot - qn));
    prefix[dot - qn] = '\0';
    const char *p = zig_canonical(z, prefix, depth + 1);
    if (!p || strcmp(p, prefix) == 0) {
        return NULL;
    }
    return zig_canonical(z, cbm_arena_sprintf(z->arena, "%s%s", p, dot), depth + 1);
}

/* zig_canonical for a container: an arena copy (or stable QN), or NULL. */
static const char *zig_canonical_type(ZigLSP *z, const char *qn) {
    const char *c = zig_canonical(z, qn, 0);
    if (!c || !zig_is_known_container(z, c)) {
        return NULL;
    }
    return c == qn ? cbm_arena_strdup(z->arena, qn) : c;
}

static const char *zig_eval_type_expr(ZigLSP *z, TSNode n);
static ZigVal zig_eval_expr(ZigLSP *z, TSNode n);
static const ZigLocal *zig_local(ZigLSP *z, const char *name);
static ZigVal zig_decl_val(ZigLSP *z, TSNode decl);

/* ── Generic instances and the std seed ─────────────────────────────────────
 * std is an opaque namespace: "std", "std.mem.Allocator" are paths, not graph
 * nodes, so calls into std emit nothing. Its generic containers become
 * instances spelled `<base>(<arg>,...)`, each arg a type string (`[]` per
 * slice layer, then a QN or another instance, "_" when unknown), normalised
 * to the internal bases below. The seed spells what their fields and methods
 * yield in terms of the instance args ($N), the call's type arguments (%N)
 * and the instance itself (@self); pointer and optional layers are
 * transparent, as everywhere in this resolver. That carries element types on
 * to project code: `list.items[0].deinit()`, `map.get(k).?.close()`. */

typedef struct {
    const char *path; /* the std path code spells */
    const char *base; /* internal base the seed describes */
    const char *args; /* internal args in terms of the written ones; NULL = as written */
} ZigStdFamily;

static const ZigStdFamily ZIG_STD_FAMILIES[] = {
    {"std.ArrayList", "std.$List", NULL},
    {"std.ArrayListUnmanaged", "std.$List", NULL},
    {"std.ArrayListAligned", "std.$List", "$0"},
    {"std.ArrayListAlignedUnmanaged", "std.$List", "$0"},
    {"std.array_list.Managed", "std.$List", NULL},
    {"std.array_list.Aligned", "std.$List", "$0"},
    {"std.array_list.AlignedManaged", "std.$List", "$0"},
    {"std.BoundedArray", "std.$List", "$0"},
    {"std.BoundedArrayAligned", "std.$List", "$0"},
    {"std.AutoHashMap", "std.$Map", NULL},
    {"std.AutoHashMapUnmanaged", "std.$Map", NULL},
    {"std.HashMap", "std.$Map", "$0,$1"},
    {"std.HashMapUnmanaged", "std.$Map", "$0,$1"},
    {"std.hash_map.HashMap", "std.$Map", "$0,$1"},
    {"std.hash_map.HashMapUnmanaged", "std.$Map", "$0,$1"},
    {"std.AutoArrayHashMap", "std.$Map", NULL},
    {"std.AutoArrayHashMapUnmanaged", "std.$Map", NULL},
    {"std.ArrayHashMap", "std.$Map", "$0,$1"},
    {"std.ArrayHashMapUnmanaged", "std.$Map", "$0,$1"},
    {"std.array_hash_map.ArrayHashMap", "std.$Map", "$0,$1"},
    {"std.array_hash_map.ArrayHashMapUnmanaged", "std.$Map", "$0,$1"},
    {"std.StringHashMap", "std.$Map", "_,$0"},
    {"std.StringHashMapUnmanaged", "std.$Map", "_,$0"},
    {"std.StringArrayHashMap", "std.$Map", "_,$0"},
    {"std.StringArrayHashMapUnmanaged", "std.$Map", "_,$0"},
    {"std.EnumArray", "std.$EnumMap", NULL},
    {"std.EnumMap", "std.$EnumMap", NULL},
    {"std.PriorityQueue", "std.$Queue", "$0"},
    {"std.PriorityDequeue", "std.$Queue", "$0"},
    {"std.atomic.Value", "std.$Cell", NULL},
    {"std.json.Parsed", "std.$Parsed", NULL},
    {"std.heap.MemoryPool", "std.$Pool", NULL},
    {"std.heap.MemoryPoolExtra", "std.$Pool", "$0"},
    {"std.heap.GeneralPurposeAllocator", "std.$Gpa", "_"},
    {"std.heap.DebugAllocator", "std.$Gpa", "_"},
};

typedef struct {
    const char *base;
    const char *members; /* "field=type|method()=type|..." */
} ZigStdType;

static const ZigStdType ZIG_STD_TYPES[] = {
    {"std.$List", "items=[]$0|empty=@self|init()=@self|initCapacity()=@self|initBuffer()=@self|"
                  "fromOwnedSlice()=@self|clone()=@self|toManaged()=@self|"
                  "moveToUnmanaged()=@self|pop()=$0|popOrNull()=$0|getLast()=$0|"
                  "getLastOrNull()=$0|addOne()=$0|addOneAssumeCapacity()=$0|swapRemove()=$0|"
                  "orderedRemove()=$0|get()=$0|slice()=[]$0|constSlice()=[]$0|"
                  "toOwnedSlice()=[]$0"},
    {"std.$Map", "empty=@self|unmanaged=@self|init()=@self|clone()=@self|promote()=@self|"
                 "get()=$1|getPtr()=$1|getKey()=$0|getKeyPtr()=$0|"
                 "getEntry()=std.$Entry($0,$1)|getOrPut()=std.$Entry($0,$1)|"
                 "getOrPutAssumeCapacity()=std.$Entry($0,$1)|getOrPutValue()=std.$Entry($0,$1)|"
                 "fetchPut()=std.$KV($0,$1)|fetchPutAssumeCapacity()=std.$KV($0,$1)|"
                 "fetchRemove()=std.$KV($0,$1)|fetchSwapRemove()=std.$KV($0,$1)|"
                 "fetchOrderedRemove()=std.$KV($0,$1)|iterator()=std.$Iter(std.$Entry($0,$1))|"
                 "valueIterator()=std.$Iter($1)|keyIterator()=std.$Iter($0)|values()=[]$1|"
                 "keys()=[]$0"},
    {"std.$Entry", "key_ptr=$0|value_ptr=$1"},
    {"std.$KV", "key=$0|value=$1"},
    {"std.$Iter", "next()=$0"},
    {"std.$EnumMap", "values=[]$1|init()=@self|initFill()=@self|get()=$1|getPtr()=$1"},
    {"std.$Queue", "items=[]$0|init()=@self|remove()=$0|removeOrNull()=$0|peek()=$0|"
                   "removeIndex()=$0|removeMin()=$0|removeMax()=$0|peekMin()=$0|peekMax()=$0"},
    {"std.$Cell", "raw=$0|init()=@self|load()=$0|swap()=$0"},
    {"std.$Parsed", "value=$0"},
    {"std.$Pool", "init()=@self|initPreheated()=@self|create()=$0"},
    {"std.$Gpa", "init=@self|allocator()=std.mem.Allocator"},
    {"std.heap.ArenaAllocator", "init()=@self|allocator()=std.mem.Allocator"},
    {"std.heap.FixedBufferAllocator", "init()=@self|allocator()=std.mem.Allocator"},
    {"std.heap.ThreadSafeAllocator", "allocator()=std.mem.Allocator"},
    {"std.mem.Allocator", "create()=%0|alloc()=[]%0|allocSentinel()=[]%0|alignedAlloc()=[]%0|"
                          "dupe()=[]%0|dupeZ()=[]%0"},
};

/* std.mem.Allocator values code names by path. */
static const char *const ZIG_STD_ALLOCATORS[] = {
    "std.heap.page_allocator", "std.heap.c_allocator",          "std.heap.raw_c_allocator",
    "std.heap.smp_allocator",  "std.heap.wasm_allocator",       "std.testing.allocator",
    "std.testing.failing_allocator",
};

enum { ZIG_MAX_TYPE_ARGS = 4 };

static ZigVal zig_type_node_val(ZigLSP *z, TSNode t);

/* v as a type string ("_" when unknown). */
static const char *zig_val_str(ZigLSP *z, ZigVal v) {
    if (!v.qn) {
        return "_";
    }
    if (v.slice_depth <= 0) {
        return v.qn;
    }
    char buf[ZIG_QN_BUF];
    size_t o = 0;
    for (int i = 0; i < v.slice_depth && o + 2 < sizeof(buf); i++) {
        buf[o++] = '[';
        buf[o++] = ']';
    }
    snprintf(buf + o, sizeof(buf) - o, "%s", v.qn);
    return cbm_arena_strdup(z->arena, buf);
}

/* The value a type string denotes; pointer and optional marks are transparent. */
static ZigVal zig_str_val(ZigLSP *z, const char *s, ZigValKind kind) {
    int slices = 0;
    for (;;) {
        if (s[0] == '[' && s[1] == ']') {
            slices++;
            s += 2;
        } else if (*s == '*' || *s == '?') {
            s++;
        } else {
            break;
        }
    }
    if (!*s || strcmp(s, "_") == 0) {
        return ZIG_UNKNOWN;
    }
    return zig_val(kind, cbm_arena_strdup(z->arena, s), slices);
}

/* Split the top-level comma-separated args of `text` (no parentheses) into
 * arena copies. Returns the count. */
static int zig_split_args(ZigLSP *z, const char *text, size_t len, const char **out) {
    int n = 0;
    int nest = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && n < ZIG_MAX_TYPE_ARGS; i++) {
        char ch = i < len ? text[i] : ',';
        if (ch == '(' || ch == '[') {
            nest++;
        } else if ((ch == ')' || ch == ']') && nest > 0) {
            nest--;
        } else if (ch == ',' && nest == 0) {
            size_t b = start;
            size_t e = i;
            while (b < e && (text[b] == ' ' || text[b] == '\n')) {
                b++;
            }
            while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\n')) {
                e--;
            }
            if (e > b) {
                out[n++] = cbm_arena_strndup(z->arena, text + b, e - b);
            }
            start = i + 1;
        }
    }
    return n;
}

/* Expand a seed spec: $N = instance arg N, %N = call type argument N,
 * @self = the instance. NULL when it does not fit. */
static const char *zig_expand_spec(ZigLSP *z, const char *spec, size_t len, const char *self,
                                   const char **args, int nargs, const char **call_args,
                                   int ncall) {
    char buf[ZIG_QN_BUF];
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        const char *sub = NULL;
        size_t skip = 1;
        if ((spec[i] == '$' || spec[i] == '%') && i + 1 < len && spec[i + 1] >= '0' &&
            spec[i + 1] <= '9') {
            int k = spec[i + 1] - '0';
            sub = spec[i] == '$' ? (k < nargs ? args[k] : "_") : (k < ncall ? call_args[k] : "_");
            skip = 2;
        } else if (self && i + 5 <= len && strncmp(spec + i, "@self", 5) == 0) {
            sub = self;
            skip = 5;
        }
        size_t sl = sub ? strlen(sub) : 1;
        if (o + sl >= sizeof(buf)) {
            return NULL;
        }
        memcpy(buf + o, sub ? sub : spec + i, sl);
        o += sl;
        i += skip;
    }
    buf[o] = '\0';
    return cbm_arena_strdup(z->arena, buf);
}

static const ZigStdFamily *zig_std_family(const char *path) {
    for (size_t i = 0; path && i < sizeof(ZIG_STD_FAMILIES) / sizeof(ZIG_STD_FAMILIES[0]); i++) {
        if (strcmp(path, ZIG_STD_FAMILIES[i].path) == 0) {
            return &ZIG_STD_FAMILIES[i];
        }
    }
    return NULL;
}

/* The instance std generic `path` names with the given type args, or NULL. */
static const char *zig_std_instance(ZigLSP *z, const char *path, const char **args, int nargs) {
    const ZigStdFamily *f = zig_std_family(path);
    if (!f) {
        return NULL;
    }
    char tmpl[32] = "";
    const char *t = f->args;
    if (!t) {
        size_t o = 0;
        for (int i = 0; i < nargs && o + 4 < sizeof(tmpl); i++) {
            o += (size_t)snprintf(tmpl + o, sizeof(tmpl) - o, "%s$%d", i ? "," : "", i);
        }
        t = tmpl;
    }
    const char *inner = zig_expand_spec(z, t, strlen(t), NULL, args, nargs, NULL, 0);
    return inner ? cbm_arena_sprintf(z->arena, "%s(%s)", f->base, inner) : NULL;
}

/* Project generics: a type function (`fn List(comptime T: type) type {
 * return struct {...}; }`) names the containers it returns (extraction gives
 * their members `<fn>.<member>`), and `List(Row)` is the instance
 * "proj.List(proj.Row)". Members of an instance are evaluated with the
 * function's type parameters bound to its args (zig_enter_instance). */

/* Parameters of type function base: names[i] is parameter i's name when it is
 * a `type`, else "_". Returns the count, or -1 when base is not a type
 * function. */
static int zig_type_params(ZigLSP *z, const char *base, const char **names) {
    if (!base) {
        return -1;
    }
    const TSNode *c = (const TSNode *)cbm_ht_get(z->containers, base);
    if (c) {
        TSNode fn = cbm_zig_container_decl(*c);
        if (!zig_kind_is(fn, "function_declaration")) {
            return -1;
        }
        TSNode ps = cbm_find_child_by_kind(fn, "parameters");
        int n = 0;
        uint32_t pc = ts_node_is_null(ps) ? 0 : ts_node_named_child_count(ps);
        for (uint32_t i = 0; i < pc && n < ZIG_MAX_TYPE_ARGS; i++) {
            TSNode p = ts_node_named_child(ps, i);
            const char *ty = zig_text(z, ts_node_child_by_field_name(p, TS_FIELD("type")));
            const char *nm = zig_text(z, ts_node_child_by_field_name(p, TS_FIELD("name")));
            names[n++] = ty && nm && strcmp(ty, "type") == 0 ? nm : "_";
        }
        return n;
    }
    const ZigTypeInfo *t = (const ZigTypeInfo *)cbm_ht_get(z->types, base);
    if (!t || !t->params) {
        return -1;
    }
    int n = zig_split_args(z, t->params, strlen(t->params), names);
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "*") == 0) {
            names[i] = "_"; /* an `anytype` parameter */
        }
    }
    return n;
}

/* The type generic base names with args: a std instance, a project instance,
 * base itself for a type function without type parameters, else NULL. */
static const char *zig_make_instance(ZigLSP *z, const char *base, const char **args, int nargs) {
    if (zig_std_family(base)) {
        return zig_std_instance(z, base, args, nargs);
    }
    const char *names[ZIG_MAX_TYPE_ARGS];
    int np = zig_type_params(z, base, names);
    if (np < 0) {
        return NULL;
    }
    char buf[ZIG_QN_BUF];
    int o = snprintf(buf, sizeof(buf), "%s(", base);
    bool any = false;
    for (int i = 0; i < np && o > 0 && o < (int)sizeof(buf); i++) {
        const char *a = strcmp(names[i], "_") != 0 && i < nargs ? args[i] : "_";
        any = any || strcmp(a, "_") != 0;
        o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%s%s", i ? "," : "", a);
    }
    if (!any) {
        return base;
    }
    if (o <= 0 || o + 1 >= (int)sizeof(buf)) {
        return NULL;
    }
    buf[o++] = ')';
    buf[o] = '\0';
    return cbm_arena_strdup(z->arena, buf);
}

static const char *zig_type_fn_qn(ZigLSP *z, TSNode fn);

/* The type a generic call expression names (`std.ArrayList(Row)`,
 * `List(Row)`), or NULL. */
static const char *zig_generic_instance(ZigLSP *z, TSNode call) {
    TSNode fn = ts_node_child_by_field_name(call, TS_FIELD("function"));
    if (!zig_kind_is(fn, "identifier") && !zig_kind_is(fn, "field_expression")) {
        return NULL;
    }
    /* A path (`std.ArrayList`, `pool.Pool`, with the leftmost segment's type
     * prefixes skipped), else a function in scope (`Pool`). */
    const char *base = zig_eval_type_expr(z, fn);
    const char *names[ZIG_MAX_TYPE_ARGS];
    if (!zig_std_family(base) && zig_type_params(z, base, names) < 0) {
        base = zig_type_fn_qn(z, fn);
    }
    if (!base) {
        return NULL;
    }
    TSNode a = cbm_find_child_by_kind(call, "arguments");
    const char *args[ZIG_MAX_TYPE_ARGS];
    int n = 0;
    uint32_t nc = ts_node_is_null(a) ? 0 : ts_node_named_child_count(a);
    for (uint32_t i = 0; i < nc && n < ZIG_MAX_TYPE_ARGS; i++) {
        args[n++] = zig_val_str(z, zig_type_node_val(z, ts_node_named_child(a, i)));
    }
    return zig_make_instance(z, base, args, n);
}

/* A project instance entered for member evaluation: base is the type
 * function's QN, and its type parameters are bound until zig_leave_instance. */
typedef struct {
    const char *base; /* the qn entered, or its type function */
    const char *inst; /* the instance, or NULL when qn is not one */
    int pushed;
} ZigInst;

static ZigInst zig_enter_instance(ZigLSP *z, const char *qn) {
    ZigInst in = {qn, NULL, 0};
    const char *open = qn && !zig_is_std_qn(qn) ? strchr(qn, '(') : NULL;
    size_t len = qn ? strlen(qn) : 0;
    if (!open || qn[len - 1] != ')') {
        return in;
    }
    size_t blen = (size_t)(open - qn);
    in.base = cbm_arena_strndup(z->arena, qn, blen);
    in.inst = qn;
    const char *args[ZIG_MAX_TYPE_ARGS];
    int na = zig_split_args(z, open + 1, len - blen - 2, args);
    const char *names[ZIG_MAX_TYPE_ARGS];
    int np = zig_type_params(z, in.base, names);
    for (int i = 0; i < np && i < na; i++) {
        if (strcmp(names[i], "_") != 0 && strcmp(args[i], "_") != 0 &&
            z->tp_count < (int)(sizeof(z->tp_names) / sizeof(z->tp_names[0]))) {
            z->tp_names[z->tp_count] = names[i];
            z->tp_vals[z->tp_count] = args[i];
            z->tp_count++;
            in.pushed++;
        }
    }
    return in;
}

/* Leave an instance; a value of its type function's own type (`*Self`,
 * `@This()`) is the instance again. */
static ZigVal zig_leave_instance(ZigLSP *z, ZigInst in, ZigVal v) {
    z->tp_count -= in.pushed;
    if (in.inst && v.qn && strcmp(v.qn, in.base) == 0) {
        v.qn = in.inst;
    }
    return v;
}

/* The type a bound type parameter stands for, or NULL. */
static const char *zig_bound_type(ZigLSP *z, const char *name) {
    for (int i = z->tp_count - 1; name && i >= 0; i--) {
        if (strcmp(z->tp_names[i], name) == 0) {
            return z->tp_vals[i];
        }
    }
    return NULL;
}

/* Seed spec of member m (a method when call) of internal base [base, +len). */
static const char *zig_std_spec(const char *base, size_t len, const char *m, bool call,
                                size_t *spec_len) {
    size_t ml = strlen(m);
    for (size_t i = 0; i < sizeof(ZIG_STD_TYPES) / sizeof(ZIG_STD_TYPES[0]); i++) {
        const ZigStdType *t = &ZIG_STD_TYPES[i];
        if (strlen(t->base) != len || strncmp(t->base, base, len) != 0) {
            continue;
        }
        for (const char *p = t->members; *p;) {
            const char *end = strchr(p, '|');
            size_t seg = end ? (size_t)(end - p) : strlen(p);
            const char *eq = (const char *)memchr(p, '=', seg);
            size_t nl = eq ? (size_t)(eq - p) : 0;
            bool is_call = nl > 2 && p[nl - 2] == '(' && p[nl - 1] == ')';
            if (eq && is_call == call && (is_call ? nl - 2 : nl) == ml && strncmp(p, m, ml) == 0) {
                *spec_len = seg - nl - 1;
                return eq + 1;
            }
            if (!end) {
                break;
            }
            p = end + 1;
        }
        return NULL;
    }
    return NULL;
}

/* What field m of std receiver o yields, or (call) method m called at
 * call_node, whose arguments supply %N. */
static ZigVal zig_std_member(ZigLSP *z, ZigVal o, const char *m, bool call, TSNode call_node) {
    if (!zig_is_std_qn(o.qn) || !m || o.slice_depth != 0) {
        return ZIG_UNKNOWN;
    }
    const char *qn = o.qn;
    for (size_t i = 0; i < sizeof(ZIG_STD_ALLOCATORS) / sizeof(ZIG_STD_ALLOCATORS[0]); i++) {
        if (strcmp(qn, ZIG_STD_ALLOCATORS[i]) == 0) {
            qn = "std.mem.Allocator";
        }
    }
    const char *open = strchr(qn, '(');
    size_t qlen = strlen(qn);
    const char *args[ZIG_MAX_TYPE_ARGS];
    int nargs = 0;
    size_t base_len = qlen;
    if (open && qn[qlen - 1] == ')') {
        base_len = (size_t)(open - qn);
        nargs = zig_split_args(z, open + 1, qlen - base_len - 2, args);
    }
    size_t spec_len = 0;
    const char *spec = zig_std_spec(qn, base_len, m, call, &spec_len);
    if (!spec) {
        return ZIG_UNKNOWN;
    }
    const char *call_args[ZIG_MAX_TYPE_ARGS];
    int ncall = 0;
    if (call && memchr(spec, '%', spec_len)) {
        TSNode a = cbm_find_child_by_kind(call_node, "arguments");
        uint32_t nc = ts_node_is_null(a) ? 0 : ts_node_named_child_count(a);
        for (uint32_t i = 0; i < nc && ncall < ZIG_MAX_TYPE_ARGS; i++) {
            call_args[ncall++] = zig_val_str(z, zig_type_node_val(z, ts_node_named_child(a, i)));
        }
    }
    const char *t = zig_expand_spec(z, spec, spec_len, qn, args, nargs, call_args, ncall);
    return t ? zig_str_val(z, t, ZV_VALUE) : ZIG_UNKNOWN;
}

/* Value node of a variable_declaration: its last named child that is neither the
 * name nor the type annotation. */
static TSNode zig_decl_value(TSNode decl) {
    TSNode type = ts_node_child_by_field_name(decl, TS_FIELD("type"));
    uint32_t nc = ts_node_named_child_count(decl);
    if (nc < 2) {
        TSNode null_node = {0};
        return null_node;
    }
    TSNode last = ts_node_named_child(decl, nc - 1);
    if (!ts_node_is_null(type) && ts_node_eq(last, type)) {
        TSNode null_node = {0};
        return null_node;
    }
    return last;
}

/* Container `base` member `m` as a type: a registered nested type or module, or
 * (for this file's containers) a declaration aliasing one. */
static const char *zig_member_type(ZigLSP *z, const char *base, const char *m) {
    if (!base || !m) {
        return NULL;
    }
    char qn[ZIG_QN_BUF];
    int n = snprintf(qn, sizeof(qn), "%s.%s", base, m);
    if (n <= 0 || n >= (int)sizeof(qn)) {
        return NULL;
    }
    const char *canon = zig_canonical_type(z, qn);
    if (canon) {
        return canon;
    }
    const TSNode *c = (const TSNode *)cbm_ht_get(z->containers, base);
    if (!c) {
        return NULL;
    }
    const TSNode *d = zig_find_decl(z, *c, m);
    if (!d || !zig_kind_is(*d, "variable_declaration")) {
        return NULL;
    }
    return zig_eval_type_expr(z, zig_decl_value(*d));
}

/* Skip type prefixes (`*`, `?`, `!`, `[]`, `[N]`), counting in *slices (when
 * non-NULL) the ones that hold elements. tree-sitter-zig binds a prefix tighter
 * than `.`: `*db.Statement` parses as field_expression(object: pointer_type(db),
 * member: Statement), so a qualified type carries its prefixes on its leftmost
 * segment. */
static TSNode zig_strip_type_prefixes(TSNode t, int *slices) {
    for (int guard = 0; !ts_node_is_null(t) && guard < ZIG_MAX_EVAL_DEPTH; guard++) {
        const char *k = ts_node_type(t);
        bool elems = strcmp(k, "slice_type") == 0 || strcmp(k, "array_type") == 0;
        if (elems || strcmp(k, "pointer_type") == 0 || strcmp(k, "nullable_type") == 0) {
            uint32_t nc = ts_node_named_child_count(t);
            t = nc ? ts_node_named_child(t, nc - 1) : (TSNode){0};
            if (elems && slices) {
                (*slices)++;
            }
        } else if (strcmp(k, "error_union_type") == 0) {
            t = ts_node_child_by_field_name(t, TS_FIELD("ok"));
        } else {
            break;
        }
    }
    return t;
}

/* The container type an expression names (`Self`, `db.Database`, `@This()`,
 * `@import("x.zig")`, `struct {...}`), or NULL. */
static const char *zig_eval_type_expr(ZigLSP *z, TSNode n) {
    if (ts_node_is_null(n) || z->depth >= ZIG_MAX_EVAL_DEPTH) {
        return NULL;
    }
    z->depth++;
    const char *res = NULL;
    const char *k = ts_node_type(n);
    if (zig_is_container_decl(n)) {
        res = zig_container_qn(z, n);
    } else if (strcmp(k, "builtin_function") == 0) {
        const char *bn = zig_builtin_name(z, n);
        if (bn && strcmp(bn, "@This") == 0) {
            res = zig_container_qn(z, zig_enclosing_container(n));
        } else if (bn && strcmp(bn, "@import") == 0) {
            res = zig_import_qn(z, n);
        }
    } else if (strcmp(k, "identifier") == 0) {
        const char *nm = zig_text(z, n);
        const ZigLocal *l = nm ? zig_local(z, nm) : NULL;
        if (l) {
            res = l->val.kind == ZV_TYPE ? l->val.qn : NULL; /* const T = Foo; in a body */
        } else {
            const TSNode *d = nm ? zig_lookup_decl(z, n, nm, NULL) : NULL;
            if (d && zig_kind_is(*d, "variable_declaration")) {
                res = zig_eval_type_expr(z, zig_decl_value(*d));
            } else if (!d) {
                res = zig_bound_type(z, nm); /* `T` of an entered instance */
            }
        }
    } else if (strcmp(k, "field_expression") == 0) {
        const char *base = zig_eval_type_expr(
            z, zig_strip_type_prefixes(ts_node_child_by_field_name(n, TS_FIELD("object")), NULL));
        res = zig_member_type(z, base, zig_text(z, ts_node_child_by_field_name(n, TS_FIELD("member"))));
    } else if (strcmp(k, "parenthesized_expression") == 0) {
        res = zig_eval_type_expr(z, ts_node_named_child(n, 0));
    } else if (strcmp(k, "call_expression") == 0) {
        res = zig_generic_instance(z, n); /* std.ArrayList(Row) */
    } else if (zig_is_if(n)) {
        /* Branches share one type (a named compile-time choice of containers
         * shares one QN): the first branch that evaluates. */
        uint32_t nc = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < nc && !res; i++) {
            TSNode b = ts_node_named_child(n, i);
            res = zig_is_if_branch(n, b) ? zig_eval_type_expr(z, b) : NULL;
        }
    }
    z->depth--;
    return res;
}

/* A value of the type a type node spells (`*Self`, `?*db.Statement`, `[]Item`,
 * `!Database`). Pointers, optionals and error unions are transparent: a member
 * call on one only compiles once it is unwrapped to the container. */
static ZigVal zig_type_node_val(ZigLSP *z, TSNode t) {
    int slices = 0;
    t = zig_strip_type_prefixes(t, &slices);
    if (ts_node_is_null(t)) {
        return ZIG_UNKNOWN;
    }
    /* `[]db.Row`, `*pool.Pool(Row)`: the prefixes sit on the leftmost segment. */
    TSNode left = t;
    for (int guard = 0; guard < ZIG_MAX_EVAL_DEPTH; guard++) {
        if (zig_kind_is(left, "field_expression")) {
            left = ts_node_child_by_field_name(left, TS_FIELD("object"));
        } else if (zig_kind_is(left, "call_expression")) {
            left = ts_node_child_by_field_name(left, TS_FIELD("function"));
        } else {
            break;
        }
    }
    zig_strip_type_prefixes(left, &slices);
    const char *qn = zig_eval_type_expr(z, t);
    if (qn && qn[0] == '[') { /* a bound type parameter spelled `[]Row` */
        ZigVal v = zig_str_val(z, qn, ZV_VALUE);
        v.slice_depth += slices;
        return v;
    }
    return zig_val(ZV_VALUE, qn, slices);
}

/* Resolve a dotted type name from a container of this file, lexically. */
static const char *zig_resolve_dotted_in_file(ZigLSP *z, TSNode c, const char *dotted) {
    char buf[ZIG_QN_BUF];
    snprintf(buf, sizeof(buf), "%s", dotted);
    char *save = NULL;
    char *seg = strtok_r(buf, ".", &save);
    if (!seg) {
        return NULL;
    }
    const char *qn = NULL;
    const TSNode *d = zig_lookup_decl(z, c, seg, NULL);
    if (d && zig_kind_is(*d, "variable_declaration")) {
        qn = zig_eval_type_expr(z, zig_decl_value(*d));
    } else if (!d) {
        qn = zig_bound_type(z, seg);
    }
    while (qn && (seg = strtok_r(NULL, ".", &save)) != NULL) {
        qn = zig_member_type(z, qn, seg);
    }
    return qn;
}

/* Resolve a dotted type name declared by another file: the first segment is a
 * container nested in scope_qn or one of its parents down to its module. Zig
 * spells the declaring container `Self` by convention (`const Self =
 * @This();`), which is not a registered type, so it falls back to scope_qn. */
static const char *zig_resolve_dotted_cross(ZigLSP *z, const char *dotted, const char *scope_qn,
                                            const char *module_qn) {
    char buf[ZIG_QN_BUF];
    snprintf(buf, sizeof(buf), "%s", dotted);
    char *save = NULL;
    char *seg = strtok_r(buf, ".", &save);
    if (!seg || !scope_qn) {
        return NULL;
    }
    const char *qn = NULL;
    char scope[ZIG_QN_BUF];
    snprintf(scope, sizeof(scope), "%s", scope_qn);
    size_t mod_len = module_qn ? strlen(module_qn) : 0;
    for (;;) {
        char cand[ZIG_QN_BUF];
        int n = snprintf(cand, sizeof(cand), "%s.%s", scope, seg);
        qn = n > 0 && n < (int)sizeof(cand) ? zig_canonical_type(z, cand) : NULL;
        if (qn) {
            break;
        }
        if (!module_qn || strlen(scope) <= mod_len) {
            break;
        }
        char *dot = strrchr(scope, '.');
        if (!dot) {
            break;
        }
        *dot = '\0';
    }
    if (!qn && strcmp(seg, "Self") == 0 && cbm_ht_get(z->types, scope_qn)) {
        qn = scope_qn;
    }
    if (!qn) {
        qn = zig_bound_type(z, seg);
    }
    while (qn && (seg = strtok_r(NULL, ".", &save)) != NULL) {
        qn = zig_member_type(z, qn, seg);
    }
    return qn;
}

/* A value of the type spelled by `text` (a return or field type recorded by the
 * extractor), resolved where it was declared: in container scope_qn of module
 * module_qn. */
static ZigVal zig_type_text_val(ZigLSP *z, const char *text, const char *scope_qn,
                                const char *module_qn) {
    if (!text || !scope_qn) {
        return ZIG_UNKNOWN;
    }
    /* `E!T` / `!T`: the payload follows the last top-level '!'. */
    int nest = 0;
    const char *p = text;
    for (const char *q = text; *q; q++) {
        if (*q == '(' || *q == '[' || *q == '{') {
            nest++;
        } else if ((*q == ')' || *q == ']' || *q == '}') && nest > 0) {
            nest--;
        } else if (*q == '!' && nest == 0) {
            p = q + 1;
        }
    }
    int slices = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '*' || *p == '?') {
            p++;
        }
        if (*p == '[') {
            /* `[*]T`, `[*c]T` are many-item pointers; `[]T`, `[N]T`, `[:0]T` hold
             * elements. */
            bool many_ptr = p[1] == '*';
            const char *close = strchr(p, ']');
            if (!close) {
                return ZIG_UNKNOWN;
            }
            if (!many_ptr) {
                slices++;
            }
            p = close + 1;
            continue;
        }
        if (strncmp(p, "const ", 6) == 0) {
            p += 6;
            continue;
        }
        if (strncmp(p, "volatile ", 9) == 0) {
            p += 9;
            continue;
        }
        if (strncmp(p, "allowzero ", 10) == 0) {
            p += 10;
            continue;
        }
        if (strncmp(p, "align(", 6) == 0) {
            const char *close = strchr(p, ')');
            if (!close) {
                return ZIG_UNKNOWN;
            }
            p = close + 1;
            continue;
        }
        break;
    }
    const char *qn = NULL;
    if (strncmp(p, "@This()", 7) == 0 && p[7] == '\0') {
        qn = scope_qn;
    } else {
        size_t len = 0;
        while (p[len] && (p[len] == '_' || p[len] == '.' || (p[len] >= 'a' && p[len] <= 'z') ||
                          (p[len] >= 'A' && p[len] <= 'Z') || (p[len] >= '0' && p[len] <= '9'))) {
            len++;
        }
        /* A name, or a generic call `std.ArrayList(Row)`. Anything else (an
         * anonymous container) is not a type this resolver can name. */
        size_t tl = strlen(p);
        bool generic = p[len] == '(' && tl > len + 1 && p[tl - 1] == ')';
        if (len == 0 || (p[len] != '\0' && !generic) || len >= ZIG_QN_BUF) {
            return ZIG_UNKNOWN;
        }
        char name[ZIG_QN_BUF];
        memcpy(name, p, len);
        name[len] = '\0';
        const TSNode *c = (const TSNode *)cbm_ht_get(z->containers, scope_qn);
        qn = c ? zig_resolve_dotted_in_file(z, *c, name)
               : zig_resolve_dotted_cross(z, name, scope_qn, module_qn);
        if (generic) {
            const char *args[ZIG_MAX_TYPE_ARGS];
            int n = zig_split_args(z, p + len + 1, tl - len - 2, args);
            for (int i = 0; i < n; i++) {
                args[i] = zig_val_str(z, zig_type_text_val(z, args[i], scope_qn, module_qn));
            }
            qn = zig_make_instance(z, qn, args, n);
        }
    }
    return zig_val(ZV_VALUE, qn, slices);
}

static const ZigCallable *zig_callable(ZigLSP *z, const char *container_qn, const char *m) {
    if (!container_qn || !m) {
        return NULL;
    }
    char qn[ZIG_QN_BUF];
    int n = snprintf(qn, sizeof(qn), "%s.%s", container_qn, m);
    if (n <= 0 || n >= (int)sizeof(qn)) {
        return NULL;
    }
    const ZigCallable *c = (const ZigCallable *)cbm_ht_get(z->callables, qn);
    const char *canon = c ? NULL : zig_canonical(z, qn, 0);
    return canon ? (const ZigCallable *)cbm_ht_get(z->callables, canon) : c;
}

static const ZigLocal *zig_local(ZigLSP *z, const char *name) {
    for (int i = z->local_count - 1; i >= 0; i--) {
        if (strcmp(z->locals[i].name, name) == 0) {
            return &z->locals[i];
        }
    }
    return NULL;
}

static void zig_bind(ZigLSP *z, TSNode name_node, ZigVal v) {
    const char *nm = zig_text(z, name_node);
    if (!nm || !nm[0] || strcmp(nm, "_") == 0) {
        return;
    }
    if (z->local_count == z->local_cap) {
        int cap = z->local_cap ? z->local_cap * 2 : 32;
        ZigLocal *grown = (ZigLocal *)realloc(z->locals, (size_t)cap * sizeof(ZigLocal));
        if (!grown) {
            return;
        }
        z->locals = grown;
        z->local_cap = cap;
    }
    z->locals[z->local_count].name = nm;
    z->locals[z->local_count].val = v;
    z->local_count++;
}

/* The function a call's `function` node names, and how it was reached. */
typedef struct {
    const ZigCallable *callable;
    const char *strategy;
    const char *inst; /* the generic instance the callable was reached through */
} ZigCallTarget;

/* Is container c one of several a type function returns (`switch (kind) {
 * .a => struct {...}, .b => struct {...} }`)? Their declarations differ, so a
 * declaration looked up in the one registered is not the instance's. */
static bool zig_is_one_of_branches(TSNode c) {
    TSNode fn = cbm_zig_container_decl(c);
    TSNode out[2];
    return zig_kind_is(fn, "function_declaration") && cbm_zig_returned_containers(fn, out, 2) > 1;
}

static ZigCallTarget zig_resolve_callable(ZigLSP *z, TSNode f) {
    ZigCallTarget none = {NULL, NULL, NULL};
    if (ts_node_is_null(f) || z->depth >= ZIG_MAX_EVAL_DEPTH) {
        return none;
    }
    z->depth++;
    ZigCallTarget res = none;
    const char *k = ts_node_type(f);
    if (strcmp(k, "identifier") == 0) {
        const char *nm = zig_text(z, f);
        /* A local or parameter of that name (a function pointer) shadows nothing
         * the resolver can name. */
        if (nm && !zig_local(z, nm)) {
            TSNode c = {0};
            const TSNode *d = zig_lookup_decl(z, f, nm, &c);
            if (d && zig_kind_is(*d, "function_declaration")) {
                res.callable = zig_callable(z, zig_container_qn(z, c), nm);
                res.strategy = "lsp_zig_direct";
            } else if (d && zig_kind_is(*d, "variable_declaration")) {
                res = zig_resolve_callable(z, zig_decl_value(*d)); /* const f = T.f; */
            }
        }
    } else if (strcmp(k, "field_expression") == 0) {
        ZigVal o = zig_eval_expr(z, ts_node_child_by_field_name(f, TS_FIELD("object")));
        const char *m = zig_text(z, ts_node_child_by_field_name(f, TS_FIELD("member")));
        if (o.qn && o.slice_depth == 0) {
            ZigInst in = zig_enter_instance(z, o.qn);
            zig_leave_instance(z, in, ZIG_UNKNOWN);
            res.callable = zig_callable(z, in.base, m);
            res.strategy = o.kind == ZV_VALUE ? "lsp_zig_method" : "lsp_zig_decl";
            res.inst = in.inst;
            if (!res.callable && o.kind == ZV_TYPE) {
                const TSNode *c = (const TSNode *)cbm_ht_get(z->containers, in.base);
                const TSNode *d = c && m && !zig_is_one_of_branches(*c) ? zig_find_decl(z, *c, m)
                                                                         : NULL;
                if (d && zig_kind_is(*d, "variable_declaration")) {
                    res = zig_resolve_callable(z, zig_decl_value(*d));
                }
            }
        }
    }
    z->depth--;
    return res.callable ? res : none;
}

/* The type function a call's `function` node names, or NULL. */
static const char *zig_type_fn_qn(ZigLSP *z, TSNode fn) {
    ZigCallTarget t = zig_resolve_callable(z, fn);
    const char *names[ZIG_MAX_TYPE_ARGS];
    return t.callable && zig_type_params(z, t.callable->qn, names) >= 0 ? t.callable->qn : NULL;
}

/* Declared type of field `m` of container value `qn`. */
static ZigVal zig_field_val(ZigLSP *z, const char *qn, const char *m) {
    if (!qn || !m) {
        return ZIG_UNKNOWN;
    }
    const TSNode *c = (const TSNode *)cbm_ht_get(z->containers, qn);
    if (c) {
        uint32_t nc = ts_node_named_child_count(*c);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode fld = ts_node_named_child(*c, i);
            if (!zig_kind_is(fld, "container_field")) {
                continue;
            }
            const char *fn = zig_text(z, ts_node_child_by_field_name(fld, TS_FIELD("name")));
            if (fn && strcmp(fn, m) == 0) {
                return zig_type_node_val(z, ts_node_child_by_field_name(fld, TS_FIELD("type")));
            }
        }
        return ZIG_UNKNOWN;
    }
    const ZigTypeInfo *t = (const ZigTypeInfo *)cbm_ht_get(z->types, qn);
    if (!t || !t->fields) {
        return ZIG_UNKNOWN;
    }
    size_t ml = strlen(m);
    for (const char *p = t->fields; p && *p;) {
        const char *end = strchr(p, '|');
        size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg > ml && strncmp(p, m, ml) == 0 && p[ml] == ':') {
            char *ty = cbm_arena_strndup(z->arena, p + ml + 1, seg - ml - 1);
            return zig_type_text_val(z, ty, qn, t->module_qn);
        }
        p = end ? end + 1 : NULL;
    }
    return ZIG_UNKNOWN;
}

/* What member m of o (a container type or a value of one) denotes. */
static ZigVal zig_member_val(ZigLSP *z, ZigVal o, const char *m, TSNode n) {
    if (zig_is_std_qn(o.qn)) {
        ZigVal v = zig_std_member(z, o, m, false, n);
        if (v.kind != ZV_UNKNOWN || o.kind == ZV_VALUE || !zig_is_std_ns(o.qn)) {
            return v;
        }
        return zig_val(ZV_TYPE, zig_member_type(z, o.qn, m), 0); /* std.mem.Allocator */
    }
    if (o.kind == ZV_VALUE) {
        return zig_field_val(z, o.qn, m);
    }
    const char *nested = zig_member_type(z, o.qn, m);
    if (nested) {
        return zig_val(ZV_TYPE, nested, 0);
    }
    /* A container-level `var`/`const` value (`Registry.default`). */
    const TSNode *c = (const TSNode *)cbm_ht_get(z->containers, o.qn);
    const TSNode *d = c && !zig_is_one_of_branches(*c) ? zig_find_decl(z, *c, m) : NULL;
    return d && zig_kind_is(*d, "variable_declaration") ? zig_decl_val(z, *d) : ZIG_UNKNOWN;
}

static ZigVal zig_eval_expr_inner(ZigLSP *z, TSNode n) {
    const char *k = ts_node_type(n);
    if (zig_is_container_decl(n)) {
        return zig_val(ZV_TYPE, zig_container_qn(z, n), 0);
    }
    if (strcmp(k, "identifier") == 0) {
        const char *nm = zig_text(z, n);
        if (!nm) {
            return ZIG_UNKNOWN;
        }
        const ZigLocal *l = zig_local(z, nm);
        if (l) {
            return l->val;
        }
        const TSNode *d = zig_lookup_decl(z, n, nm, NULL);
        return d && zig_kind_is(*d, "variable_declaration") ? zig_decl_val(z, *d) : ZIG_UNKNOWN;
    }
    if (strcmp(k, "builtin_function") == 0) {
        const char *bn = zig_builtin_name(z, n);
        if (bn && (strcmp(bn, "@This") == 0 || strcmp(bn, "@import") == 0)) {
            return zig_val(ZV_TYPE, zig_eval_type_expr(z, n), 0);
        }
        if (bn && strcmp(bn, "@as") == 0) {
            TSNode args = cbm_find_child_by_kind(n, "arguments");
            return ts_node_is_null(args) ? ZIG_UNKNOWN
                                         : zig_type_node_val(z, ts_node_named_child(args, 0));
        }
        return ZIG_UNKNOWN;
    }
    if (strcmp(k, "field_expression") == 0) {
        ZigVal o = zig_eval_expr(z, ts_node_child_by_field_name(n, TS_FIELD("object")));
        const char *m = zig_text(z, ts_node_child_by_field_name(n, TS_FIELD("member")));
        if (!o.qn || o.slice_depth != 0 || !m) {
            return ZIG_UNKNOWN;
        }
        ZigInst in = zig_enter_instance(z, o.qn);
        o.qn = in.base;
        return zig_leave_instance(z, in, zig_member_val(z, o, m, n));
    }
    if (strcmp(k, "call_expression") == 0) {
        TSNode f = ts_node_child_by_field_name(n, TS_FIELD("function"));
        ZigCallTarget t = zig_resolve_callable(z, f);
        const char *names[ZIG_MAX_TYPE_ARGS];
        if (t.callable && zig_type_params(z, t.callable->qn, names) < 0) {
            ZigInst in = zig_enter_instance(z, t.inst);
            ZigVal v = zig_type_text_val(z, t.callable->ret, t.callable->scope_qn,
                                         t.callable->module_qn);
            return zig_leave_instance(z, in, v);
        }
        const char *inst = zig_generic_instance(z, n);
        if (inst) {
            return zig_val(ZV_TYPE, inst, 0); /* const List = std.ArrayList(Row); */
        }
        if (zig_kind_is(f, "field_expression")) {
            ZigVal o = zig_eval_expr(z, ts_node_child_by_field_name(f, TS_FIELD("object")));
            if (zig_is_std_qn(o.qn)) {
                return zig_std_member(
                    z, o, zig_text(z, ts_node_child_by_field_name(f, TS_FIELD("member"))), true, n);
            }
        }
        return ZIG_UNKNOWN;
    }
    if (strcmp(k, "try_expression") == 0 || strcmp(k, "parenthesized_expression") == 0 ||
        strcmp(k, "null_coercion_expression") == 0 || strcmp(k, "dereference_expression") == 0 ||
        strcmp(k, "catch_expression") == 0) {
        return zig_eval_expr(z, ts_node_named_child(n, 0));
    }
    if (strcmp(k, "binary_expression") == 0) {
        TSNode op = ts_node_child(n, 1);
        return zig_kind_is(op, "orelse")
                   ? zig_eval_expr(z, ts_node_child_by_field_name(n, TS_FIELD("left")))
                   : ZIG_UNKNOWN;
    }
    if (strcmp(k, "unary_expression") == 0) {
        TSNode op = ts_node_child(n, 0);
        return zig_kind_is(op, "&")
                   ? zig_eval_expr(z, ts_node_child_by_field_name(n, TS_FIELD("argument")))
                   : ZIG_UNKNOWN;
    }
    if (strcmp(k, "index_expression") == 0) {
        ZigVal o = zig_eval_expr(z, ts_node_child_by_field_name(n, TS_FIELD("object")));
        return o.kind == ZV_VALUE && o.slice_depth > 0 ? zig_val(ZV_VALUE, o.qn, o.slice_depth - 1)
                                                       : ZIG_UNKNOWN;
    }
    if (strcmp(k, "struct_initializer") == 0) {
        return zig_val(ZV_VALUE, zig_eval_type_expr(z, ts_node_named_child(n, 0)), 0);
    }
    if (zig_is_if(n)) {
        /* Zig gives both branches one type: the first branch that evaluates. */
        uint32_t nc = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode b = ts_node_named_child(n, i);
            ZigVal v = zig_is_if_branch(n, b) ? zig_eval_expr(z, b) : ZIG_UNKNOWN;
            if (v.kind != ZV_UNKNOWN) {
                return v;
            }
        }
    }
    return ZIG_UNKNOWN;
}

static ZigVal zig_eval_expr(ZigLSP *z, TSNode n) {
    if (ts_node_is_null(n) || z->depth >= ZIG_MAX_EVAL_DEPTH) {
        return ZIG_UNKNOWN;
    }
    z->depth++;
    ZigVal v = zig_eval_expr_inner(z, n);
    z->depth--;
    return v;
}

/* What a declaration binds: its annotated type's value, else its value. */
static ZigVal zig_decl_val(ZigLSP *z, TSNode decl) {
    TSNode type = ts_node_child_by_field_name(decl, TS_FIELD("type"));
    if (!ts_node_is_null(type)) {
        return zig_type_node_val(z, type);
    }
    return zig_eval_expr(z, zig_decl_value(decl));
}

static void zig_emit(ZigLSP *z, TSNode call, const char *callee_qn, const char *strategy) {
    if (z->site_from >= 0) {
        /* A walk with an anytype parameter bound: only sites no earlier walk
         * of this function resolved. */
        uint32_t start = ts_node_start_byte(call);
        for (int i = z->site_from; i < z->out->count; i++) {
            if (z->out->items[i].site_start_byte == start) {
                return;
            }
        }
        strategy = "lsp_zig_anytype";
    }
    CBMResolvedCall rc;
    memset(&rc, 0, sizeof(rc));
    rc.caller_qn = z->caller_qn;
    rc.callee_qn = callee_qn;
    rc.strategy = strategy;
    rc.confidence = ZIG_CONFIDENCE;
    rc.kind = CBM_RESOLVED_INVOCATION;
    rc.site_start_byte = ts_node_start_byte(call);
    rc.site_end_byte = ts_node_end_byte(call);
    cbm_resolvedcall_push(z->out, z->arena, rc);
}

static void zig_walk(ZigLSP *z, TSNode n);

const char *cbm_zig_param_kinds(CBMArena *arena, const char *signature) {
    if (!arena || !signature) {
        return NULL;
    }
    const char *sig = signature;
    size_t len = strlen(sig);
    char *out = (char *)cbm_arena_alloc(arena, len + 2);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    int nest = 0;
    size_t start = sig[0] == '(' ? 1 : 0;
    for (size_t i = start; i <= len; i++) {
        char ch = i < len ? sig[i] : ',';
        if (ch == '(' || ch == '[' || ch == '{') {
            nest++;
            continue;
        }
        if ((ch == ')' || ch == ']' || ch == '}') && nest > 0) {
            nest--;
            continue;
        }
        if (nest > 0 || (ch != ',' && ch != ')')) {
            continue;
        }
        /* One parameter: [comptime|noalias] name: type */
        const char *p = sig + start;
        const char *end = sig + i;
        start = i + 1;
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t')) {
            p++;
        }
        if (p == end) {
            continue;
        }
        if (strncmp(p, "comptime ", 9) == 0) {
            p += 9;
        } else if (strncmp(p, "noalias ", 8) == 0) {
            p += 8;
        }
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *ty = colon ? colon + 1 : end;
        while (ty < end && *ty == ' ') {
            ty++;
        }
        const char *ty_end = end;
        while (ty_end > ty && (ty_end[-1] == ' ' || ty_end[-1] == '\n')) {
            ty_end--;
        }
        size_t nl = colon ? (size_t)(colon - p) : 0;
        while (nl > 0 && p[nl - 1] == ' ') {
            nl--;
        }
        size_t tl = (size_t)(ty_end - ty);
        if (o > 0) {
            out[o++] = ',';
        }
        if (tl == 4 && strncmp(ty, "type", 4) == 0 && nl > 0) {
            memcpy(out + o, p, nl);
            o += nl;
        } else {
            out[o++] = tl == 7 && strncmp(ty, "anytype", 7) == 0 ? '*' : '_';
        }
        if (ch == ')') {
            break;
        }
    }
    out[o] = '\0';
    return out;
}

/* Kind i of a cbm_zig_param_kinds string, or false past its end. */
static bool zig_param_kind_is(const char *kinds, int i, const char *kind) {
    const char *p = kinds;
    for (; p && i > 0; i--) {
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
    size_t len = strlen(kind);
    return p && strncmp(p, kind, len) == 0 && (p[len] == ',' || p[len] == '\0');
}

/* Collect the argument types call passes to target's `anytype` parameters. A
 * value-receiver call (`x.m(a)`) passes x as parameter 0. */
static void zig_collect_args(ZigLSP *z, TSNode call, ZigCallTarget t) {
    CBMZigArgTypeArray *c = z->collect;
    TSNode args = cbm_find_child_by_kind(call, "arguments");
    int p = strcmp(t.strategy, "lsp_zig_method") == 0 ? 1 : 0;
    uint32_t nc = ts_node_is_null(args) ? 0 : ts_node_named_child_count(args);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode a = ts_node_named_child(args, i);
        if (zig_kind_is(a, "comment")) {
            continue;
        }
        ZigVal v = zig_param_kind_is(t.callable->params, p, "*") ? zig_eval_expr(z, a)
                                                                  : ZIG_UNKNOWN;
        if (v.kind == ZV_VALUE && v.qn && !zig_is_std_qn(v.qn)) {
            if (c->count == c->cap) {
                int cap = c->cap ? c->cap * 2 : 64;
                CBMZigArgType *grown =
                    (CBMZigArgType *)realloc(c->items, (size_t)cap * sizeof(CBMZigArgType));
                if (!grown) {
                    return;
                }
                c->items = grown;
                c->cap = cap;
            }
            CBMZigArgType *e = &c->items[c->count++];
            e->callee_qn = cbm_arena_strdup(c->arena, t.callable->qn);
            e->param = p;
            e->type = cbm_arena_strdup(c->arena, zig_val_str(z, v));
        }
        p++;
    }
}

static void zig_walk_children(ZigLSP *z, TSNode n) {
    uint32_t nc = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < nc; i++) {
        zig_walk(z, ts_node_named_child(n, i));
    }
}

/* Bind every identifier of a capture payload (`|a, *b|`) to v. */
static void zig_bind_payload(ZigLSP *z, TSNode payload, ZigVal v) {
    uint32_t nc = ts_node_named_child_count(payload);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode id = ts_node_named_child(payload, i);
        if (zig_kind_is(id, "identifier")) {
            zig_bind(z, id, v);
        }
    }
}

static void zig_walk_for(ZigLSP *z, TSNode n) {
    TSNode body = ts_node_child_by_field_name(n, TS_FIELD("body"));
    ZigVal elems[16];
    int ec = 0;
    uint32_t nc = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_named_child(n, i);
        if (zig_kind_is(c, "payload")) {
            uint32_t pc = ts_node_named_child_count(c);
            for (uint32_t j = 0, e = 0; j < pc; j++) {
                TSNode id = ts_node_named_child(c, j);
                if (zig_kind_is(id, "identifier")) {
                    zig_bind(z, id, (int)e < ec ? elems[e] : ZIG_UNKNOWN);
                    e++;
                }
            }
        } else if (!ts_node_is_null(body) && ts_node_eq(c, body)) {
            zig_walk(z, c);
        } else {
            zig_walk(z, c);
            if (ec < 16) {
                ZigVal it = zig_eval_expr(z, c);
                elems[ec++] = it.kind == ZV_VALUE && it.slice_depth > 0
                                  ? zig_val(ZV_VALUE, it.qn, it.slice_depth - 1)
                                  : ZIG_UNKNOWN;
            }
        }
    }
}

/* if/while: the first payload captures the unwrapped condition. */
static void zig_walk_conditional(ZigLSP *z, TSNode n) {
    TSNode cond = ts_node_child_by_field_name(n, TS_FIELD("condition"));
    bool first_payload = true;
    uint32_t nc = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_named_child(n, i);
        if (zig_kind_is(c, "payload")) {
            zig_bind_payload(z, c, first_payload ? zig_eval_expr(z, cond) : ZIG_UNKNOWN);
            first_payload = false;
        } else {
            zig_walk(z, c);
        }
    }
}

static void zig_walk(ZigLSP *z, TSNode n) {
    if (ts_node_is_null(n) || z->walk_depth >= cbm_lsp_max_walk_depth()) {
        return;
    }
    const char *k = ts_node_type(n);
    /* Containers and functions nested in a body are not definitions the
     * extractor emits; their calls are out of scope. */
    if (zig_is_container_decl(n) || strcmp(k, "function_declaration") == 0) {
        return;
    }
    z->walk_depth++;
    if (strcmp(k, "variable_declaration") == 0) {
        zig_walk_children(z, n);
        TSNode name = ts_node_named_child(n, 0);
        if (zig_kind_is(name, "identifier")) {
            zig_bind(z, name, zig_decl_val(z, n));
        }
    } else if (strcmp(k, "for_statement") == 0) {
        zig_walk_for(z, n);
    } else if (strcmp(k, "if_statement") == 0 || strcmp(k, "while_statement") == 0 ||
               strcmp(k, "if_expression") == 0 || strcmp(k, "while_expression") == 0) {
        zig_walk_conditional(z, n);
    } else if (strcmp(k, "payload") == 0) {
        zig_bind_payload(z, n, ZIG_UNKNOWN);
    } else if (strcmp(k, "call_expression") == 0) {
        ZigCallTarget t =
            zig_resolve_callable(z, ts_node_child_by_field_name(n, TS_FIELD("function")));
        if (t.callable) {
            zig_emit(z, n, t.callable->qn, t.strategy);
            if (t.callable->params && z->collect && z->site_from < 0) {
                zig_collect_args(z, n, t);
            }
        }
        zig_walk_children(z, n);
    } else {
        zig_walk_children(z, n);
    }
    z->walk_depth--;
}

/* Walk fn's body for calls, with its parameters bound to their declared types
 * (anytype parameter bound->param to bound->type when bound is set). */
static void zig_walk_function(ZigLSP *z, TSNode fn, const CBMZigArgType *bound) {
    z->local_count = 0;
    TSNode params = cbm_find_child_by_kind(fn, "parameters");
    uint32_t pc = ts_node_is_null(params) ? 0 : ts_node_named_child_count(params);
    int pi = 0;
    for (uint32_t i = 0; i < pc; i++) {
        TSNode p = ts_node_named_child(params, i);
        if (!zig_kind_is(p, "parameter")) {
            continue;
        }
        TSNode pn = ts_node_child_by_field_name(p, TS_FIELD("name"));
        if (!ts_node_is_null(pn)) {
            zig_bind(z, pn,
                     bound && bound->param == pi
                         ? zig_str_val(z, bound->type, ZV_VALUE)
                         : zig_type_node_val(z, ts_node_child_by_field_name(p, TS_FIELD("type"))));
        }
        pi++;
    }
    TSNode body = ts_node_child_by_field_name(fn, TS_FIELD("body"));
    if (ts_node_is_null(body)) {
        body = cbm_find_child_by_kind(fn, "block");
    }
    z->depth = 0;
    z->walk_depth = 0;
    zig_walk(z, body);
}

static void zig_process_function(ZigLSP *z, TSNode fn, const char *container_qn) {
    TSNode name_node = cbm_resolve_func_name(fn, CBM_LANG_ZIG);
    const char *name =
        ts_node_is_null(name_node)
            ? NULL
            : cbm_func_name_node_text(z->arena, name_node, z->source, CBM_LANG_ZIG);
    if (!name || !name[0]) {
        return;
    }
    /* Tests are file-level Functions wherever they sit (the def walk and the
     * call-scope walk agree on that). */
    bool is_test = zig_kind_is(fn, "test_declaration");
    z->caller_qn = cbm_arena_sprintf(z->arena, "%s.%s", is_test ? z->module_qn : container_qn,
                                     name);
    int site_from = z->out->count;
    zig_walk_function(z, fn, NULL);
    /* Again per type the project passes to an `anytype` parameter. */
    int lo = 0;
    int hi = z->arg_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcmp(z->args[mid].callee_qn, z->caller_qn) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (int i = lo; i < z->arg_count && strcmp(z->args[i].callee_qn, z->caller_qn) == 0; i++) {
        z->site_from = site_from;
        zig_walk_function(z, fn, &z->args[i]);
        z->site_from = -1;
    }
}

/* Register this file's named containers so type names resolve against the AST. */
/* The containers declaration d (a `const` or a type function) names. */
static int zig_decl_containers(TSNode d, TSNode *out) {
    if (zig_kind_is(d, "function_declaration")) {
        return cbm_zig_returned_containers(d, out, ZIG_MAX_BRANCHES);
    }
    return zig_kind_is(d, "variable_declaration") ? zig_value_containers(zig_decl_value(d), out, 0)
                                                  : 0;
}

static void zig_register_containers(ZigLSP *z, TSNode c, const char *qn) {
    cbm_ht_set(z->containers, qn, zig_node_copy(z, c));
    uint32_t nc = ts_node_named_child_count(c);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode d = ts_node_named_child(c, i);
        TSNode branches[ZIG_MAX_BRANCHES];
        int nb = zig_decl_containers(d, branches);
        for (int b = 0; b < nb; b++) {
            const char *vqn = zig_container_qn(z, branches[b]);
            if (vqn) {
                zig_register_containers(z, branches[b], vqn);
            }
        }
    }
}

static void zig_process_container(ZigLSP *z, TSNode c, const char *qn) {
    uint32_t nc = ts_node_named_child_count(c);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode d = ts_node_named_child(c, i);
        const char *k = ts_node_type(d);
        if (strcmp(k, "function_declaration") == 0 || strcmp(k, "test_declaration") == 0) {
            zig_process_function(z, d, qn);
        }
        if (strcmp(k, "function_declaration") == 0 || strcmp(k, "variable_declaration") == 0) {
            TSNode branches[ZIG_MAX_BRANCHES];
            int nb = zig_decl_containers(d, branches);
            for (int b = 0; b < nb; b++) {
                const char *vqn = zig_container_qn(z, branches[b]);
                if (vqn) {
                    zig_process_container(z, branches[b], vqn);
                }
            }
        }
    }
}

static void zig_add_callable(ZigLSP *z, const char *qn, const char *ret, const char *scope_qn,
                             const char *module_qn, const char *params) {
    if (!qn || cbm_ht_get(z->callables, qn)) {
        return;
    }
    ZigCallable *c = (ZigCallable *)cbm_arena_alloc(z->arena, sizeof(ZigCallable));
    if (!c) {
        return;
    }
    c->qn = qn;
    c->ret = ret;
    c->scope_qn = scope_qn;
    c->module_qn = module_qn;
    c->params = params && strchr(params, '*') ? params : NULL;
    cbm_ht_set(z->callables, qn, c);
}

static ZigTypeInfo *zig_add_type(ZigLSP *z, const char *qn, const char *module_qn,
                                 const char *fields, const char *params) {
    if (!qn) {
        return NULL;
    }
    ZigTypeInfo *t = (ZigTypeInfo *)cbm_ht_get(z->types, qn);
    if (t) {
        return t;
    }
    t = (ZigTypeInfo *)cbm_arena_alloc(z->arena, sizeof(ZigTypeInfo));
    if (!t) {
        return NULL;
    }
    t->qn = qn;
    t->module_qn = module_qn;
    t->fields = fields;
    t->params = params;
    cbm_ht_set(z->types, qn, t);
    return t;
}

static bool zig_init(ZigLSP *z, CBMArena *arena, const char *source, const char *module_qn,
                     CBMResolvedCallArray *out) {
    memset(z, 0, sizeof(*z));
    z->arena = arena;
    z->source = source;
    z->module_qn = module_qn;
    z->out = out;
    z->site_from = -1;
    z->callables = cbm_ht_create(256);
    z->types = cbm_ht_create(64);
    z->modules = cbm_ht_create(16);
    z->containers = cbm_ht_create(32);
    z->decls = cbm_ht_create(256);
    z->aliases = cbm_ht_create(64);
    if (!z->callables || !z->types || !z->modules || !z->containers || !z->decls ||
        !z->aliases) {
        return false;
    }
    cbm_ht_set(z->modules, module_qn, (void *)module_qn);
    return true;
}

static void zig_run(ZigLSP *z, TSNode root) {
    zig_register_containers(z, root, z->module_qn);
    zig_process_container(z, root, z->module_qn);
}

static void zig_free(ZigLSP *z) {
    cbm_ht_free(z->callables);
    cbm_ht_free(z->types);
    cbm_ht_free(z->modules);
    cbm_ht_free(z->containers);
    cbm_ht_free(z->decls);
    cbm_ht_free(z->aliases);
    free(z->locals);
}

static bool zig_label_is_callable(const char *label) {
    return label && (strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0);
}

void cbm_run_zig_lsp(CBMArena *arena, CBMFileResult *result, const char *source, int source_len,
                     TSNode root) {
    (void)source_len;
    if (!arena || !result || !source || !result->module_qn || ts_node_is_null(root)) {
        return;
    }
    ZigLSP z;
    if (!zig_init(&z, arena, source, result->module_qn, &result->resolved_calls)) {
        zig_free(&z);
        return;
    }
    for (int i = 0; i < result->defs.count; i++) {
        const CBMDefinition *d = &result->defs.items[i];
        if (zig_label_is_callable(d->label)) {
            zig_add_callable(&z, d->qualified_name, d->return_type,
                             d->parent_class ? d->parent_class : result->module_qn,
                             result->module_qn, NULL);
        } else if (cbm_label_is_type_like(d->label)) {
            zig_add_type(&z, d->qualified_name, result->module_qn, NULL, NULL);
        }
    }
    zig_run(&z, root);
    zig_free(&z);
}

void cbm_run_zig_lsp_cross(CBMArena *arena, const char *source, int source_len,
                           const char *module_qn, CBMLSPDef *defs, int def_count,
                           const char **import_names, const char **import_qns, int import_count,
                           TSTree *cached_tree, CBMResolvedCallArray *out) {
    cbm_run_zig_lsp_cross_ex(arena, source, source_len, module_qn, defs, def_count, import_names,
                             import_qns, import_count, cached_tree, out, NULL, 0, NULL);
}

void cbm_run_zig_lsp_cross_ex(CBMArena *arena, const char *source, int source_len,
                              const char *module_qn, CBMLSPDef *defs, int def_count,
                              const char **import_names, const char **import_qns,
                              int import_count, TSTree *cached_tree, CBMResolvedCallArray *out,
                              const CBMZigArgType *args, int arg_count,
                              CBMZigArgTypeArray *collect) {
    if (!arena || !source || !module_qn || !out) {
        return;
    }
    ZigLSP z;
    if (!zig_init(&z, arena, source, module_qn, out)) {
        zig_free(&z);
        return;
    }
    z.args = args;
    z.arg_count = args ? arg_count : 0;
    z.collect = collect && collect->arena ? collect : NULL;
    z.imp_names = import_names;
    z.imp_qns = import_qns;
    z.imp_count = import_count;
    for (int i = 0; i < import_count; i++) {
        if (import_qns && import_qns[i]) {
            cbm_ht_set(z.modules, import_qns[i], (void *)import_qns[i]);
        }
    }
    for (int i = 0; i < def_count; i++) {
        const CBMLSPDef *d = &defs[i];
        if (d->lang != CBM_LANG_ZIG || !d->qualified_name) {
            continue;
        }
        const char *mod = d->def_module_qn ? d->def_module_qn : module_qn;
        if (zig_label_is_callable(d->label)) {
            zig_add_callable(&z, d->qualified_name, d->return_types,
                             d->receiver_type ? d->receiver_type : mod, mod, d->type_params);
        } else if (cbm_label_is_type_like(d->label)) {
            zig_add_type(&z, d->qualified_name, mod, d->field_defs, d->type_params);
        } else if (d->label && strcmp(d->label, "Variable") == 0 && d->return_types) {
            cbm_ht_set(z.aliases, d->qualified_name, (void *)d->return_types);
        }
        if (d->def_module_qn && !cbm_ht_get(z.modules, d->def_module_qn)) {
            cbm_ht_set(z.modules, d->def_module_qn, (void *)d->def_module_qn);
        }
    }
    TSTree *tree = cached_tree;
    bool owns_tree = false;
    if (!tree) {
        TSParser *parser = ts_parser_new();
        if (parser) {
            ts_parser_set_language(parser, tree_sitter_zig());
            tree = cbm_parse_source(parser, source, (uint32_t)source_len, (TSParseOptions){0});
            ts_parser_delete(parser);
            owns_tree = true;
        }
    }
    if (tree) {
        zig_run(&z, ts_tree_root_node(tree));
        if (owns_tree) {
            ts_tree_delete(tree);
        }
    }
    zig_free(&z);
}
