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

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "tree_sitter/api.h"
#include "zova/cbm_zova_operations.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_route.h"
#include <yyjson/yyjson.h>

/* Headers of the ported modules: the Zig side implements these. */
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "git/git_context.h"
#include "simhash/minhash.h"
#include "watcher/watcher.h"

#endif /* CBM_ZIG_C_H */
