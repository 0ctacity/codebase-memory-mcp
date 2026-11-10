//! cbm_zova_edge_payload.zig — Compact encoding of the per-edge text fields
//! (properties, url_path, local_name) stored beside a Zova edge.
//!
//! Implements src/zova/cbm_zova_edge_payload.h. Layout: one version byte, a
//! varint edge count, then per edge a flags byte and a length-prefixed slice
//! for each present field. A single edge with all-default fields encodes to
//! zero bytes.

const std = @import("std");
const c = @import("c");

const Edge = c.CBMDumpEdge;
const Slice = c.cbm_zova_edge_payload_slice_t;
const Record = c.cbm_zova_edge_payload_record_t;

const PAYLOAD_VERSION: u8 = 1;
const PAYLOAD_PROPERTIES: u8 = 1 << 0;
const PAYLOAD_URL_PATH: u8 = 1 << 1;
const PAYLOAD_LOCAL_NAME: u8 = 1 << 2;
const PAYLOAD_ALL_FIELDS: u8 = PAYLOAD_PROPERTIES | PAYLOAD_URL_PATH | PAYLOAD_LOCAL_NAME;

const default_record = Record{
    .properties = .{ .data = "{}", .len = 2 },
    .url_path = .{ .data = "", .len = 0 },
    .local_name = .{ .data = "", .len = 0 },
};

/// properties, url_path and local_name with their defaults applied.
fn payloadFields(edge: *const Edge) [3][]const u8 {
    return .{
        if (edge.properties != null) std.mem.span(edge.properties) else "{}",
        if (edge.url_path != null) std.mem.span(edge.url_path) else "",
        if (edge.local_name != null) std.mem.span(edge.local_name) else "",
    };
}

fn fieldPresent(fields: [3][]const u8, field: usize) bool {
    return if (field == 0) !std.mem.eql(u8, fields[0], "{}") else fields[field].len != 0;
}

fn payloadIsDefault(edge: *const Edge) bool {
    const fields = payloadFields(edge);
    return !fieldPresent(fields, 0) and !fieldPresent(fields, 1) and !fieldPresent(fields, 2);
}

fn payloadVarintSize(value: usize) usize {
    var size: usize = 1;
    var v = value;
    while (v >= 0x80) {
        v >>= 7;
        size += 1;
    }
    return size;
}

fn encodedSize(edges: []const [*c]const Edge) ?usize {
    if (edges.len == 0) return null;
    for (edges) |edge| {
        if (edge == null) return null;
    }
    if (edges.len == 1 and payloadIsDefault(edges[0])) return 0;

    const add = std.math.add;
    var size = add(usize, 1, payloadVarintSize(edges.len)) catch return null;
    for (edges) |edge| {
        const fields = payloadFields(edge);
        size = add(usize, size, 1) catch return null;
        for (fields, 0..) |text, field| {
            if (!fieldPresent(fields, field)) continue;
            size = add(usize, size, payloadVarintSize(text.len)) catch return null;
            size = add(usize, size, text.len) catch return null;
        }
    }
    return size;
}

pub export fn cbm_zova_edge_payload_encoded_size(edges: [*c]const [*c]const Edge, edge_count: usize, out_size: ?*usize) c_int {
    const out = out_size orelse return -1;
    if (edges == null) return -1;
    out.* = encodedSize(edges[0..edge_count]) orelse return -1;
    return 0;
}

fn payloadWriteVarint(out: []u8, at: usize, value: usize) usize {
    var cursor = at;
    var v = value;
    while (true) {
        const byte: u8 = @intCast(v & 0x7f);
        v >>= 7;
        out[cursor] = if (v != 0) byte | 0x80 else byte;
        cursor += 1;
        if (v == 0) return cursor;
    }
}

pub export fn cbm_zova_edge_payload_encode(edges: [*c]const [*c]const Edge, edge_count: usize, out_payload: [*c]u8, payload_capacity: usize, out_payload_len: ?*usize) c_int {
    const out_len = out_payload_len orelse return -1;
    if (edges == null) return -1;
    const list = edges[0..edge_count];
    const required = encodedSize(list) orelse return -1;
    if (required > payload_capacity or (required > 0 and out_payload == null)) return -1;
    out_len.* = required;
    if (required == 0) return 0;

    const out = out_payload[0..required];
    out[0] = PAYLOAD_VERSION;
    var cursor = payloadWriteVarint(out, 1, edge_count);
    for (list) |edge| {
        const fields = payloadFields(edge);
        var flags: u8 = 0;
        for (0..3) |field| {
            if (fieldPresent(fields, field)) flags |= @as(u8, 1) << @intCast(field);
        }
        out[cursor] = flags;
        cursor += 1;
        for (fields, 0..) |text, field| {
            if (flags & (@as(u8, 1) << @intCast(field)) == 0) continue;
            cursor = payloadWriteVarint(out, cursor, text.len);
            @memcpy(out[cursor..][0..text.len], text);
            cursor += text.len;
        }
    }
    return if (cursor == required) 0 else -1;
}

/// Reads one canonical varint off the front of `cursor`.
fn payloadReadVarint(cursor: *[]const u8) ?usize {
    const bits = @bitSizeOf(usize);
    var value: usize = 0;
    var shift: u32 = 0;
    var bytes: usize = 0;
    while (true) {
        if (cursor.len == 0 or shift >= bits) return null;
        const byte = cursor.*[0];
        cursor.* = cursor.*[1..];
        const low: usize = byte & 0x7f;
        if (shift == bits - 7 and low > (@as(usize, std.math.maxInt(usize)) >> @intCast(shift))) return null;
        value |= low << @intCast(shift);
        shift += 7;
        bytes += 1;
        if (byte & 0x80 == 0) break;
    }
    if (bytes != payloadVarintSize(value)) return null;
    return value;
}

fn payloadReadSlice(cursor: *[]const u8) ?Slice {
    const length = payloadReadVarint(cursor) orelse return null;
    if (length > cursor.len) return null;
    const slice = Slice{ .data = cursor.ptr, .len = length };
    cursor.* = cursor.*[length..];
    return slice;
}

fn payloadWalk(payload: [*c]const u8, payload_len: usize, visitor: c.cbm_zova_edge_payload_visitor_t, context: ?*anyopaque) ?usize {
    if (payload_len == 0) {
        const record = default_record;
        if (visitor) |visit| {
            if (visit(&record, context) != 0) return null;
        }
        return 1;
    }
    if (payload == null) return null;
    var cursor: []const u8 = payload[0..payload_len];
    if (cursor[0] != PAYLOAD_VERSION) return null;
    cursor = cursor[1..];
    const edge_count = payloadReadVarint(&cursor) orelse return null;
    if (edge_count == 0) return null;
    for (0..edge_count) |_| {
        if (cursor.len == 0) return null;
        const flags = cursor[0];
        cursor = cursor[1..];
        if (flags & ~PAYLOAD_ALL_FIELDS != 0) return null;
        var record = default_record;
        const fields = [3]*Slice{ &record.properties, &record.url_path, &record.local_name };
        for (fields, 0..) |slot, field| {
            if (flags & (@as(u8, 1) << @intCast(field)) == 0) continue;
            slot.* = payloadReadSlice(&cursor) orelse return null;
        }
        if (visitor) |visit| {
            if (visit(&record, context) != 0) return null;
        }
    }
    if (cursor.len != 0) return null;
    return edge_count;
}

/// Validates the whole payload first, so the visitor never sees a record of a
/// payload that turns out to be malformed.
pub export fn cbm_zova_edge_payload_visit(payload: [*c]const u8, payload_len: usize, visitor: c.cbm_zova_edge_payload_visitor_t, context: ?*anyopaque, out_edge_count: ?*usize) c_int {
    const count = payloadWalk(payload, payload_len, null, null) orelse return -1;
    if (visitor != null and payloadWalk(payload, payload_len, visitor, context) == null) return -1;
    if (out_edge_count) |out| out.* = count;
    return 0;
}
