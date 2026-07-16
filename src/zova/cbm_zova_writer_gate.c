#include "zova/cbm_zova_writer_gate.h"

#include "foundation/constants.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "foundation/win_utf8.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

static cbm_mutex_t gate_mutex;
static _Atomic uint64_t gate_waiters;

#ifdef _WIN32
static INIT_ONCE gate_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_gate(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    cbm_mutex_init(&gate_mutex);
    return TRUE;
}
static void ensure_gate_initialized(void) {
    InitOnceExecuteOnce(&gate_once, init_gate, NULL, NULL);
}
#else
static pthread_once_t gate_once = PTHREAD_ONCE_INIT;
static void init_gate(void) {
    cbm_mutex_init(&gate_mutex);
}
static void ensure_gate_initialized(void) {
    (void)pthread_once(&gate_once, init_gate);
}
#endif

static int acquire_process_lock(const char *zova_path,
                                cbm_zova_writer_guard_t *guard,
                                bool wait) {
    int n = snprintf(guard->lock_path, sizeof(guard->lock_path),
                     "%s.writer.lock", zova_path);
    if (n <= 0 || (size_t)n >= sizeof(guard->lock_path)) return -1;
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(guard->lock_path);
    if (!wide) return -1;
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED overlap;
    memset(&overlap, 0, sizeof(overlap));
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (!wait) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(handle, flags, 0, 1, 0, &overlap)) {
        CloseHandle(handle);
        return -1;
    }
    guard->handle = (intptr_t)handle;
    return 0;
#else
    int fd = open(guard->lock_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int command = wait ? F_SETLKW : F_SETLK;
    while (fcntl(fd, command, &lock) != 0) {
        if (errno == EINTR) continue;
        close(fd);
        return -1;
    }
    guard->handle = (intptr_t)fd;
    return 0;
#endif
}

int cbm_zova_writer_guard_acquire(const char *zova_path,
                                  cbm_zova_writer_guard_t *guard) {
    if (!zova_path || !zova_path[0] || !guard) return -1;
    memset(guard, 0, sizeof(*guard));
    guard->handle = -1;
    ensure_gate_initialized();
    atomic_fetch_add_explicit(&gate_waiters, 1, memory_order_relaxed);
    cbm_mutex_lock(&gate_mutex);
    atomic_fetch_sub_explicit(&gate_waiters, 1, memory_order_relaxed);
    guard->mutex_held = true;
    if (acquire_process_lock(zova_path, guard, true) != 0) {
        guard->mutex_held = false;
        cbm_mutex_unlock(&gate_mutex);
        return -1;
    }
    return 0;
}

int cbm_zova_writer_guard_try_acquire(const char *zova_path,
                                      cbm_zova_writer_guard_t *guard) {
    if (!zova_path || !zova_path[0] || !guard) return -1;
    memset(guard, 0, sizeof(*guard));
    guard->handle = -1;
    ensure_gate_initialized();
#ifdef _WIN32
    if (!TryEnterCriticalSection(&gate_mutex.cs)) return -1;
#else
    if (pthread_mutex_trylock(&gate_mutex.mtx) != 0) return -1;
#endif
    guard->mutex_held = true;
    if (acquire_process_lock(zova_path, guard, false) != 0) {
        guard->mutex_held = false;
        cbm_mutex_unlock(&gate_mutex);
        return -1;
    }
    return 0;
}

void cbm_zova_writer_guard_release(cbm_zova_writer_guard_t *guard) {
    if (!guard) return;
#ifdef _WIN32
    if (guard->handle != -1) {
        OVERLAPPED overlap;
        memset(&overlap, 0, sizeof(overlap));
        (void)UnlockFileEx((HANDLE)guard->handle, 0, 1, 0, &overlap);
        CloseHandle((HANDLE)guard->handle);
        guard->handle = -1;
    }
#else
    if (guard->handle >= 0) {
        int fd = (int)guard->handle;
        struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
        (void)fcntl(fd, F_SETLK, &lock);
        close(fd);
        guard->handle = -1;
    }
#endif
    if (guard->mutex_held) {
        guard->mutex_held = false;
        cbm_mutex_unlock(&gate_mutex);
    }
}

uint64_t cbm_zova_writer_gate_waiter_count(void) {
    return atomic_load_explicit(&gate_waiters, memory_order_relaxed);
}
