#include "cbm_zova_edge_payload.h"

#include <limits.h>
#include <string.h>

enum {
    CBM_ZOVA_EDGE_PAYLOAD_VERSION = 1,
    CBM_ZOVA_EDGE_PAYLOAD_PROPERTIES = 1u << 0,
    CBM_ZOVA_EDGE_PAYLOAD_URL_PATH = 1u << 1,
    CBM_ZOVA_EDGE_PAYLOAD_LOCAL_NAME = 1u << 2,
    CBM_ZOVA_EDGE_PAYLOAD_ALL_FIELDS =
        CBM_ZOVA_EDGE_PAYLOAD_PROPERTIES |
        CBM_ZOVA_EDGE_PAYLOAD_URL_PATH |
        CBM_ZOVA_EDGE_PAYLOAD_LOCAL_NAME,
};

static const uint8_t payload_default_properties[] = "{}";
static const uint8_t payload_default_empty[] = "";

static const char *payload_properties(const CBMDumpEdge *edge) {
    return edge && edge->properties ? edge->properties : "{}";
}

static const char *payload_url_path(const CBMDumpEdge *edge) {
    return edge && edge->url_path ? edge->url_path : "";
}

static const char *payload_local_name(const CBMDumpEdge *edge) {
    return edge && edge->local_name ? edge->local_name : "";
}

static int payload_is_default(const CBMDumpEdge *edge) {
    return edge && strcmp(payload_properties(edge), "{}") == 0 &&
           payload_url_path(edge)[0] == '\0' && payload_local_name(edge)[0] == '\0';
}

static size_t payload_varint_size(size_t value) {
    size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        size++;
    }
    return size;
}

static int payload_add_size(size_t *total, size_t value) {
    if (!total || value > SIZE_MAX - *total) return -1;
    *total += value;
    return 0;
}

int cbm_zova_edge_payload_encoded_size(
    const CBMDumpEdge *const *edges, size_t edge_count, size_t *out_size) {
    if (!edges || edge_count == 0 || !out_size) return -1;
    for (size_t i = 0; i < edge_count; i++)
        if (!edges[i]) return -1;
    if (edge_count == 1 && payload_is_default(edges[0])) {
        *out_size = 0;
        return 0;
    }

    size_t size = 1;
    if (payload_add_size(&size, payload_varint_size(edge_count)) != 0) return -1;
    for (size_t i = 0; i < edge_count; i++) {
        const char *fields[] = {
            payload_properties(edges[i]), payload_url_path(edges[i]),
            payload_local_name(edges[i]),
        };
        if (payload_add_size(&size, 1) != 0) return -1;
        for (size_t field = 0; field < 3; field++) {
            int present = field == 0 ? strcmp(fields[field], "{}") != 0
                                     : fields[field][0] != '\0';
            if (!present) continue;
            size_t length = strlen(fields[field]);
            if (payload_add_size(&size, payload_varint_size(length)) != 0 ||
                payload_add_size(&size, length) != 0) return -1;
        }
    }
    *out_size = size;
    return 0;
}

static uint8_t *payload_write_varint(uint8_t *cursor, size_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7f);
        value >>= 7;
        *cursor++ = value ? (uint8_t)(byte | 0x80) : byte;
    } while (value);
    return cursor;
}

int cbm_zova_edge_payload_encode(
    const CBMDumpEdge *const *edges, size_t edge_count,
    uint8_t *out_payload, size_t payload_capacity, size_t *out_payload_len) {
    size_t required = 0;
    if (!out_payload_len ||
        cbm_zova_edge_payload_encoded_size(edges, edge_count, &required) != 0)
        return -1;
    if (required > payload_capacity || (required > 0 && !out_payload)) return -1;
    *out_payload_len = required;
    if (required == 0) return 0;

    uint8_t *cursor = out_payload;
    *cursor++ = CBM_ZOVA_EDGE_PAYLOAD_VERSION;
    cursor = payload_write_varint(cursor, edge_count);
    for (size_t i = 0; i < edge_count; i++) {
        const char *fields[] = {
            payload_properties(edges[i]), payload_url_path(edges[i]),
            payload_local_name(edges[i]),
        };
        uint8_t flags = 0;
        if (strcmp(fields[0], "{}") != 0) flags |= CBM_ZOVA_EDGE_PAYLOAD_PROPERTIES;
        if (fields[1][0]) flags |= CBM_ZOVA_EDGE_PAYLOAD_URL_PATH;
        if (fields[2][0]) flags |= CBM_ZOVA_EDGE_PAYLOAD_LOCAL_NAME;
        *cursor++ = flags;
        for (size_t field = 0; field < 3; field++) {
            if ((flags & (1u << field)) == 0) continue;
            size_t length = strlen(fields[field]);
            cursor = payload_write_varint(cursor, length);
            memcpy(cursor, fields[field], length);
            cursor += length;
        }
    }
    return (size_t)(cursor - out_payload) == required ? 0 : -1;
}

static int payload_read_varint(const uint8_t **cursor, const uint8_t *end,
                               size_t *out_value) {
    if (!cursor || !*cursor || !out_value) return -1;
    size_t value = 0;
    unsigned shift = 0;
    size_t bytes = 0;
    uint8_t byte = 0;
    do {
        if (*cursor == end || shift >= sizeof(size_t) * CHAR_BIT) return -1;
        byte = *(*cursor)++;
        if (shift == sizeof(size_t) * CHAR_BIT - 7 &&
            (byte & 0x7f) > (SIZE_MAX >> shift)) return -1;
        value |= (size_t)(byte & 0x7f) << shift;
        shift += 7;
        bytes++;
    } while (byte & 0x80);
    if (bytes != payload_varint_size(value)) return -1;
    *out_value = value;
    return 0;
}

static int payload_read_slice(const uint8_t **cursor, const uint8_t *end,
                              cbm_zova_edge_payload_slice_t *out_slice) {
    size_t length = 0;
    if (payload_read_varint(cursor, end, &length) != 0 ||
        length > (size_t)(end - *cursor)) return -1;
    out_slice->data = *cursor;
    out_slice->len = length;
    *cursor += length;
    return 0;
}

static int payload_walk(const uint8_t *payload, size_t payload_len,
                        cbm_zova_edge_payload_visitor_t visitor, void *context,
                        size_t *out_edge_count) {
    if (payload_len == 0) {
        cbm_zova_edge_payload_record_t record = {
            .properties = {payload_default_properties, 2},
            .url_path = {payload_default_empty, 0},
            .local_name = {payload_default_empty, 0},
        };
        if (visitor && visitor(&record, context) != 0) return -1;
        if (out_edge_count) *out_edge_count = 1;
        return 0;
    }
    if (!payload) return -1;
    const uint8_t *cursor = payload;
    const uint8_t *end = payload + payload_len;
    if (*cursor++ != CBM_ZOVA_EDGE_PAYLOAD_VERSION) return -1;
    size_t edge_count = 0;
    if (payload_read_varint(&cursor, end, &edge_count) != 0 || edge_count == 0)
        return -1;
    for (size_t i = 0; i < edge_count; i++) {
        if (cursor == end) return -1;
        uint8_t flags = *cursor++;
        if (flags & ~CBM_ZOVA_EDGE_PAYLOAD_ALL_FIELDS) return -1;
        cbm_zova_edge_payload_record_t record = {
            .properties = {payload_default_properties, 2},
            .url_path = {payload_default_empty, 0},
            .local_name = {payload_default_empty, 0},
        };
        cbm_zova_edge_payload_slice_t *fields[] = {
            &record.properties, &record.url_path, &record.local_name,
        };
        for (size_t field = 0; field < 3; field++)
            if ((flags & (1u << field)) != 0 &&
                payload_read_slice(&cursor, end, fields[field]) != 0) return -1;
        if (visitor && visitor(&record, context) != 0) return -1;
    }
    if (cursor != end) return -1;
    if (out_edge_count) *out_edge_count = edge_count;
    return 0;
}

int cbm_zova_edge_payload_visit(
    const uint8_t *payload, size_t payload_len,
    cbm_zova_edge_payload_visitor_t visitor, void *context,
    size_t *out_edge_count) {
    size_t count = 0;
    if (payload_walk(payload, payload_len, NULL, NULL, &count) != 0) return -1;
    if (visitor && payload_walk(payload, payload_len, visitor, context, NULL) != 0)
        return -1;
    if (out_edge_count) *out_edge_count = count;
    return 0;
}
