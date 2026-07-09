#ifndef CBM_ZOVA_BRIDGE_H
#define CBM_ZOVA_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

int cbm_zova_bridge_build_enabled(void);
int cbm_zova_bridge_install_extension(const char *zova_path);
int cbm_zova_bridge_cosine_smoke(const char *zova_path, const int8_t *a, const int8_t *b,
                                 size_t len, double *out_score);
int cbm_zova_bridge_camel_smoke(const char *zova_path, const char *input, char *out,
                                size_t out_len);
int cbm_zova_bridge_regex_smoke(const char *zova_path, const char *pattern, const char *text,
                                int case_insensitive, int *out_match);
int cbm_zova_bridge_invalid_regex_smoke(const char *zova_path, char *out_error,
                                        size_t out_error_len);

#endif /* CBM_ZOVA_BRIDGE_H */
