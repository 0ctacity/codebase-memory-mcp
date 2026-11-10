/*
 * cbm_zig_c.h — C declarations visible to the Zig modules.
 *
 * Zig 0.17 has no @cImport: the build runs `zig translate-c` on this header
 * and exposes the result as the Zig module "c" (see build.zig and
 * Makefile.cbm). Include only what a ported module calls, so a header that
 * translate-c cannot handle never blocks an unrelated module.
 *
 * Translated without CBM_WITH_ZOVA: the declarations used here do not depend
 * on it, and leaving it unset keeps the Zova SDK headers out of the build.
 */
#ifndef CBM_ZIG_C_H
#define CBM_ZIG_C_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <fnmatch.h>
#include <poll.h>
#endif
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>

/* Platform headers used by the Zig foundation modules. */
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <psapi.h>
#include "foundation/win_utf8.h"
#include <tre/regex.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <regex.h>
#include <sched.h>
#include <spawn.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <mimalloc.h>

#include "foundation/arena.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_regex.h"
#include "foundation/compat_thread.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/limits.h"
#include "foundation/log.h"
#include "foundation/log_internal.h"
#include "foundation/mem.h"
#include "foundation/diagnostics.h"
#include "foundation/dump_verify.h"
#include "foundation/file_lock.h"
#include "foundation/git_env.h"
#include "foundation/profile.h"
#include "foundation/subprocess.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "foundation/sha256_backend.h"
#include "foundation/slab_alloc.h"
#include "foundation/str_intern.h"
#include "foundation/str_util.h"
#include "foundation/system_info_internal.h"
#include "foundation/compat_fs_internal.h"
#include "foundation/yaml.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "semantic/semantic.h"
#include "store/store.h"
#include "tree_sitter/api.h"
#include "zova/cbm_zova_coordination.h"
#include "zova/cbm_zova_delta.h"
#include "zova/cbm_zova_operations.h"
#include "zova/cbm_zova_publish_model.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_route.h"
#include <sqlite3.h>
#include <yyjson/yyjson.h>

/* stdin/stdout/stderr are macros whose expansion differs per libc (a
 * variable on glibc, a function call on Windows), so translate-c cannot give
 * the Zig side one spelling. These accessors can. */
static inline FILE *cbm_zig_stdin(void) {
    return stdin;
}
static inline FILE *cbm_zig_stdout(void) {
    return stdout;
}
static inline FILE *cbm_zig_stderr(void) {
    return stderr;
}

/* Headers of the ported modules: the Zig side implements these. */
#include "cli/cli.h"
#include "cli/cli_zig.h"
#include "cli/progress_sink.h"
#include "cypher/cypher.h"
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "git/git_context.h"
#include "graph_buffer/graph_buffer.h"
#include "mcp/index_supervisor.h"
#include "simhash/minhash.h"
#include "watcher/watcher.h"

#endif /* CBM_ZIG_C_H */
