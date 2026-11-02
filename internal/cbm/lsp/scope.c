#include "scope.h"
#include "lsp_work.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

_Thread_local uint64_t cbm_lsp_work_steps = 0;

uint64_t cbm_lsp_work_take(void) {
    uint64_t v = cbm_lsp_work_steps;
    cbm_lsp_work_steps = 0;
    return v;
}

enum {
    SCOPE_INDEX_MIN_CAP = 64, /* first table: > 2x one chunk, a power of two */
    SCOPE_INDEX_GROW = 2,
};

CBMScope* cbm_scope_push(CBMArena* a, CBMScope* current) {
    CBMScope* scope = (CBMScope*)cbm_arena_alloc(a, sizeof(CBMScope));
    if (!scope) {
        return current;
    }
    memset(scope, 0, sizeof(CBMScope));
    scope->parent = current;
    scope->arena = a;
    return scope;
}

CBMScope* cbm_scope_pop(CBMScope* scope) {
    if (!scope) {
        return NULL;
    }
    return scope->parent;
}

static CBMScopeChunk* alloc_chunk(CBMScope* scope) {
    if (!scope->arena) {
        return NULL;
    }
    CBMScopeChunk* c = (CBMScopeChunk*)cbm_arena_alloc(scope->arena, sizeof(CBMScopeChunk));
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof(CBMScopeChunk));
    c->next = scope->chunks;
    scope->chunks = c;
    return c;
}

static uint32_t scope_name_hash(const char* name) {
    uint32_t h = 2166136261U; /* FNV-1a */
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        h ^= *p;
        h *= 16777619U;
    }
    return h;
}

/* Place one binding into a table that has room (load kept below 1/2). */
static void scope_index_place(CBMVarBinding** slots, int cap, CBMVarBinding* b) {
    uint32_t mask = (uint32_t)cap - 1;
    uint32_t i = scope_name_hash(b->name) & mask;
    while (slots[i]) {
        i = (i + 1) & mask;
    }
    slots[i] = b;
}

/* (Re)build the frame's table from its chunks, sized from the whole frame. On
 * allocation failure the frame drops its index and stays on the scan. */
static void scope_index_rebuild(CBMScope* scope, int cap) {
    if (scope->binding_count < 0 || scope->binding_count > INT_MAX / 2) {
        memset(&scope->index, 0, sizeof(scope->index));
        return;
    }
    if (cap < SCOPE_INDEX_MIN_CAP) {
        cap = SCOPE_INDEX_MIN_CAP;
    }
    while (cap < scope->binding_count * 2) {
        if (cap > INT_MAX / SCOPE_INDEX_GROW) {
            memset(&scope->index, 0, sizeof(scope->index));
            return;
        }
        cap *= SCOPE_INDEX_GROW;
    }
    CBMVarBinding** slots =
        scope->arena
            ? (CBMVarBinding**)cbm_arena_alloc(scope->arena, (size_t)cap * sizeof(CBMVarBinding*))
            : NULL;
    if (!slots) {
        memset(&scope->index, 0, sizeof(scope->index));
        return;
    }
    memset(slots, 0, (size_t)cap * sizeof(CBMVarBinding*));
    int count = 0;
    for (CBMScopeChunk* c = scope->chunks; c != NULL; c = c->next) {
        for (int i = 0; i < c->used; i++) {
            if (c->bindings[i].name) {
                scope_index_place(slots, cap, &c->bindings[i]);
                count++;
            }
        }
    }
    scope->index.slots = slots;
    scope->index.cap = cap;
    scope->index.count = count;
}

/* Record a freshly appended binding in the frame's index, building the index
 * the first time the frame outgrows one chunk. */
static void scope_index_note(CBMScope* scope, CBMVarBinding* b) {
    if (scope->index.cap == 0) {
        if (scope->binding_count > CBM_SCOPE_CHUNK_BINDINGS) {
            scope_index_rebuild(scope, SCOPE_INDEX_MIN_CAP);
        }
        return;
    }
    if (scope->index.count >= scope->index.cap / 2) {
        scope_index_rebuild(scope, scope->index.cap); /* includes b */
        return;
    }
    scope_index_place(scope->index.slots, scope->index.cap, b);
    scope->index.count++;
}

/* The binding of name in one frame: hashed when the frame has an index,
 * otherwise the linear chunk scan (small frames, or no index memory). */
static CBMVarBinding* scope_frame_find(const CBMScope* scope, const char* name) {
    if (scope->index.cap > 0) {
        uint32_t mask = (uint32_t)scope->index.cap - 1;
        for (uint32_t i = scope_name_hash(name) & mask;; i = (i + 1) & mask) {
            CBMVarBinding* b = scope->index.slots[i];
            if (!b) {
                return NULL;
            }
            CBM_LSP_WORK(1);
            if (strcmp(b->name, name) == 0) {
                return b;
            }
        }
    }
    for (CBMScopeChunk* c = scope->chunks; c != NULL; c = c->next) {
        for (int i = 0; i < c->used; i++) {
            CBM_LSP_WORK(1);
            if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                return &c->bindings[i];
            }
        }
    }
    return NULL;
}

void cbm_scope_bind(CBMScope* scope, const char* name, const CBMType* type) {
    if (!scope || !name) {
        return;
    }
    CBMVarBinding* existing = scope_frame_find(scope, name);
    if (existing) {
        existing->type = type;
        return;
    }
    CBMScopeChunk* head = scope->chunks;
    if (!head || head->used >= CBM_SCOPE_CHUNK_BINDINGS) {
        head = alloc_chunk(scope);
        if (!head) {
            return;
        }
    }
    CBMVarBinding* b = &head->bindings[head->used];
    b->name = name;
    b->type = type;
    head->used++;
    scope->binding_count++;
    scope_index_note(scope, b);
}

const CBMVarBinding* cbm_scope_lookup_local(const CBMScope* scope, const char* name) {
    if (!scope || !name) {
        return NULL;
    }
    return scope_frame_find(scope, name);
}

const CBMType* cbm_scope_lookup(const CBMScope* scope, const char* name) {
    if (!name) {
        return cbm_type_unknown();
    }
    for (const CBMScope* s = scope; s != NULL; s = s->parent) {
        const CBMVarBinding* b = scope_frame_find(s, name);
        if (b) {
            return b->type;
        }
    }
    return cbm_type_unknown();
}

bool cbm_scope_binds_local(const CBMScope* scope, const char* name) {
    if (!name) {
        return false;
    }
    for (const CBMScope* s = scope; s != NULL && s->parent != NULL; s = s->parent) {
        if (scope_frame_find(s, name)) {
            return true;
        }
    }
    return false;
}
