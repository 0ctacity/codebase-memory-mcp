/*
 * log_internal.h — Line builder behind the variadic cbm_log().
 *
 * cbm_log() takes a NULL-terminated key/value list. Zig cannot walk a C
 * va_list portably, so the variadic entry point stays in c_shim.c and
 * feeds each pair to these functions, which log.zig implements. Nothing else
 * should call them.
 */
#ifndef CBM_FOUNDATION_LOG_INTERNAL_H
#define CBM_FOUNDATION_LOG_INTERNAL_H

#include "foundation/constants.h"
#include "foundation/log.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char buf[CBM_SZ_4K];
    size_t pos; /* bytes the full line needs so far; may exceed sizeof(buf) */
    bool json;
} cbm_log_line_t;

/* Start a line with the level and message. Returns false when `level` is
 * below the runtime log level: the caller must then skip the line. */
bool cbm_log_line_begin(cbm_log_line_t *line, CBMLogLevel level, const char *msg);

/* Append one key/value pair. A NULL value is logged as "". */
void cbm_log_line_pair(cbm_log_line_t *line, const char *key, const char *val);

/* Terminate the line and write it to the sink and/or stderr. */
void cbm_log_line_end(cbm_log_line_t *line);

#endif /* CBM_FOUNDATION_LOG_INTERNAL_H */
