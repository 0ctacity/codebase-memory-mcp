//! compat_fs.zig — Portable file system operations.
//!
//! Implements src/foundation/compat_fs.h (and compat_fs_internal.h).
//! POSIX: direct wrappers around opendir/readdir/closedir, popen/pclose, mkdir, unlink.
//! Windows: FindFirstFile/FindNextFile, isolated CreateProcessW, _wmkdir, _wunlink.
//!
//! The two implementations share every exported name, so each lives in its
//! own namespace and `impl` selects the one for the target.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");
const compat_thread = @import("compat_thread.zig");

const is_windows = builtin.os.tag == .windows;
const is_apple = builtin.os.tag.isDarwin();

pub const impl = if (is_windows) windows_impl else posix_impl;

const Argv = ?[*:null]const ?[*:0]const u8;

// ── Windows implementation ───────────────────────────────────────

const windows_impl = struct {
    const L = std.unicode.utf8ToUtf16LeStringLiteral;

    // Win32 constants whose macros translate-c cannot evaluate.
    const GENERIC_READ: c.DWORD = 0x80000000;
    const GENERIC_WRITE: c.DWORD = 0x40000000;
    const INVALID_FILE_ATTRIBUTES: c.DWORD = 0xFFFFFFFF;
    const PROC_THREAD_ATTRIBUTE_HANDLE_LIST: usize = 0x00020002;
    const ERROR_NOT_ENOUGH_MEMORY: c.DWORD = 8;
    const ERROR_ALREADY_EXISTS: c.DWORD = 183;

    const Dir = extern struct {
        find_handle: c.HANDLE,
        find_data: c.WIN32_FIND_DATAW,
        wide_pattern: [c.CBM_PATH_MAX]c.wchar_t,
        entry: c.cbm_dirent_t,
        first: bool,
        done: bool,
    };

    fn toWide(s: ?[*:0]const u8) ?[*:0]c.wchar_t {
        return @ptrCast(c.cbm_utf8_to_wide(s));
    }

    pub export fn cbm_opendir(path: ?[*:0]const u8) ?*Dir {
        const p = path orelse return null;
        const wpath = toWide(p) orelse return null;
        defer c.free(wpath);
        const wlen = std.mem.len(wpath);
        if (wlen == 0 or wlen + 2 >= c.CBM_PATH_MAX) return null;
        const d: *Dir = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Dir)) orelse return null));
        @memcpy(d.wide_pattern[0 .. wlen + 1], wpath[0 .. wlen + 1]);
        var end = wlen;
        const last = d.wide_pattern[wlen - 1];
        if (last != '\\' and last != '/') {
            d.wide_pattern[end] = '\\';
            end += 1;
        }
        d.wide_pattern[end] = '*';
        d.wide_pattern[end + 1] = 0;

        d.find_handle = c.FindFirstFileW(&d.wide_pattern, &d.find_data);
        if (d.find_handle == interop.INVALID_HANDLE_VALUE) {
            c.free(d);
            return null;
        }
        d.first = true;
        d.done = false;
        return d;
    }

    pub export fn cbm_readdir(dir: ?*Dir) ?*c.cbm_dirent_t {
        const d = dir orelse return null;
        if (d.done) return null;

        if (!d.first) {
            if (c.FindNextFileW(d.find_handle, &d.find_data) == 0) {
                d.done = true;
                return null;
            }
        }
        d.first = false;

        const name = &d.find_data.cFileName;
        while (name[0] == '.' and (name[1] == 0 or (name[1] == '.' and name[2] == 0))) {
            if (c.FindNextFileW(d.find_handle, &d.find_data) == 0) {
                d.done = true;
                return null;
            }
        }

        const utf8: [*:0]u8 = @ptrCast(c.cbm_wide_to_utf8(name) orelse {
            d.done = true;
            return null;
        });
        const nlen = @min(std.mem.len(utf8), c.CBM_DIRENT_NAME_MAX - 1);
        @memcpy(d.entry.name[0..nlen], utf8[0..nlen]);
        d.entry.name[nlen] = 0;
        c.free(utf8);
        d.entry.is_dir = (d.find_data.dwFileAttributes & c.FILE_ATTRIBUTE_DIRECTORY) != 0;
        d.entry.d_type = 0;
        return &d.entry;
    }

    pub export fn cbm_closedir(dir: ?*Dir) void {
        const d = dir orelse return;
        if (d.find_handle != interop.INVALID_HANDLE_VALUE) _ = c.FindClose(d.find_handle);
        c.free(d);
    }

    // Windows _popen replacement that inherits ONLY the child's stdout pipe.
    //
    // The CRT's _popen uses CreateProcess(bInheritHandles=TRUE), which leaks EVERY
    // inheritable handle we hold into the child — listening/client sockets, the
    // Winsock/AFD helper handles created by WSAStartup, the MCP stdio pipe, etc.
    // When the child is git-for-Windows (MSYS2/Cygwin runtime), its startup walks
    // every inherited handle and calls NtQueryObject on each to classify it; on an
    // inherited socket/AFD handle NtQueryObject deadlocks. Since our UI server runs
    // requests on a single thread, that wedges the whole server (list_projects,
    // which shells out to git per project, never returns → the web UI hangs).
    //
    // The fix: spawn via CreateProcessW with STARTUPINFOEXW + an explicit
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST containing only the stdout write-end and a
    // NUL handle for stdin/stderr. Nothing else crosses into git, so there is no
    // foreign handle to deadlock on. POSIX popen() already sets O_CLOEXEC on its
    // pipe, so the POSIX path is unchanged.
    //
    // There is deliberately NO fallback to _popen when the isolated spawn fails:
    // falling back would silently re-arm the deadlock. cbm_popen logs a structured
    // warning and returns NULL instead (every call site handles NULL).
    const CBM_POPEN_MAX = 16;
    const PopenSlot = struct {
        fp: ?*c.FILE = null,
        proc: c.HANDLE = null,
    };
    var g_popen_tab: [CBM_POPEN_MAX]PopenSlot = @splat(.{});
    var g_popen_lock: c.CRITICAL_SECTION = undefined;
    var g_popen_once: c.INIT_ONCE = std.mem.zeroes(c.INIT_ONCE);

    // Test hook (declared in compat_fs_internal.h): 1 when the most recent
    // cbm_popen(..., "r") stream came from the isolated spawn. Test-only
    // observable; not synchronized across threads.
    var g_popen_last_isolated: c_int = 0;

    pub export fn cbm_popen_last_was_isolated() c_int {
        return g_popen_last_isolated;
    }

    fn popenInit(_: c.PINIT_ONCE, _: c.PVOID, _: [*c]c.PVOID) callconv(.c) c.WINBOOL {
        c.InitializeCriticalSection(&g_popen_lock);
        return 1;
    }

    /// Resolve the shell explicitly — %COMSPEC%, else <system dir>\cmd.exe — so it
    /// can be passed as lpApplicationName and CreateProcess never walks the search
    /// path (no cmd.exe planting from a hostile CWD). Heap string; caller frees.
    fn resolveComspec() ?[*:0]c.wchar_t {
        var buf: [c.MAX_PATH]c.wchar_t = undefined;
        const suffix = L("\\cmd.exe");
        const n = c.GetEnvironmentVariableW(L("COMSPEC"), &buf, c.MAX_PATH);
        if (n == 0 or n >= c.MAX_PATH) {
            const sn: usize = c.GetSystemDirectoryW(&buf, c.MAX_PATH);
            if (sn == 0 or sn + suffix.len >= c.MAX_PATH) return null;
            @memcpy(buf[sn..][0 .. suffix.len + 1], suffix[0 .. suffix.len + 1]);
        }
        return @ptrCast(c._wcsdup(&buf));
    }

    /// On failure returns null with stage.* naming the failing step and gle.* the
    /// GetLastError value captured at that step (0 when errno is the signal).
    /// env: a CREATE_UNICODE_ENVIRONMENT block for the child, or null to inherit.
    fn popenIsolated(cmd: [*:0]const u8, env: ?[*]const c.wchar_t, stage: *[*:0]const u8, gle: *c.DWORD) ?*c.FILE {
        stage.* = "";
        gle.* = 0;
        _ = c.InitOnceExecuteOnce(&g_popen_once, popenInit, null, null);

        var sa: c.SECURITY_ATTRIBUTES = undefined;
        sa.nLength = @sizeOf(c.SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = null;
        sa.bInheritHandle = 1;

        var rd: c.HANDLE = null;
        var wr: c.HANDLE = null;
        if (c.CreatePipe(&rd, &wr, &sa, 0) == 0) {
            stage.* = "pipe";
            gle.* = c.GetLastError();
            return null;
        }
        // The parent read-end must never cross into the child.
        _ = c.SetHandleInformation(rd, c.HANDLE_FLAG_INHERIT, 0);

        // NUL for the child's stdin/stderr so it never touches our real stdin
        // pipe. If NUL cannot be opened, fail: STARTF_USESTDHANDLES slots must
        // never carry INVALID_HANDLE_VALUE.
        const nul = c.CreateFileW(L("NUL"), GENERIC_READ | GENERIC_WRITE, c.FILE_SHARE_READ | c.FILE_SHARE_WRITE, &sa, c.OPEN_EXISTING, 0, null);
        if (nul == interop.INVALID_HANDLE_VALUE) {
            stage.* = "nul";
            gle.* = c.GetLastError();
            _ = c.CloseHandle(rd);
            _ = c.CloseHandle(wr);
            return null;
        }

        var inherit = [2]c.HANDLE{ wr, nul };

        var attr_sz: usize = 0;
        _ = c.InitializeProcThreadAttributeList(null, 1, 0, &attr_sz);
        const attr: c.LPPROC_THREAD_ATTRIBUTE_LIST = @ptrCast(c.malloc(attr_sz));
        const attr_init = attr != null and c.InitializeProcThreadAttributeList(attr, 1, 0, &attr_sz) != 0;
        const prepared = attr_init and
            c.UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherit, @sizeOf(@TypeOf(inherit)), null, null) != 0;
        const attr_gle: c.DWORD = if (prepared) 0 else c.GetLastError();

        var si = std.mem.zeroes(c.STARTUPINFOEXW);
        si.StartupInfo.cb = @sizeOf(c.STARTUPINFOEXW);
        si.StartupInfo.dwFlags = c.STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = wr;
        si.StartupInfo.hStdError = nul;
        si.lpAttributeList = attr;

        // Run through cmd.exe /c so command quoting and `2>NUL` behave as under
        // _popen. The command line is heap-composed (no fixed-size truncation)
        // and widened via UTF-8 so non-ASCII repo paths survive intact.
        const app = resolveComspec();
        var wcmdline: ?[*:0]c.wchar_t = null;
        if (app != null) {
            const prefix = "cmd.exe /c ";
            const cmd_len = std.mem.len(cmd);
            if (@as(?[*]u8, @ptrCast(c.malloc(prefix.len + cmd_len + 1)))) |utf8| {
                @memcpy(utf8[0..prefix.len], prefix);
                @memcpy(utf8[prefix.len..][0 .. cmd_len + 1], cmd[0 .. cmd_len + 1]);
                wcmdline = toWide(@ptrCast(utf8));
                c.free(utf8);
            }
        }

        var pi = std.mem.zeroes(c.PROCESS_INFORMATION);
        var created = false;
        if (!prepared) {
            stage.* = "attr";
            gle.* = attr_gle;
        } else if (app == null or wcmdline == null) {
            stage.* = "cmdline";
            gle.* = ERROR_NOT_ENOUGH_MEMORY;
        } else {
            var flags: c.DWORD = c.EXTENDED_STARTUPINFO_PRESENT;
            if (env != null) flags |= c.CREATE_UNICODE_ENVIRONMENT;
            created = c.CreateProcessW(app, wcmdline, null, null, 1, flags, @ptrCast(@constCast(env)), null, &si.StartupInfo, &pi) != 0;
            if (!created) {
                stage.* = "spawn";
                gle.* = c.GetLastError();
            }
        }

        c.free(app);
        c.free(wcmdline);
        if (attr != null) {
            if (attr_init) c.DeleteProcThreadAttributeList(attr);
            c.free(attr);
        }
        _ = c.CloseHandle(wr); // the child owns the write-end now
        _ = c.CloseHandle(nul);
        if (!created) {
            _ = c.CloseHandle(rd);
            return null;
        }
        _ = c.CloseHandle(pi.hThread);

        const fd = c._open_osfhandle(@bitCast(@intFromPtr(rd)), c._O_RDONLY);
        if (fd == -1) {
            stage.* = "osfhandle";
            _ = c.CloseHandle(rd);
            _ = c.CloseHandle(pi.hProcess);
            return null;
        }
        const fp: *c.FILE = c._fdopen(fd, "r") orelse { // takes ownership of fd/rd
            stage.* = "fdopen";
            _ = c._close(fd);
            _ = c.CloseHandle(pi.hProcess);
            return null;
        };

        c.EnterCriticalSection(&g_popen_lock);
        for (&g_popen_tab) |*slot| {
            if (slot.fp == null) {
                slot.fp = fp;
                slot.proc = pi.hProcess;
                c.LeaveCriticalSection(&g_popen_lock);
                return fp;
            }
        }
        c.LeaveCriticalSection(&g_popen_lock);
        // Table full (shouldn't happen): don't leak the process handle.
        stage.* = "table";
        _ = c.CloseHandle(pi.hProcess);
        _ = c.fclose(fp);
        return null;
    }

    fn popenReadIsolated(cmd: [*:0]const u8, env: ?[*]const c.wchar_t) ?*c.FILE {
        var stage: [*:0]const u8 = "";
        var gle: c.DWORD = 0;
        const fp = popenIsolated(cmd, env, &stage, &gle);
        g_popen_last_isolated = @intFromBool(fp != null);
        if (fp == null) {
            var glebuf: [16]u8 = undefined;
            var errnobuf: [16]u8 = undefined;
            _ = interop.bprint(&glebuf, "{d}", .{gle});
            _ = interop.bprint(&errnobuf, "{d}", .{interop.errno()});
            interop.logWarn("compat.popen_isolated_failed", .{
                "stage", stage,
                "gle",   @as(interop.Str, @ptrCast(&glebuf)),
                "errno", @as(interop.Str, @ptrCast(&errnobuf)),
            });
        }
        return fp;
    }

    pub export fn cbm_popen(cmd: [*:0]const u8, mode: ?[*:0]const u8) ?*c.FILE {
        // Our git shell-outs are all read-mode; they MUST use the isolated
        // spawn. On failure, log and fail the call — never fall back to
        // _popen, whose full handle inheritance re-arms the UI hang (#798).
        if (mode) |m| {
            if (m[0] == 'r' and m[1] == 0) return popenReadIsolated(cmd, null);
        }
        g_popen_last_isolated = 0;
        return c._popen(cmd, mode);
    }

    pub export fn cbm_popen_git(cmd: [*:0]const u8) ?*c.FILE {
        // Fail closed: without the scrubbed block the child would inherit a
        // caller's GIT_DIR and read the wrong repository (#56).
        const env = c.cbm_git_child_env_block() orelse {
            interop.logWarn("compat.popen_git_env_failed", .{ "stage", "env_block" });
            return null;
        };
        const fp = popenReadIsolated(cmd, env);
        c.cbm_git_child_env_free(env); // CreateProcessW copied the block into the child
        return fp;
    }

    pub export fn cbm_pclose(f: ?*c.FILE) c_int {
        _ = c.InitOnceExecuteOnce(&g_popen_once, popenInit, null, null);
        var proc: c.HANDLE = null;
        c.EnterCriticalSection(&g_popen_lock);
        for (&g_popen_tab) |*slot| {
            if (slot.fp == f) {
                proc = slot.proc;
                slot.* = .{};
                break;
            }
        }
        c.LeaveCriticalSection(&g_popen_lock);
        if (proc == null) return c._pclose(f); // opened via _popen (non-read mode)
        _ = c.fclose(f);
        _ = c.WaitForSingleObject(proc, c.INFINITE);
        var code: c.DWORD = 0;
        const got = c.GetExitCodeProcess(proc, &code) != 0;
        _ = c.CloseHandle(proc);
        return if (got) @bitCast(code) else -1;
    }

    pub export fn cbm_fopen(path: ?[*:0]const u8, mode: ?[*:0]const u8) ?*c.FILE {
        const wpath = toWide(path) orelse return null;
        defer c.free(wpath);
        const wmode = toWide(mode) orelse return null;
        defer c.free(wmode);
        return c._wfopen(wpath, wmode);
    }

    pub export fn cbm_mkdir_p(path: ?[*:0]const u8, mode: c_int) bool {
        _ = mode;
        const wpath = toWide(path) orelse return false;
        defer c.free(wpath);
        if (c._wmkdir(wpath) == 0) return true;
        const wlen = std.mem.len(wpath);
        const tmp: [*]c.wchar_t = @ptrCast(@alignCast(c.malloc((wlen + 1) * @sizeOf(c.wchar_t)) orelse return false));
        defer c.free(tmp);
        @memcpy(tmp[0 .. wlen + 1], wpath[0 .. wlen + 1]);
        var i: usize = 1;
        while (i < wlen) : (i += 1) {
            if (tmp[i] == '/' or tmp[i] == '\\') {
                tmp[i] = 0;
                _ = c._wmkdir(tmp);
                tmp[i] = '\\';
            }
        }
        return c._wmkdir(tmp) == 0 or c.GetLastError() == ERROR_ALREADY_EXISTS;
    }

    pub export fn cbm_unlink(path: ?[*:0]const u8) c_int {
        const wpath = toWide(path) orelse return c.CBM_NOT_FOUND;
        defer c.free(wpath);
        return c._wunlink(wpath);
    }

    pub export fn cbm_rmdir(path: ?[*:0]const u8) c_int {
        const wpath = toWide(path) orelse return c.CBM_NOT_FOUND;
        defer c.free(wpath);
        return c._wrmdir(wpath);
    }

    pub export fn cbm_stat_utf8(path: ?[*:0]const u8, st: *c.struct_stat) c_int {
        const wpath = toWide(path) orelse return c.CBM_NOT_FOUND;
        var wst: c.struct__stat64 = undefined;
        const ret = c._wstat64(wpath, &wst);
        c.free(wpath);
        if (ret != 0) return c.CBM_NOT_FOUND;
        st.st_mode = wst.st_mode;
        st.st_size = @truncate(wst.st_size);
        st.st_mtime = wst.st_mtime;
        return 0;
    }

    pub export fn cbm_stat_no_follow(path: ?[*:0]const u8, st: *c.struct_stat) c_int {
        if (toWide(path)) |wpath| {
            const attr = c.GetFileAttributesW(wpath);
            c.free(wpath);
            if (attr != INVALID_FILE_ATTRIBUTES and (attr & c.FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return c.CBM_NOT_FOUND;
            }
        }
        return cbm_stat_utf8(path, st);
    }

    pub export fn cbm_canonical_path(path: ?[*:0]const u8, out: [*]u8, out_sz: usize) bool {
        return c._fullpath(out, path, out_sz) != null;
    }

    fn needsQuote(arg: []const u8) bool {
        if (arg.len == 0) return true;
        return std.mem.indexOfAny(u8, arg, " \t\"") != null;
    }

    /// Build a properly-quoted Windows command line from an argv array.
    /// Returns a heap-allocated wide string, or NULL on allocation failure.
    /// Quoting follows the MSVC CRT convention: arguments containing spaces,
    /// tabs, or double-quotes are wrapped in double-quotes, with backslashes
    /// before a closing quote doubled and the quote itself escaped. Argument
    /// bytes are treated as UTF-8 and converted to wide via cbm_utf8_to_wide,
    /// so non-ASCII arguments (e.g. a non-ASCII %USERPROFILE%) survive intact.
    /// Declared in compat_fs_internal.h so the test suite can drive it.
    pub export fn cbm_build_cmdline(argv: [*:null]const ?[*:0]const u8) ?[*:0]c.wchar_t {
        // First pass: compute required buffer size.
        var total: usize = 1; // NUL terminator
        var i: usize = 0;
        while (argv[i]) |arg_ptr| : (i += 1) {
            const arg = std.mem.span(arg_ptr);
            if (i > 0) total += 1; // space separator
            if (needsQuote(arg)) {
                total += 2; // opening and closing quote
                var backslashes: usize = 0;
                for (arg) |ch| {
                    if (ch == '\\') {
                        backslashes += 1;
                    } else if (ch == '"') {
                        total += backslashes + 1; // double backslashes + escape backslash
                        backslashes = 0;
                    } else {
                        backslashes = 0;
                    }
                    total += 1;
                }
                // Trailing backslashes before closing quote must be doubled.
                total += backslashes;
            } else {
                total += arg.len;
            }
        }

        // Build the quoted command line in UTF-8 first, then widen it as a
        // whole via cbm_utf8_to_wide. Every character the quoting logic acts
        // on (space, tab, '"', '\\') is ASCII and, by UTF-8's design, never
        // appears inside a multibyte sequence, so operating on raw bytes here
        // is safe and keeps multibyte argument bytes intact for conversion.
        const buf: [*]u8 = @ptrCast(c.malloc(total) orelse return null);
        defer c.free(buf);

        // Second pass: write the command line bytes.
        var w: usize = 0;
        i = 0;
        while (argv[i]) |arg_ptr| : (i += 1) {
            const arg = std.mem.span(arg_ptr);
            if (i > 0) {
                buf[w] = ' ';
                w += 1;
            }
            if (needsQuote(arg)) {
                buf[w] = '"';
                w += 1;
                var backslashes: usize = 0;
                for (arg) |ch| {
                    if (ch == '\\') {
                        backslashes += 1;
                        buf[w] = '\\';
                        w += 1;
                    } else if (ch == '"') {
                        // Double the preceding backslashes, then escape the quote.
                        @memset(buf[w..][0..backslashes], '\\');
                        w += backslashes;
                        buf[w] = '\\';
                        buf[w + 1] = '"';
                        w += 2;
                        backslashes = 0;
                    } else {
                        backslashes = 0;
                        buf[w] = ch;
                        w += 1;
                    }
                }
                // Double trailing backslashes before the closing quote.
                @memset(buf[w..][0..backslashes], '\\');
                w += backslashes;
                buf[w] = '"';
                w += 1;
            } else {
                @memcpy(buf[w..][0..arg.len], arg);
                w += arg.len;
            }
        }
        buf[w] = 0;

        return toWide(@ptrCast(buf));
    }

    pub export fn cbm_exec_no_shell(argv: Argv) c_int {
        const args = argv orelse return c.CBM_NOT_FOUND;
        if (args[0] == null) return c.CBM_NOT_FOUND;

        const cmdline = cbm_build_cmdline(args) orelse return c.CBM_NOT_FOUND;

        var si = std.mem.zeroes(c.STARTUPINFOW);
        var pi = std.mem.zeroes(c.PROCESS_INFORMATION);
        si.cb = @sizeOf(c.STARTUPINFOW);

        if (c.CreateProcessW(null, cmdline, null, null, 0, 0, null, null, &si, &pi) == 0) {
            c.free(cmdline);
            return c.CBM_NOT_FOUND;
        }
        c.free(cmdline);

        _ = c.WaitForSingleObject(pi.hProcess, c.INFINITE);
        var exit_code: c.DWORD = std.math.maxInt(c.DWORD); // (DWORD)CBM_NOT_FOUND
        _ = c.GetExitCodeProcess(pi.hProcess, &exit_code);
        _ = c.CloseHandle(pi.hProcess);
        _ = c.CloseHandle(pi.hThread);
        return @bitCast(exit_code);
    }
};

// ── POSIX implementation ─────────────────────────────────────────

const posix_impl = struct {
    const Dir = extern struct {
        dir: ?*c.DIR,
        entry: c.cbm_dirent_t,
    };

    pub export fn cbm_opendir(path: ?[*:0]const u8) ?*Dir {
        const p = path orelse return null;
        const dir = c.opendir(p) orelse return null;
        const d: *Dir = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Dir)) orelse {
            _ = c.closedir(dir);
            return null;
        }));
        d.dir = dir;
        return d;
    }

    pub export fn cbm_readdir(dir: ?*Dir) ?*c.cbm_dirent_t {
        const d = dir orelse return null;
        const handle = d.dir orelse return null;
        while (@as(?*c.struct_dirent, c.readdir(handle))) |de| {
            const name: [*:0]const u8 = @ptrCast(&de.d_name);
            // Skip "." and ".."
            if (name[0] == '.' and (name[1] == 0 or (name[1] == '.' and name[2] == 0))) continue;
            const nlen = @min(std.mem.len(name), c.CBM_DIRENT_NAME_MAX - 1);
            @memcpy(d.entry.name[0..nlen], name[0..nlen]);
            d.entry.name[nlen] = 0;
            d.entry.is_dir = de.d_type == c.DT_DIR;
            d.entry.d_type = de.d_type;
            return &d.entry;
        }
        return null;
    }

    pub export fn cbm_closedir(dir: ?*Dir) void {
        const d = dir orelse return;
        if (d.dir) |handle| _ = c.closedir(handle);
        c.free(d);
    }

    pub export fn cbm_popen(cmd: ?[*:0]const u8, mode: ?[*:0]const u8) ?*c.FILE {
        return c.popen(cmd, mode);
    }

    /// Streams opened by cbm_popen_git: popen() cannot take an environment, so
    /// those children are spawned here and reaped by cbm_pclose via this list.
    const PopenGitEntry = struct {
        fp: *c.FILE,
        pid: c.pid_t,
        next: ?*PopenGitEntry,
    };

    var g_popen_git_list: ?*PopenGitEntry = null;
    var g_popen_git_lock: c.cbm_mutex_t = undefined;
    var g_popen_git_once: c.cbm_once_t = compat_thread.once_init;

    fn popenGitLockInit() callconv(.c) void {
        c.cbm_mutex_init(&g_popen_git_lock);
    }

    /// `/bin/sh -c cmd` with stdout on a pipe, stdin on /dev/null (never the MCP
    /// transport), stderr inherited like popen(), and `envp` as the environment.
    fn popenSpawnSh(cmd: [*:0]const u8, envp: [*c][*c]u8, read_fd: *c_int) c.pid_t {
        var fds: [2]c_int = undefined;
        if (c.pipe(&fds) != 0) return c.CBM_NOT_FOUND;
        const rd = fds[0];
        const wr = fds[1];
        var actions: c.posix_spawn_file_actions_t = undefined;
        if (!c.cbm_fd_set_cloexec(rd) or !c.cbm_fd_set_cloexec(wr) or
            c.posix_spawn_file_actions_init(&actions) != 0)
        {
            _ = c.close(rd);
            _ = c.close(wr);
            return c.CBM_NOT_FOUND;
        }
        // dup2 clears close-on-exec on the child's stdout; both pipe ends
        // themselves stay close-on-exec, so no other child ever holds them.
        const configured =
            c.posix_spawn_file_actions_addopen(&actions, c.STDIN_FILENO, "/dev/null", c.O_RDONLY, 0) == 0 and
            c.posix_spawn_file_actions_adddup2(&actions, wr, c.STDOUT_FILENO) == 0;
        var attr: c.posix_spawnattr_t = undefined;
        var attr_ptr: ?*c.posix_spawnattr_t = null;
        if (is_apple) {
            if (c.posix_spawnattr_init(&attr) == 0) {
                _ = c.posix_spawnattr_setflags(&attr, c.POSIX_SPAWN_CLOEXEC_DEFAULT);
                attr_ptr = &attr;
                _ = c.posix_spawn_file_actions_addinherit_np(&actions, c.STDERR_FILENO);
            }
        }
        const argv = [_:null]?[*:0]const u8{ "sh", "-c", cmd };
        var pid: c.pid_t = c.CBM_NOT_FOUND;
        const rc = if (configured)
            c.posix_spawn(&pid, "/bin/sh", &actions, attr_ptr, @ptrCast(&argv), envp)
        else
            c.CBM_NOT_FOUND;
        if (attr_ptr != null) _ = c.posix_spawnattr_destroy(&attr);
        _ = c.posix_spawn_file_actions_destroy(&actions);
        _ = c.close(wr);
        if (rc != 0 or pid <= 0) {
            _ = c.close(rd);
            return c.CBM_NOT_FOUND;
        }
        read_fd.* = rd;
        return pid;
    }

    fn popenGitWait(pid: c.pid_t) c_int {
        var status: c_int = 0;
        while (c.waitpid(pid, &status, 0) < 0) {
            if (interop.errno() != c.EINTR) return c.CBM_NOT_FOUND;
        }
        return status;
    }

    pub export fn cbm_popen_git(cmd: ?[*:0]const u8) ?*c.FILE {
        const command = cmd orelse return null;
        // Fail closed: without the scrubbed environment the child would inherit
        // a caller's GIT_DIR and read the wrong repository (#56).
        const envp = c.cbm_git_child_envp() orelse return null;
        var read_fd: c_int = c.CBM_NOT_FOUND;
        const pid = popenSpawnSh(command, envp, &read_fd);
        c.cbm_git_child_env_free(@ptrCast(envp)); // posix_spawn copied it into the child
        if (pid < 0) return null;

        const entry: ?*PopenGitEntry = @ptrCast(@alignCast(c.malloc(@sizeOf(PopenGitEntry))));
        const fp: ?*c.FILE = if (entry != null) c.fdopen(read_fd, "r") else null;
        if (fp == null) {
            c.free(entry);
            _ = c.close(read_fd);
            _ = popenGitWait(pid);
            return null;
        }
        entry.?.fp = fp.?;
        entry.?.pid = pid;
        c.cbm_once(&g_popen_git_once, popenGitLockInit);
        c.cbm_mutex_lock(&g_popen_git_lock);
        entry.?.next = g_popen_git_list;
        g_popen_git_list = entry;
        c.cbm_mutex_unlock(&g_popen_git_lock);
        return fp;
    }

    pub export fn cbm_pclose(f: ?*c.FILE) c_int {
        var found: ?*PopenGitEntry = null;
        c.cbm_once(&g_popen_git_once, popenGitLockInit);
        c.cbm_mutex_lock(&g_popen_git_lock);
        var link = &g_popen_git_list;
        while (link.*) |entry| : (link = &entry.next) {
            if (entry.fp == f) {
                found = entry;
                link.* = entry.next;
                break;
            }
        }
        c.cbm_mutex_unlock(&g_popen_git_lock);
        const entry = found orelse return c.pclose(f); // opened by popen()
        const pid = entry.pid;
        c.free(entry);
        _ = c.fclose(f);
        return popenGitWait(pid); // the raw wait status, as pclose()
    }

    pub export fn cbm_fopen(path: ?[*:0]const u8, mode: ?[*:0]const u8) ?*c.FILE {
        const f = c.fopen(path, mode);
        if (f != null) {
            const fd = c.fileno(f);
            if (fd >= 0) _ = c.cbm_fd_set_cloexec(fd);
        }
        return f;
    }

    pub export fn cbm_mkdir_p(path: [*:0]const u8, mode: c_int) bool {
        const m: c.mode_t = @truncate(@as(c_uint, @bitCast(mode)));
        // Try direct mkdir first
        if (c.mkdir(path, m) == 0) return true;
        // Walk path and create each component
        const tmp: [*:0]u8 = @ptrCast(c.strdup(path) orelse return false);
        defer c.free(tmp);
        var i: usize = 1;
        while (tmp[0] != 0 and tmp[i] != 0) : (i += 1) {
            if (tmp[i] == '/') {
                tmp[i] = 0;
                _ = c.mkdir(tmp, m); // ignore intermediate errors
                tmp[i] = '/';
            }
        }
        return c.mkdir(tmp, m) == 0 or interop.errno() == c.EEXIST;
    }

    pub export fn cbm_unlink(path: ?[*:0]const u8) c_int {
        return c.unlink(path);
    }

    pub export fn cbm_rmdir(path: ?[*:0]const u8) c_int {
        return c.rmdir(path);
    }

    pub export fn cbm_stat_utf8(path: ?[*:0]const u8, st: ?*c.struct_stat) c_int {
        return c.stat(path, st);
    }

    pub export fn cbm_stat_no_follow(path: ?[*:0]const u8, st: *c.struct_stat) c_int {
        if (c.lstat(path, st) != 0) return c.CBM_NOT_FOUND;
        if ((st.st_mode & c.S_IFMT) == c.S_IFLNK) return c.CBM_NOT_FOUND;
        return 0;
    }

    pub export fn cbm_canonical_path(path: ?[*:0]const u8, out: [*]u8, out_sz: usize) bool {
        var resolved: [c.PATH_MAX]u8 = undefined;
        if (c.realpath(path, &resolved) == null) return false;
        const len = std.mem.len(@as([*:0]const u8, @ptrCast(&resolved)));
        if (len >= out_sz) return false;
        @memcpy(out[0 .. len + 1], resolved[0 .. len + 1]);
        return true;
    }

    pub export fn cbm_exec_no_shell(argv: Argv) c_int {
        const args = argv orelse return c.CBM_NOT_FOUND;
        const program = args[0] orelse return c.CBM_NOT_FOUND;

        if (is_apple) {
            var attr: c.posix_spawnattr_t = undefined;
            if (c.posix_spawnattr_init(&attr) != 0) return c.CBM_NOT_FOUND;
            _ = c.posix_spawnattr_setflags(&attr, c.POSIX_SPAWN_CLOEXEC_DEFAULT);
            var actions: c.posix_spawn_file_actions_t = undefined;
            if (c.posix_spawn_file_actions_init(&actions) != 0) {
                _ = c.posix_spawnattr_destroy(&attr);
                return c.CBM_NOT_FOUND;
            }
            _ = c.posix_spawn_file_actions_addinherit_np(&actions, c.STDIN_FILENO);
            _ = c.posix_spawn_file_actions_addinherit_np(&actions, c.STDOUT_FILENO);
            _ = c.posix_spawn_file_actions_addinherit_np(&actions, c.STDERR_FILENO);
            var pid: c.pid_t = c.CBM_NOT_FOUND;
            const rc = c.posix_spawnp(&pid, program, &actions, &attr, @ptrCast(args), @ptrCast(interop.environ()));
            _ = c.posix_spawn_file_actions_destroy(&actions);
            _ = c.posix_spawnattr_destroy(&attr);
            if (rc != 0 or pid <= 0) {
                return if (rc == c.ENOENT or rc == c.EACCES) 127 else c.CBM_NOT_FOUND;
            }
            var status: c_int = 0;
            while (c.waitpid(pid, &status, 0) < 0) {
                if (interop.errno() != c.EINTR) return c.CBM_NOT_FOUND;
            }
            if (interop.wifexited(status)) return interop.wexitstatus(status);
            return c.CBM_NOT_FOUND;
        } else {
            const pid = c.fork();
            if (pid < 0) return c.CBM_NOT_FOUND;
            if (pid == 0) {
                // Child: exec directly — no shell interpretation
                c.cbm_fd_close_above_stderr();
                _ = c.execvp(program, @ptrCast(args));
                // 127 = standard "command not found" exit code (POSIX convention)
                c._exit(127);
            }
            // Parent: wait for child
            var status: c_int = 0;
            if (c.waitpid(pid, &status, 0) < 0) return c.CBM_NOT_FOUND;
            if (interop.wifexited(status)) return interop.wexitstatus(status);
            return c.CBM_NOT_FOUND; // killed by signal
        }
    }
};

// ── Shared ───────────────────────────────────────────────────────

pub export fn cbm_stat_mtime_ns(st: *const c.struct_stat) i64 {
    const ns_per_sec: i64 = 1000000000;
    if (is_apple) {
        return (@as(i64, st.st_mtimespec.tv_sec) *% ns_per_sec) +% @as(i64, st.st_mtimespec.tv_nsec);
    } else if (is_windows) {
        return @as(i64, st.st_mtime) *% ns_per_sec;
    } else {
        return (@as(i64, st.st_mtim.tv_sec) *% ns_per_sec) +% @as(i64, st.st_mtim.tv_nsec);
    }
}
