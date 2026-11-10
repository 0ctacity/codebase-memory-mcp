//! cbm_zova_delta.zig — Exact difference between a committed workspace
//! snapshot and the publication model of the next generation.
//!
//! Implements src/zova/cbm_zova_delta.h. Both sides are sorted by stable id,
//! so every section is one ordered merge: rows only in the snapshot become
//! deletes, rows only in the model become inserts, and rows in both are
//! compared field by field.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const CStr = interop.CStr;
const Snapshot = c.cbm_zova_workspace_snapshot_t;
const Model = c.cbm_zova_publish_model_t;
const Components = c.cbm_zova_snapshot_components_t;
const Metrics = c.cbm_zova_workspace_delta_metrics_t;
const PublishNode = c.cbm_zova_publish_node_t;
const PublishEdge = c.cbm_zova_publish_edge_t;
const PublishNodeVector = c.cbm_zova_publish_node_vector_t;
const PublishTokenVector = c.cbm_zova_publish_token_vector_t;
const FileHash = c.cbm_zova_file_hash_input_t;
const Summary = c.cbm_zova_project_summary_input_t;
const EdgeDelete = c.cbm_zova_delta_edge_delete_t;
const TopologyEdge = c.cbm_zova_delta_topology_edge_t;

const TOPOLOGY: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY;
const NODE_VECTORS: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS;
const TOKEN_VECTORS: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS;

/// Arrays are allocated at their upper bound; the counts live in `metrics`.
/// Insert/upsert rows borrow from the model, delete rows are owned copies.
const Delta = struct {
    node_inserts: [*c][*c]const PublishNode,
    node_updates: [*c][*c]const PublishNode,
    node_deletes: [*c][*c]u8,
    edge_inserts: [*c][*c]const PublishEdge,
    edge_deletes: [*c]EdgeDelete,
    topology_inserts: [*c]TopologyEdge,
    topology_deletes: [*c]TopologyEdge,
    node_vector_upserts: [*c][*c]const PublishNodeVector,
    node_vector_deletes: [*c][*c]u8,
    token_vector_upserts: [*c][*c]const PublishTokenVector,
    token_vector_deletes: [*c][*c]u8,
    file_hash_upserts: [*c][*c]const FileHash,
    file_hash_deletes: [*c][*c]u8,
    replace_summary: bool,
    summary: [*c]const Summary,
    expected_generation: i64,
    metrics: Metrics,
};

fn textEqual(left: CStr, right: CStr) bool {
    return c.strcmp(if (left != null) left else "", if (right != null) right else "") == 0;
}

const deltaDup = interop.heapStrdup;

fn deltaElapsedMs(started: c.struct_timespec) f64 {
    var finished: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &finished);
    return @as(f64, @floatFromInt(finished.tv_sec - started.tv_sec)) * 1000.0 +
        @as(f64, @floatFromInt(finished.tv_nsec - started.tv_nsec)) / 1000000.0;
}

fn sortedUnique(ids: [*c]const [*c]u8, count: c_int) bool {
    if (count < 0 or (count > 0 and ids == null)) return false;
    for (0..@intCast(count)) |i| {
        if (ids[i] == null or (i > 0 and c.strcmp(ids[i - 1], ids[i]) >= 0)) return false;
    }
    return true;
}

const DELTA_EDGE_ID_CAP = c.CBM_ZOVA_WORKSPACE_ID_MAX + 80;
const EdgeId = [DELTA_EDGE_ID_CAP]u8;

fn snapshotEdgeIdAt(snapshot: *const Snapshot, index: c_int, out: *EdgeId) c_int {
    return c.cbm_zova_workspace_snapshot_format_edge_id(snapshot, index, out, out.len);
}

fn snapshotEdgeIdsSortedUnique(snapshot: *const Snapshot) bool {
    if (snapshot.edge_count < 0 or
        (snapshot.edge_count > 0 and (snapshot.edges == null or snapshot.edge_ids == null))) return false;
    var previous: EdgeId = @splat(0);
    var current: EdgeId = @splat(0);
    var i: c_int = 0;
    while (i < snapshot.edge_count) : (i += 1) {
        if (snapshotEdgeIdAt(snapshot, i, &current) != 0 or
            (i > 0 and c.strcmp(&previous, &current) >= 0)) return false;
        previous = current;
    }
    return true;
}

fn digestValid(digest: *const [c.CBM_ZOVA_DIGEST_HEX_SIZE]u8) bool {
    const text = std.mem.sliceTo(digest, 0);
    if (text.len != c.CBM_ZOVA_DIGEST_HEX_SIZE - 1) return false;
    for (text) |ch| {
        if (!std.ascii.isDigit(ch) and !(ch >= 'a' and ch <= 'f')) return false;
    }
    return true;
}

pub export fn cbm_zova_workspace_delta_required_components(before: ?*const Snapshot, after: ?*const Model, out_components: ?*Components) c_int {
    const b = before orelse return -1;
    const out = out_components orelse return -1;
    if (after == null) return -1;
    const digests_ptr: ?*const c.cbm_zova_workspace_generation_result_t = c.cbm_zova_publish_model_digests(after);
    const digests = digests_ptr orelse return -1;
    if (!digestValid(&b.integrity.topology_sha256) or
        !digestValid(&b.integrity.node_vector_sha256) or
        !digestValid(&b.integrity.token_vector_sha256) or
        !digestValid(&digests.topology_sha256) or
        !digestValid(&digests.node_vector_sha256) or
        !digestValid(&digests.token_vector_sha256)) return -1;
    var components: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    if (c.strcmp(&b.integrity.topology_sha256, &digests.topology_sha256) != 0)
        components |= TOPOLOGY;
    if (c.strcmp(&b.integrity.node_vector_sha256, &digests.node_vector_sha256) != 0)
        components |= NODE_VECTORS;
    if (c.strcmp(&b.integrity.token_vector_sha256, &digests.token_vector_sha256) != 0)
        components |= TOKEN_VECTORS;
    out.* = components;
    return 0;
}

fn nodeEqual(before: *const Snapshot, before_index: usize, after: *const PublishNode) bool {
    const left = &before.nodes[before_index];
    const right = after.source;
    return textEqual(left.project, right.*.project) and textEqual(left.label, right.*.label) and
        textEqual(left.name, right.*.name) and
        textEqual(left.qualified_name, right.*.qualified_name) and
        textEqual(left.file_path, right.*.file_path) and
        left.start_line == right.*.start_line and left.end_line == right.*.end_line and
        textEqual(left.properties, right.*.properties);
}

fn snapshotEdgeEndpoint(snapshot: *const Snapshot, dump_id: i64) CStr {
    return if (dump_id > 0 and dump_id <= snapshot.node_count)
        snapshot.node_stable_ids[@intCast(dump_id - 1)]
    else
        null;
}

fn edgeEqual(before: *const Snapshot, before_index: usize, after: *const PublishEdge) bool {
    const left = &before.edges[before_index];
    const right = after.source;
    return textEqual(snapshotEdgeEndpoint(before, left.source_id), after.source_stable_id) and
        textEqual(snapshotEdgeEndpoint(before, left.target_id), after.target_stable_id) and
        textEqual(left.project, right.*.project) and textEqual(left.type, right.*.type) and
        textEqual(left.properties, right.*.properties) and
        textEqual(left.url_path, right.*.url_path) and
        textEqual(left.local_name, right.*.local_name);
}

fn topologyCompare(left_source: CStr, left_type: CStr, left_target: CStr, right_source: CStr, right_type: CStr, right_target: CStr) c_int {
    var order = c.strcmp(left_source, right_source);
    if (order == 0) order = c.strcmp(left_type, right_type);
    if (order == 0) order = c.strcmp(left_target, right_target);
    return order;
}

fn blobEqual(left: [*c]const u8, right: [*c]const u8, len: c_int) bool {
    if (len < 0) return false;
    if (len == 0) return true;
    if (left == null or right == null) return false;
    const n: usize = @intCast(len);
    return std.mem.eql(u8, left[0..n], right[0..n]);
}

fn vectorEqual(left: *const c.CBMDumpVector, right: *const c.CBMDumpVector) bool {
    return textEqual(left.project, right.project) and left.vector_len == right.vector_len and
        blobEqual(left.vector, right.vector, left.vector_len);
}

fn tokenVectorEqual(left: *const c.CBMDumpTokenVec, right: *const c.CBMDumpTokenVec) bool {
    return textEqual(left.project, right.project) and textEqual(left.token, right.token) and
        left.idf == right.idf and left.vector_len == right.vector_len and
        blobEqual(left.vector, right.vector, left.vector_len);
}

fn hashEqual(left: *const FileHash, right: *const FileHash) bool {
    return textEqual(left.file_path, right.file_path) and
        textEqual(left.content_hash, right.content_hash) and
        left.mtime_ns == right.mtime_ns and left.size_bytes == right.size_bytes;
}

fn summaryEqual(left: *const Summary, right: *const Summary) bool {
    return left.present == right.present and
        (!left.present or
            (textEqual(left.summary, right.summary) and
                textEqual(left.source_hash, right.source_hash) and
                textEqual(left.created_at, right.created_at) and
                textEqual(left.updated_at, right.updated_at)));
}

/// A zeroed array of `count` rows; NULL (and no error) for count <= 0.
fn deltaAlloc(comptime T: type, count: c_int) error{OutOfMemory}![*c]T {
    if (count <= 0) return null;
    const items = interop.callocN(T, @intCast(count));
    if (items == null) return error.OutOfMemory;
    return items;
}

fn allocateArrays(delta: *Delta, before: *const Snapshot, after: *const Model, required: Components) error{OutOfMemory}!void {
    const node_count = c.cbm_zova_publish_model_node_count(after);
    const edge_count = c.cbm_zova_publish_model_edge_count(after);
    const topology = required & TOPOLOGY != 0;
    const node_vectors = required & NODE_VECTORS != 0;
    const token_vectors = required & TOKEN_VECTORS != 0;
    delta.node_inserts = try deltaAlloc([*c]const PublishNode, node_count);
    delta.node_updates = try deltaAlloc([*c]const PublishNode, node_count);
    delta.node_deletes = try deltaAlloc([*c]u8, before.node_count);
    delta.edge_inserts = try deltaAlloc([*c]const PublishEdge, edge_count);
    delta.edge_deletes = try deltaAlloc(EdgeDelete, before.edge_count +| edge_count);
    delta.topology_inserts = try deltaAlloc(TopologyEdge, if (topology) c.cbm_zova_publish_model_topology_count(after) else 0);
    delta.topology_deletes = try deltaAlloc(TopologyEdge, if (topology) before.topology_edge_count else 0);
    delta.node_vector_upserts = try deltaAlloc([*c]const PublishNodeVector, if (node_vectors) c.cbm_zova_publish_model_node_vector_count(after) else 0);
    delta.node_vector_deletes = try deltaAlloc([*c]u8, if (node_vectors) before.node_vector_count else 0);
    delta.token_vector_upserts = try deltaAlloc([*c]const PublishTokenVector, if (token_vectors) c.cbm_zova_publish_model_token_vector_count(after) else 0);
    delta.token_vector_deletes = try deltaAlloc([*c]u8, if (token_vectors) before.token_vector_count else 0);
    delta.file_hash_upserts = try deltaAlloc([*c]const FileHash, c.cbm_zova_publish_model_file_hash_count(after));
    delta.file_hash_deletes = try deltaAlloc([*c]u8, before.file_hash_count);
}

fn appendDelete(values: [*c][*c]u8, count: *u64, id: CStr) c_int {
    const copy = deltaDup(id);
    if (copy == null) return -1;
    values[count.*] = copy;
    count.* += 1;
    return 0;
}

/// Which side of an ordered merge comes first: <0 the snapshot row (left),
/// >0 the model row (right), 0 both carry the same key.
fn mergeOrder(left: c_int, left_count: c_int, right: c_int, right_count: c_int, left_key: CStr, right_key: CStr) c_int {
    if (left >= left_count) return 1;
    if (right >= right_count) return -1;
    return c.strcmp(left_key, right_key);
}

fn buildNodes(delta: *Delta, before: *const Snapshot, after: *Model) c_int {
    const no_ordinal = std.math.maxInt(u64);
    var next_source_ordinal: u64 = 0;
    for (0..@intCast(before.node_count)) |i| {
        const ordinal = before.node_source_ordinals[i];
        if (ordinal == no_ordinal) return -1;
        if (ordinal >= next_source_ordinal) next_source_ordinal = ordinal + 1;
    }
    var left: c_int = 0;
    var right: c_int = 0;
    const right_count = c.cbm_zova_publish_model_node_count(after);
    while (left < before.node_count or right < right_count) {
        var node: [*c]const PublishNode = if (right < right_count) c.cbm_zova_publish_model_node_at(after, right) else null;
        const li: usize = @intCast(left);
        const order = mergeOrder(left, before.node_count, right, right_count, if (left < before.node_count) before.node_stable_ids[li] else null, if (node != null) node.*.stable_id else null);
        if (order < 0) {
            if (appendDelete(delta.node_deletes, &delta.metrics.node_deletes, before.node_stable_ids[li]) != 0) return -1;
            left += 1;
        } else if (order > 0) {
            if (next_source_ordinal == no_ordinal or
                c.cbm_zova_publish_model_set_node_source_ordinal(after, right, next_source_ordinal) != 0) return -1;
            next_source_ordinal += 1;
            node = c.cbm_zova_publish_model_node_at(after, right);
            delta.node_inserts[delta.metrics.node_inserts] = node;
            delta.metrics.node_inserts += 1;
            right += 1;
        } else {
            const equal = nodeEqual(before, li, node);
            const old_ordinal = before.node_source_ordinals[li];
            var desired_ordinal = old_ordinal;
            if (!equal and node.*.source_ordinal != old_ordinal) {
                if (next_source_ordinal == no_ordinal) return -1;
                desired_ordinal = next_source_ordinal;
                next_source_ordinal += 1;
            }
            if (c.cbm_zova_publish_model_set_node_source_ordinal(after, right, desired_ordinal) != 0) return -1;
            node = c.cbm_zova_publish_model_node_at(after, right);
            if (!equal) {
                delta.node_updates[delta.metrics.node_updates] = node;
                delta.metrics.node_updates += 1;
            }
            left += 1;
            right += 1;
        }
    }
    return 0;
}

fn appendEdgeDelete(delta: *Delta, before: *const Snapshot, index: usize, edge_id: *const EdgeId) c_int {
    const old = &before.edges[index];
    const deleted = &delta.edge_deletes[delta.metrics.edge_deletes];
    delta.metrics.edge_deletes += 1;
    deleted.edge_id = deltaDup(edge_id);
    deleted.source_stable_id = deltaDup(snapshotEdgeEndpoint(before, old.source_id));
    deleted.edge_type = deltaDup(old.type);
    deleted.target_stable_id = deltaDup(snapshotEdgeEndpoint(before, old.target_id));
    deleted.local_name = deltaDup(if (old.local_name != null) old.local_name else "");
    return if (deleted.edge_id == null or deleted.source_stable_id == null or deleted.edge_type == null or
        deleted.target_stable_id == null or deleted.local_name == null) -1 else 0;
}

fn buildEdges(delta: *Delta, before: *const Snapshot, after: *const Model) c_int {
    var left: c_int = 0;
    var right: c_int = 0;
    const right_count = c.cbm_zova_publish_model_edge_count(after);
    while (left < before.edge_count or right < right_count) {
        var before_edge_id: EdgeId = @splat(0);
        if (left < before.edge_count and snapshotEdgeIdAt(before, left, &before_edge_id) != 0) return -1;
        const edge: [*c]const PublishEdge = if (right < right_count) c.cbm_zova_publish_model_edge_at(after, right) else null;
        const li: usize = @intCast(left);
        const order = mergeOrder(left, before.edge_count, right, right_count, &before_edge_id, if (edge != null) edge.*.edge_id else null);
        if (order < 0) {
            if (appendEdgeDelete(delta, before, li, &before_edge_id) != 0) return -1;
            left += 1;
        } else if (order > 0) {
            delta.edge_inserts[delta.metrics.edge_inserts] = edge;
            delta.metrics.edge_inserts += 1;
            right += 1;
        } else {
            if (!edgeEqual(before, li, edge)) {
                if (appendEdgeDelete(delta, before, li, &before_edge_id) != 0) return -1;
                delta.edge_inserts[delta.metrics.edge_inserts] = edge;
                delta.metrics.edge_inserts += 1;
            }
            left += 1;
            right += 1;
        }
    }
    return 0;
}

fn buildTopology(delta: *Delta, before: *const Snapshot, after: *const Model) c_int {
    var left: c_int = 0;
    var right: c_int = 0;
    const right_count = c.cbm_zova_publish_model_topology_count(after);
    while (left < before.topology_edge_count or right < right_count) {
        const edge: [*c]const PublishEdge = if (right < right_count) c.cbm_zova_publish_model_topology_at(after, right) else null;
        const old: [*c]const c.cbm_zova_snapshot_topology_edge_t = if (left < before.topology_edge_count) &before.topology_edges[@intCast(left)] else null;
        const order: c_int = if (left >= before.topology_edge_count)
            1
        else if (right >= right_count)
            -1
        else
            topologyCompare(old.*.source_stable_id, old.*.edge_type, old.*.target_stable_id, edge.*.source_stable_id, edge.*.source.*.type, edge.*.target_stable_id);
        if (order < 0) {
            const row = &delta.topology_deletes[delta.metrics.topology_deletes];
            delta.metrics.topology_deletes += 1;
            row.source_stable_id = deltaDup(old.*.source_stable_id);
            row.edge_type = deltaDup(old.*.edge_type);
            row.target_stable_id = deltaDup(old.*.target_stable_id);
            if (row.source_stable_id == null or row.edge_type == null or row.target_stable_id == null) return -1;
            left += 1;
        } else if (order > 0) {
            delta.topology_inserts[delta.metrics.topology_inserts] = .{
                .source_stable_id = edge.*.source_stable_id,
                .edge_type = edge.*.source.*.type,
                .target_stable_id = edge.*.target_stable_id,
            };
            delta.metrics.topology_inserts += 1;
            right += 1;
        } else {
            left += 1;
            right += 1;
        }
    }
    return 0;
}

fn buildNodeVectors(delta: *Delta, before: *const Snapshot, after: *const Model) c_int {
    var left: c_int = 0;
    var right: c_int = 0;
    const right_count = c.cbm_zova_publish_model_node_vector_count(after);
    while (left < before.node_vector_count or right < right_count) {
        const vector: [*c]const PublishNodeVector = if (right < right_count) c.cbm_zova_publish_model_node_vector_at(after, right) else null;
        const li: usize = @intCast(left);
        const order = mergeOrder(left, before.node_vector_count, right, right_count, if (left < before.node_vector_count) before.node_vector_ids[li] else null, if (vector != null) vector.*.stable_id else null);
        if (order < 0) {
            if (appendDelete(delta.node_vector_deletes, &delta.metrics.node_vector_deletes, before.node_vector_ids[li]) != 0) return -1;
            left += 1;
        } else if (order > 0) {
            delta.node_vector_upserts[delta.metrics.node_vector_upserts] = vector;
            delta.metrics.node_vector_upserts += 1;
            right += 1;
        } else {
            if (!vectorEqual(before.node_vectors + li, vector.*.source)) {
                delta.node_vector_upserts[delta.metrics.node_vector_upserts] = vector;
                delta.metrics.node_vector_upserts += 1;
            }
            left += 1;
            right += 1;
        }
    }
    return 0;
}

fn buildTokenVectors(delta: *Delta, before: *const Snapshot, after: *const Model) c_int {
    var left: c_int = 0;
    var right: c_int = 0;
    const right_count = c.cbm_zova_publish_model_token_vector_count(after);
    while (left < before.token_vector_count or right < right_count) {
        const vector: [*c]const PublishTokenVector = if (right < right_count) c.cbm_zova_publish_model_token_vector_at(after, right) else null;
        const li: usize = @intCast(left);
        const order = mergeOrder(left, before.token_vector_count, right, right_count, if (left < before.token_vector_count) before.token_vector_ids[li] else null, if (vector != null) vector.*.token_id else null);
        if (order < 0) {
            if (appendDelete(delta.token_vector_deletes, &delta.metrics.token_vector_deletes, before.token_vectors[li].token) != 0) return -1;
            left += 1;
        } else if (order > 0) {
            delta.token_vector_upserts[delta.metrics.token_vector_upserts] = vector;
            delta.metrics.token_vector_upserts += 1;
            right += 1;
        } else {
            if (!tokenVectorEqual(before.token_vectors + li, vector.*.source)) {
                delta.token_vector_upserts[delta.metrics.token_vector_upserts] = vector;
                delta.metrics.token_vector_upserts += 1;
            }
            left += 1;
            right += 1;
        }
    }
    return 0;
}

fn buildHashes(delta: *Delta, before: *const Snapshot, after: *const Model) c_int {
    var left: c_int = 0;
    var right: c_int = 0;
    const right_count = c.cbm_zova_publish_model_file_hash_count(after);
    while (left < before.file_hash_count or right < right_count) {
        const hash: [*c]const FileHash = if (right < right_count) c.cbm_zova_publish_model_file_hash_at(after, right) else null;
        const li: usize = @intCast(left);
        const order = mergeOrder(left, before.file_hash_count, right, right_count, if (left < before.file_hash_count) before.file_hashes[li].file_path else null, if (hash != null) hash.*.file_path else null);
        if (order < 0) {
            if (appendDelete(delta.file_hash_deletes, &delta.metrics.file_hash_deletes, before.file_hashes[li].file_path) != 0) return -1;
            left += 1;
        } else if (order > 0) {
            delta.file_hash_upserts[delta.metrics.file_hash_upserts] = hash;
            delta.metrics.file_hash_upserts += 1;
            right += 1;
        } else {
            if (!hashEqual(before.file_hashes + li, hash)) {
                delta.file_hash_upserts[delta.metrics.file_hash_upserts] = hash;
                delta.metrics.file_hash_upserts += 1;
            }
            left += 1;
            right += 1;
        }
    }
    return 0;
}

fn buildAll(delta: *Delta, before: *const Snapshot, after: *Model, required: Components) c_int {
    allocateArrays(delta, before, after, required) catch return -1;
    if (buildNodes(delta, before, after) != 0 or buildEdges(delta, before, after) != 0) return -1;
    if (required & TOPOLOGY != 0 and buildTopology(delta, before, after) != 0) return -1;
    if (required & NODE_VECTORS != 0 and buildNodeVectors(delta, before, after) != 0) return -1;
    if (required & TOKEN_VECTORS != 0 and buildTokenVectors(delta, before, after) != 0) return -1;
    return buildHashes(delta, before, after);
}

pub export fn cbm_zova_workspace_delta_build(before: ?*const Snapshot, after: ?*Model, out_delta: ?*?*Delta) c_int {
    const b = before orelse return -1;
    const a = after orelse return -1;
    const out = out_delta orelse return -1;
    out.* = null;
    var required: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    if (b.generation <= 0 or
        cbm_zova_workspace_delta_required_components(b, a, &required) != 0 or
        (b.hydrated_components & required) != required or
        (b.node_count > 0 and b.node_source_ordinals == null) or
        !sortedUnique(b.node_stable_ids, b.node_count) or
        !snapshotEdgeIdsSortedUnique(b) or
        (required & NODE_VECTORS != 0 and !sortedUnique(b.node_vector_ids, b.node_vector_count)) or
        (required & TOKEN_VECTORS != 0 and !sortedUnique(b.token_vector_ids, b.token_vector_count))) return -1;
    var started: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &started);
    const delta_ptr: ?*Delta = interop.callocN(Delta, 1);
    const delta = delta_ptr orelse return -1;
    if (buildAll(delta, b, a, required) != 0) {
        cbm_zova_workspace_delta_free(delta);
        return -1;
    }
    const input: [*c]const c.cbm_zova_workspace_generation_input_t = c.cbm_zova_publish_model_input(a);
    delta.summary = &input.*.project_summary;
    delta.expected_generation = b.generation;
    delta.replace_summary = !summaryEqual(&b.project_summary, delta.summary);
    delta.metrics.summary_replacements = if (delta.replace_summary) 1 else 0;
    delta.metrics.diff_ms = deltaElapsedMs(started);
    out.* = delta;
    return 0;
}

pub export fn cbm_zova_workspace_delta_expected_generation(delta: ?*const Delta) i64 {
    return if (delta) |d| d.expected_generation else 0;
}

fn freeConst(p: anytype) void {
    c.free(@ptrCast(@constCast(p)));
}

fn freeStrings(values: [*c][*c]u8, count: u64) void {
    for (0..count) |i| c.free(values[i]);
}

pub export fn cbm_zova_workspace_delta_free(delta: ?*Delta) void {
    const d = delta orelse return;
    freeStrings(d.node_deletes, d.metrics.node_deletes);
    for (0..d.metrics.edge_deletes) |i| {
        const row = &d.edge_deletes[i];
        freeConst(row.edge_id);
        freeConst(row.source_stable_id);
        freeConst(row.edge_type);
        freeConst(row.target_stable_id);
        freeConst(row.local_name);
    }
    for (0..d.metrics.topology_deletes) |i| {
        const row = &d.topology_deletes[i];
        freeConst(row.source_stable_id);
        freeConst(row.edge_type);
        freeConst(row.target_stable_id);
    }
    freeStrings(d.node_vector_deletes, d.metrics.node_vector_deletes);
    freeStrings(d.token_vector_deletes, d.metrics.token_vector_deletes);
    freeStrings(d.file_hash_deletes, d.metrics.file_hash_deletes);
    freeConst(d.node_inserts);
    freeConst(d.node_updates);
    freeConst(d.node_deletes);
    freeConst(d.edge_inserts);
    freeConst(d.edge_deletes);
    freeConst(d.topology_inserts);
    freeConst(d.topology_deletes);
    freeConst(d.node_vector_upserts);
    freeConst(d.node_vector_deletes);
    freeConst(d.token_vector_upserts);
    freeConst(d.token_vector_deletes);
    freeConst(d.file_hash_upserts);
    freeConst(d.file_hash_deletes);
    c.free(d);
}

pub export fn cbm_zova_workspace_delta_metrics(delta: ?*const Delta, out_metrics: ?*Metrics) void {
    const out = out_metrics orelse return;
    out.* = if (delta) |d| d.metrics else std.mem.zeroes(Metrics);
}

/// items[index], or NULL when the index is outside [0, count).
fn rowAt(items: anytype, count: u64, index: c_int) @typeInfo(@TypeOf(items)).pointer.child {
    if (index < 0 or @as(u64, @intCast(index)) >= count) return null;
    return items[@intCast(index)];
}

/// &items[index], or NULL when the index is outside [0, count).
fn rowPtrAt(comptime T: type, items: [*c]const T, count: u64, index: c_int) ?*const T {
    if (index < 0 or @as(u64, @intCast(index)) >= count) return null;
    return &items[@intCast(index)];
}

pub export fn cbm_zova_workspace_delta_node_insert_at(delta: ?*const Delta, index: c_int) [*c]const PublishNode {
    const d = delta orelse return null;
    return rowAt(d.node_inserts, d.metrics.node_inserts, index);
}

pub export fn cbm_zova_workspace_delta_node_update_at(delta: ?*const Delta, index: c_int) [*c]const PublishNode {
    const d = delta orelse return null;
    return rowAt(d.node_updates, d.metrics.node_updates, index);
}

pub export fn cbm_zova_workspace_delta_node_delete_at(delta: ?*const Delta, index: c_int) CStr {
    const d = delta orelse return null;
    return rowAt(d.node_deletes, d.metrics.node_deletes, index);
}

pub export fn cbm_zova_workspace_delta_edge_insert_at(delta: ?*const Delta, index: c_int) [*c]const PublishEdge {
    const d = delta orelse return null;
    return rowAt(d.edge_inserts, d.metrics.edge_inserts, index);
}

pub export fn cbm_zova_workspace_delta_node_vector_upsert_at(delta: ?*const Delta, index: c_int) [*c]const PublishNodeVector {
    const d = delta orelse return null;
    return rowAt(d.node_vector_upserts, d.metrics.node_vector_upserts, index);
}

pub export fn cbm_zova_workspace_delta_node_vector_delete_at(delta: ?*const Delta, index: c_int) CStr {
    const d = delta orelse return null;
    return rowAt(d.node_vector_deletes, d.metrics.node_vector_deletes, index);
}

pub export fn cbm_zova_workspace_delta_token_vector_upsert_at(delta: ?*const Delta, index: c_int) [*c]const PublishTokenVector {
    const d = delta orelse return null;
    return rowAt(d.token_vector_upserts, d.metrics.token_vector_upserts, index);
}

pub export fn cbm_zova_workspace_delta_token_vector_delete_at(delta: ?*const Delta, index: c_int) CStr {
    const d = delta orelse return null;
    return rowAt(d.token_vector_deletes, d.metrics.token_vector_deletes, index);
}

pub export fn cbm_zova_workspace_delta_file_hash_upsert_at(delta: ?*const Delta, index: c_int) [*c]const FileHash {
    const d = delta orelse return null;
    return rowAt(d.file_hash_upserts, d.metrics.file_hash_upserts, index);
}

pub export fn cbm_zova_workspace_delta_file_hash_delete_at(delta: ?*const Delta, index: c_int) CStr {
    const d = delta orelse return null;
    return rowAt(d.file_hash_deletes, d.metrics.file_hash_deletes, index);
}

pub export fn cbm_zova_workspace_delta_edge_delete_at(delta: ?*const Delta, index: c_int) ?*const EdgeDelete {
    const d = delta orelse return null;
    return rowPtrAt(EdgeDelete, d.edge_deletes, d.metrics.edge_deletes, index);
}

pub export fn cbm_zova_workspace_delta_topology_insert_at(delta: ?*const Delta, index: c_int) ?*const TopologyEdge {
    const d = delta orelse return null;
    return rowPtrAt(TopologyEdge, d.topology_inserts, d.metrics.topology_inserts, index);
}

pub export fn cbm_zova_workspace_delta_topology_delete_at(delta: ?*const Delta, index: c_int) ?*const TopologyEdge {
    const d = delta orelse return null;
    return rowPtrAt(TopologyEdge, d.topology_deletes, d.metrics.topology_deletes, index);
}

pub export fn cbm_zova_workspace_delta_replaces_summary(delta: ?*const Delta) bool {
    return if (delta) |d| d.replace_summary else false;
}

pub export fn cbm_zova_workspace_delta_summary(delta: ?*const Delta) [*c]const Summary {
    return if (delta) |d| d.summary else null;
}
