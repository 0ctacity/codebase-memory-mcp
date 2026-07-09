#ifndef CBM_ZOVA_H
#define CBM_ZOVA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
typedef struct zova_database zova_database;
#endif

typedef enum {
    CBM_ZOVA_MODE_OFF = 0,
    CBM_ZOVA_MODE_CONTAINER = 1,
    CBM_ZOVA_MODE_I8_VECTORS = 2,
    CBM_ZOVA_MODE_GRAPH_MIRROR = 3,
} cbm_zova_mode_t;

typedef struct {
    int64_t node_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    int8_t *vector;
    int vector_len;
    double first_score;
} cbm_zova_node_candidate_t;

#define CBM_ZOVA_NODE_COLLECTION "cbm_node_vectors_i8"
#define CBM_ZOVA_TOKEN_COLLECTION "cbm_token_vectors_i8"
#define CBM_ZOVA_CODE_GRAPH "cbm_code_graph"

bool cbm_zova_build_enabled(void);
cbm_zova_mode_t cbm_zova_mode_from_env(void);
const char *cbm_zova_mode_name(cbm_zova_mode_t mode);

int cbm_zova_sidecar_path(const char *db_path, char *out, size_t out_sz);
int cbm_zova_after_sqlite_dump(const char *db_path);
int cbm_zova_validate_container(const char *zova_path);
int cbm_zova_mirror_i8_vectors(const char *zova_path, int vector_dim);
int cbm_zova_mirror_graph(const char *zova_path);

#if CBM_WITH_ZOVA
int cbm_zova_register_sql_functions(zova_database *db);
#endif

int cbm_zova_vector_prefetch_nodes(const char *zova_path, const char *project,
                                   const int8_t *query, int vector_dim, int limit,
                                   cbm_zova_node_candidate_t **out, int *out_count);
void cbm_zova_node_candidates_free(cbm_zova_node_candidate_t *items, int count);

int cbm_zova_graph_neighbor_count(const char *zova_path, const char *node_id, int *out_count);

#endif /* CBM_ZOVA_H */
