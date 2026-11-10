//! cbm_zova_coordination.zig — Indexing coordination around one shared
//! cbm.zova: the workspace lease and the preparation barrier.
//!
//! Implements src/zova/cbm_zova_coordination.h; the header documents the lock
//! order and what each lock protects.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const compat_thread = @import("../foundation/compat_thread.zig");

const Str = interop.Str;
const Status = c.cbm_zova_coord_status_t;
const Hold = c.cbm_zova_coord_hold_t;
const IndexHold = c.cbm_zova_index_hold_t;

const is_windows = builtin.os.tag == .windows;

const COORD_POLL_MS = 20;
const COORD_DEFAULT_WAIT_S = 300;
const COORD_PATH_MAX = 4096;

/// In-process accounting. A file lock alone cannot exclude threads of one
/// process, and a second descriptor on the same file would drop the process's
/// lock when closed. So each lock file is held at most once per process, and
/// this table records who in the process holds it.
const CoordEntry = struct {
    next: ?*CoordEntry,
    path: [COORD_PATH_MAX]u8,
    shared: c_int, // in-process shared holders
    exclusive: bool,
    lock: c.cbm_file_lock_t,
};

var coord_entries: ?*CoordEntry = null;
var coord_mutex: c.cbm_mutex_t = undefined;
var coord_cancelled = std.atomic.Value(bool).init(false);

var coord_once: c.cbm_once_t = compat_thread.once_init;

fn coordInit() callconv(.c) void {
    c.cbm_mutex_init(&coord_mutex);
}

fn coordEnsureInitialized() void {
    c.cbm_once(&coord_once, coordInit);
}

/// The link that points at the entry for `path`, so the caller can unlink it.
fn coordFind(path: []const u8) ?*?*CoordEntry {
    var link: *?*CoordEntry = &coord_entries;
    while (link.*) |entry| : (link = &entry.next) {
        if (std.mem.eql(u8, std.mem.sliceTo(&entry.path, 0), path)) return link;
    }
    return null;
}

/// The cache directory may not exist before the first index run.
fn coordEnsureParentDir(path: []const u8) bool {
    var dir: [COORD_PATH_MAX]u8 = undefined;
    if (path.len >= dir.len) return false;
    @memcpy(dir[0..path.len], path);
    dir[path.len] = 0;
    var slash = std.mem.lastIndexOfScalar(u8, path, '/');
    if (is_windows) {
        const backslash = std.mem.lastIndexOfScalar(u8, path, '\\');
        if (slash == null or (backslash != null and backslash.? > slash.?)) slash = backslash;
    }
    const at = slash orelse return true;
    if (at == 0) return true;
    dir[at] = 0;
    return c.cbm_mkdir_p(&dir, 0o755);
}

/// One non-blocking attempt, in-process and cross-process.
fn coordTry(path: Str, exclusive: bool) Status {
    c.cbm_mutex_lock(&coord_mutex);
    defer c.cbm_mutex_unlock(&coord_mutex);
    const p = std.mem.span(path);
    if (coordFind(p)) |found| {
        const e = found.*.?;
        if (!exclusive and !e.exclusive) {
            e.shared += 1;
            return c.CBM_ZOVA_COORD_OK;
        }
        return c.CBM_ZOVA_COORD_BUSY;
    }
    const fresh: ?*CoordEntry = interop.callocN(CoordEntry, 1);
    const e = fresh orelse return c.CBM_ZOVA_COORD_ERROR;
    _ = interop.bprint(&e.path, "{s}", .{p});
    const rc = c.cbm_file_lock_acquire(path, if (exclusive) c.CBM_FILE_LOCK_EXCLUSIVE else c.CBM_FILE_LOCK_SHARED, false, &e.lock);
    if (rc != 0) {
        c.free(e);
        return if (rc == c.CBM_FILE_LOCK_BUSY) c.CBM_ZOVA_COORD_BUSY else c.CBM_ZOVA_COORD_ERROR;
    }
    e.exclusive = exclusive;
    e.shared = if (exclusive) 0 else 1;
    e.next = coord_entries;
    coord_entries = e;
    return c.CBM_ZOVA_COORD_OK;
}

fn coordNowMs() i64 {
    var ts: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &ts);
    return @as(i64, ts.tv_sec) * 1000 + @divTrunc(@as(i64, ts.tv_nsec), 1000000);
}

/// Poll instead of blocking in the kernel so a wait stays bounded and returns
/// promptly on shutdown.
fn coordAcquire(path: Str, exclusive: bool, wait_ms: i64, deadline_ms: i64, out: *Hold) Status {
    out.held = false;
    const p = std.mem.span(path);
    if (interop.bprint(&out.path, "{s}", .{p}) >= out.path.len or !coordEnsureParentDir(p))
        return c.CBM_ZOVA_COORD_ERROR;
    coordEnsureInitialized();
    while (true) {
        if (coord_cancelled.load(.seq_cst)) return c.CBM_ZOVA_COORD_CANCELLED;
        const status = coordTry(path, exclusive);
        if (status == c.CBM_ZOVA_COORD_OK) {
            out.held = true;
            return status;
        }
        if (status != c.CBM_ZOVA_COORD_BUSY or wait_ms <= 0 or coordNowMs() >= deadline_ms)
            return status;
        var pause = std.mem.zeroes(c.struct_timespec);
        pause.tv_nsec = COORD_POLL_MS * 1000000;
        _ = c.cbm_nanosleep(&pause, null);
    }
}

pub export fn cbm_zova_coord_default_wait_ms() i64 {
    const raw: [*c]const u8 = c.getenv("CBM_INDEX_LEASE_TIMEOUT_S");
    if (raw != null and raw[0] != 0) {
        var end: [*c]u8 = null;
        const seconds = c.strtoll(raw, &end, 10);
        if (end != null and end[0] == 0 and seconds >= 0) return @as(i64, seconds) *% 1000;
    }
    return COORD_DEFAULT_WAIT_S * 1000;
}

fn coordLeasePath(zova_path: ?Str, workspace_id: ?Str, out: []u8) c_int {
    const path = zova_path orelse return -1;
    if (path[0] == 0 or c.cbm_zova_workspace_id_validate(workspace_id) != 0) return -1;
    const n = interop.bprint(out, "{s}.ws-{s}.lock", .{ std.mem.span(path), std.mem.span(workspace_id.?) });
    return if (n > 0 and n < out.len) 0 else -1;
}

fn coordBarrierPath(zova_path: ?Str, suffix: []const u8, out: []u8) c_int {
    const path = zova_path orelse return -1;
    if (path[0] == 0) return -1;
    const n = interop.bprint(out, "{s}.{s}", .{ std.mem.span(path), suffix });
    return if (n > 0 and n < out.len) 0 else -1;
}

/// The barrier prefers writers. A writer holds the turnstile exclusive while it
/// waits for the barrier; a reader passes the turnstile (shared, briefly) before
/// taking the barrier shared. So once a restore or repack is waiting, no new
/// index run starts, and the runs already going drain instead of overlapping
/// forever.
fn coordBarrierAcquire(zova_path: ?Str, exclusive: bool, wait_ms: i64, deadline_ms: i64, out: *Hold) Status {
    out.held = false;
    var turnstile_path: [COORD_PATH_MAX]u8 = undefined;
    var barrier_path: [COORD_PATH_MAX]u8 = undefined;
    if (coordBarrierPath(zova_path, "prepare.turnstile.lock", &turnstile_path) != 0 or
        coordBarrierPath(zova_path, "prepare.lock", &barrier_path) != 0)
        return c.CBM_ZOVA_COORD_ERROR;
    var turnstile: Hold = undefined;
    var status = coordAcquire(@ptrCast(&turnstile_path), exclusive, wait_ms, deadline_ms, &turnstile);
    if (status != c.CBM_ZOVA_COORD_OK) {
        _ = interop.bprint(&out.path, "{s}", .{std.mem.sliceTo(&barrier_path, 0)});
        return status;
    }
    status = coordAcquire(@ptrCast(&barrier_path), exclusive, wait_ms, deadline_ms, out);
    cbm_zova_coord_release(&turnstile);
    return status;
}

pub export fn cbm_zova_workspace_lease_acquire(zova_path: ?Str, workspace_id: ?Str, wait_ms: i64, out: ?*Hold) Status {
    const hold = out orelse return c.CBM_ZOVA_COORD_ERROR;
    hold.held = false;
    var path: [COORD_PATH_MAX]u8 = undefined;
    if (coordLeasePath(zova_path, workspace_id, &path) != 0) return c.CBM_ZOVA_COORD_ERROR;
    return coordAcquire(@ptrCast(&path), true, wait_ms, coordNowMs() +| wait_ms, hold);
}

pub export fn cbm_zova_prepare_barrier_acquire(zova_path: ?Str, exclusive: bool, wait_ms: i64, out: ?*Hold) Status {
    const hold = out orelse return c.CBM_ZOVA_COORD_ERROR;
    hold.held = false;
    return coordBarrierAcquire(zova_path, exclusive, wait_ms, coordNowMs() +| wait_ms, hold);
}

pub export fn cbm_zova_coord_release(hold: ?*Hold) void {
    const h = hold orelse return;
    if (!h.held) return;
    h.held = false;
    coordEnsureInitialized();
    c.cbm_mutex_lock(&coord_mutex);
    defer c.cbm_mutex_unlock(&coord_mutex);
    const found = coordFind(std.mem.sliceTo(&h.path, 0)) orelse return;
    const e = found.*.?;
    if (!e.exclusive) e.shared -= 1;
    if (e.exclusive or e.shared <= 0) {
        c.cbm_file_lock_release(&e.lock);
        found.* = e.next;
        c.free(e);
    }
}

pub export fn cbm_zova_index_hold_acquire(root_path: ?Str, wait_ms: i64, out: ?*IndexHold, out_workspace_id: [*c]u8, out_workspace_id_size: usize) Status {
    const hold = out orelse return c.CBM_ZOVA_COORD_ERROR;
    hold.* = std.mem.zeroes(IndexHold);
    var zova_path: [COORD_PATH_MAX]u8 = undefined;
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    if (c.cbm_zova_user_database_path(&zova_path, zova_path.len) != 0 or
        c.cbm_zova_workspace_id_for_root(root_path, &workspace_id, workspace_id.len) != 0)
        return c.CBM_ZOVA_COORD_ERROR;
    if (out_workspace_id != null and out_workspace_id_size > 0)
        _ = interop.bprint(out_workspace_id[0..out_workspace_id_size], "{s}", .{std.mem.sliceTo(&workspace_id, 0)});
    const deadline_ms = coordNowMs() +| wait_ms;
    var path: [COORD_PATH_MAX]u8 = undefined;
    var status = coordBarrierAcquire(@ptrCast(&zova_path), false, wait_ms, deadline_ms, &hold.barrier);
    if (status != c.CBM_ZOVA_COORD_OK) {
        hold.barrier_busy = status == c.CBM_ZOVA_COORD_BUSY;
        return status;
    }
    if (coordLeasePath(@ptrCast(&zova_path), @ptrCast(&workspace_id), &path) != 0) {
        cbm_zova_coord_release(&hold.barrier);
        return c.CBM_ZOVA_COORD_ERROR;
    }
    status = coordAcquire(@ptrCast(&path), true, wait_ms, deadline_ms, &hold.lease);
    if (status != c.CBM_ZOVA_COORD_OK) cbm_zova_coord_release(&hold.barrier);
    return status;
}

pub export fn cbm_zova_index_hold_release(hold: ?*IndexHold) void {
    const h = hold orelse return;
    cbm_zova_coord_release(&h.lease);
    cbm_zova_coord_release(&h.barrier);
}

/// Async-signal-safe: a single atomic store.
pub export fn cbm_zova_coord_set_cancelled(cancelled: bool) void {
    coord_cancelled.store(cancelled, .seq_cst);
}
