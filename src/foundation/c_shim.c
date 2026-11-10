/*
 * c_shim.c — The parts of the foundation library that have to stay C.
 *
 * Everything else in src/foundation is Zig (hash_table.c wraps a C macro
 * template and is separate). What is here:
 *   - cbm_arena_sprintf and cbm_log consume a va_list: Zig can define a
 *     C-variadic function only on some targets and cannot forward a va_list
 *     to vsnprintf. Each one does the va_list work and hands the rest to Zig
 *     (arena.zig, log.zig).
 *   - a size check on regex_t, which translate-c cannot see on glibc.
 */
#include "foundation/arena.h"
#include "foundation/compat_regex.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/log_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <xlocale.h>
#endif
#ifdef _WIN32
#include "../../vendored/tre/regex.h"
#else
#include <regex.h>
#endif

/* ── compat_regex.zig ─────────────────────────────────────────── */

/* compat_regex.zig stores the platform regex_t inside cbm_regex_t. glibc's
 * regex_t has bitfields, so the Zig side cannot take its size; this check
 * covers every platform. If it fires, increase cbm_regex_t.opaque size. */
_Static_assert(sizeof(regex_t) <= sizeof(cbm_regex_t),
               "cbm_regex_t opaque buffer too small for regex_t");

/* ── cbm_arena_sprintf ────────────────────────────────────────── */

/* On macOS every vsnprintf consults the process locale under an unfair lock
 * (localeconv_l inside __vfprintf), so parallel workers formatting type names
 * serialize on it (upstream 7b77fd48: 10 MB C# files took 68 s instead of under
 * a second). Each thread formats with a C locale object of its own, freed by a
 * pthread key destructor at thread exit, and the common short string is
 * formatted once into a stack buffer. */
#if defined(__APPLE__)
static _Thread_local locale_t tl_c_locale;
static pthread_key_t tl_c_locale_key;
static pthread_once_t tl_c_locale_once = PTHREAD_ONCE_INIT;

static void arena_c_locale_free(void *loc) {
    if (loc) {
        freelocale((locale_t)loc);
    }
}

static void arena_c_locale_key_init(void) {
    (void)pthread_key_create(&tl_c_locale_key, arena_c_locale_free);
}

static locale_t arena_c_locale(void) {
    if (!tl_c_locale) {
        tl_c_locale = newlocale(LC_ALL_MASK, "C", NULL);
        if (tl_c_locale) {
            pthread_once(&tl_c_locale_once, arena_c_locale_key_init);
            (void)pthread_setspecific(tl_c_locale_key, tl_c_locale);
        }
    }
    return tl_c_locale; /* NULL = the global locale */
}

#define ARENA_VSNPRINTF(buf, n, fmt, ap) vsnprintf_l((buf), (n), arena_c_locale(), (fmt), (ap))
#else
#define ARENA_VSNPRINTF(buf, n, fmt, ap) vsnprintf((buf), (n), (fmt), (ap))
#endif

enum { ARENA_SPRINTF_LOCAL = 512 };

char *cbm_arena_sprintf(CBMArena *a, const char *fmt, ...) {
    char local[ARENA_SPRINTF_LOCAL];
    va_list args;
    va_start(args, fmt);
    int needed = ARENA_VSNPRINTF(local, sizeof(local), fmt, args);
    va_end(args);

    if (needed < 0) {
        return NULL;
    }

    char *dst = (char *)cbm_arena_alloc(a, (size_t)needed + SKIP_ONE);
    if (!dst) {
        return NULL;
    }
    if ((size_t)needed < sizeof(local)) {
        memcpy(dst, local, (size_t)needed + SKIP_ONE);
        return dst;
    }

    va_start(args, fmt);
    ARENA_VSNPRINTF(dst, (size_t)needed + SKIP_ONE, fmt, args);
    va_end(args);
    return dst;
}

/* ── cbm_log ──────────────────────────────────────────────────── */

void cbm_log(CBMLogLevel level, const char *msg, ...) {
    cbm_log_line_t line;
    if (!cbm_log_line_begin(&line, level, msg)) {
        return;
    }

    va_list args;
    va_start(args, msg);
    for (;;) {
        const char *key = va_arg(args, const char *);
        if (!key) {
            break;
        }
        const char *val = va_arg(args, const char *);
        cbm_log_line_pair(&line, key, val);
    }
    va_end(args);

    cbm_log_line_end(&line);
}
