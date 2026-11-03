/*
 * cypher.c — Cypher query engine: planner and executor. The lexer and
 * parser are in parser.zig.
 *
 * Translates a subset of Cypher into SQL queries against cbm_store.
 * Supports MATCH patterns with relationships, WHERE filters,
 * RETURN with COUNT/ORDER BY/LIMIT/DISTINCT.
 */
#include "cypher/cypher.h"
#include "store/store.h"
#include "foundation/platform.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/limits.h"
#include "foundation/log.h"

enum {
    CYP_BUF_16 = 16,
    CYP_BUF_48 = 48, /* ASCII '0' */
    CYP_BUF_8 = 8,
    CYP_BUF_4 = 4,
    CYP_MAX_TOKEN = 10, /* max token lookahead */
    CYP_PAIR = 2,
    CYP_TRIPLE = 3,
    CYP_INIT_CAP4 = 4,     /* initial small array capacity */
    CYP_INIT_CAP8 = 8,     /* initial medium array capacity */
    CYP_MAX_VARS = 16,     /* max Cypher variables in a query */
    CYP_MAX_EDGE_VARS = 8, /* max edge variables */
    CYP_GROWTH_10 = 10,    /* binding growth factor */
    CYP_GROWTH_2 = 2,      /* geometric buffer growth */
    CYP_CHAR_IDX1 = 1,     /* second character index (e.g. op[1]) */
    CYP_EBUF_MASK = 7,
    CYP_NODE_COLS = 4, /* columns per node var: name, qn, label, file */
    CYP_EDGE_COLS = 3, /* columns per edge var: name, qn, label */
    CYP_COL_BUF = 48,  /* max column buffer (16 vars * 3 cols) */
    CYP_FOUND_NONE = -1,
    /* search miss sentinel */ /* mask for ebuf ring buffer (8 entries) */
};
#define CYP_DBL_MAX 1e308
/* execute_single binds the first pattern's leading node even when the query
 * leaves it unnamed, so the engine needs a name for it. The binding slot is
 * real, which is why check_pattern_var_capacity counts it. */
#define CYP_ANON_HEAD_VAR "_n0"

#include <ctype.h>
#include <limits.h> /* INT_MAX for cross-join allocation bounds */
#include "foundation/compat_regex.h"
#include <stddef.h>
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static char *heap_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = malloc(len + SKIP_ONE);
    if (d) {
        memcpy(d, s, len + 1);
    }
    return d;
}

/* ASCII case-insensitive string equality. */
static bool cyp_ci_eq(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* True for single-argument functions that transform a scalar string value
 * (vs. entity-introspection funcs that act on the bound node/edge). */
static bool is_scalar_value_func(const char *f) {
    return f && (strcmp(f, "toLower") == 0 || strcmp(f, "toUpper") == 0 ||
                 strcmp(f, "toString") == 0 || strcmp(f, "toInteger") == 0 ||
                 strcmp(f, "toFloat") == 0 || strcmp(f, "toBoolean") == 0 ||
                 strcmp(f, "size") == 0 || strcmp(f, "length") == 0 || strcmp(f, "trim") == 0 ||
                 strcmp(f, "ltrim") == 0 || strcmp(f, "rtrim") == 0 || strcmp(f, "reverse") == 0);
}

/* ══════════════════════════════════════════════════════════════════
 *  EXECUTOR
 * ══════════════════════════════════════════════════════════════════ */

/* A binding: maps variable names to nodes and/or edges */
typedef struct {
    const char *var_names[CYP_MAX_VARS]; /* variable names (nodes) */
    cbm_node_t var_nodes[CYP_MAX_VARS];  /* node data */
    int var_count;
    const char *edge_var_names[CYP_MAX_EDGE_VARS]; /* variable names (edges) */
    cbm_edge_t edge_vars[CYP_MAX_EDGE_VARS];       /* edge data */
    int edge_var_count;
    cbm_store_t *store; /* for computing in_degree/out_degree on demand */
} binding_t;

/* Return a string field from a node by property name.  NULL-safe. */
static const char *node_string_field(const cbm_node_t *n, const char *prop) {
    static const struct {
        const char *key;
        size_t offset;
    } fields[] = {
        {"name", offsetof(cbm_node_t, name)},
        {"qualified_name", offsetof(cbm_node_t, qualified_name)},
        {"label", offsetof(cbm_node_t, label)},
        {"file_path", offsetof(cbm_node_t, file_path)},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strcmp(prop, fields[i].key) == 0) {
            const char *val = *(const char **)((const char *)n + fields[i].offset);
            return val ? val : "";
        }
    }
    return NULL;
}

/* Get node property by name.
 * store may be NULL; only needed for virtual degree properties. */
static const char *json_extract_prop(const char *json, const char *key, char *buf, size_t buf_sz);

static const char *node_prop(const cbm_node_t *n, const char *prop, cbm_store_t *store) {
    if (!n || !prop) {
        return "";
    }
    const char *str = node_string_field(n, prop);
    if (str && str[0]) {
        return str;
    }
    /* Computed and JSON-derived values live in rotating thread-local buffers:
     * a single row (or an ORDER-BY comparison) reads several of these before any
     * of them is copied out, so returning one shared static buffer would alias
     * every column to the last value read. Mirrors edge_prop's rotation. */
    static _Thread_local char bufs[CYP_BUF_8][CBM_SZ_512];
    static _Thread_local int buf_idx = 0;
    char *out = bufs[buf_idx];
    buf_idx = (buf_idx + SKIP_ONE) % CYP_BUF_8;

    if (strcmp(prop, "start_line") == 0) {
        snprintf(out, CBM_SZ_512, "%d", n->start_line);
        return out;
    }
    if (strcmp(prop, "end_line") == 0) {
        snprintf(out, CBM_SZ_512, "%d", n->end_line);
        return out;
    }
    /* Virtual computed properties: in_degree/out_degree via CALLS edges.
     * Enables Cypher dead-code detection: WHERE n.in_degree = '0'. */
    if (store && (strcmp(prop, "in_degree") == 0 || strcmp(prop, "out_degree") == 0)) {
        int in_deg = 0;
        int out_deg = 0;
        cbm_store_node_degree(store, n->id, &in_deg, &out_deg);
        int val = (strcmp(prop, "in_degree") == 0) ? in_deg : out_deg;
        snprintf(out, CBM_SZ_512, "%d", val);
        return out;
    }
    /* Fall back to any value stored in the node's properties JSON — exposes the
     * extraction metrics (complexity, cognitive, loop_count, loop_depth,
     * transitive_loop_depth, recursive) and any other persisted property to
     * WHERE/RETURN, e.g. WHERE n.loop_depth >= 2. */
    if (n->properties_json && n->properties_json[0] == '{') {
        const char *v = json_extract_prop(n->properties_json, prop, out, CBM_SZ_512);
        if (v && v[0]) {
            return v;
        }
    }
    return "";
}

/* Extract a string value from JSON properties_json by key.
 * Writes result to buf (up to buf_sz). Returns buf if found, "" otherwise.
 * Handles both string values ("key":"value") and numeric values ("key":1.5). */
static const char *json_extract_prop(const char *json, const char *key, char *buf, size_t buf_sz) {
    if (!json || !key) {
        buf[0] = '\0';
        return buf;
    }
    /* Build search pattern: "key": */
    char pattern[CBM_SZ_256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        buf[0] = '\0';
        return buf;
    }
    p += strlen(pattern);
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '"') {
        /* String value. Keep escape pairs intact: an escaped quote must not end
         * the value early. */
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < buf_sz - SKIP_ONE) {
            if (*p == '\\' && p[SKIP_ONE] && i + SKIP_ONE < buf_sz - SKIP_ONE) {
                buf[i++] = *p++;
            }
            buf[i++] = *p++;
        }
        buf[i] = '\0';
    } else if (*p == '[' || *p == '{') {
        /* Array or object value: copy through the matching close bracket */
        size_t i = 0;
        int depth = 0;
        bool in_string = false;
        while (*p && i < buf_sz - SKIP_ONE) {
            char c = *p++;
            buf[i++] = c;
            if (in_string) {
                if (c == '\\' && *p && i < buf_sz - SKIP_ONE) {
                    buf[i++] = *p++;
                } else if (c == '"') {
                    in_string = false;
                }
            } else if (c == '"') {
                in_string = true;
            } else if (c == '[' || c == '{') {
                depth++;
            } else if ((c == ']' || c == '}') && --depth == 0) {
                break;
            }
        }
        buf[i] = '\0';
    } else {
        /* Numeric or other value */
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < buf_sz - SKIP_ONE) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
    }
    return buf;
}

/* Get edge property by name. Uses rotating static buffers to allow
 * multiple concurrent calls (e.g. projecting r.url_path, r.confidence
 * in the same row). */
static const char *edge_prop(const cbm_edge_t *e, const char *prop) {
    if (!e || !prop) {
        return "";
    }
    if (strcmp(prop, "type") == 0) {
        return e->type ? e->type : "";
    }
    /* Rotate through 8 static buffers so multiple props can be accessed per row */
    static char ebufs[CYP_BUF_8][CBM_SZ_512];
    static int ebuf_idx = 0;
    char *buf = ebufs[ebuf_idx++ & CYP_EBUF_MASK];
    json_extract_prop(e->properties_json, prop, buf, CBM_SZ_512);
    return buf;
}

/* Find an edge variable in a binding */
static cbm_edge_t *binding_get_edge(binding_t *b, const char *var) {
    for (int i = 0; i < b->edge_var_count; i++) {
        if (strcmp(b->edge_var_names[i], var) == 0) {
            return &b->edge_vars[i];
        }
    }
    return NULL;
}

/* Find a variable's node in a binding */
static cbm_node_t *binding_get(binding_t *b, const char *var) {
    for (int i = 0; i < b->var_count; i++) {
        if (strcmp(b->var_names[i], var) == 0) {
            return &b->var_nodes[i];
        }
    }
    return NULL;
}

/* Deep copy a node: heap-dup all string fields so the binding owns them */
static void node_deep_copy(cbm_node_t *dst, const cbm_node_t *src) {
    *dst = *src;
    dst->project = heap_strdup(src->project);
    dst->label = heap_strdup(src->label);
    dst->name = heap_strdup(src->name);
    dst->qualified_name = heap_strdup(src->qualified_name);
    dst->file_path = heap_strdup(src->file_path);
    dst->properties_json = heap_strdup(src->properties_json);
}

static void node_fields_free(cbm_node_t *n) {
    if (!n) {
        return;
    }
    safe_str_free(&n->project);
    safe_str_free(&n->label);
    safe_str_free(&n->name);
    safe_str_free(&n->qualified_name);
    safe_str_free(&n->file_path);
    safe_str_free(&n->properties_json);
}

/* Deep copy an edge (binding owns the strings) */
static void edge_deep_copy(cbm_edge_t *dst, const cbm_edge_t *src) {
    *dst = *src;
    dst->project = heap_strdup(src->project);
    dst->type = heap_strdup(src->type);
    dst->properties_json = heap_strdup(src->properties_json);
}

static void edge_fields_free(cbm_edge_t *e) {
    safe_str_free(&e->project);
    safe_str_free(&e->type);
    safe_str_free(&e->properties_json);
}

/* Set an edge variable in a binding */
static void binding_set_edge(binding_t *b, const char *var, const cbm_edge_t *edge) {
    /* Check existing — free old fields first */
    for (int i = 0; i < b->edge_var_count; i++) {
        if (strcmp(b->edge_var_names[i], var) == 0) {
            edge_fields_free(&b->edge_vars[i]);
            edge_deep_copy(&b->edge_vars[i], edge);
            return;
        }
    }
    if (b->edge_var_count >= CYP_MAX_EDGE_VARS) {
        return;
    }
    b->edge_var_names[b->edge_var_count] = var; /* not owned — points to AST string */
    edge_deep_copy(&b->edge_vars[b->edge_var_count], edge);
    b->edge_var_count++;
}

/* Free all deep-copied nodes and edges in a binding */
static void binding_free(binding_t *b) {
    for (int i = 0; i < b->var_count; i++) {
        node_fields_free(&b->var_nodes[i]);
    }
    for (int i = 0; i < b->edge_var_count; i++) {
        edge_fields_free(&b->edge_vars[i]);
    }
}

/* Deep-copy a binding (so source and dest own separate string copies) */
static void binding_copy(binding_t *dst, const binding_t *src) {
    dst->var_count = src->var_count;
    for (int i = 0; i < src->var_count; i++) {
        dst->var_names[i] = src->var_names[i]; /* AST-owned, not freed */
        node_deep_copy(&dst->var_nodes[i], &src->var_nodes[i]);
    }
    dst->edge_var_count = src->edge_var_count;
    for (int i = 0; i < src->edge_var_count; i++) {
        dst->edge_var_names[i] = src->edge_var_names[i]; /* AST-owned */
        edge_deep_copy(&dst->edge_vars[i], &src->edge_vars[i]);
    }
    dst->store = src->store;
}

/* Deep-copy a node into a binding (binding owns the strings) */
static void binding_set(binding_t *b, const char *var, const cbm_node_t *node) {
    /* Check existing — free old fields first */
    for (int i = 0; i < b->var_count; i++) {
        if (strcmp(b->var_names[i], var) == 0) {
            node_fields_free(&b->var_nodes[i]);
            node_deep_copy(&b->var_nodes[i], node);
            return;
        }
    }
    if (b->var_count >= CYP_MAX_VARS) {
        return;
    }
    b->var_names[b->var_count] = var; /* not owned — points to AST string */
    node_deep_copy(&b->var_nodes[b->var_count], node);
    b->var_count++;
}

static const char *eval_multiarg_func(binding_t *b, const cbm_return_item_t *item, char *buf,
                                      size_t bufsz);

/* Resolve the actual property value for a condition from a binding */
static const char *resolve_condition_value(const cbm_condition_t *c, binding_t *b) {
    /* Multi-arg scalar function LHS: coalesce(f.depth, 0) >= 2 (#874).
     * Evaluated through the same code path as RETURN projections. The value is
     * consumed by eval_condition before any other condition is resolved, so a
     * single thread-local buffer per call is safe. */
    if (c->func) {
        static _Thread_local char func_buf[CBM_SZ_512];
        cbm_return_item_t item = {0};
        item.variable = c->variable;
        item.property = c->property;
        item.func = c->func;
        item.args = c->args;
        item.arg_count = c->arg_count;
        return eval_multiarg_func(b, &item, func_buf, sizeof(func_buf));
    }

    cbm_edge_t *e = binding_get_edge(b, c->variable);
    if (e) {
        return edge_prop(e, c->property);
    }
    cbm_node_t *n = binding_get(b, c->variable);
    if (!n) {
        return NULL; /* unbound variable */
    }
    if (c->property) {
        return node_prop(n, c->property, b->store);
    }
    /* Bare alias (e.g. post-WITH virtual var) — use node name directly */
    return n->name ? n->name : "";
}

/* Evaluate a comparison operator between actual and expected strings. */
static bool eval_comparison_op(const char *op, const char *actual, const char *expected) {
    if (strcmp(op, "=") == 0) {
        return strcmp(actual, expected) == 0;
    }
    if (strcmp(op, "<>") == 0) {
        return strcmp(actual, expected) != 0;
    }
    if (strcmp(op, "=~") == 0) {
        cbm_regex_t re;
        if (cbm_regcomp(&re, expected, CBM_REG_EXTENDED | CBM_REG_NOSUB) != 0) {
            return false;
        }
        int rc = cbm_regexec(&re, actual, 0, NULL, 0);
        cbm_regfree(&re);
        return rc == 0;
    }
    if (strcmp(op, "CONTAINS") == 0) {
        return strstr(actual, expected) != NULL;
    }
    if (strcmp(op, "STARTS WITH") == 0) {
        return strncmp(actual, expected, strlen(expected)) == 0;
    }
    if (strcmp(op, "ENDS WITH") == 0) {
        size_t alen = strlen(actual);
        size_t elen = strlen(expected);
        return alen >= elen && strcmp(actual + alen - elen, expected) == 0;
    }
    if (strcmp(op, ">") == 0 || strcmp(op, "<") == 0 || strcmp(op, ">=") == 0 ||
        strcmp(op, "<=") == 0) {
        double a = strtod(actual, NULL);
        double exp_val = strtod(expected, NULL);
        if (op[0] == '>' && op[CYP_CHAR_IDX1] == '=') {
            return a >= exp_val;
        }
        if (op[0] == '<' && op[CYP_CHAR_IDX1] == '=') {
            return a <= exp_val;
        }
        if (op[0] == '>') {
            return a > exp_val;
        }
        return a < exp_val;
    }
    return false;
}

/* Evaluate a WHERE condition against a binding */
static bool eval_condition(const cbm_condition_t *c, binding_t *b) {
    /* Label test: WHERE n:Label (#241) — compare the bound node's label
     * directly rather than a property value. */
    if (strcmp(c->op, "HAS_LABEL") == 0) {
        cbm_node_t *n = binding_get(b, c->variable);
        bool result = n && n->label && c->value && strcmp(n->label, c->value) == 0;
        return c->negated ? !result : result;
    }

    /* EXISTS { (var)-[:TYPE]->() }: does the bound node have any edge of the
     * given type in the requested direction? (dir 0=out, 1=in, 2=any) */
    if (strcmp(c->op, "EXISTS") == 0) {
        cbm_node_t *n = binding_get(b, c->variable);
        bool result = false;
        if (n && b->store) {
            cbm_edge_t *edges = NULL;
            int cnt = 0;
            if (c->exists_dir != 1) { /* outbound or any */
                if (c->value) {
                    cbm_store_find_edges_by_source_type(b->store, n->id, c->value, &edges, &cnt);
                } else {
                    cbm_store_find_edges_by_source(b->store, n->id, &edges, &cnt);
                }
                result = cnt > 0;
                cbm_store_free_edges(edges, cnt);
            }
            if (!result && c->exists_dir != 0) { /* inbound or any */
                edges = NULL;
                cnt = 0;
                if (c->value) {
                    cbm_store_find_edges_by_target_type(b->store, n->id, c->value, &edges, &cnt);
                } else {
                    cbm_store_find_edges_by_target(b->store, n->id, &edges, &cnt);
                }
                result = cnt > 0;
                cbm_store_free_edges(edges, cnt);
            }
        }
        return c->negated ? !result : result;
    }

    const char *actual = resolve_condition_value(c, b);
    if (!actual) {
        return true;
    }

    bool result;

    /* IS NULL / IS NOT NULL */
    if (strcmp(c->op, "IS NULL") == 0) {
        result = (actual[0] == '\0');
        return c->negated ? !result : result;
    }
    if (strcmp(c->op, "IS NOT NULL") == 0) {
        result = (actual[0] != '\0');
        return c->negated ? !result : result;
    }

    /* IN [...] */
    if (strcmp(c->op, "IN") == 0) {
        result = false;
        for (int i = 0; i < c->in_value_count; i++) {
            if (strcmp(actual, c->in_values[i]) == 0) {
                result = true;
                break;
            }
        }
        return c->negated ? !result : result;
    }

    const char *expected = c->value;
    if (c->value_variable) {
        cbm_condition_t rhs = {.variable = c->value_variable, .property = c->value_property};
        expected = resolve_condition_value(&rhs, b);
        if (!expected) {
            return true; /* unbound, same as an unbound left side */
        }
    }
    result = eval_comparison_op(c->op, actual, expected);
    return c->negated ? !result : result;
}

/* Expression tree evaluator. Recurses into NOT and into the left operand of a
 * binary node; a chain of one operator is linked along the right child (see
 * expr_chain_link) and is walked here in a loop. */
static bool eval_expr(const cbm_expr_t *e, binding_t *b) { // NOLINT(misc-no-recursion)
    if (!e) {
        return true;
    }
    if (e->type == EXPR_CONDITION) {
        return eval_condition(&e->cond, b);
    }
    if (e->type == EXPR_NOT) {
        return (!eval_expr(e->left, b)) != 0;
    }
    /* AND / OR / XOR. The nested form `a OP (b OP (c ...))` evaluates a, then
     * b, then c, and AND/OR stop at the first operand that decides the result;
     * the loop does exactly that, one link per iteration. A right child that
     * is not a link of the same operator is the chain's last operand. */
    const cbm_expr_type_t type = e->type;
    bool acc = eval_expr(e->left, b);
    const cbm_expr_t *rest = e->right;
    for (;;) {
        if ((type == EXPR_AND && !acc) || (type == EXPR_OR && acc)) {
            return acc;
        }
        bool linked = rest && rest->type == type;
        bool v = eval_expr(linked ? rest->left : rest, b);
        /* AND reaches here only with acc true, OR only with acc false, so the
         * combined value is v itself; XOR keeps the running parity. */
        acc = (type == EXPR_XOR) ? (acc != v) : v;
        if (!linked) {
            return acc;
        }
        rest = rest->right;
    }
}

/* Evaluate WHERE clause — uses expression tree if available, falls back to legacy */
static bool eval_where(const cbm_where_clause_t *w, binding_t *b) {
    if (!w) {
        return true;
    }
    if (w->root) {
        return eval_expr(w->root, b);
    }

    /* Legacy flat evaluation */
    if (w->count == 0) {
        return true;
    }
    bool is_and = (w->op && strcmp(w->op, "AND") == 0) != 0;
    for (int i = 0; i < w->count; i++) {
        bool r = eval_condition(&w->conditions[i], b);
        if (is_and && !r) {
            return false;
        }
        if (!is_and && r) {
            return true;
        }
    }
    return is_and;
}

/* Check if a string value looks like a regex pattern. */
static bool looks_like_regex(const char *s) {
    if (!s) {
        return false;
    }
    return strstr(s, ".*") || strstr(s, ".+") || strchr(s, '[') || strchr(s, '(') ||
           strchr(s, '|') || strchr(s, '^') || strchr(s, '$');
}

/* Check inline property filters.
 * Values that look like regex patterns are matched with POSIX ERE;
 * plain values use exact strcmp. */
static bool check_inline_props(const cbm_node_t *n, const cbm_prop_filter_t *props, int count,
                               cbm_store_t *store) {
    for (int i = 0; i < count; i++) {
        const char *actual = node_prop(n, props[i].key, store);
        if (looks_like_regex(props[i].value)) {
            cbm_regex_t re;
            if (cbm_regcomp(&re, props[i].value, CBM_REG_EXTENDED | CBM_REG_NOSUB) == 0) {
                bool matched = cbm_regexec(&re, actual, 0, NULL, 0) == 0;
                cbm_regfree(&re);
                if (!matched) {
                    return false;
                }
            } else if (strcmp(actual, props[i].value) != 0) {
                return false;
            }
        } else if (strcmp(actual, props[i].value) != 0) {
            return false;
        }
    }
    return true;
}

/* ── Result building helpers ────────────────────────────────────── */

typedef struct {
    const char ***rows;
    int row_count;
    int row_cap;
    const char **columns;
    int col_count;
} result_builder_t;

static void rb_init(result_builder_t *rb) {
    memset(rb, 0, sizeof(*rb));
    rb->row_cap = CBM_SZ_32;
    rb->rows = malloc(rb->row_cap * sizeof(const char **));
}

static void rb_set_columns(result_builder_t *rb, const char **cols, int count) {
    rb->columns = malloc((count > 0 ? (size_t)count : SKIP_ONE) * sizeof(const char *));
    for (int i = 0; i < count; i++) {
        rb->columns[i] = heap_strdup(cols[i]);
    }
    rb->col_count = count;
}

static void rb_add_row(result_builder_t *rb, const char **values) {
    if (rb->row_count >= rb->row_cap) {
        rb->row_cap *= PAIR_LEN;
        rb->rows = safe_realloc(rb->rows, rb->row_cap * sizeof(const char **));
    }
    const char **row =
        malloc((rb->col_count > 0 ? (size_t)rb->col_count : SKIP_ONE) * sizeof(const char *));
    for (int i = 0; i < rb->col_count; i++) {
        row[i] = values[i] ? heap_strdup(values[i]) : heap_strdup("");
    }
    rb->rows[rb->row_count++] = row;
}

/* ── Main execution ─────────────────────────────────────────────── */

/* Hard ceiling: queries returning more than this trigger an error instead of data.
 * Prevents accidental multi-GB JSON payloads from unbounded MATCH (n) RETURN n. */
#define CYPHER_RESULT_CEILING 100000

/* Wall-clock execution deadline (upstream 6bd11ffb, #601). The row ceiling
 * only fires once rows exist, but an unbounded OPTIONAL MATCH or a GROUP BY
 * with ~one group per node does O(bindings x groups) work for minutes before
 * a single row is produced, so the ceiling never trips and the caller hangs.
 * A monotonic deadline aborts runaway queries with a clear, actionable error.
 * Checked (throttled) in the scan, expansion and aggregation hot loops.
 * Reuses the fork's cbm_now_ms clock; no new public configuration option. */
#define CYPHER_DEADLINE_BUDGET_MS 30000  /* 30s: generous for legit heavy queries */
#define CYPHER_DEADLINE_CHECK_MASK 0x3FF /* sample the clock every 1024 iterations */

static _Thread_local uint64_t g_cypher_deadline_ms = 0; /* absolute; 0 = disarmed */
static _Thread_local bool g_cypher_timed_out = false;
static _Thread_local int64_t g_cypher_deadline_override_ms = -1; /* test hook; <0 = default */

static void cypher_deadline_arm(void) {
    g_cypher_timed_out = false;
    int64_t budget = g_cypher_deadline_override_ms >= 0 ? g_cypher_deadline_override_ms
                                                        : CYPHER_DEADLINE_BUDGET_MS;
    g_cypher_deadline_ms = cbm_now_ms() + (uint64_t)budget;
}

/* True once the query has run past its wall-clock budget. Sticky: after the
 * first trip every subsequent call returns true, so later loops short-circuit. */
static bool cypher_deadline_exceeded(void) {
    if (g_cypher_timed_out) {
        return true;
    }
    if (g_cypher_deadline_ms == 0) {
        return false;
    }
    if (cbm_now_ms() >= g_cypher_deadline_ms) {
        g_cypher_timed_out = true;
        return true;
    }
    return false;
}

/* Test-only: force the execution budget (ms) for subsequent queries on this
 * thread. 0 = trip on the first hot-loop check; <0 restores the default. */
void cbm_cypher_test_set_deadline_ms(int64_t budget_ms) {
    g_cypher_deadline_override_ms = budget_ms;
}

static _Thread_local const char *g_cypher_test_expire_site = NULL; /* test hook */

/* cypher_deadline_exceeded for a named phase. The test hook moves the deadline
 * into the past when that phase first checks it, so a test can expire the
 * budget inside one specific phase. */
static bool cypher_deadline_exceeded_at(const char *site) {
    if (g_cypher_test_expire_site && strcmp(site, g_cypher_test_expire_site) == 0) {
        g_cypher_deadline_ms = SKIP_ONE; /* an instant already in the past */
    }
    return cypher_deadline_exceeded();
}

/* Test-only: expire the deadline when the named phase ("cross_join",
 * "cross_join_rels", "sort_bindings", "order_by") checks it; NULL disables. */
void cbm_cypher_test_expire_deadline_at(const char *site) {
    g_cypher_test_expire_site = site;
}

/* ── Binding virtual variables (for WITH clause) ──────────────── */

static const char *binding_get_virtual(binding_t *b, const char *var, const char *prop) {
    if (!var) {
        return "";
    }
    /* Check virtual vars first (from WITH projection) */
    char full[CBM_SZ_256];
    if (prop) {
        snprintf(full, sizeof(full), "%s.%s", var, prop);
    } else {
        snprintf(full, sizeof(full), "%s", var);
    }
    for (int i = 0; i < b->var_count; i++) {
        if (strcmp(b->var_names[i], full) == 0) {
            return b->var_nodes[i].name ? b->var_nodes[i].name : "";
        }
    }
    /* Fall through to normal lookup */
    cbm_edge_t *e = binding_get_edge(b, var);
    if (e) {
        /* Bare `RETURN r` on an edge: surface the full properties JSON
         * (or "{}" if none) so callers can inspect timestamps, weights,
         * etc. without naming each property. */
        return prop ? edge_prop(e, prop) : (e->properties_json ? e->properties_json : "{}");
    }
    cbm_node_t *n = binding_get(b, var);
    if (n) {
        if (prop) {
            return node_prop(n, prop, b->store);
        }
        return n->name ? n->name : "";
    }
    return "";
}

/* ── String function application ──────────────────────────────── */

static const char *apply_string_func(const char *func, const char *val, char *buf, size_t buf_sz) {
    if (!func || !val) {
        return val ? val : "";
    }
    if (strcmp(func, "toLower") == 0) {
        size_t i = 0;
        for (; i < buf_sz - SKIP_ONE && val[i]; i++) {
            buf[i] = (char)tolower((unsigned char)val[i]);
        }
        buf[i] = '\0';
        return buf;
    }
    if (strcmp(func, "toUpper") == 0) {
        size_t i = 0;
        for (; i < buf_sz - SKIP_ONE && val[i]; i++) {
            buf[i] = (char)toupper((unsigned char)val[i]);
        }
        buf[i] = '\0';
        return buf;
    }
    if (strcmp(func, "toString") == 0) {
        return val; /* already strings */
    }
    if (strcmp(func, "toInteger") == 0) {
        char *end = NULL;
        long long v = strtoll(val, &end, CBM_DECIMAL_BASE);
        if (end == val) {
            /* Not an integer literal — accept a float string and truncate. */
            char *fend = NULL;
            double d = strtod(val, &fend);
            if (fend == val) {
                return ""; /* non-numeric → null */
            }
            v = (long long)d;
        }
        snprintf(buf, buf_sz, "%lld", v);
        return buf;
    }
    if (strcmp(func, "toFloat") == 0) {
        char *end = NULL;
        double d = strtod(val, &end);
        if (end == val) {
            return ""; /* non-numeric → null */
        }
        snprintf(buf, buf_sz, "%g", d);
        return buf;
    }
    if (strcmp(func, "toBoolean") == 0) {
        if (cyp_ci_eq(val, "true")) {
            return "true";
        }
        if (cyp_ci_eq(val, "false")) {
            return "false";
        }
        return ""; /* not a boolean → null */
    }
    if (strcmp(func, "size") == 0 || strcmp(func, "length") == 0) {
        snprintf(buf, buf_sz, "%zu", strlen(val));
        return buf;
    }
    if (strcmp(func, "trim") == 0 || strcmp(func, "ltrim") == 0 || strcmp(func, "rtrim") == 0) {
        bool do_left = (strcmp(func, "trim") == 0 || strcmp(func, "ltrim") == 0);
        bool do_right = (strcmp(func, "trim") == 0 || strcmp(func, "rtrim") == 0);
        const char *start = val;
        const char *end = val + strlen(val);
        while (do_left && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
            start++;
        }
        while (do_right && end > start &&
               (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
            end--;
        }
        size_t n = (size_t)(end - start);
        if (n >= buf_sz) {
            n = buf_sz - SKIP_ONE;
        }
        memcpy(buf, start, n);
        buf[n] = '\0';
        return buf;
    }
    if (strcmp(func, "reverse") == 0) {
        size_t len = strlen(val);
        if (len >= buf_sz) {
            len = buf_sz - SKIP_ONE;
        }
        for (size_t i = 0; i < len; i++) {
            buf[i] = val[len - SKIP_ONE - i];
        }
        buf[len] = '\0';
        return buf;
    }
    return val;
}

/* ── CASE expression evaluation ───────────────────────────────── */

static const char *eval_case_expr(const cbm_case_expr_t *k, binding_t *b) {
    if (!k) {
        return "";
    }
    for (int i = 0; i < k->branch_count; i++) {
        if (eval_expr(k->branches[i].when_expr, b)) {
            return k->branches[i].then_val ? k->branches[i].then_val : "";
        }
    }
    return k->else_val ? k->else_val : "";
}

/* ── Scan nodes for a pattern ─────────────────────────────────── */

/* True if `actual` matches `pat`, where `pat` may be a '|'-alternation of
 * labels ("A|B|C") — openCypher label alternation (#242). */
static bool label_alt_matches(const char *actual, const char *pat) {
    if (!pat) {
        return true;
    }
    if (!actual) {
        return false;
    }
    if (!strchr(pat, '|')) {
        return strcmp(actual, pat) == 0;
    }
    size_t al = strlen(actual);
    const char *seg = pat;
    while (*seg) {
        const char *bar = strchr(seg, '|');
        size_t seglen = bar ? (size_t)(bar - seg) : strlen(seg);
        if (seglen == al && strncmp(seg, actual, seglen) == 0) {
            return true;
        }
        if (!bar) {
            break;
        }
        seg = bar + SKIP_ONE;
    }
    return false;
}

/* Seed nodes for a label alternation "A|B|C": union the per-label results.
 * Node-struct fields are moved (shallow) into out_nodes; each per-label array
 * container is freed. */
static void scan_alternation_labels(cbm_store_t *store, const char *project, const char *labels,
                                    cbm_node_t **out_nodes, int *out_count) {
    *out_nodes = NULL;
    *out_count = 0;
    int cap = 0;
    char *copy = heap_strdup(labels);
    if (!copy) {
        return;
    }
    char *save = NULL;
    for (char *tok = strtok_r(copy, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
        cbm_node_t *part = NULL;
        int pc = 0;
        cbm_store_find_nodes_by_label(store, project, tok, &part, &pc);
        if (pc > 0 && part) {
            if (*out_count + pc > cap) {
                cap = (*out_count + pc) * PAIR_LEN;
                *out_nodes = safe_realloc(*out_nodes, (size_t)cap * sizeof(cbm_node_t));
            }
            memcpy(*out_nodes + *out_count, part, (size_t)pc * sizeof(cbm_node_t));
            *out_count += pc;
        }
        free(part); /* container only — node fields moved to out_nodes */
    }
    free(copy);
}

/* Every candidate is scanned: max_rows limits projected rows, and WHERE and
 * aggregation run after the scan, so a capped scan would drop real matches
 * (upstream #1196). */
static void scan_pattern_nodes(cbm_store_t *store, const char *project, cbm_node_pattern_t *first,
                               cbm_node_t **out_nodes, int *out_count) {
    if (first->label && strchr(first->label, '|')) {
        scan_alternation_labels(store, project, first->label, out_nodes, out_count);
    } else if (first->label) {
        cbm_store_find_nodes_by_label(store, project, first->label, out_nodes, out_count);
    } else {
        cbm_store_find_nodes(store, project, out_nodes, out_count);
    }
    /* Apply inline property filters — free rejected nodes' strings */
    if (first->prop_count > 0) {
        int kept = 0;
        for (int i = 0; i < *out_count; i++) {
            if (check_inline_props(&(*out_nodes)[i], first->props, first->prop_count, store)) {
                if (kept != i) {
                    (*out_nodes)[kept] = (*out_nodes)[i];
                }
                kept++;
            } else {
                node_fields_free(&(*out_nodes)[i]);
            }
        }
        *out_count = kept;
    }
}

/* ── Expand one pattern's relationships on a set of bindings ──── */

/* A hop's output holds EVERY matched row (upstream #1196). max_rows is an
 * output-row limit that projection enforces, and WHERE and aggregation run
 * after expansion, so the old bind_cap*10 ceiling dropped edges before
 * count() saw them, labeled source or not. The buffer grows geometrically;
 * only allocation failure stops it, and the deadline still bounds time. */
static bool binding_out_append(binding_t **rows, int *count, int *cap, binding_t *nb) {
    if (*count == *cap) {
        int next = *cap > 0 ? *cap * CYP_GROWTH_2 : CYP_GROWTH_10;
        binding_t *grown = realloc(*rows, (size_t)next * sizeof(binding_t));
        if (!grown) {
            binding_free(nb);
            return false;
        }
        *rows = grown;
        *cap = next;
    }
    (*rows)[(*count)++] = *nb;
    return true;
}

/* Process edges: look up target node, filter by label/props, add binding.
 * `inbound` controls which end of the edge is the target id. */
static void process_edges(cbm_store_t *store, cbm_edge_t *edges, int edge_count, bool inbound,
                          const cbm_node_pattern_t *target_node, binding_t *b, const char *to_var,
                          const char *rel_var, binding_t **new_bindings, int *new_count,
                          int *new_cap, int *match_count) {
    /* When the terminal node variable is ALREADY bound (e.g. the second pattern
     * `(c)-[:CALLS]->(f)` where `f` came from an earlier MATCH), we must FILTER
     * to edges that actually reach the bound node — not overwrite the caller's
     * `f` binding with whatever node the edge leads to. Overwriting corrupted
     * the result of dead-code queries and produced wrong rows (#627). */
    cbm_node_t *bound_to = binding_get(b, to_var);
    int64_t bound_to_id = bound_to ? bound_to->id : 0;
    for (int ei = 0; ei < edge_count; ei++) {
        int64_t tid = inbound ? edges[ei].source_id : edges[ei].target_id;
        if (bound_to && tid != bound_to_id) {
            continue; /* edge does not reach the already-bound terminal node */
        }
        cbm_node_t found = {0};
        if (cbm_store_find_node_by_id(store, tid, &found) != CBM_STORE_OK) {
            continue;
        }
        if (target_node->label && !label_alt_matches(found.label, target_node->label)) {
            node_fields_free(&found);
            continue;
        }
        if (!check_inline_props(&found, target_node->props, target_node->prop_count, store)) {
            node_fields_free(&found);
            continue;
        }
        binding_t nb = {0};
        binding_copy(&nb, b);
        binding_set(&nb, to_var, &found);
        if (rel_var) {
            binding_set_edge(&nb, rel_var, &edges[ei]);
        }
        node_fields_free(&found);
        (*match_count)++;
        (void)binding_out_append(new_bindings, new_count, new_cap, &nb);
    }
}

/* Expand variable-length relationship via BFS */
/* Set when a variable-length hop range is clamped to the engine ceiling
 * during the CURRENT execution; cbm_cypher_execute turns it into
 * result->warning so callers can tell "clamped" from "no such path"
 * (upstream 73a3c347/#797). */
static _Thread_local int g_cypher_depth_clamped = 0;
/* Set when a variable-length trail walk hit its row budget or result cap. */
static _Thread_local int g_cypher_trail_truncated = 0;

static void expand_var_length(cbm_store_t *store, cbm_rel_pattern_t *rel,
                              cbm_node_pattern_t *target_node, binding_t *b, cbm_node_t *src,
                              const char *to_var, binding_t **new_bindings, int *new_count,
                              int *new_cap, int *match_count) {
    /* Clamp BOTH the explicit (`*1..N`) and unbounded (`*`, `*..m`) forms to the
     * engine ceiling: an explicit N above the cap was previously honoured
     * verbatim, driving cbm_store_bfs to an unbounded hop count (#887). WARN on
     * clamp — never a silent truncation. */
    int depth_cap = cbm_cypher_max_depth();
    int max_depth = rel->max_hops > 0 ? rel->max_hops : depth_cap;
    if (max_depth > depth_cap) {
        char req_buf[16];
        char cap_buf[16];
        snprintf(req_buf, sizeof(req_buf), "%d", max_depth);
        snprintf(cap_buf, sizeof(cap_buf), "%d", depth_cap);
        cbm_log_warn("cypher.depth_capped", "requested", req_buf, "cap", cap_buf);
        g_cypher_depth_clamped = depth_cap; /* surfaced as result->warning (#797) */
        max_depth = depth_cap;
    }
    cbm_traverse_result_t tr = {0};
    const char *dir = rel->direction ? rel->direction : "outbound";
    cbm_store_bfs_trail(store, src->id, dir, rel->types, rel->type_count, max_depth, CBM_PERCENT,
                        &tr);
    if (tr.truncated) {
        g_cypher_trail_truncated = 1; /* surfaced as result->warning */
    }
    /* An already-bound terminal (e.g. (f)-[*1..2]->(f)) filters hops, as in
     * process_edges, instead of being overwritten. */
    cbm_node_t *bound_to = binding_get(b, to_var);
    int64_t bound_to_id = bound_to ? bound_to->id : 0;
    for (int v = 0; v < tr.visited_count; v++) {
        cbm_node_hop_t *hop = &tr.visited[v];
        if (hop->hop < rel->min_hops) {
            continue;
        }
        if (bound_to && hop->node.id != bound_to_id) {
            continue;
        }
        if (target_node->label && !label_alt_matches(hop->node.label, target_node->label)) {
            continue;
        }
        if (!check_inline_props(&hop->node, target_node->props, target_node->prop_count, store)) {
            continue;
        }
        binding_t nb = {0};
        binding_copy(&nb, b);
        binding_set(&nb, to_var, &hop->node);
        (*match_count)++;
        (void)binding_out_append(new_bindings, new_count, new_cap, &nb);
    }
    cbm_store_traverse_free(&tr);
}

/* Expand fixed-length (1-hop) relationship edges */
static void expand_fixed_length(cbm_store_t *store, cbm_rel_pattern_t *rel,
                                cbm_node_pattern_t *target_node, binding_t *b, cbm_node_t *src,
                                const char *to_var, binding_t **new_bindings, int *new_count,
                                int *new_cap, int *match_count) {
    bool is_inbound = rel->direction && strcmp(rel->direction, "inbound") == 0;
    bool is_any = rel->direction && strcmp(rel->direction, "any") == 0;
    const char *rel_var = rel->variable;

    if (rel->type_count > 0) {
        for (int ti = 0; ti < rel->type_count; ti++) {
            cbm_edge_t *edges = NULL;
            int edge_count = 0;
            if (is_inbound) {
                cbm_store_find_edges_by_target_type(store, src->id, rel->types[ti], &edges,
                                                    &edge_count);
            } else {
                cbm_store_find_edges_by_source_type(store, src->id, rel->types[ti], &edges,
                                                    &edge_count);
            }
            process_edges(store, edges, edge_count, is_inbound, target_node, b, to_var, rel_var,
                          new_bindings, new_count, new_cap, match_count);
            cbm_store_free_edges(edges, edge_count);
        }
        if (is_any) {
            for (int ti = 0; ti < rel->type_count; ti++) {
                cbm_edge_t *edges = NULL;
                int edge_count = 0;
                cbm_store_find_edges_by_target_type(store, src->id, rel->types[ti], &edges,
                                                    &edge_count);
                process_edges(store, edges, edge_count, true, target_node, b, to_var, rel_var,
                              new_bindings, new_count, new_cap, match_count);
                cbm_store_free_edges(edges, edge_count);
            }
        }
    } else {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (is_inbound) {
            cbm_store_find_edges_by_target(store, src->id, &edges, &edge_count);
        } else {
            cbm_store_find_edges_by_source(store, src->id, &edges, &edge_count);
        }
        process_edges(store, edges, edge_count, is_inbound, target_node, b, to_var, rel_var,
                      new_bindings, new_count, new_cap, match_count);
        cbm_store_free_edges(edges, edge_count);
        if (is_any) {
            edges = NULL;
            edge_count = 0;
            cbm_store_find_edges_by_target(store, src->id, &edges, &edge_count);
            process_edges(store, edges, edge_count, true, target_node, b, to_var, rel_var,
                          new_bindings, new_count, new_cap, match_count);
            cbm_store_free_edges(edges, edge_count);
        }
    }
}

static void expand_pattern_rels(cbm_store_t *store, cbm_pattern_t *pat, binding_t **bindings,
                                int *bind_count, int *bind_cap, const char **var_name,
                                bool is_optional) {
    for (int ri = 0; ri < pat->rel_count; ri++) {
        /* Stop expanding further hops once the wall-clock budget is spent
         * (an unbounded expansion is exactly what blows up here). */
        if (cypher_deadline_exceeded()) {
            return;
        }
        cbm_rel_pattern_t *rel = &pat->rels[ri];
        cbm_node_pattern_t *target_node = &pat->nodes[ri + SKIP_ONE];
        const char *to_var = target_node->variable ? target_node->variable : "_n_t";

        bool is_variable_length = (rel->min_hops != SKIP_ONE || rel->max_hops != SKIP_ONE);

        /* The hop's output grows to hold every matched row (upstream #1196);
         * the OPTIONAL fallback shares the same append, so no writer can run
         * off the end and no row is dropped. */
        int new_cap = *bind_count > 0 ? *bind_count : CYP_GROWTH_10;
        binding_t *new_bindings = malloc((size_t)new_cap * sizeof(binding_t));
        if (!new_bindings) {
            return; /* OOM: leave existing bindings untouched rather than corrupt */
        }
        int new_count = 0;

        for (int bi = 0; bi < *bind_count; bi++) {
            if ((bi & CYPHER_DEADLINE_CHECK_MASK) == 0 && cypher_deadline_exceeded()) {
                break;
            }
            binding_t *b = &(*bindings)[bi];
            cbm_node_t *src = binding_get(b, *var_name);
            if (!src) {
                continue;
            }

            int match_count = 0;

            if (is_variable_length) {
                expand_var_length(store, rel, target_node, b, src, to_var, &new_bindings,
                                  &new_count, &new_cap, &match_count);
            } else {
                expand_fixed_length(store, rel, target_node, b, src, to_var, &new_bindings,
                                    &new_count, &new_cap, &match_count);
            }

            /* OPTIONAL MATCH: no expansion for this source, so keep the binding
             * with the target unbound (projection renders it ""). */
            if (is_optional && match_count == 0) {
                binding_t nb = {0};
                binding_copy(&nb, b);
                /* Don't set to_var — it remains unbound; projection returns "" */
                (void)binding_out_append(&new_bindings, &new_count, &new_cap, &nb);
            }
        }

        for (int bi = 0; bi < *bind_count; bi++) {
            binding_free(&(*bindings)[bi]);
        }
        free(*bindings);
        *bindings = new_bindings;
        *bind_count = new_count;
        *bind_cap = new_cap;
        *var_name = to_var;
    }
}

/* ── Sorting and key lookup ────────────────────────────────────────
 * ORDER BY, GROUP BY and DISTINCT used to compare every row with every other
 * row (bubble sorts, linear group and duplicate scans). These helpers keep the
 * same comparisons and keys in O(n log n) and O(n) (upstream 8df0639c). */

/* Compare items a and b; positive means a sorts after b. */
typedef int (*cypher_index_cmp_fn)(const void *ctx, int a, int b);

/* Stable sort of idx[0..n). Both this and the bubble sorts it replaces are
 * stable, so for any consistent ordering they give the same permutation. An
 * item moves before an earlier one only when cmp(earlier, item) > 0, which is
 * the bubble sorts' swap test. Without scratch memory it degrades to an
 * insertion sort with the same test and result. The deadline is checked once
 * per pass under `site`; past it the sort stops with idx still a permutation,
 * and the query fails on its final deadline check. */
static void cypher_stable_sort(int *idx, int n, cypher_index_cmp_fn cmp, const void *ctx,
                               const char *site) {
    if (n < PAIR_LEN) {
        return;
    }
    int *tmp = malloc((size_t)n * sizeof(int));
    if (!tmp) {
        for (int i = SKIP_ONE; i < n; i++) {
            if (cypher_deadline_exceeded_at(site)) {
                return;
            }
            for (int j = i; j > 0 && cmp(ctx, idx[j - SKIP_ONE], idx[j]) > 0; j--) {
                int swap = idx[j];
                idx[j] = idx[j - SKIP_ONE];
                idx[j - SKIP_ONE] = swap;
            }
        }
        return;
    }
    for (int64_t width = SKIP_ONE; width < n; width *= PAIR_LEN) {
        if (cypher_deadline_exceeded_at(site)) {
            break;
        }
        for (int64_t lo = 0; lo < n; lo += PAIR_LEN * width) {
            int64_t mid = lo + width < n ? lo + width : n;
            int64_t hi = mid + width < n ? mid + width : n;
            int64_t i = lo;
            int64_t j = mid;
            int64_t k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = cmp(ctx, idx[i], idx[j]) > 0 ? idx[j++] : idx[i++];
            }
            while (i < mid) {
                tmp[k++] = idx[i++];
            }
            while (j < hi) {
                tmp[k++] = idx[j++];
            }
        }
        memcpy(idx, tmp, (size_t)n * sizeof(int));
    }
    free(tmp);
}

/* Convert intptr_t to void* without triggering performance-no-int-to-ptr. */
static inline void *intptr_to_ptr(intptr_t v) {
    void *p;
    memcpy(&p, &v, sizeof(p));
    return p;
}

/* String key → the index it was first recorded with. Keys are copied: the
 * arrays they mirror are reallocated as they grow. */
typedef struct {
    CBMHashTable *table;
    char **keys;
    int count;
    int cap;
    bool failed; /* an allocation failed: callers fall back to their scans */
} cypher_key_index_t;

/* The recorded index, CYP_FOUND_NONE, or KEY_INDEX_UNKNOWN after a failed
 * allocation (the caller then scans, so a result never depends on memory). */
enum { KEY_INDEX_UNKNOWN = -2 };

static int key_index_find(const cypher_key_index_t *ix, const char *key) {
    if (ix->failed) {
        return KEY_INDEX_UNKNOWN;
    }
    void *value = ix->table ? cbm_ht_get(ix->table, key) : NULL;
    return value ? (int)((intptr_t)value - SKIP_ONE) : CYP_FOUND_NONE;
}

static void key_index_add(cypher_key_index_t *ix, const char *key, int index) {
    if (ix->failed) {
        return;
    }
    if (!ix->table) {
        ix->table = cbm_ht_create(CBM_SZ_256);
    }
    if (ix->table && ix->count >= ix->cap) {
        int cap = ix->cap ? ix->cap * PAIR_LEN : CBM_SZ_256;
        char **keys = realloc(ix->keys, (size_t)cap * sizeof(char *));
        if (keys) {
            ix->keys = keys;
            ix->cap = cap;
        }
    }
    char *copy = ix->table && ix->count < ix->cap ? heap_strdup(key) : NULL;
    if (!copy) {
        ix->failed = true;
        return;
    }
    ix->keys[ix->count++] = copy;
    uint32_t before = cbm_ht_count(ix->table);
    (void)cbm_ht_set(ix->table, copy, intptr_to_ptr(index + SKIP_ONE));
    /* cbm_ht_set drops the entry silently when the table cannot grow. */
    ix->failed = cbm_ht_count(ix->table) == before;
}

static void key_index_free(cypher_key_index_t *ix) {
    if (ix->table) {
        cbm_ht_free(ix->table);
    }
    for (int i = 0; i < ix->count; i++) {
        free(ix->keys[i]);
    }
    free(ix->keys);
    memset(ix, 0, sizeof(*ix));
}

/* A row's DISTINCT identity: every value, length-prefixed so no value can run
 * into the next — equal keys exactly when every value is equal. Built in *buf
 * (grown as needed); NULL when out of memory. */
static const char *row_key(const char *const *vals, int n, char **buf, size_t *cap) {
    size_t need = SKIP_ONE;
    for (int c = 0; c < n; c++) {
        need += strlen(vals[c]) + CBM_SZ_32;
    }
    if (need > *cap) {
        char *grown = realloc(*buf, need);
        if (!grown) {
            return NULL;
        }
        *buf = grown;
        *cap = need;
    }
    size_t used = 0;
    for (int c = 0; c < n; c++) {
        size_t len = strlen(vals[c]);
        int prefix_len = snprintf(*buf + used, *cap - used, "%zu:", len);
        if (prefix_len <= 0 || (size_t)prefix_len + len >= *cap - used) {
            return NULL; /* cannot happen: `need` reserved this */
        }
        used += (size_t)prefix_len;
        memcpy(*buf + used, vals[c], len);
        used += len;
    }
    (*buf)[used] = '\0';
    return *buf;
}

/* ── Result postprocessing helpers ─────────────────────────────── */

/* Find the column index for one ORDER BY key, checking both column names and
 * aliases. Returns -1 if not found. */
static int rb_find_order_column(const result_builder_t *rb, const cbm_return_clause_t *ret,
                                const char *key) {
    for (int ci = 0; ci < rb->col_count; ci++) {
        if (strcmp(rb->columns[ci], key) == 0) {
            return ci;
        }
    }
    for (int ci = 0; ci < ret->count; ci++) {
        if (ret->items[ci].alias && strcmp(ret->items[ci].alias, key) == 0) {
            return ci;
        }
    }
    return CBM_NOT_FOUND;
}

/* Check whether a column contains numeric data by examining the first non-empty value */
static bool rb_is_numeric_column(const result_builder_t *rb, int col) {
    for (int i = 0; i < rb->row_count; i++) {
        const char *v = rb->rows[i][col];
        if (v && *v) {
            const char *p2 = (*v == '-') ? v + SKIP_ONE : v;
            if (*p2 == '\0') {
                return false;
            }
            for (; *p2; p2++) {
                if (*p2 < '0' || *p2 > '9') {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

typedef struct {
    const int *cols;
    const bool *numeric;
    const bool *descs;
    int keys;
    const char ***rows; /* the rows in their pre-sort order */
    const int *values;  /* numeric keys parsed once: values[row * keys + k], or NULL */
} rb_order_ctx_t;

/* One ORDER BY comparison: numeric columns by their (int)strtol value, others
 * by strcmp, a later key breaking ties; positive = a after b. */
static int rb_order_compare(const void *context, int a, int b) {
    const rb_order_ctx_t *ctx = context;
    const int *va = ctx->values ? ctx->values + ((size_t)a * (size_t)ctx->keys) : NULL;
    const int *vb = ctx->values ? ctx->values + ((size_t)b * (size_t)ctx->keys) : NULL;
    int cmp = 0;
    for (int k = 0; k < ctx->keys && cmp == 0; k++) {
        if (ctx->numeric[k]) {
            int x = va ? va[k] : (int)strtol(ctx->rows[a][ctx->cols[k]], NULL, CBM_DECIMAL_BASE);
            int y = vb ? vb[k] : (int)strtol(ctx->rows[b][ctx->cols[k]], NULL, CBM_DECIMAL_BASE);
            cmp = (x > y) - (x < y);
        } else {
            cmp = strcmp(ctx->rows[a][ctx->cols[k]], ctx->rows[b][ctx->cols[k]]);
        }
        if (ctx->descs[k]) {
            cmp = -cmp;
        }
    }
    return cmp;
}

static void rb_apply_order_by(result_builder_t *rb, const cbm_return_clause_t *ret) {
    if (ret->order_key_count == 0) {
        return;
    }
    /* Resolve every key up front; an unresolvable key is skipped, matching the
     * single-key forgiveness (sorting on what can be resolved beats dropping
     * the whole ORDER BY). */
    int cols[CBM_CYPHER_ORDER_KEYS_MAX];
    bool numeric[CBM_CYPHER_ORDER_KEYS_MAX];
    bool descs[CBM_CYPHER_ORDER_KEYS_MAX];
    int keys = 0;
    for (int k = 0; k < ret->order_key_count; k++) {
        int order_col = rb_find_order_column(rb, ret, ret->order_keys[k]);
        if (order_col < 0) {
            continue;
        }
        cols[keys] = order_col;
        numeric[keys] = rb_is_numeric_column(rb, order_col);
        descs[keys] = ret->order_descs[k];
        keys++;
    }
    int n = rb->row_count;
    if (keys == 0 || n < PAIR_LEN) {
        return;
    }
    rb_order_ctx_t ctx = {.cols = cols, .numeric = numeric, .descs = descs, .keys = keys};
    int *idx = malloc((size_t)n * sizeof(int));
    const char ***rows = malloc((size_t)n * sizeof(const char **));
    if (!idx || !rows) {
        /* No scratch memory: insertion sort in place, same comparisons. */
        free(idx);
        free(rows);
        ctx.rows = rb->rows;
        for (int i = SKIP_ONE; i < n; i++) {
            if (cypher_deadline_exceeded_at("order_by")) {
                return;
            }
            for (int j = i; j > 0 && rb_order_compare(&ctx, j - SKIP_ONE, j) > 0; j--) {
                const char **tmp = rb->rows[j];
                rb->rows[j] = rb->rows[j - SKIP_ONE];
                rb->rows[j - SKIP_ONE] = tmp;
            }
        }
        return;
    }
    int *values = malloc((size_t)n * (size_t)keys * sizeof(int));
    for (int r = 0; r < n; r++) {
        idx[r] = r;
        rows[r] = rb->rows[r];
        for (int k = 0; values && k < keys; k++) {
            values[((size_t)r * (size_t)keys) + (size_t)k] =
                numeric[k] ? (int)strtol(rb->rows[r][cols[k]], NULL, CBM_DECIMAL_BASE) : 0;
        }
    }
    ctx.rows = rows;
    ctx.values = values;
    cypher_stable_sort(idx, n, rb_order_compare, &ctx, "order_by");
    for (int r = 0; r < n; r++) {
        rb->rows[r] = rows[idx[r]];
    }
    free(values);
    free(rows);
    free(idx);
}

static void rb_apply_skip_limit(result_builder_t *rb, int skip_n, int limit) {
    /* Skip */
    if (skip_n > 0 && skip_n < rb->row_count) {
        for (int i = 0; i < skip_n; i++) {
            for (int c = 0; c < rb->col_count; c++) {
                safe_str_free(&rb->rows[i][c]);
            }
            free(rb->rows[i]);
        }
        memmove(rb->rows, rb->rows + skip_n, (rb->row_count - skip_n) * sizeof(const char **));
        rb->row_count -= skip_n;
    } else if (skip_n >= rb->row_count) {
        for (int i = 0; i < rb->row_count; i++) {
            for (int c = 0; c < rb->col_count; c++) {
                safe_str_free(&rb->rows[i][c]);
            }
            free(rb->rows[i]);
        }
        rb->row_count = 0;
    }
    /* Limit */
    if (limit >= 0 && rb->row_count > limit) {
        for (int i = limit; i < rb->row_count; i++) {
            for (int c = 0; c < rb->col_count; c++) {
                safe_str_free(&rb->rows[i][c]);
            }
            free(rb->rows[i]);
        }
        rb->row_count = limit;
    }
}

/* Whether row i equals one of the first `kept` rows, column by column. */
static bool rb_row_seen_before(const result_builder_t *rb, int i, int kept) {
    for (int j = 0; j < kept; j++) {
        bool same = true;
        for (int c = 0; c < rb->col_count && same; c++) {
            same = strcmp(rb->rows[i][c], rb->rows[j][c]) == 0;
        }
        if (same) {
            return true;
        }
    }
    return false;
}

static void rb_apply_distinct(result_builder_t *rb) {
    if (rb->row_count <= SKIP_ONE) {
        return;
    }
    cypher_key_index_t seen = {0};
    char *key_buf = NULL;
    size_t key_cap = 0;
    int kept = 0;
    for (int i = 0; i < rb->row_count; i++) {
        const char *key = row_key(rb->rows[i], rb->col_count, &key_buf, &key_cap);
        seen.failed = seen.failed || !key;
        int found = key_index_find(&seen, key);
        bool dup = found == KEY_INDEX_UNKNOWN ? rb_row_seen_before(rb, i, kept) : found >= 0;
        if (!dup) {
            key_index_add(&seen, key, kept);
            if (kept != i) {
                rb->rows[kept] = rb->rows[i];
            }
            kept++;
        } else {
            for (int c = 0; c < rb->col_count; c++) {
                safe_str_free(&rb->rows[i][c]);
            }
            free(rb->rows[i]);
        }
    }
    rb->row_count = kept;
    key_index_free(&seen);
    free(key_buf);
}

/* DISTINCT while projecting (#58). RETURN DISTINCT used to project up to the
 * row cap and only then drop duplicates, so a value first seen past the cap
 * never reached the result (`RETURN DISTINCT type(r)` over more edges than
 * max_rows could miss a rare type). The projection loops now skip duplicates
 * as they go, so the cap counts distinct rows. Only admitted rows are keyed. */
typedef struct {
    bool on;
    cypher_key_index_t seen;
    char *key_buf;
    size_t key_cap;
} rb_distinct_t;

static void rb_distinct_free(rb_distinct_t *d) {
    key_index_free(&d->seen);
    free(d->key_buf);
}

/* Whether vals equals a row rb already holds, column by column. */
static bool rb_holds_row(const result_builder_t *rb, const char *const *vals) {
    for (int j = 0; j < rb->row_count; j++) {
        bool same = true;
        for (int c = 0; c < rb->col_count && same; c++) {
            same = strcmp(rb->rows[j][c], vals[c]) == 0;
        }
        if (same) {
            return true;
        }
    }
    return false;
}

/* Add one projected row under a row cap, skipping a row DISTINCT already
 * holds. Returns false once a new row arrives with the cap full: the caller
 * stops projecting. vals entries may be NULL; they project as "". */
static bool rb_add_row_capped(result_builder_t *rb, rb_distinct_t *d, const char **vals, int cap) {
    const char *key = NULL;
    if (d->on) {
        for (int c = 0; c < rb->col_count; c++) {
            vals[c] = vals[c] ? vals[c] : "";
        }
        key = row_key(vals, rb->col_count, &d->key_buf, &d->key_cap);
        d->seen.failed = d->seen.failed || !key;
        int found = key_index_find(&d->seen, key);
        if (found == KEY_INDEX_UNKNOWN ? rb_holds_row(rb, vals) : found >= 0) {
            return true;
        }
    }
    if (rb->row_count >= cap) {
        return false;
    }
    if (d->on) {
        key_index_add(&d->seen, key, rb->row_count);
    }
    rb_add_row(rb, vals);
    return true;
}

static void rb_free(result_builder_t *rb) {
    for (int i = 0; i < rb->row_count; i++) {
        for (int c = 0; c < rb->col_count; c++) {
            safe_str_free(&rb->rows[i][c]);
        }
        free(rb->rows[i]);
    }
    free(rb->rows);
    for (int i = 0; i < rb->col_count; i++) {
        safe_str_free(&rb->columns[i]);
    }
    free(rb->columns);
}

/* ── Get projection value for a binding + return item ─────────── */

/* Element idx (negative from the end) of a JSON list of plain strings, as
 * labels()/keys() build them; "" (null) when out of range. */
static const char *json_str_list_at(const char *list, int idx, char *buf, size_t buf_sz) {
    const char *starts[CBM_SZ_32];
    size_t lens[CBM_SZ_32];
    int n = 0;
    for (const char *q = list; q && *q && n < CBM_SZ_32; q++) {
        if (*q != '"') {
            continue;
        }
        const char *end = strchr(q + SKIP_ONE, '"');
        if (!end) {
            break;
        }
        starts[n] = q + SKIP_ONE;
        lens[n] = (size_t)(end - q - SKIP_ONE);
        n++;
        q = end;
    }
    int i = idx < 0 ? n + idx : idx;
    if (i < 0 || i >= n || lens[i] >= buf_sz) {
        return "";
    }
    memmove(buf, starts[i], lens[i]);
    buf[lens[i]] = '\0';
    return buf;
}

/* Build a JSON list of a node's non-null property keys: keys(n). */
static const char *node_keys_list(const cbm_node_t *n, char *buf, size_t buf_sz) {
    const struct {
        const char *k;
        bool present;
    } ks[] = {
        {"name", n->name && n->name[0]},
        {"qualified_name", n->qualified_name && n->qualified_name[0]},
        {"label", n->label && n->label[0]},
        {"file_path", n->file_path && n->file_path[0]},
        {"start_line", n->start_line > 0},
        {"end_line", n->end_line > 0},
    };
    size_t pos = 0;
    bool first = true;
    if (pos < buf_sz - SKIP_ONE) {
        buf[pos++] = '[';
    }
    for (size_t i = 0; i < sizeof(ks) / sizeof(ks[0]) && pos < buf_sz - SKIP_ONE; i++) {
        if (!ks[i].present) {
            continue;
        }
        int w = snprintf(buf + pos, buf_sz - pos, "%s\"%s\"", first ? "" : ",", ks[i].k);
        if (w < 0 || (size_t)w >= buf_sz - pos) {
            break;
        }
        pos += (size_t)w;
        first = false;
    }
    if (pos < buf_sz - SKIP_ONE) {
        buf[pos++] = ']';
    }
    buf[pos] = '\0';
    return buf;
}

/* Resolve a function argument to its string value (literal or var.prop). */
static const char *eval_func_arg(binding_t *b, const cbm_func_arg_t *a) {
    if (a->literal) {
        return a->literal;
    }
    return binding_get_virtual(b, a->variable, a->property);
}

/* Evaluate a multi-argument scalar function into func_buf (or a direct value). */
static const char *eval_multiarg_func(binding_t *b, const cbm_return_item_t *item, char *buf,
                                      size_t bufsz) {
    const char *f = item->func;
    int n = item->arg_count;
    if (strcmp(f, "coalesce") == 0) {
        for (int i = 0; i < n; i++) {
            const char *v = eval_func_arg(b, &item->args[i]);
            if (v && v[0]) {
                return v;
            }
        }
        return "";
    }
    if (strcmp(f, "substring") == 0 && n >= 2) {
        const char *s = eval_func_arg(b, &item->args[0]);
        long start = strtol(eval_func_arg(b, &item->args[1]), NULL, CBM_DECIMAL_BASE);
        size_t slen = strlen(s);
        if (start < 0 || (size_t)start >= slen) {
            return "";
        }
        size_t take = slen - (size_t)start;
        if (n >= 3) {
            long len = strtol(eval_func_arg(b, &item->args[2]), NULL, CBM_DECIMAL_BASE);
            if (len < 0) {
                len = 0;
            }
            if ((size_t)len < take) {
                take = (size_t)len;
            }
        }
        if (take >= bufsz) {
            take = bufsz - SKIP_ONE;
        }
        memcpy(buf, s + start, take);
        buf[take] = '\0';
        return buf;
    }
    if ((strcmp(f, "left") == 0 || strcmp(f, "right") == 0) && n >= 2) {
        const char *s = eval_func_arg(b, &item->args[0]);
        long k = strtol(eval_func_arg(b, &item->args[1]), NULL, CBM_DECIMAL_BASE);
        if (k < 0) {
            k = 0;
        }
        size_t slen = strlen(s);
        size_t take = (size_t)k < slen ? (size_t)k : slen;
        if (take >= bufsz) {
            take = bufsz - SKIP_ONE;
        }
        memcpy(buf, (strcmp(f, "left") == 0) ? s : s + (slen - take), take);
        buf[take] = '\0';
        return buf;
    }
    if (strcmp(f, "replace") == 0 && n >= 3) {
        const char *s = eval_func_arg(b, &item->args[0]);
        const char *from = eval_func_arg(b, &item->args[1]);
        const char *to = eval_func_arg(b, &item->args[2]);
        size_t fromlen = strlen(from);
        size_t tolen = strlen(to);
        size_t pos = 0;
        const char *pp = s;
        if (fromlen == 0) {
            snprintf(buf, bufsz, "%s", s);
            return buf;
        }
        while (*pp && pos < bufsz - SKIP_ONE) {
            if (strncmp(pp, from, fromlen) == 0) {
                size_t cpy = tolen;
                if (pos + cpy >= bufsz) {
                    cpy = bufsz - SKIP_ONE - pos;
                }
                memcpy(buf + pos, to, cpy);
                pos += cpy;
                pp += fromlen;
            } else {
                buf[pos++] = *pp++;
            }
        }
        buf[pos] = '\0';
        return buf;
    }
    return ""; /* wrong arity → null */
}

static const char *project_item(binding_t *b, cbm_return_item_t *item, char *func_buf,
                                size_t buf_sz) {
    if (item->kase) {
        return eval_case_expr(item->kase, b);
    }
    if (item->args) {
        return eval_multiarg_func(b, item, func_buf, buf_sz);
    }
    /* Entity-introspection functions operate on the bound node/edge itself,
     * not on a scalar property value. */
    if (item->func) {
        if (strcmp(item->func, "labels") == 0) {
            cbm_node_t *n = binding_get(b, item->variable);
            if (n && n->label) {
                snprintf(func_buf, buf_sz, "[\"%s\"]", n->label);
                return item->has_index ? json_str_list_at(func_buf, item->index, func_buf, buf_sz)
                                       : func_buf;
            }
            return item->has_index ? "" : "[]";
        }
        if (strcmp(item->func, "type") == 0) {
            cbm_edge_t *e = binding_get_edge(b, item->variable);
            return (e && e->type) ? e->type : "";
        }
        if (strcmp(item->func, "id") == 0) {
            cbm_node_t *n = binding_get(b, item->variable);
            if (n) {
                snprintf(func_buf, buf_sz, "%lld", (long long)n->id);
                return func_buf;
            }
            cbm_edge_t *e = binding_get_edge(b, item->variable);
            if (e) {
                snprintf(func_buf, buf_sz, "%lld", (long long)e->id);
                return func_buf;
            }
            return "";
        }
        if (strcmp(item->func, "keys") == 0) {
            cbm_node_t *n = binding_get(b, item->variable);
            if (!n) {
                return item->has_index ? "" : "[]";
            }
            const char *keys = node_keys_list(n, func_buf, buf_sz);
            return item->has_index ? json_str_list_at(keys, item->index, func_buf, buf_sz) : keys;
        }
        if (strcmp(item->func, "properties") == 0) {
            cbm_node_t *n = binding_get(b, item->variable);
            if (n) {
                return n->properties_json ? n->properties_json : "{}";
            }
            cbm_edge_t *e = binding_get_edge(b, item->variable);
            if (e) {
                return e->properties_json ? e->properties_json : "{}";
            }
            return "{}";
        }
    }
    const char *raw = binding_get_virtual(b, item->variable, item->property);
    if (is_scalar_value_func(item->func)) {
        return apply_string_func(item->func, raw, func_buf, buf_sz);
    }
    /* Copy into the caller's per-column buffer. `raw` may point to node_prop's
     * rotating scratch buffer, which the next column's projection would overwrite
     * before rb_add_row copies the assembled row — aliasing every such column to
     * the last value read. The per-column func_buf gives each column stable storage. */
    if (raw && raw != func_buf && raw[0]) {
        size_t len = strlen(raw);
        if (len >= buf_sz) {
            len = buf_sz - SKIP_ONE;
        }
        memcpy(func_buf, raw, len);
        func_buf[len] = '\0';
        return func_buf;
    }
    return raw ? raw : "";
}

/* Check if a function name is an aggregate */
static bool is_aggregate_func(const char *func) {
    return func &&
           (strcmp(func, "COUNT") == 0 || strcmp(func, "SUM") == 0 || strcmp(func, "AVG") == 0 ||
            strcmp(func, "MIN") == 0 || strcmp(func, "MAX") == 0 || strcmp(func, "COLLECT") == 0);
}

/* Grow a string list for one more entry at `idx`: the capacity doubles at
 * every power of two, so n appends cost O(n) copying, not O(n^2). */
static char **string_list_reserve(char **list, int idx) {
    if (idx > 0 && (idx & (idx - SKIP_ONE)) != 0) {
        return list; /* capacity is the next power of two above idx */
    }
    return safe_realloc(list, (size_t)(idx ? idx * PAIR_LEN : SKIP_ONE) * sizeof(char *));
}

enum { DISTINCT_SET_MIN = 16 }; /* below this a set is scanned, not hashed */

/* Hash the first `count` list entries into a new set; NULL if it cannot be
 * built in full (the caller then keeps scanning the list). */
static CBMHashTable *distinct_set_build(char **list, int count) {
    CBMHashTable *set = cbm_ht_create(CBM_SZ_64);
    for (int i = 0; set && i < count; i++) {
        (void)cbm_ht_set(set, list[i], intptr_to_ptr(SKIP_ONE));
    }
    if (set && cbm_ht_count(set) != (uint32_t)count) {
        cbm_ht_free(set);
        set = NULL;
    }
    return set;
}

/* Append `val` to a string list only if not already present — i.e. maintain a
 * set of distinct values. Used by COUNT(DISTINCT x) (#239). Past
 * DISTINCT_SET_MIN values the membership test goes through *set, whose keys
 * are the list's own strings; a set that cannot be built stays a scan. */
static void distinct_list_add(char ***list, int *count, CBMHashTable **set, const char *val) {
    if (*set) {
        if (cbm_ht_has(*set, val)) {
            return;
        }
    } else {
        for (int i = 0; i < *count; i++) {
            if (strcmp((*list)[i], val) == 0) {
                return;
            }
        }
    }
    int idx = *count;
    *list = string_list_reserve(*list, idx);
    char *copy = *list ? heap_strdup(val) : NULL;
    if (!copy) {
        /* Out of memory: keep a consistent (possibly emptied) set. */
        if (!*list) {
            *count = 0;
            if (*set) {
                cbm_ht_free(*set);
                *set = NULL;
            }
        }
        return;
    }
    (*list)[idx] = copy;
    (*count)++;
    if (*set) {
        (void)cbm_ht_set(*set, copy, intptr_to_ptr(SKIP_ONE));
        if (cbm_ht_count(*set) != (uint32_t)*count) {
            cbm_ht_free(*set); /* the insert was dropped: back to scanning */
            *set = NULL;
        }
    } else if (*count >= DISTINCT_SET_MIN) {
        *set = distinct_set_build(*list, *count);
    }
}

typedef struct {
    const cbm_return_clause_t *wc;
    const char **values; /* values[i * keys + k]: binding i's ORDER BY key k */
    bool *numbered;      /* whether strtod consumed a number from that value */
    double *numbers;
    char **owned; /* owned[i * keys + k]: copy of a transient value, else NULL */
} with_order_ctx_t;

/* One WITH ORDER BY comparison: per key, numerically when both values start
 * with a number, else by strcmp; later keys break ties, direction is per key
 * (upstream 0a0ef516). Positive = a after b. */
static int with_order_compare(const void *context, int a, int b) {
    const with_order_ctx_t *ctx = context;
    size_t keys = (size_t)ctx->wc->order_key_count;
    int cmp = 0;
    for (size_t k = 0; k < keys && cmp == 0; k++) {
        size_t ia = ((size_t)a * keys) + k;
        size_t ib = ((size_t)b * keys) + k;
        if (ctx->numbered[ia] && ctx->numbered[ib]) {
            cmp = (ctx->numbers[ia] > ctx->numbers[ib]) - (ctx->numbers[ia] < ctx->numbers[ib]);
        } else {
            cmp = strcmp(ctx->values[ia], ctx->values[ib]);
        }
        if (ctx->wc->order_descs[k]) {
            cmp = -cmp;
        }
    }
    return cmp;
}

/* Resolve one WITH ORDER BY key on a projected binding. The key is either a
 * projected name (`c`, or an unaliased `f.name`) or a property of a carried
 * node (`g.start_line` after `WITH f AS g`, #57): an exact projected name
 * wins, otherwise `var.prop` is split and resolved on the carried node. A
 * property value may live in node_prop's rotating buffers, so it is reported
 * as transient. */
static const char *order_key_value(binding_t *b, const char *key, bool *transient) {
    *transient = false;
    const char *dot = strchr(key, '.');
    if (!dot) {
        return binding_get_virtual(b, key, NULL);
    }
    for (int i = 0; i < b->var_count; i++) {
        if (strcmp(b->var_names[i], key) == 0) {
            return binding_get_virtual(b, key, NULL);
        }
    }
    char var[CBM_SZ_256];
    size_t vlen = (size_t)(dot - key);
    if (vlen >= sizeof(var)) {
        return "";
    }
    memcpy(var, key, vlen);
    var[vlen] = '\0';
    *transient = true;
    return binding_get_virtual(b, var, dot + SKIP_ONE);
}

/* Resolve binding i's ORDER BY keys into the context. A projected name
 * resolves to a string the binding owns, which stays valid while the bindings
 * are only reordered; a carried node's property is copied into ctx->owned. */
static void with_order_resolve(with_order_ctx_t *ctx, binding_t *b, int i) {
    size_t keys = (size_t)ctx->wc->order_key_count;
    for (size_t k = 0; k < keys; k++) {
        size_t cell = ((size_t)i * keys) + k;
        bool transient = false;
        const char *v = order_key_value(b, ctx->wc->order_keys[k], &transient);
        free(ctx->owned[cell]);
        ctx->owned[cell] = NULL;
        if (transient) {
            ctx->owned[cell] = heap_strdup(v);
            v = ctx->owned[cell] ? ctx->owned[cell] : "";
        }
        char *end = NULL;
        ctx->values[cell] = v;
        ctx->numbers[cell] = strtod(v, &end);
        ctx->numbered[cell] = end != v;
    }
}

/* Sort bindings by the ORDER BY key list (virtual variables), stably. Each
 * binding's keys are resolved once, not twice per comparison. */
static void sort_bindings(binding_t *vbindings, int count, const cbm_return_clause_t *wc) {
    if (count < PAIR_LEN || wc->order_key_count <= 0) {
        return;
    }
    size_t keys = (size_t)wc->order_key_count;
    size_t cells = (size_t)count * keys;
    with_order_ctx_t ctx = {.wc = wc,
                            .values = malloc(cells * sizeof(const char *)),
                            .numbered = malloc(cells * sizeof(bool)),
                            .numbers = malloc(cells * sizeof(double)),
                            .owned = calloc(cells, sizeof(char *))};
    int *idx = malloc((size_t)count * sizeof(int));
    binding_t *sorted = malloc((size_t)count * sizeof(binding_t));
    if (ctx.values && ctx.numbered && ctx.numbers && ctx.owned && idx && sorted) {
        for (int i = 0; i < count; i++) {
            idx[i] = i;
            with_order_resolve(&ctx, &vbindings[i], i);
        }
        cypher_stable_sort(idx, count, with_order_compare, &ctx, "sort_bindings");
        for (int i = 0; i < count; i++) {
            sorted[i] = vbindings[idx[i]];
        }
        memcpy(vbindings, sorted, (size_t)count * sizeof(binding_t));
    } else {
        /* No scratch memory: insertion sort in place, same comparisons, with
         * the two compared bindings' keys resolved into a two-cell context. */
        const char *pair_values[CBM_CYPHER_ORDER_KEYS_MAX * PAIR_LEN];
        bool pair_numbered[CBM_CYPHER_ORDER_KEYS_MAX * PAIR_LEN];
        double pair_numbers[CBM_CYPHER_ORDER_KEYS_MAX * PAIR_LEN];
        char *pair_owned[CBM_CYPHER_ORDER_KEYS_MAX * PAIR_LEN] = {0};
        with_order_ctx_t pair = {.wc = wc,
                                 .values = pair_values,
                                 .numbered = pair_numbered,
                                 .numbers = pair_numbers,
                                 .owned = pair_owned};
        for (int i = SKIP_ONE; i < count; i++) {
            if (cypher_deadline_exceeded_at("sort_bindings")) {
                break;
            }
            for (int j = i; j > 0; j--) {
                with_order_resolve(&pair, &vbindings[j - SKIP_ONE], 0);
                with_order_resolve(&pair, &vbindings[j], SKIP_ONE);
                if (with_order_compare(&pair, 0, SKIP_ONE) <= 0) {
                    break;
                }
                binding_t tmp = vbindings[j];
                vbindings[j] = vbindings[j - SKIP_ONE];
                vbindings[j - SKIP_ONE] = tmp;
            }
        }
        for (size_t c = 0; c < keys * PAIR_LEN; c++) {
            free(pair_owned[c]);
        }
    }
    if (ctx.owned) {
        for (size_t c = 0; c < cells; c++) {
            free(ctx.owned[c]);
        }
    }
    free(ctx.values);
    free(ctx.numbered);
    free(ctx.numbers);
    free(ctx.owned);
    free(idx);
    free(sorted);
}

/* Apply skip and limit to a binding array, freeing discarded entries */
static void bindings_skip_limit(binding_t *vbindings, int *count, int skip, int limit) {
    if (skip > 0 && skip < *count) {
        for (int i = 0; i < skip; i++) {
            binding_free(&vbindings[i]);
        }
        memmove(vbindings, vbindings + skip, (*count - skip) * sizeof(binding_t));
        *count -= skip;
    } else if (skip >= *count) {
        for (int i = 0; i < *count; i++) {
            binding_free(&vbindings[i]);
        }
        *count = 0;
    }
    if (limit >= 0 && *count > limit) {
        for (int i = limit; i < *count; i++) {
            binding_free(&vbindings[i]);
        }
        *count = limit;
    }
}

/* Sort, skip, and limit binding array in-place */
static void with_sort_skip_limit(const cbm_return_clause_t *wc, binding_t *vbindings, int *vcount) {
    if (wc->order_key_count > 0) {
        sort_bindings(vbindings, *vcount, wc);
    }
    bindings_skip_limit(vbindings, vcount, wc->skip, wc->limit);
}

/* Resolve the alias or compute a default name for a WITH/RETURN item */
static const char *resolve_item_alias(const cbm_return_item_t *item, char *name_buf,
                                      size_t buf_sz) {
    if (item->alias) {
        return item->alias;
    }
    if (item->property) {
        snprintf(name_buf, buf_sz, "%s.%s", item->variable, item->property);
    } else {
        snprintf(name_buf, buf_sz, "%s", item->variable);
    }
    return name_buf;
}

/* ── WITH clause: project bindings through aggregation or rename ── */

/* WITH aggregation group entry */
typedef struct {
    char group_key[CBM_SZ_1K];
    const char **group_vals;
    double *sums;
    int *counts;
    double *mins, *maxs;
    char ***distinct_lists;       /* per-item set of seen values for COUNT(DISTINCT) */
    int *distinct_n;              /* per-item distinct count (#239) */
    CBMHashTable **distinct_sets; /* per-item hash over distinct_lists (lazy) */
    cbm_node_t *group_nodes;      /* per-item carried node (deep copy; id 0 = not a node) */
} with_agg_t;

/* A WITH item that is a bare, bound node variable (`WITH f`, `WITH f AS g`)
 * carries the node itself, not a scalar: return that node, else NULL. Property
 * items, functions (labels/id/keys/...) and unbound OPTIONAL vars (id 0) stay
 * scalar. The carried node keeps every field, so a later g.<prop> resolves
 * exactly as it would without the WITH (#57). */
static const cbm_node_t *with_item_carried_node(const cbm_return_item_t *item, binding_t *b) {
    if (item->func || item->property || !item->variable) {
        return NULL;
    }
    const cbm_node_t *n = binding_get(b, item->variable);
    return (n && n->id > 0) ? n : NULL;
}

/* Name a carried node is bound under: the AST-owned alias or variable, which
 * outlives every binding of the query (binding_set does not copy the name). */
static const char *with_item_node_alias(const cbm_return_item_t *item) {
    return item->alias ? item->alias : item->variable;
}

/* Build a group key from non-aggregate WITH items */
static int with_agg_build_key(cbm_return_clause_t *wc, binding_t *b, char *key, size_t key_sz) {
    int kl = 0;
    for (int ci = 0; ci < wc->count; ci++) {
        if (is_aggregate_func(wc->items[ci].func)) {
            continue;
        }
        char vbuf[CBM_SZ_512];
        const cbm_node_t *gn = with_item_carried_node(&wc->items[ci], b);
        if (gn) {
            /* Group a node by identity, never by its (non-unique) name. */
            (void)snprintf(vbuf, sizeof(vbuf), "#%lld", (long long)gn->id);
        }
        const char *v = gn ? vbuf : project_item(b, &wc->items[ci], vbuf, sizeof(vbuf));
        kl += snprintf(key + kl, key_sz - (size_t)kl, "%s|", v);
        if (kl >= (int)key_sz) {
            kl = (int)key_sz - SKIP_ONE;
        }
    }
    return kl;
}

/* Find or create an aggregation group. Returns index. */
static int with_agg_find_or_create(with_agg_t **aggs, int *agg_cnt, int *agg_cap,
                                   cypher_key_index_t *groups, cbm_return_clause_t *wc,
                                   binding_t *b, const char *key) {
    int existing = key_index_find(groups, key);
    for (int a = 0; existing == KEY_INDEX_UNKNOWN && a < *agg_cnt; a++) {
        if (strcmp((*aggs)[a].group_key, key) == 0) {
            existing = a;
        }
    }
    if (existing >= 0) {
        return existing;
    }
    if (*agg_cnt >= *agg_cap) {
        *agg_cap *= PAIR_LEN;
        *aggs = safe_realloc(*aggs, *agg_cap * sizeof(with_agg_t));
    }
    int found = (*agg_cnt)++;
    snprintf((*aggs)[found].group_key, sizeof((*aggs)[found].group_key), "%s", key);
    key_index_add(groups, (*aggs)[found].group_key, found);
    (*aggs)[found].group_vals = calloc(wc->count, sizeof(const char *));
    (*aggs)[found].sums = calloc(wc->count, sizeof(double));
    (*aggs)[found].counts = calloc(wc->count, sizeof(int));
    (*aggs)[found].mins = calloc(wc->count, sizeof(double));
    (*aggs)[found].maxs = calloc(wc->count, sizeof(double));
    (*aggs)[found].distinct_lists = calloc(wc->count, sizeof(char **));
    (*aggs)[found].distinct_n = calloc(wc->count, sizeof(int));
    (*aggs)[found].distinct_sets = calloc(wc->count, sizeof(CBMHashTable *));
    (*aggs)[found].group_nodes = calloc(wc->count, sizeof(cbm_node_t));
    for (int ci = 0; ci < wc->count; ci++) {
        (*aggs)[found].mins[ci] = CYP_DBL_MAX;
        (*aggs)[found].maxs[ci] = -CYP_DBL_MAX;
    }
    for (int ci = 0; ci < wc->count; ci++) {
        if (is_aggregate_func(wc->items[ci].func)) {
            (*aggs)[found].group_vals[ci] = heap_strdup("0");
            continue;
        }
        char vbuf[CBM_SZ_512];
        const char *v = project_item(b, &wc->items[ci], vbuf, sizeof(vbuf));
        (*aggs)[found].group_vals[ci] = heap_strdup(v);
        const cbm_node_t *gn = with_item_carried_node(&wc->items[ci], b);
        if (gn && (*aggs)[found].group_nodes) {
            node_deep_copy(&(*aggs)[found].group_nodes[ci], gn);
        }
    }
    return found;
}

/* Whether an aggregate's argument has a value in this row: `*`, a WITH
 * projection name, or a bound node/edge. An OPTIONAL MATCH with no match keeps
 * its row with the variable unbound, which COUNT must skip (#58). */
static bool binding_has_value(binding_t *b, const char *var, const char *prop) {
    if (!var || strcmp(var, "*") == 0) {
        return true;
    }
    char full[CBM_SZ_256];
    if (prop) {
        (void)snprintf(full, sizeof(full), "%s.%s", var, prop);
    } else {
        (void)snprintf(full, sizeof(full), "%s", var);
    }
    for (int i = 0; i < b->var_count; i++) {
        if (strcmp(b->var_names[i], full) == 0) {
            return true;
        }
    }
    return binding_get_edge(b, var) != NULL || binding_get(b, var) != NULL;
}

/* COUNT(x) over a row where x is unbound: skipped, so it counts 0, not 1. */
static bool count_skips_row(const cbm_return_item_t *item, binding_t *b) {
    return strcmp(item->func, "COUNT") == 0 &&
           !binding_has_value(b, item->variable, item->property);
}

/* Accumulate aggregation values for a binding */
static void with_agg_accumulate(with_agg_t *agg, cbm_return_clause_t *wc, binding_t *b) {
    for (int ci = 0; ci < wc->count; ci++) {
        if (!is_aggregate_func(wc->items[ci].func) || count_skips_row(&wc->items[ci], b)) {
            continue;
        }
        agg->counts[ci]++;
        const char *raw = binding_get_virtual(b, wc->items[ci].variable, wc->items[ci].property);
        if (wc->items[ci].distinct && strcmp(wc->items[ci].func, "COUNT") == 0) {
            distinct_list_add(&agg->distinct_lists[ci], &agg->distinct_n[ci],
                              &agg->distinct_sets[ci], raw);
        }
        double dv = strtod(raw, NULL);
        agg->sums[ci] += dv;
        if (dv < agg->mins[ci]) {
            agg->mins[ci] = dv;
        }
        if (dv > agg->maxs[ci]) {
            agg->maxs[ci] = dv;
        }
    }
}

/* Format a WITH aggregation value into buf */
static void with_agg_format(const char *func, with_agg_t *agg, int ci, char *buf, size_t buf_sz) {
    if (strcmp(func, "SUM") == 0) {
        snprintf(buf, buf_sz, "%.10g", agg->sums[ci]);
    } else if (strcmp(func, "AVG") == 0) {
        snprintf(buf, buf_sz, "%.10g", agg->counts[ci] > 0 ? agg->sums[ci] / agg->counts[ci] : 0.0);
    } else if (strcmp(func, "MIN") == 0) {
        snprintf(buf, buf_sz, "%.10g", agg->mins[ci]);
    } else if (strcmp(func, "MAX") == 0) {
        snprintf(buf, buf_sz, "%.10g", agg->maxs[ci]);
    } else {
        snprintf(buf, buf_sz, "%d", agg->counts[ci]);
    }
}

/* Add a scalar virtual variable binding for one WITH item. The value lives in
 * .name; the owned alias string (var_names points at it) lives in .project,
 * which no property accessor exposes. It used to live in .qualified_name, so
 * alias.qualified_name / keys(alias) returned the variable name as data (#57). */
static void with_add_vbinding_var(binding_t *vb, const char *alias, const char *val) {
    cbm_node_t vn = {.name = heap_strdup(val), .project = heap_strdup(alias)};
    if (vb->var_count < CYP_BUF_16) {
        vb->var_names[vb->var_count] = vn.project;
        vb->var_nodes[vb->var_count] = vn;
        vb->var_count++;
    }
}

/* Free with_agg_t array */
static void with_agg_free(with_agg_t *aggs, int agg_cnt, int item_count) {
    for (int a = 0; a < agg_cnt; a++) {
        for (int ci = 0; ci < item_count; ci++) {
            safe_str_free(&aggs[a].group_vals[ci]);
            if (aggs[a].distinct_sets && aggs[a].distinct_sets[ci]) {
                cbm_ht_free(aggs[a].distinct_sets[ci]);
            }
            if (aggs[a].distinct_lists && aggs[a].distinct_lists[ci]) {
                for (int j = 0; j < aggs[a].distinct_n[ci]; j++) {
                    free(aggs[a].distinct_lists[ci][j]);
                }
                free(aggs[a].distinct_lists[ci]);
            }
        }
        free(aggs[a].group_vals);
        free(aggs[a].sums);
        free(aggs[a].counts);
        free(aggs[a].mins);
        free(aggs[a].maxs);
        free(aggs[a].distinct_lists);
        free(aggs[a].distinct_n);
        free(aggs[a].distinct_sets);
        if (aggs[a].group_nodes) {
            for (int ci = 0; ci < item_count; ci++) {
                node_fields_free(&aggs[a].group_nodes[ci]);
            }
        }
        free(aggs[a].group_nodes);
    }
    free(aggs);
}

/* Execute WITH aggregation path */
static void execute_with_aggregate(cbm_return_clause_t *wc, binding_t *bindings, int bind_count,
                                   binding_t **vbindings, int *vcount) {
    int agg_cap = CBM_SZ_256;
    with_agg_t *aggs = calloc(agg_cap, sizeof(with_agg_t));
    int agg_cnt = 0;
    cypher_key_index_t groups = {0};

    for (int bi = 0; bi < bind_count; bi++) {
        char key[CBM_SZ_1K] = "";
        with_agg_build_key(wc, &bindings[bi], key, sizeof(key));
        int found =
            with_agg_find_or_create(&aggs, &agg_cnt, &agg_cap, &groups, wc, &bindings[bi], key);
        with_agg_accumulate(&aggs[found], wc, &bindings[bi]);
    }
    key_index_free(&groups);

    *vbindings = safe_realloc(*vbindings, (agg_cnt + SKIP_ONE) * sizeof(binding_t));
    if (!*vbindings) {
        with_agg_free(aggs, agg_cnt, wc->count);
        return;
    }
    for (int a = 0; a < agg_cnt; a++) {
        binding_t vb = {0};
        /* Carry the store so node_prop can compute a carried node's
         * in_degree/out_degree on the projected virtual binding. */
        vb.store = (bind_count > 0) ? bindings[0].store : NULL;
        for (int ci = 0; ci < wc->count; ci++) {
            char name_buf[CBM_SZ_256];
            const char *alias = resolve_item_alias(&wc->items[ci], name_buf, sizeof(name_buf));
            if (is_aggregate_func(wc->items[ci].func)) {
                char vbuf[CBM_SZ_64];
                if (wc->items[ci].distinct && strcmp(wc->items[ci].func, "COUNT") == 0) {
                    snprintf(vbuf, sizeof(vbuf), "%d", aggs[a].distinct_n[ci]); /* #239 */
                } else {
                    with_agg_format(wc->items[ci].func, &aggs[a], ci, vbuf, sizeof(vbuf));
                }
                with_add_vbinding_var(&vb, alias, vbuf);
            } else if (aggs[a].group_nodes && aggs[a].group_nodes[ci].id > 0) {
                binding_set(&vb, with_item_node_alias(&wc->items[ci]), &aggs[a].group_nodes[ci]);
            } else {
                with_add_vbinding_var(&vb, alias, aggs[a].group_vals[ci]);
            }
        }
        (*vbindings)[(*vcount)++] = vb;
    }
    with_agg_free(aggs, agg_cnt, wc->count);
}

/* Execute WITH simple (non-aggregate) projection */
static void execute_with_simple(cbm_return_clause_t *wc, binding_t *bindings, int bind_count,
                                binding_t *vbindings, int *vcount) {
    for (int bi = 0; bi < bind_count; bi++) {
        binding_t vb = {0};
        vb.store = bindings[bi].store; /* so node_prop can compute degrees on the projection */
        for (int ci = 0; ci < wc->count; ci++) {
            char name_buf[CBM_SZ_256];
            const char *alias = resolve_item_alias(&wc->items[ci], name_buf, sizeof(name_buf));
            const cbm_node_t *carried = with_item_carried_node(&wc->items[ci], &bindings[bi]);
            if (carried) {
                binding_set(&vb, with_item_node_alias(&wc->items[ci]), carried);
                continue;
            }
            char func_buf[CBM_SZ_512];
            const char *val =
                project_item(&bindings[bi], &wc->items[ci], func_buf, sizeof(func_buf));
            with_add_vbinding_var(&vb, alias, val);
        }
        vbindings[(*vcount)++] = vb;
    }
}

/* Apply post-WITH WHERE filter */
static void filter_bindings_where(const cbm_where_clause_t *where, binding_t *vbindings,
                                  int *vcount) {
    int kept = 0;
    for (int i = 0; i < *vcount; i++) {
        if (eval_where(where, &vbindings[i])) {
            if (kept != i) {
                vbindings[kept] = vbindings[i];
            }
            kept++;
        } else {
            binding_free(&vbindings[i]);
        }
    }
    *vcount = kept;
}

/* One WITH output item's value on a projected vbinding, looked up by the
 * item's alias; used to detect duplicate rows for WITH DISTINCT (#238). */
static const char *with_item_value(const cbm_return_clause_t *wc, binding_t *b, int ci) {
    char name_buf[CBM_SZ_256];
    const char *alias = resolve_item_alias(&wc->items[ci], name_buf, sizeof(name_buf));
    const char *v = binding_get_virtual(b, alias, NULL);
    return v ? v : "";
}

/* Whether vbinding i's value tuple equals one of the first `kept` tuples. */
static bool with_row_seen_before(const cbm_return_clause_t *wc, binding_t *vbindings, int i,
                                 int kept) {
    for (int j = 0; j < kept; j++) {
        bool same = true;
        for (int ci = 0; ci < wc->count && same; ci++) {
            same = strcmp(with_item_value(wc, &vbindings[i], ci),
                          with_item_value(wc, &vbindings[j], ci)) == 0;
        }
        if (same) {
            return true;
        }
    }
    return false;
}

/* Apply WITH DISTINCT: drop projected rows whose value tuple duplicates an
 * earlier one, keeping first occurrence (#238 — previously silently ignored). */
static void with_apply_distinct(cbm_return_clause_t *wc, binding_t *vbindings, int *vcount) {
    const char **vals = malloc((size_t)(wc->count > 0 ? wc->count : SKIP_ONE) * sizeof(char *));
    cypher_key_index_t seen = {.failed = vals == NULL};
    char *key_buf = NULL;
    size_t key_cap = 0;
    int kept = 0;
    for (int i = 0; i < *vcount; i++) {
        const char *key = NULL;
        if (!seen.failed) {
            for (int ci = 0; ci < wc->count; ci++) {
                vals[ci] = with_item_value(wc, &vbindings[i], ci);
            }
            key = row_key(vals, wc->count, &key_buf, &key_cap);
            seen.failed = !key;
        }
        int found = key_index_find(&seen, key);
        bool dup = found == KEY_INDEX_UNKNOWN ? with_row_seen_before(wc, vbindings, i, kept)
                                              : found >= 0;
        if (dup) {
            binding_free(&vbindings[i]);
        } else {
            key_index_add(&seen, key, kept);
            if (kept != i) {
                vbindings[kept] = vbindings[i];
            }
            kept++;
        }
    }
    *vcount = kept;
    key_index_free(&seen);
    free(key_buf);
    free(vals);
}

static void execute_with_clause(cbm_query_t *q, binding_t **bindings_ptr, int *bind_count_ptr) {
    cbm_return_clause_t *wc = q->with_clause;
    if (!wc) {
        return;
    }
    binding_t *bindings = *bindings_ptr;
    int bind_count = *bind_count_ptr;

    binding_t *vbindings = malloc((bind_count + SKIP_ONE) * sizeof(binding_t));
    int vcount = 0;

    bool has_agg = false;
    for (int i = 0; i < wc->count; i++) {
        if (is_aggregate_func(wc->items[i].func)) {
            has_agg = true;
            break;
        }
    }

    if (has_agg) {
        execute_with_aggregate(wc, bindings, bind_count, &vbindings, &vcount);
    } else {
        execute_with_simple(wc, bindings, bind_count, vbindings, &vcount);
    }

    /* WITH DISTINCT: dedup projected rows (no-op for aggregation, which already
     * collapses to one row per group). */
    if (wc->distinct) {
        with_apply_distinct(wc, vbindings, &vcount);
    }

    with_sort_skip_limit(wc, vbindings, &vcount);

    for (int bi = 0; bi < bind_count; bi++) {
        binding_free(&bindings[bi]);
    }
    free(bindings);

    if (q->post_with_where) {
        filter_bindings_where(q->post_with_where, vbindings, &vcount);
    }

    *bindings_ptr = vbindings;
    *bind_count_ptr = vcount;
}

/* ── Execute a single query (no UNION recursion) ──────────────── */

/* Project RETURN * — all bound variable properties */
/* Collect all variable names from query patterns */
/* Has this variable already been collected? A query may name the same variable
 * in more than one pattern, and RETURN * must give it one set of columns. */
static bool star_var_seen(const char **vars, int vc, const char *name) {
    for (int i = 0; i < vc; i++) {
        if (strcmp(vars[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Collect the variables a RETURN * projects, in the order the query names them
 * and with no repeats. Without the repeat check, `MATCH (f) OPTIONAL MATCH
 * (f)-[:CALLS]->(g)` names f in two patterns and f gets its four columns
 * twice. */
static int collect_pattern_vars(cbm_query_t *q, const char **vars, int max_vars) {
    int vc = 0;
    for (int pi = 0; pi < q->pattern_count; pi++) {
        for (int ni = 0; ni < q->patterns[pi].node_count && vc < max_vars; ni++) {
            const char *var = q->patterns[pi].nodes[ni].variable;
            if (var && !star_var_seen(vars, vc, var)) {
                vars[vc++] = var;
            }
        }
        for (int ri = 0; ri < q->patterns[pi].rel_count && vc < max_vars; ri++) {
            const char *var = q->patterns[pi].rels[ri].variable;
            if (var && !star_var_seen(vars, vc, var)) {
                vars[vc++] = var;
            }
        }
    }
    return vc;
}

/* Build star-projection columns: var.name, var.qualified_name, var.label, var.file_path */
static void build_star_columns(result_builder_t *rb, const char **vars, int vc) {
    int col_n = vc * CYP_NODE_COLS;
    const char *col_names[CBM_SZ_128];
    for (int v = 0; v < vc; v++) {
        char buf[CBM_SZ_128];
        snprintf(buf, sizeof(buf), "%s.name", vars[v]);
        col_names[(size_t)v * CYP_NODE_COLS] = heap_strdup(buf);
        snprintf(buf, sizeof(buf), "%s.qualified_name", vars[v]);
        col_names[((size_t)v * CYP_NODE_COLS) + SKIP_ONE] = heap_strdup(buf);
        snprintf(buf, sizeof(buf), "%s.label", vars[v]);
        col_names[((size_t)v * CYP_NODE_COLS) + PAIR_LEN] = heap_strdup(buf);
        snprintf(buf, sizeof(buf), "%s.file_path", vars[v]);
        col_names[((size_t)v * CYP_NODE_COLS) + CYP_TRIPLE] = heap_strdup(buf);
    }
    rb_set_columns(rb, col_names, col_n);
    for (int i = 0; i < col_n; i++) {
        safe_str_free(&col_names[i]);
    }
}

/* Project one variable's 4 columns for RETURN * */
static void project_star_var(binding_t *b, const char *var, const char **vals) {
    cbm_edge_t *edge = binding_get_edge(b, var);
    if (edge) {
        vals[0] = edge_prop(edge, "type");
        vals[SKIP_ONE] = "";
        vals[PAIR_LEN] = "";
        vals[CYP_TRIPLE] = "";
        return;
    }
    cbm_node_t *n = binding_get(b, var);
    vals[0] = n && n->name ? n->name : "";
    vals[SKIP_ONE] = n && n->qualified_name ? n->qualified_name : "";
    vals[PAIR_LEN] = n && n->label ? n->label : "";
    vals[CYP_TRIPLE] = n && n->file_path ? n->file_path : "";
}

/* Project one binding row for RETURN * */
static void project_star_row(binding_t *b, const char **vars, int vc, const char **vals) {
    for (int v = 0; v < vc; v++) {
        project_star_var(b, vars[v], vals + ((size_t)v * CYP_NODE_COLS));
    }
}

/* RETURN * after a WITH.
 *
 * The pattern's variables are out of scope by this point — the WITH replaced
 * them with the names it made. Each of those names holds one value, not a
 * node, so each is ONE column rather than the four a node variable gets.
 *
 * Reading the pattern here instead is the fault this function exists to avoid:
 * it named variables the bindings no longer hold, found nothing for every one
 * of them, and answered a full result of empty strings with no error. */
static void execute_return_star_after_with(cbm_query_t *q, binding_t *bindings, int bind_count,
                                           int max_rows, result_builder_t *rb) {
    cbm_return_clause_t *wc = q->with_clause;
    char name_bufs[CYP_MAX_VARS][CBM_SZ_128];
    const char *cols[CYP_MAX_VARS];
    /* parse_return_or_with refuses a WITH wider than CYP_MAX_VARS, so this
     * clamp cannot fire. It stays as the bound this function relies on. */
    int col_n = wc->count < CYP_MAX_VARS ? wc->count : CYP_MAX_VARS;
    for (int i = 0; i < col_n; i++) {
        cols[i] = resolve_item_alias(&wc->items[i], name_bufs[i], sizeof(name_bufs[i]));
    }
    rb_set_columns(rb, cols, col_n);
    rb_distinct_t seen = {.on = q->ret && q->ret->distinct};
    for (int bi = 0; bi < bind_count; bi++) {
        const char *vals[CYP_MAX_VARS];
        for (int i = 0; i < col_n; i++) {
            cbm_node_t *vn = binding_get(&bindings[bi], cols[i]);
            vals[i] = vn && vn->name ? vn->name : "";
        }
        if (!rb_add_row_capped(rb, &seen, vals, max_rows)) {
            break;
        }
    }
    rb_distinct_free(&seen);
}

static void execute_return_star(cbm_query_t *q, binding_t *bindings, int bind_count, int max_rows,
                                result_builder_t *rb) {
    if (q->with_clause) {
        execute_return_star_after_with(q, bindings, bind_count, max_rows, rb);
        return;
    }
    const char *vars[CBM_SZ_32];
    int vc = collect_pattern_vars(q, vars, CBM_SZ_32);
    build_star_columns(rb, vars, vc);
    rb_distinct_t seen = {.on = q->ret && q->ret->distinct};
    for (int bi = 0; bi < bind_count; bi++) {
        const char *vals[CBM_SZ_128];
        project_star_row(&bindings[bi], vars, vc, vals);
        if (!rb_add_row_capped(rb, &seen, vals, max_rows)) {
            break;
        }
    }
    rb_distinct_free(&seen);
}

/* Format an aggregate value into buf based on function name */
/* Format a COLLECT list as JSON array string */
static void format_collect_list(char **items, int item_count, char *buf, size_t buf_sz) {
    char cbuf[CBM_SZ_2K] = "[";
    int bl = SKIP_ONE;
    for (int i = 0; i < item_count; i++) {
        if (i > 0) {
            cbuf[bl++] = ',';
        }
        bl += snprintf(cbuf + bl, sizeof(cbuf) - (size_t)bl, "\"%s\"", items[i]);
        if (bl >= (int)sizeof(cbuf)) {
            bl = (int)sizeof(cbuf) - SKIP_ONE;
        }
    }
    if (bl < (int)sizeof(cbuf) - SKIP_ONE) {
        cbuf[bl++] = ']';
    }
    cbuf[bl] = '\0';
    snprintf(buf, buf_sz, "%s", cbuf);
}

static void format_agg_value(const char *func, int count, double sum, double min_val,
                             double max_val, char ***collect_lists, int *collect_counts, int ci,
                             char *buf, size_t buf_sz) {
    if (strcmp(func, "SUM") == 0) {
        snprintf(buf, buf_sz, "%.10g", sum);
    } else if (strcmp(func, "AVG") == 0) {
        snprintf(buf, buf_sz, "%.10g", count > 0 ? sum / count : 0.0);
    } else if (strcmp(func, "MIN") == 0) {
        snprintf(buf, buf_sz, "%.10g", min_val);
    } else if (strcmp(func, "MAX") == 0) {
        snprintf(buf, buf_sz, "%.10g", max_val);
    } else if (strcmp(func, "COLLECT") == 0) {
        format_collect_list(collect_lists[ci], collect_counts[ci], buf, buf_sz);
    } else {
        snprintf(buf, buf_sz, "%d", count);
    }
}

/* RETURN aggregation entry */
typedef struct {
    char group_key[CBM_SZ_1K];
    const char **group_vals;
    double *sums;
    int *counts;
    double *mins, *maxs;
    char ***collect_lists;
    int *collect_counts;
    CBMHashTable **distinct_sets; /* per-item hash over collect_lists for COUNT(DISTINCT) */
} ret_agg_entry_t;

/* Initialize a new RETURN aggregation group */
static void ret_agg_init_group(ret_agg_entry_t *entry, const char *key, int item_count,
                               const char **vals) {
    snprintf(entry->group_key, sizeof(entry->group_key), "%s", key);
    entry->group_vals = calloc(item_count, sizeof(const char *));
    entry->sums = calloc(item_count, sizeof(double));
    entry->counts = calloc(item_count, sizeof(int));
    entry->mins = malloc(item_count * sizeof(double));
    entry->maxs = malloc(item_count * sizeof(double));
    entry->collect_lists = calloc(item_count, sizeof(char **));
    entry->collect_counts = calloc(item_count, sizeof(int));
    entry->distinct_sets = calloc(item_count, sizeof(CBMHashTable *));
    for (int ci = 0; ci < item_count; ci++) {
        entry->mins[ci] = CYP_DBL_MAX;
        entry->maxs[ci] = -CYP_DBL_MAX;
        entry->group_vals[ci] = heap_strdup(vals[ci]);
    }
}

/* Accumulate a binding into RETURN aggregation */
static void ret_agg_accumulate(ret_agg_entry_t *entry, cbm_return_clause_t *ret, binding_t *b) {
    for (int ci = 0; ci < ret->count; ci++) {
        if (!is_aggregate_func(ret->items[ci].func) || count_skips_row(&ret->items[ci], b)) {
            continue;
        }
        entry->counts[ci]++;
        const char *raw = binding_get_virtual(b, ret->items[ci].variable, ret->items[ci].property);
        double dv = strtod(raw, NULL);
        entry->sums[ci] += dv;
        if (dv < entry->mins[ci]) {
            entry->mins[ci] = dv;
        }
        if (dv > entry->maxs[ci]) {
            entry->maxs[ci] = dv;
        }
        if (strcmp(ret->items[ci].func, "COLLECT") == 0) {
            int idx = entry->collect_counts[ci]++;
            entry->collect_lists[ci] =
                safe_realloc(entry->collect_lists[ci], (idx + SKIP_ONE) * sizeof(char *));
            entry->collect_lists[ci][idx] = heap_strdup(raw);
        } else if (ret->items[ci].distinct && strcmp(ret->items[ci].func, "COUNT") == 0) {
            /* COUNT(DISTINCT x): track unique values; emit the set size (#239). */
            distinct_list_add(&entry->collect_lists[ci], &entry->collect_counts[ci],
                              &entry->distinct_sets[ci], raw);
        }
    }
}

/* Free RETURN aggregation entries */
static void ret_agg_free(ret_agg_entry_t *aggs, int agg_count, int item_count) {
    for (int a = 0; a < agg_count; a++) {
        for (int ci = 0; ci < item_count; ci++) {
            safe_str_free(&aggs[a].group_vals[ci]);
            if (aggs[a].distinct_sets && aggs[a].distinct_sets[ci]) {
                cbm_ht_free(aggs[a].distinct_sets[ci]);
            }
            for (int j = 0; j < aggs[a].collect_counts[ci]; j++) {
                free(aggs[a].collect_lists[ci][j]);
            }
            free(aggs[a].collect_lists[ci]);
        }
        free(aggs[a].group_vals);
        free(aggs[a].sums);
        free(aggs[a].counts);
        free(aggs[a].mins);
        free(aggs[a].maxs);
        free(aggs[a].collect_lists);
        free(aggs[a].collect_counts);
        free(aggs[a].distinct_sets);
    }
    free(aggs);
}

/* Execute RETURN with aggregation */
/* Build group key and projected values for one binding */
static void ret_agg_build_key(cbm_return_clause_t *ret, binding_t *b, char *key, size_t key_sz,
                              const char **vals, char valbufs[][CBM_SZ_512]) {
    int klen = 0;
    for (int ci = 0; ci < ret->count; ci++) {
        if (is_aggregate_func(ret->items[ci].func)) {
            vals[ci] = "0";
            continue;
        }
        /* project_item may return its own scratch (stable static or a per-column
         * buffer it copied into); persist the value in the caller-owned valbufs
         * so vals[] survives until ret_agg_init_group strdup's it. */
        const char *v = project_item(b, &ret->items[ci], valbufs[ci], CBM_SZ_512);
        if (v != valbufs[ci]) {
            snprintf(valbufs[ci], CBM_SZ_512, "%s", v ? v : "");
        }
        vals[ci] = valbufs[ci];
        klen += snprintf(key + klen, key_sz - (size_t)klen, "%s|", vals[ci]);
        if (klen >= (int)key_sz) {
            klen = (int)key_sz - SKIP_ONE;
        }
    }
}

/* Emit one aggregated row into the result builder */
static void ret_agg_emit_row(cbm_return_clause_t *ret, ret_agg_entry_t *agg, result_builder_t *rb) {
    const char *row[CBM_SZ_32];
    char bufs[CBM_SZ_32][CBM_SZ_64];
    for (int ci = 0; ci < ret->count; ci++) {
        if (!is_aggregate_func(ret->items[ci].func)) {
            row[ci] = agg->group_vals[ci];
            continue;
        }
        if (ret->items[ci].distinct && strcmp(ret->items[ci].func, "COUNT") == 0) {
            /* COUNT(DISTINCT x) — number of unique values accumulated (#239). */
            snprintf(bufs[ci], sizeof(bufs[ci]), "%d", agg->collect_counts[ci]);
            row[ci] = bufs[ci];
            continue;
        }
        format_agg_value(ret->items[ci].func, agg->counts[ci], agg->sums[ci], agg->mins[ci],
                         agg->maxs[ci], agg->collect_lists, agg->collect_counts, ci, bufs[ci],
                         sizeof(bufs[ci]));
        row[ci] = bufs[ci];
    }
    rb_add_row(rb, row);
}

static void execute_return_agg(cbm_return_clause_t *ret, binding_t *bindings, int bind_count,
                               result_builder_t *rb) {
    int agg_cap = CBM_SZ_256;
    ret_agg_entry_t *aggs = calloc(agg_cap, sizeof(ret_agg_entry_t));
    int agg_count = 0;
    cypher_key_index_t groups = {0};

    for (int bi = 0; bi < bind_count; bi++) {
        /* Grouping dominates a whole-graph GROUP BY. Abort if the
         * wall-clock budget is blown. */
        if ((bi & CYPHER_DEADLINE_CHECK_MASK) == 0 && cypher_deadline_exceeded()) {
            break;
        }
        char key[CBM_SZ_1K] = "";
        const char *vals[CBM_SZ_32];
        char valbufs[CBM_SZ_32][CBM_SZ_512];
        ret_agg_build_key(ret, &bindings[bi], key, sizeof(key), vals, valbufs);

        int found = key_index_find(&groups, key);
        for (int a = 0; found == KEY_INDEX_UNKNOWN && a < agg_count; a++) {
            if (strcmp(aggs[a].group_key, key) == 0) {
                found = a;
            }
        }
        if (found < 0) {
            if (agg_count >= agg_cap) {
                agg_cap *= PAIR_LEN;
                aggs = safe_realloc(aggs, agg_cap * sizeof(ret_agg_entry_t));
            }
            found = agg_count++;
            ret_agg_init_group(&aggs[found], key, ret->count, vals);
            key_index_add(&groups, aggs[found].group_key, found);
        }
        ret_agg_accumulate(&aggs[found], ret, &bindings[bi]);
    }
    key_index_free(&groups);

    for (int a = 0; a < agg_count; a++) {
        ret_agg_emit_row(ret, &aggs[a], rb);
    }
    ret_agg_free(aggs, agg_count, ret->count);
}

/* Build RETURN column names from items */
static void build_return_columns(result_builder_t *rb, cbm_return_clause_t *ret) {
    const char *col_names[CBM_SZ_32];
    for (int i = 0; i < ret->count && i < CBM_SZ_32; i++) {
        cbm_return_item_t *item = &ret->items[i];
        if (item->alias) {
            col_names[i] = item->alias;
        } else if (item->func) {
            char buf[CBM_SZ_128];
            if (item->has_index) {
                snprintf(buf, sizeof(buf), "%s(%s)[%d]", item->func, item->variable, item->index);
            } else {
                snprintf(buf, sizeof(buf), "%s(%s)", item->func, item->variable);
            }
            col_names[i] = heap_strdup(buf);
        } else if (item->kase) {
            col_names[i] = "CASE";
        } else if (item->property) {
            char buf[CBM_SZ_128];
            snprintf(buf, sizeof(buf), "%s.%s", item->variable, item->property);
            col_names[i] = heap_strdup(buf);
        } else {
            col_names[i] = item->variable;
        }
    }
    rb_set_columns(rb, col_names, ret->count);
    for (int i = 0; i < ret->count && i < CBM_SZ_32; i++) {
        cbm_return_item_t *item = &ret->items[i];
        if (!item->alias && (item->func || (!item->kase && item->property))) {
            safe_str_free(&col_names[i]);
        }
    }
}

/* Execute simple (non-aggregate) RETURN projection */
static void execute_return_simple(cbm_return_clause_t *ret, binding_t *bindings, int bind_count,
                                  int max_rows, result_builder_t *rb) {
    int proj_cap = max_rows;
    if (ret->limit > 0 && ret->order_key_count == 0 && ret->skip <= 0) {
        proj_cap = ret->limit; /* with DISTINCT, the cap counts distinct rows */
    }
    rb_distinct_t seen = {.on = ret->distinct};
    for (int bi = 0; bi < bind_count; bi++) {
        const char *vals[CBM_SZ_32];
        char func_bufs[CBM_SZ_32][CBM_SZ_512];
        for (int ci = 0; ci < ret->count; ci++) {
            vals[ci] =
                project_item(&bindings[bi], &ret->items[ci], func_bufs[ci], sizeof(func_bufs[ci]));
        }
        if (!rb_add_row_capped(rb, &seen, vals, proj_cap)) {
            break;
        }
    }
    rb_distinct_free(&seen);
}

/* Build default 3-column headers (name, qualified_name, label) per variable */
static void build_default_columns(result_builder_t *rb, const char **vars, int vc) {
    int col_n = vc * CYP_EDGE_COLS;
    const char *col_names[CYP_COL_BUF];
    for (int v = 0; v < vc; v++) {
        char buf[CBM_SZ_128];
        snprintf(buf, sizeof(buf), "%s.name", vars[v]);
        col_names[(size_t)v * CYP_EDGE_COLS] = heap_strdup(buf);
        snprintf(buf, sizeof(buf), "%s.qualified_name", vars[v]);
        col_names[((size_t)v * CYP_EDGE_COLS) + SKIP_ONE] = heap_strdup(buf);
        snprintf(buf, sizeof(buf), "%s.label", vars[v]);
        col_names[((size_t)v * CYP_EDGE_COLS) + PAIR_LEN] = heap_strdup(buf);
    }
    rb_set_columns(rb, col_names, col_n);
    for (int i = 0; i < col_n; i++) {
        safe_str_free(&col_names[i]);
    }
}

/* Default projection when no RETURN clause */
static void execute_default_projection(cbm_pattern_t *pat0, binding_t *bindings, int bind_count,
                                       int max_rows, result_builder_t *rb) {
    const char *vars[CYP_MAX_VARS];
    int vc = 0;
    for (int ni = 0; ni < pat0->node_count && vc < CYP_MAX_VARS; ni++) {
        if (pat0->nodes[ni].variable) {
            vars[vc++] = pat0->nodes[ni].variable;
        }
    }
    build_default_columns(rb, vars, vc);
    for (int bi = 0; bi < bind_count && rb->row_count < max_rows; bi++) {
        const char *vals[CYP_COL_BUF];
        for (int v = 0; v < vc; v++) {
            cbm_node_t *n = binding_get(&bindings[bi], vars[v]);
            vals[(size_t)v * CYP_EDGE_COLS] = n && n->name ? n->name : "";
            vals[((size_t)v * CYP_EDGE_COLS) + SKIP_ONE] =
                n && n->qualified_name ? n->qualified_name : "";
            vals[((size_t)v * CYP_EDGE_COLS) + PAIR_LEN] = n && n->label ? n->label : "";
        }
        rb_add_row(rb, vals);
    }
}

/* Worst-case binding_t slot count for a node cross-join: one row per
 * (existing binding x extra node), plus — for an OPTIONAL join with no extra
 * nodes — one fallback row per existing binding, plus a trailing sentinel slot.
 *
 * The plain-int product bind_count * extra_count overflows to a negative/garbage
 * malloc size on large graphs, so the count is computed in size_t and rejected
 * if it would not fit the int binding counter or would overflow the size_t byte
 * size. Returns 0 and writes *out_n on success, CBM_NOT_FOUND on overflow.
 * Non-static so the arithmetic boundary can be unit-tested directly (upstream
 * b07dbe67). */
int cbm_cypher_cross_join_alloc(int bind_count, int extra_count, bool opt, size_t *out_n) {
    size_t per_binding = extra_count > 0 ? (size_t)extra_count : (opt ? (size_t)SKIP_ONE : 0U);
    size_t rows = (size_t)bind_count * per_binding;
    if (rows > (size_t)INT_MAX) {
        return CBM_NOT_FOUND; /* new_count would not fit the int binding counter */
    }
    size_t n = rows + SKIP_ONE;
    if (n > SIZE_MAX / sizeof(binding_t)) {
        return CBM_NOT_FOUND; /* alloc_n * sizeof(binding_t) would overflow size_t */
    }
    *out_n = n;
    return 0;
}

/* Cross-join node-only pattern into existing bindings. Returns 0 on success,
 * CBM_NOT_FOUND when the allocation is refused (overflow) or fails (OOM); the
 * caller propagates that as a query error rather than silently skipping the
 * MATCH, which would return a wrong (short) result. */
static int cross_join_nodes(binding_t **bindings, int *bind_count, cbm_node_t *extra_nodes,
                            int extra_count, const char *nvar, bool opt) {
    size_t alloc_n = 0;
    if (cbm_cypher_cross_join_alloc(*bind_count, extra_count, opt, &alloc_n) != 0) {
        return CBM_NOT_FOUND; /* product overflows int/size_t: fail the query */
    }
    binding_t *new_bindings = malloc(alloc_n * sizeof(binding_t));
    if (!new_bindings) {
        return CBM_NOT_FOUND; /* OOM: propagate, don't silently skip the MATCH */
    }
    size_t new_count = 0;
    bool expired = false;
    for (int bi = 0; bi < *bind_count && !expired; bi++) {
        for (int ni = 0; ni < extra_count && new_count < alloc_n; ni++) {
            /* Stop producing rows once the budget is spent; the caller reports
             * the timeout instead of the partial join. */
            if ((new_count & CYPHER_DEADLINE_CHECK_MASK) == 0 &&
                cypher_deadline_exceeded_at("cross_join")) {
                expired = true;
                break;
            }
            binding_t nb = {0};
            binding_copy(&nb, &(*bindings)[bi]);
            binding_set(&nb, nvar, &extra_nodes[ni]);
            new_bindings[new_count++] = nb;
        }
        if (opt && extra_count == 0 && new_count < alloc_n) {
            binding_t nb = {0};
            binding_copy(&nb, &(*bindings)[bi]);
            new_bindings[new_count++] = nb;
        }
    }
    for (int bi = 0; bi < *bind_count; bi++) {
        binding_free(&(*bindings)[bi]);
    }
    free(*bindings);
    *bindings = new_bindings;
    *bind_count = (int)new_count; /* new_count <= INT_MAX, guaranteed above */
    return 0;
}

/* Cross-join pattern-with-rels into existing bindings */
static void cross_join_with_rels(cbm_store_t *store, cbm_pattern_t *patn, binding_t **bindings,
                                 int *bind_count, cbm_node_t *extra_nodes, int extra_count,
                                 const char *nvar, bool opt) {
    /* Each pair's expansion is uncapped, so the output grows with
     * binding_out_append instead of a precomputed size; a precomputed
     * bind_count * extra_count product also overflowed int on large graphs
     * (#627). */
    int new_cap = *bind_count > 0 && extra_count > 0 ? *bind_count : SKIP_ONE;
    binding_t *new_bindings = malloc((size_t)new_cap * sizeof(binding_t));
    if (!new_bindings) {
        return; /* OOM: leave existing bindings untouched rather than corrupt */
    }
    int new_count = 0;
    bool expired = false;
    for (int bi = 0; bi < *bind_count && !expired; bi++) {
        for (int ni = 0; ni < extra_count; ni++) {
            /* Every pair runs a relationship expansion, so check each one. */
            if (cypher_deadline_exceeded_at("cross_join_rels")) {
                expired = true;
                break;
            }
            binding_t nb = {0};
            binding_copy(&nb, &(*bindings)[bi]);
            binding_set(&nb, nvar, &extra_nodes[ni]);
            binding_t *tmp = malloc(PAIR_LEN * sizeof(binding_t));
            tmp[0] = nb;
            int tc = SKIP_ONE;
            int tcap = SKIP_ONE;
            const char *tv = nvar;
            expand_pattern_rels(store, patn, &tmp, &tc, &tcap, &tv, opt);
            for (int ti = 0; ti < tc; ti++) {
                (void)binding_out_append(&new_bindings, &new_count, &new_cap, &tmp[ti]);
            }
            free(tmp);
        }
        if (opt && extra_count == 0) {
            binding_t nb = {0};
            binding_copy(&nb, &(*bindings)[bi]);
            (void)binding_out_append(&new_bindings, &new_count, &new_cap, &nb);
        }
    }
    for (int bi = 0; bi < *bind_count; bi++) {
        binding_free(&(*bindings)[bi]);
    }
    free(*bindings);
    *bindings = new_bindings;
    *bind_count = new_count;
}

/* Drive a single-relationship additional pattern from its ALREADY-BOUND
 * terminal node, binding the unbound START var to the edge's other endpoint.
 *
 * Handles `OPTIONAL MATCH (c)-[:CALLS]->(f)` where `f` is bound from an earlier
 * MATCH and `c` is new: scanning every node for `c` and cross-joining (a) risks
 * an int-overflow OOB write on large graphs and (b) leaves `c` bound to an
 * arbitrary node so a later `WHERE c IS NULL` wrongly drops every row (#627).
 * Instead we scan only the bound terminal's edges and bind `c` to real
 * neighbours; with OPTIONAL we keep the row with `c` unbound when there are
 * none — the correct dead-code semantics. */
static void expand_from_bound_terminal(cbm_store_t *store, cbm_pattern_t *patn,
                                       binding_t **bindings, int *bind_count, const char *start_var,
                                       bool opt) {
    cbm_rel_pattern_t *rel = &patn->rels[0];
    const cbm_node_pattern_t *start_node = &patn->nodes[0];
    /* The relationship is written start-[r]->terminal. To enumerate the start
     * nodes reachable from the bound terminal we invert the stored direction. */
    bool rel_inbound = rel->direction && strcmp(rel->direction, "inbound") == 0;
    bool scan_targets =
        !rel_inbound; /* (start)->(term): start = edge source = scan term's inbound */

    int new_cap = *bind_count > 0 ? *bind_count : CYP_GROWTH_10;
    binding_t *new_bindings = malloc((size_t)new_cap * sizeof(binding_t));
    if (!new_bindings) {
        return;
    }
    int new_count = 0;

    for (int bi = 0; bi < *bind_count; bi++) {
        binding_t *b = &(*bindings)[bi];
        cbm_node_t *term = binding_get(b, patn->nodes[1].variable ? patn->nodes[1].variable : "");
        int match_count = 0;
        if (term) {
            for (int ti = 0; ti < (rel->type_count > 0 ? rel->type_count : 1); ti++) {
                cbm_edge_t *edges = NULL;
                int edge_count = 0;
                if (rel->type_count > 0) {
                    if (scan_targets) {
                        cbm_store_find_edges_by_target_type(store, term->id, rel->types[ti], &edges,
                                                            &edge_count);
                    } else {
                        cbm_store_find_edges_by_source_type(store, term->id, rel->types[ti], &edges,
                                                            &edge_count);
                    }
                } else if (scan_targets) {
                    cbm_store_find_edges_by_target(store, term->id, &edges, &edge_count);
                } else {
                    cbm_store_find_edges_by_source(store, term->id, &edges, &edge_count);
                }
                for (int ei = 0; ei < edge_count; ei++) {
                    int64_t sid = scan_targets ? edges[ei].source_id : edges[ei].target_id;
                    cbm_node_t found = {0};
                    if (cbm_store_find_node_by_id(store, sid, &found) != CBM_STORE_OK) {
                        continue;
                    }
                    if (start_node->label && !label_alt_matches(found.label, start_node->label)) {
                        node_fields_free(&found);
                        continue;
                    }
                    binding_t nb = {0};
                    binding_copy(&nb, b);
                    binding_set(&nb, start_var, &found);
                    if (rel->variable) {
                        binding_set_edge(&nb, rel->variable, &edges[ei]);
                    }
                    node_fields_free(&found);
                    match_count++;
                    (void)binding_out_append(&new_bindings, &new_count, &new_cap, &nb);
                }
                cbm_store_free_edges(edges, edge_count);
            }
        }
        if (opt && match_count == 0) {
            /* No matching neighbour: keep the row with start_var left UNBOUND so
             * `WHERE <start> IS NULL` correctly identifies the no-edge case. */
            binding_t nb = {0};
            binding_copy(&nb, b);
            (void)binding_out_append(&new_bindings, &new_count, &new_cap, &nb);
        }
    }

    for (int bi = 0; bi < *bind_count; bi++) {
        binding_free(&(*bindings)[bi]);
    }
    free(*bindings);
    *bindings = new_bindings;
    *bind_count = new_count;
}

/* Expand additional MATCH patterns (pi >= 1) */
static int expand_additional_patterns(cbm_store_t *store, cbm_query_t *q, const char *project,
                                       binding_t **bindings, int *bind_count, int *bind_cap) {
    for (int pi = SKIP_ONE; pi < q->pattern_count; pi++) {
        cbm_pattern_t *patn = &q->patterns[pi];
        bool opt = q->pattern_optional[pi];
        const char *nvar = patn->nodes[0].variable ? patn->nodes[0].variable : "_n_extra";
        bool start_bound = *bind_count > 0 && binding_get(&(*bindings)[0], nvar) != NULL;

        if (start_bound && patn->rel_count > 0) {
            const char *tv = nvar;
            expand_pattern_rels(store, patn, bindings, bind_count, bind_cap, &tv, opt);
            continue;
        }

        /* Single-rel pattern whose START is unbound but whose TERMINAL is already
         * bound: drive from the bound terminal instead of scanning all nodes for
         * the start var (avoids the int-overflow OOB write and the c-IS-NULL
         * corruption of #627). */
        if (!start_bound && patn->rel_count == 1 && *bind_count > 0) {
            const char *term_var = patn->nodes[1].variable;
            bool term_bound = term_var && binding_get(&(*bindings)[0], term_var) != NULL;
            if (term_bound) {
                expand_from_bound_terminal(store, patn, bindings, bind_count, nvar, opt);
                continue;
            }
        }

        cbm_node_t *extra_nodes = NULL;
        int extra_count = 0;
        scan_pattern_nodes(store, project, &patn->nodes[0], &extra_nodes, &extra_count);
        int rc = 0;
        if (patn->rel_count == 0) {
            rc = cross_join_nodes(bindings, bind_count, extra_nodes, extra_count, nvar, opt);
        } else {
            cross_join_with_rels(store, patn, bindings, bind_count, extra_nodes, extra_count, nvar,
                                 opt);
        }
        cbm_store_free_nodes(extra_nodes, extra_count);
        if (rc != 0) {
            return rc; /* allocation refused/failed: propagate as a query error */
        }
    }
    return 0;
}

/* Project RETURN clause results */
static void execute_return_clause(cbm_query_t *q, cbm_return_clause_t *ret, binding_t *bindings,
                                  int bind_count, int max_rows, result_builder_t *rb) {
    bool has_agg = false;
    for (int i = 0; i < ret->count; i++) {
        if (is_aggregate_func(ret->items[i].func)) {
            has_agg = true;
            break;
        }
    }

    if (ret->star) {
        execute_return_star(q, bindings, bind_count, max_rows, rb);
    } else {
        build_return_columns(rb, ret);
        if (has_agg) {
            execute_return_agg(ret, bindings, bind_count, rb);
        } else {
            execute_return_simple(ret, bindings, bind_count, max_rows, rb);
        }
    }

    /* The simple and star projections deduplicate while projecting; only
     * aggregate rows still need the post-pass. */
    if (ret->distinct && has_agg) {
        rb_apply_distinct(rb);
    }
    rb_apply_order_by(rb, ret);
    rb_apply_skip_limit(rb, ret->skip, ret->limit >= 0 ? ret->limit : max_rows);
}

static bool var_is(const char *name, const char *var) {
    return !name || strcmp(name, var) == 0;
}

/* Does every condition in e reference only var? An unbound variable counts as
 * a match, so NOT over a condition on a later variable would drop every row
 * if the WHERE ran before that variable is bound. An operator chain is linked
 * along the right child (expr_chain_link), so the right child is followed in
 * a loop and only the left operand recurses. */
static bool expr_only_uses_var(const cbm_expr_t *e, const char *var) { // NOLINT(misc-no-recursion)
    while (e && e->type != EXPR_CONDITION) {
        if (e->left && !expr_only_uses_var(e->left, var)) {
            return false;
        }
        e = e->right; /* NULL for NOT, whose only operand is left */
    }
    if (!e) {
        return true;
    }
    const cbm_condition_t *c = &e->cond;
    if (!var_is(c->variable, var) || !var_is(c->value_variable, var)) {
        return false;
    }
    for (int i = 0; i < c->arg_count; i++) {
        if (!var_is(c->args[i].variable, var)) {
            return false;
        }
    }
    return true;
}

/* Early WHERE on the first scanned node: apply each top-level AND part that
 * references only that node; the late WHERE checks the full expression. The
 * AND chain is followed along its right child in a loop. */
static bool eval_early_where(const cbm_expr_t *e, binding_t *b, // NOLINT(misc-no-recursion)
                             const char *var) {
    while (e && e->type == EXPR_AND) {
        if (!eval_early_where(e->left, b, var)) {
            return false;
        }
        e = e->right;
    }
    return !expr_only_uses_var(e, var) || eval_expr(e, b);
}

static int execute_single(cbm_store_t *store, cbm_query_t *q, const char *project, int max_rows,
                          result_builder_t *rb) {
    cbm_pattern_t *pat0 = &q->patterns[0];

    /* Step 1: Scan initial nodes */
    cbm_node_t *scanned = NULL;
    int scan_count = 0;
    scan_pattern_nodes(store, project, &pat0->nodes[0], &scanned, &scan_count);

    /* Build initial bindings with early WHERE */
    int bind_cap = scan_count > max_rows ? scan_count : (max_rows > 0 ? max_rows : SKIP_ONE);
    binding_t *bindings = malloc((bind_cap + SKIP_ONE) * sizeof(binding_t));
    int bind_count = 0;
    const char *var_name = pat0->nodes[0].variable ? pat0->nodes[0].variable : CYP_ANON_HEAD_VAR;
    bool late_where = q->where && (pat0->rel_count > 0 || q->pattern_count > SKIP_ONE);

    for (int i = 0; i < scan_count && bind_count < bind_cap; i++) {
        if ((i & CYPHER_DEADLINE_CHECK_MASK) == 0 && cypher_deadline_exceeded()) {
            break;
        }
        binding_t b = {0};
        b.store = store;
        binding_set(&b, var_name, &scanned[i]);
        bool pass = !q->where ||
                    (late_where && q->where->root ? eval_early_where(q->where->root, &b, var_name)
                                                  : eval_where(q->where, &b));
        if (pass) {
            bindings[bind_count++] = b;
        } else {
            binding_free(&b);
        }
    }

    /* Step 2: Expand first pattern's relationships */
    expand_pattern_rels(store, pat0, &bindings, &bind_count, &bind_cap, &var_name,
                        q->pattern_optional[0]);

    /* Step 2b: Additional patterns */
    if (expand_additional_patterns(store, q, project, &bindings, &bind_count, &bind_cap) !=
        0) {
        for (int bi = 0; bi < bind_count; bi++) {
            binding_free(&bindings[bi]);
        }
        free(bindings);
        cbm_store_free_nodes(scanned, scan_count);
        return CBM_NOT_FOUND; /* cross-join allocation refused/failed */
    }

    /* Step 3: Late WHERE */
    if (late_where) {
        filter_bindings_where(q->where, bindings, &bind_count);
    }

    /* Step 3b: WITH clause */
    execute_with_clause(q, &bindings, &bind_count);

    /* Step 4: Project results */
    rb_init(rb);
    if (q->ret) {
        execute_return_clause(q, q->ret, bindings, bind_count, max_rows, rb);
    } else {
        execute_default_projection(pat0, bindings, bind_count, max_rows, rb);
    }

    for (int bi = 0; bi < bind_count; bi++) {
        binding_free(&bindings[bi]);
    }
    free(bindings);
    cbm_store_free_nodes(scanned, scan_count);
    return 0;
}

/* ── Main entry point ─────────────────────────────────────────── */

cbm_cypher_route_t cbm_cypher_classify_plan(const cbm_query_t *query,
                                            const char **reason) {
    if (!query || query->pattern_count <= 0 || !query->patterns) {
        if (reason) *reason = "missing logical plan";
        return CBM_CYPHER_ROUTE_UNSUPPORTED;
    }

    /* Native traversal is deliberately narrow: the graph supplies topology,
     * while predicates, aggregation and presentation remain with the existing
     * relational executor in cbm.zova. */
    const cbm_pattern_t *pattern = &query->patterns[0];
    if (query->pattern_count == 1 && pattern->rel_count > 0 && !query->where &&
        !query->with_clause && !query->union_next && !query->unwind_expr) {
        if (reason) *reason = "relationship topology supported by native graph";
        return CBM_CYPHER_ROUTE_NATIVE_GRAPH;
    }

    if (reason) *reason = "rich semantics execute in database";
    return CBM_CYPHER_ROUTE_IN_DATABASE_COMPAT;
}

/* ── Is every name a RETURN or WITH item uses actually in scope? ──
 *
 * An unbound variable resolves to NULL and renders as "" on purpose: an
 * OPTIONAL MATCH target that found no row has to project a blank rather than
 * drop the row. The cost of that convention is that a name the query NEVER
 * carried through looks exactly the same on screen. A typo, or a variable a
 * WITH dropped, then reads as "the graph holds no such data" instead of "your
 * query named something that is not there" (#1919). Same failure shape as
 * #373, and the same answer: say so out loud.
 *
 * The check runs on the parsed query, not on the run-time bindings, and that
 * is the whole trick. It asks whether the query DECLARED the name — a
 * different question from whether a row happened to bind it. So the OPTIONAL
 * MATCH convention above is untouched: a target that did not match is still
 * declared, still legal, and still projects "".
 */

/* Cap for one query's declared names. CYP_MAX_VARS is the binding cap for
 * nodes alone; a pattern also names edges, so this is the roomier of the two.
 * A query that overruns it skips the check rather than guessing — a wrong
 * refusal costs the caller a working query, which is worse than the silence
 * this guard removes. */
enum { CYP_SCOPE_MAX_NAMES = 32 };

static bool scope_holds(const char *const *names, int count, const char *want) {
    for (int i = 0; i < count; i++) {
        if (names[i] && strcmp(names[i], want) == 0) {
            return true;
        }
    }
    return false;
}

/* Every name the query's patterns declare, plus an UNWIND alias.
 * Answers -1 when there are more names than the cap holds. */
static int collect_declared_names(const cbm_query_t *q, const char **out, int cap) {
    int n = 0;
    for (int pi = 0; pi < q->pattern_count; pi++) {
        const cbm_pattern_t *pat = &q->patterns[pi];
        for (int ni = 0; ni < pat->node_count; ni++) {
            const char *var = pat->nodes[ni].variable;
            if (var && !scope_holds(out, n, var)) {
                if (n >= cap) {
                    return -1;
                }
                out[n++] = var;
            }
        }
        for (int ri = 0; ri < pat->rel_count; ri++) {
            const char *var = pat->rels[ri].variable;
            if (var && !scope_holds(out, n, var)) {
                if (n >= cap) {
                    return -1;
                }
                out[n++] = var;
            }
        }
    }
    if (q->unwind_alias && !scope_holds(out, n, q->unwind_alias)) {
        if (n >= cap) {
            return -1;
        }
        out[n++] = q->unwind_alias;
    }
    return n;
}

/* What a WITH leaves behind: its alias where it made one, and the plain
 * variable where it carried one through whole. `WITH f.name AS caller` leaves
 * `caller` and nothing else — `f` is gone, which is the case #1919 is about. */
static int collect_with_names(const cbm_return_clause_t *wc, const char **out, int cap) {
    int n = 0;
    for (int i = 0; i < wc->count; i++) {
        const cbm_return_item_t *item = &wc->items[i];
        const char *name = NULL;
        if (item->alias) {
            name = item->alias;
        } else if (item->variable && !item->property && !item->func) {
            name = item->variable;
        }
        if (name && !scope_holds(out, n, name)) {
            if (n >= cap) {
                return -1;
            }
            out[n++] = name;
        }
    }
    return n;
}

/* Build the message. It names the variable and the clause, because an error
 * that does not say WHICH name is wrong sends the reader back to guessing. */
static char *scope_error(const char *var, const char *clause, const char *why) {
    char buf[CBM_SZ_256];
    snprintf(buf, sizeof(buf), "variable '%s' is not in scope for %s — %s", var, clause, why);
    return heap_strdup(buf);
}

/* The variable this item really references, or NULL when it references none.
 * Two items carry a placeholder in that field rather than a name the query
 * declared: `count(*)` stores "*", and a CASE expression stores "CASE". Both
 * would read as an unknown variable, so neither is checkable here. */
static const char *scope_checkable_var(const cbm_return_item_t *item) {
    if (item->kase || !item->variable) {
        return NULL;
    }
    if (strcmp(item->variable, "*") == 0) {
        return NULL;
    }
    return item->variable;
}

/* Says which limit the query passed and how to get under it. An unnamed node
 * takes no slot — except the head of the first pattern, which the engine binds
 * either way — so dropping a name the query never uses is the cheap way out.
 * Splitting the MATCH is NOT — every pattern of one query shares one binding,
 * which is why the caller counts across all of them. Separate queries do work,
 * because each gets a binding of its own. */
static char *var_capacity_error(const char *kind, int limit) {
    char buf[CBM_SZ_256];
    snprintf(buf, sizeof(buf),
             "too many %s variables: a query can name at most %d — "
             "leave the name off the ones you do not use, or run separate queries",
             kind, limit);
    return heap_strdup(buf);
}

/* A binding holds a fixed number of variables: CYP_MAX_VARS node variables and
 * CYP_MAX_EDGE_VARS edge variables, both in plain arrays (see binding_t).
 * binding_set and binding_set_edge drop anything past those without a word, so
 * a query naming more variables than a binding holds cannot be answered — the
 * extra names bind to nothing and project as empty strings, which reads as
 * "the graph holds no such data" rather than "this query is too wide".
 *
 * Refuse such a query instead, before any row is touched. Bounding the input
 * here is also what stops collect_declared_names below overflowing its array:
 * the patterns contribute at most CYP_MAX_VARS + CYP_MAX_EDGE_VARS names, plus
 * one UNWIND alias, which is well inside CYP_SCOPE_MAX_NAMES.
 *
 * Counts DISTINCT variables across every pattern, because they all land in the
 * same binding: a multi-MATCH query shares one, and an OPTIONAL MATCH pattern
 * sits in this same array (q->pattern_optional marks which). A node variable
 * and an edge variable may share a name and each take a slot, because the
 * binding keeps the two in separate arrays. */
static char *check_pattern_var_capacity(const cbm_query_t *q) {
    /* Initialized because cppcheck cannot see that scope_holds reads only the
     * node_n / edge_n entries already written, and reports the first call as a
     * read of an uninitialized array. */
    const char *node_vars[CYP_MAX_VARS] = {NULL};
    const char *edge_vars[CYP_MAX_EDGE_VARS] = {NULL};
    int node_n = 0;
    int edge_n = 0;
    /* The head of the first pattern always takes a node slot, named or not:
     * execute_single binds it under CYP_ANON_HEAD_VAR when the query leaves it
     * unnamed. Count it first, or a query with an unnamed head and CYP_MAX_VARS
     * named nodes passes this check and still loses its last name in
     * binding_set — the exact silence this guard exists to remove. */
    if (q->pattern_count > 0 && q->patterns[0].node_count > 0 &&
        !q->patterns[0].nodes[0].variable) {
        node_vars[node_n++] = CYP_ANON_HEAD_VAR;
    }
    for (int pi = 0; pi < q->pattern_count; pi++) {
        const cbm_pattern_t *pat = &q->patterns[pi];
        for (int ni = 0; ni < pat->node_count; ni++) {
            const char *var = pat->nodes[ni].variable;
            if (!var || scope_holds(node_vars, node_n, var)) {
                continue;
            }
            if (node_n >= CYP_MAX_VARS) {
                return var_capacity_error("node", CYP_MAX_VARS);
            }
            node_vars[node_n++] = var;
        }
        for (int ri = 0; ri < pat->rel_count; ri++) {
            const char *var = pat->rels[ri].variable;
            if (!var || scope_holds(edge_vars, edge_n, var)) {
                continue;
            }
            if (edge_n >= CYP_MAX_EDGE_VARS) {
                return var_capacity_error("edge", CYP_MAX_EDGE_VARS);
            }
            edge_vars[edge_n++] = var;
        }
    }
    return NULL;
}

/* Answers NULL when the query is fine, or a heap message naming the first
 * variable that is not in scope. Checks one query; the caller walks a UNION. */
static char *check_projection_scope(const cbm_query_t *q) {
    /* Runs first, so the rest of this function can trust that the query names
     * no more variables than the arrays below can model. */
    char *capacity_err = check_pattern_var_capacity(q);
    if (capacity_err) {
        return capacity_err;
    }

    const char *declared[CYP_SCOPE_MAX_NAMES];
    int declared_n = collect_declared_names(q, declared, CYP_SCOPE_MAX_NAMES);
    if (declared_n < 0) {
        /* Unreachable while the capacity check above holds. Kept so the guard
         * still stands if either bound ever moves. Skipping the check was the
         * old behaviour, and it let an out-of-scope name through in silence. */
        return NULL;
    }

    /* A WITH still reads the pattern variables. */
    if (q->with_clause && !q->with_clause->star) {
        for (int i = 0; i < q->with_clause->count; i++) {
            const char *var = scope_checkable_var(&q->with_clause->items[i]);
            if (var && !scope_holds(declared, declared_n, var)) {
                return scope_error(var, "WITH", "no pattern in this query names it");
            }
        }
    }

    if (!q->ret || q->ret->star) {
        return NULL;
    }

    /* A RETURN after a WITH reads only what the WITH left behind. */
    const char *after_with[CYP_SCOPE_MAX_NAMES];
    const char *const *scope = declared;
    int scope_n = declared_n;
    if (q->with_clause) {
        scope_n = collect_with_names(q->with_clause, after_with, CYP_SCOPE_MAX_NAMES);
        if (scope_n < 0) {
            return NULL; /* unreachable: a WITH holds at most CYP_SCOPE_MAX_NAMES items */
        }
        scope = after_with;
    }

    for (int i = 0; i < q->ret->count; i++) {
        const char *var = scope_checkable_var(&q->ret->items[i]);
        if (var && !scope_holds(scope, scope_n, var)) {
            return scope_error(var, "RETURN",
                               q->with_clause ? "the WITH clause did not carry it through"
                                              : "no pattern in this query names it");
        }
    }
    return NULL;
}

int cbm_cypher_execute(cbm_store_t *store, const char *query, const char *project, int max_rows,
                       cbm_cypher_result_t *out) {
    memset(out, 0, sizeof(*out));
    g_cypher_depth_clamped = 0;
    g_cypher_trail_truncated = 0;
    cypher_deadline_arm(); /* start the wall-clock budget for this query */
    if (max_rows <= 0) {
        max_rows = CYPHER_RESULT_CEILING;
    }

    cbm_query_t *q = NULL;
    char *err = NULL;
    if (cbm_cypher_parse(query, &q, &err) < 0) {
        out->error = err;
        return CBM_NOT_FOUND;
    }

    for (const cbm_query_t *sq = q; sq; sq = sq->union_next) {
        char *scope_err = check_projection_scope(sq);
        if (scope_err) {
            cbm_query_free(q);
            out->error = scope_err;
            return CBM_NOT_FOUND;
        }
    }

    result_builder_t rb = {0};
    if (execute_single(store, q, project, max_rows, &rb) < 0) {
        rb_free(&rb);
        cbm_query_free(q);
        out->error = heap_strdup("query aborted: out of memory or an allocation limit was reached");
        return CBM_NOT_FOUND;
    }

    /* UNION chain */
    cbm_query_t *uq = q->union_next;
    while (uq) {
        result_builder_t rb2 = {0};
        if (execute_single(store, uq, project, max_rows, &rb2) < 0) {
            rb_free(&rb);
            rb_free(&rb2);
            cbm_query_free(q);
            out->error =
                heap_strdup("query aborted: out of memory or an allocation limit was reached");
            return CBM_NOT_FOUND;
        }
        /* Concatenate rows from rb2 into rb */
        for (int i = 0; i < rb2.row_count; i++) {
            rb_add_row(&rb, rb2.rows[i]);
        }
        rb_free(&rb2);

        uq = uq->union_next;
    }

    /* UNION (not ALL) deduplication */
    if (q->union_next && !q->union_all) {
        rb_apply_distinct(&rb);
    }

    /* Abort a runaway query that blew the wall-clock budget before it can
     * return a misleading partial result. Checks the clock itself, not only
     * the flag the hot loops set, so time spent in a phase without its own
     * check still fails the query. Checked before the row ceiling. */
    if (cypher_deadline_exceeded()) {
        rb_free(&rb);
        cbm_query_free(q);
        out->error =
            heap_strdup("query exceeded the execution time limit — narrow the pattern with a WHERE "
                        "filter, use a directed MATCH instead of an unbounded OPTIONAL MATCH, or "
                        "add LIMIT");
        return CBM_NOT_FOUND;
    }

    /* Check ceiling */
    if (rb.row_count >= CYPHER_RESULT_CEILING) {
        rb_free(&rb);
        cbm_query_free(q);
        out->error = heap_strdup("result exceeded 100k rows — use narrower filters or add LIMIT");
        return CBM_NOT_FOUND;
    }

    out->columns = rb.columns;
    out->col_count = rb.col_count;
    out->rows = rb.rows;
    out->row_count = rb.row_count;
    if (g_cypher_depth_clamped > 0 || g_cypher_trail_truncated) {
        char wbuf[CBM_SZ_256];
        if (g_cypher_depth_clamped > 0 && g_cypher_trail_truncated) {
            snprintf(wbuf, sizeof(wbuf),
                     "variable-length hop range clamped to the engine ceiling (%d) and "
                     "traversal budget was exhausted — results may be partial",
                     g_cypher_depth_clamped);
        } else if (g_cypher_depth_clamped > 0) {
            snprintf(wbuf, sizeof(wbuf),
                     "variable-length hop range clamped to the engine ceiling (%d) — an empty "
                     "result may mean \"clamped\", not \"no such path\"",
                     g_cypher_depth_clamped);
        } else {
            snprintf(wbuf, sizeof(wbuf),
                     "variable-length traversal budget was exhausted — results may be partial");
        }
        out->warning = heap_strdup(wbuf);
    }

    cbm_query_free(q);
    return 0;
}

void cbm_cypher_result_free(cbm_cypher_result_t *r) {
    if (!r) {
        return;
    }
    free(r->warning);
    r->warning = NULL;
    for (int i = 0; i < r->col_count; i++) {
        safe_str_free(&r->columns[i]);
    }
    free(r->columns);
    for (int i = 0; i < r->row_count; i++) {
        for (int j = 0; j < r->col_count; j++) {
            safe_str_free(&r->rows[i][j]);
        }
        free(r->rows[i]);
    }
    free(r->rows);
    free(r->error);
    memset(r, 0, sizeof(*r));
}
