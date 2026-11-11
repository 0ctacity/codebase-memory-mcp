//! pipeline_incremental.zig — Disk-based incremental re-indexing.
//!
//! Operates on the existing SQLite DB directly (not RAM-first graph buffer).
//! Compares file mtime+size against stored hashes to classify changed/unchanged.
//! Deletes changed files' nodes (edges cascade via ON DELETE CASCADE),
//! re-parses only changed files through passes into a temp graph buffer,
//! then merges new nodes/edges into the disk DB. Persists updated hashes.
//!
//! Called from pipeline.zig when a DB with stored hashes already exists.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const ptrDiff = util.ptrDiff;
const HashTable = c.CBMHashTable;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;
const Ctx = c.cbm_pipeline_ctx_t;
const FileInfo = c.cbm_file_info_t;
const FileHash = c.cbm_file_hash_t;
const StemMarks = c.cbm_stem_marks_t;
const Sha256 = c.cbm_sha256_backend_ctx;
const Timespec = c.struct_timespec;

fn logError(msg: interop.Str, kvs: anytype) void {
    interop.log(c.CBM_LOG_ERROR, msg, kvs);
}

/// (size_t)n * 2 as a hash table capacity, which cbm_ht_create takes as uint32_t.
fn htCap(n: c_int) u32 {
    return @truncate(@as(usize, @bitCast(@as(isize, n))) *% 2);
}

fn itoa(v: c_int) CStr {
    return c.cbm_log_itoa(v);
}

fn now(t: *Timespec) void {
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, t);
}

/// cbm_log_itoa of the milliseconds elapsed since `start`.
fn elapsedText(start: Timespec) CStr {
    return c.cbm_log_itoa(@intFromFloat(c.cbm_elapsed_ms(start)));
}

const MultisetDigest = extern struct {
    xor_words: [4]u64 = @splat(0),
    sum_words: [4]u64 = @splat(0),
    count: u64 = 0,
};

const DerivedState = extern struct {
    input: MultisetDigest = .{},
    config: MultisetDigest = .{},
    similarity: MultisetDigest = .{},
    semantic: MultisetDigest = .{},
};

fn derivedHashText(hash: *Sha256, text: CStr) void {
    const value: CStr = if (text != null) text else "";
    c.cbm_sha256_backend_update(hash, value, c.strlen(value));
    c.cbm_sha256_backend_update(hash, "\x00", 1);
}

fn derivedDigestAdd(set: *MultisetDigest, hash: *Sha256) void {
    var bytes: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    var words: [4]u64 = undefined;
    c.cbm_sha256_backend_final(hash, &bytes);
    @memcpy(std.mem.asBytes(&words), bytes[0..@sizeOf([4]u64)]);
    for (0..4) |i| {
        set.xor_words[i] ^= words[i];
        set.sum_words[i] +%= words[i];
    }
    set.count +%= 1;
}

fn derivedDigestEqual(left: *const MultisetDigest, right: *const MultisetDigest) bool {
    return left.count == right.count and
        std.mem.eql(u64, &left.xor_words, &right.xor_words) and
        std.mem.eql(u64, &left.sum_words, &right.sum_words);
}

const DerivedScan = struct {
    gbuf: ?*const Gbuf,
    state: *DerivedState,
};

fn derivedNodeVisitor(node: [*c]const Node, userdata: ?*anyopaque) callconv(.c) void {
    const scan: *DerivedScan = @ptrCast(@alignCast(userdata));
    var hash: Sha256 = undefined;
    c.cbm_sha256_backend_init(&hash);
    derivedHashText(&hash, "node");
    derivedHashText(&hash, node.*.label);
    derivedHashText(&hash, node.*.name);
    derivedHashText(&hash, node.*.qualified_name);
    derivedHashText(&hash, node.*.file_path);
    c.cbm_sha256_backend_update(&hash, &node.*.start_line, @sizeOf(@TypeOf(node.*.start_line)));
    c.cbm_sha256_backend_update(&hash, &node.*.end_line, @sizeOf(@TypeOf(node.*.end_line)));
    derivedHashText(&hash, node.*.properties_json);
    derivedDigestAdd(&scan.state.input, &hash);
}

fn derivedEdgeVisitor(edge: [*c]const Edge, userdata: ?*anyopaque) callconv(.c) void {
    const scan: *DerivedScan = @ptrCast(@alignCast(userdata));
    var target = &scan.state.input;
    if (streq(edge.*.type, "CONFIGURES")) {
        target = &scan.state.config;
    } else if (streq(edge.*.type, "SIMILAR_TO")) {
        target = &scan.state.similarity;
    } else if (streq(edge.*.type, "SEMANTICALLY_RELATED")) {
        target = &scan.state.semantic;
    }
    const source: ?*const Node = c.cbm_gbuf_find_by_id(scan.gbuf, edge.*.source_id);
    const destination: ?*const Node = c.cbm_gbuf_find_by_id(scan.gbuf, edge.*.target_id);
    var hash: Sha256 = undefined;
    c.cbm_sha256_backend_init(&hash);
    derivedHashText(&hash, "edge");
    derivedHashText(&hash, if (source) |s| s.qualified_name else null);
    derivedHashText(&hash, edge.*.type);
    derivedHashText(&hash, if (destination) |d| d.qualified_name else null);
    derivedHashText(&hash, edge.*.properties_json);
    derivedDigestAdd(target, &hash);
}

fn derivedState(gbuf: ?*const Gbuf) DerivedState {
    var state: DerivedState = .{};
    var scan: DerivedScan = .{ .gbuf = gbuf, .state = &state };
    c.cbm_gbuf_foreach_node(gbuf, derivedNodeVisitor, &scan);
    c.cbm_gbuf_foreach_edge(gbuf, derivedEdgeVisitor, &scan);
    return state;
}

// ── File classification ─────────────────────────────────────────────

/// Classify discovered files against stored hashes using mtime+size.
/// Returns a boolean array: changed[i] = true if files[i] needs re-parsing.
/// Caller must free the returned array.
fn classifyFiles(
    files: [*c]FileInfo,
    file_count: c_int,
    stored: [*c]FileHash,
    stored_count: c_int,
    out_changed: *c_int,
    out_unchanged: *c_int,
) [*c]bool {
    const changed = interop.callocN(bool, ix(file_count));
    if (changed == null) return null;

    var n_changed: c_int = 0;
    var n_unchanged: c_int = 0;

    // Build lookup: rel_path -> stored hash
    const ht = c.cbm_ht_create(if (stored_count > 0) htCap(stored_count) else c.CBM_SZ_64);
    for (0..ix(stored_count)) |i| {
        _ = c.cbm_ht_set(ht, stored[i].rel_path, util.at(stored, i));
    }

    for (0..ix(file_count)) |i| {
        const h: ?*FileHash = @ptrCast(@alignCast(c.cbm_ht_get(ht, files[i].rel_path)));
        if (h == null) {
            // New file
            changed[i] = true;
            n_changed += 1;
            continue;
        }

        var st: c.struct_stat = undefined;
        if (c.stat(files[i].path, &st) != 0) {
            changed[i] = true;
            n_changed += 1;
            continue;
        }

        if (c.cbm_stat_mtime_ns(&st) != h.?.mtime_ns or st.st_size != h.?.size) {
            changed[i] = true;
            n_changed += 1;
        } else {
            n_unchanged += 1;
        }
    }

    c.cbm_ht_free(ht);
    out_changed.* = n_changed;
    out_unchanged.* = n_unchanged;
    return changed;
}

/// Classify stored files that are absent from current discovery. Returns the
/// count of truly-deleted files (output via out_deleted) and ALSO collects
/// mode-skipped files into out_mode_skipped (caller frees both).
///
/// A stored file is classified as:
///   - "deleted"      — `stat()` returns ENOENT or ENOTDIR. Its nodes will
///                       be purged and its hash row dropped.
///   - "mode-skipped" — `stat()` succeeds. The file exists on disk but the
///                       current discovery pass didn't visit it (e.g. excluded
///                       by FAST_SKIP_DIRS in fast/moderate mode). Its nodes
///                       must be preserved AND its hash row must be carried
///                       forward into the new DB so subsequent reindexes can
///                       still see it as "known" rather than treating it as
///                       new-or-deleted.
///
/// Without this distinction, a fast-mode reindex after a full-mode index
/// would silently purge every file under `tools/`, `scripts/`, `bin/`,
/// `build/`, `docs/`, `__tests__/`, etc. — see task
/// claude-connectors/codebase-memory-index-repository-is-destructive-...
/// and the 2026-04-13 Skyline incident (packages/mcp/src/tools/ vanished
/// from a live graph mid-session).
///
/// Mode-skipped hash preservation is the second half of the additive-merge
/// contract: dump_and_persist re-upserts these hash rows so the next reindex
/// can correctly detect a real on-disk deletion of a mode-skipped file (as
/// opposed to seeing it as "never existed" → noop → orphaned graph nodes).
///
/// Fail-safe rules (preserve nodes on uncertainty):
///   - repo_path NULL → log error and preserve everything (return 0
///     deletions, empty mode_skipped). The caller contract is that
///     repo_path is required; a NULL means a misconfigured pipeline,
///     not a deletion signal.
///   - snprintf truncation (combined path ≥ CBM_SZ_4K) → preserve. We can't
///     reliably stat a truncated path. Treat as mode-skipped.
///   - stat() errno != ENOENT/ENOTDIR (EACCES, EIO, ELOOP, transient NFS,
///     etc.) → preserve. The file may exist; we just can't see it right now.
///     Treat as mode-skipped.
///
/// Note: we use stat() (not lstat()) on purpose. A symlink whose target was
/// deleted should be classified as deleted from the indexer's perspective
/// because the indexer follows symlinks during discovery — a stale symlink
/// has no source to parse.
fn findDeletedFiles(
    repo_path: CStr,
    files: [*c]FileInfo,
    file_count: c_int,
    stored: [*c]FileHash,
    stored_count: c_int,
    out_deleted: *[*c]MutStr,
    out_mode_skipped: *[*c]FileHash,
    out_mode_skipped_count: *c_int,
) c_int {
    out_deleted.* = null;
    out_mode_skipped.* = null;
    out_mode_skipped_count.* = 0;

    if (repo_path == null) {
        // Misconfigured pipeline. Preserve everything rather than risk
        // silently re-introducing the destructive overwrite this function
        // was rewritten to prevent.
        logError("incremental.err", .{ "msg", "find_deleted_files_null_repo_path" });
        return 0;
    }

    const current = c.cbm_ht_create(htCap(file_count));
    for (0..ix(file_count)) |i| {
        _ = c.cbm_ht_set(current, files[i].rel_path, util.at(files, i));
    }

    var del_count: c_int = 0;
    var del_cap: c_int = c.CBM_SZ_64;
    var deleted = interop.mallocN(MutStr, ix(del_cap));
    if (deleted == null) {
        logError("incremental.err", .{ "msg", "find_deleted_files_oom" });
        c.cbm_ht_free(current);
        return 0;
    }

    var ms_count: c_int = 0;
    var ms_cap: c_int = c.CBM_SZ_64;
    var mode_skipped = interop.mallocN(FileHash, ix(ms_cap));
    if (mode_skipped == null) {
        logError("incremental.err", .{ "msg", "find_deleted_files_oom_ms" });
        c.free(@ptrCast(deleted));
        c.cbm_ht_free(current);
        return 0;
    }

    for (0..ix(stored_count)) |i| {
        if (c.cbm_ht_get(current, stored[i].rel_path) != null) continue; // still visited by current pass
        // Not in current discovery — check if it's truly deleted or just
        // mode-skipped (excluded by FAST_SKIP_DIRS etc.).
        var preserve = false;
        var abs_path: [c.CBM_SZ_4K]u8 = undefined;
        const n = c.snprintf(&abs_path, abs_path.len, "%s/%s", repo_path, stored[i].rel_path);
        if (n < 0 or n >= @as(c_int, abs_path.len)) {
            // Truncation or encoding error — can't reliably stat. Preserve.
            interop.logWarn("incremental.path_truncated", .{ "rel_path", stored[i].rel_path });
            preserve = true;
        } else {
            var st: c.struct_stat = undefined;
            if (c.stat(&abs_path, &st) == 0) {
                // File exists on disk — mode-skipped, not deleted.
                preserve = true;
            } else {
                const err = std.c._errno().*;
                if (err != c.ENOENT and err != c.ENOTDIR) {
                    // Transient or permission error — fail safe by preserving.
                    // EACCES, EIO, ELOOP, ENAMETOOLONG, etc.
                    interop.logWarn("incremental.stat_uncertain", .{
                        "rel_path", stored[i].rel_path,
                        "errno",    itoa(std.c._errno().*),
                    });
                    preserve = true;
                }
            }
        }

        if (preserve) {
            // Carry forward the existing hash row so subsequent reindexes
            // can correctly classify this file.
            if (ms_count >= ms_cap) {
                ms_cap *= 2;
                const tmp = interop.reallocN(FileHash, mode_skipped, ix(ms_cap));
                if (tmp == null) {
                    logError("incremental.err", .{ "msg", "find_deleted_files_realloc_oom_ms" });
                    break;
                }
                mode_skipped = tmp;
            }
            const rp: MutStr = c.strdup(stored[i].rel_path);
            const sh: MutStr = if (stored[i].sha256 != null) c.strdup(stored[i].sha256) else null;
            if (rp == null or (stored[i].sha256 != null and sh == null)) {
                // OOM mid-record. Drop this entry rather than persist a
                // row with a NULL rel_path that would silently fail the
                // NOT NULL constraint in upsert and reintroduce the
                // orphaned-node bug.
                logError("incremental.err", .{
                    "msg",      "find_deleted_files_strdup_oom",
                    "rel_path", stored[i].rel_path,
                });
                c.free(rp);
                c.free(sh);
                break;
            }
            const ms = &mode_skipped[ix(ms_count)];
            ms.project = null; // unused by upsert API
            ms.rel_path = rp;
            ms.sha256 = sh;
            ms.mtime_ns = stored[i].mtime_ns;
            ms.size = stored[i].size;
            ms_count += 1;
            continue;
        }

        // File is truly gone — record for purge.
        if (del_count >= del_cap) {
            del_cap *= 2;
            const tmp = interop.reallocN(MutStr, deleted, ix(del_cap));
            if (tmp == null) {
                logError("incremental.err", .{ "msg", "find_deleted_files_realloc_oom" });
                break;
            }
            deleted = tmp;
        }
        deleted[ix(del_count)] = c.strdup(stored[i].rel_path);
        del_count += 1;
    }

    c.cbm_ht_free(current);
    out_deleted.* = deleted;
    out_mode_skipped.* = mode_skipped;
    out_mode_skipped_count.* = ms_count;
    return del_count;
}

/// Free a mode_skipped array allocated by findDeletedFiles.
fn freeModeSkipped(ms: [*c]FileHash, count: c_int) void {
    if (ms == null) return;
    for (0..ix(count)) |i| {
        c.free(@constCast(ms[i].rel_path));
        c.free(@constCast(ms[i].sha256));
    }
    c.free(ms);
}

/// Cuts a path at its last '/', or empties it when it has none.
fn cutLastSegment(path: MutStr) void {
    const slash: MutStr = c.strrchr(path, '/');
    if (slash != null) slash[0] = 0 else path[0] = 0;
}

/// Add the structural nodes for a newly discovered path before definition
/// extraction. This mirrors the relevant part of the full structure pass: a
/// Folder must exist before a directory-based Module definition arrives, or
/// the Module wins the shared qualified-name slot and the graph diverges.
fn incrAddChangedFileStructure(gbuf: ?*Gbuf, project: CStr, marks: ?*const StemMarks, file: ?*const FileInfo) void {
    const f = file orelse return;
    if (gbuf == null or project == null or f.rel_path == null) return;
    const rel_path: CStr = f.rel_path;
    const slash: CStr = c.strrchr(rel_path, '/');
    const basename: CStr = if (slash != null) slash + 1 else rel_path;
    const extension: CStr = c.strrchr(basename, '.');
    var properties: [c.CBM_SZ_256]u8 = undefined;
    _ = c.snprintf(
        &properties,
        properties.len,
        "{\"extension\":\"%s\"}",
        @as(CStr, if (extension != null) extension else ""),
    );
    const file_qn: MutStr = c.cbm_pipeline_fqn_compute(project, rel_path, "__file__");
    if (file_qn == null) return;
    _ = c.cbm_gbuf_upsert_node(gbuf, "File", basename, file_qn, rel_path, 0, 0, &properties);

    var branches: [*c][*c]const Node = null;
    var branch_count: c_int = 0;
    var branch_qn: CStr = project;
    if (c.cbm_gbuf_find_by_label(gbuf, "Branch", &branches, &branch_count) == 0 and
        branch_count > 0 and branches[0].*.qualified_name != null)
    {
        branch_qn = branches[0].*.qualified_name;
    }

    const dir: MutStr = c.strdup(rel_path);
    if (dir == null) {
        c.free(file_qn);
        return;
    }
    cutLastSegment(dir);
    var file_parent_buf: [c.CBM_SZ_8K]u8 = undefined;
    const file_parent_qn: CStr = if (dir[0] != 0)
        c.cbm_stem_marks_folder_qn(marks, project, dir, &file_parent_buf, file_parent_buf.len)
    else
        null;

    const walk = dir;
    while (walk[0] != 0) {
        var folder_buf: [c.CBM_SZ_8K]u8 = undefined;
        const folder_qn: CStr = c.cbm_stem_marks_folder_qn(marks, project, walk, &folder_buf, folder_buf.len);
        if (folder_qn == null) break;
        var folder_name: CStr = c.strrchr(walk, '/');
        folder_name = if (folder_name != null) folder_name + 1 else walk;
        _ = c.cbm_gbuf_upsert_node(gbuf, "Folder", folder_name, folder_qn, walk, 0, 0, "{}");

        const parent_dir: MutStr = c.strdup(walk);
        if (parent_dir == null) break;
        cutLastSegment(parent_dir);
        var parent_buf: [c.CBM_SZ_8K]u8 = undefined;
        const parent_qn: CStr = if (parent_dir[0] != 0)
            c.cbm_stem_marks_folder_qn(marks, project, parent_dir, &parent_buf, parent_buf.len)
        else
            branch_qn;
        const folder: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, folder_qn);
        const parent: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, parent_qn);
        if (folder != null and parent != null) {
            _ = c.cbm_gbuf_insert_edge(gbuf, parent.?.id, folder.?.id, "CONTAINS_FOLDER", "{}");
        }
        c.free(parent_dir);

        cutLastSegment(walk);
    }

    const file_node: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, file_qn);
    const parent_qn: CStr = if (file_parent_qn != null) file_parent_qn else branch_qn;
    const parent: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, parent_qn);
    if (file_node != null and parent != null) {
        _ = c.cbm_gbuf_insert_edge(gbuf, parent.?.id, file_node.?.id, "CONTAINS_FILE", "{}");
    }
    c.free(dir);
    c.free(file_qn);
}

/// True when the stem QN or a directory QN of rel yields on a different side
/// (CBM_STEM_*) in the two file sets.
fn incrStemSideDiffers(now_marks: *const StemMarks, before: *const StemMarks, a: *c.CBMArena, project: CStr, rel: CStr) bool {
    const stem: MutStr = c.cbm_fqn_module(a, project, rel);
    if (stem != null and c.cbm_stem_marks_side(now_marks, stem) != c.cbm_stem_marks_side(before, stem)) {
        return true;
    }
    var s: CStr = c.strchr(rel, '/');
    while (s != null) : (s = c.strchr(s + 1, '/')) {
        const dir: MutStr = c.cbm_arena_strndup(a, rel, ptrDiff(s, rel));
        const fqn: MutStr = if (dir != null) c.cbm_fqn_folder(a, project, dir) else null;
        if (fqn != null and c.cbm_stem_marks_side(now_marks, fqn) != c.cbm_stem_marks_side(before, fqn)) {
            return true;
        }
    }
    return false;
}

/// The file set of the stored generation: the discovered files it already
/// held, plus the deleted ones, which take the language their name gives.
/// Counts the discovered files it did not hold in *added.
fn incrPreviousFiles(
    files: [*c]const FileInfo,
    file_count: c_int,
    stored_paths: ?*HashTable,
    deleted: [*c]MutStr,
    deleted_count: c_int,
    out_count: *c_int,
    added: *c_int,
) [*c]FileInfo {
    const before = interop.callocN(FileInfo, ix(file_count) + ix(deleted_count) + 1);
    var n: usize = 0;
    for (0..ix(file_count)) |i| {
        if (files[i].rel_path != null and c.cbm_ht_get(stored_paths, files[i].rel_path) != null) {
            if (before != null) {
                before[n] = files[i];
                n += 1;
            }
        } else {
            added.* += 1;
        }
    }
    if (before != null) for (0..ix(deleted_count)) |i| {
        const base: CStr = c.strrchr(deleted[i], '/');
        before[n] = std.mem.zeroes(FileInfo);
        before[n].rel_path = deleted[i];
        before[n].language = c.cbm_language_for_filename(if (base != null) base + 1 else deleted[i]);
        n += 1;
    };
    out_count.* = @intCast(n);
    return before;
}

/// Whether an added or deleted file yields on a different side of a stem
/// collision in `now` than in `prev`.
fn incrStemSidesMoved(
    project: CStr,
    files: [*c]const FileInfo,
    file_count: c_int,
    stored_paths: ?*HashTable,
    now_marks: *const StemMarks,
    prev: *const StemMarks,
    deleted: [*c]MutStr,
    deleted_count: c_int,
) bool {
    var a: c.CBMArena = undefined;
    c.cbm_arena_init(&a);
    var moved = false;
    for (0..ix(file_count)) |i| {
        if (moved) break;
        if (files[i].rel_path != null and c.cbm_ht_get(stored_paths, files[i].rel_path) == null) {
            moved = incrStemSideDiffers(now_marks, prev, &a, project, files[i].rel_path);
        }
    }
    for (0..ix(deleted_count)) |i| {
        if (moved) break;
        moved = incrStemSideDiffers(now_marks, prev, &a, project, deleted[i]);
    }
    c.cbm_arena_destroy(&a);
    return moved;
}

/// Whether the added and deleted files change a stem collision
/// (cbm_stem_marks_t): one appears or disappears, or the yielding side flips
/// because a Java/Go package gains its first or loses its last file. The
/// unchanged files keep the spelling of the stored generation, so only a full
/// index re-spells them. `now` holds the discovered files; the previous set is
/// the stored paths, where a deleted file takes the language its name gives.
/// A changed file can only move the sides of its own stem QN and of the
/// directories above it, so only those are compared.
fn incrStemMarksChanged(
    project: CStr,
    files: [*c]const FileInfo,
    file_count: c_int,
    now_marks: *const StemMarks,
    stored: [*c]FileHash,
    stored_count: c_int,
    deleted: [*c]MutStr,
    deleted_count: c_int,
) bool {
    const stored_paths = c.cbm_ht_create(if (stored_count > 0) htCap(stored_count) else c.CBM_SZ_64);
    for (0..ix(stored_count)) |i| {
        if (stored[i].rel_path != null) _ = c.cbm_ht_set(stored_paths, stored[i].rel_path, util.at(stored, i));
    }
    var before_count: c_int = 0;
    var added: c_int = 0;
    const before = incrPreviousFiles(files, file_count, stored_paths, deleted, deleted_count, &before_count, &added);
    var changed = false;
    if (added > 0 or deleted_count > 0) {
        // Without the previous set the comparison cannot be made: rebuild.
        changed = true;
        if (before != null) {
            var prev: StemMarks = undefined;
            c.cbm_stem_marks_init(&prev, project, before, before_count);
            changed = incrStemSidesMoved(project, files, file_count, stored_paths, now_marks, &prev, deleted, deleted_count);
            c.cbm_stem_marks_free(&prev);
        }
    }
    c.free(before);
    c.cbm_ht_free(stored_paths);
    return changed;
}

// ── Inbound cross-file edge preservation (incremental correctness) ──
//
// The purge step (cbm_gbuf_delete_by_file) removes a changed file's nodes,
// and the cascade then drops every edge referencing them — INCLUDING inbound
// edges whose source lives in an UNCHANGED file (e.g. StudyService.grade ->
// SM2.review, or a Folder -> File containment edge). Because incremental only
// re-parses the changed files, the resolution passes never regenerate those
// inbound edges, so the graph silently loses cross-file CALLS / USAGE /
// CONTAINS_FILE / INHERITS / ... edges on every edit and diverges from a
// clean full reindex (which resolves every file).
//
// Fix: snapshot inbound cross-file edges into changed or deleted files BEFORE
// the purge. Re-run CALLS resolution for unchanged callers after re-extraction,
// then re-link non-CALLS edges by exact qualified-name target. Notes:
//   - Only edges whose target is in a changed file and whose source is NOT
//     are snapshotted; edges out of a changed file are regenerated when that
//     file is re-resolved.
//   - Edge types recomputed wholesale by post-passes (SIMILAR_TO,
//     SEMANTICALLY_RELATED) are skipped — re-linking a stale snapshot could
//     add edges a full reindex would not produce.
//   - cbm_gbuf_insert_edge dedups, so re-linking an edge the resolver already
//     recreated is a harmless no-op.
//   - A deleted target is dropped. A renamed CALLS edge is rebuilt from its
//     unchanged caller source, preserving the resolver's current confidence
//     and strategy rather than copying stale edge properties.

const SavedEdge = extern struct {
    source_qn: MutStr,
    target_qn: MutStr,
    target_name: MutStr,
    target_label: MutStr,
    type: MutStr,
    props: MutStr,
};

const EdgeCapture = struct {
    gbuf: ?*Gbuf,
    /// rel_path -> non-NULL sentinel (membership set)
    changed_paths: ?*HashTable,
    items: [*c]SavedEdge = null,
    count: c_int = 0,
    cap: c_int = 0,
};

/// Edge types that must NOT be re-linked from the pre-purge snapshot, because a
/// full reindex (re)computes them via a pass whose result can differ from the
/// snapshot — restoring a stale copy could leave wrong properties or even an
/// edge a full reindex would not produce:
///   - SIMILAR_TO / SEMANTICALLY_RELATED / FILE_CHANGES_WITH / DATA_FLOWS:
///     rebuilt wholesale by the incremental post-passes (similarity /
///     semantic_edges / githistory / route matching) over a drifting corpus.
/// Structural containment edges are regenerated from changed paths before
/// extraction, so snapshots omit them rather than retaining a pre-rename
/// parent relationship.
fn incrEdgeTypeIsRecomputed(edge_type: CStr) bool {
    return edge_type != null and (streq(edge_type, "SIMILAR_TO") or
        streq(edge_type, "SEMANTICALLY_RELATED") or streq(edge_type, "FILE_CHANGES_WITH") or
        streq(edge_type, "DATA_FLOWS") or streq(edge_type, "CONTAINS_FILE") or
        streq(edge_type, "CONTAINS_FOLDER"));
}

/// cbm_gbuf_foreach_edge visitor: snapshot inbound cross-file edges into
/// changed files so they survive the purge and can be re-linked afterward.
fn incrCaptureInboundEdge(edge: [*c]const Edge, userdata: ?*anyopaque) callconv(.c) void {
    const cap: *EdgeCapture = @ptrCast(@alignCast(userdata));
    if (incrEdgeTypeIsRecomputed(edge.*.type)) return;
    const src: *const Node = c.cbm_gbuf_find_by_id(cap.gbuf, edge.*.source_id) orelse return;
    const tgt: *const Node = c.cbm_gbuf_find_by_id(cap.gbuf, edge.*.target_id) orelse return;
    if (src.qualified_name == null or tgt.qualified_name == null or src.file_path == null or
        tgt.file_path == null)
    {
        return;
    }
    // Keep only edges that the purge would orphan permanently: target is in a
    // changed file (its node is deleted + re-created), source is NOT (its file
    // is never re-parsed, so the resolver won't regenerate the edge).
    if (c.cbm_ht_get(cap.changed_paths, tgt.file_path) == null or
        c.cbm_ht_get(cap.changed_paths, src.file_path) != null)
    {
        return;
    }
    if (cap.count >= cap.cap) {
        const ncap: c_int = if (cap.cap > 0) cap.cap * 2 else c.CBM_SZ_64;
        const tmp = interop.reallocN(SavedEdge, cap.items, ix(ncap));
        if (tmp == null) {
            interop.logWarn("incremental.edge_snapshot_oom", .{ "captured", itoa(cap.count) });
            return; // best-effort: stop capturing, keep what we have
        }
        cap.items = tmp;
        cap.cap = ncap;
    }
    const s = util.at(cap.items, ix(cap.count));
    s.source_qn = c.strdup(src.qualified_name);
    s.target_qn = c.strdup(tgt.qualified_name);
    s.target_name = c.strdup(if (tgt.name != null) tgt.name else "");
    s.target_label = c.strdup(if (tgt.label != null) tgt.label else "");
    s.type = c.strdup(edge.*.type);
    s.props = c.strdup(if (edge.*.properties_json != null) edge.*.properties_json else "{}");
    if (s.source_qn == null or s.target_qn == null or s.target_name == null or
        s.target_label == null or s.type == null or s.props == null)
    {
        c.free(s.source_qn);
        c.free(s.target_qn);
        c.free(s.target_name);
        c.free(s.target_label);
        c.free(s.type);
        c.free(s.props);
        return;
    }
    cap.count += 1;
}

fn incrFindUniqueRenamedTarget(gbuf: ?*const Gbuf, saved: *const SavedEdge) ?*const Node {
    if (saved.target_name == null or saved.target_name[0] == 0 or saved.target_label == null or
        saved.target_label[0] == 0)
    {
        return null;
    }
    var nodes: [*c][*c]const Node = null;
    var count: c_int = 0;
    if (c.cbm_gbuf_find_by_name(gbuf, saved.target_name, &nodes, &count) != 0) return null;
    var candidate: ?*const Node = null;
    for (0..ix(count)) |i| {
        const node: *const Node = nodes[i];
        if (node.qualified_name == null or node.label == null or
            !streq(node.label, saved.target_label))
        {
            continue;
        }
        if (candidate != null) return null; // ambiguous: do not guess a rename target
        candidate = node;
    }
    return candidate;
}

fn incrCmpId(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x = @as(*const i64, @ptrCast(@alignCast(a))).*;
    const y = @as(*const i64, @ptrCast(@alignCast(b))).*;
    return @as(c_int, @intFromBool(x > y)) - @as(c_int, @intFromBool(x < y));
}

const IncrIdSet = extern struct {
    ids: [*c]const i64,
    count: c_int,
};

fn incrKeepUnlessSourceInSet(gb: ?*const Gbuf, edge: [*c]const Edge, userdata: ?*anyopaque) callconv(.c) bool {
    _ = gb;
    const set: *const IncrIdSet = @ptrCast(@alignCast(userdata));
    if (edge.*.properties_json != null and
        c.strstr(edge.*.properties_json, "\"env_access\"") != null)
    {
        return true;
    }
    return c.bsearch(&edge.*.source_id, set.ids, ix(set.count), @sizeOf(i64), incrCmpId) == null;
}

/// A path rename changes the callee's qualified name. Re-run the CALLS pass
/// for only unchanged source files that previously called a purged target, so
/// the normal resolver recalculates strategy/confidence against the new
/// registry instead of preserving stale edge properties.
fn incrRebuildSnapshotCallers(ctx: *Ctx, cap: *const EdgeCapture, files: [*c]const FileInfo, file_count: c_int) void {
    if (files == null or file_count <= 0) return;
    const callers = interop.callocN(FileInfo, ix(cap.count));
    const source_ids = interop.mallocN(i64, ix(cap.count));
    if (callers == null or source_ids == null) {
        c.free(callers);
        c.free(source_ids);
        return;
    }
    var caller_count: usize = 0;
    var source_count: usize = 0;
    for (0..ix(cap.count)) |i| {
        const saved = util.at(cap.items, i);
        // Re-resolve unchanged sources that previously emitted either CALLS
        // or resolver-owned CONFIGURES. CONFIGURES may be the only derived
        // edge from a source (for example an environment accessor), so
        // restricting this list to CALLS leaves those edges missing after a
        // changed-file purge.
        if (saved.type == null or (!streq(saved.type, "CALLS") and !streq(saved.type, "CONFIGURES"))) {
            continue;
        }
        const source: *const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, saved.source_qn) orelse continue;
        if (source.file_path == null) continue;
        source_ids[source_count] = source.id;
        source_count += 1;
        for (0..ix(file_count)) |f| {
            if (!streq(files[f].rel_path, source.file_path)) continue;
            var already_added = false;
            for (0..caller_count) |existing| {
                if (streq(callers[existing].rel_path, files[f].rel_path)) {
                    already_added = true;
                    break;
                }
            }
            if (!already_added) {
                callers[caller_count] = files[f];
                caller_count += 1;
            }
            break;
        }
    }
    // Drop these sources' CONFIGURES edges in one pass over the edge list:
    // deleting per captured edge rescanned every edge and rebuilt the edge
    // indexes each time, which made a one-file reindex slower than a full one.
    if (source_count > 0) {
        c.qsort(source_ids, source_count, @sizeOf(i64), incrCmpId);
        var set: IncrIdSet = .{ .ids = source_ids, .count = @intCast(source_count) };
        _ = c.cbm_gbuf_delete_edges_by_type_unless(ctx.gbuf, "CONFIGURES", incrKeepUnlessSourceInSet, &set);
    }
    if (caller_count > 0) {
        _ = c.cbm_parallel_resolve_calls(ctx, callers, @intCast(caller_count));
    }
    c.free(source_ids);
    c.free(callers);
}

/// Re-link snapshotted inbound edges to the freshly re-created target nodes.
/// Returns the number of edges re-linked.
fn incrRestoreInboundEdges(gbuf: ?*Gbuf, cap: *EdgeCapture) c_int {
    var restored: c_int = 0;
    for (0..ix(cap.count)) |i| {
        const s = util.at(cap.items, i);
        const src: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, s.source_qn);
        var tgt: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, s.target_qn);
        if (tgt == null and s.type != null and streq(s.type, "CALLS")) {
            continue; // rebuilt from source so resolver owns its properties
        }
        // Incremental runs skip cross-file LSP, so an unchanged source cannot
        // re-prove its CALL_REFERENCE. Keep the snapshot edge only while the
        // same QN is still a callable; never guess a rename.
        if (s.type != null and streq(s.type, "CALL_REFERENCE") and
            !c.cbm_pipeline_node_is_callable_target(tgt))
        {
            continue;
        }
        if (tgt == null) tgt = incrFindUniqueRenamedTarget(gbuf, s);
        if (src != null and tgt != null) {
            _ = c.cbm_gbuf_insert_edge(gbuf, src.?.id, tgt.?.id, s.type, s.props);
            restored += 1;
        }
    }
    return restored;
}

fn incrFreeEdgeCapture(cap: *EdgeCapture) void {
    for (0..ix(cap.count)) |i| {
        c.free(cap.items[i].source_qn);
        c.free(cap.items[i].target_qn);
        c.free(cap.items[i].target_name);
        c.free(cap.items[i].target_label);
        c.free(cap.items[i].type);
        c.free(cap.items[i].props);
    }
    c.free(cap.items);
    cap.items = null;
    cap.count = 0;
    cap.cap = 0;
}

// ── Registry seed visitor ───────────────────────────────────────────

/// Labels the full index seeds into the registry
/// (pass_parallel register_and_link_def — KEEP IN SYNC). Incremental re-resolution must see the
/// SAME symbol set, or it diverges from a clean full reindex: seeding extra
/// container nodes (File / Module / Folder / ...) lets a type usage like `Word`
/// resolve to the same-named Module node instead of the Class node. Only
/// callable / declared symbols belong in the registry.
fn incrLabelIsRegistrySymbol(label: CStr) bool {
    // Mirror pass_parallel registry seeding EXACTLY:
    // callables + every type-like container (Class/Struct/Interface/Enum/Type/
    // Trait) + Variable/Field. Struct included so an incremental re-resolve seeds
    // the same struct type nodes a full reindex would.
    return label != null and (streq(label, "Function") or streq(label, "Method") or
        c.cbm_label_is_type_like(label) or streq(label, "Variable") or streq(label, "Field"));
}

/// Callback for cbm_gbuf_foreach_node: seed the registry with the existing
/// project's definition symbols so the resolver can match cross-file symbols
/// during incremental. Mirrors the full-index registry contents exactly so an
/// incremental re-resolve picks the same nodes a full reindex would.
fn registryVisitor(node: [*c]const Node, userdata: ?*anyopaque) callconv(.c) void {
    const r: ?*c.cbm_registry_t = @ptrCast(@alignCast(userdata));
    if (!incrLabelIsRegistrySymbol(node.*.label)) return;
    c.cbm_registry_add(r, node.*.name, node.*.qualified_name, node.*.label);
}

fn freeResultCache(cache: [*c][*c]c.CBMFileResult, count: c_int) void {
    for (0..ix(count)) |j| {
        if (cache[j] != null) c.cbm_free_result(cache[j]);
    }
    c.free(@ptrCast(cache));
}

/// Extract + resolve the changed files.
fn runExtractResolve(ctx: *Ctx, changed_files: [*c]FileInfo, ci: c_int) c_int {
    var t: Timespec = undefined;

    // Per-file LSP always runs (every mode). Cross-file LSP stays disabled in
    // incremental regardless (cbm_parallel_resolve is called with NULL
    // cross_registries below).
    const worker_count = c.cbm_default_worker_count(true);
    interop.logInfo("incremental.mode", .{
        "mode",    "parallel",
        "workers", itoa(worker_count),
        "changed", itoa(ci),
    });

    var shared_ids: i64 = c.cbm_gbuf_next_id(ctx.gbuf);

    const cache: [*c][*c]c.CBMFileResult =
        @ptrCast(@alignCast(c.calloc(@bitCast(@as(isize, ci)), @sizeOf([*c]c.CBMFileResult))));
    if (cache != null) {
        now(&t);
        const rc = c.cbm_parallel_extract(ctx, changed_files, ci, cache, &shared_ids, worker_count);
        if (rc == c.CBM_PIPELINE_ABORT_OVER_BUDGET) {
            // Over the memory budget: stop before publication so the
            // served generation stays as it was.
            freeResultCache(cache, ci);
            return rc;
        }
        c.cbm_pipeline_sync_next_id(ctx.gbuf, &shared_ids);
        interop.logInfo("pass.timing", .{ "pass", "incr_extract", "elapsed_ms", elapsedText(t) });

        now(&t);
        _ = c.cbm_build_registry_from_cache(ctx, changed_files, ci, cache);
        c.cbm_pipeline_share_next_id(ctx.gbuf, &shared_ids);
        interop.logInfo("pass.timing", .{ "pass", "incr_registry", "elapsed_ms", elapsedText(t) });

        // Incremental skips cross-file LSP precondition build — it
        // would need all_defs from the full project, not just the
        // changed slice. Per-file LSP (run inside cbm_extract_file)
        // still fires; cross-file resolution is deferred to the
        // next full re-index. Pass NULL/0/NULL to make the fused
        // step in resolve_worker a no-op.
        now(&t);
        _ = c.cbm_parallel_resolve(
            ctx,
            changed_files,
            ci,
            cache,
            &shared_ids,
            worker_count,
            null,
            0,
            null,
            null, // module_def_index
            null, // cross_registries — incremental skips Tier 2 prebuild
        );
        c.cbm_pipeline_sync_next_id(ctx.gbuf, &shared_ids);
        interop.logInfo("pass.timing", .{ "pass", "incr_resolve", "elapsed_ms", elapsedText(t) });

        c.cbm_pipeline_extract_infra_routes(ctx.gbuf, changed_files, cache, null, ci);
        c.cbm_pipeline_process_infra_bindings(ctx.gbuf, changed_files, cache, null, ci);
        freeResultCache(cache, ci);
    }
    return 0;
}

/// Infra Route nodes are keyed by URL across config files: a URL becomes a
/// Route only when no config file lists it under a denied key, and the node
/// belongs to one of the files that list it. A change to one config file can
/// therefore change what another contributes, so when any config file changed
/// or was deleted, every config file is re-extracted. Returns files added.
fn incrExpandInfraChanges(
    files: [*c]const FileInfo,
    file_count: c_int,
    is_changed: [*c]bool,
    deleted: [*c]const MutStr,
    deleted_count: c_int,
) c_int {
    var infra_touched = false;
    for (0..ix(file_count)) |i| {
        if (infra_touched) break;
        infra_touched = is_changed[i] and c.cbm_pipeline_is_infra_file(files[i].rel_path);
    }
    for (0..ix(deleted_count)) |i| {
        if (infra_touched) break;
        infra_touched = c.cbm_pipeline_is_infra_file(deleted[i]);
    }
    if (!infra_touched) return 0;
    var added: c_int = 0;
    for (0..ix(file_count)) |i| {
        if (!is_changed[i] and c.cbm_pipeline_is_infra_file(files[i].rel_path)) {
            is_changed[i] = true;
            added += 1;
        }
    }
    return added;
}

const IncrClientScanCtx = struct {
    changed_paths: ?*const HashTable,
    client_files: ?*HashTable,
};

fn incrFindExistingClientNodes(n_c: [*c]const Node, userdata: ?*anyopaque) callconv(.c) void {
    const ctx: *IncrClientScanCtx = @ptrCast(@alignCast(userdata));
    const n: *const Node = n_c orelse return;
    if (n.file_path == null or n.label == null or n.properties_json == null) return;
    if (!streq(n.label, "Module") and !streq(n.label, "Variable")) return;
    if (c.cbm_ht_get(ctx.changed_paths, n.file_path) == null) return;
    if (c.strstr(n.properties_json, "\"http_client\":") != null) {
        if (c.cbm_ht_get(ctx.client_files, n.file_path) == null) {
            const k: MutStr = c.strdup(n.file_path);
            if (k != null) _ = c.cbm_ht_set(ctx.client_files, k, k);
        }
    }
}

fn incrIsClientFile(fi: *const FileInfo) bool {
    if (fi.path == null) return false;
    if (fi.language != c.CBM_LANG_JAVASCRIPT and fi.language != c.CBM_LANG_TYPESCRIPT and
        fi.language != c.CBM_LANG_TSX)
    {
        return false;
    }
    const f = c.cbm_fopen(fi.path, "rb") orelse return false;
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    if (size <= 0 or size > c.cbm_max_file_bytes()) {
        _ = c.fclose(f);
        return false;
    }
    const buf: MutStr = @ptrCast(c.malloc(@as(usize, @intCast(size)) + 1));
    if (buf == null) {
        _ = c.fclose(f);
        return false;
    }
    const r = c.fread(buf, 1, @intCast(size), f);
    _ = c.fclose(f);
    buf[r] = 0;

    if (c.strstr(buf, "axios.create") == null) {
        c.free(buf);
        return false;
    }

    const fr: ?*c.CBMFileResult =
        c.cbm_extract_file(buf, @intCast(r), fi.language, "incr_probe", fi.rel_path, 0, null, null);
    c.free(buf);
    var has_client = false;
    if (fr) |result| {
        for (0..ix(result.defs.count)) |i| {
            if (result.defs.items[i].http_client != null) {
                has_client = true;
                break;
            }
        }
        c.cbm_free_result(result);
    }
    return has_client;
}

fn freeHtKeysCb(key: CStr, value: ?*anyopaque, userdata: ?*anyopaque) callconv(.c) void {
    _ = value;
    _ = userdata;
    c.free(@constCast(key));
}

fn freeStringHashTable(ht: ?*HashTable) void {
    if (ht == null) return;
    c.cbm_ht_foreach(ht, freeHtKeysCb, null);
    c.cbm_ht_free(ht);
}

/// An axios client instance's baseURL is composed into the HTTP_CALLS edges of
/// files that import it. When any file that defines an http_client changes or was
/// deleted (before or after the edit), treat the files that import it as changed
/// files so they go through full re-extraction. Returns files added. (#75, #1916)
fn incrExpandClientImporterChanges(
    existing: ?*const Gbuf,
    files: [*c]const FileInfo,
    file_count: c_int,
    is_changed: [*c]bool,
    deleted: [*c]const MutStr,
    deleted_count: c_int,
) c_int {
    if (existing == null or files == null or file_count <= 0 or is_changed == null) return 0;
    const changed_set = c.cbm_ht_create(@as(u32, @bitCast(file_count + deleted_count)));
    if (changed_set == null) return 0;
    for (0..ix(file_count)) |i| {
        if (is_changed[i]) _ = c.cbm_ht_set(changed_set, files[i].rel_path, @ptrFromInt(1));
    }
    for (0..ix(deleted_count)) |i| {
        _ = c.cbm_ht_set(changed_set, deleted[i], @ptrFromInt(1));
    }

    const client_files = c.cbm_ht_create(16);
    if (client_files == null) {
        c.cbm_ht_free(changed_set);
        return 0;
    }

    // 1. Check existing graph for files that defined an http_client before the edit
    var scan_ctx: IncrClientScanCtx = .{
        .changed_paths = changed_set,
        .client_files = client_files,
    };
    c.cbm_gbuf_foreach_node(existing, incrFindExistingClientNodes, &scan_ctx);

    // 2. Check changed files on disk to detect newly added client instances
    for (0..ix(file_count)) |i| {
        if (!is_changed[i]) continue;
        if (c.cbm_ht_get(client_files, files[i].rel_path) != null) continue;
        if (incrIsClientFile(util.at(files, i))) {
            const k: MutStr = c.strdup(files[i].rel_path);
            if (k != null) _ = c.cbm_ht_set(client_files, k, k);
        }
    }
    c.cbm_ht_free(changed_set);

    if (c.cbm_ht_count(client_files) == 0) {
        freeStringHashTable(client_files);
        return 0;
    }

    // 3. Find files in existing graph buffer that import any client file
    var edges: [*c][*c]const Edge = null;
    var edge_count: c_int = 0;
    var added: c_int = 0;
    if (c.cbm_gbuf_find_edges_by_type(existing, "IMPORTS", &edges, &edge_count) == 0 and edges != null) {
        for (0..ix(edge_count)) |e| {
            const edge: *const Edge = edges[e];
            const tgt: *const Node = c.cbm_gbuf_find_by_id(existing, edge.target_id) orelse continue;
            if (tgt.file_path == null or c.cbm_ht_get(client_files, tgt.file_path) == null) continue;
            const src: *const Node = c.cbm_gbuf_find_by_id(existing, edge.source_id) orelse continue;
            if (src.file_path == null) continue;
            for (0..ix(file_count)) |f| {
                if (streq(files[f].rel_path, src.file_path)) {
                    if (!is_changed[f]) {
                        is_changed[f] = true;
                        added += 1;
                    }
                    break;
                }
            }
        }
    }
    freeStringHashTable(client_files);
    return added;
}

/// HANDLES edges into infra Routes come only from route matching.
fn incrHandlesEdgeIsNotInfraMatch(gbuf: ?*const Gbuf, edge: [*c]const Edge, userdata: ?*anyopaque) callconv(.c) bool {
    _ = userdata;
    const infra_prefix = "__route__infra__";
    const target: ?*const Node = c.cbm_gbuf_find_by_id(gbuf, edge.*.target_id);
    const t = target orelse return true;
    return t.qualified_name == null or
        c.strncmp(t.qualified_name, infra_prefix, infra_prefix.len) != 0;
}

fn logPassTiming(pass: interop.Str, t: Timespec) void {
    interop.logInfo("pass.timing", .{ "pass", pass, "elapsed_ms", elapsedText(t) });
}

/// Run post-extraction passes (tests, decorator tags, configlink).
fn runPostpasses(ctx: *Ctx, changed_files: [*c]FileInfo, ci: c_int, project: CStr, before: ?*const DerivedState) void {
    var t: Timespec = undefined;

    now(&t);
    _ = c.cbm_pipeline_pass_tests(ctx, changed_files, ci);
    logPassTiming("incr_tests", t);

    // File nodes for changed paths are deleted and recreated above, so their
    // git-history properties disappear unless the full-repository derived
    // pass is reapplied. Rebuild the derived edges as well to match a fresh
    // full index instead of accumulating stale FILE_CHANGES_WITH rows.
    _ = c.cbm_gbuf_delete_edges_by_type(ctx.gbuf, "FILE_CHANGES_WITH");
    now(&t);
    const history_rc = c.cbm_pipeline_pass_githistory(ctx);
    interop.logInfo("pass.timing", .{
        "pass",       "incr_githistory",
        "elapsed_ms", elapsedText(t),
        "rc",         itoa(history_rc),
    });

    now(&t);
    _ = c.cbm_pipeline_pass_decorator_tags(ctx.gbuf, project);
    logPassTiming("incr_decorator_tags", t);

    // Route matching is global: an infra Route in an unchanged config file can
    // match a handler in a changed file, and DATA_FLOWS follow those matches.
    // Remove the previous matches and data flows, then rerun the pass a full
    // index runs, before the derived digest reads the graph.
    now(&t);
    _ = c.cbm_gbuf_delete_edges_by_type_unless(ctx.gbuf, "HANDLES", incrHandlesEdgeIsNotInfraMatch, null);
    _ = c.cbm_gbuf_delete_edges_by_type(ctx.gbuf, "DATA_FLOWS");
    _ = c.cbm_pipeline_create_route_nodes(ctx.gbuf);
    logPassTiming("incr_route_match", t);

    // Restore the final derived node properties before comparing pass inputs.
    // The semantic and similarity passes ignore these complexity fields, but
    // reproducing them makes the conservative whole-graph input digest exact.
    now(&t);
    _ = c.cbm_pipeline_pass_complexity(ctx);
    logPassTiming("incr_complexity", t);

    // Importance reads CALLS/USAGE (re-extraction plus relinked inbound
    // edges) and TESTS (pass_tests, first in this function). It must run
    // before the digest below, which hashes properties_json. Nodes from
    // unchanged files already carry the key; the write-back overwrites it.
    now(&t);
    _ = c.cbm_pipeline_pass_importance(ctx);
    logPassTiming("incr_importance", t);

    now(&t);
    const after = derivedState(ctx.gbuf);
    const input_unchanged = if (before) |b| derivedDigestEqual(&b.input, &after.input) else false;
    interop.logInfo("incremental.derived_digest", .{
        "unchanged",  @as(CStr, if (input_unchanged) "true" else "false"),
        "elapsed_ms", elapsedText(t),
    });

    const config_unchanged = input_unchanged and derivedDigestEqual(&before.?.config, &after.config);
    if (!config_unchanged) {
        now(&t);
        _ = c.cbm_pipeline_pass_configlink(ctx);
        logPassTiming("incr_configlink", t);
    } else {
        interop.logInfo("incremental.derived_gate", .{ "component", "configlink", "decision", "reuse" });
    }

    // SIMILAR_TO + SEMANTICALLY_RELATED edges only in moderate/full modes
    if (ctx.mode <= c.CBM_MODE_MODERATE) {
        // These are global derived relations. The old graph was loaded before
        // this incremental update, so rerunning either pass without first
        // removing its prior output leaves relationships that a clean full
        // index would no longer select after the corpus changed.
        const similarity_unchanged = input_unchanged and
            derivedDigestEqual(&before.?.similarity, &after.similarity);
        if (!similarity_unchanged) {
            _ = c.cbm_gbuf_delete_edges_by_type(ctx.gbuf, "SIMILAR_TO");
            now(&t);
            _ = c.cbm_pipeline_pass_similarity(ctx);
            logPassTiming("incr_similarity", t);
        } else {
            interop.logInfo("incremental.derived_gate", .{ "component", "similarity", "decision", "reuse" });
        }

        const semantic_unchanged = input_unchanged and
            derivedDigestEqual(&before.?.semantic, &after.semantic) and
            c.cbm_gbuf_reuse_zova_semantic_vectors(ctx.gbuf) == 0;
        if (!semantic_unchanged) {
            _ = c.cbm_gbuf_delete_edges_by_type(ctx.gbuf, "SEMANTICALLY_RELATED");
            c.cbm_gbuf_clear_semantic_vectors(ctx.gbuf);
            now(&t);
            _ = c.cbm_pipeline_pass_semantic_edges(ctx);
            logPassTiming("incr_semantic_edges", t);
        } else {
            interop.logInfo("incremental.derived_gate", .{ "component", "semantic", "decision", "reuse" });
        }
    }
}

/// Publish the merged graph and hashes to cbm.zova as a delta generation.
/// Mode-skipped hash rows are preserved so subsequent reindexes can correctly
/// distinguish "never indexed" from "indexed but not visited this pass".
fn dumpAndPersist(
    pipeline: ?*c.cbm_pipeline_t,
    gbuf: ?*Gbuf,
    files: [*c]FileInfo,
    file_count: c_int,
    mode_skipped: [*c]const FileHash,
    mode_skipped_count: c_int,
    before: ?*c.cbm_zova_workspace_snapshot_t,
) c_int {
    // #49: same post-extraction gates as the full path. Nothing is published
    // until the delta commit, so an abort leaves the served generation.
    c.cbm_pipeline_log_phase_mem("incr_pre_dump");
    var gate_rc = c.cbm_pipeline_budget_gate("before_incr_dump");
    if (gate_rc != 0) return gate_rc;
    c.cbm_pipeline_run_before_dump_hook(gbuf);
    if (c.cbm_gbuf_prepare_zova_dump_delta(gbuf) != 0) {
        logError("incremental.err", .{ "phase", "zova_prepare" });
        return c.CBM_NOT_FOUND;
    }
    c.cbm_pipeline_log_phase_mem("incr_zova_prepare");
    gate_rc = c.cbm_pipeline_budget_gate("before_incr_publish");
    if (gate_rc != 0) return gate_rc;
    if (c.cbm_pipeline_publish_zova_user_database_delta(pipeline, gbuf, files, file_count, mode_skipped, mode_skipped_count, before) != 0) {
        logError("incremental.err", .{ "phase", "zova_single_file" });
        return c.CBM_NOT_FOUND;
    }
    c.cbm_pipeline_log_phase_mem("incr_zova_publish");
    return 0;
}

// ── Incremental pipeline entry point ────────────────────────────────

fn copyZovaSnapshotHashes(
    project: CStr,
    snapshot: *const c.cbm_zova_workspace_snapshot_t,
    out_hashes: *[*c]FileHash,
    out_count: *c_int,
) c_int {
    out_hashes.* = null;
    out_count.* = 0;
    if (snapshot.file_hash_count <= 0) return 0;
    const hashes = interop.callocN(FileHash, ix(snapshot.file_hash_count));
    if (hashes == null) return c.CBM_NOT_FOUND;
    for (0..ix(snapshot.file_hash_count)) |i| {
        const source = util.at(snapshot.file_hashes, i);
        hashes[i].project = c.strdup(project);
        hashes[i].rel_path = c.strdup(source.file_path);
        hashes[i].sha256 = c.strdup(source.content_hash);
        hashes[i].mtime_ns = source.mtime_ns;
        hashes[i].size = source.size_bytes;
        if (hashes[i].project == null or hashes[i].rel_path == null or hashes[i].sha256 == null) {
            c.cbm_store_free_file_hashes(hashes, snapshot.file_hash_count);
            return c.CBM_NOT_FOUND;
        }
    }
    out_hashes.* = hashes;
    out_count.* = snapshot.file_hash_count;
    return 0;
}

fn freeDeleted(deleted: [*c]MutStr, deleted_count: c_int) void {
    for (0..ix(deleted_count)) |i| c.free(deleted[i]);
    c.free(@ptrCast(deleted));
}

fn runIncremental(
    p: ?*c.cbm_pipeline_t,
    files: [*c]FileInfo,
    file_count: c_int,
    snapshot: *c.cbm_zova_workspace_snapshot_t,
    existing: ?*Gbuf,
) c_int {
    var t0: Timespec = undefined;
    now(&t0);

    const project: CStr = c.cbm_pipeline_project_name(p);

    var stored: [*c]FileHash = null;
    var stored_count: c_int = 0;
    if (copyZovaSnapshotHashes(project, snapshot, &stored, &stored_count) != 0) {
        logError("incremental.err", .{ "msg", "load_zova_hashes_failed" });
        c.cbm_gbuf_free(existing);
        return c.CBM_NOT_FOUND;
    }

    // Classify files
    var n_changed: c_int = 0;
    var n_unchanged: c_int = 0;
    const is_changed = classifyFiles(files, file_count, stored, stored_count, &n_changed, &n_unchanged);

    // Classify stored files absent from current discovery: truly-deleted
    // (purge) vs mode-skipped (preserve nodes AND hash rows).
    var deleted: [*c]MutStr = null;
    var mode_skipped: [*c]FileHash = null;
    var mode_skipped_count: c_int = 0;
    const deleted_count = findDeletedFiles(
        c.cbm_pipeline_repo_path(p),
        files,
        file_count,
        stored,
        stored_count,
        &deleted,
        &mode_skipped,
        &mode_skipped_count,
    );

    if (is_changed != null) {
        const infra_added = incrExpandInfraChanges(files, file_count, is_changed, deleted, deleted_count);
        n_changed += infra_added;
        n_unchanged -= infra_added;

        const client_added = incrExpandClientImporterChanges(existing, files, file_count, is_changed, deleted, deleted_count);
        n_changed += client_added;
        n_unchanged -= client_added;
    }

    interop.logInfo("incremental.classify", .{
        "changed",      itoa(n_changed),
        "unchanged",    itoa(n_unchanged),
        "deleted",      itoa(deleted_count),
        "mode_skipped", itoa(mode_skipped_count),
    });

    // Fast path: nothing changed → skip. The committed generation is left
    // untouched, which means existing hash rows (including for any
    // mode-skipped files that were already preserved by an earlier run)
    // remain intact.
    if (n_changed == 0 and deleted_count == 0) {
        interop.logInfo("incremental.noop", .{ "reason", "no_changes" });
        c.free(is_changed);
        c.free(@ptrCast(deleted));
        freeModeSkipped(mode_skipped, mode_skipped_count);
        c.cbm_store_free_file_hashes(stored, stored_count);
        c.cbm_gbuf_free(existing);
        return 0;
    }

    // Module and Folder spellings follow the stem collisions of the whole
    // discovered set, as in a full run.
    var stem_marks: StemMarks = undefined;
    c.cbm_stem_marks_init(&stem_marks, project, files, file_count);
    if (incrStemMarksChanged(project, files, file_count, &stem_marks, stored, stored_count, deleted, deleted_count)) {
        interop.logInfo("incremental.declined", .{ "reason", "stem_collision_changed" });
        c.cbm_stem_marks_free(&stem_marks);
        c.free(is_changed);
        freeDeleted(deleted, deleted_count);
        freeModeSkipped(mode_skipped, mode_skipped_count);
        c.cbm_store_free_file_hashes(stored, stored_count);
        c.cbm_gbuf_free(existing);
        return c.CBM_PIPELINE_INCREMENTAL_DECLINED;
    }

    c.cbm_store_free_file_hashes(stored, stored_count);

    // Build list of changed files
    const changed_files: [*c]FileInfo =
        if (n_changed > 0) interop.mallocN(FileInfo, ix(n_changed)) else null;
    var ci: c_int = 0;
    for (0..ix(file_count)) |i| {
        if (is_changed[i]) {
            changed_files[ix(ci)] = files[i];
            ci += 1;
        }
    }
    c.free(is_changed);

    interop.logInfo("incremental.reparse", .{ "files", itoa(ci) });

    // Step 1: the existing graph was exported from the committed generation
    interop.logInfo("incremental.load_zova", .{
        "nodes", itoa(c.cbm_gbuf_node_count(existing)),
        "edges", itoa(c.cbm_gbuf_edge_count(existing)),
    });

    var t: Timespec = undefined;
    now(&t);
    const derived_before = derivedState(existing);
    interop.logInfo("incremental.derived_digest", .{
        "phase",      "baseline",
        "elapsed_ms", elapsedText(t),
    });

    // Snapshot inbound cross-file edges into changed files BEFORE purging, so
    // the cascade delete doesn't permanently drop edges whose source lives in
    // an unchanged (never-re-parsed) file. Re-linked after re-resolution.
    var edge_cap: EdgeCapture = .{ .gbuf = existing, .changed_paths = null };
    {
        const changed_paths = c.cbm_ht_create(if (ci > 0) htCap(ci) else c.CBM_SZ_64);
        for (0..ix(ci)) |i| {
            _ = c.cbm_ht_set(changed_paths, changed_files[i].rel_path, util.at(changed_files, i));
        }
        for (0..ix(deleted_count)) |i| {
            _ = c.cbm_ht_set(changed_paths, deleted[i], deleted[i]);
        }
        edge_cap.changed_paths = changed_paths;
        now(&t);
        c.cbm_gbuf_foreach_edge(existing, incrCaptureInboundEdge, &edge_cap);
        edge_cap.changed_paths = null;
        c.cbm_ht_free(changed_paths); // keys borrowed from changed_files; not freed here
    }
    interop.logInfo("incremental.edge_snapshot", .{
        "captured",   itoa(edge_cap.count),
        "elapsed_ms", elapsedText(t),
    });

    // Step 2: Purge stale nodes
    now(&t);
    for (0..ix(ci)) |i| {
        _ = c.cbm_gbuf_delete_by_file(existing, changed_files[i].rel_path);
    }
    for (0..ix(deleted_count)) |i| {
        _ = c.cbm_gbuf_delete_by_file(existing, deleted[i]);
    }

    const affected_count = ci + deleted_count;
    var affected_paths: [*c]CStr = null;
    if (affected_count > 0) {
        affected_paths = interop.callocN(CStr, ix(affected_count));
        if (affected_paths != null) {
            for (0..ix(ci)) |i| affected_paths[i] = changed_files[i].rel_path;
            for (0..ix(deleted_count)) |i| affected_paths[ix(ci) + i] = deleted[i];
        }
    }
    if (affected_paths != null) {
        _ = c.cbm_gbuf_delete_empty_folders_under(existing, affected_paths, affected_count);
        c.free(@ptrCast(affected_paths));
    } else if (affected_count > 0) {
        interop.logWarn("incremental.folder_prune_oom", .{ "affected_paths", itoa(affected_count) });
    }
    freeDeleted(deleted, deleted_count);
    interop.logInfo("incremental.purge", .{ "elapsed_ms", elapsedText(t) });

    // Step 3-5: Registry + extract + resolve
    const registry = c.cbm_registry_new();
    now(&t);
    c.cbm_gbuf_foreach_node(existing, registryVisitor, registry);
    interop.logInfo("incremental.registry_seed", .{
        "symbols",    itoa(c.cbm_registry_size(registry)),
        "elapsed_ms", elapsedText(t),
    });

    // Discovery exclusions (gitignore + skip dirs) captured by the run that
    // routed here. Borrowed from the pipeline so the auxiliary repo walks
    // (pkgmap via merge_pkg_entries, path aliases) skip excluded subtrees on
    // incremental runs too — same borrow as the full path (#792/#804).
    var excluded_dirs: [*c]MutStr = null;
    var excluded_count: c_int = 0;
    c.cbm_pipeline_get_excluded(p, &excluded_dirs, &excluded_count);

    const path_aliases =
        c.cbm_load_path_aliases_excluded(c.cbm_pipeline_repo_path(p), excluded_dirs, excluded_count);

    var ctx = std.mem.zeroes(Ctx);
    ctx.project_name = project;
    ctx.repo_path = c.cbm_pipeline_repo_path(p);
    ctx.gbuf = existing;
    ctx.registry = registry;
    ctx.cancelled = c.cbm_pipeline_cancelled_ptr(p);
    ctx.pipeline = p; // so passes can record per-file skips (Track B)
    ctx.mode = c.cbm_pipeline_get_mode(p);
    ctx.path_aliases = path_aliases;
    ctx.excluded_dirs = excluded_dirs;
    ctx.excluded_count = excluded_count;
    ctx.stem_marks = &stem_marks;

    for (0..ix(ci)) |i| {
        incrAddChangedFileStructure(existing, project, &stem_marks, util.at(changed_files, i));
    }

    var extract_rc = runExtractResolve(&ctx, changed_files, ci);
    ctx.stem_marks = null;
    c.cbm_stem_marks_free(&stem_marks);
    // #49: extract + resolve work sets are done; gate before the derived
    // passes and publication so an explicitly over-budget run stops here.
    // RSS includes the preloaded served graph. That is deliberate: a user who
    // capped memory below the graph should be told indexing cannot proceed,
    // while the detected RAM-fraction default never gates this path.
    if (extract_rc == 0) extract_rc = c.cbm_pipeline_budget_gate("before_incr_passes");
    if (extract_rc != 0) {
        incrFreeEdgeCapture(&edge_cap);
        c.free(changed_files);
        c.cbm_registry_free(registry);
        c.cbm_path_alias_collection_free(path_aliases);
        c.cbm_pkgmap_free(ctx.pkgmap);
        ctx.pkgmap = null;
        c.cbm_pipeline_set_pkgmap(null);
        freeModeSkipped(mode_skipped, mode_skipped_count);
        c.cbm_gbuf_free(existing);
        return extract_rc;
    }
    // Changed files got fresh File props above, so a stale coverage stamp
    // cannot survive; unchanged files keep theirs from the generation.
    c.cbm_pipeline_stamp_coverage(p, existing);
    _ = c.cbm_pipeline_pass_k8s(&ctx, changed_files, ci);

    incrRebuildSnapshotCallers(&ctx, &edge_cap, files, file_count);

    // Restore inbound cross-file edges before any derived pass reads the
    // mixed graph. Semantic vectors and complexity both consume CALLS
    // topology, so restoring only after those passes made their output differ
    // from a clean full index even though the final SQLite edges matched.
    now(&t);
    const relinked = incrRestoreInboundEdges(existing, &edge_cap);
    interop.logInfo("incremental.edge_relink", .{
        "relinked",   itoa(relinked),
        "captured",   itoa(edge_cap.count),
        "elapsed_ms", elapsedText(t),
    });
    incrFreeEdgeCapture(&edge_cap);

    runPostpasses(&ctx, changed_files, ci, project, &derived_before);

    c.free(changed_files);
    c.cbm_registry_free(registry);
    c.cbm_path_alias_collection_free(path_aliases);
    c.cbm_pkgmap_free(ctx.pkgmap); // built by the parallel extract, if it ran
    ctx.pkgmap = null;
    c.cbm_pipeline_set_pkgmap(null);

    // Step 7: Publish (preserves mode-skipped hash rows so the next reindex
    // can correctly classify those files instead of seeing them as
    // never-existed).
    // Record committed counts before dumpAndPersist (whose dump frees the
    // gbuf node index, zeroing the count) so the #334 plausibility gate also
    // covers incremental reindexes, not just full ones.
    c.cbm_pipeline_set_committed_counts(p, c.cbm_gbuf_node_count(existing), c.cbm_gbuf_edge_count(existing));
    const persist_rc = dumpAndPersist(p, existing, files, file_count, mode_skipped, mode_skipped_count, snapshot);
    freeModeSkipped(mode_skipped, mode_skipped_count);
    c.cbm_gbuf_free(existing);
    if (persist_rc != 0) return persist_rc;

    interop.logInfo("incremental.done", .{ "elapsed_ms", elapsedText(t0) });
    return 0;
}

pub export fn cbm_pipeline_run_incremental_zova(
    p: ?*c.cbm_pipeline_t,
    zova_path: CStr,
    files: [*c]FileInfo,
    file_count: c_int,
    snapshot: ?*c.cbm_zova_workspace_snapshot_t,
    preloaded_graph: ?*Gbuf,
) c_int {
    _ = zova_path;
    const snap = snapshot orelse return c.CBM_NOT_FOUND;
    if (preloaded_graph == null) return c.CBM_NOT_FOUND;
    return runIncremental(p, files, file_count, snap, preloaded_graph);
}
