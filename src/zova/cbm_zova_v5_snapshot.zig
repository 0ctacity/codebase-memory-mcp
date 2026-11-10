//! cbm_zova_v5_snapshot.zig — Read-only, fully validated view of a schema-5
//! cbm.zova file, as the input of an atomic repack.
//!
//! Implements src/zova/cbm_zova_v5_snapshot.h. Opening reads every ready
//! workspace into memory and checks it against the stored FTS rows and
//! generation digests; any mismatch fails the open.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const Str = interop.Str;
const CStr = interop.CStr;
const Stmt = c.sqlite3_stmt;
const Input = c.cbm_zova_workspace_generation_input_t;

const V5Workspace = struct {
    workspace_id: [*c]u8 = null,
    active_generation: i64 = 0,
    input: Input = std.mem.zeroes(Input),
    root_path: [*c]u8 = null,
    project: [*c]u8 = null,
    indexed_at: [*c]u8 = null,
    model_fingerprint: [*c]u8 = null,
    nodes: []c.CBMDumpNode = &.{},
    stable_node_ids: [][*c]u8 = &.{},
    edges: []c.CBMDumpEdge = &.{},
    node_vectors: []c.CBMDumpVector = &.{},
    token_vectors: []c.CBMDumpTokenVec = &.{},
    file_hashes: []c.cbm_zova_file_hash_input_t = &.{},
    summary: [*c]u8 = null,
    summary_source_hash: [*c]u8 = null,
    summary_created_at: [*c]u8 = null,
    summary_updated_at: [*c]u8 = null,
};

const Snapshot = struct {
    db: ?*c.sqlite3 = null,
    workspaces: []V5Workspace = &.{},
};

/// A zeroed malloc'd array; NULL on OOM.
fn callocSlice(comptime T: type, n: usize) ?[]T {
    const items: ?[*]T = interop.callocN(T, n);
    return (items orelse return null)[0..n];
}

/// free() for the borrowed-looking `const` pointers of the dump structs,
/// which this module owns.
fn freeConst(p: anytype) void {
    c.free(@ptrCast(@constCast(p)));
}

/// Frees a callocSlice array. An empty slice owns no allocation.
fn freeSlice(items: anytype) void {
    if (items.len != 0) c.free(@ptrCast(items.ptr));
}

fn finalize(stmt: ?*Stmt) void {
    _ = c.sqlite3_finalize(stmt);
}

fn v5Text(stmt: ?*Stmt, column: c_int) [*c]u8 {
    if (c.sqlite3_column_type(stmt, column) == c.SQLITE_NULL) return null;
    return interop.heapStrdup(c.sqlite3_column_text(stmt, column));
}

fn v5Blob(stmt: ?*Stmt, column: c_int, expected_length: c_int) [*c]u8 {
    const length = c.sqlite3_column_bytes(stmt, column);
    const value: ?[*]const u8 = @ptrCast(c.sqlite3_column_blob(stmt, column));
    const bytes = value orelse return null;
    if (length != expected_length or length <= 0) return null;
    const n: usize = @intCast(length);
    const copy = interop.mallocN(u8, n);
    if (copy != null) @memcpy(copy[0..n], bytes[0..n]);
    return copy;
}

fn v5Scalar(db: ?*c.sqlite3, sql: CStr, text: CStr) ?i64 {
    var stmt: ?*Stmt = null;
    if (db == null or c.sqlite3_prepare_v2(db, sql, -1, &stmt, null) != c.SQLITE_OK) return null;
    defer finalize(stmt);
    if (text != null and c.sqlite3_bind_text(stmt, 1, text, -1, null) != c.SQLITE_OK) return null;
    if (c.sqlite3_step(stmt) != c.SQLITE_ROW) return null;
    const value = c.sqlite3_column_int64(stmt, 0);
    if (c.sqlite3_step(stmt) != c.SQLITE_DONE) return null;
    return value;
}

fn v5Count(db: ?*c.sqlite3, comptime table: []const u8, workspace_id: CStr) ?c_int {
    if (workspace_id == null) return null;
    const count = v5Scalar(db, "SELECT count(*) FROM " ++ table ++ " WHERE workspace_id=?1", workspace_id) orelse return null;
    if (count < 0 or count > std.math.maxInt(c_int)) return null;
    return @intCast(count);
}

/// Prepares `sql` with the workspace id bound to ?1. The caller finalizes.
fn v5PrepareWorkspace(db: ?*c.sqlite3, sql: CStr, workspace_id: CStr) ?*Stmt {
    var stmt: ?*Stmt = null;
    if (c.sqlite3_prepare_v2(db, sql, -1, &stmt, null) != c.SQLITE_OK or
        c.sqlite3_bind_text(stmt, 1, workspace_id, -1, null) != c.SQLITE_OK)
    {
        finalize(stmt);
        return null;
    }
    return stmt;
}

fn v5NodeIndex(workspace: *const V5Workspace, stable_id: CStr) ?usize {
    if (stable_id == null) return null;
    for (workspace.stable_node_ids, 0..) |id, i| {
        if (c.strcmp(id, stable_id) == 0) return i;
    }
    return null;
}

fn v5NodeStableIdMatches(workspace_id: CStr, node: *const c.CBMDumpNode, stable_id: CStr) bool {
    if (workspace_id == null or node.file_path == null or stable_id == null) return false;
    var relative_path: [4096]u8 = undefined;
    var discriminator: [512]u8 = undefined;
    var expected: [c.CBM_ZOVA_WORKSPACE_ID_MAX + 64]u8 = undefined;
    if (interop.bprint(&relative_path, "{s}", .{std.mem.span(node.file_path)}) >= relative_path.len)
        return false;
    _ = c.cbm_normalize_path_sep(&relative_path);
    if (node.qualified_name != null and node.qualified_name[0] != 0) {
        if (interop.bprint(&discriminator, "named:{s}", .{std.mem.span(node.qualified_name)}) >= discriminator.len)
            return false;
    } else if (node.name == null or node.name[0] == 0 or node.start_line < 0 or
        node.end_line < node.start_line or
        interop.bprint(&discriminator, "local:{s}:span:{d}:{d}", .{ std.mem.span(node.name), node.start_line, node.end_line }) >= discriminator.len)
    {
        return false;
    }
    return c.cbm_zova_workspace_node_id_v2(workspace_id, node.label, &relative_path, node.qualified_name, &discriminator, &expected, expected.len) == 0 and
        c.strcmp(&expected, stable_id) == 0;
}

/// The FTS name column of a node: the name, a space, then the name again with
/// a space before each camelCase word boundary. Malloc'd; NULL on failure.
fn v5CamelSplit(input: CStr) [*c]u8 {
    if (input == null) return null;
    const text = std.mem.span(input);
    if (text.len == 0) return interop.heapStrdup("");
    if (text.len > (std.math.maxInt(usize) - 2) / 2) return null;
    const result = interop.mallocN(u8, text.len * 2 + 2);
    if (result == null) return null;
    @memcpy(result[0..text.len], text);
    var len = text.len;
    result[len] = ' ';
    len += 1;
    for (text, 0..) |curr, i| {
        const prev: u8 = if (i > 0) text[i - 1] else 0;
        const next: u8 = if (i + 1 < text.len) text[i + 1] else 0;
        if (i > 0 and std.ascii.isUpper(curr) and
            (std.ascii.isLower(prev) or (std.ascii.isUpper(prev) and std.ascii.isLower(next))))
        {
            result[len] = ' ';
            len += 1;
        }
        result[len] = curr;
        len += 1;
    }
    result[len] = 0;
    return result;
}

fn v5WorkspaceFree(workspace: *V5Workspace) void {
    for (workspace.nodes) |node| {
        freeConst(node.label);
        freeConst(node.name);
        freeConst(node.qualified_name);
        freeConst(node.file_path);
        freeConst(node.properties);
    }
    for (workspace.stable_node_ids) |id| c.free(id);
    for (workspace.edges) |edge| {
        freeConst(edge.type);
        freeConst(edge.properties);
        freeConst(edge.url_path);
        freeConst(edge.local_name);
    }
    for (workspace.node_vectors) |vector| freeConst(vector.vector);
    for (workspace.token_vectors) |vector| {
        freeConst(vector.token);
        freeConst(vector.vector);
    }
    for (workspace.file_hashes) |hash| {
        freeConst(hash.file_path);
        freeConst(hash.content_hash);
    }
    freeSlice(workspace.nodes);
    freeSlice(workspace.stable_node_ids);
    freeSlice(workspace.edges);
    freeSlice(workspace.node_vectors);
    freeSlice(workspace.token_vectors);
    freeSlice(workspace.file_hashes);
    c.free(workspace.workspace_id);
    c.free(workspace.root_path);
    c.free(workspace.project);
    c.free(workspace.indexed_at);
    c.free(workspace.model_fingerprint);
    c.free(workspace.summary);
    c.free(workspace.summary_source_hash);
    c.free(workspace.summary_created_at);
    c.free(workspace.summary_updated_at);
    workspace.* = .{};
}

pub export fn cbm_zova_v5_snapshot_close(snapshot: ?*Snapshot) void {
    const s = snapshot orelse return;
    for (s.workspaces) |*workspace| v5WorkspaceFree(workspace);
    freeSlice(s.workspaces);
    if (s.db != null) {
        _ = c.sqlite3_exec(s.db, "ROLLBACK", null, null, null);
        _ = c.sqlite3_close(s.db);
    }
    c.free(s);
}

fn v5ValidateSource(db: ?*c.sqlite3) c_int {
    const required = [_]Str{
        "_zova_meta",                  "cbm_database_schema_v1",     "cbm_workspace_registry",
        "cbm_database_generation_v1",  "cbm_projects_v1",            "cbm_nodes_v1",
        "cbm_edges_v1",                "cbm_file_hashes_v1",         "cbm_project_summaries_v2",
        "cbm_generation_integrity_v2", "cbm_node_vectors_compat_v1", "cbm_token_vectors_compat_v1",
        "cbm_nodes_fts_v1",            "cbm_fts_rowmap_v1",          "cbm_workspace_index_state_v1",
    };
    var stmt: ?*Stmt = null;
    if (c.sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') " ++
        "AND name=?1", -1, &stmt, null) != c.SQLITE_OK)
        return -1;
    var rc: c_int = 0;
    for (required) |name| {
        if (c.sqlite3_bind_text(stmt, 1, name, -1, null) != c.SQLITE_OK or
            c.sqlite3_step(stmt) != c.SQLITE_ROW or c.sqlite3_column_int64(stmt, 0) != 1)
        {
            rc = -1;
        }
        if (c.sqlite3_reset(stmt) != c.SQLITE_OK or c.sqlite3_clear_bindings(stmt) != c.SQLITE_OK) rc = -1;
        if (rc != 0) break;
    }
    finalize(stmt);
    if (rc != 0) return -1;
    const schema_version = v5Scalar(db, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1", null) orelse return -1;
    if (schema_version != 5) return -1;
    const format_matches = v5Scalar(db, "SELECT count(*) FROM _zova_meta WHERE key='format_version' AND value='7'", null) orelse return -1;
    if (format_matches != 1) return -1;
    return 0;
}

fn v5ReadNodes(db: ?*c.sqlite3, workspace: *V5Workspace) c_int {
    const count = v5Count(db, "cbm_nodes_v1", workspace.workspace_id) orelse return -1;
    workspace.input.node_count = count;
    if (count == 0) return 0;
    workspace.nodes = callocSlice(c.CBMDumpNode, @intCast(count)) orelse return -1;
    workspace.stable_node_ids = callocSlice([*c]u8, @intCast(count)) orelse return -1;
    const sql =
        "SELECT n.node_id,n.label,n.name,n.qualified_name,n.file_path,n.start_line,n.end_line," ++
        "n.properties,m.fts_rowid FROM cbm_nodes_v1 n JOIN cbm_fts_rowmap_v1 m ON " ++
        "m.workspace_id=n.workspace_id AND m.node_id=n.node_id WHERE n.workspace_id=?1 " ++
        "ORDER BY m.fts_rowid";
    const stmt = v5PrepareWorkspace(db, sql, workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    var row: usize = 0;
    while (row < workspace.nodes.len and c.sqlite3_step(stmt) == c.SQLITE_ROW) : (row += 1) {
        const node = &workspace.nodes[row];
        workspace.stable_node_ids[row] = v5Text(stmt, 0);
        node.id = c.sqlite3_column_int64(stmt, 8);
        node.project = workspace.project;
        node.label = v5Text(stmt, 1);
        node.name = v5Text(stmt, 2);
        node.qualified_name = v5Text(stmt, 3);
        node.file_path = v5Text(stmt, 4);
        node.start_line = c.sqlite3_column_int(stmt, 5);
        node.end_line = c.sqlite3_column_int(stmt, 6);
        node.properties = v5Text(stmt, 7);
        if (workspace.stable_node_ids[row] == null or node.label == null or node.name == null or
            node.qualified_name == null or node.file_path == null or node.properties == null or
            node.id <= 0 or (row > 0 and workspace.nodes[row - 1].id >= node.id) or
            !v5NodeStableIdMatches(workspace.workspace_id, node, workspace.stable_node_ids[row]))
        {
            return -1;
        }
    }
    if (row != workspace.nodes.len or c.sqlite3_step(stmt) != c.SQLITE_DONE) return -1;
    workspace.input.nodes = workspace.nodes.ptr;
    return 0;
}

fn v5ReadEdges(db: ?*c.sqlite3, workspace: *V5Workspace) c_int {
    const count = v5Count(db, "cbm_edges_v1", workspace.workspace_id) orelse return -1;
    workspace.input.edge_count = count;
    if (count == 0) return 0;
    workspace.edges = callocSlice(c.CBMDumpEdge, @intCast(count)) orelse return -1;
    const sql =
        "SELECT source_node_id,target_node_id,edge_type,properties,url_path,local_name " ++
        "FROM cbm_edges_v1 WHERE workspace_id=?1 ORDER BY edge_id";
    const stmt = v5PrepareWorkspace(db, sql, workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    var row: usize = 0;
    while (row < workspace.edges.len and c.sqlite3_step(stmt) == c.SQLITE_ROW) : (row += 1) {
        const source = v5Text(stmt, 0);
        const target = v5Text(stmt, 1);
        const source_index = v5NodeIndex(workspace, source);
        const target_index = v5NodeIndex(workspace, target);
        c.free(source);
        c.free(target);
        const edge = &workspace.edges[row];
        edge.id = @intCast(row + 1);
        edge.project = workspace.project;
        edge.source_id = if (source_index) |i| workspace.nodes[i].id else 0;
        edge.target_id = if (target_index) |i| workspace.nodes[i].id else 0;
        edge.type = v5Text(stmt, 2);
        edge.properties = v5Text(stmt, 3);
        edge.url_path = v5Text(stmt, 4);
        edge.local_name = v5Text(stmt, 5);
        if (source_index == null or target_index == null or edge.type == null or edge.type[0] == 0 or
            edge.properties == null or edge.url_path == null or edge.local_name == null)
        {
            return -1;
        }
    }
    if (row != workspace.edges.len or c.sqlite3_step(stmt) != c.SQLITE_DONE) return -1;
    workspace.input.edges = workspace.edges.ptr;
    return 0;
}

fn v5ReadNodeVectors(db: ?*c.sqlite3, workspace: *V5Workspace) c_int {
    const count = v5Count(db, "cbm_node_vectors_compat_v1", workspace.workspace_id) orelse return -1;
    workspace.input.node_vector_count = count;
    if (count == 0) return 0;
    workspace.node_vectors = callocSlice(c.CBMDumpVector, @intCast(count)) orelse return -1;
    const stmt = v5PrepareWorkspace(db, "SELECT node_id,vector FROM cbm_node_vectors_compat_v1 " ++
        "WHERE workspace_id=?1 ORDER BY node_id", workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    var row: usize = 0;
    while (row < workspace.node_vectors.len and c.sqlite3_step(stmt) == c.SQLITE_ROW) : (row += 1) {
        const node_id = v5Text(stmt, 0);
        const node_index = v5NodeIndex(workspace, node_id);
        c.free(node_id);
        const vector = &workspace.node_vectors[row];
        vector.node_id = if (node_index) |i| workspace.nodes[i].id else 0;
        vector.project = workspace.project;
        vector.vector_len = workspace.input.vector_dimensions;
        vector.vector = v5Blob(stmt, 1, vector.vector_len);
        if (node_index == null or vector.vector == null) return -1;
    }
    if (row != workspace.node_vectors.len or c.sqlite3_step(stmt) != c.SQLITE_DONE) return -1;
    workspace.input.node_vectors = workspace.node_vectors.ptr;
    return 0;
}

fn v5ReadTokenVectors(db: ?*c.sqlite3, workspace: *V5Workspace) c_int {
    const count = v5Count(db, "cbm_token_vectors_compat_v1", workspace.workspace_id) orelse return -1;
    workspace.input.token_vector_count = count;
    if (count == 0) return 0;
    workspace.token_vectors = callocSlice(c.CBMDumpTokenVec, @intCast(count)) orelse return -1;
    const stmt = v5PrepareWorkspace(db, "SELECT token_id,token,vector,idf FROM cbm_token_vectors_compat_v1 " ++
        "WHERE workspace_id=?1 ORDER BY token_id", workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    var row: usize = 0;
    var previous_id: i64 = 0;
    while (row < workspace.token_vectors.len and c.sqlite3_step(stmt) == c.SQLITE_ROW) : (row += 1) {
        const vector = &workspace.token_vectors[row];
        vector.id = c.sqlite3_column_int64(stmt, 0);
        vector.project = workspace.project;
        vector.token = v5Text(stmt, 1);
        vector.vector_len = workspace.input.vector_dimensions;
        vector.vector = v5Blob(stmt, 2, vector.vector_len);
        vector.idf = @floatCast(c.sqlite3_column_double(stmt, 3));
        if (vector.id <= previous_id or vector.token == null or vector.vector == null) return -1;
        previous_id = vector.id;
    }
    if (row != workspace.token_vectors.len or c.sqlite3_step(stmt) != c.SQLITE_DONE) return -1;
    workspace.input.token_vectors = workspace.token_vectors.ptr;
    return 0;
}

fn v5ReadHashes(db: ?*c.sqlite3, workspace: *V5Workspace) c_int {
    const count = v5Count(db, "cbm_file_hashes_v1", workspace.workspace_id) orelse return -1;
    workspace.input.file_hash_count = count;
    if (count == 0) return 0;
    workspace.file_hashes = callocSlice(c.cbm_zova_file_hash_input_t, @intCast(count)) orelse return -1;
    const stmt = v5PrepareWorkspace(db, "SELECT file_path,content_hash,mtime_ns,size_bytes FROM " ++
        "cbm_file_hashes_v1 WHERE workspace_id=?1 ORDER BY file_path", workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    var row: usize = 0;
    while (row < workspace.file_hashes.len and c.sqlite3_step(stmt) == c.SQLITE_ROW) : (row += 1) {
        const hash = &workspace.file_hashes[row];
        hash.file_path = v5Text(stmt, 0);
        hash.content_hash = v5Text(stmt, 1);
        hash.mtime_ns = c.sqlite3_column_int64(stmt, 2);
        hash.size_bytes = c.sqlite3_column_int64(stmt, 3);
        if (hash.file_path == null or hash.content_hash == null or
            (row > 0 and c.strcmp(workspace.file_hashes[row - 1].file_path, hash.file_path) >= 0))
        {
            return -1;
        }
    }
    if (row != workspace.file_hashes.len or c.sqlite3_step(stmt) != c.SQLITE_DONE) return -1;
    workspace.input.file_hashes = workspace.file_hashes.ptr;
    return 0;
}

fn v5ReadSummary(db: ?*c.sqlite3, workspace: *V5Workspace) c_int {
    const stmt = v5PrepareWorkspace(db, "SELECT summary,source_hash,created_at,updated_at FROM " ++
        "cbm_project_summaries_v2 WHERE workspace_id=?1", workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    const step = c.sqlite3_step(stmt);
    if (step == c.SQLITE_DONE) return 0;
    if (step != c.SQLITE_ROW) return -1;
    workspace.summary = v5Text(stmt, 0);
    workspace.summary_source_hash = v5Text(stmt, 1);
    workspace.summary_created_at = v5Text(stmt, 2);
    workspace.summary_updated_at = v5Text(stmt, 3);
    if (workspace.summary == null or workspace.summary_source_hash == null or
        workspace.summary_created_at == null or workspace.summary_updated_at == null or
        c.sqlite3_step(stmt) != c.SQLITE_DONE)
    {
        return -1;
    }
    workspace.input.project_summary = .{
        .present = true,
        .summary = workspace.summary,
        .source_hash = workspace.summary_source_hash,
        .created_at = workspace.summary_created_at,
        .updated_at = workspace.summary_updated_at,
    };
    return 0;
}

fn v5ValidateFts(db: ?*c.sqlite3, workspace: *const V5Workspace) c_int {
    const node_count = workspace.input.node_count;
    const fts_count = v5Count(db, "cbm_nodes_fts_v1", workspace.workspace_id) orelse return -1;
    const rowmap_count = v5Count(db, "cbm_fts_rowmap_v1", workspace.workspace_id) orelse return -1;
    if (fts_count != node_count or rowmap_count != node_count) return -1;
    const joined_sql =
        "SELECT count(*) FROM cbm_fts_rowmap_v1 m " ++
        "JOIN cbm_nodes_fts_v1 f ON f.workspace_id=m.workspace_id AND f.node_id=m.node_id " ++
        "JOIN cbm_nodes_v1 n ON n.workspace_id=m.workspace_id " ++
        "AND n.node_id=m.node_id WHERE m.workspace_id=?1 AND " ++
        "f.qualified_name=n.qualified_name AND f.file_path=n.file_path AND f.label=n.label";
    const joined = v5Scalar(db, joined_sql, workspace.workspace_id) orelse return -1;
    if (joined != node_count) return -1;
    const stmt = v5PrepareWorkspace(db, "SELECT f.name FROM cbm_fts_rowmap_v1 m JOIN cbm_nodes_fts_v1 f " ++
        "ON f.workspace_id=m.workspace_id AND f.node_id=m.node_id WHERE " ++
        "m.workspace_id=?1 ORDER BY m.fts_rowid", workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    var step: c_int = c.SQLITE_ROW;
    var row: usize = 0;
    while (row < workspace.nodes.len) : (row += 1) {
        step = c.sqlite3_step(stmt);
        if (step != c.SQLITE_ROW) break;
        const actual: CStr = c.sqlite3_column_text(stmt, 0);
        if (actual == null) return -1;
        const expected = v5CamelSplit(workspace.nodes[row].name);
        defer c.free(expected);
        if (expected == null or c.strcmp(actual, expected) != 0) return -1;
    }
    if (row == workspace.nodes.len and step == c.SQLITE_ROW) step = c.sqlite3_step(stmt);
    return if (row == workspace.nodes.len and step == c.SQLITE_DONE) 0 else -1;
}

fn v5ValidateIntegrity(db: ?*c.sqlite3, workspace: *const V5Workspace) c_int {
    var actual = std.mem.zeroes(c.cbm_zova_workspace_generation_result_t);
    if (c.cbm_zova_workspace_generation_digest_input(workspace.workspace_id, &workspace.input, &actual) != 0)
        return -1;
    const sql =
        "SELECT graph_nodes,graph_edges,metadata_nodes,metadata_edges,metadata_topology_edges," ++
        "fts_rows,node_vector_rows,token_vector_rows,node_vectors,token_vectors,metadata_sha256," ++
        "fts_sha256,topology_sha256,node_vector_sha256,token_vector_sha256 FROM " ++
        "cbm_generation_integrity_v2 WHERE workspace_id=?1 AND generation=?2";
    const stmt = v5PrepareWorkspace(db, sql, workspace.workspace_id) orelse return -1;
    defer finalize(stmt);
    if (c.sqlite3_bind_int64(stmt, 2, workspace.active_generation) != c.SQLITE_OK or
        c.sqlite3_step(stmt) != c.SQLITE_ROW)
        return -1;
    const expected_counts = [_]u64{
        actual.graph_nodes,             actual.graph_edges,   actual.metadata_nodes,   actual.metadata_edges,
        actual.metadata_topology_edges, actual.fts_rows,      actual.node_vector_rows, actual.token_vector_rows,
        actual.node_vectors,            actual.token_vectors,
    };
    for (expected_counts, 0..) |expected, i| {
        const value = c.sqlite3_column_int64(stmt, @intCast(i));
        if (value < 0 or @as(u64, @intCast(value)) != expected) return -1;
    }
    const expected_digests = [_]CStr{
        &actual.metadata_sha256,    &actual.fts_sha256,          &actual.topology_sha256,
        &actual.node_vector_sha256, &actual.token_vector_sha256,
    };
    for (expected_digests, 0..) |expected, i| {
        const digest: CStr = c.sqlite3_column_text(stmt, @intCast(10 + i));
        if (digest == null or c.strcmp(digest, expected) != 0) return -1;
    }
    return if (c.sqlite3_step(stmt) == c.SQLITE_DONE) 0 else -1;
}

fn v5ReadWorkspace(db: ?*c.sqlite3, workspace_stmt: ?*Stmt, workspace: *V5Workspace) c_int {
    workspace.workspace_id = v5Text(workspace_stmt, 0);
    workspace.active_generation = c.sqlite3_column_int64(workspace_stmt, 1);
    workspace.project = v5Text(workspace_stmt, 2);
    workspace.root_path = v5Text(workspace_stmt, 3);
    workspace.indexed_at = v5Text(workspace_stmt, 4);
    workspace.model_fingerprint = v5Text(workspace_stmt, 5);
    workspace.input.vector_dimensions = c.sqlite3_column_int(workspace_stmt, 6);
    if (workspace.workspace_id == null or workspace.project == null or workspace.root_path == null or
        workspace.indexed_at == null or workspace.model_fingerprint == null or
        workspace.active_generation <= 0 or workspace.input.vector_dimensions <= 0)
        return -1;
    workspace.input.root_path = workspace.root_path;
    workspace.input.project = workspace.project;
    workspace.input.indexed_at = workspace.indexed_at;
    workspace.input.model_fingerprint = workspace.model_fingerprint;
    return if (v5ReadNodes(db, workspace) == 0 and v5ReadEdges(db, workspace) == 0 and
        v5ReadNodeVectors(db, workspace) == 0 and v5ReadTokenVectors(db, workspace) == 0 and
        v5ReadHashes(db, workspace) == 0 and v5ReadSummary(db, workspace) == 0 and
        v5ValidateFts(db, workspace) == 0 and v5ValidateIntegrity(db, workspace) == 0) 0 else -1;
}

fn v5Open(snapshot: *Snapshot, source_path: Str) c_int {
    const flags = c.SQLITE_OPEN_READONLY | c.SQLITE_OPEN_NOMUTEX;
    if (c.sqlite3_open_v2(source_path, &snapshot.db, flags, null) != c.SQLITE_OK or
        c.sqlite3_exec(snapshot.db, "PRAGMA query_only=ON;BEGIN", null, null, null) != c.SQLITE_OK or
        v5ValidateSource(snapshot.db) != 0)
        return -1;
    const count_sql =
        "SELECT count(*) FROM cbm_workspace_registry r JOIN cbm_projects_v1 p USING(workspace_id) " ++
        "JOIN cbm_workspace_index_state_v1 s USING(workspace_id) JOIN cbm_database_generation_v1 g " ++
        "ON g.workspace_id=r.workspace_id AND g.generation=r.active_generation " ++
        "WHERE r.active_generation>0 AND s.generation=r.active_generation AND g.state='ready'";
    const count = v5Scalar(snapshot.db, count_sql, null) orelse return -1;
    if (count <= 0 or count > std.math.maxInt(c_int)) return -1;
    const workspaces: []V5Workspace = blk: {
        const items: ?[*]V5Workspace = interop.mallocN(V5Workspace, @intCast(count));
        break :blk (items orelse return -1)[0..@intCast(count)];
    };
    for (workspaces) |*workspace| workspace.* = .{};
    snapshot.workspaces = workspaces;
    const sql =
        "SELECT r.workspace_id,r.active_generation,p.project,p.root_path,p.indexed_at," ++
        "s.model_fingerprint,s.vector_dimensions FROM cbm_workspace_registry r " ++
        "JOIN cbm_projects_v1 p USING(workspace_id) JOIN cbm_workspace_index_state_v1 s " ++
        "USING(workspace_id) JOIN cbm_database_generation_v1 g ON " ++
        "g.workspace_id=r.workspace_id AND g.generation=r.active_generation WHERE " ++
        "r.active_generation>0 AND s.generation=r.active_generation AND g.state='ready' " ++
        "ORDER BY r.workspace_id";
    var stmt: ?*Stmt = null;
    if (c.sqlite3_prepare_v2(snapshot.db, sql, -1, &stmt, null) != c.SQLITE_OK) return -1;
    defer finalize(stmt);
    var row: usize = 0;
    while (row < workspaces.len and c.sqlite3_step(stmt) == c.SQLITE_ROW) : (row += 1) {
        if (v5ReadWorkspace(snapshot.db, stmt, &workspaces[row]) != 0) return -1;
    }
    if (row != workspaces.len or c.sqlite3_step(stmt) != c.SQLITE_DONE) return -1;
    return 0;
}

pub export fn cbm_zova_v5_snapshot_open(source_path: ?Str, out_snapshot: ?*?*Snapshot) c_int {
    const out = out_snapshot orelse return -1;
    out.* = null;
    const path = source_path orelse return -1;
    if (path[0] == 0) return -1;
    const snapshot_ptr: ?*Snapshot = interop.mallocN(Snapshot, 1);
    const snapshot = snapshot_ptr orelse return -1;
    snapshot.* = .{};
    if (v5Open(snapshot, path) != 0) {
        cbm_zova_v5_snapshot_close(snapshot);
        return -1;
    }
    out.* = snapshot;
    return 0;
}

pub export fn cbm_zova_v5_snapshot_workspace_count(snapshot: ?*const Snapshot) c_int {
    const s = snapshot orelse return -1;
    return @intCast(s.workspaces.len);
}

pub export fn cbm_zova_v5_snapshot_input_at(snapshot: ?*const Snapshot, index: c_int, out_active_generation: ?*i64) ?*const Input {
    if (out_active_generation) |out| out.* = 0;
    const s = snapshot orelse return null;
    if (index < 0 or index >= s.workspaces.len) return null;
    const workspace = &s.workspaces[@intCast(index)];
    if (out_active_generation) |out| out.* = workspace.active_generation;
    return &workspace.input;
}
