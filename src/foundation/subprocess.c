/*
 * subprocess.c — cross-platform spawn + supervise + classify.
 * See subprocess.h.
 */
#include "subprocess.h"

#include "compat.h"   /* cbm_nanosleep */
#include "compat_fs.h"
#include "platform.h" /* cbm_now_ms */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "win_utf8.h" /* cbm_utf8_to_wide — spawn the worker with a wide command line so a
                       * non-ASCII repo path survives CreateProcess (#423/#20) */
#include <stdlib.h>   /* free */
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* NTSTATUS severity ERROR (top two bits set) covers the Windows crash exception
 * exit codes: 0xC0000005 (access violation), 0xC00000FD (stack overflow),
 * 0xC000001D (illegal instruction), 0xC0000094 (integer divide by zero), … */
#define CBM_WIN_CRASH_CODE_MIN 0xC0000000u

#ifndef _WIN32
static bool cbm_is_fault_signal(int sig) {
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGABRT:
    case SIGSYS:
        return true;
    default:
        return false;
    }
}
#endif

cbm_proc_outcome_t cbm_proc_classify(bool exited_normally, int exit_code, int term_signal,
                                     bool timed_out) {
    if (timed_out) {
        return CBM_PROC_HANG;
    }
    if (!exited_normally) {
        /* POSIX signal death. */
#ifndef _WIN32
        if (cbm_is_fault_signal(term_signal)) {
            return CBM_PROC_CRASH;
        }
#else
        (void)term_signal;
#endif
        return CBM_PROC_KILLED;
    }
    /* Exited with a code. A Windows NTSTATUS exception code is a crash; on POSIX
     * exit codes are 0..255 so this branch never misfires there. */
    if ((unsigned)exit_code >= CBM_WIN_CRASH_CODE_MIN) {
        return CBM_PROC_CRASH;
    }
    return (exit_code == 0) ? CBM_PROC_CLEAN : CBM_PROC_EXIT_NONZERO;
}

const char *cbm_proc_outcome_str(cbm_proc_outcome_t o) {
    switch (o) {
    case CBM_PROC_CLEAN:
        return "clean";
    case CBM_PROC_EXIT_NONZERO:
        return "exit_nonzero";
    case CBM_PROC_CRASH:
        return "crash";
    case CBM_PROC_HANG:
        return "hang";
    case CBM_PROC_KILLED:
        return "killed";
    case CBM_PROC_OUTPUT_LIMIT:
        return "output_limit";
    case CBM_PROC_SPAWN_FAILED:
    default:
        return "spawn_failed";
    }
}

/* Tail newly-appended complete lines from the child log, starting at *tail_pos.
 * A partial (non-newline-terminated) final line is left buffered: *tail_pos is
 * not advanced past it, so it is re-read once completed. Returns true if any
 * complete line was consumed (i.e. there was progress). */
static bool cbm_tail_log(const char *log_file, long *tail_pos, cbm_proc_log_cb cb, void *ud) {
    if (!log_file) {
        return false;
    }
    FILE *lf = cbm_fopen(log_file, "r");
    if (!lf) {
        return false;
    }
    bool progressed = false;
    if (fseek(lf, *tail_pos, SEEK_SET) == 0) {
        char line[1024];
        for (;;) {
            long before = ftell(lf);
            if (!fgets(line, sizeof(line), lf)) {
                break;
            }
            size_t l = strlen(line);
            bool complete = (l > 0 && line[l - 1] == '\n');
            if (complete) {
                line[l - 1] = '\0';
                *tail_pos = ftell(lf);
                progressed = true;
                if (line[0] && cb) {
                    cb(line, ud);
                }
            } else if (l == sizeof(line) - 1) {
                /* Oversized line filled the buffer without a newline — consume it
                 * anyway (counts as progress) so we never stall on one long line. */
                *tail_pos = ftell(lf);
                progressed = true;
                if (cb) {
                    cb(line, ud);
                }
            } else {
                /* Genuine partial final line — keep it buffered for next poll. */
                *tail_pos = before;
                break;
            }
        }
    }
    fclose(lf);
    return progressed;
}

/* ── Windows command-line quoting (pure; unit-tested on every platform) ─────── */

/* Append char `c` to buf[cap], reserving the final byte for a NUL terminator.
 * On overflow: sets *ovf, stops writing, and returns pos UNCHANGED — callers detect
 * the overflow via the *ovf flag (not via the return value). */
static size_t cbm_cmdline_put(char *buf, size_t cap, size_t pos, char c, bool *ovf) {
    if (pos + 1 >= cap) {
        *ovf = true;
        return pos;
    }
    buf[pos] = c;
    return pos + 1;
}

/* Append one argv element to the command line using the Microsoft C runtime
 * quoting rules (see MS "Parsing C Command-Line Arguments"). CreateProcess takes
 * a SINGLE string that the child re-parses back into argv, so any element with a
 * space, tab or double-quote must be wrapped in quotes and its embedded quotes /
 * preceding backslashes escaped. Without this a JSON argument like
 * {"repo_path":"C:/r"} loses its inner quotes and the child receives the invalid
 * {repo_path:C:/r} — the Windows-only index-worker cmdline-quoting bug (the worker exited
 * non-zero at JSON-arg parse, misattributed to the last-marked file). POSIX is
 * unaffected: cbm_run_posix passes the argv array straight to execv. */
static size_t cbm_cmdline_append_arg(char *buf, size_t cap, size_t pos, const char *arg, bool first,
                                     bool *ovf) {
    if (!first) {
        pos = cbm_cmdline_put(buf, cap, pos, ' ', ovf);
    }
    pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
    for (const char *p = arg; *p;) {
        size_t nbs = 0;
        while (*p == '\\') {
            nbs++;
            p++;
        }
        if (*p == '\0') {
            /* Trailing backslashes precede the closing quote: double them so the
             * quote stays a delimiter, not an escaped literal. */
            for (size_t k = 0; k < nbs * 2; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            break;
        }
        if (*p == '"') {
            /* N backslashes then a quote -> 2N+1 backslashes then an escaped quote. */
            for (size_t k = 0; k < nbs * 2 + 1; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
            p++;
        } else {
            for (size_t k = 0; k < nbs; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            pos = cbm_cmdline_put(buf, cap, pos, *p, ovf);
            p++;
        }
    }
    pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
    return pos;
}

/* Build a full Windows CreateProcess command line from a NULL-terminated argv,
 * applying the MS C runtime quoting rules so the child re-parses byte-identical
 * argv. Returns true on success, false if the result would overflow `buf`.
 *
 * Defined unconditionally (pure string logic, no Windows headers) so the quoting
 * contract is unit-tested on Linux/macOS CI too — even though the real spawn path
 * only runs on Windows. Shared by cbm_run_win AND the UI http_server index spawn
 * so both escape identically; a naive `"%s"` wrap silently corrupts any argument
 * containing a quote (e.g. the index JSON {"repo_path":"…"}), corrupting the
 * spawned child's argv. */
bool cbm_build_win_cmdline(char *buf, size_t cap, const char *const *argv) {
    if (!buf || cap == 0 || !argv) {
        return false;
    }
    size_t pos = 0;
    bool ovf = false;
    for (int i = 0; argv[i]; i++) {
        pos = cbm_cmdline_append_arg(buf, cap, pos, argv[i], i == 0, &ovf);
        if (ovf) {
            buf[0] = '\0'; /* overflow: leave buf a valid (empty) string, never unterminated */
            return false;
        }
    }
    buf[pos] = '\0';
    return true;
}

/* Wall-clock deadline / output cap checks shared by both run loops. */
static bool deadline_passed(const cbm_proc_opts_t *opts, uint64_t started) {
    return opts->deadline_ms > 0 && (cbm_now_ms() - started) >= (uint64_t)opts->deadline_ms;
}

static bool log_over_limit(const cbm_proc_opts_t *opts) {
    if (opts->max_log_bytes <= 0 || !opts->log_file) {
        return false;
    }
    FILE *lf = fopen(opts->log_file, "rb");
    if (!lf) {
        return false;
    }
    bool over = fseek(lf, 0, SEEK_END) == 0 && ftell(lf) > opts->max_log_bytes;
    (void)fclose(lf);
    return over;
}

#ifdef _WIN32

static int cbm_run_win(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    const char *bin = opts->bin;
    const char *const default_argv[] = {bin, NULL};
    const char *const *argv = opts->argv ? opts->argv : default_argv;

    char cmdline[8192];
    if (!cbm_build_win_cmdline(cmdline, sizeof(cmdline), argv)) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    /* Spawn via CreateProcessW with a WIDE command line. CreateProcessA would
     * re-interpret our UTF-8 cmdline bytes through the ANSI code page (CP_ACP),
     * re-mangling a non-ASCII repo path at the parent->worker boundary — so the
     * worker's own wide-argv read could never recover it (#423/#20). */
    wchar_t *wcmd = cbm_utf8_to_wide(cmdline);
    if (!wcmd) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }

    HANDLE hlog = INVALID_HANDLE_VALUE;
    STARTUPINFOW si = {.cb = sizeof(si)};
    if (opts->log_file) {
        /* STARTF_USESTDHANDLES needs inheritable handles; a default (NULL)
         * security descriptor is not, and the child's output would be lost. */
        SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
        hlog = CreateFileA(opts->log_file, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (hlog != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdError = hlog;
            si.hStdOutput = hlog;
        }
    }

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(wcmd);
    if (hlog != INVALID_HANDLE_VALUE) {
        CloseHandle(hlog);
    }
    if (!ok) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }

    long tail_pos = 0;
    uint64_t started = cbm_now_ms();
    uint64_t last_activity = started;
    bool timed_out = false;
    bool output_limited = false;
    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 200);
        if (cbm_tail_log(opts->log_file, &tail_pos, opts->on_log_line, opts->log_ud)) {
            last_activity = cbm_now_ms();
        }
        if (w == WAIT_OBJECT_0) {
            break;
        }
        output_limited = log_over_limit(opts);
        timed_out = !output_limited &&
                    ((opts->quiet_timeout_ms > 0 &&
                      (cbm_now_ms() - last_activity) >= (uint64_t)opts->quiet_timeout_ms) ||
                     deadline_passed(opts, started));
        if (timed_out || output_limited) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (opts->log_file && opts->delete_log_on_exit) {
        DeleteFileA(opts->log_file);
    }

    out->exit_code = (int)code;
    out->term_signal = 0;
    out->outcome = output_limited ? CBM_PROC_OUTPUT_LIMIT
                                  : cbm_proc_classify(true, (int)code, 0, timed_out);
    return 0;
}

#else /* POSIX */

#if defined(__APPLE__)
#include <spawn.h>

#ifndef POSIX_SPAWN_CLOEXEC_DEFAULT
#define POSIX_SPAWN_CLOEXEC_DEFAULT 0x2000
#endif

static int cbm_run_apple_spawn(const cbm_proc_opts_t *opts, cbm_proc_result_t *out, pid_t *out_pid) {
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    if (opts->own_process_group) {
        flags |= POSIX_SPAWN_SETPGROUP;
        (void)posix_spawnattr_setpgroup(&attr, 0);
    }
    (void)posix_spawnattr_setflags(&attr, flags);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        (void)posix_spawnattr_destroy(&attr);
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }

    /* Stdin: explicitly open /dev/null so child never steals from stdin (e.g. MCP stdio transport) */
    (void)posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    /* Redirect stdout and stderr to the log (or discard) */
    const char *target = opts->log_file ? opts->log_file : "/dev/null";
    (void)posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, target,
                                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
    (void)posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    const char *bin = opts->bin;
    const char *const default_argv[] = {bin, NULL};
    const char *const *argv = opts->argv ? opts->argv : default_argv;

    pid_t pid = -1;
    extern char **environ;
    int rc = posix_spawnp(&pid, bin, &actions, &attr, (char *const *)argv, environ);

    (void)posix_spawn_file_actions_destroy(&actions);
    (void)posix_spawnattr_destroy(&attr);

    if (rc != 0 || pid <= 0) {
        if (opts->delete_log_on_exit && opts->log_file) {
            (void)unlink(opts->log_file);
        }
        if (rc == ENOENT || rc == EACCES) {
            /* Bogus binary path fails cleanly like execv failure (_exit 127) */
            out->outcome = CBM_PROC_EXIT_NONZERO;
            out->exit_code = 127;
            out->term_signal = 0;
            return 0;
        }
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    *out_pid = pid;
    return 1;
}
#endif

static int cbm_run_posix(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    pid_t pid = -1;
#if defined(__APPLE__)
    int spawned = cbm_run_apple_spawn(opts, out, &pid);
    if (spawned <= 0) {
        return spawned;
    }
#else
    pid = fork();
    if (pid < 0) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    if (pid == 0) {
        if (opts->own_process_group) {
            (void)setpgid(0, 0);
        }
        /* Child: redirect stdout+stderr to the log (or discard), then exec.
         * Use open()+dup2() (async-signal-safe, no malloc) rather than freopen():
         * the parent may be multithreaded (the MCP server holds worker/watcher/http
         * threads plus mimalloc/sqlite global state), and a fork() copies
         * only the calling thread — a malloc between fork and exec could deadlock on
         * a lock another thread held at fork time. open/dup2/execv touch no heap. */
        const char *bin = opts->bin;
        const char *const default_argv[] = {bin, NULL};
        const char *const *argv = opts->argv ? opts->argv : default_argv;
        const char *target = opts->log_file ? opts->log_file : "/dev/null";
        int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                (void)close(fd);
            }
        }
        /* Close all descriptors above stderr so children (workers, git, grep)
         * never inherit open server descriptors or lock files (#77). */
        cbm_fd_close_above_stderr();
        execv(bin, (char *const *)argv);
        _exit(127); /* exec failed */
    }

    if (opts->own_process_group) {
        /* Also set from the parent so a kill issued before the child's own
         * setpgid runs still reaches the group (EACCES after exec is fine). */
        (void)setpgid(pid, pid);
    }
#endif
    pid_t kill_target = opts->own_process_group ? -pid : pid;

    long tail_pos = 0;
    uint64_t started = cbm_now_ms();
    uint64_t last_activity = started;
    bool timed_out = false;
    bool output_limited = false;
    int wstatus = 0;
    for (;;) {
        pid_t wr;
        do {
            wr = waitpid(pid, &wstatus, WNOHANG);
        } while (wr < 0 && errno == EINTR);
        bool done = (wr == pid);

        if (cbm_tail_log(opts->log_file, &tail_pos, opts->on_log_line, opts->log_ud)) {
            last_activity = cbm_now_ms();
        }
        if (done) {
            break;
        }
        output_limited = log_over_limit(opts);
        timed_out = !output_limited &&
                    ((opts->quiet_timeout_ms > 0 &&
                      (cbm_now_ms() - last_activity) >= (uint64_t)opts->quiet_timeout_ms) ||
                     deadline_passed(opts, started));
        if (timed_out || output_limited) {
            kill(kill_target, SIGKILL);
            do {
                wr = waitpid(pid, &wstatus, 0);
            } while (wr < 0 && errno == EINTR);
            break;
        }
        struct timespec ts = {0, 100000000L}; /* 100 ms poll */
        cbm_nanosleep(&ts, NULL);
    }

    if (opts->log_file && opts->delete_log_on_exit) {
        (void)unlink(opts->log_file);
    }

    if (output_limited) {
        out->exit_code = -1;
        out->term_signal = SIGKILL;
        out->outcome = CBM_PROC_OUTPUT_LIMIT;
    } else if (WIFEXITED(wstatus)) {
        out->exit_code = WEXITSTATUS(wstatus);
        out->term_signal = 0;
        out->outcome = cbm_proc_classify(true, out->exit_code, 0, timed_out);
    } else if (WIFSIGNALED(wstatus)) {
        out->exit_code = -1;
        out->term_signal = WTERMSIG(wstatus);
        out->outcome = cbm_proc_classify(false, -1, out->term_signal, timed_out);
    } else {
        out->exit_code = -1;
        out->term_signal = 0;
        out->outcome = timed_out ? CBM_PROC_HANG : CBM_PROC_KILLED;
    }
    return 0;
}

#endif

int cbm_subprocess_run(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    cbm_proc_result_t local;
    if (!out) {
        out = &local;
    }
    out->outcome = CBM_PROC_SPAWN_FAILED;
    out->exit_code = -1;
    out->term_signal = 0;
    if (!opts || !opts->bin || !opts->bin[0]) {
        return -1;
    }
#ifdef _WIN32
    return cbm_run_win(opts, out);
#else
    return cbm_run_posix(opts, out);
#endif
}

/* ── Own executable path ──────────────────────────────────────── */

static bool copy_path(char *out, size_t outsz, const char *path) {
    if (!out || outsz == 0 || !path || !path[0]) {
        return false;
    }
    int n = snprintf(out, outsz, "%s", path);
    return n > 0 && (size_t)n < outsz;
}

#ifndef _WIN32
static bool is_executable_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static bool resolve_from_path(const char *name, char *out, size_t outsz) {
    const char *path = getenv("PATH");
    if (!name || !name[0] || strchr(name, '/') || !path || !path[0]) {
        return false;
    }

    const char *cur = path;
    while (*cur) {
        const char *colon = strchr(cur, ':');
        size_t dir_len = colon ? (size_t)(colon - cur) : strlen(cur);
        if (dir_len > 0 && dir_len < 900) {
            char candidate[1024];
            int n = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)dir_len, cur, name);
            if (n > 0 && (size_t)n < sizeof(candidate) && is_executable_file(candidate)) {
                return copy_path(out, outsz, candidate);
            }
        }
        if (!colon) {
            break;
        }
        cur = colon + 1;
    }
    return false;
}

static bool resolve_self_executable(char *out, size_t outsz) {
#if defined(__APPLE__)
    char buf[1024];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0 && buf[0]) {
        return copy_path(out, outsz, buf);
    }
    return false;
#else
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return copy_path(out, outsz, buf);
    }
    return false;
#endif
}
#else
static bool resolve_self_executable(char *out, size_t outsz) {
    char buf[1024];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        return copy_path(out, outsz, buf);
    }
    return false;
}
#endif

bool cbm_resolve_binary_path(const char *argv0, char *out, size_t outsz) {
    if (!out || outsz == 0) {
        return false;
    }
    out[0] = '\0';

#ifndef _WIN32
    if (argv0 && strchr(argv0, '/') && is_executable_file(argv0)) {
        return copy_path(out, outsz, argv0);
    }
    if (resolve_from_path(argv0, out, outsz)) {
        return true;
    }
#else
    if (argv0 && argv0[0]) {
        DWORD attrs = GetFileAttributesA(argv0);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return copy_path(out, outsz, argv0);
        }
    }
#endif

#ifndef _WIN32
    /* #1204 (upstream 11bbf0d8, 5b96b067): never hand back a self-resolved
     * path that is not executable. After an installer's atomic rename-over the
     * image path no longer exists (Linux readlink reads "<path> (deleted)"),
     * and spawning it fails with ENOENT. */
    if (resolve_self_executable(out, outsz)) {
        if (is_executable_file(out)) {
            return true;
        }
#if defined(__linux__)
        /* The /proc/self/exe magic link still executes the in-memory old
         * build, the only image whose build fingerprint the worker accepts. */
        return copy_path(out, outsz, "/proc/self/exe");
#else
        /* No magic link: fail closed, and the supervisor degrades to
         * in-process indexing instead of spawning a missing binary. */
        out[0] = '\0';
        return false;
#endif
    }
#else
    if (resolve_self_executable(out, outsz)) {
        return true;
    }
#endif
    return copy_path(out, outsz, argv0);
}
