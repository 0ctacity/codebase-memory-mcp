#include "cbm.h"
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "tree_sitter/api.h" // TSNode, ts_node_*
#include "foundation/constants.h"

#include <stdint.h> // uint32_t
#include <string.h>
#include <ctype.h>

// Is this an identifier-like node that represents a reference?
static bool is_reference_node(TSNode node, CBMLanguage lang) {
    const char *kind = ts_node_type(node);

    // Common identifier types across languages
    if (strcmp(kind, "identifier") == 0 || strcmp(kind, "simple_identifier") == 0 ||
        strcmp(kind, "type_identifier") == 0) {
        return true;
    }

    // Language-specific reference types
    switch (lang) {
    case CBM_LANG_GO:
        return strcmp(kind, "field_identifier") == 0 || strcmp(kind, "package_identifier") == 0;
    case CBM_LANG_PYTHON:
        return strcmp(kind, "attribute") == 0;
    case CBM_LANG_RUST:
        return strcmp(kind, "field_identifier") == 0 || strcmp(kind, "scoped_identifier") == 0;
    case CBM_LANG_HASKELL:
        return strcmp(kind, "variable") == 0 || strcmp(kind, "constructor") == 0;
    case CBM_LANG_OCAML:
        return strcmp(kind, "value_path") == 0 || strcmp(kind, "constructor_path") == 0;
    case CBM_LANG_ERLANG:
        return strcmp(kind, "atom") == 0 || strcmp(kind, "var") == 0;
    default:
        return false;
    }
}

// --- Callable-reference candidates (CALL_REFERENCE) ---
//
// A candidate is a usage whose exact occurrence a language LSP may prove to
// name one callable; the pipeline joins it to the LSP row by byte span. These
// helpers only decide syntax. Without a proving row nothing is promoted.

// Languages whose LSP proves bare identifiers passed directly as arguments.
static bool lang_marks_argument_values(CBMLanguage lang) {
    switch (lang) {
    case CBM_LANG_JAVASCRIPT:
    case CBM_LANG_TYPESCRIPT:
    case CBM_LANG_TSX:
    case CBM_LANG_GO:
    case CBM_LANG_PYTHON:
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA:
    case CBM_LANG_RUST:
    case CBM_LANG_CSHARP:
        return true;
    default:
        return false;
    }
}

static bool is_kind(TSNode node, const char *kind) {
    return !ts_node_is_null(node) && strcmp(ts_node_type(node), kind) == 0;
}

static bool is_field_value(TSNode parent, const char *field, uint32_t field_len, TSNode node) {
    TSNode value = ts_node_child_by_field_name(parent, field, field_len);
    return !ts_node_is_null(value) && ts_node_eq(value, node);
}

// `node` is passed as one whole argument: f(node), f((node)) in Python,
// f(cb=node) in Python, F(node) / F(name: node) in C#.
static bool is_direct_argument_value(const WalkState *state, TSNode node, CBMLanguage lang) {
    TSNode current = node;
    uint32_t up = 1;
    TSNode parent = cbm_walk_ancestor(state, current, up);
    while (lang == CBM_LANG_PYTHON && is_kind(parent, "parenthesized_expression") &&
           ts_node_named_child_count(parent) == 1) {
        node = parent;
        parent = cbm_walk_ancestor(state, current, ++up);
    }
    if (lang == CBM_LANG_PYTHON && is_kind(parent, "keyword_argument")) {
        if (!is_field_value(parent, TS_FIELD("value"), node)) {
            return false;
        }
        node = parent;
        parent = cbm_walk_ancestor(state, current, ++up);
    }
    if (lang == CBM_LANG_CSHARP && is_kind(parent, "argument")) {
        node = parent;
        parent = cbm_walk_ancestor(state, current, ++up);
    }
    return is_kind(parent, "arguments") || is_kind(parent, "argument_list");
}

// `node` is the whole right-hand side of a plain value binding:
// Python `cb = node` / `(cb := node)`, TS/JS `const cb = node` / `cb = node`.
static bool is_binding_value(const WalkState *state, TSNode node, CBMLanguage lang) {
    TSNode parent = cbm_walk_ancestor(state, node, 1);
    if (lang == CBM_LANG_PYTHON) {
        return (is_kind(parent, "assignment") && is_field_value(parent, TS_FIELD("right"), node)) ||
               (is_kind(parent, "named_expression") &&
                is_field_value(parent, TS_FIELD("value"), node));
    }
    if (lang == CBM_LANG_TYPESCRIPT || lang == CBM_LANG_TSX || lang == CBM_LANG_JAVASCRIPT) {
        return (is_kind(parent, "variable_declarator") &&
                is_field_value(parent, TS_FIELD("value"), node)) ||
               (is_kind(parent, "assignment_expression") &&
                is_field_value(parent, TS_FIELD("right"), node));
    }
    return false;
}

static bool is_candidate_value_kind(TSNode node, CBMLanguage lang) {
    const char *kind = ts_node_type(node);
    return strcmp(kind, "identifier") == 0 ||
           (lang == CBM_LANG_RUST && strcmp(kind, "scoped_identifier") == 0);
}

// Explicit reference syntax names its callable at the last named child:
// Java `Lhs::name`, Kotlin `::name` / `Lhs::name`. The reference occurrence
// (the join site) is the whole reference node.
static bool is_reference_syntax(TSNode node, CBMLanguage lang) {
    return (lang == CBM_LANG_JAVA && is_kind(node, "method_reference")) ||
           (lang == CBM_LANG_KOTLIN && is_kind(node, "callable_reference")) ||
           (lang == CBM_LANG_PHP && cbm_php_is_first_class_callable(node));
}

static bool is_reference_syntax_name(const WalkState *state, TSNode node, CBMLanguage lang) {
    TSNode parent = cbm_walk_ancestor(state, node, 1);
    if (ts_node_is_null(parent) || lang == CBM_LANG_PHP || !is_reference_syntax(parent, lang)) {
        return false;
    }
    uint32_t nc = ts_node_named_child_count(parent);
    return (lang == CBM_LANG_KOTLIN || nc >= 2) &&
           ts_node_eq(ts_node_named_child(parent, nc - 1), node);
}

static void push_reference_usage(CBMExtractCtx *ctx, const char *name, const char *func_qn,
                                 TSNode site, CBMUsageKind kind, bool reference_only) {
    CBMUsage usage = {0};
    usage.ref_name = name;
    usage.enclosing_func_qn = func_qn;
    usage.site_start_byte = ts_node_start_byte(site);
    usage.site_end_byte = ts_node_end_byte(site);
    usage.kind = kind;
    usage.may_be_call_reference = true;
    usage.reference_only = reference_only;
    cbm_usages_push(&ctx->result->usages, ctx->arena, usage);
}

// Java `Lhs::name` / `Lhs::new`, Kotlin `::name` / `Lhs::name`, PHP `f(...)` /
// `$o->m(...)` / `C::m(...)`. Emitted inside
// or outside a call: the reference is never an invocation, so it is not a CALLS
// edge. Kotlin `Foo::class` is a class literal, not a callable.
static void emit_reference_syntax(CBMExtractCtx *ctx, TSNode node, const WalkState *state) {
    uint32_t nc = ts_node_named_child_count(node);
    char *name = NULL;
    if (ctx->language == CBM_LANG_JAVA) {
        name = nc >= 2 ? cbm_node_text(ctx->arena, ts_node_named_child(node, nc - 1), ctx->source)
                       : "new"; // constructor reference: `new` is an unnamed token
    } else if (ctx->language == CBM_LANG_PHP) {
        TSNode target = ts_node_child_by_field_name(node, TS_FIELD("name"));
        if (ts_node_is_null(target)) {
            target = ts_node_child_by_field_name(node, TS_FIELD("function"));
        }
        // Only a written name: `$fn(...)` / `$o->$m(...)` name nothing.
        if (!ts_node_is_null(target) && (is_kind(target, "name") || is_kind(target, "qualified_name"))) {
            name = cbm_node_text(ctx->arena, target, ctx->source);
            const char *leaf = name ? strrchr(name, '\\') : NULL;
            if (leaf) {
                name = (char *)leaf + 1;
            }
        }
    } else if (nc >= 1 && is_kind(ts_node_named_child(node, nc - 1), "simple_identifier")) {
        name = cbm_node_text(ctx->arena, ts_node_named_child(node, nc - 1), ctx->source);
    }
    if (name && name[0]) {
        push_reference_usage(ctx, name, state->enclosing_func_qn, node, CBM_USAGE_CALL_REFERENCE,
                             false);
    }
}

// --- Unified handler: called once per node by the cursor walk ---
// Uses WalkState flags instead of parent-chain walks for O(1) context checks.

void handle_usages(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state) {
    (void)spec;
    if (is_reference_syntax(node, ctx->language)) {
        if (!state->inside_import) {
            emit_reference_syntax(ctx, node, state);
        }
        return;
    }
    if (!is_reference_node(node, ctx->language)) {
        return;
    }
    // The name half of explicit reference syntax is carried by the reference
    // node itself (above).
    if (is_reference_syntax_name(state, node, ctx->language)) {
        return;
    }

    // Inside a call only a value passed directly as an argument is kept, as a
    // candidate for an LSP-proven CALL_REFERENCE; everything else there stays
    // skipped (the call is already a CALLS edge) — O(1) via state
    if (state->inside_call) {
        if (!state->inside_import && lang_marks_argument_values(ctx->language) &&
            is_candidate_value_kind(node, ctx->language) &&
            is_direct_argument_value(state, node, ctx->language)) {
            char *arg = cbm_node_text(ctx->arena, node, ctx->source);
            if (arg && arg[0] && !cbm_is_keyword(arg, ctx->language)) {
                push_reference_usage(ctx, arg, state->enclosing_func_qn, node, CBM_USAGE_VALUE,
                                     true);
            }
        }
        return;
    }
    // Skip if inside an import
    if (state->inside_import) {
        return;
    }

    // Skip if it's a definition name (left side of assignment, function name)
    TSNode parent = cbm_walk_ancestor(state, node, 1);
    if (!ts_node_is_null(parent)) {
        TSNode name_field = ts_node_child_by_field_name(parent, TS_FIELD("name"));
        if (!ts_node_is_null(name_field) &&
            ts_node_start_byte(name_field) == ts_node_start_byte(node) &&
            ts_node_end_byte(name_field) == ts_node_end_byte(node)) {
            return;
        }
    }

    char *name = cbm_node_text(ctx->arena, node, ctx->source);
    if (name && name[0] && !cbm_is_keyword(name, ctx->language)) {
        CBMUsage usage = {0};
        usage.ref_name = name;
        usage.enclosing_func_qn = state->enclosing_func_qn;
        usage.site_start_byte = ts_node_start_byte(node);
        usage.site_end_byte = ts_node_end_byte(node);
        usage.may_be_call_reference = is_candidate_value_kind(node, ctx->language) &&
                                      is_binding_value(state, node, ctx->language);
        /* The member half of a selector is its own reference node
         * (field_identifier); record that shape — the name alone cannot carry
         * it, and the Go Field guard keys on it (#1962). */
        usage.is_member_access = strcmp(ts_node_type(node), "field_identifier") == 0;
        cbm_usages_push(&ctx->result->usages, ctx->arena, usage);
    }
}
