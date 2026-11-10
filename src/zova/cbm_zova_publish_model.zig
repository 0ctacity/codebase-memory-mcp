//! cbm_zova_publish_model.zig — The normalized form of one workspace
//! generation: stable ids, rows sorted by id, topology payloads and the
//! digests of every component.
//!
//! Implements src/zova/cbm_zova_publish_model.h. A model borrows the dump
//! arrays of its input and owns only the id, FTS and payload storage.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const CStr = interop.CStr;
const max_usize = std.math.maxInt(usize);
const Input = c.cbm_zova_workspace_generation_input_t;
const Result = c.cbm_zova_workspace_generation_result_t;
const Metrics = c.cbm_zova_publish_model_metrics_t;
const PublishNode = c.cbm_zova_publish_node_t;
const PublishEdge = c.cbm_zova_publish_edge_t;
const PublishNodeVector = c.cbm_zova_publish_node_vector_t;
const PublishTokenVector = c.cbm_zova_publish_token_vector_t;
const FileHash = c.cbm_zova_file_hash_input_t;
const Summary = c.cbm_zova_project_summary_input_t;
const DumpNode = c.CBMDumpNode;
const DumpEdge = c.CBMDumpEdge;
const Sha = c.cbm_sha256_backend_ctx;

const MODEL_ID_CAP = c.CBM_ZOVA_WORKSPACE_ID_MAX + 80;
const MODEL_PATH_CAP = 4096;
const MODEL_DISCRIMINATOR_CAP = 512;

/// Every way a build fails: invalid input or a failed allocation.
const BuildError = error{BuildFailed};

const ModelNode = extern struct {
    value: PublishNode,
    fts_name: [*c]u8,
};

const IdMap = extern struct {
    dump_id: i64,
    stable_id: CStr,
    ordinal: u64,
};

const DigestState = extern struct {
    metadata: Sha,
    fts: Sha,
    topology: Sha,
    nodes: Sha,
    tokens: Sha,
};

const Model = extern struct {
    workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8,
    input: Input,
    nodes: [*c]ModelNode,
    node_id_storage: [*c]u8,
    node_fts_storage: [*c]u8,
    node_count: c_int,
    dense_ids: [*c]CStr,
    dense_ordinals: [*c]u64,
    id_map: [*c]IdMap,
    edges: [*c]PublishEdge,
    edge_id_storage: [*c]u8,
    edge_count: c_int,
    topology: [*c]PublishEdge,
    topology_count: c_int,
    edge_payload_storage: [*c]u8,
    hashes: [*c][*c]const FileHash,
    hash_count: c_int,
    node_vectors: [*c]PublishNodeVector,
    node_vector_count: c_int,
    token_vectors: [*c]PublishTokenVector,
    token_id_storage: [*c]u8,
    token_vector_count: c_int,
    digests: Result,
    metrics: Metrics,
    pending_digests: DigestState,
    prepared_pending: bool,
};

/// Test hooks; atomic because index runs may build models concurrently.
var model_fail_allocation_at = std.atomic.Value(i64).init(-1);
var model_allocation_count = std.atomic.Value(i64).init(0);
var model_build_count = std.atomic.Value(u64).init(0);

fn modelAllocationFails() bool {
    const index = model_allocation_count.fetchAdd(1, .seq_cst);
    return index == model_fail_allocation_at.load(.seq_cst);
}

fn modelMalloc(comptime T: type, count: usize) [*c]T {
    return if (modelAllocationFails()) null else interop.mallocN(T, count);
}

fn modelCalloc(comptime T: type, count: usize) [*c]T {
    return if (modelAllocationFails()) null else interop.callocN(T, count);
}

pub export fn cbm_zova_publish_model_test_fail_allocation_at(index: i64) void {
    model_fail_allocation_at.store(index, .seq_cst);
    model_allocation_count.store(0, .seq_cst);
}

pub export fn cbm_zova_publish_model_test_allocation_count() i64 {
    return model_allocation_count.load(.seq_cst);
}

pub export fn cbm_zova_publish_model_test_reset_build_count() void {
    model_build_count.store(0, .seq_cst);
}

pub export fn cbm_zova_publish_model_test_build_count() u64 {
    return model_build_count.load(.seq_cst);
}

fn modelNow() c.struct_timespec {
    var now = std.mem.zeroes(c.struct_timespec);
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &now);
    return now;
}

fn modelElapsedMs(started: c.struct_timespec, finished: c.struct_timespec) f64 {
    return @as(f64, @floatFromInt(finished.tv_sec - started.tv_sec)) * 1000.0 +
        @as(f64, @floatFromInt(finished.tv_nsec - started.tv_nsec)) / 1000000.0;
}

/// Milliseconds since `phase_started`, which then moves to now.
fn modelLap(phase_started: *c.struct_timespec) f64 {
    const finished = modelNow();
    defer phase_started.* = finished;
    return modelElapsedMs(phase_started.*, finished);
}

fn modelNonzero(values: [*c]const u8, length: c_int) bool {
    if (values == null or length < 0) return false;
    for (values[0..@intCast(length)]) |value| {
        if (value != 0) return true;
    }
    return false;
}

fn hexInto(out: []u8, bytes: []const u8) void {
    const digits = "0123456789abcdef";
    for (bytes, 0..) |byte, i| {
        out[i * 2] = digits[byte >> 4];
        out[i * 2 + 1] = digits[byte & 0x0f];
    }
}

fn hashBytes(hash: *c.cbm_sha256_ctx, bytes: []const u8) void {
    c.cbm_sha256_update(hash, bytes.ptr, bytes.len);
}

/// "<prefix>:<workspace>:<first 16 digest bytes in hex>". `out` holds at
/// least the id stride of modelIdStorageLayout.
fn modelFinishId(hash: *c.cbm_sha256_ctx, prefix: []const u8, workspace_id: CStr, out: [*]u8) BuildError!void {
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    var hex: [32]u8 = undefined;
    c.cbm_sha256_final(hash, &digest);
    hexInto(&hex, digest[0..16]);
    const written = interop.bprint(out[0..MODEL_ID_CAP], "{s}:{s}:{s}", .{ prefix, std.mem.span(workspace_id), &hex });
    if (written >= MODEL_ID_CAP) return error.BuildFailed;
}

fn modelNodeId(workspace_id: CStr, maybe_node: ?*const DumpNode, out: [*]u8) BuildError!void {
    const node = maybe_node orelse return error.BuildFailed;
    if (node.label == null or node.name == null or node.qualified_name == null or node.file_path == null)
        return error.BuildFailed;
    const path = std.mem.span(node.file_path);
    const qualified = std.mem.span(node.qualified_name);
    if (path.len >= MODEL_PATH_CAP) return error.BuildFailed;

    var hash: c.cbm_sha256_ctx = undefined;
    c.cbm_sha256_init(&hash);
    hashBytes(&hash, std.mem.span(node.label));
    hashBytes(&hash, "\x00");
    // The path is hashed with '/' separators on every platform.
    var rest = path;
    while (rest.len > 0) {
        const separator = std.mem.indexOfScalar(u8, rest, '\\') orelse {
            hashBytes(&hash, rest);
            break;
        };
        hashBytes(&hash, rest[0..separator]);
        hashBytes(&hash, "/");
        rest = rest[separator + 1 ..];
    }
    hashBytes(&hash, "\x00");
    hashBytes(&hash, qualified);
    hashBytes(&hash, "\x00");
    if (qualified.len > 0) {
        const prefix = "named:";
        if (qualified.len > MODEL_DISCRIMINATOR_CAP - (prefix.len + 1)) return error.BuildFailed;
        hashBytes(&hash, prefix);
        hashBytes(&hash, qualified);
    } else {
        var discriminator: [MODEL_DISCRIMINATOR_CAP]u8 = undefined;
        if (node.name[0] == 0 or node.start_line < 0 or node.end_line < node.start_line)
            return error.BuildFailed;
        const length = interop.bprint(&discriminator, "local:{s}:span:{d}:{d}", .{ std.mem.span(node.name), node.start_line, node.end_line });
        if (length >= discriminator.len) return error.BuildFailed;
        hashBytes(&hash, discriminator[0..length]);
    }
    hashBytes(&hash, "\x00");
    return modelFinishId(&hash, "n:v2", workspace_id, out);
}

fn modelHashId(prefix: []const u8, workspace_id: CStr, parts: []const CStr, out: [*]u8) BuildError!void {
    var hash: c.cbm_sha256_ctx = undefined;
    c.cbm_sha256_init(&hash);
    for (parts) |part| {
        if (part == null) return error.BuildFailed;
        hashBytes(&hash, std.mem.span(part));
        hashBytes(&hash, "\x00");
    }
    return modelFinishId(&hash, prefix, workspace_id, out);
}

/// A NULL `local_name` reads as "".
fn modelEdgeId(workspace_id: CStr, source: CStr, edge_type: CStr, target: CStr, local_name: CStr, out: [*]u8) BuildError!void {
    return modelHashId("e:v1", workspace_id, &.{ source, edge_type, target, if (local_name != null) local_name else "" }, out);
}

fn modelTokenId(workspace_id: CStr, token: CStr, out: [*]u8) BuildError!void {
    return modelHashId("t:v1", workspace_id, &.{token}, out);
}

fn modelCamelBoundary(input: []const u8, index: usize) bool {
    if (index == 0) return false;
    const current = input[index];
    const previous = input[index - 1];
    const next: u8 = if (index + 1 < input.len) input[index + 1] else 0;
    if (std.ascii.isUpper(current) and std.ascii.isLower(previous)) return true;
    return std.ascii.isUpper(current) and std.ascii.isUpper(previous) and std.ascii.isLower(next);
}

const CamelSize = struct { size: usize, length: usize };

/// Bytes modelCamelNameInto writes for `input`, and the length of `input`.
fn modelCamelStorageSize(input: CStr) ?CamelSize {
    if (input == null) return null;
    const name = std.mem.span(input);
    var boundaries: usize = 0;
    for (0..name.len) |i| {
        if (modelCamelBoundary(name, i)) boundaries += 1;
    }
    if (name.len > (max_usize - 2) / 2 or boundaries > max_usize - (name.len * 2) - 2) return null;
    return .{ .size = name.len * 2 + boundaries + 2, .length = name.len };
}

/// "<name> <name with a space before every camel-case boundary>".
fn modelCamelNameInto(input: CStr, length: usize, value: [*c]u8, capacity: usize) BuildError!void {
    if (input == null or value == null or length > (max_usize - 2) / 2 or capacity < length * 2 + 2)
        return error.BuildFailed;
    const name = input[0..length];
    @memcpy(value[0..length], name);
    var output = length;
    value[output] = ' ';
    output += 1;
    for (name, 0..) |char, i| {
        if (modelCamelBoundary(name, i)) {
            value[output] = ' ';
            output += 1;
        }
        value[output] = char;
        output += 1;
    }
    value[output] = 0;
}

const IdStorageLayout = struct { stride: usize, bytes: usize };

fn modelIdStorageLayout(workspace_id: CStr, count: usize) ?IdStorageLayout {
    if (workspace_id == null) return null;
    const workspace_length = std.mem.span(workspace_id).len;
    // n:v2:<workspace>:<32 hex>, e:v1:..., and t:v1:... are equal-width.
    if (workspace_length > max_usize - 39) return null;
    const stride = workspace_length + 39;
    if (count > max_usize / stride or stride > MODEL_ID_CAP) return null;
    return .{ .stride = stride, .bytes = count * stride };
}

// ── qsort comparators ──────────────────────────────────────────────

fn cast(comptime T: type, ptr: ?*const anyopaque) *const T {
    return @ptrCast(@alignCast(ptr.?));
}

fn compare(left: anytype, right: @TypeOf(left)) c_int {
    return if (left < right) -1 else if (left > right) 1 else 0;
}

fn modelIdCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    return compare(cast(IdMap, left_ptr).dump_id, cast(IdMap, right_ptr).dump_id);
}

fn modelNodeCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(ModelNode, left_ptr).value.stable_id, cast(ModelNode, right_ptr).value.stable_id);
}

fn modelNullableCompare(left: CStr, right: CStr) c_int {
    return c.strcmp(if (left != null) left else "", if (right != null) right else "");
}

fn modelEdgeCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(PublishEdge, left_ptr);
    const right = cast(PublishEdge, right_ptr);
    var comparison = c.strcmp(left.edge_id, right.edge_id);
    if (comparison != 0) return comparison;
    comparison = c.strcmp(left.source_stable_id, right.source_stable_id);
    if (comparison != 0) return comparison;
    comparison = modelNullableCompare(left.source.*.type, right.source.*.type);
    if (comparison != 0) return comparison;
    comparison = c.strcmp(left.target_stable_id, right.target_stable_id);
    if (comparison != 0) return comparison;
    comparison = modelNullableCompare(left.source.*.properties, right.source.*.properties);
    if (comparison != 0) return comparison;
    return modelNullableCompare(left.source.*.url_path, right.source.*.url_path);
}

fn modelTopologyCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(PublishEdge, left_ptr);
    const right = cast(PublishEdge, right_ptr);
    var comparison = compare(left.source_ordinal, right.source_ordinal);
    if (comparison != 0) return comparison;
    comparison = c.strcmp(left.source.*.type, right.source.*.type);
    if (comparison != 0) return comparison;
    return compare(left.target_ordinal, right.target_ordinal);
}

const TopologySeed = extern struct {
    source: [*c]const DumpEdge,
    source_ordinal: u64,
    target_ordinal: u64,
};

fn preparedTopologySeedCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(TopologySeed, left_ptr);
    const right = cast(TopologySeed, right_ptr);
    if (left.source_ordinal != right.source_ordinal) return compare(left.source_ordinal, right.source_ordinal);
    const left_type = left.source.*.type;
    const right_type = right.source.*.type;
    const comparison = if (left_type == right_type) 0 else c.strcmp(left_type, right_type);
    if (comparison != 0) return comparison;
    return compare(left.target_ordinal, right.target_ordinal);
}

const GroupEdge = extern struct {
    source: [*c]const DumpEdge,
    edge_id: [MODEL_ID_CAP]u8,
};

const PayloadGroup = extern struct {
    topology_index: usize,
    edge_offset: usize,
    edge_count: usize,
    payload_size: usize,
};

fn preparedEdgePayloadIsDefault(edge: [*c]const DumpEdge) bool {
    if (edge == null) return false;
    const properties: CStr = if (edge.*.properties != null) edge.*.properties else "{}";
    const url_path: CStr = if (edge.*.url_path != null) edge.*.url_path else "";
    const local_name: CStr = if (edge.*.local_name != null) edge.*.local_name else "";
    return c.strcmp(properties, "{}") == 0 and url_path[0] == 0 and local_name[0] == 0;
}

fn preparedGroupEdgeCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(&cast(GroupEdge, left_ptr).edge_id, &cast(GroupEdge, right_ptr).edge_id);
}

fn modelHashCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast([*c]const FileHash, left_ptr).*;
    const right = cast([*c]const FileHash, right_ptr).*;
    return c.strcmp(left.*.file_path, right.*.file_path);
}

fn modelNodeVectorCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(PublishNodeVector, left_ptr).stable_id, cast(PublishNodeVector, right_ptr).stable_id);
}

fn modelTokenVectorCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(PublishTokenVector, left_ptr).token_id, cast(PublishTokenVector, right_ptr).token_id);
}

// ── Dump id lookups ────────────────────────────────────────────────

fn modelSparseLookup(model: *const Model, dump_id: i64) ?*IdMap {
    var low: usize = 0;
    var high: usize = @intCast(model.node_count);
    while (low < high) {
        const middle = low + (high - low) / 2;
        const entry: *IdMap = model.id_map + middle;
        if (entry.dump_id == dump_id) return entry;
        if (entry.dump_id < dump_id) low = middle + 1 else high = middle;
    }
    return null;
}

fn modelLookup(model: *const Model, dump_id: i64) CStr {
    if (model.dense_ids != null) {
        if (dump_id <= 0 or dump_id > model.node_count) return null;
        return model.dense_ids[@intCast(dump_id - 1)];
    }
    return if (modelSparseLookup(model, dump_id)) |entry| entry.stable_id else null;
}

fn modelLookupOrdinal(model: *const Model, dump_id: i64) ?u64 {
    if (model.dense_ids != null) {
        if (dump_id <= 0 or dump_id > model.node_count or
            model.dense_ids[@intCast(dump_id - 1)] == null or model.dense_ordinals == null)
            return null;
        return model.dense_ordinals[@intCast(dump_id - 1)];
    }
    return if (modelSparseLookup(model, dump_id)) |entry| entry.ordinal else null;
}

// ── Digests ────────────────────────────────────────────────────────

fn modelDigestText(context: *Sha, value: CStr) void {
    const text: []const u8 = if (value != null) std.mem.span(value) else "";
    c.cbm_sha256_backend_update(context, text.ptr, text.len);
    c.cbm_sha256_backend_update(context, "\x00", 1);
}

fn modelDigestI64(context: *Sha, value: i64) void {
    var bytes: [8]u8 = undefined;
    std.mem.writeInt(i64, &bytes, value, .little);
    c.cbm_sha256_backend_update(context, &bytes, bytes.len);
}

fn modelDigestFinish(context: *Sha, output: *[c.CBM_ZOVA_DIGEST_HEX_SIZE]u8) void {
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    c.cbm_sha256_backend_final(context, &digest);
    hexInto(output[0..c.CBM_SHA256_HEX_LEN], &digest);
    output[c.CBM_SHA256_HEX_LEN] = 0;
}

fn modelDigestTopologyKey(context: *Sha, row: *const PublishEdge) void {
    modelDigestText(context, row.source_stable_id);
    modelDigestText(context, row.source.*.type);
    modelDigestText(context, row.target_stable_id);
}

fn modelDigestTopologyPayload(digests: *DigestState, row: *const PublishEdge) void {
    modelDigestTopologyKey(&digests.metadata, row);
    c.cbm_sha256_backend_update(&digests.metadata, row.payload, row.payload_len);
}

fn modelDigestInit(digests: *DigestState) void {
    c.cbm_sha256_backend_init(&digests.metadata);
    c.cbm_sha256_backend_init(&digests.fts);
    c.cbm_sha256_backend_init(&digests.topology);
    c.cbm_sha256_backend_init(&digests.nodes);
    c.cbm_sha256_backend_init(&digests.tokens);
}

// ── Build ──────────────────────────────────────────────────────────

fn empty(text: CStr) bool {
    return text == null or text[0] == 0;
}

fn summaryValid(summary: *const Summary) bool {
    return !summary.present or (summary.summary != null and summary.source_hash != null and
        summary.created_at != null and summary.updated_at != null);
}

fn modelValidateInput(workspace_id: CStr, maybe_input: ?*const Input) BuildError!*const Input {
    const input = maybe_input orelse return error.BuildFailed;
    if (workspace_id == null or c.cbm_zova_workspace_id_validate(workspace_id) != 0 or
        empty(input.root_path) or empty(input.project) or empty(input.indexed_at) or
        empty(input.model_fingerprint) or input.vector_dimensions <= 0 or
        input.node_count < 0 or input.edge_count < 0 or input.node_vector_count < 0 or
        input.token_vector_count < 0 or input.file_hash_count < 0 or
        (input.node_count != 0 and input.nodes == null) or
        (input.edge_count != 0 and input.edges == null) or
        (input.node_vector_count != 0 and input.node_vectors == null) or
        (input.token_vector_count != 0 and input.token_vectors == null) or
        (input.file_hash_count != 0 and input.file_hashes == null) or
        !summaryValid(&input.project_summary))
        return error.BuildFailed;
    return input;
}

const MODEL_NODE_CHUNK_SIZE = 1024;

const NodeBuildContext = struct {
    model: *Model,
    id_stride: usize,
    fts_bytes: usize,
    failed: [*]u8,
};

/// Stable ids and FTS names of one chunk of nodes. On entry a row's `ordinal`
/// holds the length of the node name.
fn modelBuildNodeChunk(chunk: c_int, opaque_context: ?*anyopaque) callconv(.c) void {
    const context: *NodeBuildContext = @ptrCast(@alignCast(opaque_context.?));
    const model = context.model;
    const nodes = model.nodes[0..@intCast(model.node_count)];
    const begin = @as(usize, @intCast(chunk)) * MODEL_NODE_CHUNK_SIZE;
    const end = @min(begin + MODEL_NODE_CHUNK_SIZE, nodes.len);
    for (nodes[begin..end], begin..) |*row, i| {
        const node = row.value.source;
        const stable_id = model.node_id_storage + i * context.id_stride;
        const fts_end = if (i + 1 < nodes.len) nodes[i + 1].fts_name else model.node_fts_storage + context.fts_bytes;
        const fts_capacity = @intFromPtr(fts_end) - @intFromPtr(row.fts_name);
        const name_length: usize = @intCast(row.value.ordinal);
        modelNodeId(&model.workspace_id, node, stable_id) catch {
            context.failed[@intCast(chunk)] = 1;
            return;
        };
        modelCamelNameInto(node.*.name, name_length, row.fts_name, fts_capacity) catch {
            context.failed[@intCast(chunk)] = 1;
            return;
        };
        row.value.stable_id = stable_id;
        row.value.ordinal = 0;
    }
}

fn modelBuildNodes(model: *Model, digests: *DigestState) BuildError!void {
    if (model.input.node_count == 0) return;
    const count: usize = @intCast(model.input.node_count);
    model.nodes = modelCalloc(ModelNode, count);
    model.dense_ids = modelCalloc(CStr, count);
    model.dense_ordinals = modelCalloc(u64, count);
    if (model.nodes == null or model.dense_ids == null or model.dense_ordinals == null)
        return error.BuildFailed;
    const layout = modelIdStorageLayout(&model.workspace_id, count) orelse return error.BuildFailed;
    const nodes = model.nodes[0..count];
    const input_nodes = model.input.nodes[0..count];
    // Until the rows are filled in, `ordinal` and `source_ordinal` hold the
    // FTS size and the name length.
    var fts_bytes: usize = 0;
    for (nodes, input_nodes) |*row, *node| {
        const camel = modelCamelStorageSize(node.name) orelse return error.BuildFailed;
        if (camel.size > max_usize - fts_bytes) return error.BuildFailed;
        row.value.ordinal = camel.size;
        row.value.source_ordinal = camel.length;
        fts_bytes += camel.size;
    }
    model.node_id_storage = modelMalloc(u8, layout.bytes);
    model.node_fts_storage = modelMalloc(u8, fts_bytes);
    if (model.node_id_storage == null or model.node_fts_storage == null) return error.BuildFailed;
    model.node_count = model.input.node_count;
    var dense_ids = true;
    var fts_cursor: [*c]u8 = model.node_fts_storage;
    for (nodes, input_nodes, 0..) |*row, *node, i| {
        const stable_id = model.node_id_storage + i * layout.stride;
        const fts_capacity: usize = @intCast(row.value.ordinal);
        const name_length = row.value.source_ordinal;
        if (node.id <= 0 or node.project == null or c.strcmp(node.project, model.input.project) != 0 or
            node.label == null or node.name == null or node.qualified_name == null or node.file_path == null)
            return error.BuildFailed;
        row.value = .{ .source = node, .stable_id = stable_id, .ordinal = name_length, .source_ordinal = i };
        row.fts_name = fts_cursor;
        fts_cursor += fts_capacity;
        if (node.id > model.node_count or model.dense_ids[@intCast(node.id - 1)] != null) {
            dense_ids = false;
        } else {
            model.dense_ids[@intCast(node.id - 1)] = stable_id;
        }
    }
    {
        const chunk_count = (count + MODEL_NODE_CHUNK_SIZE - 1) / MODEL_NODE_CHUNK_SIZE;
        const failed = modelCalloc(u8, chunk_count);
        if (failed == null) return error.BuildFailed;
        defer c.free(failed);
        var context = NodeBuildContext{ .model = model, .id_stride = layout.stride, .fts_bytes = fts_bytes, .failed = failed };
        c.cbm_parallel_for(@intCast(chunk_count), modelBuildNodeChunk, &context, .{});
        for (failed[0..chunk_count]) |chunk_failed| {
            if (chunk_failed != 0) return error.BuildFailed;
        }
    }
    model.metrics.stable_node_id_computations += count;
    model.metrics.camel_split_computations += count;
    if (!dense_ids) {
        c.free(@ptrCast(model.dense_ids));
        model.dense_ids = null;
        c.free(model.dense_ordinals);
        model.dense_ordinals = null;
        model.id_map = modelCalloc(IdMap, count);
        if (model.id_map == null) return error.BuildFailed;
        const id_map = model.id_map[0..count];
        for (id_map, nodes) |*entry, *row| {
            entry.* = .{ .dump_id = row.value.source.*.id, .stable_id = row.value.stable_id, .ordinal = 0 };
        }
        if (count > 1) c.qsort(id_map.ptr, count, @sizeOf(IdMap), modelIdCompare);
        model.metrics.global_sorts += 1;
        for (1..count) |i| {
            if (id_map[i - 1].dump_id == id_map[i].dump_id) return error.BuildFailed;
        }
    }
    if (count > 1) c.qsort(nodes.ptr, count, @sizeOf(ModelNode), modelNodeCompare);
    model.metrics.global_sorts += 1;
    for (nodes, 0..) |*row, i| {
        if (i > 0 and c.strcmp(nodes[i - 1].value.stable_id, row.value.stable_id) == 0)
            return error.BuildFailed;
        row.value.ordinal = i;
        const node: *const DumpNode = row.value.source;
        if (model.dense_ids != null) {
            model.dense_ordinals[@intCast(node.id - 1)] = i;
        } else {
            const entry = modelSparseLookup(model, node.id) orelse return error.BuildFailed;
            entry.ordinal = i;
        }
        modelDigestText(&digests.metadata, row.value.stable_id);
        modelDigestText(&digests.metadata, node.project);
        modelDigestText(&digests.metadata, node.label);
        modelDigestText(&digests.metadata, node.name);
        modelDigestText(&digests.metadata, node.qualified_name);
        modelDigestText(&digests.metadata, node.file_path);
        modelDigestI64(&digests.metadata, node.start_line);
        modelDigestI64(&digests.metadata, node.end_line);
        modelDigestText(&digests.metadata, node.properties);
        modelDigestText(&digests.fts, row.value.stable_id);
        modelDigestText(&digests.fts, row.fts_name);
        modelDigestText(&digests.fts, node.qualified_name);
        modelDigestText(&digests.fts, node.label);
        modelDigestText(&digests.fts, node.file_path);
    }
}

fn modelBuildEdges(model: *Model, digests: *DigestState) BuildError!void {
    if (model.input.edge_count == 0) return;
    const count: usize = @intCast(model.input.edge_count);
    model.edges = modelCalloc(PublishEdge, count);
    const layout = modelIdStorageLayout(&model.workspace_id, count) orelse return error.BuildFailed;
    model.edge_id_storage = modelMalloc(u8, layout.bytes);
    if (model.edges == null or model.edge_id_storage == null) return error.BuildFailed;
    model.edge_count = model.input.edge_count;
    const edges = model.edges[0..count];
    for (edges, model.input.edges[0..count], 0..) |*row, *edge, i| {
        const source = modelLookup(model, edge.source_id);
        const target = modelLookup(model, edge.target_id);
        model.metrics.endpoint_lookups += 2;
        if (edge.project == null or c.strcmp(edge.project, model.input.project) != 0 or
            empty(edge.type) or source == null or target == null) return error.BuildFailed;
        const source_ordinal = modelLookupOrdinal(model, edge.source_id) orelse return error.BuildFailed;
        const target_ordinal = modelLookupOrdinal(model, edge.target_id) orelse return error.BuildFailed;
        const edge_id = model.edge_id_storage + i * layout.stride;
        try modelEdgeId(&model.workspace_id, source, edge.type, target, edge.local_name, edge_id);
        row.* = .{
            .source = edge,
            .edge_id = edge_id,
            .source_stable_id = source,
            .target_stable_id = target,
            .source_ordinal = source_ordinal,
            .target_ordinal = target_ordinal,
        };
        model.metrics.edge_id_computations += 1;
    }
    if (count > 1) c.qsort(edges.ptr, count, @sizeOf(PublishEdge), modelEdgeCompare);
    model.metrics.global_sorts += 1;
    // Rows with the same edge id collapse into one; the last one wins.
    var unique_edges: usize = 0;
    for (0..count) |i| {
        const current = edges[i];
        if (unique_edges > 0 and c.strcmp(edges[unique_edges - 1].edge_id, current.edge_id) == 0) {
            const previous = &edges[unique_edges - 1];
            if (c.strcmp(previous.source_stable_id, current.source_stable_id) != 0 or
                c.strcmp(previous.source.*.type, current.source.*.type) != 0 or
                c.strcmp(previous.target_stable_id, current.target_stable_id) != 0 or
                modelNullableCompare(previous.source.*.local_name, current.source.*.local_name) != 0)
                return error.BuildFailed;
            previous.* = current;
        } else {
            if (unique_edges != i) edges[unique_edges] = current;
            unique_edges += 1;
        }
    }
    model.edge_count = @intCast(unique_edges);
    const unique = edges[0..unique_edges];
    for (unique, 0..) |*row, i| row.ordinal = i;

    model.topology = modelCalloc(PublishEdge, unique_edges);
    if (model.topology == null) return error.BuildFailed;
    const topology = model.topology[0..unique_edges];
    @memcpy(topology, unique);
    if (unique_edges > 1) c.qsort(topology.ptr, unique_edges, @sizeOf(PublishEdge), modelTopologyCompare);
    model.metrics.global_sorts += 1;
    var unique_topology: usize = 0;
    for (0..unique_edges) |i| {
        const current = topology[i];
        if (unique_topology == 0 or modelTopologyCompare(&topology[unique_topology - 1], &current) != 0) {
            const row = &topology[unique_topology];
            if (unique_topology != i) row.* = current;
            row.ordinal = unique_topology;
            modelDigestTopologyKey(&digests.topology, row);
            unique_topology += 1;
        }
        if (current.ordinal >= unique_edges) return error.BuildFailed;
        unique[@intCast(current.ordinal)].topology_ordinal = unique_topology - 1;
    }
    model.topology_count = @intCast(unique_topology);

    // One allocation: the edge count, first edge and write cursor of each
    // topology row.
    const counts_storage = modelCalloc(usize, unique_topology * 3);
    defer c.free(counts_storage);
    const grouped_storage = modelMalloc([*c]const DumpEdge, unique_edges);
    defer c.free(@ptrCast(grouped_storage));
    if (counts_storage == null or grouped_storage == null) return error.BuildFailed;
    const counts = counts_storage[0..unique_topology];
    const offsets = counts_storage[unique_topology..][0..unique_topology];
    const cursors = counts_storage[unique_topology * 2 ..][0..unique_topology];
    const grouped = grouped_storage[0..unique_edges];
    for (unique) |*row| {
        if (row.topology_ordinal >= unique_topology) return error.BuildFailed;
        counts[@intCast(row.topology_ordinal)] += 1;
    }
    var grouped_count: usize = 0;
    for (counts, offsets, cursors) |edge_count, *offset, *cursor| {
        offset.* = grouped_count;
        cursor.* = grouped_count;
        if (edge_count > max_usize - grouped_count) return error.BuildFailed;
        grouped_count += edge_count;
    }
    if (grouped_count != unique_edges) return error.BuildFailed;
    for (unique) |*row| {
        const cursor = &cursors[@intCast(row.topology_ordinal)];
        grouped[cursor.*] = row.source;
        cursor.* += 1;
    }

    // The cursors are done; from here they hold each row's payload size.
    var payload_bytes: usize = 0;
    for (counts, offsets, cursors) |edge_count, offset, *payload_size| {
        payload_size.* = 0;
        if (c.cbm_zova_edge_payload_encoded_size(grouped.ptr + offset, edge_count, payload_size) != 0 or
            payload_size.* > max_usize - payload_bytes) return error.BuildFailed;
        payload_bytes += payload_size.*;
    }
    if (payload_bytes > 0) {
        model.edge_payload_storage = modelMalloc(u8, payload_bytes);
        if (model.edge_payload_storage == null) return error.BuildFailed;
    }
    var payload_cursor: [*c]u8 = model.edge_payload_storage;
    for (topology[0..unique_topology], counts, offsets, cursors) |*row, edge_count, offset, payload_size| {
        row.logical_edge_count = edge_count;
        row.payload = if (payload_size > 0) payload_cursor else null;
        row.payload_len = payload_size;
        var encoded: usize = 0;
        if (c.cbm_zova_edge_payload_encode(grouped.ptr + offset, edge_count, payload_cursor, payload_size, &encoded) != 0 or
            encoded != payload_size) return error.BuildFailed;
        if (encoded > 0) payload_cursor += encoded;
        modelDigestTopologyPayload(digests, row);
    }
}

/// End of the run of seeds that share the topology key of seeds[begin].
fn preparedGroupEnd(seeds: []const TopologySeed, begin: usize) usize {
    var end = begin + 1;
    while (end < seeds.len and preparedTopologySeedCompare(&seeds[begin], &seeds[end]) == 0) end += 1;
    return end;
}

/// Build only the topology rows required by authoritative full publication.
/// Finalized graph-buffer input already has dense node IDs and deduplicated
/// logical edges, so delta-only logical edge IDs and edge ordering are omitted.
fn modelBuildPreparedTopology(model: *Model, digests: *DigestState) BuildError!void {
    model.edge_count = model.input.edge_count;
    if (model.input.edge_count == 0) return;
    if (model.dense_ids == null or model.dense_ordinals == null) return error.BuildFailed;
    const count: usize = @intCast(model.input.edge_count);
    var phase_started = modelNow();
    model.topology = modelCalloc(PublishEdge, count);
    var seeds_storage = modelMalloc(TopologySeed, count);
    var payload_groups: [*c]PayloadGroup = null;
    var payload_edges: [*c][*c]const DumpEdge = null;
    var ordered: [*c]GroupEdge = null;
    defer {
        c.free(ordered);
        c.free(@ptrCast(payload_edges));
        c.free(payload_groups);
        c.free(seeds_storage);
    }
    if (model.topology == null or seeds_storage == null) return error.BuildFailed;
    const topology = model.topology[0..count];
    const seeds = seeds_storage[0..count];
    for (seeds, model.input.edges[0..count]) |*seed, *edge| {
        model.metrics.endpoint_lookups += 2;
        if (edge.source_id <= 0 or edge.source_id > model.node_count or
            edge.target_id <= 0 or edge.target_id > model.node_count) return error.BuildFailed;
        const source_index: usize = @intCast(edge.source_id - 1);
        const target_index: usize = @intCast(edge.target_id - 1);
        if (edge.project == null or
            (edge.project != model.input.project and c.strcmp(edge.project, model.input.project) != 0) or
            empty(edge.type) or model.dense_ids[source_index] == null or model.dense_ids[target_index] == null)
            return error.BuildFailed;
        seed.* = .{
            .source = edge,
            .source_ordinal = model.dense_ordinals[source_index],
            .target_ordinal = model.dense_ordinals[target_index],
        };
    }
    model.metrics.prepared_endpoint_ms = modelLap(&phase_started);
    if (count > 1) c.qsort(seeds.ptr, count, @sizeOf(TopologySeed), preparedTopologySeedCompare);
    model.metrics.global_sorts += 1;
    model.metrics.prepared_topology_sort_ms = modelLap(&phase_started);

    // One topology row per run of equal seeds. A row with one edge gets its
    // payload size here; rows with several edges are sized in the next pass.
    var unique_topology: usize = 0;
    var payload_group_count: usize = 0;
    var payload_edge_count: usize = 0;
    var payload_bytes: usize = 0;
    var i: usize = 0;
    while (i < count) {
        const end = preparedGroupEnd(seeds, i);
        const edge = seeds[i].source;
        const group_edge_count = end - i;
        const row = &topology[unique_topology];
        row.* = .{
            .source = edge,
            .source_stable_id = model.dense_ids[@intCast(edge.*.source_id - 1)],
            .target_stable_id = model.dense_ids[@intCast(edge.*.target_id - 1)],
            .source_ordinal = seeds[i].source_ordinal,
            .target_ordinal = seeds[i].target_ordinal,
            .ordinal = unique_topology,
            .logical_edge_count = group_edge_count,
        };
        if (group_edge_count == 1 and preparedEdgePayloadIsDefault(edge)) {
            model.metrics.prepared_single_default_payload_count += 1;
        } else if (group_edge_count == 1) {
            if (c.cbm_zova_edge_payload_encoded_size(&edge, 1, &row.payload_len) != 0 or
                row.payload_len > max_usize - payload_bytes) return error.BuildFailed;
            payload_bytes += row.payload_len;
        } else {
            payload_group_count += 1;
            if (group_edge_count > max_usize - payload_edge_count) return error.BuildFailed;
            payload_edge_count += group_edge_count;
        }
        unique_topology += 1;
        i = end;
    }
    model.topology_count = @intCast(unique_topology);
    model.metrics.prepared_payload_scratch_edge_count = payload_edge_count;
    model.metrics.prepared_topology_group_ms = modelLap(&phase_started);

    if (payload_group_count > 0) {
        payload_groups = modelCalloc(PayloadGroup, payload_group_count);
        payload_edges = modelMalloc([*c]const DumpEdge, payload_edge_count);
        if (payload_groups == null or payload_edges == null) return error.BuildFailed;
    }
    // The edges of a multi-edge row are encoded in logical edge id order.
    var payload_group_index: usize = 0;
    var payload_edge_offset: usize = 0;
    var topology_index: usize = 0;
    i = 0;
    while (i < count) : (topology_index += 1) {
        const end = preparedGroupEnd(seeds, i);
        const group_edge_count = end - i;
        if (group_edge_count > 1) {
            const group: *PayloadGroup = payload_groups + payload_group_index;
            payload_group_index += 1;
            group.* = .{
                .topology_index = topology_index,
                .edge_offset = payload_edge_offset,
                .edge_count = group_edge_count,
                .payload_size = 0,
            };
            const group_edges = payload_edges[payload_edge_offset..][0..group_edge_count];
            for (group_edges, seeds[i..end]) |*slot, *seed| slot.* = seed.source;
            ordered = modelMalloc(GroupEdge, group_edge_count);
            if (ordered == null) return error.BuildFailed;
            const ordered_edges = ordered[0..group_edge_count];
            const row = &topology[topology_index];
            for (ordered_edges, group_edges) |*entry, source| {
                entry.source = source;
                try modelEdgeId(&model.workspace_id, row.source_stable_id, row.source.*.type, row.target_stable_id, source.*.local_name, &entry.edge_id);
                model.metrics.edge_id_computations += 1;
            }
            c.qsort(ordered_edges.ptr, group_edge_count, @sizeOf(GroupEdge), preparedGroupEdgeCompare);
            for (ordered_edges, group_edges, 0..) |*entry, *slot, j| {
                if (j > 0 and c.strcmp(&ordered_edges[j - 1].edge_id, &entry.edge_id) == 0)
                    return error.BuildFailed;
                slot.* = entry.source;
            }
            c.free(ordered);
            ordered = null;
            if (c.cbm_zova_edge_payload_encoded_size(group_edges.ptr, group_edge_count, &group.payload_size) != 0 or
                group.payload_size > max_usize - payload_bytes) return error.BuildFailed;
            row.payload_len = group.payload_size;
            payload_bytes += group.payload_size;
            payload_edge_offset += group_edge_count;
        }
        i = end;
    }
    if (payload_group_index != payload_group_count or payload_edge_offset != payload_edge_count)
        return error.BuildFailed;
    if (payload_bytes > 0) {
        model.edge_payload_storage = modelMalloc(u8, payload_bytes);
        if (model.edge_payload_storage == null) return error.BuildFailed;
    }
    var payload_cursor: [*c]u8 = model.edge_payload_storage;
    var duplicate_group_index: usize = 0;
    for (topology[0..unique_topology], 0..) |*row, row_index| {
        if (row.payload_len == 0) continue;
        const single_edge = row.source;
        var edges: [*c]const [*c]const DumpEdge = &single_edge;
        var edge_count: usize = 1;
        if (row.logical_edge_count > 1) {
            const group: *const PayloadGroup = payload_groups + duplicate_group_index;
            duplicate_group_index += 1;
            if (group.topology_index != row_index or group.payload_size != row.payload_len)
                return error.BuildFailed;
            edges = payload_edges + group.edge_offset;
            edge_count = group.edge_count;
        }
        row.payload = payload_cursor;
        var encoded: usize = 0;
        if (c.cbm_zova_edge_payload_encode(edges, edge_count, payload_cursor, row.payload_len, &encoded) != 0 or
            encoded != row.payload_len) return error.BuildFailed;
        if (encoded > 0) payload_cursor += encoded;
    }
    if (duplicate_group_index != payload_group_count) return error.BuildFailed;
    c.free(@ptrCast(payload_edges));
    payload_edges = null;
    c.free(payload_groups);
    payload_groups = null;
    c.free(seeds_storage);
    seeds_storage = null;
    model.metrics.prepared_payload_ms = modelLap(&phase_started);

    for (topology[0..unique_topology]) |*row| {
        modelDigestTopologyKey(&digests.topology, row);
        modelDigestTopologyPayload(digests, row);
    }
    model.metrics.prepared_topology_digest_ms = modelLap(&phase_started);
}

fn modelBuildHashes(model: *Model, digests: *DigestState) BuildError!void {
    if (model.input.file_hash_count == 0) return;
    const count: usize = @intCast(model.input.file_hash_count);
    model.hashes = modelCalloc([*c]const FileHash, count);
    if (model.hashes == null) return error.BuildFailed;
    model.hash_count = model.input.file_hash_count;
    const hashes = model.hashes[0..count];
    for (hashes, model.input.file_hashes[0..count]) |*slot, *hash| {
        if (hash.file_path == null or hash.content_hash == null) return error.BuildFailed;
        slot.* = hash;
    }
    if (count > 1) c.qsort(@ptrCast(hashes.ptr), count, @sizeOf([*c]const FileHash), modelHashCompare);
    model.metrics.global_sorts += 1;
    for (hashes, 0..) |hash, i| {
        if (i > 0 and c.strcmp(hashes[i - 1].*.file_path, hash.*.file_path) == 0) return error.BuildFailed;
        modelDigestText(&digests.metadata, hash.*.file_path);
        modelDigestText(&digests.metadata, hash.*.content_hash);
        modelDigestI64(&digests.metadata, hash.*.mtime_ns);
        modelDigestI64(&digests.metadata, hash.*.size_bytes);
    }
}

fn modelBuildVectors(model: *Model, digests: *DigestState) BuildError!void {
    const node_capacity: usize = @intCast(model.input.node_vector_count);
    const token_capacity: usize = @intCast(model.input.token_vector_count);
    if (node_capacity > 0) {
        model.node_vectors = modelCalloc(PublishNodeVector, node_capacity);
        if (model.node_vectors == null) return error.BuildFailed;
    }
    for (0..node_capacity) |i| {
        const vector: *const c.CBMDumpVector = model.input.node_vectors + i;
        const stable_id = modelLookup(model, vector.node_id);
        if (vector.project == null or c.strcmp(vector.project, model.input.project) != 0 or stable_id == null)
            return error.BuildFailed;
        const node_ordinal = modelLookupOrdinal(model, vector.node_id) orelse return error.BuildFailed;
        if (vector.vector == null or vector.vector_len != model.input.vector_dimensions) return error.BuildFailed;
        if (!modelNonzero(vector.vector, vector.vector_len)) continue;
        model.node_vectors[@intCast(model.node_vector_count)] = .{ .source = vector, .stable_id = stable_id, .node_ordinal = node_ordinal };
        model.node_vector_count += 1;
    }
    if (model.node_vector_count > 0) {
        const rows = model.node_vectors[0..@intCast(model.node_vector_count)];
        if (rows.len > 1) c.qsort(rows.ptr, rows.len, @sizeOf(PublishNodeVector), modelNodeVectorCompare);
        model.metrics.global_sorts += 1;
        for (rows, 0..) |*row, i| {
            if (i > 0 and c.strcmp(rows[i - 1].stable_id, row.stable_id) == 0) return error.BuildFailed;
            row.ordinal = i;
            modelDigestText(&digests.nodes, row.stable_id);
            c.cbm_sha256_backend_update(&digests.nodes, row.source.*.vector, @intCast(row.source.*.vector_len));
        }
    }
    var token_id_stride: usize = 0;
    if (token_capacity > 0) {
        model.token_vectors = modelCalloc(PublishTokenVector, token_capacity);
        const layout = modelIdStorageLayout(&model.workspace_id, token_capacity) orelse return error.BuildFailed;
        token_id_stride = layout.stride;
        model.token_id_storage = modelMalloc(u8, layout.bytes);
        if (model.token_vectors == null or model.token_id_storage == null) return error.BuildFailed;
    }
    for (0..token_capacity) |i| {
        const vector: *const c.CBMDumpTokenVec = model.input.token_vectors + i;
        if (vector.id <= 0 or vector.project == null or
            c.strcmp(vector.project, model.input.project) != 0 or vector.token == null or
            vector.vector == null or vector.vector_len != model.input.vector_dimensions or
            !std.math.isFinite(vector.idf)) return error.BuildFailed;
        const token_id = model.token_id_storage + i * token_id_stride;
        try modelTokenId(&model.workspace_id, vector.token, token_id);
        model.metrics.token_id_computations += 1;
        if (!modelNonzero(vector.vector, vector.vector_len)) continue;
        model.token_vectors[@intCast(model.token_vector_count)] = .{ .source = vector, .token_id = token_id };
        model.token_vector_count += 1;
    }
    if (model.token_vector_count > 0) {
        const rows = model.token_vectors[0..@intCast(model.token_vector_count)];
        if (rows.len > 1) c.qsort(rows.ptr, rows.len, @sizeOf(PublishTokenVector), modelTokenVectorCompare);
        model.metrics.global_sorts += 1;
        for (rows, 0..) |*row, i| {
            if (i > 0 and c.strcmp(rows[i - 1].token_id, row.token_id) == 0) return error.BuildFailed;
            row.ordinal = i;
            const source: *const c.CBMDumpTokenVec = row.source;
            modelDigestText(&digests.tokens, row.token_id);
            modelDigestText(&digests.tokens, source.token);
            c.cbm_sha256_backend_update(&digests.tokens, &source.idf, @sizeOf(f32));
            c.cbm_sha256_backend_update(&digests.tokens, source.vector, @intCast(source.vector_len));
        }
    }
}

fn modelDigestSummary(model: *Model, digests: *DigestState) void {
    const summary = &model.input.project_summary;
    modelDigestText(&digests.metadata, if (summary.present) "1" else "0");
    if (summary.present) {
        modelDigestText(&digests.metadata, summary.summary);
        modelDigestText(&digests.metadata, summary.source_hash);
        modelDigestText(&digests.metadata, summary.created_at);
        modelDigestText(&digests.metadata, summary.updated_at);
    }
}

fn modelDigestFinishAll(model: *Model, digests: *DigestState) void {
    modelDigestFinish(&digests.metadata, &model.digests.metadata_sha256);
    modelDigestFinish(&digests.fts, &model.digests.fts_sha256);
    modelDigestFinish(&digests.topology, &model.digests.topology_sha256);
    modelDigestFinish(&digests.nodes, &model.digests.node_vector_sha256);
    modelDigestFinish(&digests.tokens, &model.digests.token_vector_sha256);
    model.digests.graph_nodes = @intCast(model.node_count);
    model.digests.graph_edges = @intCast(model.topology_count);
    model.digests.metadata_nodes = @intCast(model.node_count);
    model.digests.metadata_edges = @intCast(model.edge_count);
    model.digests.metadata_topology_edges = @intCast(model.topology_count);
    model.digests.fts_rows = @intCast(model.node_count);
    model.digests.node_vectors = @intCast(model.node_vector_count);
    model.digests.node_vector_rows = @intCast(model.node_vector_count);
    model.digests.token_vectors = @intCast(model.token_vector_count);
    model.digests.token_vector_rows = @intCast(model.token_vector_count);
}

/// A zeroed model that holds the workspace id and a copy of `input`.
fn modelCreate(workspace_id: CStr, input: *const Input) BuildError!*Model {
    model_allocation_count.store(0, .seq_cst);
    const model = @as(?*Model, modelCalloc(Model, 1)) orelse return error.BuildFailed;
    _ = interop.bprint(&model.workspace_id, "{s}", .{std.mem.span(workspace_id)});
    model.input = input.*;
    return model;
}

fn publishModelBuild(workspace_id: CStr, maybe_input: ?*const Input) BuildError!*Model {
    const started = modelNow();
    const input = try modelValidateInput(workspace_id, maybe_input);
    const model = try modelCreate(workspace_id, input);
    errdefer cbm_zova_publish_model_free(model);
    var digests: DigestState = undefined;
    modelDigestInit(&digests);
    var phase_started = modelNow();
    try modelBuildNodes(model, &digests);
    model.metrics.nodes_ms = modelLap(&phase_started);
    try modelBuildEdges(model, &digests);
    model.metrics.edges_ms = modelLap(&phase_started);
    try modelBuildHashes(model, &digests);
    model.metrics.hashes_ms = modelLap(&phase_started);
    modelDigestSummary(model, &digests);
    try modelBuildVectors(model, &digests);
    model.metrics.vectors_ms = modelLap(&phase_started);
    modelDigestFinishAll(model, &digests);
    model.metrics.digests_ms = modelLap(&phase_started);
    model.metrics.normalization_ms = modelElapsedMs(started, phase_started);
    _ = model_build_count.fetchAdd(1, .seq_cst);
    return model;
}

pub export fn cbm_zova_publish_model_build(workspace_id: CStr, input: ?*const Input, out_model: ?*?*Model) c_int {
    const out = out_model orelse return -1;
    out.* = null;
    out.* = publishModelBuild(workspace_id, input) catch return -1;
    return 0;
}

fn preparedViewBuildBase(workspace_id: CStr, maybe_input: ?*const Input) BuildError!*Model {
    const started = modelNow();
    const input = try modelValidateInput(workspace_id, maybe_input);
    if (input.file_hash_count != 0 or input.file_hashes != null or input.project_summary.present)
        return error.BuildFailed;
    const view = try modelCreate(workspace_id, input);
    errdefer cbm_zova_publish_model_free(view);
    const digests = &view.pending_digests;
    modelDigestInit(digests);
    var phase_started = modelNow();
    try modelBuildNodes(view, digests);
    if (view.node_count > 0 and (view.dense_ids == null or view.dense_ordinals == null))
        return error.BuildFailed;
    view.metrics.nodes_ms = modelLap(&phase_started);
    try modelBuildPreparedTopology(view, digests);
    view.metrics.edges_ms = modelLap(&phase_started);
    try modelBuildVectors(view, digests);
    view.metrics.vectors_ms = modelLap(&phase_started);
    view.metrics.normalization_ms = modelElapsedMs(started, phase_started);
    view.prepared_pending = true;
    return view;
}

pub export fn cbm_zova_prepared_view_build_base(workspace_id: CStr, input: ?*const Input, out_view: ?*?*Model) c_int {
    const out = out_view orelse return -1;
    out.* = null;
    out.* = preparedViewBuildBase(workspace_id, input) catch return -1;
    return 0;
}

pub export fn cbm_zova_prepared_view_complete(maybe_view: ?*Model, file_hashes: [*c]const FileHash, file_hash_count: c_int, project_summary: ?*const Summary) c_int {
    const view = maybe_view orelse return -1;
    if (!view.prepared_pending or file_hash_count < 0 or (file_hash_count > 0 and file_hashes == null))
        return -1;
    const summary: Summary = if (project_summary) |given| given.* else .{};
    if (!summaryValid(&summary)) return -1;
    view.input.file_hashes = file_hashes;
    view.input.file_hash_count = file_hash_count;
    view.input.project_summary = summary;
    const started = modelNow();
    var phase_started = started;
    modelBuildHashes(view, &view.pending_digests) catch return -1;
    view.metrics.hashes_ms = modelLap(&phase_started);
    modelDigestSummary(view, &view.pending_digests);
    modelDigestFinishAll(view, &view.pending_digests);
    view.metrics.digests_ms = modelLap(&phase_started);
    view.metrics.normalization_ms += modelElapsedMs(started, phase_started);
    view.prepared_pending = false;
    return 0;
}

pub export fn cbm_zova_prepared_view_build(workspace_id: CStr, maybe_input: ?*const Input, out_view: ?*?*Model) c_int {
    const input = maybe_input orelse return -1;
    const out = out_view orelse return -1;
    out.* = null;
    var base = input.*;
    base.file_hashes = null;
    base.file_hash_count = 0;
    base.project_summary = .{};
    var view: ?*Model = null;
    if (cbm_zova_prepared_view_build_base(workspace_id, &base, &view) != 0) return -1;
    if (cbm_zova_prepared_view_complete(view, input.file_hashes, input.file_hash_count, &input.project_summary) != 0) {
        cbm_zova_prepared_view_free(view);
        return -1;
    }
    out.* = view;
    return 0;
}

pub export fn cbm_zova_prepared_view_free(view: ?*Model) void {
    cbm_zova_publish_model_free(view);
}

pub export fn cbm_zova_publish_model_free(maybe_model: ?*Model) void {
    const model = maybe_model orelse return;
    c.free(model.nodes);
    c.free(model.node_id_storage);
    c.free(model.node_fts_storage);
    c.free(@ptrCast(model.dense_ids));
    c.free(model.dense_ordinals);
    c.free(model.id_map);
    c.free(model.edges);
    c.free(model.edge_id_storage);
    c.free(model.topology);
    c.free(model.edge_payload_storage);
    c.free(@ptrCast(model.hashes));
    c.free(model.node_vectors);
    c.free(model.token_vectors);
    c.free(model.token_id_storage);
    c.free(model);
}

// ── Accessors ──────────────────────────────────────────────────────

/// rows[index] when `index` is inside `count` rows.
fn rowAt(comptime T: type, rows: [*c]T, count: c_int, index: c_int) ?*const T {
    if (index < 0 or index >= count) return null;
    return rows + @as(usize, @intCast(index));
}

pub export fn cbm_zova_publish_model_workspace_id(model: ?*const Model) CStr {
    return if (model) |m| &m.workspace_id else null;
}

pub export fn cbm_zova_publish_model_input(model: ?*const Model) ?*const Input {
    return if (model) |m| &m.input else null;
}

pub export fn cbm_zova_publish_model_node_count(model: ?*const Model) c_int {
    return if (model) |m| m.node_count else 0;
}

pub export fn cbm_zova_publish_model_node_at(model: ?*const Model, index: c_int) ?*const PublishNode {
    const m = model orelse return null;
    const row = rowAt(ModelNode, m.nodes, m.node_count, index) orelse return null;
    return &row.value;
}

pub export fn cbm_zova_publish_model_set_node_source_ordinal(model: ?*Model, index: c_int, source_ordinal: u64) c_int {
    const m = model orelse return -1;
    if (index < 0 or index >= m.node_count or source_ordinal > std.math.maxInt(i64)) return -1;
    m.nodes[@intCast(index)].value.source_ordinal = source_ordinal;
    return 0;
}

pub export fn cbm_zova_publish_model_fts_name_at(model: ?*const Model, index: c_int) CStr {
    const m = model orelse return null;
    const row = rowAt(ModelNode, m.nodes, m.node_count, index) orelse return null;
    return row.fts_name;
}

pub export fn cbm_zova_publish_model_stable_id_for_dump_id(model: ?*const Model, dump_id: i64) CStr {
    return if (model) |m| modelLookup(m, dump_id) else null;
}

pub export fn cbm_zova_publish_model_edge_count(model: ?*const Model) c_int {
    return if (model) |m| m.edge_count else 0;
}

pub export fn cbm_zova_publish_model_edge_at(model: ?*const Model, index: c_int) ?*const PublishEdge {
    const m = model orelse return null;
    return rowAt(PublishEdge, m.edges, m.edge_count, index);
}

pub export fn cbm_zova_publish_model_topology_count(model: ?*const Model) c_int {
    return if (model) |m| m.topology_count else 0;
}

pub export fn cbm_zova_publish_model_topology_at(model: ?*const Model, index: c_int) ?*const PublishEdge {
    const m = model orelse return null;
    return rowAt(PublishEdge, m.topology, m.topology_count, index);
}

pub export fn cbm_zova_publish_model_file_hash_count(model: ?*const Model) c_int {
    return if (model) |m| m.hash_count else 0;
}

pub export fn cbm_zova_publish_model_file_hash_at(model: ?*const Model, index: c_int) ?*const FileHash {
    const m = model orelse return null;
    const slot = rowAt([*c]const FileHash, m.hashes, m.hash_count, index) orelse return null;
    return slot.*;
}

pub export fn cbm_zova_publish_model_node_vector_count(model: ?*const Model) c_int {
    return if (model) |m| m.node_vector_count else 0;
}

pub export fn cbm_zova_publish_model_node_vector_at(model: ?*const Model, index: c_int) ?*const PublishNodeVector {
    const m = model orelse return null;
    return rowAt(PublishNodeVector, m.node_vectors, m.node_vector_count, index);
}

pub export fn cbm_zova_publish_model_token_vector_count(model: ?*const Model) c_int {
    return if (model) |m| m.token_vector_count else 0;
}

pub export fn cbm_zova_publish_model_token_vector_at(model: ?*const Model, index: c_int) ?*const PublishTokenVector {
    const m = model orelse return null;
    return rowAt(PublishTokenVector, m.token_vectors, m.token_vector_count, index);
}

/// NULL until a prepared view is completed.
pub export fn cbm_zova_publish_model_digests(model: ?*const Model) ?*const Result {
    const m = model orelse return null;
    return if (m.prepared_pending) null else &m.digests;
}

pub export fn cbm_zova_publish_model_metrics(model: ?*const Model, out_metrics: ?*Metrics) void {
    const out = out_metrics orelse return;
    out.* = if (model) |m| m.metrics else std.mem.zeroes(Metrics);
}

fn digestComplete(hex: *const [c.CBM_ZOVA_DIGEST_HEX_SIZE]u8) bool {
    return std.mem.sliceTo(hex, 0).len == c.CBM_ZOVA_DIGEST_HEX_SIZE - 1;
}

pub export fn cbm_zova_publish_model_reuse_vector_digests(maybe_model: ?*Model, maybe_previous: ?*const Result, components: u32) c_int {
    const NODE_VECTORS: u32 = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS;
    const TOKEN_VECTORS: u32 = c.CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS;
    const model = maybe_model orelse return -1;
    const previous = maybe_previous orelse return -1;
    if (components == 0 or (components & ~(NODE_VECTORS | TOKEN_VECTORS)) != 0) return -1;
    if ((components & NODE_VECTORS) != 0) {
        if (model.node_vector_count != 0 or previous.node_vectors == 0 or
            !digestComplete(&previous.node_vector_sha256)) return -1;
        model.digests.node_vector_sha256 = previous.node_vector_sha256;
        model.digests.node_vectors = previous.node_vectors;
        model.digests.node_vector_rows = previous.node_vector_rows;
    }
    if ((components & TOKEN_VECTORS) != 0) {
        if (model.token_vector_count != 0 or previous.token_vectors == 0 or
            !digestComplete(&previous.token_vector_sha256)) return -1;
        model.digests.token_vector_sha256 = previous.token_vector_sha256;
        model.digests.token_vectors = previous.token_vectors;
        model.digests.token_vector_rows = previous.token_vector_rows;
    }
    return 0;
}
