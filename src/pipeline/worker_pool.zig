//! worker_pool.zig — Parallel-for dispatch with pthreads.
//!
//! Implements src/pipeline/worker_pool.h. Uses pthreads with 8MB stacks and
//! an atomic work-stealing index. GCD is avoided because its worker threads
//! have 512KB stacks, which overflows on deeply nested ASTs (tree-sitter +
//! walk_defs).
//!
//! Each worker pulls indices from a shared atomic counter — zero
//! contention, natural load balancing across heterogeneous cores.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const AtomicInt = std.atomic.Value(c_int);

/// 8 MB per worker: CBM_WORKER_STACK_SIZE in worker_pool.h.
const WORKER_STACK_SIZE: usize = 8 * c.CBM_SZ_1K * c.CBM_SZ_1K;

const ParallelFn = *const fn (c_int, ?*anyopaque) callconv(.c) void;

// ── Serial fallback ─────────────────────────────────────────────────

fn runSerial(count: c_int, func: ParallelFn, ctx: ?*anyopaque) void {
    var i: c_int = 0;
    while (i < count) : (i += 1) {
        func(i, ctx);
    }
}

// ── pthreads backend ────────────────────────────────────────────────

const PthreadWorkerArg = struct {
    func: ParallelFn,
    ctx: ?*anyopaque,
    next_idx: *AtomicInt,
    count: c_int,
};

fn pthreadWorker(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    const wa: *PthreadWorkerArg = @ptrCast(@alignCast(arg));
    while (true) {
        const idx = wa.next_idx.fetchAdd(1, .monotonic);
        if (idx >= wa.count) break;
        wa.func(idx, wa.ctx);
    }
    return null;
}

fn runPthreads(count: c_int, func: ParallelFn, ctx: ?*anyopaque, nworkers_in: c_int) void {
    var nworkers: usize = @intCast(nworkers_in);
    var next_idx: AtomicInt = .init(0);

    var wa: PthreadWorkerArg = .{
        .func = func,
        .ctx = ctx,
        .next_idx = &next_idx,
        .count = count,
    };

    const threads = interop.mallocN(c.cbm_thread_t, nworkers);
    if (threads == null) {
        runSerial(count, func, ctx);
        return;
    }

    for (0..nworkers) |i| {
        if (c.cbm_thread_create(&threads[i], WORKER_STACK_SIZE, pthreadWorker, &wa) != 0) {
            // Failed to create thread — let remaining work run in main thread
            nworkers = i;
            break;
        }
    }

    // Main thread also participates
    while (true) {
        const idx = next_idx.fetchAdd(1, .monotonic);
        if (idx >= count) break;
        func(idx, ctx);
    }

    for (0..nworkers) |i| {
        _ = c.cbm_thread_join(&threads[i]);
    }

    c.free(@ptrCast(threads));
}

// ── Public API ──────────────────────────────────────────────────────

pub export fn cbm_parallel_for(
    count: c_int,
    func_c: ?ParallelFn,
    ctx: ?*anyopaque,
    opts: c.cbm_parallel_for_opts_t,
) void {
    const func = func_c orelse return;
    if (count <= 0) return;

    // Determine worker count
    var nworkers = opts.max_workers;
    if (nworkers <= 0) {
        nworkers = c.cbm_default_worker_count(true);
    }
    if (nworkers < 1) {
        nworkers = 1;
    }

    // Serial fallback: single worker or trivially small workload
    if (nworkers <= 1 or count <= 1) {
        runSerial(count, func, ctx);
        return;
    }

    runPthreads(count, func, ctx, nworkers);
}
