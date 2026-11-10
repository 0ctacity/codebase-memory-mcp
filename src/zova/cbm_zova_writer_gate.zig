//! cbm_zova_writer_gate.zig — One writer at a time per cbm.zova: a process-wide
//! mutex for threads, plus an exclusive lock file for other processes.
//!
//! Implements src/zova/cbm_zova_writer_gate.h.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const compat_thread = @import("../foundation/compat_thread.zig");

const Str = interop.Str;
const Guard = c.cbm_zova_writer_guard_t;

const is_windows = builtin.os.tag == .windows;

var gate_mutex: c.cbm_mutex_t = undefined;
var gate_waiters = std.atomic.Value(u64).init(0);
var gate_once: c.cbm_once_t = compat_thread.once_init;

fn initGate() callconv(.c) void {
    c.cbm_mutex_init(&gate_mutex);
}

fn ensureGateInitialized() void {
    c.cbm_once(&gate_once, initGate);
}

fn acquireProcessLock(zova_path: Str, guard: *Guard, wait: bool) c_int {
    const n = interop.bprint(&guard.lock_path, "{s}.writer.lock", .{std.mem.span(zova_path)});
    if (n == 0 or n >= guard.lock_path.len) return -1;
    var lock: c.cbm_file_lock_t = undefined;
    if (c.cbm_file_lock_acquire(&guard.lock_path, c.CBM_FILE_LOCK_EXCLUSIVE, wait, &lock) != 0)
        return -1;
    guard.handle = lock.handle;
    return 0;
}

pub export fn cbm_zova_writer_guard_acquire(zova_path: ?Str, guard: ?*Guard) c_int {
    const path = zova_path orelse return -1;
    const g = guard orelse return -1;
    if (path[0] == 0) return -1;
    g.* = std.mem.zeroes(Guard);
    g.handle = -1;
    ensureGateInitialized();
    _ = gate_waiters.fetchAdd(1, .monotonic);
    c.cbm_mutex_lock(&gate_mutex);
    _ = gate_waiters.fetchSub(1, .monotonic);
    g.mutex_held = true;
    if (acquireProcessLock(path, g, true) != 0) {
        g.mutex_held = false;
        c.cbm_mutex_unlock(&gate_mutex);
        return -1;
    }
    return 0;
}

pub export fn cbm_zova_writer_guard_try_acquire(zova_path: ?Str, guard: ?*Guard) c_int {
    const path = zova_path orelse return -1;
    const g = guard orelse return -1;
    if (path[0] == 0) return -1;
    g.* = std.mem.zeroes(Guard);
    g.handle = -1;
    ensureGateInitialized();
    if (is_windows) {
        if (c.TryEnterCriticalSection(&gate_mutex.cs) == 0) return -1;
    } else {
        if (c.pthread_mutex_trylock(&gate_mutex.mtx) != 0) return -1;
    }
    g.mutex_held = true;
    if (acquireProcessLock(path, g, false) != 0) {
        g.mutex_held = false;
        c.cbm_mutex_unlock(&gate_mutex);
        return -1;
    }
    return 0;
}

pub export fn cbm_zova_writer_guard_release(guard: ?*Guard) void {
    const g = guard orelse return;
    var lock = c.cbm_file_lock_t{ .handle = g.handle };
    c.cbm_file_lock_release(&lock);
    g.handle = -1;
    if (g.mutex_held) {
        g.mutex_held = false;
        c.cbm_mutex_unlock(&gate_mutex);
    }
}

pub export fn cbm_zova_writer_gate_waiter_count() u64 {
    return gate_waiters.load(.monotonic);
}
