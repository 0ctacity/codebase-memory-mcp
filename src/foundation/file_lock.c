#include "foundation/file_lock.h"
#include "foundation/compat.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "foundation/win_utf8.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

int cbm_file_lock_acquire(const char *path, cbm_file_lock_mode_t mode, bool wait,
                          cbm_file_lock_t *out) {
    if (!out) return -1;
    out->handle = -1;
    if (!path || !path[0]) return -1;
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) return -1;
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED overlap;
    memset(&overlap, 0, sizeof(overlap));
    DWORD flags = mode == CBM_FILE_LOCK_EXCLUSIVE ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!wait) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(handle, flags, 0, 1, 0, &overlap)) {
        DWORD error = GetLastError();
        CloseHandle(handle);
        return !wait && error == ERROR_LOCK_VIOLATION ? CBM_FILE_LOCK_BUSY : -1;
    }
    out->handle = (intptr_t)handle;
    return 0;
#else
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    (void)cbm_fd_set_cloexec(fd);
    struct flock lock = {.l_type = mode == CBM_FILE_LOCK_EXCLUSIVE ? F_WRLCK : F_RDLCK,
                         .l_whence = SEEK_SET};
    int command = wait ? F_SETLKW : F_SETLK;
    while (fcntl(fd, command, &lock) != 0) {
        if (errno == EINTR) continue;
        int error = errno;
        close(fd);
        return !wait && (error == EAGAIN || error == EACCES) ? CBM_FILE_LOCK_BUSY : -1;
    }
    out->handle = (intptr_t)fd;
    return 0;
#endif
}

void cbm_file_lock_release(cbm_file_lock_t *lock) {
    if (!lock || lock->handle == -1) return;
#ifdef _WIN32
    OVERLAPPED overlap;
    memset(&overlap, 0, sizeof(overlap));
    (void)UnlockFileEx((HANDLE)lock->handle, 0, 1, 0, &overlap);
    CloseHandle((HANDLE)lock->handle);
#else
    int fd = (int)lock->handle;
    struct flock unlock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
    (void)fcntl(fd, F_SETLK, &unlock);
    close(fd);
#endif
    lock->handle = -1;
}
