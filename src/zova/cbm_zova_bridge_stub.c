#include "zova/cbm_zova_bridge.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if !CBM_WITH_ZOVA

int cbm_zova_bridge_build_enabled(void) {
    return 0;
}

int cbm_zova_bridge_install_extension(const char *zova_path) {
    (void)zova_path;
    return -1;
}

int cbm_zova_bridge_cosine_smoke(const char *zova_path, const int8_t *a, const int8_t *b,
                                 size_t len, double *out_score) {
    (void)zova_path;
    (void)a;
    (void)b;
    (void)len;
    (void)out_score;
    return -1;
}

int cbm_zova_bridge_camel_smoke(const char *zova_path, const char *input, char *out,
                                size_t out_len) {
    (void)zova_path;
    (void)input;
    (void)out;
    (void)out_len;
    return -1;
}

int cbm_zova_bridge_regex_smoke(const char *zova_path, const char *pattern, const char *text,
                                int case_insensitive, int *out_match) {
    (void)zova_path;
    (void)pattern;
    (void)text;
    (void)case_insensitive;
    (void)out_match;
    return -1;
}

int cbm_zova_bridge_invalid_regex_smoke(const char *zova_path, char *out_error,
                                        size_t out_error_len) {
    (void)zova_path;
    (void)out_error;
    (void)out_error_len;
    return -1;
}

#endif
