#ifndef CBM_ZOVA_ROUTE_H
#define CBM_ZOVA_ROUTE_H

#include <stddef.h>

typedef enum {
    CBM_ZOVA_ROUTE_COMPATIBILITY = 0,
    CBM_ZOVA_ROUTE_FULL_AUTHORITY = 1,
    CBM_ZOVA_ROUTE_MIGRATION_LEGACY = 2,
} cbm_zova_route_t;

cbm_zova_route_t cbm_zova_route_from_env(void);
cbm_zova_route_t cbm_zova_route_for_project(const char *project);
int cbm_zova_user_database_path(char *out, size_t out_size);

#endif
