//! pass_k8s.zig — Pipeline pass for Kubernetes manifest and Kustomize overlay processing.
//!
//! For each discovered YAML file:
//!   1. Check if it is a kustomize overlay (kustomization.yaml / kustomization.yml)
//!      → emit a Module node and IMPORTS edges for each resources/bases/patches entry
//!   2. Else if it is a generic k8s manifest (apiVersion: detected)
//!      → emit one Resource node per file (first document only — multi-document YAML is not yet
//! supported)
//!
//! Depends on: pass_infrascan (cbm_is_kustomize_file, cbm_is_k8s_manifest, cbm_infra_qn),
//!             extraction layer (cbm.h), graph_buffer, pipeline internals.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Ctx = c.cbm_pipeline_ctx_t;
const Node = c.cbm_gbuf_node_t;

// ── Internal helpers ────────────────────────────────────────────────

/// Read entire file into heap-allocated buffer. Returns NULL on error.
/// Caller must free(). Sets out_len to byte count.
fn k8sReadFile(path: CStr, out_len: *c_int) MutStr {
    const f = c.cbm_fopen(path, "rb") orelse return null;

    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);

    if (size <= 0 or size > c.cbm_max_file_bytes()) { // generous, env-configurable cap (B4)
        _ = c.fclose(f);
        return null;
    }
    const usize_size: usize = @intCast(size);

    // +pad: tree-sitter lexer lookahead reads past EOF; keep it in-bounds
    const CBM_TS_LOOKAHEAD_PAD = 16;
    const buf: MutStr = @ptrCast(c.malloc(usize_size + CBM_TS_LOOKAHEAD_PAD));
    if (buf == null) {
        _ = c.fclose(f);
        return null;
    }

    var nread = c.fread(buf, 1, usize_size, f);
    _ = c.fclose(f);
    if (nread > usize_size) nread = usize_size;
    @memset(buf[nread..][0..CBM_TS_LOOKAHEAD_PAD], 0);
    out_len.* = @intCast(nread);
    return buf;
}

fn logFileCount(msg: [*:0]const u8, rel_path: CStr, key: [*:0]const u8, count: c_int) void {
    interop.logInfo(msg, .{
        @as(CStr, "file"), rel_path, @as(CStr, key), @as(CStr, c.cbm_log_itoa(count)),
    });
}

// ── Kustomize handler ───────────────────────────────────────────────

fn handleKustomize(ctx: *Ctx, path: CStr, rel_path: CStr) void {
    // Emit Module node for this kustomize overlay file
    const mod_qn = c.cbm_infra_qn(ctx.project_name, rel_path, "kustomize", null);
    if (mod_qn == null) return;

    const mod_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "Module", c.cbm_path_base(rel_path), mod_qn, rel_path, 1, 0, "{\"source\":\"kustomize\"}");
    c.free(mod_qn);

    if (mod_id <= 0) return;

    // Emit IMPORTS edges for resources/bases/patches/components entries
    var import_count: c_int = 0;
    var res: ?*c.CBMFileResult = null;
    var src_len: c_int = 0;
    const source = k8sReadFile(path, &src_len);
    if (source != null) {
        res = c.cbm_extract_file(source, src_len, c.CBM_LANG_KUSTOMIZE, ctx.project_name, rel_path, c.CBM_EXTRACT_BUDGET, null, null);
        c.free(source);
    }

    if (res) |result| {
        for (0..ix(result.imports.count)) |j| {
            const imp = util.at(result.imports.items, j);
            if (imp.module_path == null) continue;

            // Compute target file QN
            const target_qn = c.cbm_pipeline_fqn_compute(ctx.project_name, imp.module_path, "__file__");
            if (target_qn == null) continue;

            const target: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, target_qn);
            c.free(target_qn);

            if (target) |t| {
                _ = c.cbm_gbuf_insert_edge(ctx.gbuf, mod_id, t.id, "IMPORTS", "{\"via\":\"kustomize\"}");
                import_count += 1;
            }
        }
        c.cbm_free_result(result);
    }

    logFileCount("pass.k8s.kustomize", rel_path, "imports", import_count);
}

// ── K8s cross-manifest label-selector matching ──────────────────────
// A Service routes traffic to the workload Pods whose labels match the
// Service's spec.selector.  We record, per manifest, the Resource node id plus
// its selector label-values (Services) and pod label-values (workloads), then
// after all manifests are processed connect each Service to the workload(s) it
// targets via an INFRA_MAPS edge.  This models the runtime traffic path the same
// way k8s itself resolves Service → Endpoints by label.
//
// Matching is by label *value*: a Service selector `app: frontend` matches a
// workload that either carries the pod label value "frontend" OR is named
// "frontend" (the ubiquitous app==name convention, used when the pod template
// omits explicit labels).
const K8S_MAX_LABELS = 16;
const K8S_MAX_RECORDS = 512;
const K8S_LABEL_LEN = 128;

const LabelVals = [K8S_MAX_LABELS][K8S_LABEL_LEN]u8;

const K8sRecord = extern struct {
    node_id: i64,
    name: [K8S_LABEL_LEN]u8,
    is_service: bool,
    is_workload: bool,
    selector_vals: LabelVals, // Service spec.selector values
    n_selector: c_int,
    label_vals: LabelVals, // workload pod-template label values
    n_label: c_int,
};

const K8sRecordArray = struct {
    items: [*c]K8sRecord = null,
    count: c_int = 0,
    cap: c_int = 0,
};

/// Count leading-space indentation of a line (tabs are invalid YAML indent).
fn k8sIndent(line: [*]const u8) c_int {
    var n: usize = 0;
    while (line[n] == ' ') n += 1;
    return @intCast(n);
}

/// Split `key: value` (already de-indented). Returns true if a key was found;
/// fills key/val (val empty when the key opens a nested block).
fn k8sSplitKv(t: [*:0]const u8, key: []u8, val: []u8) bool {
    key[0] = 0;
    val[0] = 0;
    if (t[0] == '#' or t[0] == '-' or t[0] == 0) return false;
    const colon: CStr = c.strchr(t, ':');
    if (colon == null) return false;
    const klen = util.ptrDiff(colon, t);
    if (klen == 0 or klen >= key.len) return false;
    @memcpy(key[0..klen], t[0..klen]);
    key[klen] = 0;
    var v: [*]const u8 = colon + 1;
    while (v[0] == ' ' or v[0] == '\t') v += 1;
    var vn: usize = 0;
    while (v[vn] != 0 and v[vn] != '\r' and v[vn] != '\n' and v[vn] != '#' and vn + 1 < val.len) {
        val[vn] = v[vn];
        vn += 1;
    }
    // trim trailing space
    while (vn > 0 and (val[vn - 1] == ' ' or val[vn - 1] == '\t')) vn -= 1;
    val[vn] = 0;
    // strip surrounding quotes
    if (vn >= 2 and (val[0] == '"' or val[0] == '\'') and val[vn - 1] == val[0]) {
        _ = c.memmove(val.ptr, val.ptr + 1, vn - 2);
        val[vn - 2] = 0;
    }
    return true;
}

fn k8sAddVal(dst: *LabelVals, n: *c_int, v: CStr) void {
    if (n.* >= K8S_MAX_LABELS or v == null or v[0] == 0) return;
    _ = c.snprintf(&dst[ix(n.*)], K8S_LABEL_LEN, "%s", v);
    n.* += 1;
}

/// Scan a single-document k8s manifest's text for the resource name, selector
/// label-values (Service) and pod-template label-values (workload).  A small
/// indentation path-stack distinguishes spec.selector from
/// spec.template.metadata.labels and from top-level metadata.name.
fn k8sScanLabels(source: CStr, rec: *K8sRecord) void {
    const K8S_PATH_DEPTH = 12;
    const Frame = struct {
        indent: c_int,
        key: [64]u8,
    };
    var stack: [K8S_PATH_DEPTH]Frame = undefined;
    var depth: usize = 0;
    var got_name = false;

    var p: CStr = source;
    while (p != null and p[0] != 0) {
        const eol: CStr = c.strchr(p, '\n');
        const len: usize = if (eol != null) util.ptrDiff(eol, p) else c.strlen(p);
        var line: [c.CBM_SZ_512:0]u8 = undefined;
        const cp = @min(len, c.CBM_SZ_512 - 1);
        @memcpy(line[0..cp], p[0..cp]);
        line[cp] = 0;

        // End of first YAML document — stop (one Resource per file).
        var trimmed: [*:0]const u8 = &line;
        while (trimmed[0] == ' ') trimmed += 1;
        if (c.strncmp(trimmed, "---", 3) == 0 and @intFromPtr(trimmed) == @intFromPtr(&line)) break;

        if (trimmed[0] != 0 and trimmed[0] != '#') {
            const ind = k8sIndent(&line);
            while (depth > 0 and stack[depth - 1].indent >= ind) depth -= 1;
            var key: [64]u8 = undefined;
            var val: [K8S_LABEL_LEN]u8 = undefined;
            if (k8sSplitKv(trimmed, &key, &val)) {
                // Build the current dotted path for context decisions.
                const under_selector = depth >= 1 and streq(&stack[depth - 1].key, "selector");
                const under_labels = depth >= 1 and streq(&stack[depth - 1].key, "labels");
                const under_metadata = depth >= 1 and streq(&stack[depth - 1].key, "metadata");

                if (val[0] == 0) {
                    // Block-opening key: push onto the path stack.
                    if (depth < K8S_PATH_DEPTH) {
                        stack[depth].indent = ind;
                        _ = c.snprintf(&stack[depth].key, stack[depth].key.len, "%s", @as(CStr, &key));
                        depth += 1;
                    }
                } else {
                    // Leaf key: value.
                    if (ind == 0 and streq(&key, "kind")) {
                        rec.is_service = streq(&val, "Service");
                        rec.is_workload =
                            streq(&val, "Deployment") or streq(&val, "StatefulSet") or
                            streq(&val, "DaemonSet") or streq(&val, "ReplicaSet") or
                            streq(&val, "ReplicationController") or streq(&val, "Pod") or
                            streq(&val, "Job") or streq(&val, "CronJob");
                    } else if (!got_name and under_metadata and streq(&key, "name")) {
                        _ = c.snprintf(&rec.name, rec.name.len, "%s", @as(CStr, &val));
                        got_name = true;
                    } else if (under_selector) {
                        // Service spec.selector matchLabels values.
                        k8sAddVal(&rec.selector_vals, &rec.n_selector, &val);
                    } else if (under_labels) {
                        // Pod-template / metadata labels values.
                        k8sAddVal(&rec.label_vals, &rec.n_label, &val);
                    }
                }
            }
        }

        if (eol == null) break;
        p = eol + 1;
    }
}

/// True if any of the service's selector values matches the workload.
fn k8sSelectorMatches(svc: *const K8sRecord, wl: *const K8sRecord) bool {
    for (svc.selector_vals[0..ix(svc.n_selector)]) |*sv| {
        if (sv[0] == 0) continue;
        if (wl.name[0] != 0 and streq(sv, &wl.name)) return true;
        for (wl.label_vals[0..ix(wl.n_label)]) |*label| {
            if (streq(sv, label)) return true;
        }
    }
    return false;
}

/// After all manifests are recorded, connect each Service to the workload(s) its
/// selector targets via an INFRA_MAPS edge (Service Resource → workload Resource).
fn k8sLinkSelectors(ctx: *Ctx, recs: *const K8sRecordArray) void {
    var edges: c_int = 0;
    const count = ix(recs.count);
    for (0..count) |i| {
        const svc = util.at(recs.items, i);
        if (!svc.is_service or svc.n_selector == 0 or svc.node_id <= 0) continue;
        for (0..count) |j| {
            const wl = util.at(recs.items, j);
            if (i == j or !wl.is_workload or wl.node_id <= 0) continue;
            if (k8sSelectorMatches(svc, wl)) {
                var props: [c.CBM_SZ_256]u8 = undefined;
                _ = c.snprintf(
                    &props,
                    props.len,
                    "{\"kind\":\"selector\",\"service\":\"%s\",\"workload\":\"%s\"}",
                    @as(CStr, &svc.name),
                    @as(CStr, &wl.name),
                );
                _ = c.cbm_gbuf_insert_edge(ctx.gbuf, svc.node_id, wl.node_id, "INFRA_MAPS", &props);
                edges += 1;
            }
        }
    }
    if (edges > 0) {
        interop.logInfo("pass.k8s.selectors", .{
            @as(CStr, "linked"), @as(CStr, c.cbm_log_itoa(edges)),
        });
    }
}

// ── K8s manifest handler ────────────────────────────────────────────

/// The File node of rel_path, or null.
fn fileNodeOf(ctx: *Ctx, rel_path: CStr) ?*const Node {
    const file_qn = c.cbm_pipeline_fqn_compute(ctx.project_name, rel_path, "__file__");
    const node: ?*const Node = if (file_qn != null) c.cbm_gbuf_find_by_qn(ctx.gbuf, file_qn) else null;
    c.free(file_qn);
    return node;
}

/// source/src_len are the already-read file bytes (caller retains ownership and
/// must free after this call returns).  When `rec` is non-NULL it is populated
/// with the first Resource's node id, name and label/selector values for later
/// cross-manifest selector matching.
fn handleK8sManifest(ctx: *Ctx, rel_path: CStr, source: CStr, src_len: c_int, rec: ?*K8sRecord) void {
    var resource_count: c_int = 0;

    const res: *c.CBMFileResult = @as(?*c.CBMFileResult, c.cbm_extract_file(source, src_len, c.CBM_LANG_K8S, ctx.project_name, rel_path, c.CBM_EXTRACT_BUDGET, null, null)) orelse return;

    // Compute file node QN for DEFINES edges
    const file_node = fileNodeOf(ctx, rel_path);

    for (0..ix(res.defs.count)) |d| {
        const def = util.at(res.defs.items, d);
        if (def.label == null or !streq(def.label, "Resource")) continue;
        if (def.name == null or def.qualified_name == null) continue;

        const node_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "Resource", def.name, def.qualified_name, rel_path, @intCast(def.start_line), @intCast(def.end_line), "{\"source\":\"k8s\"}");

        // DEFINES edge: File → Resource
        if (file_node != null and node_id > 0) {
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, file_node.?.id, node_id, "DEFINES", "{}");
        }

        // Capture the first Resource for cross-manifest selector matching.
        if (rec) |r| {
            if (r.node_id <= 0 and node_id > 0) r.node_id = node_id;
        }

        resource_count += 1;
    }

    c.cbm_free_result(res);

    // Record selector / pod-label values for later Service → workload linking.
    if (rec) |r| {
        if (r.node_id > 0) k8sScanLabels(source, r);
    }

    logFileCount("pass.k8s.manifest", rel_path, "resources", resource_count);
}

// ── Helm chart handler ──────────────────────────────────────────────

fn isHelmChartFile(base: CStr) bool {
    return streq(base, "Chart.yaml") or streq(base, "Chart.yml");
}

/// Emit a Chart node for a Chart.yaml and a DEPENDS_ON edge to a (shared,
/// deduplicated) Chart node per declared dependency (#338).
fn handleHelmChart(ctx: *Ctx, rel_path: CStr, source: CStr) void {
    var hc: c.cbm_helm_chart_t = undefined;
    if (c.cbm_parse_helm_chart(source, &hc) != 0) return;

    const cname: CStr = if (hc.chart_name[0] != 0) &hc.chart_name else c.cbm_path_base(rel_path);
    const chart_qn = c.cbm_infra_qn(ctx.project_name, rel_path, "helm-chart", null);
    if (chart_qn == null) return;
    const chart_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "Chart", cname, chart_qn, rel_path, 1, 0, "{\"source\":\"helm\"}");
    c.free(chart_qn);

    if (fileNodeOf(ctx, rel_path)) |file_node| {
        if (chart_id > 0) {
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, file_node.id, chart_id, "DEFINES", "{}");
        }
    }

    var dep_edges: c_int = 0;
    var i: usize = 0;
    while (i < hc.dep_count and chart_id > 0) : (i += 1) {
        // Stable per-project QN so multiple charts depending on the same chart
        // link to one shared dependency node.
        const dep_name: CStr = &hc.deps[i];
        var dep_qn: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(&dep_qn, dep_qn.len, "%s.__helm_dep__.%s", ctx.project_name, dep_name);
        const dep_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "Chart", dep_name, &dep_qn, rel_path, 1, 0, "{\"source\":\"helm\",\"external\":true}");
        if (dep_id > 0) {
            _ = c.cbm_gbuf_insert_edge(ctx.gbuf, chart_id, dep_id, "DEPENDS_ON", "{}");
            dep_edges += 1;
        }
    }
    logFileCount("pass.k8s.helm", rel_path, "deps", dep_edges);
}

// ── Dependency-manifest handler (go.mod / requirements.txt) ─────────

fn isGomodFile(base: CStr) bool {
    return streq(base, "go.mod");
}

fn isRequirementsFile(base: CStr) bool {
    return streq(base, "requirements.txt");
}

/// Emit a DEPENDS_ON edge from the manifest file node to a (shared, per-project)
/// external Package node.  Mirrors the Helm Chart.yaml DEPENDS_ON shape.
fn emitDepEdge(ctx: *Ctx, src: *const Node, rel_path: CStr, ecosystem: CStr, name: [*:0]const u8) c_int {
    if (name[0] == 0) return 0;
    var dep_qn: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(&dep_qn, dep_qn.len, "%s.__%s_dep__.%s", ctx.project_name, ecosystem, name);
    var dep_props: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(&dep_props, dep_props.len, "{\"source\":\"%s\",\"external\":true}", ecosystem);
    const dep_id = c.cbm_gbuf_upsert_node(ctx.gbuf, "Package", name, &dep_qn, rel_path, 1, 0, &dep_props);
    if (dep_id > 0 and dep_id != src.id) {
        _ = c.cbm_gbuf_insert_edge(ctx.gbuf, src.id, dep_id, "DEPENDS_ON", "{}");
        return 1;
    }
    return 0;
}

/// Copy the first whitespace-delimited token of `line` into `out`.
fn firstToken(line_in: [*:0]const u8, out: [:0]u8) void {
    out[0] = 0;
    var line = line_in;
    while (line[0] == ' ' or line[0] == '\t') line += 1;
    var n: usize = 0;
    while (line[n] != 0 and line[n] != ' ' and line[n] != '\t' and line[n] != '\r' and
        line[n] != '\n' and n + 1 < out.len + 1)
    {
        out[n] = line[n];
        n += 1;
    }
    out[n] = 0;
}

/// One line of `source` starting at p, copied into `line` (truncated to fit).
/// Returns the pointer to the line's newline, or null on the last line.
fn copyLine(p: CStr, line: *[c.CBM_SZ_512:0]u8) CStr {
    const eol: CStr = c.strchr(p, '\n');
    const len: usize = if (eol != null) util.ptrDiff(eol, p) else c.strlen(p);
    const cp = @min(len, c.CBM_SZ_512 - 1);
    @memcpy(line[0..cp], p[0..cp]);
    line[cp] = 0;
    return eol;
}

/// Parse go.mod `require` directives (single-line and block forms) and emit a
/// DEPENDS_ON edge per dependency.  go.mod requires are not surfaced as imports
/// by the extraction layer, so we parse the manifest text directly here.
fn parseGomodDeps(ctx: *Ctx, src: *const Node, rel_path: CStr, source: CStr) c_int {
    var edges: c_int = 0;
    var in_block = false;
    var p: CStr = source;
    while (p != null and p[0] != 0) {
        var line: [c.CBM_SZ_512:0]u8 = undefined;
        const eol = copyLine(p, &line);
        var t: [*:0]const u8 = &line;
        while (t[0] == ' ' or t[0] == '\t') t += 1;
        if (in_block) {
            if (t[0] == ')') {
                in_block = false;
            } else if (t[0] != 0 and t[0] != '/') {
                var name: [c.CBM_SZ_256 - 1:0]u8 = undefined;
                firstToken(t, &name);
                edges += emitDepEdge(ctx, src, rel_path, "gomod", &name);
            }
        } else if (c.strncmp(t, "require", 7) == 0 and (t[7] == ' ' or t[7] == '\t' or t[7] == '(')) {
            var rest: [*:0]const u8 = t + 7;
            while (rest[0] == ' ' or rest[0] == '\t') rest += 1;
            if (rest[0] == '(') {
                in_block = true;
            } else if (rest[0] != 0) {
                var name: [c.CBM_SZ_256 - 1:0]u8 = undefined;
                firstToken(rest, &name);
                edges += emitDepEdge(ctx, src, rel_path, "gomod", &name);
            }
        }
        if (eol == null) break;
        p = eol + 1;
    }
    return edges;
}

/// Parse requirements.txt entries (one package spec per line) and emit a
/// DEPENDS_ON edge per dependency.  The package name is the leading token up to
/// the first version/extras/comment delimiter.
fn parseRequirementsDeps(ctx: *Ctx, src: *const Node, rel_path: CStr, source: CStr) c_int {
    var edges: c_int = 0;
    var p: CStr = source;
    while (p != null and p[0] != 0) {
        var line: [c.CBM_SZ_512:0]u8 = undefined;
        const eol = copyLine(p, &line);
        var t: [*:0]const u8 = &line;
        while (t[0] == ' ' or t[0] == '\t') t += 1;
        // Skip blanks, comments, options (-r, --hash), and URLs.
        if (t[0] != 0 and t[0] != '#' and t[0] != '-' and c.strstr(t, "://") == null) {
            var name: [c.CBM_SZ_256 - 1:0]u8 = undefined;
            var n: usize = 0;
            while (t[n] != 0 and t[n] != '=' and t[n] != '<' and t[n] != '>' and t[n] != '!' and
                t[n] != '~' and t[n] != '[' and t[n] != ';' and t[n] != ' ' and t[n] != '\t' and
                t[n] != '\r' and n + 1 < c.CBM_SZ_256)
            {
                name[n] = t[n];
                n += 1;
            }
            name[n] = 0;
            edges += emitDepEdge(ctx, src, rel_path, "pypi", &name);
        }
        if (eol == null) break;
        p = eol + 1;
    }
    return edges;
}

fn handleDepManifest(ctx: *Ctx, rel_path: CStr, source: CStr, ecosystem: CStr) void {
    if (source == null) return;
    const src = fileNodeOf(ctx, rel_path) orelse return;
    const dep_edges = if (streq(ecosystem, "gomod"))
        parseGomodDeps(ctx, src, rel_path, source)
    else
        parseRequirementsDeps(ctx, src, rel_path, source);
    logFileCount("pass.k8s.depmanifest", rel_path, "deps", dep_edges);
}

// ── Pass entry point ────────────────────────────────────────────────

pub export fn cbm_pipeline_pass_k8s(ctx: *Ctx, files: [*c]const c.cbm_file_info_t, file_count: c_int) c_int {
    interop.logInfo("pass.start", .{
        @as(CStr, "pass"), @as(CStr, "k8s"), @as(CStr, "files"), @as(CStr, c.cbm_log_itoa(file_count)),
    });

    _ = c.cbm_init();

    var kustomize_count: c_int = 0;
    var manifest_count: c_int = 0;

    // Collect per-manifest selector/label records for cross-manifest matching.
    var recs: K8sRecordArray = .{};
    recs.items = interop.callocN(K8sRecord, K8S_MAX_RECORDS);
    recs.cap = if (recs.items != null) K8S_MAX_RECORDS else 0;

    var i: usize = 0;
    while (i < file_count) : (i += 1) {
        if (util.checkCancel(ctx) != 0) {
            c.free(recs.items);
            return c.CBM_NOT_FOUND;
        }

        const path: CStr = files[i].path;
        const rel: CStr = files[i].rel_path;
        const lang = files[i].language;
        const base: CStr = c.cbm_path_base(rel);

        if (isGomodFile(base) or lang == c.CBM_LANG_GOMOD or isRequirementsFile(base)) {
            var dep_len: c_int = 0;
            const dep_src = k8sReadFile(path, &dep_len);
            if (dep_src != null) {
                handleDepManifest(ctx, rel, dep_src, if (isRequirementsFile(base)) "pypi" else "gomod");
                c.free(dep_src);
            }
        } else if (c.cbm_is_kustomize_file(base)) {
            handleKustomize(ctx, path, rel);
            kustomize_count += 1;
        } else if (lang == c.CBM_LANG_YAML or lang == c.CBM_LANG_K8S) {
            // Read source once to classify (and reuse for uncached extraction).
            var src_len: c_int = 0;
            const source = k8sReadFile(path, &src_len);
            if (source != null) {
                if (isHelmChartFile(base)) {
                    handleHelmChart(ctx, rel, source);
                } else if (c.cbm_is_k8s_manifest(base, source)) {
                    // Extract with CBM_LANG_K8S: the YAML extraction has no
                    // "Resource" definitions. Pass the already-read source
                    // buffer so handleK8sManifest does not re-read.
                    const rec: ?*K8sRecord = if (recs.count < recs.cap) util.at(recs.items, ix(recs.count)) else null;
                    handleK8sManifest(ctx, rel, source, src_len, rec);
                    if (rec) |r| {
                        if (r.node_id > 0) recs.count += 1;
                    }
                    manifest_count += 1;
                }
                c.free(source);
            }
        }
    }

    // Connect Services to the workloads their selectors target (INFRA_MAPS).
    k8sLinkSelectors(ctx, &recs);
    c.free(recs.items);

    interop.logInfo("pass.done", .{
        @as(CStr, "pass"),      @as(CStr, "k8s"),
        @as(CStr, "kustomize"), @as(CStr, c.cbm_log_itoa(kustomize_count)),
        @as(CStr, "manifests"), @as(CStr, c.cbm_log_itoa(manifest_count)),
    });
    return 0;
}
