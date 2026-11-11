//! service_bindings.zig — Classify a call as a service call through what its
//! receiver or callee name is bound to.
//!
//!   cbm_pipeline_http_client_call_url    a call on an axios.create() instance
//!                                         (#1916): the instance's baseURL joined
//!                                         with the request path.
//!   cbm_pipeline_external_import_service a callee an import binds to an external
//!                                         client library (`send` under
//!                                         `from paho.mqtt import publish as send`).
//!
//! Used by the CALLS resolution in pass_parallel.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const streq = util.streq;
const Node = c.cbm_gbuf_node_t;

/// The graph node whose QN is cbm_pipeline_fqn_compute(project, rel_path, name),
/// or NULL.
fn pcFindByComputedQn(gbuf: ?*const c.cbm_gbuf_t, project: CStr, rel_path: CStr, name: CStr) ?*const Node {
    const qn: MutStr = c.cbm_pipeline_fqn_compute(project, rel_path, name);
    const n: ?*const Node = if (qn != null) c.cbm_gbuf_find_by_qn(gbuf, qn) else null;
    c.free(qn);
    return n;
}

// ── #1916: axios client-instance calls ──────────────────────────────

/// Copy the plain (unescaped) JSON string value of "key" in props into out.
/// False when absent, escaped, or too long — an unreadable base is unknown.
fn pcJsonStrProp(props: CStr, key: CStr, out: []u8) bool {
    if (props == null) return false;
    var pat: [c.CBM_SZ_64]u8 = undefined;
    _ = c.snprintf(&pat, pat.len, "\"%s\":\"", key);
    var p: CStr = c.strstr(props, &pat);
    if (p == null) return false;
    p += c.strlen(&pat);
    const e: CStr = c.strchr(p, '"');
    if (e == null) return false;
    const len = util.ptrDiff(e, p);
    if (len >= out.len or c.memchr(p, '\\', len) != null) return false;
    @memcpy(out[0..len], p[0..len]);
    out[len] = 0;
    return true;
}

/// A node the extractor marked as an axios instance. `base` receives the
/// literal baseURL ("" when unknown).
fn pcNodeIsHttpClient(n_c: ?*const Node, base: []u8) bool {
    var client: [c.CBM_SZ_32]u8 = undefined;
    const n = n_c orelse return false;
    if (!pcJsonStrProp(n.properties_json, "http_client", &client)) return false;
    if (!pcJsonStrProp(n.properties_json, "http_base_url", base)) base[0] = 0;
    return true;
}

/// The module-level Variable `name` declared in file `file_rel`.
fn pcModuleVar(gbuf: ?*const c.cbm_gbuf_t, project: CStr, file_rel: CStr, name: CStr) ?*const Node {
    if (file_rel == null) return null;
    const n = pcFindByComputedQn(gbuf, project, file_rel, name) orelse return null;
    return if (n.label != null and streq(n.label, "Variable")) n else null;
}

/// Resolve the binding `recv` names in the calling file to a client node:
/// a same-file declaration first, then its ES import (default import → the
/// module's default export; named import → that module's Variable).
fn pcReceiverClientBase(
    gbuf: ?*const c.cbm_gbuf_t,
    project: CStr,
    rel: CStr,
    result: ?*const c.CBMFileResult,
    imp_keys: [*c]const CStr,
    imp_vals: [*c]const CStr,
    imp_count: c_int,
    recv: CStr,
    base: []u8,
) bool {
    if (pcNodeIsHttpClient(pcModuleVar(gbuf, project, rel, recv), base)) return true;
    var imp: ?*const c.CBMImport = null;
    if (result) |res| {
        var i: usize = 0;
        while (i < res.imports.count) : (i += 1) {
            const it = util.at(res.imports.items, i);
            if (it.local_name != null and streq(it.local_name, recv)) {
                imp = it;
                break;
            }
        }
    }
    const found = imp orelse return false;
    var i: usize = 0;
    while (i < imp_count) : (i += 1) {
        if (imp_keys[i] == null or !streq(imp_keys[i], recv) or imp_vals[i] == null) continue;
        const t: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_qn(gbuf, imp_vals[i])) orelse return false;
        if (t.label == null) return false;
        if (streq(t.label, "Variable")) return pcNodeIsHttpClient(t, base);
        if (!streq(t.label, "Module")) return false;
        if (found.is_default) return pcNodeIsHttpClient(t, base);
        return pcNodeIsHttpClient(pcModuleVar(gbuf, project, t.file_path, recv), base);
    }
    return false;
}

/// axios instance request methods (the verb after the receiver).
fn pcIsAxiosVerb(verb: CStr) bool {
    const verbs = [_][*:0]const u8{ "get", "post", "put", "delete", "patch", "head", "options" };
    for (verbs) |v| {
        if (streq(verb, v)) return true;
    }
    return false;
}

/// Copy the plain-identifier receiver of `callee` (text before `dot`) into
/// recv. False for `this.api.get`, `a().get` and other non-binding receivers.
fn pcPlainReceiver(callee: CStr, dot: CStr, recv: []u8) bool {
    const rlen = util.ptrDiff(dot, callee);
    if (rlen == 0 or rlen >= recv.len) return false;
    for (callee[0..rlen]) |ch| {
        const ident = (ch >= 'a' and ch <= 'z') or (ch >= 'A' and ch <= 'Z') or
            (ch >= '0' and ch <= '9') or ch == '_' or ch == '$';
        if (!ident) return false;
    }
    @memcpy(recv[0..rlen], callee[0..rlen]);
    recv[rlen] = 0;
    return true;
}

pub export fn cbm_pipeline_http_client_call_url(
    gbuf: ?*const c.cbm_gbuf_t,
    project: CStr,
    rel: CStr,
    result: ?*const c.CBMFileResult,
    imp_keys: [*c]CStr,
    imp_vals: [*c]CStr,
    imp_count: c_int,
    call: ?*const c.CBMCall,
    out: MutStr,
    out_sz: usize,
) bool {
    const callee: CStr = if (call) |cl| cl.callee_name else null;
    const url: CStr = if (call) |cl| cl.first_string_arg else null;
    if (gbuf == null or project == null or callee == null or url == null or
        (url[0] != '/' and c.strstr(url, "://") == null))
    {
        return false;
    }
    const dot: CStr = c.strrchr(callee, '.');
    var recv: [c.CBM_SZ_128]u8 = undefined;
    if (dot == null or !pcIsAxiosVerb(dot + 1) or !pcPlainReceiver(callee, dot, &recv)) {
        return false;
    }
    var base: [c.CBM_SZ_256]u8 = undefined;
    if (!pcReceiverClientBase(gbuf, project, rel, result, imp_keys, imp_vals, imp_count, &recv, &base)) {
        return false;
    }
    // axios combineURLs: an absolute request URL ignores baseURL; otherwise
    // the base (trailing '/' trimmed) is joined with the '/'-leading path.
    if (c.strstr(url, "://") != null or base[0] == 0) {
        const n = c.snprintf(out, out_sz, "%s", url);
        return n > 0 and @as(usize, @intCast(n)) < out_sz;
    }
    var blen = c.strlen(&base);
    while (blen > 0 and base[blen - 1] == '/') {
        blen -= 1;
        base[blen] = 0;
    }
    const n = c.snprintf(out, out_sz, "%s%s", @as(CStr, &base), url);
    return n > 0 and @as(usize, @intCast(n)) < out_sz;
}

// ── Calls through an import of an external library ──────────────────
// See cbm_pipeline_external_import_service in pipeline_internal.h.

fn pcExternalImportLang(lang: c.CBMLanguage) bool {
    return lang == c.CBM_LANG_PYTHON or lang == c.CBM_LANG_JAVASCRIPT or
        lang == c.CBM_LANG_TYPESCRIPT or lang == c.CBM_LANG_TSX;
}

/// Whether an import can name an external module at all: a relative path
/// never does, nor (JS/TS) an absolute path or a project-root alias.
fn pcImportMayBeExternal(lang: c.CBMLanguage, imp: *const c.CBMImport) bool {
    const m: CStr = imp.module_path;
    if (m == null or m[0] == 0 or m[0] == '.' or c.strstr(m, " as ") != null) return false;
    if (lang == c.CBM_LANG_PYTHON) return true;
    return m[0] != '/' and m[0] != '~' and m[0] != '#' and c.strncmp(m, "@/", 2) != 0;
}

/// Two imports bind the same name to the same thing.
fn pcSameBinding(a: *const c.CBMImport, b: *const c.CBMImport) bool {
    return streq(a.module_path, b.module_path) and
        (a.member == b.member or
            (a.member != null and b.member != null and streq(a.member, b.member)));
}

/// The callee as its import makes it; false when the import adds nothing to
/// the callee's own spelling.
///   Python binds `local` to module_path: `from m import x as y` and
///   `import m.x as y` both store y -> m.x, so `y.f` is m.x.f. A plain
///   `import a` binds `a` and the callee already spells its path.
///   JS/TS: a named import binds the module's member, alias or not
///   (`navigate` under `import { get as navigate } from 'axios'` is
///   axios.get); a default or namespace import binds the module itself, so
///   `ax.get` is axios.get.
fn pcSpellThroughImport(
    lang: c.CBMLanguage,
    imp: *const c.CBMImport,
    callee: CStr,
    root_len: usize,
    out: MutStr,
    out_sz: usize,
) bool {
    const node_scheme = "node:";
    var module: CStr = imp.module_path;
    const rest: CStr = callee + root_len;
    var n: c_int = undefined;
    if (lang == c.CBM_LANG_PYTHON) {
        const local_len = c.strlen(imp.local_name);
        if (c.strncmp(module, imp.local_name, local_len) == 0 and
            (module[local_len] == 0 or module[local_len] == '.'))
        {
            return false;
        }
        n = c.snprintf(out, out_sz, "%s%s", module, rest);
    } else {
        if (c.strncmp(module, node_scheme, node_scheme.len) == 0) {
            module += node_scheme.len;
        }
        n = if (imp.member != null)
            c.snprintf(out, out_sz, "%s.%s%s", module, imp.member, rest)
        else
            c.snprintf(out, out_sz, "%s%s", module, rest);
    }
    return n > 0 and @as(usize, @intCast(n)) < out_sz;
}

pub export fn cbm_pipeline_external_import_service(
    lang: c.CBMLanguage,
    imports_c: ?*const c.CBMImportArray,
    imp_keys: [*c]CStr,
    imp_count: c_int,
    call: ?*const c.CBMCall,
    spelled: MutStr,
    spelled_sz: usize,
) c.cbm_svc_kind_t {
    if (spelled == null or spelled_sz == 0) return c.CBM_SVC_NONE;
    spelled[0] = 0;
    const callee: CStr = if (call) |cl| cl.callee_name else null;
    const u: CStr = if (call) |cl| cl.first_string_arg else null;
    const imports = imports_c orelse return c.CBM_SVC_NONE;
    if (imports.count <= 0 or callee == null or callee[0] == 0 or u == null or u[0] == 0 or
        !pcExternalImportLang(lang) or
        (lang == c.CBM_LANG_PYTHON and call.?.callee_is_locally_bound) or
        c.cbm_service_pattern_route_method(callee) != null or
        c.cbm_service_pattern_is_global_fetch(callee))
    {
        return c.CBM_SVC_NONE;
    }
    const dot: CStr = c.strchr(callee, '.');
    const root_len: usize = if (dot != null) util.ptrDiff(dot, callee) else c.strlen(callee);
    var root: [c.CBM_SZ_256]u8 = undefined;
    if (root_len == 0 or root_len >= root.len) return c.CBM_SVC_NONE;
    @memcpy(root[0..root_len], callee[0..root_len]);
    root[root_len] = 0;
    // The import map holds exactly the imports that resolved to a project
    // node: such a name is a project module, whatever its spelling.
    var i: usize = 0;
    while (i < imp_count) : (i += 1) {
        if (imp_keys != null and imp_keys[i] != null and streq(imp_keys[i], &root)) {
            return c.CBM_SVC_NONE;
        }
    }
    // One external binding only: a relative import of the name, or a second
    // import that binds it to something else, leaves it ambiguous.
    var imp: ?*const c.CBMImport = null;
    i = 0;
    while (i < imports.count) : (i += 1) {
        const cand = util.at(imports.items, i);
        if (cand.local_name == null or !streq(cand.local_name, &root)) continue;
        if (!pcImportMayBeExternal(lang, cand) or (imp != null and !pcSameBinding(imp.?, cand))) {
            return c.CBM_SVC_NONE;
        }
        imp = cand;
    }
    if (imp == null or !pcSpellThroughImport(lang, imp.?, callee, root_len, spelled, spelled_sz)) {
        spelled[0] = 0;
        return c.CBM_SVC_NONE;
    }
    const svc = c.cbm_service_pattern_match(spelled);
    const is_url = u[0] == '/' or c.strstr(u, "://") != null;
    const is_topic = svc == c.CBM_SVC_ASYNC and c.strlen(u) > 2;
    // An HTTP library also exports helpers that take a URL without sending
    // one (urllib.parse.urljoin): only a request verb is a request.
    const is_request = svc == c.CBM_SVC_HTTP and c.cbm_service_pattern_http_method(spelled) != null;
    if ((is_request and is_url) or (svc == c.CBM_SVC_ASYNC and (is_url or is_topic))) {
        return svc;
    }
    spelled[0] = 0;
    return c.CBM_SVC_NONE;
}
