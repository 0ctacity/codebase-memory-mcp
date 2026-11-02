#ifndef CBM_LSP_SCOPE_H
#define CBM_LSP_SCOPE_H

#include "type_rep.h"
#include "../arena.h"
#include <stdlib.h> /* getenv, atoi (cbm_lsp_max_walk_depth) */

typedef struct {
    const char* name;
    const CBMType* type;
} CBMVarBinding;

#define CBM_SCOPE_CHUNK_BINDINGS 16

typedef struct CBMScopeChunk {
    CBMVarBinding bindings[CBM_SCOPE_CHUNK_BINDINGS];
    int used;
    struct CBMScopeChunk* next;
} CBMScopeChunk;

/* Name index for one frame. A frame that outgrows one chunk (a module scope
 * binding thousands of classes and functions) is looked up through an
 * open-addressing table of binding pointers instead of a linear chunk scan;
 * the scan made every identifier lookup O(frame size) and a whole file
 * O(n^2). Bindings never move (chunks are arena-allocated and only
 * prepended) and a name is bound at most once per frame (rebinding updates
 * in place), so the table maps each name to exactly the binding the linear
 * scan would have found. The table is an accelerator only: when it cannot be
 * allocated or grown the frame drops it and every lookup falls back to the
 * scan, which is always correct. */
typedef struct {
    CBMVarBinding **slots; // arena-owned; NULL = empty
    int cap;               // power of two, or 0 = no index (linear scan)
    int count;
} CBMScopeIndex;

typedef struct CBMScope {
    struct CBMScope* parent;
    CBMScopeChunk* chunks;
    CBMArena* arena;        // owning arena, propagated to children at push time
    int binding_count;      // bindings recorded in THIS frame
    CBMScopeIndex index;    // built once binding_count exceeds one chunk
} CBMScope;

// Bail-to-UNKNOWN depth for type-lookup chains: alias resolution, MRO walks,
// embedded-field/struct-traversal. Exceeding this collapses to cbm_type_unknown
// rather than recursing — guards against pathological hierarchies.
#define CBM_LSP_MAX_LOOKUP_DEPTH 16

// Recursion cap for the per-language "resolve calls in AST node" walkers. These
// recurse once per AST nesting level; a deeply-nested or cyclic file can drive
// them into a native stack overflow (SIGSEGV) that takes down the whole index.
// Past this cap the wrapper skips the subtree — those calls stay unresolved,
// which is graceful degradation, not a crash. 512 is far deeper than any
// hand-written source nests; override for pathological/generated repos via the
// CBM_LSP_MAX_WALK_DEPTH env var (positive integer).
#define CBM_LSP_MAX_WALK_DEPTH 512

// Resolved walk-depth cap: env override (CBM_LSP_MAX_WALK_DEPTH, if a positive
// integer) else CBM_LSP_MAX_WALK_DEPTH. Read once and cached — the walkers call
// this per node, so it must not hit getenv on the hot path. The cache is a
// benign idempotent race under multi-threaded indexing (every thread computes
// the same value).
static inline int cbm_lsp_max_walk_depth(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("CBM_LSP_MAX_WALK_DEPTH");
        int v = (e && *e) ? atoi(e) : 0;
        cached = (v > 0) ? v : CBM_LSP_MAX_WALK_DEPTH;
    }
    return cached;
}

CBMScope* cbm_scope_push(CBMArena* a, CBMScope* current);
CBMScope* cbm_scope_pop(CBMScope* scope);
void cbm_scope_bind(CBMScope* scope, const char* name, const CBMType* type);
const CBMType* cbm_scope_lookup(const CBMScope* scope, const char* name);
// The binding of name in THIS frame only (no parent walk), or NULL. Hashed once
// the frame outgrows one chunk.
const CBMVarBinding* cbm_scope_lookup_local(const CBMScope* scope, const char* name);
// True when `name` is bound in `scope` or an enclosing scope other than the
// outermost (module) scope, whatever its type: a local or parameter that
// shadows a module-level symbol of the same name.
bool cbm_scope_binds_local(const CBMScope* scope, const char* name);

#endif // CBM_LSP_SCOPE_H
