//! util.zig — general C-interop helpers used by the pipeline port.
//!
//! These belong in src/foundation/interop.zig. They live here only while the
//! foundation port is in flight on another branch, and move there once both
//! land.

const std = @import("std");
const c = @import("c");

pub const CStr = [*c]const u8;
pub const MutStr = [*c]u8;

// ── Indexing ────────────────────────────────────────────────────────

/// A C int used as an array index or element count. A negative value gives
/// 0, so `for (0..ix(n))` runs as often as C's `for (i = 0; i < n; i++)`.
pub inline fn ix(i: c_int) usize {
    return @intCast(@max(i, 0));
}

// ── foundation/dyn_array.h ──────────────────────────────────────────

/// CBM_DYN_ARRAY(T): the layout every dyn_array.h macro works on.
pub fn DynArray(comptime T: type) type {
    return extern struct {
        items: [*c]T = null,
        count: c_int = 0,
        cap: c_int = 0,
    };
}

fn DaItem(comptime Da: type) type {
    return @typeInfo(@FieldType(@typeInfo(Da).pointer.child, "items")).pointer.child;
}

/// cbm_da_push: appends item, doubling the capacity when full. Drops the item
/// when realloc fails, as the macro does.
pub fn daPush(da: anytype, item: DaItem(@TypeOf(da))) void {
    const Item = DaItem(@TypeOf(da));
    if (da.count >= da.cap) {
        const new_cap: c_int = if (da.cap != 0) da.cap * 2 else 8;
        const new = c.realloc(@ptrCast(da.items), ix(new_cap) * @sizeOf(Item)) orelse return;
        da.items = @ptrCast(@alignCast(new));
        da.cap = new_cap;
    }
    da.items[ix(da.count)] = item;
    da.count += 1;
}

/// cbm_da_push_ptr: grows the array by one slot and returns a pointer to it.
/// Like the macro, it does not check the realloc result.
pub fn daPushPtr(da: anytype) *DaItem(@TypeOf(da)) {
    const Item = DaItem(@TypeOf(da));
    if (da.count >= da.cap) {
        da.cap = if (da.cap != 0) da.cap * 2 else 8;
        da.items = @ptrCast(@alignCast(c.realloc(@ptrCast(da.items), ix(da.cap) * @sizeOf(Item))));
    }
    const slot = &da.items[ix(da.count)];
    da.count += 1;
    return slot;
}

/// cbm_da_free: releases the items and resets the array to empty.
pub fn daFree(da: anytype) void {
    c.free(@ptrCast(da.items));
    da.items = null;
    da.count = 0;
    da.cap = 0;
}

/// cbm_da_reserve: grows the capacity to at least n; keeps the array as it is
/// when realloc fails.
pub fn daReserve(da: anytype, n: c_int) void {
    const Item = DaItem(@TypeOf(da));
    if (n > da.cap) {
        if (c.realloc(@ptrCast(da.items), ix(n) * @sizeOf(Item))) |new| {
            da.items = @ptrCast(@alignCast(new));
            da.cap = n;
        }
    }
}

// ── C strings ───────────────────────────────────────────────────────

/// strcmp(a, b) == 0.
pub inline fn streq(a: CStr, b: CStr) bool {
    return c.strcmp(a, b) == 0;
}

/// The bytes between two pointers into the same C string.
pub inline fn ptrDiff(end: anytype, start: anytype) usize {
    return @intFromPtr(end) - @intFromPtr(start);
}

/// &p[i] for a C pointer. Plain indexing gives a `*allowzero T`, which does
/// not coerce to `*T`.
pub inline fn at(p: anytype, i: usize) @TypeOf(&p[0..1][0]) {
    // An array would be copied into the parameter: take its element address
    // with plain `&array[i]` instead.
    if (@typeInfo(@TypeOf(p)) != .pointer) @compileError("at() takes a pointer, not " ++ @typeName(@TypeOf(p)));
    return &p[i .. i + 1][0];
}

// ── foundation/profile.h ────────────────────────────────────────────

/// CBM_PROF_START: the start time of a profiled phase, read only while
/// profiling is active.
pub inline fn profStart() c.struct_timespec {
    var ts: c.struct_timespec = undefined;
    if (c.cbm_profile_active) c.cbm_profile_now(&ts);
    return ts;
}

/// CBM_PROF_END_N: logs the time since `start` and the item count while
/// profiling is active. CBM_PROF_END is the same with zero items.
pub inline fn profEnd(phase: CStr, sub: CStr, start: *const c.struct_timespec, items: anytype) void {
    if (c.cbm_profile_active) {
        c.cbm_profile_log_elapsed(phase, sub, start, @intCast(items));
    }
}

// ── foundation/compat_thread.h ──────────────────────────────────────

/// cbm_once for Zig callers. A cbm_once_t needs CBM_ONCE_INIT, a C
/// initializer macro that translate-c cannot express on every platform
/// (PTHREAD_ONCE_INIT is a brace initializer on macOS, INIT_ONCE_STATIC_INIT
/// on Windows). Like cbm_once, callers that arrive while the function runs
/// wait for it to finish; they spin instead of blocking.
pub const Once = struct {
    state: std.atomic.Value(u8) = .init(not_started),

    const not_started = 0;
    const running = 1;
    const done = 2;

    pub fn call(self: *Once, comptime init: fn () void) void {
        if (self.state.load(.acquire) == done) return;
        if (self.state.cmpxchgStrong(not_started, running, .acq_rel, .acquire) == null) {
            init();
            self.state.store(done, .release);
            return;
        }
        while (self.state.load(.acquire) != done) std.atomic.spinLoopHint();
    }
};

// ── sys/stat.h ──────────────────────────────────────────────────────

/// S_ISDIR(st.st_mode).
pub inline fn isDirMode(st: *const c.struct_stat) bool {
    return (st.st_mode & c.S_IFMT) == c.S_IFDIR;
}

// ── pipeline_internal.h inline functions ────────────────────────────

/// cbm_pipeline_check_cancel. The header's inline version reads a C11 atomic,
/// which translate-c cannot express, so it is written out here.
pub inline fn checkCancel(ctx: *const c.cbm_pipeline_ctx_t) c_int {
    return if (@atomicLoad(c_int, ctx.cancelled, .seq_cst) != 0) c.CBM_NOT_FOUND else 0;
}
