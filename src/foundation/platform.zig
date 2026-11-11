//! platform.zig — OS abstraction implementations.
//!
//! Implements src/foundation/platform.h (except cbm_system_info and
//! cbm_default_worker_count, which live in system_info.zig). macOS, Linux,
//! and Windows; platform-specific code sits behind comptime branches.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");

const is_windows = builtin.os.tag == .windows;
const is_apple = builtin.os.tag.isDarwin();

/// Canonicalize a Windows drive letter to upper-case in place: "c:/x" -> "C:/x".
/// Windows drive letters are case-insensitive, but a lowercase one (as agent
/// CWDs often report, e.g. Claude Code's "c:\...") otherwise produces a distinct
/// project key ("c-..." vs "C-...") and, on a case-insensitive FS, a colliding
/// cache file that clobbers the good index (#227/#367/#394). Folding to a single
/// canonical form here — at the one path-normalization choke point — keeps the
/// project key, cache file and integrity check consistent regardless of case.
/// Only the strict drive-root form `X:/` or bare `X:` is touched, so ordinary
/// POSIX paths (which never start that way) are unaffected.
fn canonicalizeDrive(path: [*:0]u8) void {
    if (path[0] >= 'a' and path[0] <= 'z' and path[1] == ':' and (path[2] == '/' or path[2] == 0)) {
        path[0] = path[0] - 'a' + 'A';
    }
}

// Win32 constants whose macros translate-c cannot evaluate.
const GENERIC_READ: c_uint = 0x80000000;
const FILE_MAP_READ: c_uint = 0x0004;
const INVALID_FILE_ATTRIBUTES: c_uint = 0xFFFFFFFF;

// ── Memory mapping ───────────────────────────────────────────────

pub export fn cbm_mmap_read(path: ?[*:0]const u8, out_size: ?*usize) ?*anyopaque {
    const p = path orelse return null;
    const size_out = out_size orelse return null;
    size_out.* = 0;

    if (is_windows) {
        const wpath = c.cbm_utf8_to_wide(p) orelse return null;
        defer c.free(wpath);
        const file = c.CreateFileW(wpath, GENERIC_READ, c.FILE_SHARE_READ, null, c.OPEN_EXISTING, c.FILE_ATTRIBUTE_NORMAL, null);
        if (file == interop.INVALID_HANDLE_VALUE) return null;
        defer _ = c.CloseHandle(file);

        var sz: c.LARGE_INTEGER = undefined;
        if (c.GetFileSizeEx(file, &sz) == 0 or sz.QuadPart == 0) return null;

        const mapping = c.CreateFileMappingW(file, null, c.PAGE_READONLY, 0, 0, null) orelse return null;
        const addr = c.MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        _ = c.CloseHandle(mapping);
        if (addr == null) return null;
        size_out.* = @intCast(sz.QuadPart);
        return addr;
    } else {
        const fd = c.open(p, c.O_RDONLY | c.O_CLOEXEC);
        if (fd < 0) return null;

        var st: c.struct_stat = undefined;
        if (c.fstat(fd, &st) != 0 or st.st_size == 0) {
            _ = c.close(fd);
            return null;
        }

        const len: usize = @intCast(st.st_size);
        const addr = c.mmap(null, len, c.PROT_READ, c.MAP_PRIVATE, fd, 0);
        _ = c.close(fd);

        if (@intFromPtr(addr) == std.math.maxInt(usize)) return null; // MAP_FAILED
        size_out.* = len;
        return addr;
    }
}

pub export fn cbm_munmap(addr: ?*anyopaque, size: usize) void {
    if (is_windows) {
        if (addr != null) _ = c.UnmapViewOfFile(addr);
    } else {
        if (addr != null and size > 0) _ = c.munmap(addr, size);
    }
}

// ── Timing ───────────────────────────────────────────────────────

const apple_clock = struct {
    var timebase_info: c.mach_timebase_info_data_t = undefined;
    var timebase_init: bool = false;
};

pub export fn cbm_now_ns() u64 {
    if (is_windows) {
        var freq: c.LARGE_INTEGER = undefined;
        var count: c.LARGE_INTEGER = undefined;
        _ = c.QueryPerformanceFrequency(&freq);
        _ = c.QueryPerformanceCounter(&count);
        return @as(u64, @bitCast(count.QuadPart)) *% 1000000000 / @as(u64, @bitCast(freq.QuadPart));
    } else if (is_apple) {
        if (!apple_clock.timebase_init) {
            _ = c.mach_timebase_info(&apple_clock.timebase_info);
            apple_clock.timebase_init = true;
        }
        const ticks = c.mach_absolute_time();
        return ticks *% apple_clock.timebase_info.numer / apple_clock.timebase_info.denom;
    } else {
        var ts: c.struct_timespec = undefined;
        _ = c.clock_gettime(c.CLOCK_MONOTONIC, &ts);
        return @as(u64, @bitCast(@as(i64, ts.tv_sec))) *% 1000000000 +% @as(u64, @bitCast(@as(i64, ts.tv_nsec)));
    }
}

const CBM_USEC_PER_SEC: u64 = 1000000;

pub export fn cbm_now_ms() u64 {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

// ── System info ──────────────────────────────────────────────────

pub export fn cbm_nprocs() c_int {
    if (is_windows) {
        var si: c.SYSTEM_INFO = undefined;
        c.GetSystemInfo(&si);
        const n: c_int = @bitCast(si.dwNumberOfProcessors);
        return if (n > 0) n else 1;
    } else if (is_apple) {
        var ncpu: c_int = 0;
        var len: usize = @sizeOf(c_int);
        if (c.sysctlbyname("hw.ncpu", &ncpu, &len, null, 0) == 0 and ncpu > 0) return ncpu;
        return 1;
    } else {
        const n = c.sysconf(c._SC_NPROCESSORS_ONLN);
        return if (n > 0) @truncate(n) else 1;
    }
}

// ── File system ──────────────────────────────────────────────────

/// GetFileAttributesW for a UTF-8 path; INVALID_FILE_ATTRIBUTES on failure.
fn winFileAttributes(path: ?[*:0]const u8) c_uint {
    const wpath = c.cbm_utf8_to_wide(path) orelse return INVALID_FILE_ATTRIBUTES;
    defer c.free(wpath);
    return c.GetFileAttributesW(wpath);
}

pub export fn cbm_file_exists(path: ?[*:0]const u8) bool {
    if (is_windows) {
        return winFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
    } else {
        var st: c.struct_stat = undefined;
        return c.stat(path, &st) == 0;
    }
}

pub export fn cbm_is_dir(path: ?[*:0]const u8) bool {
    if (is_windows) {
        const attr = winFileAttributes(path);
        return attr != INVALID_FILE_ATTRIBUTES and (attr & c.FILE_ATTRIBUTE_DIRECTORY) != 0;
    } else {
        var st: c.struct_stat = undefined;
        return c.stat(path, &st) == 0 and (st.st_mode & c.S_IFMT) == c.S_IFDIR;
    }
}

pub export fn cbm_file_size(path: ?[*:0]const u8) i64 {
    if (is_windows) {
        const wpath = c.cbm_utf8_to_wide(path) orelse return c.CBM_NOT_FOUND;
        var fad: c.WIN32_FILE_ATTRIBUTE_DATA = undefined;
        const ok = c.GetFileAttributesExW(wpath, c.GetFileExInfoStandard, &fad);
        c.free(wpath);
        if (ok == 0) return c.CBM_NOT_FOUND;
        return @bitCast((@as(u64, fad.nFileSizeHigh) << 32) | fad.nFileSizeLow);
    } else {
        var st: c.struct_stat = undefined;
        if (c.stat(path, &st) != 0) return c.CBM_NOT_FOUND;
        return st.st_size;
    }
}

pub export fn cbm_normalize_path_sep(path: ?[*:0]u8) ?[*:0]u8 {
    // Normalize on ALL platforms — backslash paths can arrive via stored
    // data, cross-platform DB files, or Windows-style arguments.
    if (path) |p| {
        var i: usize = 0;
        while (p[i] != 0) : (i += 1) {
            if (p[i] == '\\') p[i] = '/';
        }
        canonicalizeDrive(p);
    }
    return path;
}

// ── Environment variables ────────────────────────────────────────

/// Thread-safe getenv: iterates environ directly instead of calling getenv().
/// getenv() is flagged by concurrency-mt-unsafe because the returned pointer
/// can be invalidated by setenv/putenv in another thread. We copy to a
/// caller-owned buffer immediately.
pub export fn cbm_safe_getenv(name: [*:0]const u8, buf: [*]u8, buf_sz: usize, fallback: ?[*:0]const u8) ?[*:0]const u8 {
    if (interop.environ()) |environ| {
        const key = std.mem.span(name);
        var env = environ;
        while (env[0]) |entry| : (env += 1) {
            if (c.strncmp(entry, name, key.len) == 0 and entry[key.len] == '=') {
                _ = c.snprintf(buf, buf_sz, "%s", entry + key.len + 1);
                return @ptrCast(buf);
            }
        }
    }
    if (fallback) |fb| {
        _ = c.snprintf(buf, buf_sz, "%s", fb);
        return @ptrCast(buf);
    }
    buf[0] = 0;
    return null;
}

pub export fn cbm_env_long(name: [*:0]const u8, out: ?*c_long) bool {
    const result = out orelse return false;
    var raw: [64]u8 = @splat(0);
    if (cbm_safe_getenv(name, &raw, raw.len, null) == null or raw[0] == 0) return false;
    // strtol skips leading blanks, so " 5" would read as 5; a blank in front
    // of a setting is a slip, not a number.
    if (c.isspace(raw[0]) != 0) return false;
    var end: [*c]u8 = null;
    interop.setErrno(0);
    const value = c.strtol(&raw, &end, 10);
    if (interop.errno() != 0 or end == null or end == @as([*c]u8, &raw) or end[0] != 0) return false;
    result.* = value;
    return true;
}

/// Reads `name` into a 256-byte buffer the way the C callers do: the text when
/// the variable is set and non-empty, else null.
fn envSmall(tmp: *[256]u8, name: [*:0]const u8) ?[*:0]const u8 {
    tmp[0] = 0;
    _ = cbm_safe_getenv(name, tmp, tmp.len, null);
    return if (tmp[0] != 0) @ptrCast(tmp) else null;
}

// ── Home directory (cross-platform) ──────────────────────────────

pub export fn cbm_get_home_dir() ?[*:0]const u8 {
    const S = struct {
        var buf: [1024]u8 = @splat(0);
    };
    var tmp: [256]u8 = undefined;
    for ([_][*:0]const u8{ "HOME", "USERPROFILE" }) |name| {
        if (envSmall(&tmp, name)) |value| {
            _ = c.snprintf(&S.buf, S.buf.len, "%s", value);
            _ = cbm_normalize_path_sep(@ptrCast(&S.buf));
            return @ptrCast(&S.buf);
        }
    }
    return null;
}

// ── App config directories (cross-platform) ──────────────────────

pub export fn cbm_app_config_dir() ?[*:0]const u8 {
    const S = struct {
        var buf: [1024]u8 = @splat(0);
    };
    var tmp: [256]u8 = undefined;
    if (is_windows) {
        if (envSmall(&tmp, "APPDATA")) |value| {
            _ = c.snprintf(&S.buf, S.buf.len, "%s", value);
            _ = cbm_normalize_path_sep(@ptrCast(&S.buf));
            return @ptrCast(&S.buf);
        }
        const home = cbm_get_home_dir() orelse return null;
        _ = c.snprintf(&S.buf, S.buf.len, "%s/AppData/Roaming", home);
        return @ptrCast(&S.buf);
    } else {
        // Linux: XDG_CONFIG_HOME or ~/.config
        if (envSmall(&tmp, "XDG_CONFIG_HOME")) |value| {
            _ = c.snprintf(&S.buf, S.buf.len, "%s", value);
            return @ptrCast(&S.buf);
        }
        const home = cbm_get_home_dir() orelse return null;
        _ = c.snprintf(&S.buf, S.buf.len, "%s/.config", home);
        return @ptrCast(&S.buf);
    }
}

pub export fn cbm_app_local_dir() ?[*:0]const u8 {
    if (!is_windows) return cbm_app_config_dir();

    const S = struct {
        var buf: [1024]u8 = @splat(0);
    };
    var tmp: [256]u8 = undefined;
    if (envSmall(&tmp, "LOCALAPPDATA")) |value| {
        _ = c.snprintf(&S.buf, S.buf.len, "%s", value);
        _ = cbm_normalize_path_sep(@ptrCast(&S.buf));
        return @ptrCast(&S.buf);
    }
    const home = cbm_get_home_dir() orelse return null;
    _ = c.snprintf(&S.buf, S.buf.len, "%s/AppData/Local", home);
    return @ptrCast(&S.buf);
}

// ── Cache directory ──────────────────────────────────────────────

pub export fn cbm_resolve_cache_dir() ?[*:0]const u8 {
    const S = struct {
        var buf: [1024]u8 = @splat(0);
    };
    var tmp: [256]u8 = undefined;
    if (envSmall(&tmp, "CBM_CACHE_DIR")) |value| {
        _ = c.snprintf(&S.buf, S.buf.len, "%s", value);
        _ = cbm_normalize_path_sep(@ptrCast(&S.buf));
        return @ptrCast(&S.buf);
    }
    const home = cbm_get_home_dir() orelse return null;
    _ = c.snprintf(&S.buf, S.buf.len, "%s/.cache/codebase-memory-mcp", home);
    return @ptrCast(&S.buf);
}
