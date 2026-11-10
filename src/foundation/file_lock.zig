//! file_lock.zig — Cross-process advisory lock on a lock file.
//!
//! Implements src/foundation/file_lock.h: fcntl record locks on POSIX,
//! LockFileEx on Windows.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");

const is_windows = builtin.os.tag == .windows;

fn handleToInt(handle: anytype) isize {
    return @bitCast(@intFromPtr(handle));
}

pub export fn cbm_file_lock_acquire(path: ?[*:0]const u8, mode: c.cbm_file_lock_mode_t, wait: bool, out: ?*c.cbm_file_lock_t) c_int {
    const o = out orelse return -1;
    o.handle = -1;
    const p = path orelse return -1;
    if (p[0] == 0) return -1;

    if (is_windows) {
        const wide = c.cbm_utf8_to_wide(p);
        if (wide == null) return -1;
        const handle = c.CreateFileW(wide, c.GENERIC_READ | c.GENERIC_WRITE, c.FILE_SHARE_READ | c.FILE_SHARE_WRITE, null, c.OPEN_ALWAYS, c.FILE_ATTRIBUTE_NORMAL, null);
        c.free(wide);
        if (handle == interop.INVALID_HANDLE_VALUE) return -1;

        var overlap = std.mem.zeroes(c.OVERLAPPED);
        var flags: c.DWORD = if (mode == c.CBM_FILE_LOCK_EXCLUSIVE) c.LOCKFILE_EXCLUSIVE_LOCK else 0;
        if (!wait) flags |= c.LOCKFILE_FAIL_IMMEDIATELY;
        if (c.LockFileEx(handle, flags, 0, 1, 0, &overlap) == 0) {
            const err = c.GetLastError();
            _ = c.CloseHandle(handle);
            return if (!wait and err == c.ERROR_LOCK_VIOLATION) c.CBM_FILE_LOCK_BUSY else -1;
        }
        o.handle = handleToInt(handle);
        return 0;
    } else {
        const cloexec = if (@hasDecl(c, "O_CLOEXEC")) c.O_CLOEXEC else 0;
        const fd = c.open(p, c.O_CREAT | c.O_RDWR | cloexec, @as(c_uint, 0o600));
        if (fd < 0) return -1;
        _ = c.cbm_fd_set_cloexec(fd);

        var lock = std.mem.zeroes(c.struct_flock);
        lock.l_type = if (mode == c.CBM_FILE_LOCK_EXCLUSIVE) c.F_WRLCK else c.F_RDLCK;
        lock.l_whence = c.SEEK_SET;
        const command: c_int = if (wait) c.F_SETLKW else c.F_SETLK;
        while (c.fcntl(fd, command, &lock) != 0) {
            if (interop.errno() == c.EINTR) continue;
            const err = interop.errno();
            _ = c.close(fd);
            return if (!wait and (err == c.EAGAIN or err == c.EACCES)) c.CBM_FILE_LOCK_BUSY else -1;
        }
        o.handle = fd;
        return 0;
    }
}

pub export fn cbm_file_lock_release(lock: ?*c.cbm_file_lock_t) void {
    const l = lock orelse return;
    if (l.handle == -1) return;
    if (is_windows) {
        const handle: c.HANDLE = @ptrFromInt(@as(usize, @bitCast(l.handle)));
        var overlap = std.mem.zeroes(c.OVERLAPPED);
        _ = c.UnlockFileEx(handle, 0, 1, 0, &overlap);
        _ = c.CloseHandle(handle);
    } else {
        const fd: c_int = @truncate(l.handle);
        var unlock = std.mem.zeroes(c.struct_flock);
        unlock.l_type = c.F_UNLCK;
        unlock.l_whence = c.SEEK_SET;
        _ = c.fcntl(fd, c.F_SETLK, &unlock);
        _ = c.close(fd);
    }
    l.handle = -1;
}
