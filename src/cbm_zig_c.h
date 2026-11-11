/*
 * cbm_zig_c.h — C declarations visible to the Zig modules.
 *
 * Zig 0.17 has no @cImport: the build runs `zig translate-c` on this header
 * and exposes the result as the Zig module "c" (see build.zig and
 * Makefile.cbm). Include only what a ported module calls, so a header that
 * translate-c cannot handle never blocks an unrelated module.
 *
 * The build passes -DCBM_WITH_ZOVA=0|1 to translate-c. With Zova, the SDK
 * headers zova.h and zova_plugin.h are translated too, and the Zig Zova modules compile their
 * real implementation instead of the no-Zova stubs.
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
#include <sys/statvfs.h>
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
#include <shellapi.h> /* CommandLineToArgvW: not in windows.h under WIN32_LEAN_AND_MEAN */
#include <signal.h>
#include <tlhelp32.h>
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

/* C11 atomics, as translate-c can read them. translate-c has no atomic
 * types, so a declaration that names one (the pipeline context's cancel flag,
 * the shared id counter) would be dropped together with everything that uses
 * it. Here an atomic type is its plain type and the atomic operations are the
 * compiler builtins, which keeps those declarations and their layout. The Zig
 * side reads and writes such fields with @atomicLoad / @atomicStore /
 * @atomicRmw. Only this translation unit sees the view: the C sources keep
 * the real <stdatomic.h>. */
#define __CLANG_STDATOMIC_H /* keeps clang's <stdatomic.h> out */
#define _Atomic
typedef _Bool atomic_bool;
typedef int atomic_int;
typedef unsigned int atomic_uint;
typedef long atomic_long;
typedef unsigned long atomic_ulong;
typedef long long atomic_llong;
typedef unsigned long long atomic_ullong;
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_consume __ATOMIC_CONSUME
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_release __ATOMIC_RELEASE
#define memory_order_acq_rel __ATOMIC_ACQ_REL
#define memory_order_seq_cst __ATOMIC_SEQ_CST
#define atomic_load(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_load_explicit(p, o) __atomic_load_n((p), (o))
#define atomic_fetch_add(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(p, v, o) __atomic_fetch_add((p), (v), (o))

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
#include "arena.h"
#include "cbm.h"
#include "helpers.h"
#include "lang_specs.h"
#include "lsp/c_lsp.h"
#include "lsp/go_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/rust_cargo.h"
#include "lsp/rust_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/zig_lsp.h"
#include "mcp/mcp.h"
#include "nomic/code_vectors.h"
#include "service_patterns.h"
#include "spawn_patterns.h"
#include "store/store.h"
#include "tree_sitter/api.h"
#include "xxhash/xxhash.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_bridge.h"
#include "zova/cbm_zova_coordination.h"
#include "zova/cbm_zova_delta.h"
#include "zova/cbm_zova_edge_payload.h"
#include "zova/cbm_zova_operations.h"
#include "zova/cbm_zova_plugin.h"
#include "zova/cbm_zova_publish_model.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_route.h"
#include "zova/cbm_zova_v5_snapshot.h"
#include "zova/cbm_zova_writer_gate.h"
#if CBM_WITH_ZOVA
#include <zova.h>
#include <zova_plugin.h>
#endif
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
#include "main.h"
#include "mcp/index_supervisor.h"
#include "pipeline/laravel_routing.h"
#include "pipeline/lsp_resolve.h"
#include "pipeline/spawns.h"
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/path_alias.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/result_spill.h"
#include "pipeline/worker_pool.h"
#include "semantic/ast_profile.h"
#include "semantic/semantic.h"
#include "simhash/minhash.h"
#include "watcher/watcher.h"

#endif /* CBM_ZIG_C_H */
