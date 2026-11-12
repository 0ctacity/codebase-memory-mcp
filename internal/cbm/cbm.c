/* Full declaration set for the same CBMArena, and it must precede cbm.h:
 * internal/cbm/arena.h declares a subset and the two share the CBM_ARENA_H
 * guard, so whichever is included first is the one this file sees. */
#include "foundation/arena.h" // cbm_arena_init_sized
#include "cbm.h"
#include "arena.h" // CBMArena, cbm_arena_init/alloc/strdup/destroy
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/cs_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/rust_lsp.h"
#include "lsp/zig_lsp.h"
#include "preprocessor.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"  // cbm_fopen — crash-supervisor per-file marker write
#include "foundation/hash_table.h" // CBMHashTable — crash-supervisor quarantine set
#include "foundation/str_util.h"   // cbm_json_escape — variants file_path
#include "tree_sitter/api.h" // TSParser, TSNode, TSTree, TSInput, TSLanguage, TSPoint, TSParseOptions, TSParseState
#include "foundation/constants.h"
#include "mimalloc.h" // mi_malloc/mi_calloc/mi_realloc/mi_free/mi_usable_size — bind 3rd-party allocators (#424)
#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
#include "sqlite3.h" // sqlite3_mem_methods, sqlite3_config, SQLITE_CONFIG_MALLOC — bind sqlite to mimalloc
#endif
#include <limits.h> // INT_MAX
#include <stdint.h> // uint32_t, uint64_t, int64_t
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h> // struct timespec, CLOCK_MONOTONIC
#include <stdatomic.h>

// C/C++ preprocessor #define macros are extracted as Macro nodes (#375). On a
// macro-dense codebase (e.g. the Linux kernel: ~2.4M macros, 49% of all nodes)
// this is the dominant extraction cost, so it is gated to the full/advanced
// index modes. Default ON to preserve behavior for direct callers/tests; the
// pipeline sets it from the index mode before extraction. Set once pre-extract,
// read-only during, so a relaxed atomic is sufficient.
static _Atomic int g_extract_macros = 1;
void cbm_set_macro_extraction(int enabled) {
    atomic_store_explicit(&g_extract_macros, enabled ? 1 : 0, memory_order_relaxed);
}
int cbm_macro_extraction_enabled(void) {
    return atomic_load_explicit(&g_extract_macros, memory_order_relaxed);
}

#define NSEC_PER_SEC 1000000000ULL
#define USEC_TO_NSEC 1000ULL
/* Use compat.h's cbm_clock_gettime which accepts CLOCK_MONOTONIC (value
 * varies by platform: 1 on Linux/Windows, 6 on macOS). We pass the
 * platform value via the compat.h fallback. */
#if defined(CLOCK_MONOTONIC)
#define CBM_CLOCK_MONO CLOCK_MONOTONIC
#elif defined(__APPLE__)
#define CBM_CLOCK_MONO 6
#else
#define CBM_CLOCK_MONO 1
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    cbm_clock_gettime(CBM_CLOCK_MONO, &ts);
    return ((uint64_t)ts.tv_sec * NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

// --- Growable array push functions ---

#define GROW_ARRAY(arr, arena)                                                                   \
    do {                                                                                         \
        if ((arr)->count >= (arr)->cap) {                                                        \
            int new_cap = (arr)->cap == 0 ? CBM_SZ_32 : (arr)->cap * PAIR_LEN;                   \
            void *new_items = cbm_arena_alloc((arena), (size_t)new_cap * sizeof(*(arr)->items)); \
            if (!new_items)                                                                      \
                return;                                                                          \
            if ((arr)->items && (arr)->count > 0) {                                              \
                memcpy(new_items, (arr)->items, (size_t)(arr)->count * sizeof(*(arr)->items));   \
            }                                                                                    \
            (arr)->items = new_items;                                                            \
            (arr)->cap = new_cap;                                                                \
        }                                                                                        \
    } while (0)

void cbm_defs_push(CBMDefArray *arr, CBMArena *a, CBMDefinition def) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = def;
}

void cbm_calls_push(CBMCallArray *arr, CBMArena *a, CBMCall call) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = call;
}

void cbm_imports_push(CBMImportArray *arr, CBMArena *a, CBMImport imp) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = imp;
}

void cbm_usages_push(CBMUsageArray *arr, CBMArena *a, CBMUsage usage) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = usage;
}

void cbm_throws_push(CBMThrowArray *arr, CBMArena *a, CBMThrow thr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = thr;
}

void cbm_rw_push(CBMRWArray *arr, CBMArena *a, CBMReadWrite rw) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = rw;
}

void cbm_typerefs_push(CBMTypeRefArray *arr, CBMArena *a, CBMTypeRef tr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = tr;
}

void cbm_envaccess_push(CBMEnvAccessArray *arr, CBMArena *a, CBMEnvAccess ea) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ea;
}

void cbm_typeassign_push(CBMTypeAssignArray *arr, CBMArena *a, CBMTypeAssign ta) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ta;
}

void cbm_stringref_push(CBMStringRefArray *arr, CBMArena *a, CBMStringRef sr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = sr;
}

void cbm_infrabinding_push(CBMInfraBindingArray *arr, CBMArena *a, CBMInfraBinding ib) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ib;
}

void cbm_impltrait_push(CBMImplTraitArray *arr, CBMArena *a, CBMImplTrait it) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = it;
}

void cbm_resolvedcall_push(CBMResolvedCallArray *arr, CBMArena *a, CBMResolvedCall rc) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = rc;
}

void cbm_channels_push(CBMChannelArray *arr, CBMArena *a, CBMChannel ch) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ch;
}

// --- String input reader (for parse_with_options) ---

typedef struct {
    const char *string;
    uint32_t length;
    /* #53: serve one "\n" at offset `length` -- see cbm_parse_source. */
    bool virtual_newline;
} CBMStringInput;

static const char *cbm_string_read(void *payload, uint32_t byte, TSPoint point,
                                   uint32_t *bytes_read) {
    (void)point;
    CBMStringInput *self = (CBMStringInput *)payload;
    if (byte >= self->length) {
        if (self->virtual_newline && byte == self->length) {
            *bytes_read = SKIP_ONE;
            return "\n";
        }
        *bytes_read = 0;
        return "";
    }
    *bytes_read = self->length - byte;
    return self->string + byte;
}

/* Where the parser stands after reading all `len` real bytes. */
static TSPoint cbm_source_end_point(const char *source, uint32_t len) {
    TSPoint end = {0, 0};
    const char *p = source;
    const char *stop = source + len;
    const char *nl;
    while (p < stop && (nl = memchr(p, '\n', (size_t)(stop - p))) != NULL) {
        end.row++;
        p = nl + SKIP_ONE;
    }
    end.column = (uint32_t)(stop - p);
    return end;
}

/* #53: parse every file as if its last line were terminated.
 *
 * Many grammars treat the line terminator as part of the construct it ends --
 * a markdown fence, heading or list marker, a Makefile recipe, a Dockerfile
 * instruction. A file whose last byte is not "\n" leaves that construct
 * unterminated: at best a zero-width MISSING token (excused at EOF by
 * cbm_is_eof_terminator_miss), at worst a WIDTH-BEARING error that costs the
 * whole last line or, for a one-line file, the whole file. The same bytes plus
 * one "\n" parse clean, so the absent newline is supplied instead of excused.
 *
 * The parser reads one virtual "\n" past EOF; afterwards a single tree edit
 * deletes it again. tree-sitter's own edit arithmetic then clamps every node
 * that reached into the virtual byte back to the real end -- byte offsets AND
 * row/column points -- so every consumer (def line ranges, node text, call
 * byte spans, parse-coverage ranges, the retained tree the LSP passes reuse)
 * sees only real bytes and real lines. The caller's buffer is never copied or
 * written.
 *
 * A file that is empty or already ends in "\n" is parsed exactly as before. */
TSTree *cbm_parse_source(TSParser *parser, const char *source, uint32_t source_len,
                         TSParseOptions opts) {
    bool unterminated = source_len > 0 && source[source_len - SKIP_ONE] != '\n';
    CBMStringInput input = {source, source_len, unterminated};
    TSInput ts_input = {&input, cbm_string_read, TSInputEncodingUTF8, NULL};
    TSTree *tree = ts_parser_parse_with_options(parser, NULL, ts_input, opts);
    if (tree && input.virtual_newline) {
        TSPoint end = cbm_source_end_point(source, source_len);
        TSInputEdit drop_virtual_newline = {
            .start_byte = source_len,
            .old_end_byte = source_len + SKIP_ONE,
            .new_end_byte = source_len,
            .start_point = end,
            .old_end_point = {end.row + SKIP_ONE, 0},
            .new_end_point = end,
        };
        ts_tree_edit(tree, &drop_virtual_newline);
    }
    return tree;
}

// --- Parse timeout callback ---

/* Budget for the tree-sitter progress callback (upstream 52701625). The
 * primary gate is per-thread CPU time: a worker descheduled under contention
 * burns wall time but not CPU, so a parseable file must not be abandoned (and
 * its defs, and every cross-file edge they anchor, dropped) because the budget
 * elapsed in wall-clock against near-zero CPU. A generous wall ceiling stays as
 * a backstop so a stuck parse still ends in bounded time. */
#define CBM_PARSE_WALL_CEILING_FACTOR 12ULL /* ~60 s ceiling for the 5 s CPU budget */

typedef struct {
    uint64_t cpu_deadline_ns; // trip once this thread's CPU time passes it
    uint64_t wall_ceiling_ns; // hard wall backstop for a stuck parse
    uint64_t wall_offset_ns;  // CBM_TEST_WALL_STALL_ON: emulated descheduling
    bool tripped;             // the budget, not tree-sitter, ended the parse
} CBMParseBudget;

static bool cbm_timeout_cb(TSParseState *state) {
    CBMParseBudget *budget = (CBMParseBudget *)state->payload;
    bool over = cbm_thread_cpu_time_ns() > budget->cpu_deadline_ns ||
                now_ns() + budget->wall_offset_ns > budget->wall_ceiling_ns;
    if (over) {
        budget->tripped = true;
    }
    return over;
}

// --- Thread-local parser pool ---
// TSParser is not thread-safe, but can be reused across files on the same thread.
// We keep one parser per thread, and just switch language as needed.
// This avoids ~70K ts_parser_new()/ts_parser_delete() cycles on large repos.

static CBM_TLS TSParser *tl_parser = NULL;
static CBM_TLS CBMLanguage tl_parser_lang = CBM_LANG_COUNT; // invalid sentinel

// Get or create a thread-local parser configured for the given language.
static TSParser *get_thread_parser(const TSLanguage *ts_lang, CBMLanguage lang) {
    if (!tl_parser) {
        tl_parser = ts_parser_new();
        if (!tl_parser) {
            return NULL;
        }
        tl_parser_lang = CBM_LANG_COUNT;
    }
    if (tl_parser_lang != lang) {
        ts_parser_set_language(tl_parser, ts_lang);
        tl_parser_lang = lang;
    }
    return tl_parser;
}

// --- Allocator binding (defense-in-depth, #424) ---

/* Bind tree-sitter and sqlite3 to mimalloc explicitly so a correct
 * binary does NOT depend on the fragile MI_OVERRIDE symbol override. Under
 * MI_OVERRIDE=1 — particularly the Windows static-MinGW link with
 * --allow-multiple-definition — `malloc`/`free` can resolve to DIFFERENT
 * allocators (mimalloc vs the CRT) inside third-party libs, so a block
 * allocated by mimalloc gets freed by the CRT (or vice-versa), corrupting the
 * heap freelist (#424). Binding each library through one explicit allocator
 * eliminates that mismatch class generically, on every platform.
 *
 * Guarded to the production build (CBM_BIND_TS_ALLOCATOR=1, which CFLAGS_PROD
 * defines alongside MI_OVERRIDE=1). The test build is CRT + ASan, where binding
 * to mimalloc would mismatch ASan/CRT frees — there these binds compile to
 * no-ops and the build stays unchanged. */

#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
#include <assert.h>

/* sqlite3 mem methods backed by mimalloc. sqlite's xMalloc/xRealloc/xSize use
 * `int` sizes; wrap with size_t casts. xRoundup rounds to an 8-byte boundary
 * (sqlite requires 8-byte-aligned roundup, and mimalloc honors that alignment).
 * Field order matches struct sqlite3_mem_methods exactly:
 * xMalloc, xFree, xRealloc, xSize, xRoundup, xInit, xShutdown, pAppData. */
static void *cbm_sqlite_malloc(int n) {
    return mi_malloc((size_t)n);
}
static void cbm_sqlite_free(void *p) {
    mi_free(p);
}
static void *cbm_sqlite_realloc(void *p, int n) {
    return mi_realloc(p, (size_t)n);
}
static int cbm_sqlite_size(void *p) {
    return (int)mi_usable_size(p);
}
static int cbm_sqlite_roundup(int n) {
    return (n + 7) & ~7; /* round up to 8-byte boundary */
}
static int cbm_sqlite_meminit(void *appdata) {
    (void)appdata;
    return SQLITE_OK;
}
static void cbm_sqlite_memshutdown(void *appdata) {
    (void)appdata;
}
#endif /* CBM_BIND_TS_ALLOCATOR */

void cbm_alloc_init(void) {
#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
    static int alloc_bound = 0; /* single-threaded startup; plain int is fine */
    if (alloc_bound) {
        return;
    }
    alloc_bound = 1;

    /* tree-sitter runtime (was previously bound in cbm_init; consolidated here). */
    ts_set_allocator(mi_malloc, mi_calloc, mi_realloc, mi_free);

    /* sqlite3. SQLITE_CONFIG_MALLOC MUST run before sqlite3_initialize / the
     * first sqlite3_open* — otherwise sqlite3_config returns SQLITE_MISUSE
     * silently and the binding is ignored. cbm_alloc_init() runs as the very
     * first statement of main(), before cbm_mcp_server_new → cbm_store_open*. */
    static sqlite3_mem_methods cbm_sqlite_mem = {
        cbm_sqlite_malloc,      /* xMalloc */
        cbm_sqlite_free,        /* xFree */
        cbm_sqlite_realloc,     /* xRealloc */
        cbm_sqlite_size,        /* xSize */
        cbm_sqlite_roundup,     /* xRoundup */
        cbm_sqlite_meminit,     /* xInit */
        cbm_sqlite_memshutdown, /* xShutdown */
        NULL,                   /* pAppData */
    };
    int sqlite_rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &cbm_sqlite_mem);
    assert(sqlite_rc == SQLITE_OK && "SQLITE_CONFIG_MALLOC must run before sqlite3_initialize");
    (void)sqlite_rc;
#endif /* CBM_BIND_TS_ALLOCATOR */
}

// --- Init/Shutdown ---

static int cbm_initialized = 0;

int cbm_init(void) {
    if (cbm_initialized) {
        return 0;
    }
    enum { CBM_INIT_DONE = 1 };
    cbm_initialized = CBM_INIT_DONE;
    /* Defense-in-depth allocator binds (idempotent). main() calls cbm_alloc_init
     * first; this covers non-main entry points (pipeline passes call cbm_init).
     * For sqlite the SQLITE_CONFIG_MALLOC bind only takes effect if it runs
     * before sqlite initializes — main() guarantees that ordering; here it is a
     * best-effort idempotent re-assert for paths that never hit main(). */
    cbm_alloc_init();
    return 0;
}

void cbm_destroy_thread_parser(void) {
    // Full cleanup: delete the parser. Call on worker thread exit.
    if (tl_parser) {
        ts_parser_delete(tl_parser);
        tl_parser = NULL;
        tl_parser_lang = CBM_LANG_COUNT;
    }
}

void cbm_shutdown(void) {
    // Clean up thread-local parser for the calling thread.
    // Note: other threads' TLS parsers are freed when those threads exit.
    cbm_destroy_thread_parser();
    cbm_initialized = 0;
}

// --- Bottleneck call-name classification (language-agnostic heuristics) ---

// Case-insensitive equality for short callee names.
static bool name_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool name_in_set(const char *name, const char *const *set) {
    for (const char *const *s = set; *s; s++) {
        if (name_ieq(name, *s)) {
            return true;
        }
    }
    return false;
}

// Linear-scan / membership calls: a hit inside a loop is the textbook hidden
// O(n^2) (cf. Olivo et al., PLDI'15) that syntactic loop-depth alone misses.
static bool is_linear_scan_name(const char *n) {
    static const char *const set[] = {"find",    "indexof",   "contains", "includes", "search",
                                      "lookup",  "strstr",    "strchr",   "strrchr",  "memchr",
                                      "find_if", "findindex", "count",    "index",    NULL};
    return name_in_set(n, set);
}

// Allocation / growable-append calls: repeated inside a loop is the classic
// accidental reallocation / string-concat O(n^2). Names are deliberately
// conservative; meaningless in some languages → simply never matches there.
static bool is_alloc_name(const char *n) {
    static const char *const set[] = {"malloc",  "calloc",    "realloc",      "strdup", "strndup",
                                      "append",  "push_back", "emplace_back", "concat", "strcat",
                                      "strncat", "push",      "pushback",     NULL};
    return name_in_set(n, set);
}

// Extract the receiver identifier from a def's receiver text — Go's
// "(s *Store)" / "(s Store)" → "s". Stores the identifier start in *out and
// returns its length; returns 0 for unnamed receivers ("(*Store)", "(Store)"),
// where no second token follows the identifier (a lone token is the TYPE, not
// a name — such methods have no receiver variable to call through anyway).
static size_t receiver_ident(const char *recv_text, const char **out) {
    const char *p = recv_text;
    if (*p == '(') {
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *start = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
           *p == '_') {
        p++;
    }
    size_t len = (size_t)(p - start);
    if (len == 0) {
        return 0; // "(*Store)": leading '*', no identifier
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == ')' || *p == '\0') {
        return 0; // "(Store)": single token is the type, receiver unnamed
    }
    *out = start;
    return len;
}

// Whether a callee expression targets the same instance/class as the enclosing
// def, i.e. counts as genuine self-recursion rather than a same-named call on a
// different receiver. callee_name may be bare ("recur") or qualified
// ("self.recur", "this.recur", "super().save", "axios.get", "self.obj.recur").
//
// Bare names have no receiver → assume self-call (free function calling itself
// by bare name; preserves prior behavior). Qualified names: the receiver chain
// is everything before the LAST '.', and the WHOLE chain must name the same
// object — self/this/cls/@self, or the enclosing def's own receiver identifier
// (Go: `s` in `func (s *Store) save()`, from CBMDefinition.receiver). Matching
// the whole chain (not its first segment) keeps self.obj.recur() out: it
// targets self's FIELD obj, a different object. super() is the parent class and
// any other receiver (axios, console, ...) a different target. See #599.
static bool is_self_receiver(const char *callee_name, const char *def_receiver) {
    if (!callee_name || !callee_name[0]) {
        return false;
    }
    const char *dot = strrchr(callee_name, '.');
    if (!dot) {
        return true; // bare name → self-recursion candidate
    }
    size_t rlen = (size_t)(dot - callee_name);
    static const char *const self_receivers[] = {"self", "this", "cls", "@self", NULL};
    for (int i = 0; self_receivers[i]; i++) {
        size_t sl = strlen(self_receivers[i]);
        if (rlen == sl && strncmp(callee_name, self_receivers[i], sl) == 0) {
            return true;
        }
    }
    if (def_receiver) {
        const char *rid = NULL;
        size_t ril = receiver_ident(def_receiver, &rid);
        if (ril > 0 && ril == rlen && strncmp(callee_name, rid, ril) == 0) {
            return true; // call through the enclosing method's own receiver
        }
    }
    return false; // super() / axios / console / self.obj / any other receiver
}

// Count parameters from a signature string like "(int a, Foo* b, cb (*)(int,int))".
// Fallback for languages where param_names isn't populated (e.g. C keeps only the
// signature text). Counts commas at the top paren level; treats "()"/"(void)" as 0.
// Approximate by design (a structural smell, not an exact arity).
static int count_params_from_signature(const char *sig) {
    if (!sig) {
        return 0;
    }
    const char *p = sig;
    while (*p && *p != '(') {
        p++;
    }
    if (*p != '(') {
        return 0;
    }
    p++;
    const char *list = p;
    int depth = 0;
    int commas = 0;
    bool any = false;
    for (; *p; p++) {
        char ch = *p;
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if (ch == ')') {
            if (depth == 0) {
                break;
            }
            depth--;
        } else if (ch == ']' || ch == '}' || ch == '>') {
            if (depth > 0) {
                depth--;
            }
        } else if (ch == ',' && depth == 0) {
            commas++;
        } else if (!isspace((unsigned char)ch)) {
            any = true;
        }
    }
    if (!any) {
        return 0; /* "()" */
    }
    if (commas == 0) {
        while (*list == ' ' || *list == '\t') {
            list++;
        }
        if (strncmp(list, "void", 4) == 0 &&
            (list[4] == ')' || list[4] == ' ' || list[4] == '\0')) {
            return 0; /* C "(void)" */
        }
    }
    return commas + 1;
}

// --- Main extraction function ---

/* Test-only deterministic fault injection for the crash/hang supervisor tests.
 * Gated entirely behind env vars that are never set in production; a matching
 * rel_path either aborts (a fault signal the supervisor classifies as a crash)
 * or spins forever (an external-scanner infinite loop the quiet-timeout kills).
 * This gives an honest guard — green iff the supervisor actually contains a real
 * fault — instead of a fixture that may stop faulting once a root cause is fixed. */
/* Crash-supervisor per-file marker JOURNAL (Stage 3c skip-and-continue,
 * parallel-safe). Recovery re-runs are PARALLEL (there are no sequential
 * production runs), so a single overwrite-style marker would race across
 * workers and — worse — go stale during non-extract phases, blaming
 * whatever file was extracted LAST (that mis-quarantined four innocent
 * ms-typescript fixtures, one 15-minute retry at a time). Instead every
 * worker APPENDS one short line per event: "S <rel_path>" when it STARTS
 * work on a file, "D <rel_path>" when it finishes it. A single short
 * append of one line is atomic in practice on every target platform, and
 * the parent discards a torn final line by design. The parent's suspect
 * set after a crash/hang = files with an S but no D — exactly the
 * in-flight set; a file is only quarantined after appearing in the
 * suspect set of TWO CONSECUTIVE failed runs, so a stale or merely
 * unlucky in-flight file is never quarantined alone. The env var is set
 * solely by the supervisor during recovery — a no-op on normal runs. */
static void cbm_index_mark(const char *rel_path, char event) {
    const char *mf = getenv("CBM_INDEX_MARKER_FILE");
    if (!mf || !mf[0] || !rel_path || !rel_path[0]) {
        return;
    }
    FILE *f = cbm_fopen(mf, "ab");
    if (f) {
        (void)fprintf(f, "%c %s\n", event, rel_path);
        (void)fclose(f);
    }
}

void cbm_index_mark_start(const char *rel_path) {
    cbm_index_mark(rel_path, 'S');
}

void cbm_index_mark_done(const char *rel_path) {
    cbm_index_mark(rel_path, 'D');
}

/* ── Crash-quarantine set (Stage 3c skip-and-continue) ──────────────────────
 * After a crash the supervisor re-runs the worker single-threaded, passing
 * CBM_INDEX_QUARANTINE_FILE — a newline-delimited list of repo-relative paths
 * that already crashed the indexer and MUST NOT be extracted again. Owned here,
 * next to the other env-driven extract hooks (marker + fault injector), so the
 * single hard guard lives at the one choke point every pass funnels through
 * (cbm_extract_file): whether a pass re-extracts from disk on a cache miss
 * or extracts fresh, a quarantined file
 * short-circuits to an empty result and never reaches the parser/crash. The
 * pipeline extract loops separately REPORT the skip as phase="crash" via
 * cbm_index_is_quarantined() so the crasher surfaces in the response skipped[].
 * Loaded once, lazily; read-only after load (safe for the parallel workers,
 * though recovery runs single-threaded). Unset env ⇒ empty set ⇒ cheap no-op. */
static CBMHashTable *g_quarantine_set = NULL;
enum { CBM_QSET_UNINIT = 0, CBM_QSET_INITING = 1, CBM_QSET_INITED = 2 };
static atomic_int g_quarantine_state = CBM_QSET_UNINIT;

static void cbm_quarantine_load(void) {
    const char *qf = getenv("CBM_INDEX_QUARANTINE_FILE");
    if (!qf || !qf[0]) {
        return; /* normal path: empty set */
    }
    FILE *f = cbm_fopen(qf, "rb");
    if (!f) {
        return;
    }
    CBMHashTable *set = cbm_ht_create(16);
    if (!set) {
        (void)fclose(f);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        /* Line format: "path\tphase" where phase is "crash" or "hang". A bare
         * "path" line (no tab) is tolerated and defaults to phase "crash" for
         * backward compatibility with older quarantine files. */
        char *tab = strchr(line, '\t');
        const char *phase = "crash";
        if (tab) {
            *tab = '\0';
            if (tab[1]) {
                phase = tab + 1;
            }
        }
        if (line[0] == '\0') {
            continue; /* empty path (line began with a tab) — skip */
        }
        /* The table borrows the key + value pointers, so dup both. Intentionally
         * never freed: the set lives for the whole (short-lived worker) process.
         * The value stores the phase so cbm_index_quarantine_phase() can report
         * "crash" vs "hang"; membership (cbm_index_is_quarantined) is value != NULL. */
        char *key = cbm_strdup(line);
        char *pval = cbm_strdup(phase);
        if (key && pval) {
            cbm_ht_set(set, key, (void *)pval);
        }
    }
    (void)fclose(f);
    g_quarantine_set = set;
}

bool cbm_index_is_quarantined(const char *rel_path) {
    if (!rel_path || !rel_path[0]) {
        return false;
    }
    int state = atomic_load(&g_quarantine_state);
    if (state != CBM_QSET_INITED) {
        /* First caller wins the CAS and loads; racers spin until INITED.
         * Same once-init pattern as cbm_ui_log_init (http_server.c). */
        state = CBM_QSET_UNINIT;
        if (atomic_compare_exchange_strong(&g_quarantine_state, &state, CBM_QSET_INITING)) {
            cbm_quarantine_load();
            atomic_store(&g_quarantine_state, CBM_QSET_INITED);
        } else {
            while (atomic_load(&g_quarantine_state) != CBM_QSET_INITED) {
                cbm_usleep(1000); /* 1ms */
            }
        }
    }
    return g_quarantine_set && cbm_ht_has(g_quarantine_set, rel_path);
}

const char *cbm_index_quarantine_phase(const char *rel_path) {
    /* cbm_index_is_quarantined drives the lazy once-load and returns true only
     * when the set is loaded and holds rel_path — so on true, g_quarantine_set is
     * non-NULL and the stored value is the phase string ("crash"/"hang"). */
    if (!cbm_index_is_quarantined(rel_path)) {
        return NULL;
    }
    return (const char *)cbm_ht_get(g_quarantine_set, rel_path);
}

static void cbm_test_fault_inject(const char *rel_path) {
    if (!rel_path || !rel_path[0]) {
        return;
    }
    const char *crash_on = getenv("CBM_TEST_CRASH_ON");
    if (crash_on && crash_on[0] && strstr(rel_path, crash_on)) {
        abort(); /* SIGABRT → WIFSIGNALED → classified as a crash */
    }
    const char *hang_on = getenv("CBM_TEST_HANG_ON");
    if (hang_on && hang_on[0] && strstr(rel_path, hang_on)) {
        for (;;) {
            /* Busy-spin: the supervisor's quiet-timeout kills + reports us. */
        }
    }
}

static CBMFileResult *cbm_extract_file_impl(const char *source, int source_len,
                                            CBMLanguage language, const char *project,
                                            const char *rel_path, int64_t timeout_micros,
                                            const char **extra_defines, const char **include_paths);

/* Best-effort parse-coverage collection (#963). Walks only the has_error paths
 * of the tree and records the 1-based line ranges of the TOP-MOST ERROR/MISSING
 * nodes (does not descend into an error subtree — one range per failed region).
 * Bounded by CBM_MAX_ERROR_REGIONS so pathological input can't blow up the
 * output. The ranges mark where constructs were dropped; they are a detection
 * aid, never a completeness proof.
 *
 * `dropped` counts the ranges the cap threw away. It exists so a clipped list
 * cannot read as a complete one: cbm_error_ranges_str turns a non-zero count
 * into a trailing "+<N>" marker. */
#define CBM_MAX_ERROR_REGIONS 256
typedef struct {
    uint32_t starts[CBM_MAX_ERROR_REGIONS];
    uint32_t ends[CBM_MAX_ERROR_REGIONS];
    int count;
    int dropped;
} cbm_error_regions_t;

static void cbm_error_regions_push(cbm_error_regions_t *acc, TSNode n) {
    TSPoint start = ts_node_start_point(n);
    TSPoint end = ts_node_end_point(n);
    uint32_t start_line = start.row + 1;
    uint32_t end_line = end.row + 1;

    /* A node that ends at column 0 stopped right after the previous line's
     * newline, so it holds no text on the row it points at. Counting that row
     * named a line past the end of the file whenever the region ran to EOF:
     * scripts/setup-windows.ps1 has 326 lines and reported "245-327". */
    if (end.column == 0 && end.row > start.row) {
        end_line = end.row;
    }

    /* One line can carry several error nodes, and repeating the same line range
     * says nothing new. Line 113 of scripts/setup-windows.ps1 has two error
     * nodes, at columns 25-29 and 31-32, and the report read "113-113,113-113".
     * Drop the repeat.
     *
     * Only an EXACT repeat of the range already open is dropped. Do not merge
     * ranges that merely overlap. Each range is judged separately later by
     * cbm_region_is_recovered, which asks whether definitions starting inside
     * that range cover it. Two ranges with the same numbers always get the same
     * verdict, so collapsing them changes nothing. Two DIFFERENT ranges do not:
     * merging 3-3 into 2-3 hands the wider range's covering definition to an
     * error the definition does not explain, and a real parse failure then
     * disappears from the report. tests/test_parse_coverage.c pins that case in
     * perl_malformed_source_remains_partial_issue1838.
     *
     * This runs BEFORE the cap check, so a dropped repeat never counts as a
     * range the cap threw away. */
    if (acc->count > 0 && start_line == acc->starts[acc->count - 1] &&
        end_line == acc->ends[acc->count - 1]) {
        return;
    }

    if (acc->count >= CBM_MAX_ERROR_REGIONS) {
        acc->dropped++;
        return;
    }
    acc->starts[acc->count] = start_line;
    acc->ends[acc->count] = end_line;
    acc->count++;
}

/* #1610: a file that does not end with a newline leaves the grammar's
 * mandatory line terminator MISSING. That node is ZERO-WIDTH and sits at EOF.
 *
 * It is not a miss. The parser consumed no source for it — start_byte ==
 * end_byte — so by construction nothing was dropped: no construct can live in
 * a zero-byte span, and every real instruction above it parsed normally. This
 * is a property of the grammar's terminator rule, not of the file.
 *
 * Flagging it made the verdict arbitrary. Grammars whose terminator token is
 * VISIBLE (dockerfile, tcl, fish, gomod, hyprlang) reported parse_partial for
 * a missing final newline; grammars whose terminator is HIDDEN (ini, fsharp,
 * beancount, requirements, gitignore, sshconfig, kconfig) reported nothing for
 * exactly the same omission, because a hidden node is invisible to
 * ts_node_child(). Whether a user was told their file was partially parsed
 * depended on a grammar-authoring accident.
 *
 * The cost was not cosmetic: a phantom parse_partial writes a
 * "<project>::missed" shadow row, and until #1609 that row made the project
 * fail cross-repo validation as both source and target.
 *
 * Deliberately narrow — ZERO-WIDTH AT EOF ONLY. A MISSING or ERROR node with
 * WIDTH still counts even at EOF (a Makefile whose last recipe line is
 * unterminated really does lose the recipe), and anything before EOF is
 * untouched.
 *
 * #1746: the Dockerfile grammar places that zero-width missing newline before
 * trailing whitespace rather than at raw EOF. Preserve the broad exact-EOF
 * rule above; only extend it past blanks when the missing token is specifically
 * a newline.
 *
 * #53: cbm_parse_source now supplies the absent final newline, so a MISSING
 * newline at EOF no longer arises from it; the rule stays for the other
 * zero-width terminators a grammar can leave at EOF. */
static bool cbm_is_blank_not_newline(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

static bool cbm_is_eof_terminator_miss(TSNode n, const char *source, int source_len) {
    if (!ts_node_is_missing(n) || source_len < 0) {
        return false;
    }
    uint32_t start = ts_node_start_byte(n);
    uint32_t end = ts_node_end_byte(n);
    if (start != end || end > (uint32_t)source_len) {
        return false;
    }
    if (end == (uint32_t)source_len) {
        return true;
    }
    if (!source || strcmp(ts_node_type(n), "\n") != 0) {
        return false;
    }
    for (uint32_t i = end; i < (uint32_t)source_len; i++) {
        if (!cbm_is_blank_not_newline(source[i])) {
            return false;
        }
    }
    return true;
}

/* Walks to the end even after the cap is full, so `dropped` is the real number
 * of ranges lost rather than a lower bound. This costs little: the walk never
 * descends into an ERROR subtree — it records the top-most node and moves on —
 * so it only visits the spine of nodes that contain an error, plus one level.
 *
 * That descent follows the nesting of the construct that failed — every
 * ancestor of the failing node carries ts_node_has_error — so a recursive walk
 * used one stack frame per nesting level of the indexed file, and a deep
 * enough nest ran the worker thread out of stack. The cursor keeps that path
 * on the heap. The nodes visited and their order are those of the child-index
 * loop it replaced: the cursor stops on exactly the children ts_node_child
 * enumerates. */
static void cbm_collect_error_regions(TSNode n, cbm_error_regions_t *acc, const char *source,
                                      int source_len) {
    TSTreeCursor cursor = ts_tree_cursor_new(n);
    if (!ts_tree_cursor_goto_first_child(&cursor)) {
        ts_tree_cursor_delete(&cursor);
        return;
    }
    int depth = 0; /* 0 while on a direct child of n */
    for (;;) {
        TSNode c = ts_tree_cursor_current_node(&cursor);
        bool descend = false;
        if (ts_node_is_missing(c) || strcmp(ts_node_type(c), "ERROR") == 0) {
            /* An absent final newline only means nothing was dropped. Anything
             * else is the top-most region; do not descend. */
            if (!cbm_is_eof_terminator_miss(c, source, source_len)) {
                cbm_error_regions_push(acc, c);
            }
        } else if (ts_node_has_error(c)) {
            descend = true;
        }
        if (descend && ts_tree_cursor_goto_first_child(&cursor)) {
            depth++;
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
            if (depth == 0) {
                ts_tree_cursor_delete(&cursor);
                return;
            }
            (void)ts_tree_cursor_goto_parent(&cursor);
            depth--;
        }
    }
}

/* Recovery subtraction (#963): tree-sitter error recovery plus the
 * ERROR-descending def walker often still extract constructs INSIDE a failed
 * region (verified: a function in an #ifdef-split ERROR region and even a
 * `def broken(:` both came back as defs). A region whose every line is
 * covered by definitions that START inside it is definitely recovered — its
 * constructs ARE in the graph — so flagging it would be a false miss.
 * Container defs (Module/Package) are ignored: a file-spanning Module node is
 * not evidence the region's constructs survived. Conservative: partially
 * covered regions stay flagged. */
static bool cbm_region_is_recovered(uint32_t rs, uint32_t re, const CBMDefArray *defs) {
    enum { MAX_COVER_DEFS = 256 };
    uint32_t starts[MAX_COVER_DEFS];
    uint32_t ends[MAX_COVER_DEFS];
    int n = 0;
    for (int i = 0; i < defs->count && n < MAX_COVER_DEFS; i++) {
        const CBMDefinition *d = &defs->items[i];
        if (!d->label || strcmp(d->label, "Module") == 0 || strcmp(d->label, "Package") == 0) {
            continue;
        }
        if (d->start_line < rs || d->start_line > re) {
            continue; /* recovery evidence must originate inside the region */
        }
        starts[n] = d->start_line;
        ends[n] = d->end_line < d->start_line ? d->start_line : d->end_line;
        n++;
    }
    if (n == 0) {
        return false;
    }
    /* Insertion-sort by start, then sweep for gaps in [rs, re]. */
    for (int i = 1; i < n; i++) {
        uint32_t s = starts[i];
        uint32_t e = ends[i];
        int j = i - 1;
        while (j >= 0 && starts[j] > s) {
            starts[j + 1] = starts[j];
            ends[j + 1] = ends[j];
            j--;
        }
        starts[j + 1] = s;
        ends[j + 1] = e;
    }
    uint32_t covered_to = rs - 1;
    for (int i = 0; i < n; i++) {
        if (starts[i] > covered_to + 1) {
            return false; /* uncovered gap */
        }
        if (ends[i] > covered_to) {
            covered_to = ends[i];
        }
    }
    return covered_to >= re;
}

static bool cbm_identifier_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static void cbm_subtract_recovered_regions(cbm_error_regions_t *regs, const CBMDefArray *defs) {
    int kept = 0;
    for (int i = 0; i < regs->count; i++) {
        if (!cbm_region_is_recovered(regs->starts[i], regs->ends[i], defs)) {
            regs->starts[kept] = regs->starts[i];
            regs->ends[kept] = regs->ends[i];
            kept++;
        }
    }
    regs->count = kept;
}

/* #1071: a function-like macro invocation whose argument is a type token
 * (e.g. ALLOC(int, n)) makes tree-sitter's C/C++ grammar emit an ERROR node — it
 * parses `int` in expression position — which would be recorded as a parse_partial
 * coverage gap. But the macro is #defined in THIS file, so nothing is actually
 * missing from the graph; it's a benign call the grammar can't parse without the
 * preprocessor. True if the [start_line, end_line] span contains a call `NAME(` to
 * a file-defined function-like macro (Macro label + a parameter signature). */
static bool cbm_byte_span_is_macro_invocation(const char *src, int src_len, int span_start,
                                              int span_end, const CBMDefArray *defs) {
    if (!src || src_len <= 0 || !defs || span_start < 0 || span_end > src_len ||
        span_start >= span_end) {
        return false;
    }
    for (int di = 0; di < defs->count; di++) {
        const CBMDefinition *d = &defs->items[di];
        /* Function-like macros only: an object-like macro (#define PI 3.14) has no
         * parameter signature and can't be mistaken for a call. */
        if (!d->label || strcmp(d->label, "Macro") != 0 || !d->signature || !d->name ||
            !d->name[0]) {
            continue;
        }
        int nlen = (int)strlen(d->name);
        for (int pos = span_start; pos + nlen <= span_end; pos++) {
            if (strncmp(src + pos, d->name, (size_t)nlen) != 0 ||
                (pos > 0 && cbm_identifier_char(src[pos - 1])) ||
                (pos + nlen < src_len && cbm_identifier_char(src[pos + nlen]))) {
                continue;
            }
            int open = pos + nlen;
            while (open < span_end && isspace((unsigned char)src[open])) {
                open++;
            }
            if (open < span_end && src[open] == '(') {
                return true; /* NAME( ... ) — an invocation of this file's macro */
            }
        }
    }
    return false;
}

/* Same question by line number: walks the source to find the span. */
static bool cbm_span_is_macro_invocation(const char *src, int src_len, uint32_t start_line,
                                         uint32_t end_line, const CBMDefArray *defs) {
    if (!src || src_len <= 0 || !defs || start_line == 0 || end_line < start_line) {
        return false;
    }
    int span_start = 0;
    uint32_t line = 1;
    while (span_start < src_len && line < start_line) {
        if (src[span_start++] == '\n') {
            line++;
        }
    }
    if (line != start_line) {
        return false;
    }
    int span_end = span_start;
    while (span_end < src_len && line <= end_line) {
        if (src[span_end++] == '\n') {
            line++;
        }
    }
    return cbm_byte_span_is_macro_invocation(src, src_len, span_start, span_end, defs);
}

/* True if [rs, re] is fully enclosed by an extracted callable definition (a
 * Function/Method body). A macro invocation INSIDE a real function body is an
 * expression-level use where nothing is missing (#1071). A TOP-LEVEL invocation
 * is different: the macro may itself expand to a definition that the original
 * span doesn't contain (#949), which must stay flagged. Restricting the #1071
 * suppression to in-body calls keeps that #949 gap honest and fails safe. */
static bool cbm_region_inside_callable(uint32_t rs, uint32_t re, const CBMDefArray *defs) {
    for (int i = 0; i < defs->count; i++) {
        const CBMDefinition *d = &defs->items[i];
        if (!d->label) {
            continue;
        }
        if (strcmp(d->label, "Function") != 0 && strcmp(d->label, "Method") != 0 &&
            strcmp(d->label, "Constructor") != 0 && strcmp(d->label, "Destructor") != 0) {
            continue;
        }
        if (d->start_line <= rs && d->end_line >= re && d->end_line > d->start_line) {
            return true;
        }
    }
    return false;
}

static void cbm_subtract_macro_invocation_regions(cbm_error_regions_t *regs,
                                                  const CBMDefArray *defs, const char *src,
                                                  int src_len) {
    int kept = 0;
    for (int i = 0; i < regs->count; i++) {
        bool benign =
            cbm_span_is_macro_invocation(src, src_len, regs->starts[i], regs->ends[i], defs) &&
            cbm_region_inside_callable(regs->starts[i], regs->ends[i], defs);
        if (!benign) {
            regs->starts[kept] = regs->starts[i];
            regs->ends[kept] = regs->ends[i];
            kept++;
        }
    }
    regs->count = kept;
}

/* Serialize collected regions as "start-end,start-end,..." into the arena. */
/* Share of a file one range must cover before the range stops being advice and
 * becomes noise. 80% is well clear of anything real: the widest single range in
 * this repo covers 25.5% of its file, and the next widest 3.9%. */
#define CBM_UNUSABLE_PCT 80

/* Number of 1-based lines in `src`. A file that does not end with a newline
 * still has a last line, so the count is separators plus one. */
static uint32_t cbm_count_lines(const char *src, int src_len) {
    uint32_t n = 1;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '\n' && i + 1 < src_len) {
            n++;
        }
    }
    return n;
}

/* Serialize collected regions as "start-end,start-end,...", with a trailing
 * ",+<N>" when the cap threw N ranges away.
 *
 * The marker must stay a SUFFIX and nothing else. Every reader stops at the
 * first token that is not a range, so a marker in the middle of a string
 * silently hides everything after it, so strip markers before joining two
 * serialized lists.
 *
 * N can be non-zero while the kept list is short, because the recovery and
 * macro rules run after collection and remove ranges the cap never saw. That
 * still reports honestly: the cap bound, so what was lost is unknown. */
static const char *cbm_error_ranges_str(CBMArena *a, const cbm_error_regions_t *regs) {
    if (regs->count <= 0 && regs->dropped <= 0) {
        return NULL;
    }
    enum { RANGE_MAX = 24 }; /* "4294967295-4294967295," */
    char *buf = (char *)cbm_arena_alloc(a, (size_t)(regs->count + 1) * RANGE_MAX);
    if (!buf) {
        return NULL;
    }
    size_t off = 0;
    for (int i = 0; i < regs->count; i++) {
        off += (size_t)snprintf(buf + off, RANGE_MAX, "%s%u-%u", i ? "," : "", regs->starts[i],
                                regs->ends[i]);
    }
    if (regs->dropped > 0) {
        snprintf(buf + off, RANGE_MAX, "%s+%d", off ? "," : "", regs->dropped);
    }
    return buf;
}

/* ── Same-file duplicate definitions: the `variants` property ──────────────
 *
 * The graph keeps one node per qualified name. When one file defines a QN more
 * than once under one label -- both branches of an #if, a macro redefined per
 * platform, C++ overloads -- the node shows the winner's span and the other
 * definitions leave no trace. The member the graph keeps gets the group's
 * spans as CBMDefinition.variants (format in cbm.h), so the surviving node
 * carries them.
 *
 * C-preprocessor languages only (cbm_is_c_preprocessor_lang, the caller's
 * gate): conditional compilation is what the property describes. A repeated
 * QN elsewhere is a different matter -- a JSON file repeats one key thousands
 * of times -- and gets no list.
 *
 * Computed here from this file's defs alone: a pure function of the file,
 * hence deterministic, identical on the sequential and the parallel path, and
 * owned by the file under incremental re-extraction. Two defs with the same
 * span are one definition seen twice and count once; a group left with a
 * single span gets nothing. */

typedef struct {
    uint64_t key; /* hash of (QN, label) */
    uint32_t start;
    uint32_t end;
    int idx; /* index into result->defs */
} cbm_variant_ref_t;

static int cbm_variant_ref_cmp(const void *pa, const void *pb) {
    const cbm_variant_ref_t *a = (const cbm_variant_ref_t *)pa;
    const cbm_variant_ref_t *b = (const cbm_variant_ref_t *)pb;
    if (a->key != b->key) {
        return a->key < b->key ? -1 : 1;
    }
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    if (a->end != b->end) {
        return a->end < b->end ? -1 : 1;
    }
    return (a->idx > b->idx) - (a->idx < b->idx);
}

/* FNV-1a over the QN, a separator and the label. */
static uint64_t cbm_variant_key(const char *qn, const char *label) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = qn; *p; p++) {
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    }
    h = (h ^ 0xffU) * 1099511628211ULL;
    for (const char *p = label; *p; p++) {
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    }
    return h;
}

/* refs[0..n) is one (QN, label) group in span order: give every member the
 * list of the group's distinct spans. */
static void cbm_variant_assign(CBMFileResult *result, const cbm_variant_ref_t *refs, int n) {
    /* ,{"file_path":"","start_line":4294967295,"end_line":4294967295} + path;
     * cbm_json_escape writes at most 6 bytes per source byte (\u00XX). */
    enum { VARIANT_ENTRY_FIXED = 64, JSON_ESCAPE_GROWTH = 6 };
    int distinct = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || refs[i].start != refs[i - 1].start || refs[i].end != refs[i - 1].end) {
            distinct++;
        }
    }
    if (distinct < 2) {
        return;
    }
    const char *path = result->defs.items[refs[0].idx].file_path;
    size_t path_cap = (path ? strlen(path) : 0) * JSON_ESCAPE_GROWTH + 1;
    if (path_cap > INT_MAX) {
        return;
    }
    char *esc = (char *)cbm_arena_alloc(&result->arena, path_cap);
    if (!esc) {
        return;
    }
    (void)cbm_json_escape(esc, (int)path_cap, path ? path : "");
    size_t cap = (size_t)distinct * (VARIANT_ENTRY_FIXED + strlen(esc)) + 3;
    char *json = (char *)cbm_arena_alloc(&result->arena, cap);
    if (!json) {
        return;
    }
    size_t pos = 0;
    json[pos++] = '[';
    for (int i = 0; i < n; i++) {
        if (i > 0 && refs[i].start == refs[i - 1].start && refs[i].end == refs[i - 1].end) {
            continue;
        }
        /* graph_buffer.c's variants schema, so its same-QN merge and every
         * reader of the node (the test-impact engine) see one format. */
        int w = snprintf(json + pos, cap - pos,
                         "%s{\"file_path\":\"%s\",\"start_line\":%u,\"end_line\":%u}",
                         pos > 1 ? "," : "", esc, refs[i].start, refs[i].end);
        if (w <= 0 || (size_t)w >= cap - pos) {
            return; /* cannot happen: cap covers the longest entry */
        }
        pos += (size_t)w;
    }
    json[pos++] = ']';
    json[pos] = '\0';
    /* Only the definition the graph keeps for this file carries the list: the
     * one with the largest start line (cbm_gbuf_upsert_node's same-file rule;
     * every member starting on that line, since arrival order breaks that
     * tie). Giving it to all n members would serialize an n-entry list n
     * times -- quadratic on a file that defines one name very often. */
    uint32_t last_start = refs[n - 1].start;
    for (int i = n - 1; i >= 0 && refs[i].start == last_start; i--) {
        result->defs.items[refs[i].idx].variants = json;
    }
}

static void cbm_mark_def_variants(CBMFileResult *result) {
    enum { VARIANT_STACK_REFS = 128 };
    int n = result->defs.count;
    if (n < 2) {
        return;
    }
    cbm_variant_ref_t stack[VARIANT_STACK_REFS];
    cbm_variant_ref_t *refs = stack;
    if (n > VARIANT_STACK_REFS) {
        refs = (cbm_variant_ref_t *)malloc((size_t)n * sizeof(*refs));
        if (!refs) {
            return;
        }
    }
    const CBMDefinition *defs = result->defs.items;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (defs[i].qualified_name && defs[i].label) {
            refs[m].key = cbm_variant_key(defs[i].qualified_name, defs[i].label);
            refs[m].start = defs[i].start_line;
            refs[m].end = defs[i].end_line;
            refs[m].idx = i;
            m++;
        }
    }
    qsort(refs, (size_t)m, sizeof(*refs), cbm_variant_ref_cmp);
    for (int i = 0; i < m;) {
        int j = i + 1;
        while (j < m && refs[j].key == refs[i].key) {
            j++;
        }
        /* [i, j) shares one key. That is one (QN, label) group unless two
         * keys collided, so split it by real equality, keeping span order. */
        int lo = i;
        while (j - i >= 2 && lo < j) {
            const CBMDefinition *lead = &defs[refs[lo].idx];
            int hi = lo + 1;
            for (int k = lo + 1; k < j; k++) {
                const CBMDefinition *d = &defs[refs[k].idx];
                if (strcmp(lead->qualified_name, d->qualified_name) == 0 &&
                    strcmp(lead->label, d->label) == 0) {
                    cbm_variant_ref_t moved = refs[k];
                    memmove(&refs[hi + 1], &refs[hi], (size_t)(k - hi) * sizeof(*refs));
                    refs[hi++] = moved;
                }
            }
            if (hi - lo >= 2) {
                cbm_variant_assign(result, &refs[lo], hi - lo);
            }
            lo = hi;
        }
        i = j;
    }
    if (refs != stack) {
        free(refs);
    }
}

/* Gates for adopting a def from the projection parse, as in upstream #961:
 * the name is on the def's first raw line and its span holds definition
 * syntax, not a macro invocation or call site. */
/* #961: true when 1-based `line` of `src` contains `name` (used to verify a
 * def recovered from EXPANDED source really lives on that ORIGINAL line —
 * rejects header-inlined defs whose physical expanded lines alias unrelated
 * raw lines when compile_commands include paths are present). */
static bool cbm_line_contains(const char *src, int src_len, uint32_t line, const char *name) {
    if (!src || !name || !name[0] || line == 0) {
        return false;
    }
    uint32_t cur = 1;
    int i = 0;
    while (i < src_len && cur < line) {
        if (src[i] == '\n') {
            cur++;
        }
        i++;
    }
    if (cur != line) {
        return false;
    }
    int end = i;
    while (end < src_len && src[end] != '\n') {
        end++;
    }
    size_t nlen = strlen(name);
    for (int j = i; j + (int)nlen <= end; j++) {
        if (strncmp(src + j, name, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/* Verify that the mapped original span contains callable-definition syntax,
 * not merely the name at a macro invocation or call site. */
static bool cbm_span_contains_callable_def(const char *src, int src_len, uint32_t start_line,
                                           uint32_t end_line, const char *name) {
    if (!src || src_len <= 0 || !name || !name[0] || start_line == 0 || end_line < start_line) {
        return false;
    }
    int span_start = 0;
    uint32_t line = 1;
    while (span_start < src_len && line < start_line) {
        if (src[span_start++] == '\n') {
            line++;
        }
    }
    if (line != start_line) {
        return false;
    }
    int span_end = span_start;
    while (span_end < src_len && line <= end_line) {
        if (src[span_end++] == '\n') {
            line++;
        }
    }
    size_t name_len = strlen(name);
    for (int pos = span_start; pos + (int)name_len <= span_end; pos++) {
        if (strncmp(src + pos, name, name_len) != 0 ||
            (pos > 0 && cbm_identifier_char(src[pos - 1])) ||
            (pos + (int)name_len < src_len && cbm_identifier_char(src[pos + name_len]))) {
            continue;
        }
        int before = pos;
        while (before > span_start && isspace((unsigned char)src[before - 1])) {
            before--;
        }
        if (before > span_start && strchr("(,=!?[.", src[before - 1])) {
            continue;
        }
        int open = pos + (int)name_len;
        while (open < span_end && isspace((unsigned char)src[open])) {
            open++;
        }
        if (open >= span_end || src[open] != '(') {
            continue;
        }
        int depth = 0;
        int close = -1;
        for (int i = open; i < span_end; i++) {
            if (src[i] == '(') {
                depth++;
            } else if (src[i] == ')' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close < 0) {
            continue;
        }
        for (int i = close + 1; i < span_end; i++) {
            if (src[i] == '{') {
                return true;
            }
            if (src[i] == ';') {
                break;
            }
        }
    }
    return false;
}

static bool cbm_def_label_is_callable(const char *label) {
    return label != NULL && (strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0);
}

/* ── First-branch projection rescue (C/C++/CUDA) ───────────────────────────
 *
 * The raw parse sees every #if branch at once. When branches split a brace
 * (`#ifndef X (a)) { #else (b)) { #endif`) or a directive sits inside an
 * expression, the braces stop balancing and an ERROR region swallows whole
 * functions: they get no node at all (curl ossl_connect_step2). The #961
 * rescue re-parses the preprocessed source, but with no build defines a
 * whole-file guard (`#ifdef USE_OPENSSL`) leaves nothing to parse. The
 * projection below needs no defines: every conditional group keeps exactly one
 * branch (the first; for `#if 0` the #else), so the kept text is one
 * consistent configuration the author wrote. Directive lines and dropped
 * branches become spaces with newlines kept, so every byte keeps its line and
 * column and a def found there needs no remapping. */

enum { CBM_PROJ_MAX_DEPTH = 64 };

typedef struct {
    uint8_t branch; /* index of the branch being scanned (0 = the #if part) */
    uint8_t keep;   /* branch that survives: 0, or 1 for `#if 0` */
} cbm_proj_frame_t;

/* Directive name after '#' and optional blanks, as [*name, *name + return). */
static int cbm_proj_directive(const char *p, const char *e, const char **name) {
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= e || *p != '#') {
        return 0;
    }
    p++;
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    const char *n = p;
    while (p < e && isalpha((unsigned char)*p)) {
        p++;
    }
    *name = n;
    return (int)(p - n);
}

static bool cbm_proj_word_is(const char *n, int len, const char *w) {
    return (size_t)len == strlen(w) && strncmp(n, w, (size_t)len) == 0;
}

/* `#if 0` (the dead-code idiom): the condition is the single token 0. */
static bool cbm_proj_if_zero(const char *after, const char *e) {
    while (after < e && (*after == ' ' || *after == '\t')) {
        after++;
    }
    if (after >= e || *after != '0') {
        return false;
    }
    after++;
    while (after < e && (*after == ' ' || *after == '\t' || *after == '\r')) {
        after++;
    }
    return after >= e || (after + 1 < e && after[0] == '/' && (after[1] == '/' || after[1] == '*'));
}

/* Track block-comment state across one line, skipping string and char
 * literals, so a `#if` inside a block comment is not taken for a directive. */
static bool cbm_proj_scan_comments(const char *p, const char *e, bool in_comment) {
    while (p < e) {
        if (in_comment) {
            if (p + 1 < e && p[0] == '*' && p[1] == '/') {
                in_comment = false;
                p += 2;
                continue;
            }
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (p < e && *p != q) {
                p += (*p == '\\' && p + 1 < e) ? 2 : 1;
            }
            p++;
            continue;
        }
        if (p + 1 < e && p[0] == '/' && p[1] == '/') {
            return false;
        }
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            in_comment = true;
            p += 2;
            continue;
        }
        p++;
    }
    return in_comment;
}

static void cbm_proj_blank(char *p, const char *e) {
    for (; p < e; p++) {
        if (*p != '\n') {
            *p = ' ';
        }
    }
}

/* Heap copy of `src` with the first-branch projection applied, or NULL when
 * the source has no conditional group (nothing to project), nests deeper than
 * CBM_PROJ_MAX_DEPTH, or allocation fails. Caller frees with free(). */
static char *cbm_first_branch_projection(const char *src, int len) {
    char *out = malloc((size_t)len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, src, (size_t)len);
    out[len] = '\0';
    cbm_proj_frame_t frames[CBM_PROJ_MAX_DEPTH];
    int depth = 0;
    bool any_group = false;
    bool in_comment = false;
    bool continuation = false; /* previous conditional-directive line ended in '\' */
    const char *end = out + len;
    for (char *line = out; line < end;) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        char *eol = nl ? nl : (char *)end;
        bool active = true;
        for (int i = 0; i < depth && active; i++) {
            active = frames[i].branch == frames[i].keep;
        }
        bool blank = !active || continuation;
        bool was_continuation = continuation;
        continuation = false;
        const char *name = NULL;
        int nlen = (!in_comment && !was_continuation) ? cbm_proj_directive(line, eol, &name) : 0;
        bool conditional = false;
        if (nlen > 0) {
            if (cbm_proj_word_is(name, nlen, "if") || cbm_proj_word_is(name, nlen, "ifdef") ||
                cbm_proj_word_is(name, nlen, "ifndef")) {
                if (depth >= CBM_PROJ_MAX_DEPTH) {
                    free(out);
                    return NULL;
                }
                bool zero =
                    cbm_proj_word_is(name, nlen, "if") && cbm_proj_if_zero(name + nlen, eol);
                frames[depth].branch = 0;
                frames[depth].keep = zero ? 1 : 0;
                depth++;
                conditional = any_group = true;
            } else if (cbm_proj_word_is(name, nlen, "elif") ||
                       cbm_proj_word_is(name, nlen, "elifdef") ||
                       cbm_proj_word_is(name, nlen, "elifndef") ||
                       cbm_proj_word_is(name, nlen, "else")) {
                if (depth > 0 && frames[depth - 1].branch < UINT8_MAX) {
                    frames[depth - 1].branch++;
                }
                conditional = true;
            } else if (cbm_proj_word_is(name, nlen, "endif")) {
                if (depth > 0) {
                    depth--;
                }
                conditional = true;
            }
        }
        if (conditional) {
            blank = true;
            char *last = eol;
            while (last > line && (last[-1] == '\r' || last[-1] == ' ' || last[-1] == '\t')) {
                last--;
            }
            continuation = last > line && last[-1] == '\\';
        } else if (was_continuation) {
            char *last = eol;
            while (last > line && (last[-1] == '\r' || last[-1] == ' ' || last[-1] == '\t')) {
                last--;
            }
            continuation = last > line && last[-1] == '\\';
        }
        in_comment = cbm_proj_scan_comments(line, eol, in_comment);
        if (blank) {
            cbm_proj_blank(line, eol);
        }
        line = nl ? nl + 1 : (char *)end;
    }
    if (!any_group) {
        free(out);
        return NULL;
    }
    return out;
}

/* Only a parse that lost structure wholesale is worth a second parse: the raw
 * root is itself an ERROR, or one top-most error region spans this many lines.
 * Small local errors (a macro the grammar cannot read) are the common case —
 * about half the kernel's C files have one — and the projection cannot fix
 * them; the gate keeps the extra parse to ~1% of kernel C bytes (sampled). */
enum { CBM_PROJ_MIN_ERROR_LINES = 20 };

/* Adopt from the projection only what the raw pass lost: a Function/Method
 * def that overlaps a raw ERROR region, whose QN no def already holds, whose
 * name is on its first raw line, and whose span has definition syntax (the
 * #961 gates). Everything else the projected walk produced is discarded. */
static void cbm_rescue_defs_from_projection(CBMExtractCtx *raw_ctx, const TSLanguage *ts_lang) {
    CBMFileResult *result = raw_ctx->result;
    if (!ts_node_has_error(raw_ctx->root)) {
        return;
    }
    cbm_error_regions_t regs = {{0}, {0}, 0, 0};
    bool broken = strcmp(ts_node_type(raw_ctx->root), "ERROR") == 0;
    if (broken) {
        /* The whole file failed: every line is error territory. */
        regs.starts[0] = 1;
        regs.ends[0] = ts_node_end_point(raw_ctx->root).row + 1;
        regs.count = 1;
    } else {
        cbm_collect_error_regions(raw_ctx->root, &regs, raw_ctx->source, raw_ctx->source_len);
        for (int r = 0; r < regs.count && !broken; r++) {
            broken = regs.ends[r] >= regs.starts[r] + CBM_PROJ_MIN_ERROR_LINES;
        }
    }
    if (!broken) {
        return;
    }
    char *projected = cbm_first_branch_projection(raw_ctx->source, raw_ctx->source_len);
    if (!projected) {
        return;
    }
    TSParser *parser = get_thread_parser(ts_lang, raw_ctx->language);
    TSTree *tree = NULL;
    if (parser) {
        ts_parser_reset(parser);
        /* The projection keeps the raw source's length and line structure, so
         * it is parsed the way the raw source was, terminated last line
         * included (#53). */
        TSParseOptions opts = {0};
        tree = cbm_parse_source(parser, projected, (uint32_t)raw_ctx->source_len, opts);
    }
    CBMHashTable *held = tree ? cbm_ht_create(CBM_SZ_256) : NULL;
    if (held) {
        for (int i = 0; i < result->defs.count; i++) {
            if (result->defs.items[i].qualified_name) {
                cbm_ht_set(held, result->defs.items[i].qualified_name, &result->defs.items[i]);
            }
        }
        CBMExtractCtx pctx = {
            .arena = raw_ctx->arena,
            .scratch = raw_ctx->scratch,
            .result = result,
            .source = projected,
            .source_len = raw_ctx->source_len,
            .language = raw_ctx->language,
            .project = raw_ctx->project,
            .rel_path = raw_ctx->rel_path,
            .module_qn = result->module_qn,
            .root = ts_tree_root_node(tree),
        };
        int before = result->defs.count;
        cbm_extract_definitions_without_module(&pctx);
        int w = before;
        for (int i = before; i < result->defs.count; i++) {
            CBMDefinition *d = &result->defs.items[i];
            bool adopt = d->name && d->qualified_name && cbm_def_label_is_callable(d->label) &&
                         !cbm_ht_has(held, d->qualified_name);
            bool overlaps = false;
            for (int r = 0; adopt && r < regs.count && !overlaps; r++) {
                overlaps = d->start_line <= regs.ends[r] && d->end_line >= regs.starts[r];
            }
            adopt =
                adopt && overlaps &&
                cbm_line_contains(raw_ctx->source, raw_ctx->source_len, d->start_line, d->name) &&
                cbm_span_contains_callable_def(raw_ctx->source, raw_ctx->source_len, d->start_line,
                                               d->end_line, d->name);
            if (adopt) {
                result->defs.items[w++] = *d;
                cbm_ht_set(held, result->defs.items[w - 1].qualified_name,
                           &result->defs.items[w - 1]);
            }
        }
        result->defs.count = w;
        cbm_ht_free(held);
    }
    if (tree) {
        ts_tree_delete(tree);
    }
    free(projected);
}

/* Public entry: run the extraction and journal completion. The DONE mark on
 * every ordinary return (including error/timeout results) tells the crash
 * supervisor this file did NOT kill the worker — only a file whose S has no
 * D is a crash/hang suspect. */
CBMFileResult *cbm_extract_file(const char *source, int source_len, CBMLanguage language,
                                const char *project, const char *rel_path, int64_t timeout_micros,
                                const char **extra_defines, const char **include_paths) {
    CBMFileResult *r = cbm_extract_file_impl(source, source_len, language, project, rel_path,
                                             timeout_micros, extra_defines, include_paths);
    cbm_index_mark_done(rel_path);
    return r;
}

/* Initial block for the per-file traversal scratch arena, chosen by measuring
 * arena_grow on a 14k-file TypeScript tree: it fires on one file in 12,000 at
 * both this size and at 1 MB, and on most files at 256 KB, where the two
 * channel walks alone are exactly 262144 bytes. 512 KB therefore buys the same
 * growth behaviour as 1 MB for half the resident block per worker. It is also
 * exactly MI_LARGE_MAX_OBJ_SIZE in the vendored mimalloc
 * (vendored/mimalloc/include/mimalloc/types.h:426, MI_LARGE_PAGE_SIZE/8 with
 * MI_ENABLE_LARGE_PAGES defaulting to 1 at :115 and not overridden here), so
 * the block is still bin-allocated from a large page. Growth is not free at
 * this size for the same reason: arena_grow doubles to 1 MiB, which is above
 * that bound and so a singleton OS allocation. One file in twelve thousand
 * pays it, which is why the cost is accepted. */
enum { CBM_EXTRACT_SCRATCH_BLOCK = CBM_SZ_512 * CBM_SZ_1K };

static CBMFileResult *extract_file_impl_body(const char *source, int source_len,
                                             CBMLanguage language, const char *project,
                                             const char *rel_path, int64_t timeout_micros,
                                             const char **extra_defines,
                                             const char **include_paths, CBMArena *scratch) {
    // Allocate result on heap (arena inside for all string data)
    enum { SINGLE = 1 };
    CBMFileResult *result = (CBMFileResult *)calloc(SINGLE, sizeof(CBMFileResult));
    if (!result) {
        return NULL;
    }

    cbm_arena_init(&result->arena);
    CBMArena *a = &result->arena;

    /* Crash-quarantine hard guard (Stage 3c): a file the supervisor pinned as a
     * crasher must NEVER be parsed again. Return a clean empty result BEFORE the
     * marker write and fault injector so no pass (including sequential re-extract
     * passes that miss the result cache) can crash on it. The pipeline extract
     * loops separately record it as a phase="crash" skip. Checked before the
     * marker so quarantined files never overwrite it — the marker keeps pointing
     * at the real (non-quarantined) file being processed when a crash hits. */
    if (cbm_index_is_quarantined(rel_path)) {
        return result;
    }

    cbm_index_mark_start(rel_path);
    cbm_test_fault_inject(rel_path);

    // Get language spec
    const CBMLangSpec *spec = cbm_lang_spec(language);
    if (!spec) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "unsupported language");
        return result;
    }

    // Get tree-sitter language
    const TSLanguage *ts_lang = cbm_ts_language(language);
    if (!ts_lang) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "no tree-sitter grammar");
        return result;
    }

    // Get thread-local parser (reused across files on same thread)
    TSParser *parser = get_thread_parser(ts_lang, language);
    if (!parser) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "parser alloc failed");
        return result;
    }

    // Reset parser state from any previous parse (cancellation flags etc.)
    ts_parser_reset(parser);

    uint64_t t0 = now_ns();

    // Timeout options for parse_with_options
    TSParseOptions opts = {0};
    CBMParseBudget budget = {0}; // cppcheck-suppress unreadVariable
    if (timeout_micros > 0) {
        uint64_t budget_ns = (uint64_t)timeout_micros * USEC_TO_NSEC;
        budget.cpu_deadline_ns = cbm_thread_cpu_time_ns() + budget_ns;
        budget.wall_ceiling_ns = t0 + (budget_ns * CBM_PARSE_WALL_CEILING_FACTOR);
        /* Test hook, like CBM_TEST_CRASH_ON: read the wall clock budget + 1 s
         * ahead for the named file, as if the worker had been descheduled that
         * long. Past the old wall-only budget, under the ceiling. */
        const char *stall_on = getenv("CBM_TEST_WALL_STALL_ON");
        if (stall_on && stall_on[0] && rel_path && strstr(rel_path, stall_on)) {
            budget.wall_offset_ns = budget_ns + NSEC_PER_SEC;
        }
        opts.payload = &budget;
        opts.progress_callback = cbm_timeout_cb;
    }

    TSTree *tree = cbm_parse_source(parser, source, (uint32_t)source_len, opts);

    if (!tree) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, budget.tripped ? "parse timeout" : "parse failed");
        return result;
    }

    TSNode root = ts_tree_root_node(tree);

    // Compute module QN. Java/Go derive the module from the CONTAINING
    // DIRECTORY (package semantics) rather than baking the filename stem in,
    // so def QNs, the LSP caller_qn, and the textual calls-enclosing QN all
    // agree (e.g. Outer.java -> module "proj", not "proj.Outer"). Other
    // languages are unchanged.
    result->module_qn = cbm_fqn_module_source_lang(a, project, rel_path, language);
    result->is_test_file = cbm_is_test_file(rel_path, language);

    // Build extraction context
    CBMExtractCtx ctx = {
        .arena = a,
        .scratch = scratch,
        .result = result,
        .source = source,
        .source_len = source_len,
        .language = language,
        .project = project,
        .rel_path = rel_path,
        .module_qn = result->module_qn,
        .root = root,
    };

    // Run extractors: defs + imports use separate walks (unique recursion patterns),
    // then a single unified cursor walk handles the remaining 7 extractors.
    cbm_extract_definitions(&ctx);
    cbm_extract_imports(&ctx);
    cbm_extract_unified(&ctx);

    // Channel detection (Socket.IO / EventEmitter) — JS/TS only.
    cbm_extract_channels(&ctx);

    // K8s / Kustomize semantic pass (additional structured extraction for YAML-based infra files).
    if (ctx.language == CBM_LANG_KUSTOMIZE || ctx.language == CBM_LANG_K8S) {
        cbm_extract_k8s(&ctx);
    }

    // LSP type-aware call/usage resolution (per-file). Runs in every mode;
    // refines the tree-sitter + textual-resolution graph with type info.
    bool c_lsp_complete = true; // the raw C/C++ walk did not stop on a failure
    {
        if (language == CBM_LANG_GO) {
            cbm_run_go_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
            c_lsp_complete =
                cbm_run_c_lsp(a, result, source, source_len, root, language != CBM_LANG_C);
        }
        if (language == CBM_LANG_PHP) {
            cbm_run_php_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_PYTHON) {
            cbm_run_py_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_JAVASCRIPT || language == CBM_LANG_TYPESCRIPT ||
            language == CBM_LANG_TSX) {
            bool js_mode = (language == CBM_LANG_JAVASCRIPT);
            // jsx_mode: TSX always; .jsx in the JS bucket also enables it.
            bool jsx_mode = (language == CBM_LANG_TSX);
            if (language == CBM_LANG_JAVASCRIPT && rel_path) {
                size_t rl = strlen(rel_path);
                if (rl >= 4 && strcmp(rel_path + rl - 4, ".jsx") == 0)
                    jsx_mode = true;
            }
            // dts_mode: ".d.ts" suffix (TypeScript only).
            bool dts_mode = false;
            if (language == CBM_LANG_TYPESCRIPT && rel_path) {
                size_t rl = strlen(rel_path);
                if (rl >= 5 && strcmp(rel_path + rl - 5, ".d.ts") == 0)
                    dts_mode = true;
            }
            cbm_run_ts_lsp(a, result, source, source_len, root, js_mode, jsx_mode, dts_mode);
        }
        if (language == CBM_LANG_CSHARP) {
            cbm_run_cs_lsp(a, result, source, source_len, root);
        }
    }
    if (language == CBM_LANG_JAVA) {
        cbm_run_java_lsp(a, result, source, source_len, root);
    }
    if (language == CBM_LANG_KOTLIN) {
        cbm_run_kotlin_lsp(a, result, source, source_len, root);
    }
    if (language == CBM_LANG_RUST) {
        cbm_run_rust_lsp(a, result, source, source_len, root);
    }
    if (language == CBM_LANG_ZIG) {
        cbm_run_zig_lsp(a, result, source, source_len, root);
    }

    // Calls extracted so far all carry ORIGINAL-source line numbers; the C/C++
    // preprocessor second pass below appends calls with EXPANDED-source lines,
    // which must not be used for the def line-range attribution of the bottleneck
    // metrics. Remember the boundary.
    int orig_calls_count = result->calls.count;

    // Second pass: preprocess C/C++/CUDA and extract additional macro-hidden calls.
    // Defs keep original-source line numbers; only CALLS are extracted from expanded source.
    if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
        char *expanded = cbm_preprocess(source, source_len, rel_path, extra_defines, include_paths,
                                        language != CBM_LANG_C);
        if (expanded) {
            int expanded_len = (int)strlen(expanded);
            // Record calls count before second pass
            int calls_before = result->calls.count;

            // Parse expanded source with fresh tree
            TSParser *pp_parser = get_thread_parser(ts_lang, language);
            if (pp_parser) {
                ts_parser_reset(pp_parser);
                CBMStringInput pp_input = {expanded, (uint32_t)expanded_len, false};
                TSInput pp_ts_input = {
                    &pp_input,
                    cbm_string_read,
                    TSInputEncodingUTF8,
                    NULL,
                };
                TSParseOptions pp_opts = {0};
                TSTree *pp_tree =
                    ts_parser_parse_with_options(pp_parser, NULL, pp_ts_input, pp_opts);
                if (pp_tree) {
                    TSNode pp_root = ts_tree_root_node(pp_tree);

                    // Build context for expanded source — extract only calls via unified extractor
                    CBMExtractCtx pp_ctx = {
                        .arena = a,
                        .scratch = scratch,
                        .result = result,
                        .source = expanded,
                        .source_len = expanded_len,
                        .language = language,
                        .project = project,
                        .rel_path = rel_path,
                        .module_qn = result->module_qn,
                        .root = pp_root,
                    };
                    // Re-run unified extraction on expanded source.
                    // This adds macro-expanded calls; duplicates with original calls are
                    // harmless (pipeline deduplicates by caller+callee).
                    cbm_extract_unified(&pp_ctx);

                    // Also run LSP on expanded source for additional type-resolved
                    // calls (language is already C/C++/CUDA — checked in enclosing
                    // block). Runs in every mode, unless the raw walk already
                    // stopped on a memo or depth failure.
                    if (c_lsp_complete) {
                        cbm_run_c_lsp(a, result, expanded, expanded_len, pp_root,
                                      language != CBM_LANG_C);
                    }

                    ts_tree_delete(pp_tree);
                }
            }
            cbm_preprocess_free(expanded);
            (void)calls_before; // used for future logging
        }

        /* Functions both the raw parse and the #961 preprocessed rescue lost
         * (whole-file feature guards, directives inside expressions). Runs
         * only when the raw tree has errors and the file has #if groups. */
        cbm_rescue_defs_from_projection(&ctx, ts_lang);
    }

    // Bottleneck call-context metrics. Each call is attributed to the INNERMOST
    // enclosing Function/Method def by source-line range (defs and calls in one
    // CBMFileResult share the same file). Range matching is used instead of
    // enclosing_func_qn string matching because some grammars (notably C, whose
    // function_definition has no "name" field) attribute the call's scope to the
    // module rather than the function — line ranges are unambiguous and
    // language-agnostic. Bounded per file (defs x calls), not a repo-scale scan.
    int def_count = result->defs.count;
    bool *has_self = def_count > 0 ? calloc((size_t)def_count, sizeof(bool)) : NULL;
    bool *has_guarded = def_count > 0 ? calloc((size_t)def_count, sizeof(bool)) : NULL;

    // param_count is a standalone structural smell (independent of calls). Prefer
    // the parsed param_names array; fall back to counting from the signature text
    // for languages (e.g. C) that populate only the signature.
    for (int di = 0; di < def_count; di++) {
        CBMDefinition *d = &result->defs.items[di];
        int pc = 0;
        if (d->param_names) {
            while (d->param_names[pc]) {
                pc++;
            }
        }
        if (pc == 0 && d->signature) {
            pc = count_params_from_signature(d->signature);
        }
        d->param_count = pc;
    }

    for (int ci = 0; ci < orig_calls_count; ci++) {
        const CBMCall *c = &result->calls.items[ci];
        if (!c->callee_name || c->start_line <= 0) {
            continue;
        }
        // Innermost enclosing Function/Method def by line range (smallest span).
        int best = -1;
        int best_span = -1;
        for (int di = 0; di < def_count; di++) {
            const CBMDefinition *d = &result->defs.items[di];
            if (!d->name || !d->label ||
                (strcmp(d->label, "Function") != 0 && strcmp(d->label, "Method") != 0)) {
                continue;
            }
            if ((int)d->start_line <= c->start_line && c->start_line <= (int)d->end_line) {
                int span = (int)d->end_line - (int)d->start_line;
                if (best < 0 || span < best_span) {
                    best_span = span;
                    best = di;
                }
            }
        }
        if (best < 0) {
            continue;
        }
        CBMDefinition *d = &result->defs.items[best];
        // callee_name may be bare ("recur") or qualified ("self.recur",
        // "super().save", "axios.get"). A short-name match alone is not
        // self-recursion: the callee must also target the same object
        // (is_self_receiver), or super().save() inside save and axios.get
        // inside get are false positives (#599).
        const char *dot = strrchr(c->callee_name, '.');
        const char *callee_short = dot ? dot + 1 : c->callee_name;
        bool in_loop = c->loop_depth > 0;

        if (strcmp(callee_short, d->name) == 0 && is_self_receiver(c->callee_name, d->receiver)) {
            // Direct self-recursion. The call graph omits self-edges (the call
            // resolver skips source==target), so detect it here; seeds "recursive".
            d->is_recursive = true;
            if (has_self) {
                has_self[best] = true;
            }
            if (in_loop) {
                d->recursion_in_loop = true; // recursion compounded by a loop
            }
            if (c->branch_depth > 0 && has_guarded) {
                has_guarded[best] = true; // a self-call guarded by some conditional
            }
        }
        if (in_loop && is_linear_scan_name(callee_short)) {
            d->linear_scan_in_loop++; // hidden O(n^2): linear scan inside a loop
        }
        if (in_loop && is_alloc_name(callee_short)) {
            d->alloc_in_loop++; // repeated allocation/append inside a loop
        }
    }

    // Recursive with no self-call guarded by any conditional → no obvious base
    // case on the recursive path: a stronger "potentially unbounded" signal.
    for (int di = 0; di < def_count; di++) {
        if (has_self && has_self[di] && !(has_guarded && has_guarded[di])) {
            result->defs.items[di].unguarded_recursion = true;
        }
    }
    free(has_self);
    free(has_guarded);

    /* Best-effort parse-coverage signal (#963): flag files whose tree contains
     * ERROR/MISSING regions. Computed AFTER extraction so definite recovery is
     * subtracted first — a region fully re-extracted as definitions is not a
     * miss, and a fully recovered file is not flagged at all. Detection aid
     * only: the absence of this flag is NOT a completeness guarantee. */
    if (ts_node_has_error(root)) {
        cbm_error_regions_t regs = {{0}, {0}, 0, 0};
        if (strcmp(ts_node_type(root), "ERROR") == 0) {
            cbm_error_regions_push(&regs, root); /* whole file unparseable */
        } else {
            cbm_collect_error_regions(root, &regs, source, source_len);
        }
        cbm_subtract_recovered_regions(&regs, &result->defs);
        /* #1071: don't flag a benign function-like-macro call (defined in-file)
         * that tree-sitter can't parse without the preprocessor. */
        cbm_subtract_macro_invocation_regions(&regs, &result->defs, source, source_len);
        /* A file whose kept list is empty but whose cap still bound is NOT clean:
         * the ranges the cap threw away were never judged by the two rules
         * above, so nothing proves they were recovered. Flag it. */
        if (regs.count > 0 || regs.dropped > 0) {
            result->parse_incomplete = true;
            result->error_region_count = regs.count;
            result->error_ranges = cbm_error_ranges_str(a, &regs);
            /* One range covering nearly the whole file is not advice, it is
             * noise: "look at lines 1 to 13047" of a 13046-line file tells a
             * reader nothing they did not already know. Mark those separately
             * so the report can say "read the source" instead. See
             * parse_unusable in cbm.h for which files land here and why. */
            if (regs.count == 1 && regs.dropped == 0) {
                uint32_t total = cbm_count_lines(source, source_len);
                uint32_t span = regs.ends[0] - regs.starts[0] + 1;
                if (total > 0 && span * 100 >= total * CBM_UNUSABLE_PCT) {
                    result->parse_unusable = true;
                }
            }
        }
    }

    /* The def list is final here (raw walk, preprocessed and projection rescues). */
    if (cbm_is_c_preprocessor_lang(language)) {
        cbm_mark_def_variants(result);
    }

    result->imports_count = result->imports.count;

    // Retain tree for cross-file LSP reuse (caller frees via cbm_free_tree)
    result->cached_tree = tree;
    result->cached_lang = language;
    return result;
}

/* Owns the traversal scratch arena for the whole of one file's extraction:
 * created here, handed to the body as ctx->scratch, destroyed on the way out.
 * The body has several early returns, so bracketing it in a wrapper keeps that
 * to one create and one destroy. If the arena cannot be created, the body is
 * handed NULL and the traversal stacks fall back to the result arena, which is
 * the behaviour before upstream #1997. */
static CBMFileResult *cbm_extract_file_impl(const char *source, int source_len,
                                            CBMLanguage language, const char *project,
                                            const char *rel_path, int64_t timeout_micros,
                                            const char **extra_defines,
                                            const char **include_paths) {
    CBMArena scratch;
    cbm_arena_init_sized(&scratch, CBM_EXTRACT_SCRATCH_BLOCK);
    CBMFileResult *result = extract_file_impl_body(
        source, source_len, language, project, rel_path, timeout_micros, extra_defines,
        include_paths, scratch.nblocks > 0 ? &scratch : NULL);
    cbm_arena_destroy(&scratch);
    return result;
}

void cbm_free_result(CBMFileResult *result) {
    if (!result) {
        return;
    }
    if (result->cached_tree) {
        ts_tree_delete(result->cached_tree);
        result->cached_tree = NULL;
    }
    cbm_arena_destroy(&result->arena);
    free(result);
}

void cbm_free_tree(CBMFileResult *result) {
    if (result && result->cached_tree) {
        ts_tree_delete(result->cached_tree);
        result->cached_tree = NULL;
    }
}
