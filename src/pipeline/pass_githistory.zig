//! pass_githistory.zig — Analyze git log to find change coupling.
//!
//! Runs `git log --name-only --since=1 year ago` and computes
//! file pairs that change together frequently. Creates FILE_CHANGES_WITH
//! edges between File nodes with coupling_score properties.
//!
//! Skips commits with >20 files (refactoring/merge noise).
//! Requires minimum 3 co-changes for an edge.
//!
//! Depends on: pass_structure having created File nodes

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Node = c.cbm_gbuf_node_t;
const HashTable = c.CBMHashTable;
const Coupling = c.cbm_change_coupling_t;
const FileTemporal = c.cbm_file_temporal_t;

const GH_INIT_CAP = 16;
const GH_MIN_COMMITS = 3;
const GH_MAX_FILES = 20;

/// Minimum coupling score to create an edge
const MIN_COUPLING_SCORE: f64 = 0.3;

fn hasPrefix(s: CStr, comptime prefix: [:0]const u8) bool {
    return c.strncmp(s, prefix, prefix.len) == 0;
}

pub export fn cbm_is_trackable_file(path: CStr) bool {
    if (path == null) return false;
    // Skip directory prefixes
    if (hasPrefix(path, ".git/") or hasPrefix(path, "node_modules/") or
        hasPrefix(path, "vendor/") or hasPrefix(path, "__pycache__/") or
        hasPrefix(path, ".cache/"))
    {
        return false;
    }
    // Skip lock/generated file names
    var base: CStr = c.strrchr(path, '/');
    base = if (base != null) base + 1 else path;
    if (streq(base, "package-lock.json") or streq(base, "yarn.lock") or
        streq(base, "pnpm-lock.yaml") or streq(base, "Cargo.lock") or
        streq(base, "poetry.lock") or streq(base, "composer.lock") or
        streq(base, "Gemfile.lock") or streq(base, "Pipfile.lock"))
    {
        return false;
    }
    // Skip non-source file extensions
    const skipped = [_][*:0]const u8{
        ".lock", ".sum", ".min.js", ".min.css", ".map", ".wasm", ".png", ".jpg", ".gif", ".ico", ".svg",
    };
    for (skipped) |suffix| {
        if (c.cbm_str_ends_with(path, suffix)) return false;
    }
    return true;
}

// ── Commit parsing ──────────────────────────────────────────────────

const Commit = extern struct {
    files: [*c]MutStr,
    count: c_int,
    cap: c_int,
    timestamp: c_longlong, // unix epoch of this commit; 0 when unknown
};

fn commitAddFile(commit: *Commit, file: CStr) void {
    if (commit.count >= commit.cap) {
        commit.cap = if (commit.cap != 0) commit.cap * 2 else GH_INIT_CAP;
        commit.files = interop.safeReallocN(MutStr, commit.files, ix(commit.cap));
    }
    commit.files[ix(commit.count)] = c.strdup(file);
    commit.count += 1;
}

fn commitFree(commit: *Commit) void {
    for (0..ix(commit.count)) |i| c.free(commit.files[i]);
    c.free(@ptrCast(commit.files));
}

// ── git log parsing (popen "git log") ───────────────────────────────

/// Appends `current` to the commit array, doubling it when full.
fn pushCommit(commits: *[*c]Commit, count: *c_int, cap: *c_int, current: Commit) void {
    if (count.* >= cap.*) {
        cap.* *= 2;
        commits.* = interop.safeReallocN(Commit, commits.*, ix(cap.*));
    }
    commits.*[ix(count.*)] = current;
    count.* += 1;
}

fn parseGitLog(repo_path: CStr, out: *[*c]Commit, out_count: *c_int) c_int {
    out.* = null;
    out_count.* = 0;

    if (!c.cbm_validate_shell_arg(repo_path)) return c.CBM_NOT_FOUND;

    var cmd: [c.CBM_SZ_1K]u8 = undefined;
    // cmd.exe does not recognize single quotes, and '/dev/null' is a POSIX path.
    const null_dev: CStr = if (builtin.os.tag == .windows) "NUL" else "/dev/null";
    // git -C "<path>" works on both cmd.exe and POSIX shells. Double quotes are
    // safe here because cbm_validate_shell_arg (above) rejects ", $, `, \ and the
    // other shell metacharacters that would otherwise be active inside them.
    _ = c.snprintf(
        &cmd,
        cmd.len,
        "git -C \"%s\" log --name-only --pretty=format:COMMIT:%%H:%%ct " ++
            "--since=\"1 year ago\" --max-count=10000 2>%s",
        repo_path,
        null_dev,
    );

    const fp = c.cbm_popen_git(&cmd) orelse return c.CBM_NOT_FOUND;

    var cap: c_int = c.CBM_SZ_64;
    var commits: [*c]Commit = interop.mallocN(Commit, ix(cap));
    var count: c_int = 0;
    var current = std.mem.zeroes(Commit);

    var line: [c.CBM_SZ_1K]u8 = undefined;
    while (c.fgets(&line, line.len, fp) != null) {
        var len = c.strlen(&line);
        while (len > 0 and (line[len - 1] == '\n' or line[len - 1] == '\r')) {
            len -= 1;
            line[len] = 0;
        }
        if (len == 0) continue;

        if (hasPrefix(&line, "COMMIT:")) {
            if (current.count > 0) {
                pushCommit(&commits, &count, &cap, current);
                current = std.mem.zeroes(Commit);
            }
            // Parse the unix timestamp from "COMMIT:<hash>:<unix_epoch>".
            // Older callers / stripped-down git output without %ct land on 0.
            const hash_end: CStr = c.strchr(@as(CStr, &line) + "COMMIT:".len, ':');
            if (hash_end != null) {
                current.timestamp = c.strtoll(hash_end + 1, null, 10);
            }
            continue;
        }

        if (cbm_is_trackable_file(&line)) {
            commitAddFile(&current, &line);
        }
    }
    if (current.count > 0) {
        pushCommit(&commits, &count, &cap, current);
    } else {
        commitFree(&current);
    }

    _ = c.cbm_pclose(fp);
    out.* = commits;
    out_count.* = count;
    return 0;
}

/// Callback to free hash table entries.
fn freeCounter(key: CStr, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = ud;
    c.free(@constCast(key));
    c.free(val);
}

// ── Standalone coupling computation (testable) ──────────────────────

/// Context for collectCouplingCb callback.
const CollectCouplingCtx = struct {
    file_counts: ?*HashTable,
    pair_timestamps: ?*HashTable, // pair_key → long long*: max commit ts
    out: [*c]Coupling,
    out_count: c_int,
    max_out: c_int,
};

fn intAt(table: ?*HashTable, key: CStr) ?*c_int {
    return @ptrCast(@alignCast(c.cbm_ht_get(table, key)));
}

fn timestampAt(table: ?*HashTable, key: CStr) ?*c_longlong {
    return @ptrCast(@alignCast(c.cbm_ht_get(table, key)));
}

/// A malloc'd int holding `value`, as the hash tables here store them.
fn newInt(value: c_int) *c_int {
    const nv: *c_int = interop.mallocN(c_int, 1);
    nv.* = value;
    return nv;
}

fn collectCouplingCb(pair_key: CStr, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    const cctx: *CollectCouplingCtx = @ptrCast(@alignCast(ud));
    const co_count = @as(*c_int, @ptrCast(@alignCast(val))).*;
    if (co_count < GH_MIN_COMMITS) return;
    if (cctx.out_count >= cctx.max_out) return;

    const sep: CStr = c.strchr(pair_key, 0x01);
    if (sep == null) return;
    const la = util.ptrDiff(sep, pair_key);
    const file_b: CStr = sep + 1;

    var file_a_buf: [c.CBM_SZ_512]u8 = undefined;
    if (la >= file_a_buf.len) return;
    @memcpy(file_a_buf[0..la], pair_key[0..la]);
    file_a_buf[la] = 0;

    const count_a = intAt(cctx.file_counts, &file_a_buf) orelse return;
    const count_b = intAt(cctx.file_counts, file_b) orelse return;

    const min_total = @min(count_a.*, count_b.*);
    if (min_total == 0) return;

    const score = @as(f64, @floatFromInt(co_count)) / @as(f64, @floatFromInt(min_total));
    if (score < MIN_COUPLING_SCORE) return;

    const cc = util.at(cctx.out, ix(cctx.out_count));
    cctx.out_count += 1;
    _ = c.snprintf(&cc.file_a, cc.file_a.len, "%s", @as(CStr, &file_a_buf));
    _ = c.snprintf(&cc.file_b, cc.file_b.len, "%s", file_b);
    cc.co_change_count = co_count;
    cc.coupling_score = score;
    const ts = timestampAt(cctx.pair_timestamps, pair_key);
    cc.last_co_change = if (ts) |t| t.* else 0;
}

pub export fn cbm_compute_change_coupling(
    commits: [*c]const c.cbm_commit_files_t,
    commit_count: c_int,
    out: [*c]Coupling,
    max_out: c_int,
) c_int {
    const file_counts = c.cbm_ht_create(c.CBM_SZ_1K);
    const pair_counts = c.cbm_ht_create(c.CBM_SZ_2K);
    // Parallel table mapping pair_key → max commit timestamp seen for that
    // pair, so the resulting edge can carry last_co_change. The pair_counts
    // table consumes its key on insert; pair_timestamps gets its own copy.
    const pair_timestamps = c.cbm_ht_create(c.CBM_SZ_2K);

    var ci: usize = 0;
    while (ci < commit_count) : (ci += 1) {
        const commit = &commits[ci];
        if (commit.count > GH_MAX_FILES) continue;
        const n = ix(commit.count);

        for (0..n) |i| {
            if (intAt(file_counts, commit.files[i])) |val| {
                val.* += 1;
            } else {
                _ = c.cbm_ht_set(file_counts, c.strdup(commit.files[i]), newInt(1));
            }
        }

        for (0..n) |i| {
            for (i + 1..n) |j| {
                var a: CStr = commit.files[i];
                var b: CStr = commit.files[j];
                if (c.strcmp(a, b) > 0) {
                    const t = a;
                    a = b;
                    b = t;
                }
                const la = c.strlen(a);
                const lb = c.strlen(b);
                const pk_len = la + 1 + lb + 1;
                const pk: MutStr = @ptrCast(c.malloc(pk_len));
                @memcpy(pk[0..la], a[0..la]);
                pk[la] = 0x01;
                @memcpy(pk[la + 1 ..][0 .. lb + 1], b[0 .. lb + 1]);

                if (intAt(pair_counts, pk)) |val| {
                    val.* += 1;
                    if (timestampAt(pair_timestamps, pk)) |ts| {
                        if (commit.timestamp > ts.*) ts.* = commit.timestamp;
                    }
                    c.free(pk);
                } else {
                    // pair_counts takes ownership of pk; pair_timestamps
                    // needs its own copy.
                    const pk2: MutStr = @ptrCast(c.malloc(pk_len));
                    @memcpy(pk2[0..pk_len], pk[0..pk_len]);
                    _ = c.cbm_ht_set(pair_counts, pk, newInt(1));
                    const nts: *c_longlong = interop.mallocN(c_longlong, 1);
                    nts.* = commit.timestamp;
                    _ = c.cbm_ht_set(pair_timestamps, pk2, nts);
                }
            }
        }
    }

    var cctx: CollectCouplingCtx = .{
        .file_counts = file_counts,
        .pair_timestamps = pair_timestamps,
        .out = out,
        .out_count = 0,
        .max_out = max_out,
    };
    c.cbm_ht_foreach(pair_counts, collectCouplingCb, &cctx);

    c.cbm_ht_foreach(pair_counts, freeCounter, null);
    c.cbm_ht_free(pair_counts);
    c.cbm_ht_foreach(pair_timestamps, freeCounter, null);
    c.cbm_ht_free(pair_timestamps);
    c.cbm_ht_foreach(file_counts, freeCounter, null);
    c.cbm_ht_free(file_counts);

    return cctx.out_count;
}

// ── Split pass: compute (I/O-bound) + apply (gbuf writes) ───────────

/// Pre-computed coupling result buffer for fused post-pass parallelism.
const MAX_COUPLINGS = 8192;
const MAX_FILE_TEMPORAL = 16384;

/// Compute change couplings without touching the graph buffer.
/// Can run on a separate thread while other passes use the gbuf.
pub export fn cbm_pipeline_githistory_compute(repo_path: CStr, result: *c.cbm_githistory_result_t) c_int {
    result.couplings = null;
    result.count = 0;
    result.commit_count = 0;
    result.file_temporal = null;
    result.file_temporal_count = 0;

    var commits: [*c]Commit = null;
    var commit_count: c_int = 0;
    const rc = parseGitLog(repo_path, &commits, &commit_count);
    if (rc != 0 or commit_count == 0) {
        c.free(commits);
        return 0;
    }
    const n_commits = ix(commit_count);

    result.commit_count = commit_count;

    // Convert to testable format
    const cf = interop.callocN(c.cbm_commit_files_t, n_commits);
    if (cf == null) {
        for (0..n_commits) |i| commitFree(util.at(commits, i));
        c.free(commits);
        return 0;
    }
    for (0..n_commits) |i| {
        cf[i].files = commits[i].files;
        cf[i].count = commits[i].count;
        cf[i].timestamp = commits[i].timestamp;
    }

    const couplings = interop.mallocN(Coupling, MAX_COUPLINGS);
    const coupling_count = cbm_compute_change_coupling(cf, commit_count, couplings, MAX_COUPLINGS);

    // Per-file temporal aggregation: change_count + last_modified.
    // Single hash-table pass over the same commit set used for coupling so
    // we don't re-scan history. NULL on OOM is fine — the caller still
    // gets the couplings.
    const ft_arr = interop.mallocN(FileTemporal, MAX_FILE_TEMPORAL);
    if (ft_arr != null) {
        var ft_count: c_int = 0;
        const file_idx = c.cbm_ht_create(c.CBM_SZ_1K);
        for (0..n_commits) |i| {
            if (cf[i].count > GH_MAX_FILES) continue;
            for (0..ix(cf[i].count)) |f| {
                const fp: CStr = cf[i].files[f];
                if (intAt(file_idx, fp)) |idx| {
                    const entry = util.at(ft_arr, ix(idx.*));
                    entry.change_count += 1;
                    if (cf[i].timestamp > entry.last_modified) {
                        entry.last_modified = cf[i].timestamp;
                    }
                } else if (ft_count < MAX_FILE_TEMPORAL) {
                    const new_idx = ft_count;
                    ft_count += 1;
                    const entry = util.at(ft_arr, ix(new_idx));
                    _ = c.snprintf(&entry.file_path, entry.file_path.len, "%s", fp);
                    entry.change_count = 1;
                    entry.last_modified = cf[i].timestamp;
                    _ = c.cbm_ht_set(file_idx, c.strdup(fp), newInt(new_idx));
                }
            }
        }
        c.cbm_ht_foreach(file_idx, freeCounter, null);
        c.cbm_ht_free(file_idx);
        result.file_temporal = ft_arr;
        result.file_temporal_count = ft_count;
    }

    c.free(cf);
    for (0..n_commits) |i| commitFree(util.at(commits, i));
    c.free(commits);

    result.couplings = couplings;
    result.count = coupling_count;
    return 0;
}

/// Apply pre-computed couplings to the graph buffer (must be on main thread).
pub export fn cbm_pipeline_githistory_apply(ctx: *c.cbm_pipeline_ctx_t, result: *const c.cbm_githistory_result_t) c_int {
    @setEvalBranchQuota(10000);
    var edge_count: c_int = 0;

    for (0..ix(result.count)) |i| {
        const cc = util.at(result.couplings, i);

        const qn_a = c.cbm_pipeline_fqn_compute(ctx.project_name, &cc.file_a, "__file__");
        const qn_b = c.cbm_pipeline_fqn_compute(ctx.project_name, &cc.file_b, "__file__");

        const node_a: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, qn_a);
        const node_b: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, qn_b);

        c.free(qn_a);
        c.free(qn_b);

        const a = node_a orelse continue;
        const b = node_b orelse continue;
        if (a.id == b.id) continue;

        var props: [c.CBM_SZ_128]u8 = undefined;
        _ = c.snprintf(
            &props,
            props.len,
            "{\"co_changes\":%d,\"coupling_score\":%.2f,\"last_co_change\":%lld}",
            cc.co_change_count,
            cc.coupling_score,
            cc.last_co_change,
        );

        _ = c.cbm_gbuf_insert_edge(ctx.gbuf, a.id, b.id, "FILE_CHANGES_WITH", &props);
        edge_count += 1;
    }

    // Apply per-file temporal metadata to existing File nodes so callers
    // can query change_count / last_modified for hotspot analysis. The keys
    // are merged into the node's properties: the coverage stamp written
    // after extraction lives there too, and replacing the blob dropped it.
    for (0..ix(result.file_temporal_count)) |i| {
        const ft = util.at(result.file_temporal, i);
        const file_path: CStr = &ft.file_path;
        const qn = c.cbm_pipeline_fqn_compute(ctx.project_name, file_path, "__file__");
        const node: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_qn(ctx.gbuf, qn)) orelse {
            c.free(qn);
            continue;
        };

        var base: CStr = c.strrchr(file_path, '/');
        base = if (base != null) base + 1 else file_path;
        const ext: CStr = c.strrchr(base, '.');

        const old: CStr = if (node.properties_json != null) node.properties_json else "{}";
        const odoc = c.yyjson_read(old, c.strlen(old), 0);
        const doc = c.yyjson_mut_doc_new(null);
        const root = if (odoc != null and c.yyjson_is_obj(c.yyjson_doc_get_root(odoc)))
            c.yyjson_val_mut_copy(doc, c.yyjson_doc_get_root(odoc))
        else
            c.yyjson_mut_obj(doc);
        c.yyjson_mut_doc_set_root(doc, root);
        _ = c.yyjson_mut_obj_remove_key(root, "extension");
        _ = c.yyjson_mut_obj_remove_key(root, "last_modified");
        _ = c.yyjson_mut_obj_remove_key(root, "change_count");
        _ = c.yyjson_mut_obj_add_strcpy(doc, root, "extension", if (ext != null) ext else "");
        _ = c.yyjson_mut_obj_add_sint(doc, root, "last_modified", ft.last_modified);
        _ = c.yyjson_mut_obj_add_int(doc, root, "change_count", ft.change_count);
        const props = c.yyjson_mut_write(doc, 0, null);
        c.yyjson_mut_doc_free(doc);
        c.yyjson_doc_free(odoc);
        if (props != null) {
            _ = c.cbm_gbuf_set_node_props(ctx.gbuf, qn, props);
            c.free(props);
        }
        c.free(qn);
    }

    return edge_count;
}

// ── Main pass (original serial interface) ───────────────────────────

pub export fn cbm_pipeline_pass_githistory(ctx: *c.cbm_pipeline_ctx_t) c_int {
    interop.logInfo("pass.start", .{ @as(CStr, "pass"), @as(CStr, "githistory") });

    var result = std.mem.zeroes(c.cbm_githistory_result_t);
    _ = cbm_pipeline_githistory_compute(ctx.repo_path, &result);

    var edge_count: c_int = 0;
    if (result.count > 0 or result.file_temporal_count > 0) {
        edge_count = cbm_pipeline_githistory_apply(ctx, &result);
    }

    c.free(result.couplings);
    c.free(result.file_temporal);

    interop.logInfo("pass.done", .{
        @as(CStr, "pass"),    @as(CStr, "githistory"),
        @as(CStr, "commits"), @as(CStr, c.cbm_log_itoa(result.commit_count)),
        @as(CStr, "edges"),   @as(CStr, c.cbm_log_itoa(edge_count)),
    });
    return 0;
}
