//! compat_thread.zig — Portable thread, mutex, and aligned allocation.
//!
//! Implements src/foundation/compat_thread.h.
//! POSIX: thin wrappers around pthreads and posix_memalign.
//! Windows: CreateThread, CRITICAL_SECTION, _aligned_malloc.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");

const is_windows = builtin.os.tag == .windows;

/// Default 8MB stack for all threads. macOS ARM64 default is only 512KB,
/// which is too small for deep pipeline passes (configlink, etc.).
const CBM_DEFAULT_STACK_SIZE: usize = 8 * 1024 * 1024;

const ThreadFn = *const fn (?*anyopaque) callconv(.c) ?*anyopaque;

// ── Thread ───────────────────────────────────────────────────────

const WinThreadArg = extern struct {
    func: ThreadFn,
    arg: ?*anyopaque,
};

fn winThreadWrapper(param: ?*anyopaque) callconv(.winapi) c.DWORD {
    const a: *WinThreadArg = @ptrCast(@alignCast(param));
    const func = a.func;
    const arg = a.arg;
    c.free(a);
    _ = func(arg);
    return 0;
}

pub export fn cbm_thread_create(t: *c.cbm_thread_t, stack_size_in: usize, func: ?ThreadFn, arg: ?*anyopaque) c_int {
    const stack_size = if (stack_size_in == 0) CBM_DEFAULT_STACK_SIZE else stack_size_in;
    if (is_windows) {
        const a: *WinThreadArg = @ptrCast(@alignCast(c.malloc(@sizeOf(WinThreadArg)) orelse return c.CBM_NOT_FOUND));
        a.func = func.?;
        a.arg = arg;
        t.handle = c.CreateThread(null, stack_size, winThreadWrapper, a, 0, null);
        if (t.handle == null) {
            c.free(a);
            return c.CBM_NOT_FOUND;
        }
        return 0;
    } else {
        var attr: c.pthread_attr_t = undefined;
        _ = c.pthread_attr_init(&attr);
        _ = c.pthread_attr_setstacksize(&attr, stack_size);
        const rc = c.pthread_create(&t.handle, &attr, func, arg);
        _ = c.pthread_attr_destroy(&attr);
        return rc;
    }
}

pub export fn cbm_thread_join(t: *c.cbm_thread_t) c_int {
    if (is_windows) {
        if (c.WaitForSingleObject(t.handle, c.INFINITE) != c.WAIT_OBJECT_0) return c.CBM_NOT_FOUND;
        _ = c.CloseHandle(t.handle);
        t.handle = null;
        return 0;
    } else {
        const rc = c.pthread_join(t.handle, null);
        if (rc == 0) t.handle = std.mem.zeroes(c.pthread_t);
        return rc;
    }
}

pub export fn cbm_thread_detach(t: *c.cbm_thread_t) c_int {
    if (is_windows) {
        if (t.handle != null) {
            _ = c.CloseHandle(t.handle);
            t.handle = null;
        }
        return 0;
    } else {
        const rc = c.pthread_detach(t.handle);
        if (rc == 0) t.handle = std.mem.zeroes(c.pthread_t);
        return rc;
    }
}

// ── Mutex ────────────────────────────────────────────────────────

pub export fn cbm_mutex_init(m: *c.cbm_mutex_t) void {
    if (is_windows) c.InitializeCriticalSection(&m.cs) else _ = c.pthread_mutex_init(&m.mtx, null);
}

pub export fn cbm_mutex_lock(m: *c.cbm_mutex_t) void {
    if (is_windows) c.EnterCriticalSection(&m.cs) else _ = c.pthread_mutex_lock(&m.mtx);
}

pub export fn cbm_mutex_unlock(m: *c.cbm_mutex_t) void {
    if (is_windows) c.LeaveCriticalSection(&m.cs) else _ = c.pthread_mutex_unlock(&m.mtx);
}

pub export fn cbm_mutex_destroy(m: *c.cbm_mutex_t) void {
    if (is_windows) c.DeleteCriticalSection(&m.cs) else _ = c.pthread_mutex_destroy(&m.mtx);
}

// ── One-time initialization ──────────────────────────────────────

/// CBM_ONCE_INIT. PTHREAD_ONCE_INIT is a brace initializer on macOS, which
/// translate-c cannot turn into a value.
pub const once_init: c.cbm_once_t = if (builtin.os.tag.isDarwin())
    .{ .__sig = c._PTHREAD_ONCE_SIG_init, .__opaque = @splat(0) }
else
    std.mem.zeroes(c.cbm_once_t);

const OnceFn = *const fn () callconv(.c) void;

const OnceCall = extern struct {
    func: OnceFn,
};

fn onceTrampoline(once: ?*anyopaque, parameter: ?*anyopaque, context: ?*?*anyopaque) callconv(.winapi) c_int {
    _ = once;
    _ = context;
    const call: *OnceCall = @ptrCast(@alignCast(parameter));
    call.func();
    return 1;
}

pub export fn cbm_once(once: *c.cbm_once_t, func: ?OnceFn) void {
    if (is_windows) {
        var call = OnceCall{ .func = func.? };
        _ = c.InitOnceExecuteOnce(once, @ptrCast(&onceTrampoline), &call, null);
    } else {
        _ = c.pthread_once(once, func);
    }
}

// ── Aligned allocation ───────────────────────────────────────────

pub export fn cbm_aligned_alloc(ptr: *?*anyopaque, alignment: usize, size: usize) c_int {
    if (is_windows) {
        ptr.* = c._aligned_malloc(size, alignment);
        return if (ptr.* != null) 0 else -1;
    } else {
        return c.posix_memalign(ptr, alignment, size);
    }
}

pub export fn cbm_aligned_free(ptr: ?*anyopaque) void {
    if (is_windows) c._aligned_free(ptr) else c.free(ptr);
}
