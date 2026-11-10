//! cbm_zova_plugin.zig — Statically linked Zova plugin "cbm": the read-only
//! table operations trace_path, edges and cypher.
//!
//! Implements src/zova/cbm_zova_plugin.h, which documents the operations.
//! Without Zova the descriptor is NULL and nothing else here is compiled.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const with_zova = c.CBM_WITH_ZOVA != 0;

const Value = c.zova_plugin_value_v1;
const Bytes = c.zova_plugin_bytes_v1;
const Host = c.zova_plugin_host_v1;
const DataRequest = c.zova_plugin_data_request_v1;
const DataPage = c.zova_plugin_data_page_v1;
const QueryRequest = c.zova_plugin_query_request_v1;
const Column = c.zova_plugin_operation_column_v1;
const Operation = c.zova_plugin_operation_v1;
const PayloadRecord = c.cbm_zova_edge_payload_record_t;

const OK: i32 = c.ZOVA_PLUGIN_OK;
const ERROR: i32 = c.ZOVA_PLUGIN_ERROR;
const OUT_OF_MEMORY: i32 = c.ZOVA_PLUGIN_OUT_OF_MEMORY;
const INVALID_ARGUMENT: i32 = c.ZOVA_PLUGIN_INVALID_ARGUMENT;
const LIMIT: i32 = c.ZOVA_PLUGIN_LIMIT;

const INTEGER: u32 = c.ZOVA_PLUGIN_VALUE_INTEGER;
const TEXT: u32 = c.ZOVA_PLUGIN_VALUE_TEXT;
const BLOB: u32 = c.ZOVA_PLUGIN_VALUE_BLOB;
const OUTGOING: u32 = c.ZOVA_PLUGIN_OUTGOING;
const INCOMING: u32 = c.ZOVA_PLUGIN_INCOMING;

const STORE_OK: c_int = c.CBM_STORE_OK;
const STORE_ERR: c_int = c.CBM_STORE_ERR;
const STORE_NOT_FOUND: c_int = c.CBM_STORE_NOT_FOUND;

const PLUGIN_EDGE_TYPES_MAX = 16;
const PLUGIN_NAME_MAX = 255;
const PLUGIN_FRONTIER_BATCH = 1024;
const PLUGIN_HYDRATE_BATCH = 128;
const PLUGIN_PAGE_ROWS = 4096;
const PLUGIN_PAGE_BYTES = 1024 * 1024;
const PLUGIN_MAX_RESULTS = 100000;
const PLUGIN_TEXT_COLUMNS = 5;
const PLUGIN_NODE_COLUMNS = 8;
const PLUGIN_TRACE_ARGUMENTS = 6;
const PLUGIN_TRACE_COLUMNS = 9;
const PLUGIN_NEIGHBOR_COLUMNS = 6;
const PLUGIN_PAYLOAD_BATCH = 256;
const PLUGIN_PAYLOAD_COLUMNS = 4;
const PLUGIN_EDGES_ARGUMENTS = 4;
const PLUGIN_EDGES_COLUMNS = 6;
const PLUGIN_SCAN_PAGE = 2048;
const PLUGIN_CYPHER_ARGUMENTS = 3;
const PLUGIN_CYPHER_COLUMNS = 2;

const PLUGIN_NODE_SELECT =
    "SELECT n.zova_node_key,n.label,n.name,n.qualified_name,f.file_path,n.start_line," ++
    "n.end_line,n.properties FROM cbm_nodes_v1 n JOIN cbm_files_v1 f " ++
    "ON f.file_key=n.file_key ";

/// The host services one hook or operation callback may use.
const Session = struct {
    connection: ?*anyopaque,
    data: [*c]const c.zova_plugin_data_service_v1,
    query: [*c]const c.zova_plugin_query_service_v1,
};

const ROW_LABEL = 0;
const ROW_NAME = 1;
const ROW_QN = 2;
const ROW_FILE = 3;
const ROW_PROPERTIES = 4;

/// One reached node. text holds label, name, qualified_name, file_path and
/// properties back to back.
const Row = struct {
    key: i64,
    start_line: i64,
    end_line: i64,
    hop: i32,
    text: [*c]u8,
    off: [PLUGIN_TEXT_COLUMNS]u64,
    len: [PLUGIN_TEXT_COLUMNS]u64,

    fn column(row: *const Row, index: usize) []const u8 {
        return (row.text + @as(usize, @intCast(row.off[index])))[0..@intCast(row.len[index])];
    }
};

const Trace = struct {
    rows: [*c]Row = null,
    count: usize = 0,
    cap: usize = 0,
    next: usize = 0,
    /// Visited node keys in discovery order; a level is a contiguous slice.
    keys: [*c]i64 = null,
    key_count: usize = 0,
    key_cap: usize = 0,
    /// Open-addressing set over keys (0 = empty; node keys are positive).
    set: [*c]i64 = null,
    set_cap: usize = 0,
    level: i32 = 0,
    last_ordinal: u64 = 0,
};

/// A malloc'd T with its default field values; NULL on OOM.
fn create(comptime T: type) ?*T {
    const item: ?*T = interop.mallocN(T, 1);
    if (item) |p| p.* = .{};
    return item;
}

/// The bytes of a TEXT or BLOB value.
fn valueBytes(value: *const Value) []const u8 {
    return if (value.bytes_len == 0) "" else value.bytes[0..@intCast(value.bytes_len)];
}

fn textValue(bytes: []const u8) Value {
    return .{ .kind = TEXT, .bytes = bytes.ptr, .bytes_len = bytes.len };
}

fn integerValue(integer: i64) Value {
    return .{ .kind = INTEGER, .integer = integer };
}

fn pluginService(host: [*c]const Host, connection: ?*anyopaque, id: u32, size: u32, out: *?*const anyopaque) i32 {
    out.* = null;
    if (host == null or host.*.struct_size < @sizeOf(c.zova_plugin_service_host_v1))
        return c.ZOVA_PLUGIN_UNSUPPORTED;
    const service_host: *const c.zova_plugin_service_host_v1 = @ptrCast(host);
    return service_host.get_service.?(connection, id, 1, size, out);
}

fn sessionOpen(host: [*c]const Host, connection: ?*anyopaque, session: *Session) i32 {
    var data: ?*const anyopaque = null;
    var query: ?*const anyopaque = null;
    var status = pluginService(host, connection, c.ZOVA_PLUGIN_SERVICE_DATA, @sizeOf(c.zova_plugin_data_service_v1), &data);
    if (status == OK) {
        status = pluginService(host, connection, c.ZOVA_PLUGIN_SERVICE_QUERY, @sizeOf(c.zova_plugin_query_service_v1), &query);
    }
    session.* = .{ .connection = connection, .data = @ptrCast(@alignCast(data)), .query = @ptrCast(@alignCast(query)) };
    return status;
}

fn traceFree(trace: ?*Trace) void {
    const t = trace orelse return;
    for (0..t.count) |i| c.free(t.rows[i].text);
    c.free(t.rows);
    c.free(t.keys);
    c.free(t.set);
    c.free(t);
}

fn setSlot(set: [*]const i64, cap: usize, key: i64) usize {
    const hash = @as(u64, @bitCast(key)) *% 0x9E3779B97F4A7C15;
    var slot = @as(usize, @truncate(hash)) & (cap - 1);
    while (set[slot] != 0 and set[slot] != key) slot = (slot + 1) & (cap - 1);
    return slot;
}

/// True when key was added, false when it was already present.
fn visit(trace: *Trace, key: i64) error{OutOfMemory}!bool {
    if ((trace.key_count + 1) * 2 > trace.set_cap) {
        const cap: usize = if (trace.set_cap != 0) trace.set_cap * 2 else 1024;
        const set = interop.callocN(i64, cap);
        if (set == null) return error.OutOfMemory;
        for (0..trace.key_count) |i| set[setSlot(set, cap, trace.keys[i])] = trace.keys[i];
        c.free(trace.set);
        trace.set = set;
        trace.set_cap = cap;
    }
    const slot = setSlot(trace.set, trace.set_cap, key);
    if (trace.set[slot] == key) return false;
    if (trace.key_count == trace.key_cap) {
        const cap: usize = if (trace.key_cap != 0) trace.key_cap * 2 else 1024;
        const keys = interop.reallocN(i64, trace.keys, cap);
        if (keys == null) return error.OutOfMemory;
        trace.keys = keys;
        trace.key_cap = cap;
    }
    trace.set[slot] = key;
    trace.keys[trace.key_count] = key;
    trace.key_count += 1;
    return true;
}

/// NEIGHBORS_KEYED row: ordinal, input_key, edge_key, neighbor_key, edge_type, order.
fn neighborRow(user_data: ?*anyopaque, values: [*c]const Value, count: u64) callconv(.c) i32 {
    const trace: *Trace = @ptrCast(@alignCast(user_data));
    if (count != PLUGIN_NEIGHBOR_COLUMNS or values[0].kind != INTEGER or values[0].integer < 0 or
        values[3].kind != INTEGER or values[3].integer <= 0)
        return ERROR;
    trace.last_ordinal = @intCast(values[0].integer);
    _ = visit(trace, values[3].integer) catch return OUT_OF_MEMORY;
    return OK;
}

/// Reads every matching neighbor of keys[first..first+count), following the keyed cursor.
fn expand(session: *const Session, trace: *Trace, request: *DataRequest, first_key: usize, key_count: usize) i32 {
    var first = first_key;
    var count = key_count;
    var after = std.mem.zeroes(c.zova_plugin_cursor_v1);
    while (count > 0) {
        var page = std.mem.zeroes(DataPage);
        // The row callback may grow trace.keys; re-read the pointer per page.
        request.keys = trace.keys + first;
        request.key_count = count;
        request.after = after;
        trace.last_ordinal = 0;
        const status = session.data.*.read.?(session.connection, request, &page);
        if (status != OK) return status;
        if (page.has_more == 0) break;
        if (page.rows == 0 or trace.last_ordinal >= count) return ERROR;
        first += @intCast(trace.last_ordinal);
        count -= @intCast(trace.last_ordinal);
        after = page.next;
    }
    return OK;
}

/// Node row: zova_node_key, label, name, qualified_name, file_path, start_line,
/// end_line, properties.
fn nodeRow(user_data: ?*anyopaque, values: [*c]const Value, count: u64) callconv(.c) i32 {
    const text_column = [PLUGIN_TEXT_COLUMNS]usize{ 1, 2, 3, 4, 7 };
    const trace: *Trace = @ptrCast(@alignCast(user_data));
    if (count != PLUGIN_NODE_COLUMNS or values[0].kind != INTEGER or values[5].kind != INTEGER or
        values[6].kind != INTEGER)
        return ERROR;
    if (trace.count == trace.cap) {
        const cap: usize = if (trace.cap != 0) trace.cap * 2 else 64;
        const rows = interop.reallocN(Row, trace.rows, cap);
        if (rows == null) return OUT_OF_MEMORY;
        trace.rows = rows;
        trace.cap = cap;
    }
    var row = Row{
        .key = values[0].integer,
        .start_line = values[5].integer,
        .end_line = values[6].integer,
        .hop = trace.level,
        .text = null,
        .off = undefined,
        .len = undefined,
    };
    var total: u64 = 0;
    for (text_column, 0..) |column, i| {
        if (values[column].kind != TEXT) return ERROR;
        row.off[i] = total;
        row.len[i] = values[column].bytes_len;
        total += values[column].bytes_len;
    }
    row.text = interop.mallocN(u8, if (total != 0) @intCast(total) else 1);
    if (row.text == null) return OUT_OF_MEMORY;
    for (text_column, 0..) |column, i| {
        if (row.len[i] != 0) @memcpy(@constCast(row.column(i)), valueBytes(values + column));
    }
    trace.rows[trace.count] = row;
    trace.count += 1;
    return OK;
}

fn rowsTruncate(trace: *Trace, count: usize) void {
    while (trace.count > count) {
        trace.count -= 1;
        c.free(trace.rows[trace.count].text);
    }
}

/// Loads the cbm_nodes_v1 rows of keys[first..first+count). Graph nodes without a
/// row are skipped. A batch over the query byte budget is retried in halves.
fn hydrate(session: *const Session, trace: *Trace, first_key: usize, key_count: usize) i32 {
    const head = PLUGIN_NODE_SELECT ++ "WHERE n.zova_node_key IN (";
    var sql: [head.len + PLUGIN_HYDRATE_BATCH * 2 + 2]u8 = undefined;
    var parameters: [PLUGIN_HYDRATE_BATCH]Value = undefined;
    var batch: usize = PLUGIN_HYDRATE_BATCH;
    var first = first_key;
    var count = key_count;
    while (count > 0) {
        const n = @min(count, batch);
        var len: usize = head.len;
        @memcpy(sql[0..len], head);
        for (0..n) |i| {
            if (i > 0) {
                sql[len] = ',';
                len += 1;
            }
            sql[len] = '?';
            len += 1;
            parameters[i] = integerValue(trace.keys[first + i]);
        }
        sql[len] = ')';
        len += 1;
        const request = QueryRequest{
            .struct_size = @sizeOf(QueryRequest),
            .sql = &sql,
            .sql_len = len,
            .parameters = &parameters,
            .parameter_count = n,
            .row_limit = n,
            .byte_limit = PLUGIN_PAGE_BYTES,
            .row = nodeRow,
            .user_data = trace,
        };
        const before = trace.count;
        const status = session.query.*.query.?(session.connection, &request);
        if (status == LIMIT and n > 1) {
            rowsTruncate(trace, before);
            batch = n / 2;
            continue;
        }
        if (status != OK) return status;
        first += n;
        count -= n;
    }
    return OK;
}

/// Result order: hop, file_path, qualified_name, label, start_line, end_line, key.
fn rowLessThan(_: void, a: Row, b: Row) bool {
    if (a.hop != b.hop) return a.hop < b.hop;
    for ([_]usize{ ROW_FILE, ROW_QN, ROW_LABEL }) |index| {
        const order = std.mem.order(u8, a.column(index), b.column(index));
        if (order != .eq) return order == .lt;
    }
    if (a.start_line != b.start_line) return a.start_line < b.start_line;
    if (a.end_line != b.end_line) return a.end_line < b.end_line;
    return a.key < b.key;
}

/// Splits "A,B" into at most PLUGIN_EDGE_TYPES_MAX non-empty names. An empty
/// list yields 0; a malformed one null.
fn edgeTypes(value: *const Value, out: *[PLUGIN_EDGE_TYPES_MAX]Bytes) ?usize {
    const text = valueBytes(value);
    if (text.len == 0) return 0;
    var count: usize = 0;
    var start: usize = 0;
    for (0..text.len + 1) |i| {
        if (i < text.len and text[i] != ',') continue;
        if (i == start or i - start > PLUGIN_NAME_MAX or count == PLUGIN_EDGE_TYPES_MAX) return null;
        out[count] = .{ .data = text[start..i].ptr, .len = i - start };
        count += 1;
        start = i + 1;
    }
    return count;
}

/// No names means every edge type.
fn requestEdgeTypes(request: *DataRequest, edge_types: []const Bytes) void {
    if (edge_types.len == 1) {
        request.edge_type = edge_types[0];
    } else if (edge_types.len > 1) {
        request.ids = edge_types.ptr;
        request.id_count = edge_types.len;
    }
}

fn direction(value: *const Value) ?u32 {
    const text = valueBytes(value);
    if (std.mem.eql(u8, text, "outbound")) return OUTGOING;
    if (std.mem.eql(u8, text, "inbound")) return INCOMING;
    return null;
}

/// Walks breadth-first from start_key and leaves the reached nodes in
/// trace.rows, ordered and cut to max_results.
fn traceRun(session: *const Session, trace: *Trace, graph: Bytes, start_key: i64, dir: u32, edge_types: []const Bytes, max_depth: i64, max_results: i64) i32 {
    _ = visit(trace, start_key) catch return OUT_OF_MEMORY;
    var request = DataRequest{
        .struct_size = @sizeOf(DataRequest),
        .operation = c.ZOVA_PLUGIN_GRAPH_NEIGHBORS_KEYED,
        .name = graph,
        .direction = dir,
        .row_limit = PLUGIN_PAGE_ROWS,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = neighborRow,
        .user_data = trace,
    };
    requestEdgeTypes(&request, edge_types);

    const result_limit: usize = @intCast(max_results);
    var level_start: usize = 0;
    var level_end: usize = 1;
    var hop: i64 = 1;
    while (hop <= max_depth and level_start < level_end) : (hop += 1) {
        var first = level_start;
        while (first < level_end) : (first += PLUGIN_FRONTIER_BATCH) {
            const count = @min(level_end - first, PLUGIN_FRONTIER_BATCH);
            const status = expand(session, trace, &request, first, count);
            if (status != OK) return status;
        }
        level_start = level_end;
        level_end = trace.key_count;
        trace.level = @truncate(hop);
        const status = hydrate(session, trace, level_start, level_end - level_start);
        if (status != OK) return status;
        // Later levels sort after this one, so they cannot enter the result.
        if (trace.count >= result_limit) break;
    }
    if (trace.count > 1) std.mem.sortUnstable(Row, trace.rows[0..trace.count], {}, rowLessThan);
    rowsTruncate(trace, result_limit);
    return OK;
}

fn traceOpen(host: [*c]const Host, connection: ?*anyopaque, state: ?*anyopaque, arguments: [*c]const Value, argument_count: u64, out_cursor: [*c]?*anyopaque) callconv(.c) i32 {
    _ = state;
    out_cursor.* = null;
    if (argument_count != PLUGIN_TRACE_ARGUMENTS) return INVALID_ARGUMENT;
    const graph = Bytes{ .data = arguments[0].bytes, .len = arguments[0].bytes_len };
    const start_key = arguments[1].integer;
    const max_depth = arguments[4].integer;
    const max_results = arguments[5].integer;
    var edge_types: [PLUGIN_EDGE_TYPES_MAX]Bytes = undefined;
    const edge_type_count = edgeTypes(arguments + 3, &edge_types) orelse 0;
    const dir = direction(arguments + 2) orelse return INVALID_ARGUMENT;
    if (graph.len == 0 or graph.len > PLUGIN_NAME_MAX or start_key <= 0 or edge_type_count == 0 or
        max_depth < 0 or max_results <= 0 or max_results > PLUGIN_MAX_RESULTS)
        return INVALID_ARGUMENT;
    var session: Session = undefined;
    const status = sessionOpen(host, connection, &session);
    if (status != OK) return status;
    const trace = create(Trace) orelse return OUT_OF_MEMORY;
    out_cursor.* = trace;
    return traceRun(&session, trace, graph, start_key, dir, edge_types[0..edge_type_count], max_depth, max_results);
}

fn traceNext(host: [*c]const Host, connection: ?*anyopaque, cursor: ?*anyopaque, row: c.zova_plugin_row_v1, row_context: ?*anyopaque, out_has_row: [*c]u32) callconv(.c) i32 {
    _ = host;
    _ = connection;
    const text_column = [PLUGIN_TEXT_COLUMNS]usize{ 2, 3, 4, 5, 8 };
    const trace: *Trace = @ptrCast(@alignCast(cursor));
    out_has_row.* = 0;
    if (trace.next >= trace.count) return OK;
    const item: *const Row = trace.rows + trace.next;
    trace.next += 1;
    var values: [PLUGIN_TRACE_COLUMNS]Value = undefined;
    values[0] = integerValue(item.key);
    values[1] = integerValue(item.hop);
    for (text_column, 0..) |column, i| values[column] = textValue(item.column(i));
    values[6] = integerValue(item.start_line);
    values[7] = integerValue(item.end_line);
    out_has_row.* = 1;
    return row.?(row_context, &values, PLUGIN_TRACE_COLUMNS);
}

fn traceClose(cursor: ?*anyopaque) callconv(.c) void {
    traceFree(@ptrCast(@alignCast(cursor)));
}

// ── edges ───────────────────────────────────────────────────────

/// One adjacency entry; type is copied because row bytes are borrowed.
const Neighbor = struct {
    edge_key: i64,
    neighbor_key: i64,
    type: [*c]u8,
    type_len: u64,
};

/// One cbm edge. text holds local_name then properties. A graph edge carries
/// one or more of these in its payload.
const Edge = struct {
    neighbor: usize,
    text: [*c]u8,
    local_name_len: u64,
    properties_len: u64,
};

const Edges = struct {
    neighbors: [*c]Neighbor = null,
    neighbor_count: usize = 0,
    neighbor_cap: usize = 0,
    edges: [*c]Edge = null,
    edge_count: usize = 0,
    edge_cap: usize = 0,
    next: usize = 0,
    node_key: i64 = 0,
    direction: u32 = 0,
    /// First neighbor of the payload batch being read, and the one being decoded.
    batch_first: usize = 0,
    batch_count: usize = 0,
    current: usize = 0,
};

fn edgesTruncate(state: *Edges, count: usize) void {
    while (state.edge_count > count) {
        state.edge_count -= 1;
        c.free(state.edges[state.edge_count].text);
    }
}

fn edgesFree(edges: ?*Edges) void {
    const state = edges orelse return;
    edgesTruncate(state, 0);
    for (0..state.neighbor_count) |i| c.free(state.neighbors[i].type);
    c.free(state.neighbors);
    c.free(state.edges);
    c.free(state);
}

fn edgesNeighborRow(user_data: ?*anyopaque, values: [*c]const Value, count: u64) callconv(.c) i32 {
    const state: *Edges = @ptrCast(@alignCast(user_data));
    if (count != PLUGIN_NEIGHBOR_COLUMNS or values[2].kind != INTEGER or values[3].kind != INTEGER or
        values[4].kind != TEXT)
        return ERROR;
    if (state.neighbor_count == state.neighbor_cap) {
        const cap: usize = if (state.neighbor_cap != 0) state.neighbor_cap * 2 else 64;
        const neighbors = interop.reallocN(Neighbor, state.neighbors, cap);
        if (neighbors == null) return OUT_OF_MEMORY;
        state.neighbors = neighbors;
        state.neighbor_cap = cap;
    }
    const name = valueBytes(values + 4);
    const copy = interop.mallocN(u8, if (name.len != 0) name.len else 1);
    if (copy == null) return OUT_OF_MEMORY;
    @memcpy(copy[0..name.len], name);
    state.neighbors[state.neighbor_count] = .{
        .edge_key = values[2].integer,
        .neighbor_key = values[3].integer,
        .type = copy,
        .type_len = name.len,
    };
    state.neighbor_count += 1;
    return OK;
}

fn edgesRecord(record: [*c]const PayloadRecord, context: ?*anyopaque) callconv(.c) c_int {
    const state: *Edges = @ptrCast(@alignCast(context));
    if (state.edge_count == state.edge_cap) {
        const cap: usize = if (state.edge_cap != 0) state.edge_cap * 2 else 64;
        const edges = interop.reallocN(Edge, state.edges, cap);
        if (edges == null) return -1;
        state.edges = edges;
        state.edge_cap = cap;
    }
    const local_name = record.*.local_name;
    const properties = record.*.properties;
    const total = local_name.len + properties.len;
    const text = interop.mallocN(u8, if (total != 0) total else 1);
    if (text == null) return -1;
    if (local_name.len != 0) @memcpy(text[0..local_name.len], local_name.data[0..local_name.len]);
    if (properties.len != 0) @memcpy(text[local_name.len..total], properties.data[0..properties.len]);
    state.edges[state.edge_count] = .{
        .neighbor = state.current,
        .text = text,
        .local_name_len = local_name.len,
        .properties_len = properties.len,
    };
    state.edge_count += 1;
    return 0;
}

/// EDGE_PAYLOADS_GET row: ordinal, found, input_key, payload.
fn edgesPayloadRow(user_data: ?*anyopaque, values: [*c]const Value, count: u64) callconv(.c) i32 {
    const state: *Edges = @ptrCast(@alignCast(user_data));
    if (count != PLUGIN_PAYLOAD_COLUMNS or values[0].kind != INTEGER or values[0].integer < 0 or
        @as(u64, @intCast(values[0].integer)) >= state.batch_count or values[1].kind != INTEGER or
        values[1].integer != 1 or values[3].kind != BLOB)
        return ERROR;
    state.current = state.batch_first + @as(usize, @intCast(values[0].integer));
    if (values[2].integer != state.neighbors[state.current].edge_key) return ERROR;
    const rc = c.cbm_zova_edge_payload_visit(values[3].bytes, @intCast(values[3].bytes_len), edgesRecord, state, null);
    return if (rc == 0) OK else ERROR;
}

/// Decodes the payloads of every collected neighbor edge. A batch over the byte
/// budget is retried in halves.
fn edgesPayloads(session: *const Session, state: *Edges, graph: Bytes) i32 {
    var keys: [PLUGIN_PAYLOAD_BATCH]i64 = undefined;
    var batch: usize = PLUGIN_PAYLOAD_BATCH;
    var first: usize = 0;
    while (first < state.neighbor_count) {
        const n = @min(state.neighbor_count - first, batch);
        for (0..n) |i| keys[i] = state.neighbors[first + i].edge_key;
        const request = DataRequest{
            .struct_size = @sizeOf(DataRequest),
            .operation = c.ZOVA_PLUGIN_GRAPH_EDGE_PAYLOADS_GET,
            .name = graph,
            .keys = &keys,
            .key_count = n,
            .row_limit = n,
            .byte_limit = PLUGIN_PAGE_BYTES,
            .row = edgesPayloadRow,
            .user_data = state,
        };
        var page = std.mem.zeroes(DataPage);
        const before = state.edge_count;
        state.batch_first = first;
        state.batch_count = n;
        const status = session.data.*.read.?(session.connection, &request, &page);
        if (status == LIMIT and n > 1) {
            edgesTruncate(state, before);
            batch = n / 2;
            continue;
        }
        if (status != OK) return status;
        if (page.rows != n) return ERROR;
        first += n;
    }
    return OK;
}

/// Collects the adjacency of node_key into state.neighbors.
fn edgesNeighbors(session: *const Session, state: *Edges, graph: Bytes, node_key: i64, dir: u32, edge_types: []const Bytes) i32 {
    state.node_key = node_key;
    state.direction = dir;
    var request = DataRequest{
        .struct_size = @sizeOf(DataRequest),
        .operation = c.ZOVA_PLUGIN_GRAPH_NEIGHBORS_KEYED,
        .name = graph,
        .keys = &state.node_key,
        .key_count = 1,
        .direction = dir,
        .row_limit = PLUGIN_PAGE_ROWS,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = edgesNeighborRow,
        .user_data = state,
    };
    requestEdgeTypes(&request, edge_types);
    while (true) {
        var page = std.mem.zeroes(DataPage);
        const status = session.data.*.read.?(session.connection, &request, &page);
        if (status != OK) return status;
        if (page.has_more == 0) return OK;
        if (page.rows == 0) return ERROR;
        request.after = page.next;
    }
}

fn edgesOpen(host: [*c]const Host, connection: ?*anyopaque, unused: ?*anyopaque, arguments: [*c]const Value, argument_count: u64, out_cursor: [*c]?*anyopaque) callconv(.c) i32 {
    _ = unused;
    out_cursor.* = null;
    if (argument_count != PLUGIN_EDGES_ARGUMENTS) return INVALID_ARGUMENT;
    const graph = Bytes{ .data = arguments[0].bytes, .len = arguments[0].bytes_len };
    var edge_types: [PLUGIN_EDGE_TYPES_MAX]Bytes = undefined;
    const edge_type_count = edgeTypes(arguments + 3, &edge_types) orelse return INVALID_ARGUMENT;
    const dir = direction(arguments + 2) orelse return INVALID_ARGUMENT;
    if (graph.len == 0 or graph.len > PLUGIN_NAME_MAX or arguments[1].integer <= 0)
        return INVALID_ARGUMENT;
    var session: Session = undefined;
    var status = sessionOpen(host, connection, &session);
    if (status != OK) return status;
    const state = create(Edges) orelse return OUT_OF_MEMORY;
    out_cursor.* = state;
    status = edgesNeighbors(&session, state, graph, arguments[1].integer, dir, edge_types[0..edge_type_count]);
    return if (status == OK) edgesPayloads(&session, state, graph) else status;
}

fn edgesNext(host: [*c]const Host, connection: ?*anyopaque, cursor: ?*anyopaque, row: c.zova_plugin_row_v1, row_context: ?*anyopaque, out_has_row: [*c]u32) callconv(.c) i32 {
    _ = host;
    _ = connection;
    const state: *Edges = @ptrCast(@alignCast(cursor));
    out_has_row.* = 0;
    if (state.next >= state.edge_count) return OK;
    const edge: *const Edge = state.edges + state.next;
    state.next += 1;
    const neighbor: *const Neighbor = state.neighbors + edge.neighbor;
    const outgoing = state.direction == OUTGOING;
    const local_name_len: usize = @intCast(edge.local_name_len);
    const values = [PLUGIN_EDGES_COLUMNS]Value{
        integerValue(neighbor.edge_key),
        integerValue(if (outgoing) state.node_key else neighbor.neighbor_key),
        integerValue(if (outgoing) neighbor.neighbor_key else state.node_key),
        .{ .kind = TEXT, .bytes = neighbor.type, .bytes_len = neighbor.type_len },
        .{ .kind = TEXT, .bytes = edge.text, .bytes_len = edge.local_name_len },
        .{ .kind = TEXT, .bytes = edge.text + local_name_len, .bytes_len = edge.properties_len },
    };
    out_has_row.* = 1;
    return row.?(row_context, &values, PLUGIN_EDGES_COLUMNS);
}

fn edgesClose(cursor: ?*anyopaque) callconv(.c) void {
    edgesFree(@ptrCast(@alignCast(cursor)));
}

// ── cypher ──────────────────────────────────────────────────────

/// Store backend for one workspace: answers the reads the Cypher engine makes
/// from the plugin services. Node ids are Zova node keys and edge ids Zova edge
/// keys, so id(n) and id(r) differ from the host store's hashed ids.
const Store = struct {
    session: Session,
    graph: Bytes = std.mem.zeroes(Bytes),
    graph_name: [c.CBM_ZOVA_WORKSPACE_ID_MAX + 32]u8 = undefined,
    workspace_key: i64 = 0,
    project: [*c]u8 = null,
};

/// A malloc'd NUL-terminated copy; NULL on OOM.
fn dup(bytes: []const u8) [*c]u8 {
    const copy = interop.mallocN(u8, bytes.len + 1);
    if (copy == null) return null;
    @memcpy(copy[0..bytes.len], bytes);
    copy[bytes.len] = 0;
    return copy;
}

fn nodeClear(node: *c.cbm_node_t) void {
    c.free(@constCast(node.project));
    c.free(@constCast(node.label));
    c.free(@constCast(node.name));
    c.free(@constCast(node.qualified_name));
    c.free(@constCast(node.file_path));
    c.free(@constCast(node.properties_json));
    node.* = std.mem.zeroes(c.cbm_node_t);
}

fn nodeFromRow(store: *const Store, row: *const Row, out: *c.cbm_node_t) bool {
    out.* = .{
        .id = row.key,
        .project = dup(std.mem.span(store.project)),
        .label = dup(row.column(ROW_LABEL)),
        .name = dup(row.column(ROW_NAME)),
        .qualified_name = dup(row.column(ROW_QN)),
        .file_path = dup(row.column(ROW_FILE)),
        .start_line = @truncate(row.start_line),
        .end_line = @truncate(row.end_line),
        .properties_json = dup(row.column(ROW_PROPERTIES)),
    };
    if (out.project != null and out.label != null and out.name != null and
        out.qualified_name != null and out.file_path != null and out.properties_json != null)
        return true;
    nodeClear(out);
    return false;
}

/// Workspace nodes ordered by name, qualified_name, key, optionally of one label.
/// Read in keyset pages; a page over the byte budget is retried at half size.
fn storeSearch(ctx: ?*anyopaque, params: [*c]const c.cbm_search_params_t, out: [*c]c.cbm_search_output_t) callconv(.c) c_int {
    const sql = PLUGIN_NODE_SELECT ++
        "WHERE n.workspace_key=?1 AND (?2='' OR n.label=?2) AND " ++
        "(?3=0 OR (n.name,n.qualified_name,n.zova_node_key)>(?4,?5,?6)) " ++
        "ORDER BY n.name,n.qualified_name,n.zova_node_key LIMIT ?7";
    const store: *const Store = @ptrCast(@alignCast(ctx));
    out.* = std.mem.zeroes(c.cbm_search_output_t);
    // The engine only scans by label; refuse filters this backend does not apply.
    if (params.*.name_pattern != null or params.*.qn_pattern != null or
        params.*.file_pattern != null or params.*.relationship != null or
        params.*.exclude_labels != null or params.*.exclude_entry_points or
        params.*.min_degree >= 0 or params.*.max_degree >= 0 or params.*.offset != 0)
        return STORE_ERR;
    const limit: usize = if (params.*.limit > 0) @intCast(params.*.limit) else 10;
    const label: []const u8 = if (params.*.label != null) std.mem.span(params.*.label) else "";
    const rows = create(Trace) orelse return STORE_ERR;
    defer traceFree(rows);
    var page: usize = PLUGIN_SCAN_PAGE;
    var rc = STORE_OK;
    while (rows.count < limit) {
        const n = @min(limit - rows.count, page);
        const last: ?*const Row = if (rows.count != 0) rows.rows + (rows.count - 1) else null;
        const parameters = [_]Value{
            integerValue(store.workspace_key),
            textValue(label),
            integerValue(@intFromBool(last != null)),
            textValue(if (last) |row| row.column(ROW_NAME) else ""),
            textValue(if (last) |row| row.column(ROW_QN) else ""),
            integerValue(if (last) |row| row.key else 0),
            integerValue(@intCast(n)),
        };
        const request = QueryRequest{
            .struct_size = @sizeOf(QueryRequest),
            .sql = sql,
            .sql_len = sql.len,
            .parameters = &parameters,
            .parameter_count = parameters.len,
            .row_limit = n,
            .byte_limit = PLUGIN_PAGE_BYTES,
            .row = nodeRow,
            .user_data = rows,
        };
        const before = rows.count;
        const status = store.session.query.*.query.?(store.session.connection, &request);
        if (status == LIMIT and n > 1) {
            rowsTruncate(rows, before);
            page = n / 2;
            continue;
        }
        if (status != OK) {
            rc = STORE_ERR;
            break;
        }
        if (rows.count - before < n) break;
    }
    var results: [*c]c.cbm_search_result_t = null;
    if (rc == STORE_OK and rows.count != 0) {
        results = interop.callocN(c.cbm_search_result_t, rows.count);
        if (results == null) rc = STORE_ERR;
    }
    var i: usize = 0;
    while (rc == STORE_OK and i < rows.count) : (i += 1) {
        const result: *c.cbm_search_result_t = results + i;
        if (!nodeFromRow(store, rows.rows + i, &result.node)) {
            rc = STORE_ERR;
            break;
        }
        out.*.count = @intCast(i + 1);
    }
    out.*.results = results;
    out.*.total = out.*.count;
    if (rc != STORE_OK) c.cbm_store_search_free(out);
    return rc;
}

fn storeFindNodeById(ctx: ?*anyopaque, id: i64, out: [*c]c.cbm_node_t) callconv(.c) c_int {
    const store: *const Store = @ptrCast(@alignCast(ctx));
    out.* = std.mem.zeroes(c.cbm_node_t);
    if (id <= 0) return STORE_NOT_FOUND;
    const rows = create(Trace) orelse return STORE_ERR;
    defer traceFree(rows);
    const added = visit(rows, id) catch return STORE_ERR;
    if (!added or hydrate(&store.session, rows, 0, 1) != OK) return STORE_ERR;
    if (rows.count == 0) return STORE_NOT_FOUND;
    return if (nodeFromRow(store, rows.rows, out)) STORE_OK else STORE_ERR;
}

/// NULL means outbound, like the host store.
fn storeDirection(text: [*c]const u8) ?u32 {
    if (text == null) return OUTGOING;
    const name = std.mem.span(text);
    if (std.mem.eql(u8, name, "outbound")) return OUTGOING;
    if (std.mem.eql(u8, name, "inbound")) return INCOMING;
    return null;
}

fn storeFindEdges(ctx: ?*anyopaque, node_id: i64, direction_name: [*c]const u8, type_name: [*c]const u8, out: [*c][*c]c.cbm_edge_t, count: [*c]c_int) callconv(.c) c_int {
    const store: *const Store = @ptrCast(@alignCast(ctx));
    out.* = null;
    count.* = 0;
    if (node_id <= 0) return STORE_ERR;
    const dir = storeDirection(direction_name) orelse return STORE_ERR;
    const edge_type = [1]Bytes{.{ .data = type_name, .len = if (type_name != null) c.strlen(type_name) else 0 }};
    if (edge_type[0].len > PLUGIN_NAME_MAX) return STORE_ERR;
    const state = create(Edges) orelse return STORE_ERR;
    defer edgesFree(state);
    const edge_types: []const Bytes = if (edge_type[0].len != 0) &edge_type else &.{};
    if (edgesNeighbors(&store.session, state, store.graph, node_id, dir, edge_types) != OK or
        edgesPayloads(&store.session, state, store.graph) != OK)
        return STORE_ERR;
    if (state.edge_count == 0) return STORE_OK;
    const edges = interop.callocN(c.cbm_edge_t, state.edge_count);
    if (edges == null) return STORE_ERR;
    const outgoing = dir == OUTGOING;
    for (0..state.edge_count) |i| {
        const edge: *const Edge = state.edges + i;
        const neighbor: *const Neighbor = state.neighbors + edge.neighbor;
        const local_name_len: usize = @intCast(edge.local_name_len);
        edges[i] = .{
            .id = neighbor.edge_key,
            .project = dup(std.mem.span(store.project)),
            .source_id = if (outgoing) node_id else neighbor.neighbor_key,
            .target_id = if (outgoing) neighbor.neighbor_key else node_id,
            .type = dup(neighbor.type[0..@intCast(neighbor.type_len)]),
            .properties_json = dup((edge.text + local_name_len)[0..@intCast(edge.properties_len)]),
        };
        if (edges[i].project == null or edges[i].type == null or edges[i].properties_json == null) {
            c.cbm_store_free_edges(edges, @intCast(i + 1));
            return STORE_ERR;
        }
    }
    out.* = edges;
    count.* = @intCast(state.edge_count);
    return STORE_OK;
}

/// CALLS edges in and out, like the host store.
fn storeNodeDegree(ctx: ?*anyopaque, node_id: i64, in_deg: [*c]c_int, out_deg: [*c]c_int) callconv(.c) c_int {
    const calls = [1]Bytes{.{ .data = "CALLS", .len = 5 }};
    const store: *const Store = @ptrCast(@alignCast(ctx));
    const degrees = [2][*c]c_int{ out_deg, in_deg };
    const directions = [2]u32{ OUTGOING, INCOMING };
    if (node_id <= 0) return STORE_ERR;
    for (degrees, directions) |degree, dir| {
        const state = create(Edges) orelse return STORE_ERR;
        defer edgesFree(state);
        const status = edgesNeighbors(&store.session, state, store.graph, node_id, dir, &calls);
        degree.* = @intCast(state.neighbor_count);
        if (status != OK) return STORE_ERR;
    }
    return STORE_OK;
}

fn storeBfs(ctx: ?*anyopaque, start_id: i64, direction_name: [*c]const u8, edge_types_in: [*c][*c]const u8, edge_type_count_in: c_int, max_depth: c_int, max_results: c_int, out: [*c]c.cbm_traverse_result_t) callconv(.c) c_int {
    const default_types = [1][*c]const u8{"CALLS"};
    const store: *const Store = @ptrCast(@alignCast(ctx));
    out.* = std.mem.zeroes(c.cbm_traverse_result_t);
    const result: *c.cbm_traverse_result_t = out;
    // The host store walks one direction per call too.
    const dir = storeDirection(direction_name) orelse return STORE_NOT_FOUND;
    var edge_types: [*c]const [*c]const u8 = edge_types_in;
    var edge_type_count = edge_type_count_in;
    if (edge_type_count <= 0) {
        edge_types = &default_types;
        edge_type_count = 1;
    }
    if (edge_type_count > PLUGIN_EDGE_TYPES_MAX or max_depth < 0 or max_results <= 0 or
        max_results > PLUGIN_MAX_RESULTS)
        return STORE_ERR;
    var types: [PLUGIN_EDGE_TYPES_MAX]Bytes = undefined;
    const type_count: usize = @intCast(edge_type_count);
    for (0..type_count) |i| {
        types[i] = .{ .data = edge_types[i], .len = c.strlen(edge_types[i]) };
        if (types[i].len == 0 or types[i].len > PLUGIN_NAME_MAX) return STORE_ERR;
    }
    var rc = storeFindNodeById(ctx, start_id, &result.root);
    if (rc != STORE_OK) return rc;
    rc = STORE_ERR;
    const trace = create(Trace);
    defer traceFree(trace);
    if (trace) |t| {
        if (traceRun(&store.session, t, store.graph, start_id, dir, types[0..type_count], max_depth, max_results) == OK) {
            if (t.count != 0) result.visited = interop.callocN(c.cbm_node_hop_t, t.count);
            rc = if (t.count != 0 and result.visited == null) STORE_ERR else STORE_OK;
            var i: usize = 0;
            while (rc == STORE_OK and i < t.count) : (i += 1) {
                const visited: *c.cbm_node_hop_t = result.visited + i;
                if (!nodeFromRow(store, t.rows + i, &visited.node)) {
                    rc = STORE_ERR;
                    break;
                }
                visited.hop = t.rows[i].hop;
                result.visited_count = @intCast(i + 1);
            }
        }
    }
    if (rc != STORE_OK) c.cbm_store_traverse_free(out);
    return rc;
}

const plugin_store_backend = c.cbm_store_backend_t{
    .search = storeSearch,
    .find_node_by_id = storeFindNodeById,
    .find_edges = storeFindEdges,
    .node_degree = storeNodeDegree,
    .bfs = storeBfs,
};

/// Workspace row: workspace_key, project.
fn workspaceRow(user_data: ?*anyopaque, values: [*c]const Value, count: u64) callconv(.c) i32 {
    const store: *Store = @ptrCast(@alignCast(user_data));
    if (count != 2 or values[0].kind != INTEGER or values[1].kind != TEXT) return ERROR;
    store.workspace_key = values[0].integer;
    store.project = dup(valueBytes(values + 1));
    return if (store.project != null) OK else OUT_OF_MEMORY;
}

/// Result rows as (kind, cells) pairs, in emission order.
const Cypher = struct {
    kinds: [*c][*c]const u8 = null,
    cells: [*c][*c]u8 = null,
    count: usize = 0,
    next: usize = 0,
};

fn cypherFree(cypher: ?*Cypher) void {
    const cursor = cypher orelse return;
    for (0..cursor.count) |i| c.free(cursor.cells[i]);
    c.free(@ptrCast(cursor.kinds));
    c.free(@ptrCast(cursor.cells));
    c.free(cursor);
}

/// Takes ownership of cells; NULL cells reports an allocation failure.
fn cypherPush(cursor: *Cypher, kind: [*:0]const u8, cells: [*c]u8) bool {
    if (cells == null) return false;
    cursor.kinds[cursor.count] = kind;
    cursor.cells[cursor.count] = cells;
    cursor.count += 1;
    return true;
}

fn jsonArray(values: [*c]const [*c]const u8, count: c_int) [*c]u8 {
    const doc = c.yyjson_mut_doc_new(null);
    if (doc == null) return null;
    const array = c.yyjson_mut_arr(doc);
    c.yyjson_mut_doc_set_root(doc, array);
    for (0..@intCast(@max(count, 0))) |i| {
        if (values[i] != null) {
            _ = c.yyjson_mut_arr_add_str(doc, array, values[i]);
        } else {
            _ = c.yyjson_mut_arr_add_null(doc, array);
        }
    }
    const json = c.yyjson_mut_write(doc, 0, null);
    c.yyjson_mut_doc_free(doc);
    return json;
}

fn cypherRun(session: *const Session, arguments: [*c]const Value, cursor: *Cypher) i32 {
    const workspace_sql =
        "SELECT r.workspace_key,p.project FROM cbm_workspace_registry r " ++
        "JOIN cbm_projects_v1 p ON p.workspace_key=r.workspace_key WHERE r.workspace_id=?1";
    var store = Store{ .session = session.* };
    const workspace_id = dup(valueBytes(arguments));
    const query = dup(valueBytes(arguments + 1));
    var handle: ?*c.cbm_store_t = null;
    var result = std.mem.zeroes(c.cbm_cypher_result_t);
    defer {
        c.cbm_cypher_result_free(&result);
        c.cbm_store_close(handle);
        c.free(store.project);
        c.free(workspace_id);
        c.free(query);
    }
    // Up to three rows besides the result rows: columns, warning, error.
    cursor.kinds = interop.callocN([*c]const u8, 3);
    cursor.cells = interop.callocN([*c]u8, 3);
    if (workspace_id == null or query == null or cursor.kinds == null or cursor.cells == null)
        return OUT_OF_MEMORY;
    if (c.strlen(workspace_id) != arguments[0].bytes_len or c.strlen(query) != arguments[1].bytes_len or
        c.cbm_zova_workspace_graph_name(workspace_id, &store.graph_name, store.graph_name.len) != 0)
        return INVALID_ARGUMENT;
    store.graph = .{ .data = &store.graph_name, .len = c.strlen(&store.graph_name) };
    const request = QueryRequest{
        .struct_size = @sizeOf(QueryRequest),
        .sql = workspace_sql,
        .sql_len = workspace_sql.len,
        .parameters = arguments,
        .parameter_count = 1,
        .row_limit = 1,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = workspaceRow,
        .user_data = &store,
    };
    const status = session.query.*.query.?(session.connection, &request);
    if (status != OK) return status;
    if (store.project == null)
        return if (cypherPush(cursor, "error", dup("workspace not found"))) OK else OUT_OF_MEMORY;
    handle = c.cbm_store_open_backend(&plugin_store_backend, &store);
    if (handle == null) return OUT_OF_MEMORY;
    if (c.cbm_cypher_execute(handle, query, store.project, @intCast(arguments[2].integer), &result) != 0) {
        const message: []const u8 = if (result.@"error" != null) std.mem.span(result.@"error") else "query failed";
        return if (cypherPush(cursor, "error", dup(message))) OK else OUT_OF_MEMORY;
    }
    const row_count: usize = @intCast(result.row_count);
    const capacity = row_count + 3;
    const kinds = interop.reallocN([*c]const u8, cursor.kinds, capacity);
    if (kinds == null) return OUT_OF_MEMORY;
    cursor.kinds = kinds;
    const cells = interop.reallocN([*c]u8, cursor.cells, capacity);
    if (cells == null) return OUT_OF_MEMORY;
    cursor.cells = cells;
    if (!cypherPush(cursor, "columns", jsonArray(result.columns, result.col_count)))
        return OUT_OF_MEMORY;
    for (0..row_count) |i| {
        if (!cypherPush(cursor, "row", jsonArray(result.rows[i], result.col_count)))
            return OUT_OF_MEMORY;
    }
    if (result.warning != null and !cypherPush(cursor, "warning", dup(std.mem.span(result.warning))))
        return OUT_OF_MEMORY;
    return OK;
}

fn cypherOpen(host: [*c]const Host, connection: ?*anyopaque, unused: ?*anyopaque, arguments: [*c]const Value, argument_count: u64, out_cursor: [*c]?*anyopaque) callconv(.c) i32 {
    _ = unused;
    out_cursor.* = null;
    if (argument_count != PLUGIN_CYPHER_ARGUMENTS or arguments[1].bytes_len == 0 or
        arguments[2].integer < 0 or arguments[2].integer > std.math.maxInt(i32))
        return INVALID_ARGUMENT;
    var session: Session = undefined;
    const status = sessionOpen(host, connection, &session);
    if (status != OK) return status;
    const cursor = create(Cypher) orelse return OUT_OF_MEMORY;
    out_cursor.* = cursor;
    return cypherRun(&session, arguments, cursor);
}

fn cypherNext(host: [*c]const Host, connection: ?*anyopaque, raw: ?*anyopaque, row: c.zova_plugin_row_v1, row_context: ?*anyopaque, out_has_row: [*c]u32) callconv(.c) i32 {
    _ = host;
    _ = connection;
    const cursor: *Cypher = @ptrCast(@alignCast(raw));
    out_has_row.* = 0;
    if (cursor.next >= cursor.count) return OK;
    const values = [PLUGIN_CYPHER_COLUMNS]Value{
        textValue(std.mem.span(cursor.kinds[cursor.next])),
        textValue(std.mem.span(cursor.cells[cursor.next])),
    };
    cursor.next += 1;
    out_has_row.* = 1;
    return row.?(row_context, &values, PLUGIN_CYPHER_COLUMNS);
}

fn cypherClose(cursor: ?*anyopaque) callconv(.c) void {
    cypherFree(@ptrCast(@alignCast(cursor)));
}

// ── registration ────────────────────────────────────────────────

fn tableColumn(comptime name: []const u8, kind: u32) Column {
    return .{ .name = .{ .data = name.ptr, .len = name.len }, .kind = kind, .nullable = 0 };
}

const trace_arguments = [PLUGIN_TRACE_ARGUMENTS]Column{
    tableColumn("graph", TEXT),
    tableColumn("start_key", INTEGER),
    tableColumn("direction", TEXT),
    tableColumn("edge_types", TEXT),
    tableColumn("max_depth", INTEGER),
    tableColumn("max_results", INTEGER),
};
const trace_columns = [PLUGIN_TRACE_COLUMNS]Column{
    tableColumn("node_key", INTEGER),
    tableColumn("hop", INTEGER),
    tableColumn("label", TEXT),
    tableColumn("name", TEXT),
    tableColumn("qualified_name", TEXT),
    tableColumn("file_path", TEXT),
    tableColumn("start_line", INTEGER),
    tableColumn("end_line", INTEGER),
    tableColumn("properties", TEXT),
};

const edges_arguments = [PLUGIN_EDGES_ARGUMENTS]Column{
    tableColumn("graph", TEXT),
    tableColumn("node_key", INTEGER),
    tableColumn("direction", TEXT),
    tableColumn("edge_types", TEXT),
};
const edges_columns = [PLUGIN_EDGES_COLUMNS]Column{
    tableColumn("edge_key", INTEGER),
    tableColumn("source_key", INTEGER),
    tableColumn("target_key", INTEGER),
    tableColumn("edge_type", TEXT),
    tableColumn("local_name", TEXT),
    tableColumn("properties", TEXT),
};

const cypher_arguments = [PLUGIN_CYPHER_ARGUMENTS]Column{
    tableColumn("workspace_id", TEXT),
    tableColumn("query", TEXT),
    tableColumn("max_rows", INTEGER),
};
const cypher_columns = [PLUGIN_CYPHER_COLUMNS]Column{
    tableColumn("kind", TEXT),
    tableColumn("cells", TEXT),
};

/// An exact, ordered table operation.
fn tableOperation(
    comptime name: []const u8,
    arguments: []const Column,
    columns: []const Column,
    open: c.zova_plugin_operation_open_v1,
    next: c.zova_plugin_operation_next_v1,
    close: c.zova_plugin_operation_close_v1,
) Operation {
    return .{
        .struct_size = @sizeOf(Operation),
        .kind = c.ZOVA_PLUGIN_OPERATION_TABLE,
        .flags = c.ZOVA_PLUGIN_OPERATION_EXACT | c.ZOVA_PLUGIN_OPERATION_ORDERED,
        .name = .{ .data = name.ptr, .len = name.len },
        .arguments = arguments.ptr,
        .argument_count = @intCast(arguments.len),
        .column_count = @intCast(columns.len),
        .columns = columns.ptr,
        .open = open,
        .next = next,
        .close = close,
    };
}

fn registerSql(host: [*c]const Host, connection: ?*anyopaque) callconv(.c) i32 {
    var raw: ?*const anyopaque = null;
    var status = pluginService(host, connection, c.ZOVA_PLUGIN_SERVICE_OPERATIONS, @sizeOf(c.zova_plugin_operation_service_v1), &raw);
    if (status != OK) return status;
    const operations: *const c.zova_plugin_operation_service_v1 = @ptrCast(@alignCast(raw));

    const trace = tableOperation("trace_path", &trace_arguments, &trace_columns, traceOpen, traceNext, traceClose);
    status = operations.register_operation.?(connection, &trace);
    if (status != OK) return status;

    const edges = tableOperation("edges", &edges_arguments, &edges_columns, edgesOpen, edgesNext, edgesClose);
    status = operations.register_operation.?(connection, &edges);
    if (status != OK) return status;

    const cypher = tableOperation("cypher", &cypher_arguments, &cypher_columns, cypherOpen, cypherNext, cypherClose);
    return operations.register_operation.?(connection, &cypher);
}

/// The plugin owns no tables. data_version names the cbm tables and graph layout
/// it reads (cbm_nodes_v1, cbm_files_v1, cbm_graph_<workspace>); a newer plugin
/// keeps opening older installs by listing them in accepted_data_versions.
const plugin_descriptor = c.zova_plugin_compatibility_descriptor_v1{
    .base = .{
        .base = .{
            .struct_size = @sizeOf(c.zova_plugin_compatibility_descriptor_v1),
            .abi_version = c.ZOVA_PLUGIN_ABI_V1,
            .flags = c.ZOVA_PLUGIN_REQUIRES_QUERY_V1 | c.ZOVA_PLUGIN_REQUIRES_DATA_V1 |
                c.ZOVA_PLUGIN_REQUIRES_OPERATIONS_V1 | c.ZOVA_PLUGIN_HAS_DATA_COMPATIBILITY_V1,
            .name = "cbm",
            .version = "0.1.0",
            .storage_prefix = "_zova_ext_cbm_",
            .zova_abi_min = "1.3.0",
            .capabilities = "",
            .register_sql = registerSql,
        },
    },
    .data_version = "1.0.0",
    .accepted_data_versions = "1.0.0",
};

/// NULL when the build has no Zova.
pub export fn cbm_zova_plugin_descriptor() ?*const c.struct_zova_plugin_descriptor_v1 {
    return if (with_zova) &plugin_descriptor.base.base else null;
}
