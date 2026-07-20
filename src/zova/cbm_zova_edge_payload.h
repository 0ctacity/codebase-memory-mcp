#ifndef CBM_ZOVA_EDGE_PAYLOAD_H
#define CBM_ZOVA_EDGE_PAYLOAD_H

#include "store/store.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t len;
} cbm_zova_edge_payload_slice_t;

typedef struct {
    cbm_zova_edge_payload_slice_t properties;
    cbm_zova_edge_payload_slice_t url_path;
    cbm_zova_edge_payload_slice_t local_name;
} cbm_zova_edge_payload_record_t;

typedef int (*cbm_zova_edge_payload_visitor_t)(
    const cbm_zova_edge_payload_record_t *record, void *context);

int cbm_zova_edge_payload_encoded_size(
    const CBMDumpEdge *const *edges, size_t edge_count, size_t *out_size);
int cbm_zova_edge_payload_encode(
    const CBMDumpEdge *const *edges, size_t edge_count,
    uint8_t *out_payload, size_t payload_capacity, size_t *out_payload_len);
int cbm_zova_edge_payload_visit(
    const uint8_t *payload, size_t payload_len,
    cbm_zova_edge_payload_visitor_t visitor, void *context,
    size_t *out_edge_count);

#endif
