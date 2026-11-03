/*
 * service_bindings.c — Classify a call as a service call through what its
 * receiver or callee name is bound to.
 *
 *   cbm_pipeline_http_client_call_url    a call on an axios.create() instance
 *                                         (#1916): the instance's baseURL joined
 *                                         with the request path.
 *   cbm_pipeline_external_import_service a callee an import binds to an external
 *                                         client library (`send` under
 *                                         `from paho.mqtt import publish as send`).
 *
 * Used by the CALLS resolution in pass_parallel.c.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/limits.h"
#include "cbm.h"
#include "service_patterns.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The graph node whose QN is cbm_pipeline_fqn_compute(project, rel_path, name),
 * or NULL. */
static const cbm_gbuf_node_t *pc_find_by_computed_qn(const cbm_gbuf_t *gbuf, const char *project,
                                                     const char *rel_path, const char *name) {
    char *qn = cbm_pipeline_fqn_compute(project, rel_path, name);
    const cbm_gbuf_node_t *n = qn ? cbm_gbuf_find_by_qn(gbuf, qn) : NULL;
    free(qn);
    return n;
}

/* ── #1916: axios client-instance calls ─────────────────────────── */

/* Copy the plain (unescaped) JSON string value of "key" in props into out.
 * False when absent, escaped, or too long — an unreadable base is unknown. */
static bool pc_json_str_prop(const char *props, const char *key, char *out, size_t out_sz) {
    if (!props) {
        return false;
    }
    char pat[CBM_SZ_64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(props, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) {
        return false;
    }
    size_t len = (size_t)(e - p);
    if (len >= out_sz || memchr(p, '\\', len) != NULL) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* A node the extractor marked as an axios instance. `base` receives the
 * literal baseURL ("" when unknown). */
static bool pc_node_is_http_client(const cbm_gbuf_node_t *n, char *base, size_t base_sz) {
    char client[CBM_SZ_32];
    if (!n || !pc_json_str_prop(n->properties_json, "http_client", client, sizeof(client))) {
        return false;
    }
    if (!pc_json_str_prop(n->properties_json, "http_base_url", base, base_sz)) {
        base[0] = '\0';
    }
    return true;
}

/* The module-level Variable `name` declared in file `file_rel`. */
static const cbm_gbuf_node_t *pc_module_var(const cbm_gbuf_t *gbuf, const char *project,
                                            const char *file_rel, const char *name) {
    if (!file_rel) {
        return NULL;
    }
    const cbm_gbuf_node_t *n = pc_find_by_computed_qn(gbuf, project, file_rel, name);
    return (n && n->label && strcmp(n->label, "Variable") == 0) ? n : NULL;
}

/* Resolve the binding `recv` names in the calling file to a client node:
 * a same-file declaration first, then its ES import (default import → the
 * module's default export; named import → that module's Variable). */
static bool pc_receiver_client_base(const cbm_gbuf_t *gbuf, const char *project, const char *rel,
                                    const CBMFileResult *result, const char **imp_keys,
                                    const char **imp_vals, int imp_count, const char *recv,
                                    char *base, size_t base_sz) {
    if (pc_node_is_http_client(pc_module_var(gbuf, project, rel, recv), base, base_sz)) {
        return true;
    }
    const CBMImport *imp = NULL;
    for (int i = 0; result && i < result->imports.count; i++) {
        const CBMImport *it = &result->imports.items[i];
        if (it->local_name && strcmp(it->local_name, recv) == 0) {
            imp = it;
            break;
        }
    }
    if (!imp) {
        return false;
    }
    for (int i = 0; i < imp_count; i++) {
        if (!imp_keys[i] || strcmp(imp_keys[i], recv) != 0 || !imp_vals[i]) {
            continue;
        }
        const cbm_gbuf_node_t *t = cbm_gbuf_find_by_qn(gbuf, imp_vals[i]);
        if (!t || !t->label) {
            return false;
        }
        if (strcmp(t->label, "Variable") == 0) {
            return pc_node_is_http_client(t, base, base_sz);
        }
        if (strcmp(t->label, "Module") != 0) {
            return false;
        }
        if (imp->is_default) {
            return pc_node_is_http_client(t, base, base_sz);
        }
        return pc_node_is_http_client(pc_module_var(gbuf, project, t->file_path, recv), base,
                                      base_sz);
    }
    return false;
}

/* axios instance request methods (the verb after the receiver). */
static bool pc_is_axios_verb(const char *verb) {
    static const char *const verbs[] = {"get",   "post", "put",     "delete",
                                        "patch", "head", "options", NULL};
    for (int i = 0; verbs[i]; i++) {
        if (strcmp(verb, verbs[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Copy the plain-identifier receiver of `callee` (text before `dot`) into
 * recv. False for `this.api.get`, `a().get` and other non-binding receivers. */
static bool pc_plain_receiver(const char *callee, const char *dot, char *recv, size_t recv_sz) {
    size_t rlen = (size_t)(dot - callee);
    if (rlen == 0 || rlen >= recv_sz) {
        return false;
    }
    for (size_t i = 0; i < rlen; i++) {
        char c = callee[i];
        bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '_' || c == '$';
        if (!ident) {
            return false;
        }
    }
    memcpy(recv, callee, rlen);
    recv[rlen] = '\0';
    return true;
}

bool cbm_pipeline_http_client_call_url(const cbm_gbuf_t *gbuf, const char *project, const char *rel,
                                       const CBMFileResult *result, const char **imp_keys,
                                       const char **imp_vals, int imp_count, const CBMCall *call,
                                       char *out, size_t out_sz) {
    const char *callee = call ? call->callee_name : NULL;
    const char *url = call ? call->first_string_arg : NULL;
    if (!gbuf || !project || !callee || !url || (url[0] != '/' && !strstr(url, "://"))) {
        return false;
    }
    const char *dot = strrchr(callee, '.');
    char recv[CBM_SZ_128];
    if (!dot || !pc_is_axios_verb(dot + SKIP_ONE) ||
        !pc_plain_receiver(callee, dot, recv, sizeof(recv))) {
        return false;
    }
    char base[CBM_SZ_256];
    if (!pc_receiver_client_base(gbuf, project, rel, result, imp_keys, imp_vals, imp_count, recv,
                                 base, sizeof(base))) {
        return false;
    }
    /* axios combineURLs: an absolute request URL ignores baseURL; otherwise
     * the base (trailing '/' trimmed) is joined with the '/'-leading path. */
    if (strstr(url, "://") || base[0] == '\0') {
        int n = snprintf(out, out_sz, "%s", url);
        return n > 0 && (size_t)n < out_sz;
    }
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - SKIP_ONE] == '/') {
        base[--blen] = '\0';
    }
    int n = snprintf(out, out_sz, "%s%s", base, url);
    return n > 0 && (size_t)n < out_sz;
}

/* ── Calls through an import of an external library ─────────────────
 * See cbm_pipeline_external_import_service in pipeline_internal.h. */

static bool pc_external_import_lang(CBMLanguage lang) {
    return lang == CBM_LANG_PYTHON || lang == CBM_LANG_JAVASCRIPT || lang == CBM_LANG_TYPESCRIPT ||
           lang == CBM_LANG_TSX;
}

/* Whether an import can name an external module at all: a relative path
 * never does, nor (JS/TS) an absolute path or a project-root alias. */
static bool pc_import_may_be_external(CBMLanguage lang, const CBMImport *imp) {
    const char *m = imp->module_path;
    if (!m || !m[0] || m[0] == '.' || strstr(m, " as ")) {
        return false;
    }
    if (lang == CBM_LANG_PYTHON) {
        return true;
    }
    return m[0] != '/' && m[0] != '~' && m[0] != '#' && strncmp(m, "@/", PAIR_LEN) != 0;
}

/* Two imports bind the same name to the same thing. */
static bool pc_same_binding(const CBMImport *a, const CBMImport *b) {
    return strcmp(a->module_path, b->module_path) == 0 &&
           (a->member == b->member ||
            (a->member && b->member && strcmp(a->member, b->member) == 0));
}

/* The callee as its import makes it; false when the import adds nothing to
 * the callee's own spelling.
 *   Python binds `local` to module_path: `from m import x as y` and
 *   `import m.x as y` both store y -> m.x, so `y.f` is m.x.f. A plain
 *   `import a` binds `a` and the callee already spells its path.
 *   JS/TS: a named import binds the module's member, alias or not
 *   (`navigate` under `import { get as navigate } from 'axios'` is
 *   axios.get); a default or namespace import binds the module itself, so
 *   `ax.get` is axios.get. */
static bool pc_spell_through_import(CBMLanguage lang, const CBMImport *imp, const char *callee,
                                    size_t root_len, char *out, size_t out_sz) {
    static const char node_scheme[] = "node:";
    const char *module = imp->module_path;
    const char *rest = callee + root_len;
    int n;
    if (lang == CBM_LANG_PYTHON) {
        size_t local_len = strlen(imp->local_name);
        if (strncmp(module, imp->local_name, local_len) == 0 &&
            (module[local_len] == '\0' || module[local_len] == '.')) {
            return false;
        }
        n = snprintf(out, out_sz, "%s%s", module, rest);
    } else {
        if (strncmp(module, node_scheme, sizeof(node_scheme) - SKIP_ONE) == 0) {
            module += sizeof(node_scheme) - SKIP_ONE;
        }
        n = imp->member ? snprintf(out, out_sz, "%s.%s%s", module, imp->member, rest)
                        : snprintf(out, out_sz, "%s%s", module, rest);
    }
    return n > 0 && (size_t)n < out_sz;
}

cbm_svc_kind_t cbm_pipeline_external_import_service(CBMLanguage lang,
                                                    const CBMImportArray *imports,
                                                    const char **imp_keys, int imp_count,
                                                    const CBMCall *call, char *spelled,
                                                    size_t spelled_sz) {
    if (!spelled || spelled_sz == 0) {
        return CBM_SVC_NONE;
    }
    spelled[0] = '\0';
    const char *callee = call ? call->callee_name : NULL;
    const char *u = call ? call->first_string_arg : NULL;
    if (!imports || imports->count <= 0 || !callee || !callee[0] || !u || !u[0] ||
        !pc_external_import_lang(lang) ||
        (lang == CBM_LANG_PYTHON && call->callee_is_locally_bound) ||
        cbm_service_pattern_route_method(callee) != NULL ||
        cbm_service_pattern_is_global_fetch(callee)) {
        return CBM_SVC_NONE;
    }
    const char *dot = strchr(callee, '.');
    size_t root_len = dot ? (size_t)(dot - callee) : strlen(callee);
    char root[CBM_SZ_256];
    if (root_len == 0 || root_len >= sizeof(root)) {
        return CBM_SVC_NONE;
    }
    memcpy(root, callee, root_len);
    root[root_len] = '\0';
    /* The import map holds exactly the imports that resolved to a project
     * node: such a name is a project module, whatever its spelling. */
    for (int i = 0; i < imp_count; i++) {
        if (imp_keys && imp_keys[i] && strcmp(imp_keys[i], root) == 0) {
            return CBM_SVC_NONE;
        }
    }
    /* One external binding only: a relative import of the name, or a second
     * import that binds it to something else, leaves it ambiguous. */
    const CBMImport *imp = NULL;
    for (int i = 0; i < imports->count; i++) {
        const CBMImport *cand = &imports->items[i];
        if (!cand->local_name || strcmp(cand->local_name, root) != 0) {
            continue;
        }
        if (!pc_import_may_be_external(lang, cand) || (imp && !pc_same_binding(imp, cand))) {
            return CBM_SVC_NONE;
        }
        imp = cand;
    }
    if (!imp || !pc_spell_through_import(lang, imp, callee, root_len, spelled, spelled_sz)) {
        spelled[0] = '\0';
        return CBM_SVC_NONE;
    }
    cbm_svc_kind_t svc = cbm_service_pattern_match(spelled);
    bool is_url = u[0] == '/' || strstr(u, "://") != NULL;
    bool is_topic = svc == CBM_SVC_ASYNC && strlen(u) > PAIR_LEN;
    /* An HTTP library also exports helpers that take a URL without sending
     * one (urllib.parse.urljoin): only a request verb is a request. */
    bool is_request = svc == CBM_SVC_HTTP && cbm_service_pattern_http_method(spelled) != NULL;
    if ((is_request && is_url) || (svc == CBM_SVC_ASYNC && (is_url || is_topic))) {
        return svc;
    }
    spelled[0] = '\0';
    return CBM_SVC_NONE;
}
