#ifndef CBM_SHA256_BACKEND_H
#define CBM_SHA256_BACKEND_H

#include "foundation/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal digest context that may use a platform-accelerated SHA-256
 * implementation. The public cbm_sha256_ctx API and layout remain unchanged. */
typedef struct {
    union {
        max_align_t alignment;
        uint8_t bytes[256];
    } state;
} cbm_sha256_backend_ctx;

void cbm_sha256_backend_init(cbm_sha256_backend_ctx *c);
void cbm_sha256_backend_update(cbm_sha256_backend_ctx *c, const void *data, size_t len);
void cbm_sha256_backend_final(cbm_sha256_backend_ctx *c,
                              uint8_t out[CBM_SHA256_DIGEST_LEN]);
bool cbm_sha256_backend_is_accelerated(void);

#endif /* CBM_SHA256_BACKEND_H */
