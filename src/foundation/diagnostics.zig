//! diagnostics.zig — Periodic diagnostics file writer.
//!
//! Implements src/foundation/diagnostics.h. Writes JSON to
//! <tmpdir>/cbm-diagnostics-<pid>.json every 5 seconds. Atomic: writes .tmp
//! then renames to avoid partial reads.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");

const is_windows = builtin.os.tag == .windows;
const has_fd_dir = builtin.os.tag == .linux or builtin.os.tag.isDarwin();

// ── Globals ──────────────────────────────────────────────────────

/// cbm_query_stats_t. translate-c cannot resolve the C11 atomic typedefs, so
/// the layout is restated here: an _Atomic int/long long has the size and
/// alignment of the plain type on every supported target.
const QueryStats = extern struct {
    count: c_int = 0, // total tool calls
    errors: c_int = 0, // tool calls that returned isError=true
    time_us: c_longlong = 0, // cumulative wall-clock time (microseconds)
    max_us: c_longlong = 0, // max single call time (microseconds)
};

pub export var g_query_stats: QueryStats = .{};

var g_diag_stop = std.atomic.Value(c_int).init(0);
var g_diag_thread: c.cbm_thread_t = undefined;
var g_diag_started: bool = false;
var g_start_time: c.time_t = 0;
var g_diag_path: [256]u8 = @splat(0);

// Persistent NDJSON time-series (the memory TRAJECTORY users send us to diagnose
// slow leaks like #581). Unlike g_diag_path (a latest-snapshot file overwritten
// every interval and deleted on stop), this is appended-to and KEPT on exit.
var g_diag_ndjson_path: [256]u8 = @splat(0);
var g_diag_ndjson_size: usize = 0;
const DIAG_NDJSON_CAP_BYTES: usize = 8 * 1024 * 1024; // rotate to .1 past this

fn getpid() c_int {
    return if (is_windows) c._getpid() else c.getpid();
}

// ── Query stats ──────────────────────────────────────────────────

pub export fn cbm_diag_record_query(duration_us: c_longlong, is_error: bool) void {
    _ = @atomicRmw(c_int, &g_query_stats.count, .Add, 1, .seq_cst);
    _ = @atomicRmw(c_longlong, &g_query_stats.time_us, .Add, duration_us, .seq_cst);
    if (is_error) {
        _ = @atomicRmw(c_int, &g_query_stats.errors, .Add, 1, .seq_cst);
    }
    // Update max (lock-free CAS loop)
    var old_max = @atomicLoad(c_longlong, &g_query_stats.max_us, .seq_cst);
    while (duration_us > old_max) {
        old_max = @cmpxchgWeak(c_longlong, &g_query_stats.max_us, old_max, duration_us, .seq_cst, .seq_cst) orelse break;
    }
}

// ── FD count (platform-specific) ─────────────────────────────────

fn countOpenFds() c_int {
    if (!has_fd_dir) return c.CBM_NOT_FOUND; // Not available on Windows

    // Linux: /proc/self/fd. macOS: /dev/fd. scandir is MT-safe.
    const dir = if (builtin.os.tag == .linux) "/proc/self/fd" else "/dev/fd";
    var entries: [*c][*c]c.struct_dirent = null;
    const n = c.scandir(dir, &entries, null, null);
    if (n < 0) return c.CBM_NOT_FOUND;
    for (0..@intCast(n)) |i| c.free(entries[i]);
    c.free(@ptrCast(entries));
    return n - 2; // . and ..
}

// ── Writer ───────────────────────────────────────────────────────

const DIAG_INTERVAL_S = 5;
const DIAG_PATH_EXTRA = 24; // ".tmp" + safety margin

/// Append one compact JSON line to the persistent NDJSON trajectory, rotating to
/// <path>.1 once it passes the cap. Best-effort: a failed append never disrupts
/// the server. The trajectory (a monotonic rss/committed climb over hours) is
/// what reveals a slow leak like #581 — the single latest-snapshot file cannot.
fn appendTrajectory(uptime: c_long, rss: usize, peak_rss: usize, commit: usize, peak_commit: usize, page_faults: usize, fds: c_int, qcount: c_int) void {
    if (g_diag_ndjson_path[0] == 0) return;
    if (g_diag_ndjson_size > DIAG_NDJSON_CAP_BYTES) {
        var rot: [g_diag_ndjson_path.len + DIAG_PATH_EXTRA]u8 = undefined;
        _ = c.snprintf(&rot, rot.len, "%s.1", &g_diag_ndjson_path);
        _ = c.rename(&g_diag_ndjson_path, &rot); // keep one previous generation
        g_diag_ndjson_size = 0;
    }
    const f = c.cbm_fopen(&g_diag_ndjson_path, "a") orelse return;
    const n = c.fprintf(
        f,
        "{\"uptime_s\":%ld,\"rss\":%zu,\"peak_rss\":%zu,\"committed\":%zu," ++
            "\"peak_committed\":%zu,\"page_faults\":%zu,\"fd\":%d,\"queries\":%d}\n",
        uptime,
        rss,
        peak_rss,
        commit,
        peak_commit,
        page_faults,
        fds,
        qcount,
    );
    if (n > 0) g_diag_ndjson_size += @intCast(n);
    _ = c.fclose(f);
}

fn writeDiagnostics() void {
    // Collect mimalloc stats
    var elapsed_ms: usize = 0;
    var user_ms: usize = 0;
    var sys_ms: usize = 0;
    var current_rss: usize = 0;
    var peak_rss: usize = 0;
    var current_commit: usize = 0;
    var peak_commit: usize = 0;
    var page_faults: usize = 0;
    c.mi_process_info(&elapsed_ms, &user_ms, &sys_ms, &current_rss, &peak_rss, &current_commit, &peak_commit, &page_faults);

    // Fallback RSS for ASan builds
    if (current_rss == 0) current_rss = c.cbm_mem_rss();

    const fds = countOpenFds();
    const now = c.time(null);
    const uptime: c_long = @truncate(now - g_start_time);

    const qcount = @atomicLoad(c_int, &g_query_stats.count, .seq_cst);
    const qerrors = @atomicLoad(c_int, &g_query_stats.errors, .seq_cst);
    const qtime = @atomicLoad(c_longlong, &g_query_stats.time_us, .seq_cst);
    const qmax = @atomicLoad(c_longlong, &g_query_stats.max_us, .seq_cst);
    const qavg: c_longlong = if (qcount > 0) @divTrunc(qtime, qcount) else 0;

    // Write to .tmp then rename (atomic)
    var tmp_path: [g_diag_path.len + DIAG_PATH_EXTRA]u8 = undefined;
    _ = c.snprintf(&tmp_path, tmp_path.len, "%s.tmp", &g_diag_path);

    const f = c.cbm_fopen(&tmp_path, "w") orelse return;

    if (c.fprintf(
        f,
        "{\n" ++
            "  \"uptime_s\": %ld,\n" ++
            "  \"rss_bytes\": %zu,\n" ++
            "  \"peak_rss_bytes\": %zu,\n" ++
            "  \"heap_committed_bytes\": %zu,\n" ++
            "  \"peak_committed_bytes\": %zu,\n" ++
            "  \"page_faults\": %zu,\n" ++
            "  \"fd_count\": %d,\n" ++
            "  \"query_count\": %d,\n" ++
            "  \"query_errors\": %d,\n" ++
            "  \"query_total_us\": %lld,\n" ++
            "  \"query_avg_us\": %lld,\n" ++
            "  \"query_max_us\": %lld,\n" ++
            "  \"pid\": %d\n" ++
            "}\n",
        uptime,
        current_rss,
        peak_rss,
        current_commit,
        peak_commit,
        page_faults,
        fds,
        qcount,
        qerrors,
        qtime,
        qavg,
        qmax,
        getpid(),
    ) < 0) {
        _ = c.fclose(f);
        return;
    }
    if (c.fclose(f) != 0) return;
    _ = c.rename(&tmp_path, &g_diag_path);

    // Also append to the persistent trajectory (kept on exit for users to send).
    appendTrajectory(uptime, current_rss, peak_rss, current_commit, peak_commit, page_faults, fds, qcount);
}

fn diagThreadFn(_: ?*anyopaque) callconv(.c) ?*anyopaque {
    while (g_diag_stop.load(.seq_cst) == 0) {
        writeDiagnostics();
        var ts: c.struct_timespec = .{ .tv_sec = DIAG_INTERVAL_S, .tv_nsec = 0 };
        _ = c.cbm_nanosleep(&ts, null);
    }
    // Final write before exit
    writeDiagnostics();
    return null;
}

// ── Public API ───────────────────────────────────────────────────

pub export fn cbm_diag_start() bool {
    var env_buf: [32]u8 = @splat(0);
    _ = c.cbm_safe_getenv("CBM_DIAGNOSTICS", &env_buf, env_buf.len, null);
    const env = std.mem.sliceTo(&env_buf, 0);
    if (!std.mem.eql(u8, env, "1") and !std.mem.eql(u8, env, "true")) return false;

    g_start_time = c.time(null);
    g_diag_stop.store(0, .seq_cst);
    _ = c.snprintf(&g_diag_path, g_diag_path.len, "%s/cbm-diagnostics-%d.json", c.cbm_tmpdir(), getpid());
    _ = c.snprintf(&g_diag_ndjson_path, g_diag_ndjson_path.len, "%s/cbm-diagnostics-%d.ndjson", c.cbm_tmpdir(), getpid());
    g_diag_ndjson_size = 0;

    if (c.cbm_thread_create(&g_diag_thread, 0, diagThreadFn, null) != 0) return false;
    g_diag_started = true;
    _ = c.fprintf(
        interop.stderr(),
        "level=info msg=diagnostics.start snapshot=%s trajectory=%s interval=%ds\n",
        @as([*c]const u8, &g_diag_path),
        @as([*c]const u8, &g_diag_ndjson_path),
        @as(c_int, DIAG_INTERVAL_S),
    );
    return true;
}

pub export fn cbm_diag_stop() void {
    if (!g_diag_started) return;
    g_diag_stop.store(1, .seq_cst);
    _ = c.cbm_thread_join(&g_diag_thread);
    g_diag_started = false;
    // Remove the live latest-snapshot file (and its .tmp), but KEEP the
    // trajectory NDJSON so the user can send it after the server exits.
    _ = c.cbm_unlink(&g_diag_path);
    var tmp_path: [g_diag_path.len + DIAG_PATH_EXTRA]u8 = undefined;
    _ = c.snprintf(&tmp_path, tmp_path.len, "%s.tmp", &g_diag_path);
    _ = c.cbm_unlink(&tmp_path);
    if (g_diag_ndjson_path[0] != 0) {
        _ = c.fprintf(interop.stderr(), "level=info msg=diagnostics.trajectory_kept path=%s\n", @as([*c]const u8, &g_diag_ndjson_path));
    }
}
