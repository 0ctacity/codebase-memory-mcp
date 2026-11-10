//! store.zig — SQLite graph store implementation.
//!
//! Implements src/store/store.h: the opaque cbm_store_t handle with prepared
//! statement caching, schema initialization, and all CRUD operations for
//! nodes, edges, projects, file hashes, search, BFS traversal, and schema
//! introspection.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const Str = interop.Str;
const CStr = interop.CStr;
const Stmt = c.sqlite3_stmt;

const OK = c.CBM_STORE_OK;
const ERR = c.CBM_STORE_ERR;
const NOT_FOUND = c.CBM_STORE_NOT_FOUND;

const ST_BUF_16 = 16;
const ST_BUF_64 = 64;
/// file: URI for the immutable read-only fallback. A path is at most
/// CBM_SZ_1K; percent-encoding can triple it, plus the "file://" prefix,
/// the "?immutable=1" suffix and a leading '/'.
const ST_QUERY_URI_MAX = 3 * 1024 + 64;
/// Matches the PRAGMA busy_timeout configurePragmas applies.
const ST_BUSY_TIMEOUT_MS = 10000;
const ST_GROWTH = 2;
const ST_MAX_DEGREE = 8192;
const ST_INIT_CAP_8 = 8;
const ST_INIT_CAP_16 = 16;
const ST_SQL_BUF = 8192;
const ST_QN_MAX_DOTS = 5;
const ST_QN_MIN_DOTS = 3;
const ST_IN_CLAUSE_MARGIN = 4;
const ST_GLOB_MIN_LEN = 3;
const ST_MAX_LANG = 10;
const ST_SEARCH_MAX_BINDS = 32; // increased: LIKE pre-filter adds binds per pattern
const ST_LIKE_POOL_MAX = 12; // max malloc'd LIKE strings alive during one search
const ST_LIKE_HINT_MAX = 2; // max LIKE hints extracted per regex pattern
const ST_MAX_PKGS = 64;
const ST_INIT_CAP_4 = 4;
const ST_MIN_INDEGREE = 3;
const ST_MAX_PATH_DEPTH = 3;
const ST_MAX_ITERATIONS = 10;

// ── SQLite bind helpers ─────────────────────────────────────────

/// SQLITE_TRANSIENT, ((void *)-1). Type-punned through memory because -1 is
/// not an aligned function address, so no pointer cast can produce it.
fn transient() c.sqlite3_destructor_type {
    var raw: usize = std.math.maxInt(usize);
    _ = &raw;
    return @as(*const c.sqlite3_destructor_type, @ptrCast(&raw)).*;
}

fn bindText(stmt: ?*Stmt, col: c_int, v: CStr) c_int {
    return c.sqlite3_bind_text(stmt, col, v, -1, transient());
}

fn columnText(stmt: ?*Stmt, col: c_int) CStr {
    return c.sqlite3_column_text(stmt, col);
}

/// A malloc'd copy of a text column; NULL for SQL NULL.
fn columnDup(stmt: ?*Stmt, col: c_int) [*c]u8 {
    return interop.heapStrdup(columnText(stmt, col));
}

fn prepare(db: ?*c.sqlite3, sql: CStr, stmt: *?*Stmt) c_int {
    return c.sqlite3_prepare_v2(db, sql, -1, stmt, null);
}

// ── Internal store structure ─────────────────────────────────────

/// One cbm edge of a Zova workspace store: Zova node keys, which are also the
/// ids of the nodes view, and an index into the store's edge type names.
const ZovaEdge = extern struct {
    source_key: i64,
    target_key: i64,
    type: c_int,
};

/// Prepared statements (lazily initialized, cached for lifetime).
const Stmts = extern struct {
    upsert_node: ?*Stmt,
    find_node_by_id: ?*Stmt,
    find_node_by_qn: ?*Stmt,
    find_node_by_qn_any: ?*Stmt, // QN lookup without project filter
    find_nodes_by_name: ?*Stmt,
    find_nodes_by_name_any: ?*Stmt, // name lookup without project filter
    find_nodes: ?*Stmt,
    find_nodes_by_label: ?*Stmt,
    find_nodes_by_file: ?*Stmt,
    count_nodes: ?*Stmt,
    delete_nodes_by_project: ?*Stmt,
    delete_nodes_by_file: ?*Stmt,
    delete_nodes_by_label: ?*Stmt,

    insert_edge: ?*Stmt,
    find_edges_by_source: ?*Stmt,
    find_edges_by_target: ?*Stmt,
    find_edges_by_source_type: ?*Stmt,
    find_edges_by_target_type: ?*Stmt,
    find_edges_by_type: ?*Stmt,
    count_edges: ?*Stmt,
    count_edges_by_type: ?*Stmt,
    delete_edges_by_project: ?*Stmt,
    delete_edges_by_type: ?*Stmt,

    upsert_project: ?*Stmt,
    get_project: ?*Stmt,
    list_projects: ?*Stmt,
    delete_project: ?*Stmt,

    upsert_file_hash: ?*Stmt,
    get_file_hashes: ?*Stmt,
    delete_file_hash: ?*Stmt,
    delete_file_hashes: ?*Stmt,
};

const Store = extern struct {
    db: ?*c.sqlite3,
    db_path: CStr, // heap-allocated, or NULL for :memory:
    /// Set only by cbm_store_open_backend; db is NULL then.
    backend: ?*const c.cbm_store_backend_t,
    backend_ctx: ?*anyopaque,
    errbuf: [c.CBM_SZ_512]u8,

    /// Read-only Zova sidecar session. One store is single-thread-owned.
    zova_vector_session: ?*c.cbm_zova_vector_session_t,
    zova_vector_session_path: [*c]u8,
    zova_vector_session_stat: c.struct_stat,
    zova_vector_session_stat_valid: bool,

    /// Read-only Zova graph session. Separate from vectors so either sidecar
    /// capability can be unavailable without disturbing the other.
    zova_graph_session: ?*c.cbm_zova_graph_session_t,
    zova_graph_workspace_root: [*c]u8,
    zova_graph_workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8,
    zova_graph_cached_numeric_id: i64,
    zova_graph_cached_stable_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX + 96]u8,
    zova_graph_cached_stable_id_valid: bool,
    zova_workspace_query: bool,
    zova_repository: ?*c.cbm_zova_repository_t,
    /// Legacy nodes view over the workspace; see storeLegacyScope.
    zova_legacy_project: [*c]u8,
    /// The workspace's edges, read once from the graph; see storeZovaEdges.
    zova_edges: [*c]ZovaEdge,
    zova_edge_count: c_int,
    zova_edges_loaded: bool,
    zova_edge_types: [*c][*c]u8,
    zova_edge_type_count: c_int,
    zova_native_topology_ops: c_int,

    stmts: Stmts,

    fn zovaEdges(s: *const Store) []ZovaEdge {
        return span(s.zova_edges, s.zova_edge_count);
    }

    fn zovaEdgeTypes(s: *const Store) [][*c]u8 {
        return span(s.zova_edge_types, s.zova_edge_type_count);
    }
};

// ── Helpers ──────────────────────────────────────────────────────

fn storeSetError(s: *Store, msg: []const u8) void {
    _ = interop.bprint(&s.errbuf, "{s}", .{msg});
}

fn storeSetErrorSqlite(s: *Store, prefix: []const u8) void {
    _ = interop.bprint(&s.errbuf, "{s}: {s}", .{ prefix, interop.sp(c.sqlite3_errmsg(s.db)) });
}

fn execSql(s_c: ?*Store, sql: CStr) c_int {
    const s = s_c orelse return ERR;
    if (s.db == null) return ERR;
    var err: [*c]u8 = null;
    const rc = c.sqlite3_exec(s.db, sql, null, null, &err);
    if (rc != c.SQLITE_OK) {
        _ = interop.bprint(&s.errbuf, "exec: {s}", .{if (err != null) std.mem.span(err) else "unknown"});
        c.sqlite3_free(err);
        return ERR;
    }
    return OK;
}

/// Safe string: returns "" if NULL.
fn safeStr(s: CStr) CStr {
    return if (s != null) s else "";
}

/// Safe properties: returns "{}" if NULL.
fn safeProps(s: CStr) CStr {
    return if (s != null and s[0] != 0) s else "{}";
}

fn streq(a: CStr, b: CStr) bool {
    return c.strcmp(a, b) == 0;
}

/// Prepare a statement (cached). If already prepared, reset+clear.
fn prepareCached(s: *Store, slot: *?*Stmt, sql: CStr) ?*Stmt {
    if (s.db == null) return null;
    if (slot.*) |stmt| {
        _ = c.sqlite3_reset(stmt);
        _ = c.sqlite3_clear_bindings(stmt);
        return stmt;
    }
    if (prepare(s.db, sql, slot) != c.SQLITE_OK) {
        storeSetErrorSqlite(s, "prepare");
        return null;
    }
    return slot.*;
}

/// Get ISO-8601 timestamp.
fn isoNow(buf: []u8) void {
    var t: c.time_t = c.time(null);
    var tm: c.struct_tm = undefined;
    _ = c.cbm_gmtime_r(&t, &tm);
    // strftime only fails if the buffer is too small — the 21-byte ISO
    // timestamp always fits in caller-provided buffers
    _ = c.strftime(buf.ptr, buf.len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/// A zeroed malloc'd array of n items; NULL on OOM.
fn callocSlice(comptime T: type, n: usize) ?[]T {
    const items: ?[*]T = interop.callocN(T, n);
    return (items orelse return null)[0..n];
}

/// A zeroed malloc'd T; NULL on OOM.
fn callocOne(comptime T: type) ?*T {
    return @ptrCast(@alignCast(c.calloc(1, @sizeOf(T))));
}

/// CBM_SNPRINTF_APPEND for a fixed buffer: appends formatted text at `len`
/// and returns the new length, clamped so the buffer always stays
/// NUL-terminated.
fn appendf(buf: []u8, len: usize, comptime fmt: []const u8, args: anytype) usize {
    if (len >= buf.len) return len;
    const n = interop.bprint(buf[len..], fmt, args);
    return @min(len + n, buf.len - 1);
}

// ── Schema ───────────────────────────────────────────────────────

fn initSchema(s: *Store) c_int {
    const ddl =
        "CREATE TABLE IF NOT EXISTS projects (" ++
        "  name TEXT PRIMARY KEY," ++
        "  indexed_at TEXT NOT NULL," ++
        "  root_path TEXT NOT NULL" ++
        ");" ++
        "CREATE TABLE IF NOT EXISTS file_hashes (" ++
        "  project TEXT NOT NULL REFERENCES projects(name) ON DELETE CASCADE," ++
        "  rel_path TEXT NOT NULL," ++
        "  sha256 TEXT NOT NULL," ++
        "  mtime_ns INTEGER NOT NULL DEFAULT 0," ++
        "  size INTEGER NOT NULL DEFAULT 0," ++
        "  PRIMARY KEY (project, rel_path)" ++
        ");" ++
        "CREATE TABLE IF NOT EXISTS nodes (" ++
        "  id INTEGER PRIMARY KEY AUTOINCREMENT," ++
        "  project TEXT NOT NULL REFERENCES projects(name) ON DELETE CASCADE," ++
        "  label TEXT NOT NULL," ++
        "  name TEXT NOT NULL," ++
        "  qualified_name TEXT NOT NULL," ++
        "  file_path TEXT DEFAULT ''," ++
        "  start_line INTEGER DEFAULT 0," ++
        "  end_line INTEGER DEFAULT 0," ++
        "  properties TEXT DEFAULT '{}'," ++
        "  UNIQUE(project, qualified_name)" ++
        ");" ++
        // local_name_gen (#768): IMPORTS edges carry one imported symbol's
        // local_name each, so uniqueness must discriminate on it — two named
        // imports from the same specifier are distinct edges. Non-IMPORTS
        // edges get '' (NOT NULL: NULLs never conflict in a UNIQUE index,
        // which would break their dedup entirely). Mirrors the graph-buffer
        // dedup key (makeEdgeKey) — keep both in sync.
        "CREATE TABLE IF NOT EXISTS edges (" ++
        "  id INTEGER PRIMARY KEY AUTOINCREMENT," ++
        "  project TEXT NOT NULL REFERENCES projects(name) ON DELETE CASCADE," ++
        "  source_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE," ++
        "  target_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE," ++
        "  type TEXT NOT NULL," ++
        "  properties TEXT DEFAULT '{}'," ++
        "  url_path_gen TEXT GENERATED ALWAYS AS (json_extract(properties,'$.url_path'))," ++
        "  local_name_gen TEXT GENERATED ALWAYS AS (CASE WHEN type='IMPORTS'" ++
        "    THEN coalesce(json_extract(properties,'$.local_name'),'') ELSE '' END)," ++
        "  UNIQUE(source_id, target_id, type, local_name_gen)" ++
        ");" ++
        "CREATE TABLE IF NOT EXISTS project_summaries (" ++
        "  project TEXT PRIMARY KEY," ++
        "  summary TEXT NOT NULL," ++
        "  source_hash TEXT NOT NULL," ++
        "  created_at TEXT NOT NULL," ++
        "  updated_at TEXT NOT NULL" ++
        ");";

    const rc = execSql(s, ddl);
    if (rc != OK) return rc;

    // Schema-compat probe (#768): DBs created before the local_name_gen
    // discriminator still enforce UNIQUE(source_id,target_id,type) and lack
    // the column — the widened upsert in cbm_store_insert_edge can neither
    // prepare nor let two named imports coexist against them. SQLite cannot
    // ALTER a table-level UNIQUE constraint in place, so fail the open:
    // callers already treat an unopenable DB as incompatible (a full index
    // deletes + rebuilds it, artifact import refuses and falls back to a
    // reindex). Read-only query opens skip initSchema and keep working.
    {
        var probe: ?*Stmt = null;
        if (prepare(s.db, "SELECT local_name_gen FROM edges LIMIT 0;", &probe) != c.SQLITE_OK) {
            interop.logWarn("store.schema", .{ "result", "incompatible", "missing", "edges.local_name_gen" });
            return ERR;
        }
        _ = c.sqlite3_finalize(probe);
    }

    // FTS5 contentless virtual table for BM25 full-text search.
    // Contentless (content='') means FTS5 stores only the inverted index,
    // not a copy of the source text — required for camelCase tokenization
    // because we feed it `cbm_camel_split(name)` at insert time but want
    // queries to match against the split tokens, not the original.
    // Fails silently if FTS5 is not compiled in (SQLITE_ENABLE_FTS5).
    {
        var fts_err: [*c]u8 = null;
        const fts_rc = c.sqlite3_exec(s.db, "CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5(" ++
            "  name, qualified_name, label, file_path," ++
            "  content=''," ++
            "  tokenize='unicode61 remove_diacritics 2'" ++
            ");", null, null, &fts_err);
        if (fts_rc != c.SQLITE_OK and fts_err != null) c.sqlite3_free(fts_err);
    }
    return OK;
}

fn createUserIndexes(s: ?*Store) c_int {
    const sql =
        "CREATE INDEX IF NOT EXISTS idx_nodes_label ON nodes(project, label);" ++
        "CREATE INDEX IF NOT EXISTS idx_nodes_name ON nodes(project, name);" ++
        "CREATE INDEX IF NOT EXISTS idx_nodes_file ON nodes(project, file_path);" ++
        "CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_id, type);" ++
        "CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_id, type);" ++
        "CREATE INDEX IF NOT EXISTS idx_edges_type ON edges(project, type);" ++
        "CREATE INDEX IF NOT EXISTS idx_edges_target_type ON edges(project, target_id, type);" ++
        "CREATE INDEX IF NOT EXISTS idx_edges_source_type ON edges(project, source_id, type);" ++
        "CREATE INDEX IF NOT EXISTS idx_edges_url_path ON edges(project, url_path_gen);";
    // NOTE: a partial expression index on json_extract(properties,'$.is_entry_point')
    // was tried for archEntryPoints and REVERTED: json_extract in an index WHERE
    // aborts CREATE INDEX (and thus store open) on any row whose properties JSON is
    // malformed — and pre-fix databases contain such rows (see
    // pipeline_def_props_valid_json_when_oversized). Revisit only with a
    // json_valid()-guarded expression once legacy DBs have aged out.
    return execSql(s, sql);
}

pub export fn cbm_store_resolve_mmap_size() i64 {
    const MMAP_DEFAULT = 67108864; // default 64 MB
    var buf: [ST_BUF_64]u8 = undefined;
    if (c.cbm_safe_getenv("CBM_SQLITE_MMAP_SIZE", &buf, buf.len, null) == null) return MMAP_DEFAULT;
    var end: [*c]u8 = null;
    const parsed = c.strtoll(&buf, &end, 10);
    if (end == @as([*c]u8, &buf) or end[0] != 0) {
        // Malformed — fall back to default rather than fail the store open.
        return MMAP_DEFAULT;
    }
    if (parsed < 0) return 0;
    return parsed;
}

fn execMmapSize(s: *Store) c_int {
    var mmap_sql: [ST_BUF_64]u8 = undefined;
    _ = interop.bprint(&mmap_sql, "PRAGMA mmap_size = {d};", .{cbm_store_resolve_mmap_size()});
    return execSql(s, &mmap_sql);
}

/// Configure connection pragmas.
///   in_memory  — :memory: DB (synchronous OFF, no journal file).
///   read_only  — query-only connection opened SQLITE_OPEN_READONLY. Runs
///                ONLY non-writing pragmas (foreign_keys, temp_store,
///                busy_timeout, mmap_size) and SKIPS journal_mode=WAL,
///                wal_checkpoint and synchronous — those WRITE to the DB/WAL
///                and would (a) mutate the DB on every read query and (b)
///                fail on a read-only DB file / filesystem.
/// in_memory and read_only are mutually exclusive.
fn configurePragmas(s: *Store, in_memory: bool, read_only: bool) c_int {
    var rc = execSql(s, "PRAGMA foreign_keys = ON;");
    if (rc != OK) return rc;
    rc = execSql(s, "PRAGMA temp_store = MEMORY;");
    if (rc != OK) return rc;

    if (in_memory) return execSql(s, "PRAGMA synchronous = OFF;");

    rc = execSql(s, "PRAGMA busy_timeout = 10000;");
    if (rc != OK) return rc;
    // A read-only connection runs non-writing pragmas only — see the
    // function comment.
    if (!read_only) {
        rc = execSql(s, "PRAGMA journal_mode = WAL;");
        if (rc != OK) return rc;
        // Recover stale WAL from previous crash (best-effort).
        // PASSIVE never blocks readers and never ftruncates.
        // May fail with SQLITE_BUSY if another process holds a lock.
        _ = c.sqlite3_exec(s.db, "PRAGMA wal_checkpoint(PASSIVE)", null, null, null);
        rc = execSql(s, "PRAGMA synchronous = NORMAL;");
        if (rc != OK) return rc;
    }
    return execMmapSize(s);
}

// ── camelCase splitter for FTS5 indexing ─────────────────────────

// Emits the original identifier plus a space-separated split version, so FTS5's
// whitespace tokenizer produces both `updateCloudClient` (exact match) and the
// word tokens `update`, `cloud`, `client`.  Rules:
//   1. insert space before an uppercase letter preceded by a lowercase letter
//      ("updateCloud" → "update Cloud")
//   2. insert space before an uppercase letter preceded by another uppercase
//      but followed by a lowercase letter ("XMLParser" → "XML Parser", not
//      "X M L Parser")
// snake_case is already split by FTS5's unicode61 tokenizer on `_`.
const CAMEL_SPLIT_BUF = 2048;
const CAMEL_BUF_GUARD = 2; // reserve room for inserted space + NUL terminator.

/// Denominator epsilon guard for double-precision cosine.
const CBM_STORE_DENOM_EPS_D = 1e-10;
const CBM_STORE_DENOM_EPS_F: f32 = 1e-10;
const CBM_STORE_INT8_MAX: f32 = 127.0;

const SqlValues = [*c]?*c.sqlite3_value;

/// True if we should insert a space BEFORE input[i] to split a camelCase word
/// boundary (lowercase→uppercase or uppercase-run→uppercase-then-lowercase).
fn camelShouldSplit(input: [:0]const u8, i: usize) bool {
    if (i == 0) return false;
    const curr = input[i];
    const prev = input[i - 1];
    const next = input[i + 1];
    if (!std.ascii.isUpper(curr)) return false;
    return std.ascii.isLower(prev) or (std.ascii.isUpper(prev) and std.ascii.isLower(next));
}

fn sqliteCamelSplit(ctx: ?*c.sqlite3_context, argc: c_int, argv: SqlValues) callconv(.c) void {
    _ = argc;
    const input_c: CStr = c.sqlite3_value_text(argv[0]);
    if (input_c == null or input_c[0] == 0) {
        c.sqlite3_result_text(ctx, safeStr(input_c), -1, transient());
        return;
    }
    const input = std.mem.span(input_c);
    var buf: [CAMEL_SPLIT_BUF]u8 = undefined;
    if (input.len + 1 >= buf.len) {
        // Input too long — fall back to the original string unmodified.
        c.sqlite3_result_text(ctx, input_c, -1, transient());
        return;
    }
    @memcpy(buf[0..input.len], input);
    buf[input.len] = ' ';
    var len = input.len + 1;
    for (input, 0..) |ch, i| {
        if (len >= buf.len - CAMEL_BUF_GUARD) break;
        if (camelShouldSplit(input, i)) {
            buf[len] = ' ';
            len += 1;
        }
        buf[len] = ch;
        len += 1;
    }
    c.sqlite3_result_text(ctx, &buf, @intCast(len), transient());
}

// ── REGEXP function for SQLite ───────────────────────────────────

/// Destructor passed to sqlite3_set_auxdata — frees the cached compiled regex.
fn regexFreeCb(p: ?*anyopaque) callconv(.c) void {
    const re: *c.cbm_regex_t = @ptrCast(@alignCast(p orelse return));
    c.cbm_regfree(re);
    c.free(re);
}

/// Cache the compiled regex on argument slot 0 for the lifetime of the statement.
/// sqlite3_get_auxdata returns the cached pointer on subsequent rows (same parameter
/// value), so cbm_regcomp is called exactly once per statement instead of once per row.
fn regexpWithFlags(ctx: ?*c.sqlite3_context, argv: SqlValues, flags: c_int) void {
    const pattern: CStr = c.sqlite3_value_text(argv[0]);
    const text: CStr = c.sqlite3_value_text(argv[1]);
    if (pattern == null or text == null) {
        c.sqlite3_result_int(ctx, 0);
        return;
    }

    var re: ?*c.cbm_regex_t = @ptrCast(@alignCast(c.sqlite3_get_auxdata(ctx, 0)));
    if (re == null) {
        re = @ptrCast(@alignCast(c.malloc(@sizeOf(c.cbm_regex_t))));
        if (re == null) {
            c.sqlite3_result_error_nomem(ctx);
            return;
        }
        if (c.cbm_regcomp(re, pattern, flags) != 0) {
            c.free(re);
            c.sqlite3_result_error(ctx, "invalid regex", -1);
            return;
        }
        c.sqlite3_set_auxdata(ctx, 0, re, regexFreeCb);
    }

    c.sqlite3_result_int(ctx, @intFromBool(c.cbm_regexec(re, text, 0, null, 0) == 0));
}

fn sqliteRegexp(ctx: ?*c.sqlite3_context, argc: c_int, argv: SqlValues) callconv(.c) void {
    _ = argc;
    regexpWithFlags(ctx, argv, c.CBM_REG_EXTENDED | c.CBM_REG_NOSUB);
}

/// Case-insensitive REGEXP variant — same auxdata caching strategy.
fn sqliteIregexp(ctx: ?*c.sqlite3_context, argc: c_int, argv: SqlValues) callconv(.c) void {
    _ = argc;
    regexpWithFlags(ctx, argv, c.CBM_REG_EXTENDED | c.CBM_REG_NOSUB | c.CBM_REG_ICASE);
}

/// Cosine similarity between two int8 BLOB vectors.
/// Returns float in [-1, 1].  Used for vector search at query time.
fn sqliteCosineI8(ctx: ?*c.sqlite3_context, argc: c_int, argv: SqlValues) callconv(.c) void {
    _ = argc;
    if (c.sqlite3_value_type(argv[0]) != c.SQLITE_BLOB or c.sqlite3_value_type(argv[1]) != c.SQLITE_BLOB) {
        c.sqlite3_result_double(ctx, 0.0);
        return;
    }
    const len_a = c.sqlite3_value_bytes(argv[0]);
    const len_b = c.sqlite3_value_bytes(argv[1]);
    if (len_a != len_b or len_a == 0) {
        c.sqlite3_result_double(ctx, 0.0);
        return;
    }
    const len: usize = @intCast(len_a);
    const a: [*]const i8 = @ptrCast(c.sqlite3_value_blob(argv[0]).?);
    const b: [*]const i8 = @ptrCast(c.sqlite3_value_blob(argv[1]).?);
    var dot: i32 = 0;
    var mag_a: i32 = 0;
    var mag_b: i32 = 0;
    for (a[0..len], b[0..len]) |x, y| {
        dot +%= @as(i32, x) * @as(i32, y);
        mag_a +%= @as(i32, x) * @as(i32, x);
        mag_b +%= @as(i32, y) * @as(i32, y);
    }
    const denom = @sqrt(@as(f64, @floatFromInt(mag_a))) * @sqrt(@as(f64, @floatFromInt(mag_b)));
    c.sqlite3_result_double(ctx, if (denom > CBM_STORE_DENOM_EPS_D) @as(f64, @floatFromInt(dot)) / denom else 0.0);
}

// ── Lifecycle ────────────────────────────────────────────────────

/// SQLite authorizer: deny dangerous operations that could be exploited via
/// SQL injection through the Cypher→SQL translation layer.
fn storeAuthorizer(user_data: ?*anyopaque, action: c_int, p3: [*c]const u8, p4: [*c]const u8, p5: [*c]const u8, p6: [*c]const u8) callconv(.c) c_int {
    _ = .{ user_data, p3, p4, p5, p6 };
    return switch (action) {
        // ATTACH DATABASE could create/read arbitrary files
        c.SQLITE_ATTACH, c.SQLITE_DETACH => c.SQLITE_DENY,
        else => c.SQLITE_OK,
    };
}

/// Block ATTACH/DETACH and register the SQL functions the store's queries use.
fn registerFunctions(db: ?*c.sqlite3) void {
    // Security: block ATTACH/DETACH to prevent file creation via SQL injection.
    // The authorizer runs inside SQLite's query planner — no string-level bypass.
    _ = c.sqlite3_set_authorizer(db, storeAuthorizer, null);

    const flags = c.SQLITE_UTF8 | c.SQLITE_DETERMINISTIC;
    // REGEXP function (SQLite doesn't have one built-in)
    _ = c.sqlite3_create_function(db, "regexp", 2, flags, null, sqliteRegexp, null, null);
    // Case-insensitive variant for search with case_sensitive=false
    _ = c.sqlite3_create_function(db, "iregexp", 2, flags, null, sqliteIregexp, null, null);
    // Int8 cosine similarity for vector search
    _ = c.sqlite3_create_function(db, "cbm_cosine_i8", 2, flags, null, sqliteCosineI8, null, null);
    // camelCase splitter for FTS5 BM25 indexing
    _ = c.sqlite3_create_function(db, "cbm_camel_split", 1, flags, null, sqliteCamelSplit, null, null);
}

fn storeOpenInternal(path: Str, in_memory: bool) ?*Store {
    const s = callocOne(Store) orelse return null;

    var flags: c_int = c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_CREATE;
    if (in_memory) flags |= c.SQLITE_OPEN_MEMORY;

    if (c.sqlite3_open_v2(path, &s.db, flags, null) != c.SQLITE_OK) {
        c.free(s);
        return null;
    }

    if (!in_memory) s.db_path = interop.heapStrdup(path);

    registerFunctions(s.db);

    if (configurePragmas(s, in_memory, false) != OK or initSchema(s) != OK or createUserIndexes(s) != OK) {
        _ = c.sqlite3_close(s.db);
        interop.strFree(&s.db_path);
        c.free(s);
        return null;
    }

    return s;
}

pub export fn cbm_store_open_memory() ?*Store {
    return storeOpenInternal(":memory:", true);
}

fn storeCleanupPending(db_path: Str) bool {
    var marker: [2048]u8 = undefined;
    const n = interop.bprint(&marker, "{s}.cbm-cleanup-pending", .{std.mem.span(db_path)});
    if (n == 0 or n >= marker.len) return false;
    var marker_stat: c.struct_stat = undefined;
    return c.cbm_lstat(&marker, &marker_stat) == 0;
}

pub export fn cbm_store_db_path(s: ?*const Store) ?Str {
    return if (s) |store| store.db_path else null;
}

/// Build a SQLite "file:" URI with immutable=1 from a filesystem path.
/// immutable=1 bypasses WAL and locking and reads the main DB file directly —
/// used only as a fallback for read-only filesystems where the wal-index
/// (-shm) cannot be created. URI-special characters are percent-encoded.
/// Windows backslashes are normalized to '/', and a leading '/' is inserted
/// for drive-letter paths so the drive is not parsed as a URI authority.
/// Returns false if the output buffer is too small.
fn buildImmutableUri(path: []const u8, out: []u8) bool {
    const prefix = "file://";
    const suffix = "?immutable=1";
    const hex = "0123456789ABCDEF";
    if (prefix.len + 1 > out.len) return false;
    @memcpy(out[0..prefix.len], prefix);
    var pos: usize = prefix.len;

    // Ensure the path component begins with '/' (POSIX absolute paths already
    // do; Windows "C:\..." gets a leading '/' -> "/C:/...").
    if (path.len == 0 or path[0] != '/') {
        if (pos + 1 >= out.len) return false;
        out[pos] = '/';
        pos += 1;
    }

    for (path) |raw| {
        const ch: u8 = if (raw == '\\') '/' else raw; // normalize Windows separators
        const safe = std.ascii.isAlphanumeric(ch) or
            ch == '/' or ch == '.' or ch == '-' or ch == '_' or ch == '~' or ch == ':';
        if (safe) {
            if (pos + 1 >= out.len) return false;
            out[pos] = ch;
            pos += 1;
        } else {
            if (pos + 3 >= out.len) return false;
            out[pos] = '%';
            out[pos + 1] = hex[ch >> 4];
            out[pos + 2] = hex[ch & 0xF];
            pos += 3;
        }
    }

    if (pos + suffix.len + 1 > out.len) return false;
    @memcpy(out[pos..][0..suffix.len], suffix);
    out[pos + suffix.len] = 0;
    return true;
}

pub export fn cbm_store_open_path_query(db_path_c: ?Str) ?*Store {
    const db_path = db_path_c orelse return null;
    if (storeCleanupPending(db_path)) return null;

    const s = callocOne(Store) orelse return null;

    // Query tools open the project DB READ-ONLY: a read query must never
    // mutate the DB (the previous READWRITE open + WAL write-pragmas did),
    // and must work on a read-only DB file / filesystem.
    //
    // Try a plain READONLY open first — on a normal writable filesystem this
    // reads WAL frames correctly via the -shm wal-index. SQLite opens lazily,
    // so a read-only-filesystem failure (cannot create -shm for a WAL-mode
    // DB) surfaces on first access, not at open time; we probe with a trivial
    // read to force it. If the probe fails, retry once with an immutable URI
    // that bypasses WAL and reads the main DB file directly.
    //
    // No SQLITE_OPEN_CREATE on either path — a missing DB must return NULL
    // (no ghost .db for unknown/unindexed projects).
    var rc = c.sqlite3_open_v2(db_path, &s.db, c.SQLITE_OPEN_READONLY, null);
    if (rc == c.SQLITE_OK) {
        // Wait out a concurrent writer before probing: a busy probe is not a
        // read-only filesystem, and the immutable fallback below ignores the
        // WAL, so it would serve a stale snapshot without recent commits.
        _ = c.sqlite3_busy_timeout(s.db, ST_BUSY_TIMEOUT_MS);
        // Force first DB access so a read-only-FS WAL failure surfaces now.
        if (c.sqlite3_exec(s.db, "SELECT 1 FROM sqlite_master LIMIT 1;", null, null, null) != c.SQLITE_OK) {
            const busy = (c.sqlite3_errcode(s.db) & 0xFF) == c.SQLITE_BUSY;
            _ = c.sqlite3_close(s.db);
            s.db = null;
            if (busy) {
                c.free(s);
                return null;
            }
            rc = c.SQLITE_CANTOPEN; // trigger immutable fallback
        }
    }
    if (rc != c.SQLITE_OK) {
        _ = c.sqlite3_close(s.db); // no-op if already NULL
        s.db = null;
        // A genuinely missing DB must return NULL without creating anything —
        // only retry with the immutable URI when the file exists but could not
        // be opened (the read-only-filesystem case). This also keeps the
        // common "project not found" path to a single open attempt.
        var uri: [ST_QUERY_URI_MAX]u8 = undefined;
        if (!c.cbm_file_exists(db_path) or !buildImmutableUri(std.mem.span(db_path), &uri)) {
            c.free(s);
            return null;
        }
        rc = c.sqlite3_open_v2(&uri, &s.db, c.SQLITE_OPEN_READONLY | c.SQLITE_OPEN_URI, null);
        if (rc != c.SQLITE_OK) {
            // sqlite3_open_v2 allocates a handle even on failure — must close it.
            _ = c.sqlite3_close(s.db);
            c.free(s);
            return null;
        }
    }

    s.db_path = interop.heapStrdup(db_path);

    registerFunctions(s.db);

    if (configurePragmas(s, false, true) != OK) {
        _ = c.sqlite3_close(s.db);
        interop.strFree(&s.db_path);
        c.free(s);
        return null;
    }

    return s;
}

fn storeStableNumericId(value: CStr) i64 {
    var hash: u64 = 1469598103934665603;
    if (value != null) {
        for (std.mem.span(value)) |byte| hash = (hash ^ byte) *% 1099511628211;
    }
    return @intCast(hash & std.math.maxInt(i64));
}

pub export fn cbm_store_open_zova_workspace_query(db_path: ?Str, workspace_id: ?Str) ?*Store {
    if (db_path == null or c.cbm_zova_workspace_id_validate(workspace_id) != 0) return null;
    const s = cbm_store_open_path_query(db_path) orelse return null;
    // A user-local database can hold several workspaces, and graph hydration
    // precedes vector sampling on the same handle. A bounded 256 MiB ceiling
    // keeps both working sets resident on large repositories without changing
    // compatibility project-store behavior. SQLite allocates pages lazily.
    if (c.sqlite3_exec(s.db, "PRAGMA cache_size=-262144", null, null, null) != c.SQLITE_OK) {
        cbm_store_close(s);
        return null;
    }
    var project_stmt: ?*Stmt = null;
    var project = std.mem.zeroes([256]u8);
    if (prepare(s.db, "SELECT p.project FROM cbm_projects_v1 p JOIN cbm_workspace_registry r " ++
        "USING(workspace_key) WHERE r.workspace_id=?1", &project_stmt) != c.SQLITE_OK or
        c.sqlite3_bind_text(project_stmt, 1, workspace_id, -1, null) != c.SQLITE_OK or
        c.sqlite3_step(project_stmt) != c.SQLITE_ROW)
    {
        if (project_stmt != null) _ = c.sqlite3_finalize(project_stmt);
        cbm_store_close(s);
        return null;
    }
    const project_value = columnText(project_stmt, 0);
    if (project_value != null) _ = interop.bprint(&project, "{s}", .{std.mem.span(project_value)});
    _ = c.sqlite3_finalize(project_stmt);
    if (project[0] == 0) {
        cbm_store_close(s);
        return null;
    }
    s.zova_repository = c.cbm_zova_repository_open(db_path, &project);
    if (s.zova_repository == null or !streq(c.cbm_zova_repository_workspace_id(s.zova_repository), workspace_id)) {
        cbm_store_close(s);
        return null;
    }
    s.zova_graph_session = c.cbm_zova_graph_session_open(db_path);
    if (s.zova_graph_session == null) {
        cbm_store_close(s);
        return null;
    }
    _ = interop.bprint(&s.zova_graph_workspace_id, "{s}", .{std.mem.span(workspace_id.?)});
    s.zova_workspace_query = true;
    return s;
}

pub export fn cbm_store_zova_native_topology_ops(s: ?*const Store) c_int {
    return if (s) |store| store.zova_native_topology_ops else 0;
}

// ── Integrity check ──────────────────────────────────────────────

fn finalizeStmt(slot: *?*Stmt) void {
    if (slot.*) |stmt| {
        _ = c.sqlite3_finalize(stmt);
        slot.* = null;
    }
}

pub export fn cbm_store_open_backend(backend: ?*const c.cbm_store_backend_t, ctx: ?*anyopaque) ?*Store {
    if (backend == null) return null;
    const s = callocOne(Store) orelse return null;
    s.backend = backend;
    s.backend_ctx = ctx;
    return s;
}

pub export fn cbm_store_close(s_c: ?*Store) void {
    const s = s_c orelse return;
    if (s.backend != null) {
        c.free(s);
        return;
    }

    storeCloseZovaVectorSession(s);
    storeCloseZovaGraphSession(s);
    storeClearZovaGraphWorkspace(s);
    c.cbm_zova_repository_close(s.zova_repository);
    s.zova_repository = null;
    c.free(s.zova_legacy_project);
    s.zova_legacy_project = null;
    c.free(@ptrCast(s.zova_edges));
    for (s.zovaEdgeTypes()) |edge_type| c.free(edge_type);
    c.free(@ptrCast(s.zova_edge_types));

    // Checkpoint WAL before close to prevent orphan WAL accumulation.
    // Best-effort — silently skips if concurrent reader holds a lock.
    if (s.db != null and s.db_path != null) {
        _ = c.sqlite3_wal_checkpoint_v2(s.db, null, c.SQLITE_CHECKPOINT_PASSIVE, null, null);
    }

    // Finalize all cached statements
    inline for (@typeInfo(Stmts).@"struct".field_names) |name| finalizeStmt(&@field(s.stmts, name));

    // Use sqlite3_close_v2 — auto-deallocates when last statement finalizes.
    // Prevents ASan false-positive leaks from sqlite3 internal state.
    _ = c.sqlite3_close_v2(s.db);
    interop.strFree(&s.db_path);
    c.free(s);
}

pub export fn cbm_store_get_db(s: ?*Store) ?*c.sqlite3 {
    return if (s) |store| store.db else null;
}

pub export fn cbm_store_exec(s: ?*Store, sql: ?Str) c_int {
    return execSql(s, sql);
}

pub export fn cbm_store_error(s: ?*Store) ?Str {
    return if (s) |store| @ptrCast(&store.errbuf) else "null store";
}

// ── Transaction ──────────────────────────────────────────────────

pub export fn cbm_store_begin(s: ?*Store) c_int {
    return execSql(s, "BEGIN IMMEDIATE;");
}

pub export fn cbm_store_commit(s: ?*Store) c_int {
    return execSql(s, "COMMIT;");
}

pub export fn cbm_store_rollback(s: ?*Store) c_int {
    return execSql(s, "ROLLBACK;");
}

// ── Bulk write ───────────────────────────────────────────────────

pub export fn cbm_store_begin_bulk(s: ?*Store) c_int {
    // Stay in WAL mode throughout. Switching to MEMORY journal mode would
    // make the database unrecoverable if the process crashes mid-write,
    // because the in-memory rollback journal is lost on crash.
    // WAL mode is crash-safe: uncommitted WAL entries are simply discarded
    // on the next open. Performance is preserved via synchronous=OFF and a
    // larger cache, which are safe with WAL.
    const rc = execSql(s, "PRAGMA synchronous = OFF;");
    if (rc != OK) return rc;
    return execSql(s, "PRAGMA cache_size = -65536;"); // 64 MB
}

pub export fn cbm_store_end_bulk(s: ?*Store) c_int {
    const rc = execSql(s, "PRAGMA synchronous = NORMAL;");
    if (rc != OK) return rc;
    return execSql(s, "PRAGMA cache_size = -2000;"); // default ~2 MB
}

pub export fn cbm_store_drop_indexes(s: ?*Store) c_int {
    return execSql(s, "DROP INDEX IF EXISTS idx_nodes_label;" ++
        "DROP INDEX IF EXISTS idx_nodes_name;" ++
        "DROP INDEX IF EXISTS idx_nodes_file;" ++
        "DROP INDEX IF EXISTS idx_edges_source;" ++
        "DROP INDEX IF EXISTS idx_edges_target;" ++
        "DROP INDEX IF EXISTS idx_edges_type;" ++
        "DROP INDEX IF EXISTS idx_edges_target_type;" ++
        "DROP INDEX IF EXISTS idx_edges_source_type;");
}

pub export fn cbm_store_create_indexes(s: ?*Store) c_int {
    return createUserIndexes(s);
}

// ── Result arrays ────────────────────────────────────────────────

/// The C store's growable result array: malloc(cap), then `cap *= ST_GROWTH`
/// through safe_realloc when full. `items` is what the caller receives.
fn Rows(comptime T: type) type {
    return struct {
        items: [*c]T,
        len: c_int = 0,
        cap: c_int,

        const Self = @This();

        /// An array that stays NULL until the first add, which allocates
        /// ST_INIT_CAP_16 slots.
        const lazy: Self = .{ .items = null, .cap = 0 };

        fn init(cap: c_int) Self {
            return .{ .items = interop.mallocN(T, @intCast(cap)), .cap = cap };
        }

        /// The next free slot, uninitialized.
        fn add(self: *Self) *T {
            if (self.len >= self.cap) {
                self.cap = if (self.cap != 0) self.cap * ST_GROWTH else ST_INIT_CAP_16;
                self.items = interop.safeReallocN(T, self.items, @intCast(self.cap));
            }
            self.len += 1;
            return self.at(self.len - 1);
        }

        fn at(self: *const Self, index: c_int) *T {
            return &@as([*]T, self.items)[@intCast(index)];
        }

        fn slice(self: *const Self) []T {
            if (self.len == 0) return &.{};
            return @as([*]T, self.items)[0..@intCast(self.len)];
        }
    };
}

fn Span(comptime P: type) type {
    const info = @typeInfo(P).pointer;
    return if (info.attrs.@"const") []const info.child else []info.child;
}

/// The items of a (C pointer, count) array; empty for NULL or count <= 0.
fn span(items: anytype, count: c_int) Span(@TypeOf(items)) {
    if (items == null or count <= 0) return &.{};
    return items[0..@intCast(count)];
}

/// Runs a cached statement that binds text parameters and returns no rows.
/// `what` names the operation in the store's error text.
fn execCached(s_c: ?*Store, comptime slot: []const u8, sql: CStr, binds: []const CStr, comptime what: []const u8) c_int {
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &@field(s.stmts, slot), sql) orelse return ERR;
    for (binds, 1..) |value, col| _ = bindText(stmt, @intCast(col), value);
    if (c.sqlite3_step(stmt) != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, what);
        return ERR;
    }
    return OK;
}

/// (a > b) - (a < b).
fn order(a: anytype, b: @TypeOf(a)) c_int {
    return @as(c_int, @intFromBool(a > b)) - @intFromBool(a < b);
}

/// True when queries are answered by the Zova workspace repository.
fn usesRepository(s: *const Store) bool {
    return s.zova_workspace_query and s.zova_repository != null;
}

// ── Project CRUD ─────────────────────────────────────────────────

pub export fn cbm_store_upsert_project(s_c: ?*Store, name: CStr, root_path: CStr) c_int {
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &s.stmts.upsert_project, "INSERT INTO projects (name, indexed_at, root_path) VALUES (?1, ?2, ?3) " ++
        "ON CONFLICT(name) DO UPDATE SET indexed_at=?2, root_path=?3;") orelse return ERR;

    var ts: [c.CBM_SZ_64]u8 = undefined;
    isoNow(&ts);

    _ = bindText(stmt, 1, name);
    _ = bindText(stmt, 2, &ts);
    _ = bindText(stmt, 3, root_path);

    if (c.sqlite3_step(stmt) != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "upsert_project");
        return ERR;
    }
    return OK;
}

fn scanProject(stmt: ?*Stmt, p: *c.cbm_project_t) void {
    p.name = columnDup(stmt, 0);
    p.indexed_at = columnDup(stmt, 1);
    p.root_path = columnDup(stmt, 2);
}

pub export fn cbm_store_get_project(s_c: ?*Store, name: CStr, out: *c.cbm_project_t) c_int {
    const s = s_c orelse return ERR;
    if (usesRepository(s)) {
        return c.cbm_zova_repository_get_project(s.zova_repository, &s.zova_graph_workspace_id, out);
    }
    const stmt = prepareCached(s, &s.stmts.get_project, "SELECT name, indexed_at, root_path FROM projects WHERE name = ?1;") orelse return ERR;

    _ = bindText(stmt, 1, name);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        scanProject(stmt, out);
        return OK;
    }
    return NOT_FOUND;
}

pub export fn cbm_store_list_projects(s_c: ?*Store, out: *[*c]c.cbm_project_t, count: *c_int) c_int {
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &s.stmts.list_projects, "SELECT name, indexed_at, root_path FROM projects ORDER BY name;") orelse return ERR;

    // Collect into dynamic array
    var rows = Rows(c.cbm_project_t).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) scanProject(stmt, rows.add());
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        cbm_store_free_projects(rows.items, rows.len);
        out.* = null;
        count.* = 0;
        return ERR;
    }

    out.* = rows.items;
    count.* = rows.len;
    return OK;
}

pub export fn cbm_store_delete_project(s: ?*Store, name: CStr) c_int {
    return execCached(s, "delete_project", "DELETE FROM projects WHERE name = ?1;", &.{name}, "delete_project");
}

// ── Node CRUD ────────────────────────────────────────────────────

const node_columns = "SELECT id, project, label, name, qualified_name, file_path, " ++
    "start_line, end_line, properties FROM nodes ";

pub export fn cbm_store_upsert_node(s_c: ?*Store, n: *const c.cbm_node_t) i64 {
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &s.stmts.upsert_node, "INSERT INTO nodes (project, label, name, qualified_name, file_path, " ++
        "start_line, end_line, properties) " ++
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) " ++
        "ON CONFLICT(project, qualified_name) DO UPDATE SET " ++
        "label=?2, name=?3, file_path=?5, start_line=?6, end_line=?7, properties=?8 " ++
        "RETURNING id;") orelse return ERR;

    _ = bindText(stmt, 1, safeStr(n.project));
    _ = bindText(stmt, 2, safeStr(n.label));
    _ = bindText(stmt, 3, safeStr(n.name));
    _ = bindText(stmt, 4, safeStr(n.qualified_name));
    _ = bindText(stmt, 5, safeStr(n.file_path));
    _ = c.sqlite3_bind_int(stmt, 6, n.start_line);
    _ = c.sqlite3_bind_int(stmt, 7, n.end_line);
    _ = bindText(stmt, 8, safeProps(n.properties_json));

    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const id = c.sqlite3_column_int64(stmt, 0);
        _ = c.sqlite3_reset(stmt); // unblock COMMIT — RETURNING leaves stmt active
        return id;
    }
    _ = c.sqlite3_reset(stmt);
    storeSetErrorSqlite(s, "upsert_node");
    return ERR;
}

/// Scan a node from current row of stmt. Heap-allocates strings.
fn scanNode(stmt: ?*Stmt, n: *c.cbm_node_t) void {
    n.id = c.sqlite3_column_int64(stmt, 0);
    n.project = columnDup(stmt, 1);
    n.label = columnDup(stmt, 2);
    n.name = columnDup(stmt, 3);
    n.qualified_name = columnDup(stmt, 4);
    n.file_path = columnDup(stmt, 5);
    n.start_line = c.sqlite3_column_int(stmt, 6);
    n.end_line = c.sqlite3_column_int(stmt, 7);
    n.properties_json = columnDup(stmt, 8);
}

/// Steps stmt to completion, scanning every row into a malloc'd node array.
fn collectNodes(s: *Store, stmt: ?*Stmt, cap: c_int, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    var rows = Rows(c.cbm_node_t).init(cap);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) scanNode(stmt, rows.add());
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        cbm_store_free_nodes(rows.items, rows.len);
        out.* = null;
        count.* = 0;
        return ERR;
    }
    out.* = rows.items;
    count.* = rows.len;
    return OK;
}

pub export fn cbm_store_find_node_by_id(s_c: ?*Store, id: i64, out: *c.cbm_node_t) c_int {
    const s = s_c orelse return ERR;
    if (s.backend) |backend| return backend.find_node_by_id.?(s.backend_ctx, id, out);
    if (usesRepository(s)) {
        return c.cbm_zova_repository_find_node_by_numeric_id(s.zova_repository, &s.zova_graph_workspace_id, id, out);
    }
    const stmt = prepareCached(s, &s.stmts.find_node_by_id, node_columns ++ "WHERE id = ?1;") orelse return ERR;

    _ = c.sqlite3_bind_int64(stmt, 1, id);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        scanNode(stmt, out);
        return OK;
    }
    return NOT_FOUND;
}

pub export fn cbm_store_find_node_by_qn(s_c: ?*Store, project: CStr, qn: CStr, out: *c.cbm_node_t) c_int {
    const s = s_c orelse return ERR;
    if (s.db == null) return ERR;
    if (usesRepository(s)) {
        return c.cbm_zova_repository_find_node_by_qn(s.zova_repository, &s.zova_graph_workspace_id, qn, out);
    }
    const stmt = prepareCached(s, &s.stmts.find_node_by_qn, node_columns ++
        "WHERE project = ?1 AND qualified_name = ?2;") orelse return ERR;

    _ = bindText(stmt, 1, project);
    _ = bindText(stmt, 2, qn);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        scanNode(stmt, out);
        return OK;
    }
    return NOT_FOUND;
}

pub export fn cbm_store_find_node_by_qn_any(s_c: ?*Store, qn: CStr, out: *c.cbm_node_t) c_int {
    const s = s_c orelse return ERR;
    if (s.db == null) return ERR;
    if (usesRepository(s)) {
        return c.cbm_zova_repository_find_node_by_qn(s.zova_repository, &s.zova_graph_workspace_id, qn, out);
    }
    const stmt = prepareCached(s, &s.stmts.find_node_by_qn_any, node_columns ++
        "WHERE qualified_name = ?1 LIMIT 1;") orelse return ERR;

    _ = bindText(stmt, 1, qn);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        scanNode(stmt, out);
        return OK;
    }
    return NOT_FOUND;
}

pub export fn cbm_store_find_nodes_by_name_any(s_c: ?*Store, name: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    out.* = null;
    count.* = 0;
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &s.stmts.find_nodes_by_name_any, node_columns ++ "WHERE name = ?1;") orelse return ERR;

    _ = bindText(stmt, 1, name);
    return collectNodes(s, stmt, ST_INIT_CAP_16, out, count);
}

pub export fn cbm_store_find_node_ids_by_qns(s: ?*Store, project: CStr, qns_c: ?[*]CStr, qn_count: c_int, out_ids_c: ?[*]i64) c_int {
    const qns = qns_c orelse return 0;
    const out_ids = out_ids_c orelse return 0;
    if (s == null or project == null or qn_count <= 0) return 0;
    const n: usize = @intCast(qn_count);

    // Zero out results
    @memset(out_ids[0..n], 0);

    var found: c_int = 0;
    var node = std.mem.zeroes(c.cbm_node_t);
    for (qns[0..n], out_ids[0..n]) |qn, *out_id| {
        if (qn == null) continue;
        if (cbm_store_find_node_by_qn(s, project, qn, &node) == OK) {
            out_id.* = node.id;
            found += 1;
            cbm_node_free_fields(&node);
            node = std.mem.zeroes(c.cbm_node_t);
        }
    }
    return found;
}

/// Generic: find multiple nodes by a single-column filter.
fn findNodesGeneric(s_c: ?*Store, comptime slot: []const u8, sql: CStr, project: CStr, val: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    const s = s_c orelse {
        out.* = null;
        count.* = 0;
        return ERR;
    };
    if (s.db == null) {
        out.* = null;
        count.* = 0;
        return ERR;
    }
    const stmt = prepareCached(s, &@field(s.stmts, slot), sql) orelse return ERR;

    _ = bindText(stmt, 1, project);
    if (val != null) _ = bindText(stmt, 2, val);

    return collectNodes(s, stmt, ST_INIT_CAP_16, out, count);
}

fn nodePublicOrderCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left: *const c.cbm_node_t = @ptrCast(@alignCast(left_ptr));
    const right: *const c.cbm_node_t = @ptrCast(@alignCast(right_ptr));
    var cmp = c.strcmp(safeStr(left.qualified_name), safeStr(right.qualified_name));
    if (cmp == 0) cmp = c.strcmp(safeStr(left.file_path), safeStr(right.file_path));
    if (cmp == 0) cmp = order(left.start_line, right.start_line);
    if (cmp == 0) cmp = order(left.end_line, right.end_line);
    if (cmp == 0) cmp = order(left.id, right.id);
    return cmp;
}

/// Runs a search on the store's backend or Zova repository and moves the
/// matching nodes out of the results. `keep` is the node field that must equal
/// `value`, or null to keep every result.
fn findNodesBySearch(s: *Store, params: *const c.cbm_search_params_t, comptime keep: ?[]const u8, value: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    var search = std.mem.zeroes(c.cbm_search_output_t);
    const rc = if (s.backend) |backend|
        backend.search.?(s.backend_ctx, params, &search)
    else
        c.cbm_zova_repository_search(s.zova_repository, &s.zova_graph_workspace_id, params, &search);
    if (rc != OK) {
        out.* = null;
        count.* = 0;
        return rc;
    }
    const nodes: [*c]c.cbm_node_t = if (search.count != 0) interop.callocN(c.cbm_node_t, @intCast(search.count)) else null;
    if (search.count != 0 and nodes == null) {
        cbm_store_search_free(&search);
        return ERR;
    }
    var matched: usize = 0;
    for (span(search.results, search.count)) |*result| {
        if (keep) |field| {
            const actual: CStr = @field(result.node, field);
            if (actual == null or !streq(actual, value)) continue;
        }
        nodes[matched] = result.node;
        matched += 1;
        result.node = std.mem.zeroes(c.cbm_node_t);
    }
    out.* = nodes;
    count.* = @intCast(matched);
    cbm_store_search_free(&search);
    return OK;
}

fn searchAllParams(project: CStr) c.cbm_search_params_t {
    return .{ .project = project, .min_degree = -1, .max_degree = -1, .limit = std.math.maxInt(c_int) };
}

pub export fn cbm_store_find_nodes_by_name(s_c: ?*Store, project: CStr, name: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (s.backend != null or usesRepository(s)) {
            var params = searchAllParams(project);
            params.name_pattern = name;
            return findNodesBySearch(s, &params, "name", name, out, count);
        }
    }
    return findNodesGeneric(s_c, "find_nodes_by_name", node_columns ++
        "WHERE project = ?1 AND name = ?2;", project, name, out, count);
}

/// label NULL matches every label.
pub export fn cbm_store_find_nodes_by_label(s_c: ?*Store, project: CStr, label: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (s.backend != null or usesRepository(s)) {
            var params = searchAllParams(project);
            params.label = label;
            const rc = findNodesBySearch(s, &params, null, null, out, count);
            if (rc == OK and count.* > 1) {
                c.qsort(out.*, @intCast(count.*), @sizeOf(c.cbm_node_t), nodePublicOrderCompare);
            }
            return rc;
        }
    }
    if (label == null) {
        return findNodesGeneric(s_c, "find_nodes", node_columns ++
            "WHERE project = ?1 " ++
            "ORDER BY qualified_name,file_path,start_line,end_line,id;", project, null, out, count);
    }
    return findNodesGeneric(s_c, "find_nodes_by_label", node_columns ++
        "WHERE project = ?1 AND label = ?2 " ++
        "ORDER BY qualified_name,file_path,start_line,end_line,id;", project, label, out, count);
}

pub export fn cbm_store_find_nodes(s: ?*Store, project: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    return cbm_store_find_nodes_by_label(s, project, null, out, count);
}

pub export fn cbm_store_find_nodes_by_file(s_c: ?*Store, project: CStr, file_path: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (usesRepository(s)) {
            var params = searchAllParams(project);
            params.file_pattern = file_path;
            return findNodesBySearch(s, &params, "file_path", file_path, out, count);
        }
    }
    return findNodesGeneric(s_c, "find_nodes_by_file", node_columns ++
        "WHERE project = ?1 AND file_path = ?2;", project, file_path, out, count);
}

fn fileOutlineIsContainer(label: CStr) bool {
    const containers = [_]Str{ "Module", "Package", "File", "Folder", "Project" };
    for (containers) |container| {
        if (streq(label, container)) return true;
    }
    return false;
}

fn fileOutlineNodeCmp(pa: ?*const anyopaque, pb: ?*const anyopaque) callconv(.c) c_int {
    const a = @as(*const *const c.cbm_node_t, @ptrCast(@alignCast(pa))).*;
    const b = @as(*const *const c.cbm_node_t, @ptrCast(@alignCast(pb))).*;
    if (a.start_line != b.start_line) return if (a.start_line < b.start_line) -1 else 1;
    if (a.end_line != b.end_line) return if (a.end_line < b.end_line) -1 else 1;
    var cmp = c.strcmp(safeStr(a.label), safeStr(b.label));
    if (cmp == 0) cmp = c.strcmp(safeStr(a.name), safeStr(b.name));
    if (cmp == 0) cmp = c.strcmp(safeStr(a.qualified_name), safeStr(b.qualified_name));
    if (cmp == 0 and a.id != b.id) cmp = if (a.id < b.id) -1 else 1;
    return cmp;
}

pub export fn cbm_store_get_file_outline(
    s: ?*Store,
    project: CStr,
    file_path: CStr,
    labels_c: ?[*]const CStr,
    label_count: c_int,
    limit: c_int,
    offset: c_int,
    out_c: ?*[*c]c.cbm_file_outline_row_t,
    count_c: ?*c_int,
    total_c: ?*c_int,
) c_int {
    const out = out_c orelse return ERR;
    const count = count_c orelse return ERR;
    const total = total_c orelse return ERR;
    out.* = null;
    count.* = 0;
    total.* = 0;
    if (s == null or project == null or file_path == null or file_path[0] == 0 or limit < 1 or
        limit > c.CBM_STORE_FILE_OUTLINE_MAX_LIMIT or offset < 0 or label_count < 0 or
        label_count > c.CBM_STORE_FILE_OUTLINE_MAX_LABELS or (label_count > 0 and labels_c == null))
    {
        return ERR;
    }
    const labels: []const CStr = if (labels_c) |l| l[0..@intCast(label_count)] else &.{};
    for (labels) |label| {
        if (label == null or label[0] == 0) return ERR;
    }

    var nodes: [*c]c.cbm_node_t = null;
    var node_count: c_int = 0;
    var rc = cbm_store_find_nodes_by_file(s, project, file_path, &nodes, &node_count);
    if (rc != OK) return rc;
    const kept: [*c]*const c.cbm_node_t = if (node_count != 0) interop.callocN(*const c.cbm_node_t, @intCast(node_count)) else null;
    if (node_count != 0 and kept == null) {
        cbm_store_free_nodes(nodes, node_count);
        return ERR;
    }
    var kept_count: c_int = 0;
    for (span(nodes, node_count)) |*node| {
        const label = safeStr(node.label);
        if (fileOutlineIsContainer(label)) continue;
        var wanted = labels.len == 0;
        for (labels) |want| {
            if (wanted) break;
            wanted = streq(label, want);
        }
        if (wanted) {
            kept[@intCast(kept_count)] = node;
            kept_count += 1;
        }
    }
    if (kept_count > 1) {
        c.qsort(@ptrCast(kept), @intCast(kept_count), @sizeOf(*const c.cbm_node_t), fileOutlineNodeCmp);
    }

    const first = @min(offset, kept_count);
    const page = @min(kept_count - first, limit);
    const page_nodes = span(kept, kept_count)[@intCast(first)..][0..@intCast(page)];
    var text_bytes: usize = 0;
    for (page_nodes) |n| {
        text_bytes += c.strlen(safeStr(n.name)) + c.strlen(safeStr(n.label)) + c.strlen(safeStr(n.qualified_name));
    }
    if (text_bytes > c.CBM_STORE_FILE_OUTLINE_MAX_TEXT_BYTES) {
        c.free(@ptrCast(kept));
        cbm_store_free_nodes(nodes, node_count);
        return c.CBM_STORE_SCAN_LIMIT;
    }

    var rows: [*c]c.cbm_file_outline_row_t = if (page != 0) interop.callocN(c.cbm_file_outline_row_t, @intCast(page)) else null;
    rc = if (page != 0 and rows == null) ERR else OK;
    if (rc == OK) {
        for (page_nodes, span(rows, page), 0..) |n, *row, i| {
            row.name = interop.heapStrdup(safeStr(n.name));
            row.label = interop.heapStrdup(safeStr(n.label));
            row.qualified_name = interop.heapStrdup(safeStr(n.qualified_name));
            row.start_line = n.start_line;
            row.end_line = n.end_line;
            if (row.name == null or row.label == null or row.qualified_name == null) {
                cbm_store_free_file_outline(rows, @intCast(i + 1));
                rows = null;
                rc = ERR;
                break;
            }
        }
    }
    c.free(@ptrCast(kept));
    cbm_store_free_nodes(nodes, node_count);
    if (rc != OK) return rc;
    out.* = rows;
    count.* = page;
    total.* = kept_count;
    return OK;
}

pub export fn cbm_store_free_file_outline(rows: [*c]c.cbm_file_outline_row_t, count: c_int) void {
    if (rows == null) return;
    for (span(rows, count)) |*row| {
        c.free(@constCast(row.name));
        c.free(@constCast(row.label));
        c.free(@constCast(row.qualified_name));
    }
    c.free(rows);
}

// ── Graph comparison ─────────────────────────────────────────────

const GraphCmpNode = extern struct {
    key_qn: CStr, // qualified_name without the leading "<project>."
    row: *const c.cbm_zova_identity_node_t,
};

const GraphCmpEdge = extern struct {
    source: *const GraphCmpNode,
    target: *const GraphCmpNode,
    row: *const c.cbm_zova_identity_edge_t,
};

const GraphCmpSide = struct {
    rows: [*c]c.cbm_zova_identity_node_t = null,
    row_count: c_int = 0,
    edge_rows: [*c]c.cbm_zova_identity_edge_t = null,
    edge_row_count: c_int = 0,
    nodes: [*c]GraphCmpNode = null, // identity order
    by_key: [*c]GraphCmpNode = null, // node-key order, for edge endpoints
    edges: [*c]GraphCmpEdge = null, // identity order
};

fn graphCmpStripProject(qn: CStr, project: CStr) CStr {
    const n = c.strlen(project);
    if (c.strncmp(qn, project, n) == 0) {
        if (qn[n] == '.') return qn + n + 1;
        if (qn[n] == 0) return qn + n;
    }
    return qn;
}

fn graphCmpNodeIdentity(a: *const GraphCmpNode, b: *const GraphCmpNode) c_int {
    var cmp = c.strcmp(a.key_qn, b.key_qn);
    if (cmp == 0) cmp = c.strcmp(a.row.label, b.row.label);
    if (cmp == 0) cmp = c.strcmp(a.row.file_path, b.row.file_path);
    return cmp;
}

fn graphCmpNodeQsort(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    return graphCmpNodeIdentity(@ptrCast(@alignCast(a)), @ptrCast(@alignCast(b)));
}

fn graphCmpEdgeIdentity(a: *const GraphCmpEdge, b: *const GraphCmpEdge) c_int {
    var cmp = graphCmpNodeIdentity(a.source, b.source);
    if (cmp == 0) cmp = graphCmpNodeIdentity(a.target, b.target);
    if (cmp == 0) cmp = c.strcmp(a.row.type, b.row.type);
    if (cmp == 0) cmp = c.strcmp(a.row.local_name, b.row.local_name);
    return cmp;
}

fn graphCmpEdgeQsort(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    return graphCmpEdgeIdentity(@ptrCast(@alignCast(a)), @ptrCast(@alignCast(b)));
}

fn graphCmpKeyBsearch(key: ?*const anyopaque, elem: ?*const anyopaque) callconv(.c) c_int {
    const k = @as(*const i64, @ptrCast(@alignCast(key))).*;
    const e = @as(*const GraphCmpNode, @ptrCast(@alignCast(elem))).row.node_key;
    return order(k, e);
}

fn graphCmpSideFree(side: *GraphCmpSide) void {
    c.cbm_zova_identities_free(side.rows, side.row_count, side.edge_rows, side.edge_row_count);
    c.free(side.nodes);
    c.free(side.by_key);
    c.free(side.edges);
    side.* = .{};
}

/// Load one side and order its nodes and edges by identity.
fn graphCmpSideLoad(s_c: ?*Store, project: CStr, max_nodes: u64, max_edges: u64, side: *GraphCmpSide, meta: *c.cbm_graph_compare_project_t) c_int {
    const s = s_c orelse return ERR;
    if (!usesRepository(s) or project == null) return ERR;
    var indexed_at: [*c]u8 = null;
    if (c.cbm_zova_repository_index_status(s.zova_repository, &s.zova_graph_workspace_id, &meta.generation, &indexed_at) != OK) {
        c.free(indexed_at);
        return ERR;
    }
    _ = interop.bprint(&meta.indexed_at, "{s}", .{std.mem.span(safeStr(indexed_at))});
    c.free(indexed_at);
    const rc = c.cbm_zova_repository_graph_identities(s.zova_repository, &s.zova_graph_workspace_id, max_nodes, max_edges, &side.rows, &side.row_count, &side.edge_rows, &side.edge_row_count);
    if (rc != OK) return rc;
    meta.node_count = side.row_count;
    meta.edge_count = side.edge_row_count;
    const nn: usize = @intCast(side.row_count);
    const ne: usize = @intCast(side.edge_row_count);
    side.nodes = if (nn != 0) interop.mallocN(GraphCmpNode, nn) else null;
    side.by_key = if (nn != 0) interop.mallocN(GraphCmpNode, nn) else null;
    side.edges = if (ne != 0) interop.mallocN(GraphCmpEdge, ne) else null;
    if ((nn != 0 and (side.nodes == null or side.by_key == null)) or (ne != 0 and side.edges == null)) {
        return ERR;
    }
    const by_key = span(side.by_key, side.row_count);
    const nodes = span(side.nodes, side.row_count);
    const edges = span(side.edges, side.edge_row_count);
    for (span(side.rows, side.row_count), by_key) |*row, *node| {
        for (std.mem.span(row.file_path)) |*ch| {
            if (ch.* == '\\') ch.* = '/';
        }
        node.* = .{ .key_qn = graphCmpStripProject(row.qualified_name, project), .row = row };
    }
    // Rows arrive in node-key order, so by_key is already sorted.
    @memcpy(nodes, by_key);
    c.qsort(nodes.ptr, nn, @sizeOf(GraphCmpNode), graphCmpNodeQsort);
    for (span(side.edge_rows, side.edge_row_count), edges) |*row, *edge| {
        const source: ?*const GraphCmpNode = @ptrCast(@alignCast(c.bsearch(&row.source_key, by_key.ptr, nn, @sizeOf(GraphCmpNode), graphCmpKeyBsearch)));
        const target: ?*const GraphCmpNode = @ptrCast(@alignCast(c.bsearch(&row.target_key, by_key.ptr, nn, @sizeOf(GraphCmpNode), graphCmpKeyBsearch)));
        // an edge endpoint outside the generation
        edge.* = .{ .source = source orelse return ERR, .target = target orelse return ERR, .row = row };
    }
    c.qsort(edges.ptr, ne, @sizeOf(GraphCmpEdge), graphCmpEdgeQsort);
    return OK;
}

fn graphCmpNodeOut(node: *const GraphCmpNode) c.cbm_graph_node_identity_t {
    return .{ .qualified_name = node.row.qualified_name, .label = node.row.label, .file_path = node.row.file_path };
}

fn graphCmpEmitNode(callback: c.cbm_graph_compare_node_fn, context: ?*anyopaque, added: bool, node: *const GraphCmpNode, total: *u64) bool {
    total.* += 1;
    const id = graphCmpNodeOut(node);
    return if (callback) |call| call(context, added, &id) else true;
}

fn graphCmpEmitEdge(callback: c.cbm_graph_compare_edge_fn, context: ?*anyopaque, added: bool, edge: *const GraphCmpEdge, total: *u64) bool {
    total.* += 1;
    const id: c.cbm_graph_edge_identity_t = .{
        .source = graphCmpNodeOut(edge.source),
        .target = graphCmpNodeOut(edge.target),
        .type = edge.row.type,
        .local_name = edge.row.local_name,
    };
    return if (callback) |call| call(context, added, &id) else true;
}

pub export fn cbm_store_compare_graphs(
    base_store: ?*Store,
    base_project: CStr,
    target_store: ?*Store,
    target_project: CStr,
    scan_limit: u64,
    on_node: c.cbm_graph_compare_node_fn,
    on_edge: c.cbm_graph_compare_edge_fn,
    context: ?*anyopaque,
    out_c: ?*c.cbm_graph_compare_result_t,
) c_int {
    const out = out_c orelse return ERR;
    out.* = std.mem.zeroes(c.cbm_graph_compare_result_t);
    var base: GraphCmpSide = .{};
    var target: GraphCmpSide = .{};
    var rc = graphCmpSideLoad(base_store, base_project, scan_limit, scan_limit, &base, &out.base);
    if (rc == OK) {
        rc = graphCmpSideLoad(target_store, target_project, scan_limit -% @as(u64, @intCast(base.row_count)), scan_limit -% @as(u64, @intCast(base.edge_row_count)), &target, &out.target);
    }
    var ok = rc == OK;
    if (ok) {
        const base_nodes = span(base.nodes, base.row_count);
        const target_nodes = span(target.nodes, target.row_count);
        var i: usize = 0;
        var j: usize = 0;
        while (ok and (i < base_nodes.len or j < target_nodes.len)) {
            const cmp: c_int = if (i >= base_nodes.len)
                1
            else if (j >= target_nodes.len)
                -1
            else
                graphCmpNodeIdentity(&base_nodes[i], &target_nodes[j]);
            if (cmp < 0) {
                ok = graphCmpEmitNode(on_node, context, false, &base_nodes[i], &out.nodes_removed_total);
                i += 1;
            } else if (cmp > 0) {
                ok = graphCmpEmitNode(on_node, context, true, &target_nodes[j], &out.nodes_added_total);
                j += 1;
            } else {
                i += 1;
                j += 1;
            }
        }
        const base_edges = span(base.edges, base.edge_row_count);
        const target_edges = span(target.edges, target.edge_row_count);
        i = 0;
        j = 0;
        while (ok and (i < base_edges.len or j < target_edges.len)) {
            const cmp: c_int = if (i >= base_edges.len)
                1
            else if (j >= target_edges.len)
                -1
            else
                graphCmpEdgeIdentity(&base_edges[i], &target_edges[j]);
            if (cmp < 0) {
                ok = graphCmpEmitEdge(on_edge, context, false, &base_edges[i], &out.edges_removed_total);
                i += 1;
            } else if (cmp > 0) {
                ok = graphCmpEmitEdge(on_edge, context, true, &target_edges[j], &out.edges_added_total);
                j += 1;
            } else {
                i += 1;
                j += 1;
            }
        }
    }
    if (rc == OK and !ok) rc = ERR;
    graphCmpSideFree(&base);
    graphCmpSideFree(&target);
    return rc;
}

/// A Zova workspace store has no legacy nodes and edges tables. The schema,
/// count and architecture queries below read nodes through a temp view over the
/// workspace's rows, whose ids are Zova node keys; edges come from the graph
/// through storeZovaEdges. The store holds one workspace, so project.* is
/// replaced by the name the rows carry. Does nothing for other stores.
fn storeLegacyScope(s_c: ?*Store, project: *CStr) c_int {
    const s = s_c orelse return OK;
    if (s.db == null or !usesRepository(s)) return OK;
    if (s.zova_legacy_project == null) {
        var row = std.mem.zeroes(c.cbm_project_t);
        if (c.cbm_zova_repository_get_project(s.zova_repository, &s.zova_graph_workspace_id, &row) != OK) {
            return ERR;
        }
        const sql = c.sqlite3_mprintf("CREATE TEMP VIEW nodes AS SELECT n.zova_node_key AS id,%Q AS project,n.label," ++
            "n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,n.properties " ++
            "FROM main.cbm_nodes_v1 n JOIN main.cbm_files_v1 f ON f.file_key=n.file_key " ++
            "WHERE n.workspace_key=(SELECT workspace_key FROM main.cbm_workspace_registry " ++
            "WHERE workspace_id=%Q)", row.name, @as(CStr, &s.zova_graph_workspace_id));
        const rc = if (sql != null) c.sqlite3_exec(s.db, sql, null, null, null) else c.SQLITE_NOMEM;
        c.sqlite3_free(sql);
        if (rc == c.SQLITE_OK) s.zova_legacy_project = interop.heapStrdup(row.name);
        cbm_project_free_fields(&row);
        if (s.zova_legacy_project == null) {
            storeSetErrorSqlite(s, "zova legacy nodes");
            return ERR;
        }
    }
    project.* = s.zova_legacy_project;
    return OK;
}

fn storeIsZovaWorkspace(s_c: ?*const Store) bool {
    const s = s_c orelse return false;
    return usesRepository(s);
}

const ZovaEdgeLoad = struct {
    store: *Store,
    capacity: c_int = 0,
};

fn storeZovaEdgeTypeFind(s: *const Store, edge_type: []const u8) c_int {
    for (s.zovaEdgeTypes(), 0..) |name, i| {
        if (std.mem.eql(u8, std.mem.span(name), edge_type)) return @intCast(i);
    }
    return c.CBM_NOT_FOUND;
}

fn storeZovaEdgeLoad(source_key: i64, target_key: i64, type_c: [*c]const u8, type_len: usize, properties: [*c]const u8, properties_len: usize, context_ptr: ?*anyopaque) callconv(.c) c_int {
    _ = .{ properties, properties_len };
    const context: *ZovaEdgeLoad = @ptrCast(@alignCast(context_ptr));
    const s = context.store;
    const edge_type: []const u8 = if (type_len == 0) "" else type_c[0..type_len];
    var t = storeZovaEdgeTypeFind(s, edge_type);
    if (t < 0) {
        const types = interop.reallocN([*c]u8, s.zova_edge_types, @intCast(s.zova_edge_type_count + 1));
        const name: [*c]u8 = if (types != null) interop.mallocN(u8, type_len + 1) else null;
        if (types != null) s.zova_edge_types = types;
        if (name == null) return c.CBM_NOT_FOUND;
        @memcpy(name[0..type_len], edge_type);
        name[type_len] = 0;
        t = s.zova_edge_type_count;
        s.zova_edge_types[@intCast(t)] = name;
        s.zova_edge_type_count += 1;
    }
    if (s.zova_edge_count == context.capacity) {
        const capacity = if (context.capacity != 0) context.capacity * ST_GROWTH else c.CBM_SZ_1K;
        const edges = interop.reallocN(ZovaEdge, s.zova_edges, @intCast(capacity));
        if (edges == null) return c.CBM_NOT_FOUND;
        s.zova_edges = edges;
        context.capacity = capacity;
    }
    s.zova_edges[@intCast(s.zova_edge_count)] = .{ .source_key = source_key, .target_key = target_key, .type = t };
    s.zova_edge_count += 1;
    return 0;
}

/// The workspace's cbm edges, read once per store from the Zova graph in scan
/// order. Schema, count and architecture reads aggregate this list instead of
/// querying an edges table.
fn storeZovaEdges(s: *Store) c_int {
    if (s.zova_edges_loaded) return OK;
    var context: ZovaEdgeLoad = .{ .store = s };
    if (c.cbm_zova_repository_visit_edges(s.zova_repository, &s.zova_graph_workspace_id, null, storeZovaEdgeLoad, &context) != OK) {
        c.free(s.zova_edges);
        s.zova_edges = null;
        s.zova_edge_count = 0;
        storeSetError(s, "zova edges");
        return ERR;
    }
    s.zova_edges_loaded = true;
    return OK;
}

fn storeIdCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = @as(*const i64, @ptrCast(@alignCast(left_ptr))).*;
    const right = @as(*const i64, @ptrCast(@alignCast(right_ptr))).*;
    return order(left, right);
}

/// True when id is in the ascending id list.
fn storeIdIn(ids: []const i64, id: i64) bool {
    return ids.len > 0 and c.bsearch(&id, ids.ptr, ids.len, @sizeOf(i64), storeIdCompare) != null;
}

pub export fn cbm_store_count_nodes(s_c: ?*Store, project_c: CStr) c_int {
    const s = s_c orelse return 0;
    if (s.db == null) return 0;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;
    const stmt = prepareCached(s, &s.stmts.count_nodes, "SELECT COUNT(*) FROM nodes WHERE project = ?1;") orelse return ERR;

    _ = bindText(stmt, 1, project);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) return c.sqlite3_column_int(stmt, 0);
    return 0;
}

pub export fn cbm_store_delete_nodes_by_project(s: ?*Store, project: CStr) c_int {
    return execCached(s, "delete_nodes_by_project", "DELETE FROM nodes WHERE project = ?1;", &.{project}, "delete_nodes_by_project");
}

pub export fn cbm_store_delete_nodes_by_file(s: ?*Store, project: CStr, file_path: CStr) c_int {
    return execCached(s, "delete_nodes_by_file", "DELETE FROM nodes WHERE project = ?1 AND file_path = ?2;", &.{ project, file_path }, "delete_nodes_by_file");
}

pub export fn cbm_store_delete_nodes_by_label(s: ?*Store, project: CStr, label: CStr) c_int {
    return execCached(s, "delete_nodes_by_label", "DELETE FROM nodes WHERE project = ?1 AND label = ?2;", &.{ project, label }, "delete_nodes_by_label");
}

// ── Node batch ───────────────────────────────────────────────────

pub export fn cbm_store_upsert_node_batch(s: ?*Store, nodes: [*c]const c.cbm_node_t, count: c_int, out_ids: [*c]i64) c_int {
    if (count == 0) return OK;

    _ = execSql(s, "BEGIN IMMEDIATE;");
    for (span(nodes, count), 0..) |*node, i| {
        const id = cbm_store_upsert_node(s, node);
        if (id == ERR) {
            _ = execSql(s, "ROLLBACK;");
            return ERR;
        }
        if (out_ids != null) out_ids[i] = id;
    }
    _ = execSql(s, "COMMIT;");
    return OK;
}

// ── Edge CRUD ────────────────────────────────────────────────────

const edge_columns = "SELECT id, project, source_id, target_id, type, properties FROM edges ";

pub export fn cbm_store_insert_edge(s_c: ?*Store, e: *const c.cbm_edge_t) i64 {
    const s = s_c orelse return ERR;
    // Conflict target includes local_name_gen (#768) so IMPORTS edges with
    // different local_name coexist while re-inserting the same import still
    // upserts. Must match the table's UNIQUE constraint in initSchema.
    const stmt = prepareCached(s, &s.stmts.insert_edge, "INSERT INTO edges (project, source_id, target_id, type, properties) " ++
        "VALUES (?1, ?2, ?3, ?4, ?5) " ++
        "ON CONFLICT(source_id, target_id, type, local_name_gen) DO UPDATE SET " ++
        "properties = json_patch(properties, ?5) " ++
        "RETURNING id;") orelse return ERR;

    _ = bindText(stmt, 1, safeStr(e.project));
    _ = c.sqlite3_bind_int64(stmt, 2, e.source_id);
    _ = c.sqlite3_bind_int64(stmt, 3, e.target_id);
    _ = bindText(stmt, 4, safeStr(e.type));
    _ = bindText(stmt, 5, safeProps(e.properties_json));

    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const id = c.sqlite3_column_int64(stmt, 0);
        _ = c.sqlite3_reset(stmt); // unblock COMMIT — RETURNING leaves stmt active
        return id;
    }
    _ = c.sqlite3_reset(stmt);
    storeSetErrorSqlite(s, "insert_edge");
    return ERR;
}

/// Scan an edge from current row of stmt.
fn scanEdge(stmt: ?*Stmt, e: *c.cbm_edge_t) void {
    e.id = c.sqlite3_column_int64(stmt, 0);
    e.project = columnDup(stmt, 1);
    e.source_id = c.sqlite3_column_int64(stmt, 2);
    e.target_id = c.sqlite3_column_int64(stmt, 3);
    e.type = columnDup(stmt, 4);
    e.properties_json = columnDup(stmt, 5);
}

/// Steps stmt to completion, scanning every row into a malloc'd edge array.
fn collectEdges(s: *Store, stmt: ?*Stmt, cap: c_int, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    var rows = Rows(c.cbm_edge_t).init(cap);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) scanEdge(stmt, rows.add());
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        cbm_store_free_edges(rows.items, rows.len);
        out.* = null;
        count.* = 0;
        return ERR;
    }
    out.* = rows.items;
    count.* = rows.len;
    return OK;
}

/// What an edge query binds: an endpoint id or a project as ?1, then an
/// optional edge type as ?2.
const EdgeBind = struct {
    id: ?i64 = null,
    project: CStr = null,
    type: ?CStr = null,
};

/// Generic: find multiple edges by a filter.
fn findEdgesGeneric(s_c: ?*Store, comptime slot: []const u8, sql: CStr, bind: EdgeBind, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    const s = s_c orelse {
        out.* = null;
        count.* = 0;
        return ERR;
    };
    if (s.db == null) {
        out.* = null;
        count.* = 0;
        return ERR;
    }
    const stmt = prepareCached(s, &@field(s.stmts, slot), sql) orelse return ERR;

    if (bind.id) |id| {
        _ = c.sqlite3_bind_int64(stmt, 1, id);
    } else {
        _ = bindText(stmt, 1, bind.project);
    }
    if (bind.type) |edge_type| _ = bindText(stmt, 2, edge_type);

    return collectEdges(s, stmt, ST_INIT_CAP_16, out, count);
}

fn storeZovaWorkspaceEdges(s: *Store, node_numeric_id: i64, direction: CStr, edge_type: CStr, out_c: ?*[*c]c.cbm_edge_t, count_c: ?*c_int) c_int {
    const out = out_c orelse return NOT_FOUND;
    const count = count_c orelse return NOT_FOUND;
    if (!usesRepository(s)) return NOT_FOUND;
    out.* = null;
    count.* = 0;
    var stable = std.mem.zeroes([c.CBM_ZOVA_WORKSPACE_ID_MAX + 96]u8);
    var rc = c.cbm_zova_repository_stable_id_for_numeric_id(s.zova_repository, &s.zova_graph_workspace_id, node_numeric_id, &stable, stable.len);
    if (rc != OK) return rc;
    var edges: [*c]c.cbm_edge_t = null;
    var edge_count: c_int = 0;
    rc = c.cbm_zova_repository_find_edges(s.zova_repository, &s.zova_graph_workspace_id, &stable, direction, &edges, &edge_count);
    if (rc != OK) return rc;
    if (edge_type != null) {
        var kept: usize = 0;
        for (span(edges, edge_count), 0..) |*edge, i| {
            if (edge.type != null and streq(edge.type, edge_type)) {
                if (kept != i) {
                    edges[kept] = edge.*;
                    edge.* = std.mem.zeroes(c.cbm_edge_t);
                }
                kept += 1;
            } else {
                interop.strFree(&edge.project);
                interop.strFree(&edge.type);
                interop.strFree(&edge.properties_json);
            }
        }
        edge_count = @intCast(kept);
    }
    out.* = edges;
    count.* = edge_count;
    s.zova_native_topology_ops += 1;
    return OK;
}

pub export fn cbm_store_find_edges_by_source(s_c: ?*Store, source_id: i64, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (s.backend) |backend| return backend.find_edges.?(s.backend_ctx, source_id, "outbound", null, out, count);
        if (s.zova_workspace_query) return storeZovaWorkspaceEdges(s, source_id, "outbound", null, out, count);
    }
    return findEdgesGeneric(s_c, "find_edges_by_source", edge_columns ++ "WHERE source_id = ?1;", .{ .id = source_id }, out, count);
}

pub export fn cbm_store_find_edges_by_target(s_c: ?*Store, target_id: i64, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (s.backend) |backend| return backend.find_edges.?(s.backend_ctx, target_id, "inbound", null, out, count);
        if (s.zova_workspace_query) return storeZovaWorkspaceEdges(s, target_id, "inbound", null, out, count);
    }
    return findEdgesGeneric(s_c, "find_edges_by_target", edge_columns ++ "WHERE target_id = ?1;", .{ .id = target_id }, out, count);
}

pub export fn cbm_store_find_edges_by_source_type(s_c: ?*Store, source_id: i64, edge_type: CStr, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (s.backend) |backend| return backend.find_edges.?(s.backend_ctx, source_id, "outbound", edge_type, out, count);
        if (s.zova_workspace_query) return storeZovaWorkspaceEdges(s, source_id, "outbound", edge_type, out, count);
    }
    return findEdgesGeneric(s_c, "find_edges_by_source_type", edge_columns ++ "WHERE source_id = ?1 AND type = ?2;", .{ .id = source_id, .type = edge_type }, out, count);
}

pub export fn cbm_store_find_edges_by_target_type(s_c: ?*Store, target_id: i64, edge_type: CStr, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (s.backend) |backend| return backend.find_edges.?(s.backend_ctx, target_id, "inbound", edge_type, out, count);
        if (s.zova_workspace_query) return storeZovaWorkspaceEdges(s, target_id, "inbound", edge_type, out, count);
    }
    return findEdgesGeneric(s_c, "find_edges_by_target_type", edge_columns ++ "WHERE target_id = ?1 AND type = ?2;", .{ .id = target_id, .type = edge_type }, out, count);
}

pub export fn cbm_store_find_edges_by_type(s_c: ?*Store, project: CStr, edge_type: CStr, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (usesRepository(s)) {
            return c.cbm_zova_repository_find_edges_by_type(s.zova_repository, &s.zova_graph_workspace_id, edge_type, out, count);
        }
    }
    return findEdgesGeneric(s_c, "find_edges_by_type", edge_columns ++ "WHERE project = ?1 AND type = ?2;", .{ .project = project, .type = edge_type }, out, count);
}

pub export fn cbm_store_count_edges(s_c: ?*Store, project: CStr) c_int {
    const s = s_c orelse return 0;
    if (s.db == null) return 0;
    if (usesRepository(s)) {
        var nodes: c_int = 0;
        var edges: c_int = 0;
        return if (c.cbm_zova_repository_counts(s.zova_repository, &s.zova_graph_workspace_id, &nodes, &edges) == OK) edges else ERR;
    }
    const stmt = prepareCached(s, &s.stmts.count_edges, "SELECT COUNT(*) FROM edges WHERE project = ?1;") orelse return ERR;

    _ = bindText(stmt, 1, project);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) return c.sqlite3_column_int(stmt, 0);
    return 0;
}

pub export fn cbm_store_count_edges_by_type(s_c: ?*Store, project: CStr, edge_type: CStr) c_int {
    const s = s_c orelse return ERR;
    if (usesRepository(s)) {
        if (storeZovaEdges(s) != OK) return ERR;
        const t = if (edge_type != null) storeZovaEdgeTypeFind(s, std.mem.span(edge_type)) else c.CBM_NOT_FOUND;
        var n: c_int = 0;
        if (t >= 0) {
            for (s.zovaEdges()) |edge| n += @intFromBool(edge.type == t);
        }
        return n;
    }
    const stmt = prepareCached(s, &s.stmts.count_edges_by_type, "SELECT COUNT(*) FROM edges WHERE project = ?1 AND type = ?2;") orelse return ERR;

    _ = bindText(stmt, 1, project);
    _ = bindText(stmt, 2, edge_type);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) return c.sqlite3_column_int(stmt, 0);
    return 0;
}

pub export fn cbm_store_delete_edges_by_project(s: ?*Store, project: CStr) c_int {
    return execCached(s, "delete_edges_by_project", "DELETE FROM edges WHERE project = ?1;", &.{project}, "delete_edges_by_project");
}

pub export fn cbm_store_delete_edges_by_type(s: ?*Store, project: CStr, edge_type: CStr) c_int {
    return execCached(s, "delete_edges_by_type", "DELETE FROM edges WHERE project = ?1 AND type = ?2;", &.{ project, edge_type }, "delete_edges_by_type");
}

// ── Edge batch ───────────────────────────────────────────────────

pub export fn cbm_store_insert_edge_batch(s: ?*Store, edges: [*c]const c.cbm_edge_t, count: c_int) c_int {
    if (count == 0) return OK;

    _ = execSql(s, "BEGIN IMMEDIATE;");
    for (span(edges, count)) |*edge| {
        if (cbm_store_insert_edge(s, edge) == ERR) {
            _ = execSql(s, "ROLLBACK;");
            return ERR;
        }
    }
    _ = execSql(s, "COMMIT;");
    return OK;
}

// ── File hash CRUD ───────────────────────────────────────────────

pub export fn cbm_store_upsert_file_hash(s_c: ?*Store, project: CStr, rel_path: CStr, sha256: CStr, mtime_ns: i64, size: i64) c_int {
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &s.stmts.upsert_file_hash, "INSERT INTO file_hashes (project, rel_path, sha256, mtime_ns, size) " ++
        "VALUES (?1, ?2, ?3, ?4, ?5) " ++
        "ON CONFLICT(project, rel_path) DO UPDATE SET " ++
        "sha256=?3, mtime_ns=?4, size=?5;") orelse return ERR;

    _ = bindText(stmt, 1, project);
    _ = bindText(stmt, 2, rel_path);
    _ = bindText(stmt, 3, sha256);
    _ = c.sqlite3_bind_int64(stmt, 4, mtime_ns);
    _ = c.sqlite3_bind_int64(stmt, 5, size);

    if (c.sqlite3_step(stmt) != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "upsert_file_hash");
        return ERR;
    }
    return OK;
}

pub export fn cbm_store_get_file_hashes(s_c: ?*Store, project: CStr, out: *[*c]c.cbm_file_hash_t, count: *c_int) c_int {
    const s = s_c orelse return ERR;
    const stmt = prepareCached(s, &s.stmts.get_file_hashes, "SELECT project, rel_path, sha256, mtime_ns, size " ++
        "FROM file_hashes WHERE project = ?1;") orelse return ERR;

    _ = bindText(stmt, 1, project);

    var rows = Rows(c.cbm_file_hash_t).init(ST_INIT_CAP_16);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const row = rows.add();
        row.project = columnDup(stmt, 0);
        row.rel_path = columnDup(stmt, 1);
        row.sha256 = columnDup(stmt, 2);
        row.mtime_ns = c.sqlite3_column_int64(stmt, 3);
        row.size = c.sqlite3_column_int64(stmt, 4);
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        cbm_store_free_file_hashes(rows.items, rows.len);
        out.* = null;
        count.* = 0;
        return ERR;
    }

    out.* = rows.items;
    count.* = rows.len;
    return OK;
}

pub export fn cbm_store_delete_file_hash(s: ?*Store, project: CStr, rel_path: CStr) c_int {
    return execCached(s, "delete_file_hash", "DELETE FROM file_hashes WHERE project = ?1 AND rel_path = ?2;", &.{ project, rel_path }, "delete_file_hash");
}

pub export fn cbm_store_delete_file_hashes(s: ?*Store, project: CStr) c_int {
    return execCached(s, "delete_file_hashes", "DELETE FROM file_hashes WHERE project = ?1;", &.{project}, "delete_file_hashes");
}

// ── FindNodesByFileOverlap ───────────────────────────────────────

pub export fn cbm_store_find_nodes_by_file_overlap(s_c: ?*Store, project: CStr, file_path: CStr, start_line: c_int, end_line: c_int, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    if (s_c) |s| {
        if (usesRepository(s)) {
            return c.cbm_zova_repository_find_nodes_by_file_overlap(s.zova_repository, &s.zova_graph_workspace_id, file_path, start_line, end_line, out, count);
        }
    }
    out.* = null;
    count.* = 0;
    const s = s_c orelse return ERR;

    var stmt: ?*Stmt = null;
    if (prepare(s.db, node_columns ++
        "WHERE project = ?1 AND file_path = ?2 " ++
        "AND label NOT IN ('Module', 'Package', 'File', 'Folder') " ++
        "AND start_line <= ?4 AND end_line >= ?3 " ++
        "ORDER BY start_line", &stmt) != c.SQLITE_OK)
    {
        storeSetErrorSqlite(s, "overlap prepare");
        return ERR;
    }
    defer _ = c.sqlite3_finalize(stmt);

    _ = bindText(stmt, 1, project);
    _ = bindText(stmt, 2, file_path);
    _ = c.sqlite3_bind_int(stmt, 3, start_line);
    _ = c.sqlite3_bind_int(stmt, 4, end_line);

    return collectNodes(s, stmt, ST_INIT_CAP_8, out, count);
}

// ── FindNodesByQNSuffix ──────────────────────────────────────────

pub export fn cbm_store_find_nodes_by_qn_suffix(s_c: ?*Store, project_c: CStr, suffix: CStr, out: *[*c]c.cbm_node_t, count: *c_int) c_int {
    out.* = null;
    count.* = 0;
    const s = s_c orelse return ERR;
    if (s.db == null) return ERR;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;
    // Match QNs ending with ".suffix" or exactly equal to suffix
    var like_pattern: [c.CBM_SZ_512]u8 = undefined;
    _ = interop.bprint(&like_pattern, "%.{s}", .{interop.sp(suffix)});

    const sql_with_project = node_columns ++
        "WHERE project = ?1 AND (qualified_name LIKE ?2 OR qualified_name = ?3)";
    const sql_any = node_columns ++
        "WHERE (qualified_name LIKE ?1 OR qualified_name = ?2)";

    var stmt: ?*Stmt = null;
    if (prepare(s.db, if (project != null) sql_with_project else sql_any, &stmt) != c.SQLITE_OK) {
        storeSetErrorSqlite(s, "qn_suffix prepare");
        return ERR;
    }

    if (project != null) {
        _ = bindText(stmt, 1, project);
        _ = bindText(stmt, 2, &like_pattern);
        _ = bindText(stmt, 3, suffix);
    } else {
        _ = bindText(stmt, 1, &like_pattern);
        _ = bindText(stmt, 2, suffix);
    }

    var nodes: [*c]c.cbm_node_t = null;
    var n: c_int = 0;
    const rc = collectNodes(s, stmt, ST_INIT_CAP_8, &nodes, &n);
    _ = c.sqlite3_finalize(stmt);
    if (rc != OK) return rc;
    if (usesRepository(s)) {
        // The view's ids are Zova node keys; callers expect repository ids.
        for (span(nodes, n)) |*node| {
            var resolved = std.mem.zeroes(c.cbm_node_t);
            if (c.cbm_zova_repository_find_node_by_qn(s.zova_repository, &s.zova_graph_workspace_id, node.qualified_name, &resolved) != OK) {
                cbm_store_free_nodes(nodes, n);
                return ERR;
            }
            cbm_node_free_fields(node);
            node.* = resolved;
        }
    }
    out.* = nodes;
    count.* = n;
    return OK;
}

// ── NodeDegree ───────────────────────────────────────────────────

/// COUNT(*) of the CALLS edges that have node_id in `column`; leaves out
/// untouched when the query fails.
fn countCallEdges(s: *Store, comptime column: []const u8, node_id: i64, out: *c_int) void {
    var stmt: ?*Stmt = null;
    if (prepare(s.db, "SELECT COUNT(*) FROM edges WHERE " ++ column ++ " = ?1 AND type = 'CALLS'", &stmt) != c.SQLITE_OK) return;
    _ = c.sqlite3_bind_int64(stmt, 1, node_id);
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) out.* = c.sqlite3_column_int(stmt, 0);
    _ = c.sqlite3_finalize(stmt);
}

pub export fn cbm_store_node_degree(s_c: ?*Store, node_id: i64, in_deg: [*c]c_int, out_deg: [*c]c_int) void {
    const s = s_c orelse {
        if (in_deg != null) in_deg.* = 0;
        if (out_deg != null) out_deg.* = 0;
        return;
    };
    in_deg.* = 0;
    out_deg.* = 0;

    if (s.backend) |backend| {
        if (backend.node_degree.?(s.backend_ctx, node_id, in_deg, out_deg) != OK) {
            in_deg.* = 0;
            out_deg.* = 0;
        }
        return;
    }
    if (s.zova_workspace_query) {
        var stable = std.mem.zeroes([c.CBM_ZOVA_WORKSPACE_ID_MAX + 96]u8);
        var graph: [c.CBM_ZOVA_WORKSPACE_ID_MAX + 32]u8 = undefined;
        const workspace_id: CStr = &s.zova_graph_workspace_id;
        if (s.zova_graph_session == null or s.zova_repository == null or
            c.cbm_zova_repository_stable_id_for_numeric_id(s.zova_repository, workspace_id, node_id, &stable, stable.len) != OK)
        {
            return;
        }
        if (stable[0] == 0 or c.cbm_zova_workspace_graph_name(workspace_id, &graph, graph.len) != 0) return;
        const types = [_]CStr{"CALLS"};
        if (c.cbm_zova_graph_session_degree(s.zova_graph_session, workspace_id, &graph, &stable, "inbound", &types, 1, in_deg) != 0 or
            c.cbm_zova_graph_session_degree(s.zova_graph_session, workspace_id, &graph, &stable, "outbound", &types, 1, out_deg) != 0)
        {
            in_deg.* = 0;
            out_deg.* = 0;
            return;
        }
        s.zova_native_topology_ops += 2;
        return;
    }

    countCallEdges(s, "target_id", node_id, in_deg);
    countCallEdges(s, "source_id", node_id, out_deg);
}

// ── List distinct file paths ─────────────────────────────────────

fn freeStrings(items: [*c][*c]u8, count: c_int) void {
    for (span(items, count)) |item| c.free(item);
    c.free(@ptrCast(items));
}

pub export fn cbm_store_list_files(s_c: ?*Store, project_c: CStr, out: *[*c][*c]u8, count: *c_int) c_int {
    out.* = null;
    count.* = 0;
    const s = s_c orelse return ERR;
    if (s.db == null or project_c == null) return ERR;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;

    // Upstream cc9c2f4c: a node path can name a graph-only identity (a Folder,
    // <python-builtins>, another synthetic source). Indexed stores have one
    // canonical File node per scannable path, so prefer that exact set; stores
    // without File nodes keep the node-path fallback minus Folder directories.
    var stmt: ?*Stmt = null;
    if (prepare(s.db, "SELECT DISTINCT file_path FROM nodes " ++
        "WHERE project = ?1 AND label != 'Folder' " ++
        "AND file_path IS NOT NULL AND file_path != '' " ++
        "AND (label = 'File' OR NOT EXISTS (" ++
        "SELECT 1 FROM nodes WHERE project = ?1 AND label = 'File'))", &stmt) != c.SQLITE_OK)
    {
        return ERR;
    }
    _ = c.sqlite3_bind_text(stmt, 1, project, -1, null);

    var files = Rows([*c]u8).init(c.CBM_SZ_64);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const fp = columnText(stmt, 0);
        if (fp == null) continue;
        files.add().* = interop.heapStrdup(fp);
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        freeStrings(files.items, files.len);
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);
    out.* = files.items;
    count.* = files.len;
    return OK;
}

// ── Node neighbor names ──────────────────────────────────────────

fn queryNeighborNames(db: ?*c.sqlite3, sql: CStr, node_id: i64, limit: c_int, out: *[*c][*c]u8, out_count: *c_int) c_int {
    out.* = null;
    out_count.* = 0;
    var stmt: ?*Stmt = null;
    if (prepare(db, sql, &stmt) != c.SQLITE_OK) return c.CBM_NOT_FOUND;
    _ = c.sqlite3_bind_int64(stmt, 1, node_id);
    _ = c.sqlite3_bind_int(stmt, 2, limit);

    var names = Rows([*c]u8).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const name = columnText(stmt, 0);
        if (name == null) continue;
        names.add().* = c.strdup(name);
    }
    _ = c.sqlite3_finalize(stmt);
    if (rc != c.SQLITE_DONE) {
        // No store handle here; neighbor names are include_connected
        // enrichment — fail the call, primary surfaces report loudly.
        freeStrings(names.items, names.len);
        return c.CBM_NOT_FOUND;
    }
    out.* = names.items;
    out_count.* = names.len;
    return 0;
}

fn storeNameCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(@as(*const [*c]u8, @ptrCast(@alignCast(left))).*, @as(*const [*c]u8, @ptrCast(@alignCast(right))).*);
}

/// Distinct, sorted names of the call neighbors of a Zova node, from the graph's
/// own adjacency index and the workspace nodes view.
fn storeZovaNeighborNames(s: *Store, stable_id: CStr, inbound: bool, limit: c_int, out: *[*c][*c]u8, out_count: *c_int) c_int {
    var keys: [*c]i64 = null;
    var key_count: c_int = 0;
    if (c.cbm_zova_repository_call_neighbor_keys(s.zova_repository, &s.zova_graph_workspace_id, stable_id, inbound, &keys, &key_count) != OK) {
        return ERR;
    }
    var stmt: ?*Stmt = null;
    if (key_count == 0 or prepare(s.db, "SELECT name FROM nodes WHERE id = ?1", &stmt) != c.SQLITE_OK) {
        c.free(keys);
        return if (key_count == 0) OK else ERR;
    }
    const names: [*c][*c]u8 = interop.mallocN([*c]u8, @intCast(key_count));
    var count: usize = 0;
    if (names != null) {
        for (span(keys, key_count)) |key| {
            _ = c.sqlite3_reset(stmt);
            _ = c.sqlite3_bind_int64(stmt, 1, key);
            const name: CStr = if (c.sqlite3_step(stmt) == c.SQLITE_ROW) columnText(stmt, 0) else null;
            if (name != null) {
                names[count] = interop.heapStrdup(name);
                count += 1;
            }
        }
    }
    _ = c.sqlite3_finalize(stmt);
    c.free(keys);
    if (names == null) return ERR;
    c.qsort(@ptrCast(names), count, @sizeOf([*c]u8), storeNameCompare);
    var kept: usize = 0;
    for (names[0..count]) |name| {
        if (kept < limit and (kept == 0 or !streq(names[kept - 1], name))) {
            names[kept] = name;
            kept += 1;
        } else {
            c.free(name);
        }
    }
    out.* = names;
    out_count.* = @intCast(kept);
    return OK;
}

pub export fn cbm_store_node_neighbor_names(s_c: ?*Store, node_id: i64, limit: c_int, out_callers: *[*c][*c]u8, caller_count: *c_int, out_callees: *[*c][*c]u8, callee_count: *c_int) c_int {
    const s = s_c orelse return c.CBM_NOT_FOUND;
    out_callers.* = null;
    caller_count.* = 0;
    out_callees.* = null;
    callee_count.* = 0;

    if (usesRepository(s)) {
        var project: CStr = null;
        var stable = std.mem.zeroes([c.CBM_ZOVA_WORKSPACE_ID_MAX + 96]u8);
        if (storeLegacyScope(s, &project) != OK or
            c.cbm_zova_repository_stable_id_for_numeric_id(s.zova_repository, &s.zova_graph_workspace_id, node_id, &stable, stable.len) != OK)
        {
            return ERR;
        }
        if (storeZovaNeighborNames(s, &stable, true, limit, out_callers, caller_count) != OK or
            storeZovaNeighborNames(s, &stable, false, limit, out_callees, callee_count) != OK)
        {
            return ERR;
        }
        return 0;
    }

    _ = queryNeighborNames(s.db, "SELECT DISTINCT n.name FROM edges e JOIN nodes n ON e.source_id = n.id " ++
        "WHERE e.target_id = ?1 AND e.type IN ('CALLS','HTTP_CALLS','ASYNC_CALLS') " ++
        "ORDER BY n.name LIMIT ?2", node_id, limit, out_callers, caller_count);

    _ = queryNeighborNames(s.db, "SELECT DISTINCT n.name FROM edges e JOIN nodes n ON e.target_id = n.id " ++
        "WHERE e.source_id = ?1 AND e.type IN ('CALLS','HTTP_CALLS','ASYNC_CALLS') " ++
        "ORDER BY n.name LIMIT ?2", node_id, limit, out_callees, callee_count);

    return 0;
}

fn countDegreesDirection(s: *Store, node_ids: []const i64, in_clause: []const u8, edge_type: ?CStr, inbound: bool, out_counts: []c_int) c_int {
    var sql: [ST_SQL_BUF]u8 = undefined;
    const id_col = if (inbound) "target_id" else "source_id";
    _ = interop.bprint(&sql, "SELECT {s}, COUNT(*) FROM edges WHERE {s} IN ({s}){s} GROUP BY {s}", .{
        id_col, id_col, in_clause, if (edge_type != null) " AND type = ?" else "", id_col,
    });

    var stmt: ?*Stmt = null;
    if (prepare(s.db, &sql, &stmt) != c.SQLITE_OK) return ERR;

    for (node_ids, 1..) |id, col| _ = c.sqlite3_bind_int64(stmt, @intCast(col), id);
    if (edge_type) |t| _ = bindText(stmt, @intCast(node_ids.len + 1), t);

    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const nid = c.sqlite3_column_int64(stmt, 0);
        if (std.mem.findScalar(i64, node_ids, nid)) |i| out_counts[i] = c.sqlite3_column_int(stmt, 1);
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);
    return OK;
}

pub export fn cbm_store_batch_count_degrees(s_c: ?*Store, node_ids_c: [*c]const i64, id_count: c_int, edge_type: CStr, out_in_c: [*c]c_int, out_out_c: [*c]c_int) c_int {
    const s = s_c orelse return ERR;
    if (node_ids_c == null or id_count <= 0 or out_in_c == null or out_out_c == null) return ERR;
    const node_ids = span(node_ids_c, id_count);
    const out_in = span(out_in_c, id_count);
    const out_out = span(out_out_c, id_count);

    @memset(out_in, 0);
    @memset(out_out, 0);

    if (s.zova_workspace_query) {
        for (node_ids, out_in, out_out) |node_id, *in_deg, *out_deg| {
            if (edge_type == null or streq(edge_type, "CALLS")) {
                cbm_store_node_degree(s, node_id, in_deg, out_deg);
                continue;
            }
            var edges: [*c]c.cbm_edge_t = null;
            var count: c_int = 0;
            if (storeZovaWorkspaceEdges(s, node_id, "inbound", edge_type, &edges, &count) != OK) return ERR;
            in_deg.* = count;
            cbm_store_free_edges(edges, count);
            edges = null;
            count = 0;
            if (storeZovaWorkspaceEdges(s, node_id, "outbound", edge_type, &edges, &count) != OK) return ERR;
            out_deg.* = count;
            cbm_store_free_edges(edges, count);
        }
        return OK;
    }

    // Build IN clause: (?,?,?)
    var in_clause: [c.CBM_SZ_4K]u8 = undefined;
    var pos: usize = 0;
    for (0..node_ids.len) |i| {
        if (pos >= in_clause.len - ST_IN_CLAUSE_MARGIN) break;
        if (i > 0) {
            in_clause[pos] = ',';
            pos += 1;
        }
        in_clause[pos] = '?';
        pos += 1;
    }

    const typed: ?CStr = if (edge_type != null and edge_type[0] != 0) edge_type else null;

    const rc = countDegreesDirection(s, node_ids, in_clause[0..pos], typed, true, out_in);
    if (rc != OK) return rc;

    return countDegreesDirection(s, node_ids, in_clause[0..pos], typed, false, out_out);
}

// ── UpsertFileHashBatch ──────────────────────────────────────────

pub export fn cbm_store_upsert_file_hash_batch(s: ?*Store, hashes: [*c]const c.cbm_file_hash_t, count: c_int) c_int {
    if (count == 0) return OK;

    var rc = cbm_store_begin(s);
    if (rc != OK) return rc;

    for (span(hashes, count)) |hash| {
        rc = cbm_store_upsert_file_hash(s, hash.project, hash.rel_path, hash.sha256, hash.mtime_ns, hash.size);
        if (rc != OK) {
            _ = cbm_store_rollback(s);
            return rc;
        }
    }

    return cbm_store_commit(s);
}

// ── FindEdgesByURLPath ───────────────────────────────────────────

pub export fn cbm_store_find_edges_by_url_path(s_c: ?*Store, project: CStr, keyword: CStr, out: *[*c]c.cbm_edge_t, count: *c_int) c_int {
    out.* = null;
    count.* = 0;
    const s = s_c orelse return ERR;

    // Search properties JSON for url_path containing keyword
    var like_pattern: [c.CBM_SZ_512]u8 = undefined;
    _ = interop.bprint(&like_pattern, "%\"url_path\":\"%{s}%\"%", .{interop.sp(keyword)});

    var stmt: ?*Stmt = null;
    if (prepare(s.db, edge_columns ++ "WHERE project = ?1 AND properties LIKE ?2", &stmt) != c.SQLITE_OK) {
        storeSetErrorSqlite(s, "url_path prepare");
        return ERR;
    }
    defer _ = c.sqlite3_finalize(stmt);

    _ = bindText(stmt, 1, project);
    _ = bindText(stmt, 2, &like_pattern);

    return collectEdges(s, stmt, ST_INIT_CAP_8, out, count);
}

// ── RestoreFrom ──────────────────────────────────────────────────

pub export fn cbm_store_restore_from(dst_c: ?*Store, src_c: ?*Store) c_int {
    const dst = dst_c orelse return ERR;
    const src = src_c orelse return ERR;
    const bk = c.sqlite3_backup_init(dst.db, "main", src.db, "main") orelse {
        storeSetErrorSqlite(dst, "backup init");
        return ERR;
    };
    const rc = c.sqlite3_backup_step(bk, -1); // copy all pages
    _ = c.sqlite3_backup_finish(bk);

    if (rc != c.SQLITE_DONE) {
        storeSetError(dst, "backup step failed");
        return ERR;
    }
    return OK;
}

// ── Search ───────────────────────────────────────────────────────

/// Convert a glob pattern to SQL LIKE pattern.
pub export fn cbm_glob_to_like(pattern_c: ?Str) [*c]u8 {
    const pattern = std.mem.span(pattern_c orelse return null);
    const out: [*]u8 = @as(?[*]u8, interop.mallocN(u8, pattern.len * ST_GROWTH + 1)) orelse return null;
    var j: usize = 0;

    var i: usize = 0;
    while (i < pattern.len) : (i += 1) {
        if (pattern[i] == '*' and i + 1 < pattern.len and pattern[i + 1] == '*') {
            // Remove leading / from output if present (handles glob dir-star)
            if (j > 0 and out[j - 1] == '/') j -= 1;
            out[j] = '%';
            j += 1;
            i += 1; // skip second *
            if (i + 1 < pattern.len and pattern[i + 1] == '/') i += 1; // skip trailing /
        } else {
            out[j] = switch (pattern[i]) {
                '*' => '%',
                '?' => '_',
                else => |ch| ch,
            };
            j += 1;
        }
    }
    out[j] = 0;
    return out;
}

// ── extractLikeHints ─────────────────────────────────────────────

pub export fn cbm_extract_like_hints(pattern_c: ?Str, out_c: ?[*][*c]u8, max_out: c_int) c_int {
    const pattern = std.mem.span(pattern_c orelse return 0);
    const out = out_c orelse return 0;
    if (max_out <= 0) return 0;

    // Bail on alternation — can't convert OR regex to AND LIKE
    if (std.mem.findScalar(u8, pattern, '|') != null) return 0;

    var count: c_int = 0;
    var buf: [c.CBM_SZ_256]u8 = undefined;
    var blen: usize = 0;

    var i: usize = 0;
    while (i < pattern.len) {
        const ch = pattern[i];
        switch (ch) {
            '\\' => {
                // Escaped char — the next char is literal
                if (i + 1 < pattern.len) {
                    if (blen < buf.len - 1) {
                        buf[blen] = pattern[i + 1];
                        blen += 1;
                    }
                    i += 2;
                } else {
                    i += 1;
                }
            },
            '.', '*', '+', '?', '^', '$', '(', ')', '[', ']', '{', '}' => {
                // Meta character — flush current literal segment
                if (blen >= ST_GLOB_MIN_LEN and count < max_out) {
                    buf[blen] = 0;
                    out[@intCast(count)] = c.strdup(&buf);
                    count += 1;
                }
                blen = 0;
                i += 1;
            },
            else => {
                if (blen < buf.len - 1) {
                    buf[blen] = ch;
                    blen += 1;
                }
                i += 1;
            },
        }
    }
    // Flush trailing segment
    if (blen >= ST_GLOB_MIN_LEN and count < max_out) {
        buf[blen] = 0;
        out[@intCast(count)] = c.strdup(&buf);
        count += 1;
    }
    return count;
}

// ── ensureCaseInsensitive / stripCaseFlag ────────────────────────

const case_flag = "(?i)";

fn hasCaseFlag(pattern: Str) bool {
    return c.strncmp(pattern, case_flag, case_flag.len) == 0;
}

pub export fn cbm_ensure_case_insensitive(pattern_c: ?Str) Str {
    const static = struct {
        var buf = std.mem.zeroes([c.CBM_SZ_2K:0]u8);
    };
    const buf: []u8 = &static.buf;
    const pattern = pattern_c orelse {
        buf[0] = 0;
        return &static.buf;
    };
    // Already has (?i) prefix? Return as-is.
    _ = interop.bprint(buf, "{s}{s}", .{ if (hasCaseFlag(pattern)) "" else case_flag, std.mem.span(pattern) });
    return &static.buf;
}

pub export fn cbm_strip_case_flag(pattern_c: ?Str) Str {
    const static = struct {
        var buf = std.mem.zeroes([c.CBM_SZ_2K:0]u8);
    };
    const buf: []u8 = &static.buf;
    const pattern = pattern_c orelse {
        buf[0] = 0;
        return &static.buf;
    };
    _ = interop.bprint(buf, "{s}", .{std.mem.span(if (hasCaseFlag(pattern)) pattern + case_flag.len else pattern)});
    return &static.buf;
}

/// Wrap a literal string in % for use as a LIKE pattern (%literal%).
fn makeLikeHint(literal: CStr) [*c]u8 {
    const text = std.mem.span(literal);
    const buf: [*c]u8 = interop.mallocN(u8, text.len + 3); // % + literal + % + NUL
    if (buf != null) {
        buf[0] = '%';
        @memcpy(buf[1..][0..text.len], text);
        buf[text.len + 1] = '%';
        buf[text.len + 2] = 0;
    }
    return buf;
}

/// The WHERE clause of a search while it is built: the clause text, the text
/// values to bind in placeholder order, and the malloc'd LIKE strings that
/// must outlive statement execution.
const SearchWhere = struct {
    text: [c.CBM_SZ_2K]u8 = undefined,
    len: usize = 0,
    nparams: c_int = 0,
    binds: [ST_SEARCH_MAX_BINDS]CStr = undefined,
    bind_count: usize = 0,
    /// Pool of malloc'd strings, freed in one shot after both statements are
    /// finalized.
    pool: [ST_LIKE_POOL_MAX][*c]u8 = undefined,
    pool_count: usize = 0,

    /// Takes ownership of ptr; false when the pool is full and ptr was freed.
    fn poolAdd(w: *SearchWhere, ptr: [*c]u8) bool {
        if (ptr != null and w.pool_count < ST_LIKE_POOL_MAX) {
            w.pool[w.pool_count] = ptr;
            w.pool_count += 1;
            return true;
        }
        c.free(ptr); // pool full — don't leak
        return false;
    }

    fn poolFree(w: *SearchWhere) void {
        for (w.pool[0..w.pool_count]) |ptr| c.free(ptr);
        w.pool_count = 0;
    }

    /// Append a WHERE clause fragment, joining with AND if not the first.
    fn append(w: *SearchWhere, comptime fmt: []const u8, args: anytype) void {
        if (w.nparams > 0) w.len = appendf(&w.text, w.len, " AND ", .{});
        w.len = appendf(&w.text, w.len, fmt, args);
        w.nparams += 1;
    }

    /// The placeholder number the next bound value gets.
    fn nextParam(w: *const SearchWhere) usize {
        return w.bind_count + 1;
    }

    /// Bind a text parameter and increment the bind index.
    fn bind(w: *SearchWhere, val: CStr) void {
        w.binds[w.bind_count] = val;
        w.bind_count += 1;
    }

    fn bindAll(w: *const SearchWhere, stmt: ?*Stmt) void {
        for (w.binds[0..w.bind_count], 1..) |val, col| _ = bindText(stmt, @intCast(col), val);
    }

    /// Append a regex WHERE clause for a column (case-sensitive or insensitive).
    fn addRegex(w: *SearchWhere, comptime column: []const u8, pattern: CStr, case_sensitive: bool) void {
        if (case_sensitive) {
            w.append(column ++ " REGEXP ?{d}", .{w.nextParam()});
        } else {
            w.append("iregexp(?{d}, " ++ column ++ ")", .{w.nextParam()});
        }
        w.bind(pattern);
    }

    /// Prepend LIKE pre-filter conditions for literal segments of a regex pattern.
    /// The idx_nodes_name index satisfies LIKE '%literal%', cutting the rows that
    /// reach the (more expensive) iregexp call to only those containing the literal.
    /// cbm_extract_like_hints bails on alternation, so no false negatives.
    fn addLikeHints(w: *SearchWhere, comptime column: []const u8, pattern: CStr) void {
        var hints: [ST_LIKE_HINT_MAX][*c]u8 = undefined;
        const nhints = cbm_extract_like_hints(pattern, &hints, ST_LIKE_HINT_MAX);
        for (hints[0..@intCast(nhints)]) |hint| {
            const lp = makeLikeHint(hint);
            c.free(hint);
            if (lp == null) continue;
            if (!w.poolAdd(lp)) continue; // lp was freed — skip bind
            w.append(column ++ " LIKE ?{d}", .{w.nextParam()});
            w.bind(lp);
        }
    }

    /// Build basic WHERE clauses: project, label, name, file, qn patterns.
    fn addBasic(w: *SearchWhere, params: *const c.cbm_search_params_t) void {
        if (params.project != null) {
            w.append("n.project = ?{d}", .{w.nextParam()});
            w.bind(params.project);
        }
        // Ignore an empty-string label: it is non-NULL but should behave like an
        // omitted label (no filter), matching the BM25 query path. Without the
        // label[0] guard, name_pattern/qn_pattern searches that pass label=""
        // append `n.label = ''`, which matches no node and silently returns
        // zero results (issue #481).
        if (params.label != null and params.label[0] != 0) {
            w.append("n.label = ?{d}", .{w.nextParam()});
            w.bind(params.label);
        }
        if (params.name_pattern != null) {
            w.addLikeHints("n.name", params.name_pattern);
            w.addRegex("n.name", params.name_pattern, params.case_sensitive);
        }
        if (params.qn_pattern != null) {
            w.addLikeHints("n.qualified_name", params.qn_pattern);
            w.addRegex("n.qualified_name", params.qn_pattern, params.case_sensitive);
        }
        if (params.file_pattern != null) {
            var lp = cbm_glob_to_like(params.file_pattern);
            // A file_pattern with no glob wildcards is treated as a path-substring
            // match (issue #200): file_pattern="offer-server" should match
            // "src/offer-server/x.js", not only a path equal to "offer-server".
            // Patterns that DO contain * / ? keep their explicit glob semantics.
            if (lp != null and c.strchr(params.file_pattern, '*') == null and c.strchr(params.file_pattern, '?') == null) {
                const contains = makeLikeHint(lp);
                if (contains != null) {
                    c.free(lp);
                    lp = contains;
                }
            }
            if (w.poolAdd(lp)) {
                w.append("n.file_path LIKE ?{d}", .{w.nextParam()});
                w.bind(lp);
            }
        }
    }

    /// Build advanced WHERE clauses: relationship, entry points, exclude labels.
    fn addAdvanced(w: *SearchWhere, params: *const c.cbm_search_params_t) void {
        if (params.relationship != null) {
            w.append("EXISTS(SELECT 1 FROM edges e WHERE " ++
                "(e.source_id = n.id OR e.target_id = n.id) AND e.type = ?{d})", .{w.nextParam()});
            w.bind(params.relationship);
        }
        if (params.exclude_entry_points) {
            // Exclude nodes with no inbound CALLS but at least one outbound CALLS.
            // Dead code (degree=0) is NOT excluded — only true entry points.
            w.append("NOT (NOT EXISTS(SELECT 1 FROM edges e WHERE e.target_id = n.id " ++
                "AND e.type = 'CALLS') " ++
                "AND EXISTS(SELECT 1 FROM edges e2 WHERE e2.source_id = n.id " ++
                "AND e2.type = 'CALLS'))", .{});
        }
        if (params.exclude_labels != null) {
            // Build exclude-labels NOT IN clause with bind placeholders.
            var clause: [c.CBM_SZ_512]u8 = undefined;
            var elen = appendf(&clause, 0, "n.label NOT IN (", .{});
            var i: usize = 0;
            while (params.exclude_labels[i] != null and w.bind_count < w.binds.len) : (i += 1) {
                if (i > 0) elen = appendf(&clause, elen, ",", .{});
                elen = appendf(&clause, elen, "?{d}", .{w.nextParam()});
                w.bind(params.exclude_labels[i]);
            }
            _ = appendf(&clause, elen, ")", .{});
            w.append("{s}", .{std.mem.sliceTo(&clause, 0)});
        }
    }
};

fn searchApplyDegreeFilter(sql: []u8, p: *const c.cbm_search_params_t) void {
    if (p.min_degree < 0 and p.max_degree < 0) return;
    var inner_sql: [c.CBM_SZ_4K]u8 = undefined;
    _ = interop.bprint(&inner_sql, "{s}", .{std.mem.sliceTo(sql, 0)});
    const inner = std.mem.sliceTo(&inner_sql, 0);
    if (p.min_degree >= 0 and p.max_degree >= 0) {
        _ = interop.bprint(sql, "SELECT * FROM ({s}) WHERE (in_deg + out_deg) >= {d} AND (in_deg + out_deg) <= {d}", .{ inner, p.min_degree, p.max_degree });
    } else if (p.min_degree >= 0) {
        _ = interop.bprint(sql, "SELECT * FROM ({s}) WHERE (in_deg + out_deg) >= {d}", .{ inner, p.min_degree });
    } else {
        _ = interop.bprint(sql, "SELECT * FROM ({s}) WHERE (in_deg + out_deg) <= {d}", .{ inner, p.max_degree });
    }
}

pub export fn cbm_store_search(s_c: ?*Store, params: *const c.cbm_search_params_t, out: *c.cbm_search_output_t) c_int {
    out.* = std.mem.zeroes(c.cbm_search_output_t);
    const s = s_c orelse return ERR;
    if (s.backend) |backend| return backend.search.?(s.backend_ctx, params, out);
    if (s.db == null) return ERR;
    if (usesRepository(s)) {
        const rc = c.cbm_zova_repository_search(s.zova_repository, &s.zova_graph_workspace_id, params, out);
        if (rc == OK) s.zova_native_topology_ops += 8;
        return rc;
    }

    var sql: [c.CBM_SZ_4K]u8 = undefined;
    var count_sql: [c.CBM_SZ_4K]u8 = undefined;

    const select_cols = "SELECT n.id, n.project, n.label, n.name, n.qualified_name, " ++
        "n.file_path, n.start_line, n.end_line, n.properties, " ++
        "(SELECT COUNT(*) FROM edges e WHERE e.target_id = n.id AND " ++
        "e.type IN ('CALLS', 'USAGE', 'CALL_REFERENCE', 'INHERITS', 'IMPLEMENTS')) " ++
        "AS in_deg, " ++
        "(SELECT COUNT(*) FROM edges e WHERE e.source_id = n.id AND " ++
        "e.type IN ('CALLS', 'USAGE', 'CALL_REFERENCE', 'INHERITS', 'IMPLEMENTS')) " ++
        "AS out_deg ";

    var where: SearchWhere = .{};
    where.text[0] = 0;
    where.addBasic(params);
    where.addAdvanced(params);
    defer where.poolFree();
    const where_text = where.text[0..where.len];

    // Build full SQL
    if (where.nparams > 0) {
        _ = interop.bprint(&sql, select_cols ++ " FROM nodes n WHERE {s}", .{where_text});
    } else {
        _ = interop.bprint(&sql, select_cols ++ " FROM nodes n", .{});
    }

    // Degree filters
    const has_degree_filter = params.min_degree >= 0 or params.max_degree >= 0;
    searchApplyDegreeFilter(&sql, params);

    // Count query — stripped of per-row edge subqueries for the common (no-degree-filter)
    // case, since we only need the row count, not in_deg/out_deg.  The degree-filter
    // case must wrap the full query because the filter references those columns.
    if (has_degree_filter) {
        _ = interop.bprint(&count_sql, "SELECT COUNT(*) FROM ({s})", .{std.mem.sliceTo(&sql, 0)});
    } else if (where.nparams > 0) {
        _ = interop.bprint(&count_sql, "SELECT COUNT(*) FROM nodes n WHERE {s}", .{where_text});
    } else {
        _ = interop.bprint(&count_sql, "SELECT COUNT(*) FROM nodes n", .{});
    }

    // Add ORDER BY + LIMIT
    const limit = if (params.limit > 0) params.limit else c.CBM_DEFAULT_SEARCH_LIMIT;
    const order_by: []const u8 = if (has_degree_filter) "name, qualified_name, id" else "n.name, n.qualified_name, n.id";
    _ = appendf(&sql, std.mem.sliceTo(&sql, 0).len, " ORDER BY {s} LIMIT {d} OFFSET {d}", .{ order_by, limit, params.offset });

    // Execute count query
    var cnt_stmt: ?*Stmt = null;
    if (prepare(s.db, &count_sql, &cnt_stmt) == c.SQLITE_OK) {
        where.bindAll(cnt_stmt);
        if (c.sqlite3_step(cnt_stmt) == c.SQLITE_ROW) out.total = c.sqlite3_column_int(cnt_stmt, 0);
        _ = c.sqlite3_finalize(cnt_stmt);
    }

    // Execute main query
    var main_stmt: ?*Stmt = null;
    if (prepare(s.db, &sql, &main_stmt) != c.SQLITE_OK) {
        storeSetErrorSqlite(s, "search prepare");
        return ERR;
    }

    where.bindAll(main_stmt);

    var results = Rows(c.cbm_search_result_t).init(ST_INIT_CAP_16);
    var rc = c.sqlite3_step(main_stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(main_stmt)) {
        const result = results.add();
        result.* = std.mem.zeroes(c.cbm_search_result_t);
        scanNode(main_stmt, &result.node);
        result.in_degree = c.sqlite3_column_int(main_stmt, 9);
        result.out_degree = c.sqlite3_column_int(main_stmt, 10);
    }
    out.results = results.items;
    out.count = results.len;
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(main_stmt);
        return ERR;
    }

    _ = c.sqlite3_finalize(main_stmt);
    return OK;
}

pub export fn cbm_store_search_free(out_c: ?*c.cbm_search_output_t) void {
    const out = out_c orelse return;
    for (span(out.results, out.count)) |*r| {
        cbm_node_free_fields(&r.node);
        for (span(r.connected_names, r.connected_count)) |*name| interop.strFree(name);
        c.free(@ptrCast(r.connected_names));
    }
    c.free(out.results);
    out.* = std.mem.zeroes(c.cbm_search_output_t);
}

// ── BFS Traversal ────────────────────────────────────────────────

const TraverseResult = c.cbm_traverse_result_t;
const NodeHop = c.cbm_node_hop_t;

/// Binds the edge types of a `?1,?2,…` clause; "CALLS" when none are given.
fn bfsBindEdgeTypes(stmt: ?*Stmt, edge_types: []const CStr) void {
    if (edge_types.len > 0) {
        for (edge_types, 1..) |edge_type, col| _ = bindText(stmt, @intCast(col), edge_type);
    } else {
        _ = bindText(stmt, 1, "CALLS");
    }
}

fn bfsCollectEdges(
    s: *Store,
    start_id: i64,
    visited: []const NodeHop,
    types_clause: []const u8,
    edge_types: []const CStr,
    out_edges: *[*c]c.cbm_edge_info_t,
    out_edge_count: *c_int,
) c_int {
    // Build ID set: root + all visited
    var id_set: [c.CBM_SZ_4K]u8 = undefined;
    var ilen = appendf(&id_set, 0, "{d}", .{start_id});
    for (visited) |*hop| ilen = appendf(&id_set, ilen, ",{d}", .{hop.node.id});
    const ids = id_set[0..ilen];

    var edge_sql: [ST_SQL_BUF]u8 = undefined;
    _ = interop.bprint(&edge_sql, "SELECT n1.name, n2.name, e.type, e.source_id, e.target_id, e.properties " ++
        "FROM edges e " ++
        "JOIN nodes n1 ON n1.id = e.source_id " ++
        "JOIN nodes n2 ON n2.id = e.target_id " ++
        "WHERE e.source_id IN ({s}) AND e.target_id IN ({s}) " ++
        "AND e.type IN ({s})", .{ ids, ids, types_clause });

    var estmt: ?*Stmt = null;
    if (prepare(s.db, &edge_sql, &estmt) != c.SQLITE_OK) {
        out_edges.* = null;
        out_edge_count.* = 0;
        return OK;
    }

    bfsBindEdgeTypes(estmt, edge_types);

    var edges = Rows(c.cbm_edge_info_t).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(estmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(estmt)) {
        edges.add().* = .{
            .from_name = columnDup(estmt, 0),
            .to_name = columnDup(estmt, 1),
            .type = columnDup(estmt, 2),
            .confidence = 1.0,
            .source_id = c.sqlite3_column_int64(estmt, 3),
            .target_id = c.sqlite3_column_int64(estmt, 4),
            .properties_json = columnDup(estmt, 5),
        };
    }
    out_edges.* = edges.items;
    out_edge_count.* = edges.len;
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(estmt);
        return ERR;
    }
    _ = c.sqlite3_finalize(estmt);
    return OK;
}

/// Build parameterized placeholder list "?1,?2,?3" for N edge types.
fn bfsBuildTypesClause(edge_type_count: c_int, buf: []u8) []const u8 {
    if (edge_type_count <= 0) return buf[0..appendf(buf, 0, "?1", .{})];
    var tlen: usize = 0;
    var i: c_int = 0;
    while (i < edge_type_count) : (i += 1) {
        if (i > 0) tlen = appendf(buf, tlen, ",", .{});
        tlen = appendf(buf, tlen, "?{d}", .{i + 1});
    }
    return buf[0..tlen];
}

fn storeFindCachedZovaWorkspaceNode(s_c: ?*Store, node_numeric_id: i64, out: *c.cbm_node_t) c_int {
    const s = s_c orelse return NOT_FOUND;
    if (!s.zova_workspace_query or
        !s.zova_graph_cached_stable_id_valid or
        s.zova_graph_cached_numeric_id != node_numeric_id)
    {
        return NOT_FOUND;
    }
    return c.cbm_zova_repository_find_node_by_numeric_id(s.zova_repository, &s.zova_graph_workspace_id, node_numeric_id, out);
}

fn storeZovaWorkspaceBfs(
    s: *Store,
    start_id: i64,
    direction: CStr,
    edge_types: []const CStr,
    max_depth: c_int,
    max_results: c_int,
    out: *TraverseResult,
) c_int {
    if (!s.zova_workspace_query or s.zova_graph_session == null or
        edge_types.len > 1 or
        (edge_types.len == 1 and !streq(edge_types[0], "CALLS")) or
        (direction != null and !streq(direction, "outbound") and !streq(direction, "inbound")))
    {
        return NOT_FOUND;
    }
    var stable_copy: [c.CBM_ZOVA_WORKSPACE_ID_MAX + 96]u8 = undefined;
    stable_copy[0] = 0;
    if (s.zova_graph_cached_stable_id_valid and s.zova_graph_cached_numeric_id == start_id) {
        _ = interop.bprint(&stable_copy, "{s}", .{std.mem.sliceTo(&s.zova_graph_cached_stable_id, 0)});
    } else {
        const stable_rc = c.cbm_zova_repository_stable_id_for_numeric_id(s.zova_repository, &s.zova_graph_workspace_id, start_id, &stable_copy, stable_copy.len);
        if (stable_rc != OK) return stable_rc;
        if (stable_copy[0] != 0) {
            s.zova_graph_cached_numeric_id = start_id;
            _ = interop.bprint(&s.zova_graph_cached_stable_id, "{s}", .{std.mem.sliceTo(&stable_copy, 0)});
            s.zova_graph_cached_stable_id_valid = true;
        }
    }
    if (stable_copy[0] == 0) return NOT_FOUND;
    var graph_name: [c.CBM_ZOVA_WORKSPACE_ID_MAX + 32]u8 = undefined;
    if (c.cbm_zova_workspace_graph_name(&s.zova_graph_workspace_id, &graph_name, graph_name.len) != 0) return ERR;
    var visits: [*c]c.cbm_zova_graph_visit_t = null;
    var count: c_int = 0;
    var metrics = std.mem.zeroes(c.cbm_zova_graph_metrics_t);
    if (c.cbm_zova_graph_session_walk_calls(
        s.zova_graph_session,
        &s.zova_graph_workspace_id,
        &graph_name,
        &stable_copy,
        if (direction != null) direction else "outbound",
        max_depth,
        max_results,
        &visits,
        &count,
        &metrics,
    ) != 0) return ERR;
    var items: [*c]NodeHop = null;
    if (count != 0) {
        items = interop.callocN(NodeHop, @intCast(count));
        if (items == null) {
            c.cbm_zova_graph_visits_free(visits, count);
            return ERR;
        }
    }
    for (span(items, count), span(visits, count)) |*item, *visit| {
        item.node.id = storeStableNumericId(visit.node_id);
        item.node.project = interop.heapStrdup(safeStr(out.root.project));
        item.node.label = visit.label;
        visit.label = null;
        item.node.name = visit.name;
        visit.name = null;
        item.node.qualified_name = visit.qualified_name;
        visit.qualified_name = null;
        item.node.file_path = visit.file_path;
        visit.file_path = null;
        item.node.start_line = visit.start_line;
        item.node.end_line = visit.end_line;
        item.node.properties_json = visit.properties;
        visit.properties = null;
        item.hop = visit.hop;
    }
    c.cbm_zova_graph_visits_free(visits, count);
    out.visited = items;
    out.visited_count = count;
    s.zova_native_topology_ops += 1;
    return OK;
}

/// The native workspace walk follows CALLS only. Every other edge-type set
/// (CALL_REFERENCE, HTTP_CALLS, mixed sets) walks level by level over the
/// workspace edge lookup with the SQL route's hop ordering and limit, and keeps
/// the traversed edges so callers can read their properties.
fn bfsEdgeTypeSelected(edge_type: CStr, edge_types: []const CStr) bool {
    if (edge_type == null) return false;
    if (edge_types.len == 0) return streq(edge_type, "CALLS");
    for (edge_types) |selected| {
        if (selected != null and streq(edge_type, selected)) return true;
    }
    return false;
}

fn bfsHopIndex(hops: []const NodeHop, id: i64) ?usize {
    for (hops, 0..) |*hop, i| {
        if (hop.node.id == id) return i;
    }
    return null;
}

fn bfsHopOrder(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x = &@as(*const NodeHop, @ptrCast(@alignCast(a))).node;
    const y = &@as(*const NodeHop, @ptrCast(@alignCast(b))).node;
    var cmp = c.strcmp(safeStr(x.file_path), safeStr(y.file_path));
    if (cmp == 0) cmp = c.strcmp(safeStr(x.qualified_name), safeStr(y.qualified_name));
    if (cmp == 0) cmp = c.strcmp(safeStr(x.label), safeStr(y.label));
    if (cmp != 0) return cmp;
    if (x.start_line != y.start_line) return if (x.start_line < y.start_line) -1 else 1;
    if (x.end_line != y.end_line) return if (x.end_line < y.end_line) -1 else 1;
    return order(x.id, y.id);
}

fn storeZovaWorkspaceBfsTyped(
    s: *Store,
    start_id: i64,
    direction: CStr,
    edge_types: []const CStr,
    max_depth: c_int,
    max_results: c_int,
    out: *TraverseResult,
) c_int {
    const inbound = direction != null and streq(direction, "inbound");
    var hops = Rows(NodeHop).lazy;
    var seen = Rows(c.cbm_edge_t).lazy;
    var level_start: c_int = 0;
    var level_end: c_int = 0; // hops[level_start, level_end) is the frontier
    var rc: c_int = OK;
    var depth: c_int = 0;
    while (depth < max_depth and rc == OK) : (depth += 1) {
        const frontier = if (depth == 0) 1 else level_end - level_start;
        const next_start = hops.len;
        var f: c_int = 0;
        while (f < frontier and rc == OK) : (f += 1) {
            const from = if (depth == 0) start_id else hops.at(level_start + f).node.id;
            var edges: [*c]c.cbm_edge_t = null;
            var edge_count: c_int = 0;
            rc = storeZovaWorkspaceEdges(s, from, if (inbound) "inbound" else "outbound", null, &edges, &edge_count);
            for (span(edges, edge_count)) |*edge| {
                if (rc != OK) break;
                if (!bfsEdgeTypeSelected(edge.type, edge_types)) continue;
                const next = if (inbound) edge.source_id else edge.target_id;
                seen.add().* = edge.*;
                edge.* = std.mem.zeroes(c.cbm_edge_t);
                if (next == start_id or bfsHopIndex(hops.slice(), next) != null) continue;
                const hop = hops.add();
                hop.* = std.mem.zeroes(NodeHop);
                rc = cbm_store_find_node_by_id(s, next, &hop.node);
                if (rc == OK) {
                    hop.hop = depth + 1;
                } else {
                    hops.len -= 1;
                    if (rc == NOT_FOUND) rc = OK;
                }
            }
            cbm_store_free_edges(edges, edge_count);
        }
        if (hops.len - next_start > 1) { // hops stays NULL until the first hop
            c.qsort(hops.at(next_start), @intCast(hops.len - next_start), @sizeOf(NodeHop), bfsHopOrder);
        }
        while (hops.len > max_results and hops.len > 0) {
            hops.len -= 1;
            cbm_node_free_fields(&hops.at(hops.len).node);
        }
        level_start = next_start;
        level_end = hops.len;
        if (level_start == level_end) break;
    }
    // Keep the edges whose endpoints both survived, as the SQL route does.
    const infos: [*c]c.cbm_edge_info_t = if (seen.len != 0) interop.callocN(c.cbm_edge_info_t, @intCast(seen.len)) else null;
    var info_count: c_int = 0;
    if (infos != null) {
        const kept = hops.slice();
        for (seen.slice()) |*edge| {
            const si = bfsHopIndex(kept, edge.source_id);
            const ti = bfsHopIndex(kept, edge.target_id);
            if ((si == null and edge.source_id != start_id) or
                (ti == null and edge.target_id != start_id)) continue;
            const info = &infos[0..@intCast(seen.len)][@intCast(info_count)];
            info_count += 1;
            info.from_name = interop.heapStrdup(if (si) |i| kept[i].node.name else out.root.name);
            info.to_name = interop.heapStrdup(if (ti) |i| kept[i].node.name else out.root.name);
            info.type = edge.type;
            edge.type = null;
            info.confidence = 1.0;
            info.source_id = edge.source_id;
            info.target_id = edge.target_id;
            info.properties_json = edge.properties_json;
            edge.properties_json = null;
        }
    }
    cbm_store_free_edges(seen.items, seen.len);
    out.visited = hops.items;
    out.visited_count = hops.len;
    out.edges = infos;
    out.edge_count = info_count;
    return rc;
}

fn storeBfs(
    s_c: ?*Store,
    start_id: i64,
    direction: CStr,
    edge_types_c: [*c]CStr,
    edge_type_count: c_int,
    max_depth: c_int,
    max_results: c_int,
    need_edges: bool,
    out: *TraverseResult,
) c_int {
    out.* = std.mem.zeroes(TraverseResult);
    if (s_c) |s| {
        if (s.backend) |backend| {
            return backend.bfs.?(s.backend_ctx, start_id, direction, edge_types_c, edge_type_count, max_depth, max_results, out);
        }
    }

    var root = std.mem.zeroes(c.cbm_node_t);
    var rc = storeFindCachedZovaWorkspaceNode(s_c, start_id, &root);
    if (rc == NOT_FOUND) rc = cbm_store_find_node_by_id(s_c, start_id, &root);
    if (rc != OK) return rc;
    const s = s_c orelse return ERR; // a NULL store has no root node
    out.root = root;
    const edge_types = span(edge_types_c, edge_type_count);

    // The native walk returns nodes only; a caller that reads edge
    // properties takes the typed walk instead.
    rc = if (need_edges)
        NOT_FOUND
    else
        storeZovaWorkspaceBfs(s, start_id, direction, edge_types, max_depth, max_results, out);
    if (rc == OK) return rc;
    if (s.zova_workspace_query and rc == NOT_FOUND) {
        return storeZovaWorkspaceBfsTyped(s, start_id, direction, edge_types, max_depth, max_results, out);
    }
    if (s.zova_workspace_query) return rc;

    var types_clause_buf: [c.CBM_SZ_512]u8 = undefined;
    const types_clause = bfsBuildTypesClause(edge_type_count, &types_clause_buf);

    // Build recursive CTE for BFS
    var sql: [c.CBM_SZ_4K]u8 = undefined;
    const is_inbound = direction != null and streq(direction, "inbound");
    const join_cond = if (is_inbound) "e.target_id = bfs.node_id" else "e.source_id = bfs.node_id";
    const next_id = if (is_inbound) "e.source_id" else "e.target_id";

    _ = interop.bprint(&sql, "WITH RECURSIVE bfs(node_id, hop) AS (" ++
        "  SELECT {d}, 0" ++
        "  UNION" ++
        "  SELECT {s}, bfs.hop + 1" ++
        "  FROM bfs" ++
        "  JOIN edges e ON {s}" ++
        "  WHERE e.type IN ({s}) AND bfs.hop < {d}" ++
        "), shortest AS (" ++
        "  SELECT node_id, MIN(hop) AS hop FROM bfs " ++
        "  WHERE hop > 0 AND node_id != {d} GROUP BY node_id" ++
        ")" ++
        "SELECT DISTINCT n.id, n.project, n.label, n.name, n.qualified_name, " ++
        "n.file_path, n.start_line, n.end_line, n.properties, shortest.hop " ++
        "FROM shortest " ++
        "JOIN nodes n ON n.id = shortest.node_id " ++
        "ORDER BY shortest.hop, n.file_path, n.qualified_name, n.label, n.start_line, " ++
        "n.end_line, n.id " ++
        "LIMIT {d};", .{ start_id, next_id, join_cond, types_clause, max_depth, start_id, max_results });

    var stmt: ?*Stmt = null;
    if (prepare(s.db, &sql, &stmt) != c.SQLITE_OK) {
        storeSetErrorSqlite(s, "bfs prepare");
        return ERR;
    }

    // Bind edge type parameters
    bfsBindEdgeTypes(stmt, edge_types);

    var visited = Rows(NodeHop).init(ST_INIT_CAP_16);
    rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const hop = visited.add();
        scanNode(stmt, &hop.node);
        hop.hop = c.sqlite3_column_int(stmt, 9);
    }
    out.visited = visited.items;
    out.visited_count = visited.len;
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        return ERR;
    }

    _ = c.sqlite3_finalize(stmt);

    // Collect edges between visited nodes (including root)
    if (visited.len > 0) {
        _ = bfsCollectEdges(s, start_id, visited.slice(), types_clause, edge_types, &out.edges, &out.edge_count);
    } else {
        out.edges = null;
        out.edge_count = 0;
    }

    return OK;
}

pub export fn cbm_store_bfs(
    s: ?*Store,
    start_id: i64,
    direction: CStr,
    edge_types: [*c]CStr,
    edge_type_count: c_int,
    max_depth: c_int,
    max_results: c_int,
    out: *TraverseResult,
) c_int {
    return storeBfs(s, start_id, direction, edge_types, edge_type_count, max_depth, max_results, false, out);
}

pub export fn cbm_store_bfs_with_edges(
    s: ?*Store,
    start_id: i64,
    direction: CStr,
    edge_types: [*c]CStr,
    edge_type_count: c_int,
    max_depth: c_int,
    max_results: c_int,
    out: *TraverseResult,
) c_int {
    return storeBfs(s, start_id, direction, edge_types, edge_type_count, max_depth, max_results, true, out);
}

// ── Relationship-trail walk for Cypher variable-length paths ──────
//
// cbm_store_bfs reports each node once, at its shortest distance. A Cypher
// pattern such as [:CALLS*2..2] asks for paths of length 2, which may reach a
// node whose shortest distance is 1, and must not reuse a relationship (a
// self-loop cannot pad a path). So this walk reports every (node, hop) pair
// that some edge-distinct path reaches.
//
// Path enumeration is exponential, so the walk has a row budget: every node
// entered on a path counts one row, the start included. It goes deepest first
// and, among siblings, lower node id first, so a wide first hop cannot use up
// the budget before a deeper match is seen (upstream 0c5ffa50, 0d97729c,
// cd22487c). It reads edges through cbm_store_find_edges_by_{source,target},
// so the SQLite, Zova workspace and query-backend stores share one semantics.
const ST_TRAIL_ROW_MULTIPLIER = 8;
const ST_TRAIL_MAX_ROWS = 4096;

fn trailRowLimit(max_results: c_int, max_depth: c_int) c_int {
    const base = @as(i64, if (max_results > 0) max_results else ST_INIT_CAP_16) * ST_TRAIL_ROW_MULTIPLIER + 1;
    const rows = base *| @as(i64, if (max_depth > 0) max_depth else ST_INIT_CAP_16);
    return if (rows > ST_TRAIL_MAX_ROWS) ST_TRAIL_MAX_ROWS else @intCast(rows);
}

const TrailAdj = struct {
    node: i64,
    next: [*c]i64 = null, // selected neighbours, lower id first
    count: c_int = 0,
};

const TrailFrame = struct {
    adj: c_int, // index into the adjacency cache
    cursor: c_int, // next neighbour to try; the edge taken to reach the child is cursor - 1
};

fn trailNextOrder(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x = @as(*const i64, @ptrCast(@alignCast(a))).*;
    const y = @as(*const i64, @ptrCast(@alignCast(b))).*;
    return order(x, y);
}

/// Index of node's adjacency in the cache, loading it on first use.
fn trailAdjacency(
    s: ?*Store,
    cache: *Rows(TrailAdj),
    node: i64,
    inbound: bool,
    edge_types: []const CStr,
    rc: *c_int,
) c_int {
    for (cache.slice(), 0..) |*cached, i| {
        if (cached.node == node) return @intCast(i);
    }
    var edges: [*c]c.cbm_edge_t = null;
    var edge_count: c_int = 0;
    rc.* = if (inbound)
        cbm_store_find_edges_by_target(s, node, &edges, &edge_count)
    else
        cbm_store_find_edges_by_source(s, node, &edges, &edge_count);
    if (rc.* == NOT_FOUND) rc.* = OK;
    if (rc.* != OK) return c.CBM_NOT_FOUND;
    var adj: TrailAdj = .{ .node = node };
    if (edge_count > 0) adj.next = interop.mallocN(i64, @intCast(edge_count));
    if (adj.next != null) {
        for (span(edges, edge_count)) |*edge| {
            if (!bfsEdgeTypeSelected(edge.type, edge_types)) continue;
            adj.next[@intCast(adj.count)] = if (inbound) edge.source_id else edge.target_id;
            adj.count += 1;
        }
    }
    cbm_store_free_edges(edges, edge_count);
    // glibc declares qsort's base nonnull: an empty adjacency has next == NULL.
    if (adj.count > 1) c.qsort(adj.next, @intCast(adj.count), @sizeOf(i64), trailNextOrder);
    cache.add().* = adj;
    return cache.len - 1;
}

/// True when the edge (adjacency a, neighbour slot k) is already on the path.
fn trailUsesEdge(path: []const TrailFrame, a: c_int, k: c_int) bool {
    for (path) |frame| {
        if (frame.adj == a and frame.cursor - 1 == k) return true;
    }
    return false;
}

fn trailHopOrder(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x: *const NodeHop = @ptrCast(@alignCast(a));
    const y: *const NodeHop = @ptrCast(@alignCast(b));
    if (x.hop != y.hop) return if (x.hop < y.hop) -1 else 1;
    return bfsHopOrder(a, b);
}

pub export fn cbm_store_bfs_trail(
    s: ?*Store,
    start_id: i64,
    direction: CStr,
    edge_types_c: [*c]CStr,
    edge_type_count: c_int,
    max_depth: c_int,
    max_results_c: c_int,
    out: *TraverseResult,
) c_int {
    out.* = std.mem.zeroes(TraverseResult);
    var rc = cbm_store_find_node_by_id(s, start_id, &out.root);
    if (rc != OK) return rc;
    const max_results = @max(max_results_c, 0);
    const inbound = direction != null and streq(direction, "inbound");
    const row_limit = trailRowLimit(max_results, max_depth);
    const edge_types = span(edge_types_c, edge_type_count);

    var cache = Rows(TrailAdj).lazy;
    const stack_len = @as(usize, @intCast(@max(max_depth, 0))) + 1;
    const stack_ptr: ?[*]TrailFrame = interop.mallocN(TrailFrame, stack_len);
    const stack = (stack_ptr orelse return ERR)[0..stack_len];
    var hops = Rows(NodeHop).lazy; // distinct (node, hop) pairs; node holds only the id here
    var rows: c_int = 1; // the start node
    var depth: c_int = 0;
    stack[0] = .{ .adj = trailAdjacency(s, &cache, start_id, inbound, edge_types, &rc), .cursor = 0 };
    while (rc == OK and depth >= 0 and stack[0].adj >= 0) {
        const f = &stack[@intCast(depth)];
        const adj = cache.at(f.adj);
        if (depth >= max_depth or f.cursor >= adj.count) {
            depth -= 1;
            continue;
        }
        const k = f.cursor;
        f.cursor += 1;
        if (trailUsesEdge(stack[0..@intCast(depth)], f.adj, k)) continue;
        if (rows >= row_limit) {
            out.truncated = true;
            break;
        }
        rows += 1;
        const next = adj.next[@intCast(k)];
        var seen = false;
        for (hops.slice()) |*hop| {
            seen = hop.node.id == next and hop.hop == depth + 1;
            if (seen) break;
        }
        if (!seen) {
            const hop = hops.add();
            hop.* = std.mem.zeroes(NodeHop);
            hop.node.id = next;
            hop.hop = depth + 1;
        }
        if (depth + 1 < max_depth) {
            const child = trailAdjacency(s, &cache, next, inbound, edge_types, &rc);
            if (child >= 0) {
                depth += 1;
                stack[@intCast(depth)] = .{ .adj = child, .cursor = 0 };
            }
        }
    }
    for (cache.slice()) |*cached| c.free(cached.next);
    c.free(cache.items);
    c.free(stack.ptr);
    if (out.truncated) {
        var limit_buf: [ST_BUF_16]u8 = undefined;
        _ = interop.bprint(&limit_buf, "{d}", .{row_limit});
        interop.logWarn("cypher.trail_truncated", .{ "rows", @as(CStr, &limit_buf), "result", "partial" });
    }

    // Materialize each pair; a node reached at several hops is read once.
    var kept: usize = 0;
    if (rc == OK) {
        const pairs = hops.slice();
        for (pairs) |*pair| {
            const id = pair.node.id;
            const prior = bfsHopIndex(pairs[0..kept], id);
            var node = std.mem.zeroes(c.cbm_node_t);
            const nrc = if (prior != null) OK else cbm_store_find_node_by_id(s, id, &node);
            if (nrc == NOT_FOUND) continue;
            if (nrc != OK) {
                rc = nrc;
                break;
            }
            if (prior) |index| {
                const p = &pairs[index].node;
                node = p.*;
                node.project = interop.heapStrdup(p.project);
                node.label = interop.heapStrdup(p.label);
                node.name = interop.heapStrdup(p.name);
                node.qualified_name = interop.heapStrdup(p.qualified_name);
                node.file_path = interop.heapStrdup(p.file_path);
                node.properties_json = interop.heapStrdup(p.properties_json);
            }
            pairs[kept] = .{ .node = node, .hop = pair.hop };
            kept += 1;
        }
    }
    hops.len = @intCast(kept);
    if (hops.len > 0) c.qsort(hops.items, @intCast(hops.len), @sizeOf(NodeHop), trailHopOrder);
    if (hops.len > max_results) {
        while (hops.len > max_results) {
            hops.len -= 1;
            cbm_node_free_fields(&hops.at(hops.len).node);
        }
        out.truncated = true;
    }
    out.visited = hops.items;
    out.visited_count = hops.len;
    return rc;
}

pub export fn cbm_store_traverse_free(out_c: ?*TraverseResult) void {
    const out = out_c orelse return;
    // Free root
    cbm_node_free_fields(&out.root);

    // Free visited
    for (span(out.visited, out.visited_count)) |*h| cbm_node_free_fields(&h.node);
    c.free(out.visited);

    // Free edges
    for (span(out.edges, out.edge_count)) |*edge| {
        interop.strFree(&edge.from_name);
        interop.strFree(&edge.to_name);
        interop.strFree(&edge.type);
        interop.strFree(&edge.properties_json);
    }
    c.free(out.edges);

    out.* = std.mem.zeroes(TraverseResult);
}

// ── Impact analysis ──────────────────────────────────────────────

const Risk = c.cbm_risk_level_t;

pub export fn cbm_hop_to_risk(hop: c_int) Risk {
    return switch (hop) {
        1 => c.CBM_RISK_CRITICAL,
        2 => c.CBM_RISK_HIGH,
        3 => c.CBM_RISK_MEDIUM,
        else => c.CBM_RISK_LOW,
    };
}

pub export fn cbm_risk_label(level: Risk) CStr {
    return switch (level) {
        c.CBM_RISK_CRITICAL => "CRITICAL",
        c.CBM_RISK_HIGH => "HIGH",
        c.CBM_RISK_MEDIUM => "MEDIUM",
        else => "LOW",
    };
}

pub export fn cbm_build_impact_summary(
    hops: [*c]const NodeHop,
    hop_count: c_int,
    edges: [*c]const c.cbm_edge_info_t,
    edge_count: c_int,
) c.cbm_impact_summary_t {
    var s = std.mem.zeroes(c.cbm_impact_summary_t);
    for (span(hops, hop_count)) |*hop| {
        switch (cbm_hop_to_risk(hop.hop)) {
            c.CBM_RISK_CRITICAL => s.critical += 1,
            c.CBM_RISK_HIGH => s.high += 1,
            c.CBM_RISK_MEDIUM => s.medium += 1,
            else => s.low += 1,
        }
        s.total += 1;
    }
    for (span(edges, edge_count)) |*edge| {
        if (edge.type != null and (streq(edge.type, "HTTP_CALLS") or streq(edge.type, "ASYNC_CALLS"))) {
            s.has_cross_service = true;
            break;
        }
    }
    return s;
}

pub export fn cbm_deduplicate_hops(hops_c: [*c]const NodeHop, hop_count: c_int, out: *[*c]NodeHop, out_count: *c_int) c_int {
    out.* = null;
    out_count.* = 0;
    const hops = span(hops_c, hop_count);
    if (hops.len == 0) return OK;

    // Simple O(n²) dedup — keep minimum hop per node ID
    const result_ptr: ?[*]NodeHop = interop.mallocN(NodeHop, hops.len);
    const result = (result_ptr orelse return ERR)[0..hops.len];
    var n: usize = 0;

    for (hops) |*hop| {
        if (bfsHopIndex(result[0..n], hop.node.id)) |found| {
            if (hop.hop < result[found].hop) result[found].hop = hop.hop;
        } else {
            result[n] = hop.*;
            n += 1;
        }
    }

    out.* = interop.safeReallocN(NodeHop, result.ptr, n);
    out_count.* = @intCast(n);
    return OK;
}

// ── Schema ───────────────────────────────────────────────────────

const SCHEMA_MAX_JSON_KEYS = 50;

const SchemaInfo = c.cbm_schema_info_t;

/// A one-shot statement; null when prepare fails or the SQL holds no statement.
fn prepareOne(db: ?*c.sqlite3, sql: CStr) ?*Stmt {
    var stmt: ?*Stmt = null;
    if (prepare(db, sql, &stmt) != c.SQLITE_OK) {
        if (stmt != null) _ = c.sqlite3_finalize(stmt);
        return null;
    }
    return stmt;
}

/// Discover distinct JSON property keys for a table/column via json_each().
/// Prepends base_cols, then appends up to SCHEMA_MAX_JSON_KEYS from the query.
/// Caller must free the returned array and each string in it.
fn schemaDiscoverProps(
    db: ?*c.sqlite3,
    sql: CStr,
    project: CStr,
    filter: CStr,
    base_cols: []const CStr,
    out_props: *[*c][*c]u8,
    out_count: *c_int,
) void {
    const pcap = base_cols.len + SCHEMA_MAX_JSON_KEYS;
    const props: [*c][*c]u8 = interop.mallocN([*c]u8, pcap);
    var pn: usize = 0;

    for (base_cols) |col| {
        props[pn] = interop.heapStrdup(col);
        pn += 1;
    }

    var pstmt: ?*Stmt = null;
    if (prepare(db, sql, &pstmt) == c.SQLITE_OK) {
        _ = bindText(pstmt, 1, project);
        _ = bindText(pstmt, 2, filter);
        while (c.sqlite3_step(pstmt) == c.SQLITE_ROW and pn < pcap) {
            props[pn] = columnDup(pstmt, 0);
            pn += 1;
        }
        _ = c.sqlite3_finalize(pstmt);
    }

    out_props.* = props;
    out_count.* = @intCast(pn);
}

// Path scoping for architecture / schema (shared).
const arch_blank = " \t\n\r";

fn archPathIsSet(path: CStr) bool {
    if (path == null) return false;
    return std.mem.trimStart(u8, std.mem.span(path), arch_blank).len != 0;
}

/// Normalizes a path scope into norm_out and writes its `norm/%` LIKE pattern
/// into like_out. False when the path names no scope.
fn archPathPrepare(path_c: CStr, norm_out: []u8, like_out: []u8) bool {
    if (!archPathIsSet(path_c) or norm_out.len == 0) return false;
    var path = std.mem.trimStart(u8, std.mem.span(path_c), arch_blank);
    if (std.mem.startsWith(u8, path, "./")) path = path[2..];
    path = std.mem.trimStart(u8, path, "/");
    if (path.len == 0) return false;
    var len = @min(path.len, norm_out.len - 1);
    @memcpy(norm_out[0..len], path[0..len]);
    while (len > 0 and (norm_out[len - 1] == ' ' or norm_out[len - 1] == '\t' or norm_out[len - 1] == '/')) {
        len -= 1;
    }
    // Collapse duplicate slashes
    var w: usize = 0;
    for (norm_out[0..len]) |ch| {
        if (ch == '/' and w > 0 and norm_out[w - 1] == '/') continue;
        norm_out[w] = ch;
        w += 1;
    }
    norm_out[w] = 0;
    if (w == 0) return false;
    _ = interop.bprint(like_out, "{s}/%", .{norm_out[0..w]});
    return true;
}

const arch_path_scope_sql = " AND (file_path = ? OR file_path LIKE ?)";

fn archBindPathScope(stmt: ?*Stmt, exact_idx: c_int, like_idx: c_int, norm: CStr, like_pat: CStr) void {
    _ = bindText(stmt, exact_idx, norm);
    _ = bindText(stmt, like_idx, like_pat);
}

pub export fn cbm_store_arch_path_scoped(path: CStr) bool {
    var norm: [c.CBM_SZ_512]u8 = undefined;
    var like: [c.CBM_SZ_512 + 4]u8 = undefined;
    return archPathPrepare(path, &norm, &like);
}

pub export fn cbm_store_normalize_arch_path(path: CStr, norm_out: [*c]u8, norm_sz: usize) bool {
    if (norm_out == null) return false;
    var like: [c.CBM_SZ_512 + 4]u8 = undefined;
    return archPathPrepare(path, norm_out[0..norm_sz], &like);
}

/// Ids of the nodes view rows under a prepared path scope, sorted ascending.
fn storeScopeNodeIds(s: *Store, project: CStr, norm: CStr, like: CStr, out: *[*c]i64, out_count: *c_int) c_int {
    out.* = null;
    out_count.* = 0;
    var stmt: ?*Stmt = null;
    if (prepare(s.db, "SELECT id FROM nodes WHERE project = ?1 " ++
        "AND (file_path = ?2 OR file_path LIKE ?3) ORDER BY id;", &stmt) != c.SQLITE_OK)
    {
        storeSetErrorSqlite(s, "scope_node_ids");
        return ERR;
    }
    _ = bindText(stmt, 1, project);
    archBindPathScope(stmt, 2, 3, norm, like);
    var cap: usize = 0;
    var n: usize = 0;
    var ids: [*c]i64 = null;
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        if (n == cap) {
            cap = if (cap != 0) cap * ST_GROWTH else c.CBM_SZ_256;
            const grown = interop.reallocN(i64, ids, cap);
            if (grown == null) {
                rc = c.SQLITE_NOMEM;
                break;
            }
            ids = grown;
        }
        ids[n] = c.sqlite3_column_int64(stmt, 0);
        n += 1;
    }
    _ = c.sqlite3_finalize(stmt);
    if (rc != c.SQLITE_DONE) {
        c.free(ids);
        storeSetError(s, "scope_node_ids");
        return ERR;
    }
    out.* = ids;
    out_count.* = @intCast(n);
    return OK;
}

fn storeTypeCountCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left: *const c.cbm_type_count_t = @ptrCast(@alignCast(left_ptr));
    const right: *const c.cbm_type_count_t = @ptrCast(@alignCast(right_ptr));
    if (left.count != right.count) return if (left.count > right.count) -1 else 1;
    return c.strcmp(left.type, right.type);
}

/// Edge type counts of a Zova workspace store, most frequent first. With
/// scoped, only edges whose two ends are in the sorted scope ids count.
fn storeZovaEdgeTypeCounts(
    s: *Store,
    scoped: bool,
    scope: []const i64,
    out: *[*c]c.cbm_type_count_t,
    out_count: *c_int,
) c_int {
    out.* = null;
    out_count.* = 0;
    if (storeZovaEdges(s) != OK) return ERR;
    const types = s.zovaEdgeTypes();
    const counts_ptr: [*c]c_int = interop.callocN(c_int, types.len + 1);
    const arr_ptr: [*c]c.cbm_type_count_t = interop.callocN(c.cbm_type_count_t, types.len + 1);
    if (counts_ptr == null or arr_ptr == null) {
        c.free(counts_ptr);
        c.free(arr_ptr);
        return ERR;
    }
    const counts = counts_ptr[0 .. types.len + 1];
    const arr = arr_ptr[0 .. types.len + 1];
    for (s.zovaEdges()) |*e| {
        if (!scoped or (storeIdIn(scope, e.source_key) and storeIdIn(scope, e.target_key))) {
            counts[@intCast(e.type)] += 1;
        }
    }
    var n: usize = 0;
    for (types, counts[0..types.len]) |name, count| {
        if (count == 0) continue;
        arr[n].type = interop.heapStrdup(name);
        arr[n].count = count;
        if (arr[n].type == null) {
            for (arr[0..n]) |*item| interop.strFree(&item.type);
            c.free(counts_ptr);
            c.free(arr_ptr);
            return ERR;
        }
        n += 1;
    }
    c.free(counts_ptr);
    c.qsort(arr_ptr, n, @sizeOf(c.cbm_type_count_t), storeTypeCountCompare);
    out.* = arr_ptr;
    out_count.* = @intCast(n);
    return OK;
}

const StoreEdgeKeys = struct {
    schema: *SchemaInfo,
    keys: [*c][*c][*c]u8,
    key_counts: [*c]c_int,
    key_caps: [*c]c_int,
    failed: bool = false,
};

fn storeEdgeKeysVisit(
    source_key: i64,
    target_key: i64,
    type_c: [*c]const u8,
    type_len: usize,
    properties: [*c]const u8,
    properties_len: usize,
    context_ptr: ?*anyopaque,
) callconv(.c) c_int {
    _ = source_key;
    _ = target_key;
    const context: *StoreEdgeKeys = @ptrCast(@alignCast(context_ptr));
    if (properties_len == 0 or std.mem.eql(u8, properties[0..properties_len], "{}")) return 0;
    const edge_type: []const u8 = if (type_len == 0) "" else type_c[0..type_len];
    const t = for (span(context.schema.edge_types, context.schema.edge_type_count), 0..) |*entry, i| {
        if (std.mem.eql(u8, std.mem.span(entry.type), edge_type)) break i;
    } else return 0;
    const doc = c.yyjson_read(properties, properties_len, 0);
    const root = c.yyjson_doc_get_root(doc);
    var iter: c.yyjson_obj_iter = undefined;
    if (c.yyjson_is_obj(root) and c.yyjson_obj_iter_init(root, &iter)) {
        while (@as(?*c.yyjson_val, c.yyjson_obj_iter_next(&iter))) |key| {
            const k = c.yyjson_get_str(key);
            if (k == null) continue;
            const seen = for (span(context.keys[t], context.key_counts[t])) |existing| {
                if (streq(existing, k)) break true;
            } else false;
            if (seen) continue;
            if (context.key_counts[t] == context.key_caps[t]) {
                const cap = if (context.key_caps[t] != 0) context.key_caps[t] * ST_GROWTH else ST_INIT_CAP_8;
                const grown = interop.reallocN([*c]u8, context.keys[t], @intCast(cap));
                if (grown == null) {
                    context.failed = true;
                    break;
                }
                context.keys[t] = grown;
                context.key_caps[t] = cap;
            }
            const copy = interop.heapStrdup(k);
            if (copy == null) {
                context.failed = true;
                break;
            }
            context.keys[t][@intCast(context.key_counts[t])] = copy;
            context.key_counts[t] += 1;
        }
    }
    c.yyjson_doc_free(doc);
    return if (context.failed) c.CBM_NOT_FOUND else 0;
}

/// Property keys of each edge type in schema, as getSchemaImpl lists them:
/// the base columns, then up to SCHEMA_MAX_JSON_KEYS distinct JSON keys in
/// key order, read from the edge payloads in the Zova graph.
fn storeZovaEdgePropertyKeys(s: *Store, schema: *SchemaInfo, base_cols: []const CStr) c_int {
    const edge_types = span(schema.edge_types, schema.edge_type_count);
    var context: StoreEdgeKeys = .{
        .schema = schema,
        .keys = interop.callocN([*c][*c]u8, edge_types.len + 1),
        .key_counts = interop.callocN(c_int, edge_types.len + 1),
        .key_caps = interop.callocN(c_int, edge_types.len + 1),
    };
    var rc: c_int = if (context.keys != null and context.key_counts != null and context.key_caps != null and
        c.cbm_zova_repository_visit_edges(s.zova_repository, &s.zova_graph_workspace_id, null, storeEdgeKeysVisit, &context) == OK)
        OK
    else
        ERR;
    for (edge_types, 0..) |*edge_type, t| {
        const found: c_int = if (context.key_counts != null) context.key_counts[t] else 0;
        const keys: [*c][*c]u8 = if (context.keys != null) context.keys[t] else null;
        if (rc == OK) {
            if (found > 1) c.qsort(@ptrCast(keys), @intCast(found), @sizeOf([*c]u8), storeNameCompare);
            const take: c_int = @min(found, SCHEMA_MAX_JSON_KEYS);
            const props: [*c][*c]u8 = interop.mallocN([*c]u8, base_cols.len + @as(usize, @intCast(take)) + 1);
            if (props != null) {
                var pn: usize = 0;
                for (base_cols) |col| {
                    props[pn] = interop.heapStrdup(col);
                    pn += 1;
                }
                for (span(keys, take)) |*key| {
                    props[pn] = key.*;
                    key.* = null;
                    pn += 1;
                }
                edge_type.properties = props;
                edge_type.property_count = @intCast(pn);
            } else {
                rc = ERR;
            }
        }
        freeStrings(keys, found);
    }
    c.free(@ptrCast(context.keys));
    c.free(context.key_counts);
    c.free(context.key_caps);
    if (rc != OK) storeSetError(s, "zova edge property keys");
    return rc;
}

/// COUNT(*) of a query that binds the project and a prepared path scope.
fn scopedCount(s: *Store, sql: CStr, project: CStr, norm: CStr, like: CStr) c_int {
    const stmt = prepareOne(s.db, sql) orelse return ERR;
    _ = bindText(stmt, 1, project);
    archBindPathScope(stmt, 2, 3, norm, like);
    var n: c_int = 0;
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) n = c.sqlite3_column_int(stmt, 0);
    _ = c.sqlite3_finalize(stmt);
    return n;
}

pub export fn cbm_store_count_nodes_scoped(s_c: ?*Store, project_c: CStr, path: CStr) c_int {
    const s = s_c orelse return 0;
    if (s.db == null or project_c == null) return 0;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;
    var norm: [c.CBM_SZ_512]u8 = undefined;
    var like: [c.CBM_SZ_512]u8 = undefined;
    if (!archPathPrepare(path, &norm, &like)) return cbm_store_count_nodes(s, project);
    return scopedCount(s, "SELECT COUNT(*) FROM nodes WHERE project = ?1 " ++
        "AND (file_path = ?2 OR file_path LIKE ?3);", project, &norm, &like);
}

pub export fn cbm_store_count_edges_scoped(s_c: ?*Store, project_c: CStr, path: CStr) c_int {
    const s = s_c orelse return 0;
    if (s.db == null or project_c == null) return 0;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;
    var norm: [c.CBM_SZ_512]u8 = undefined;
    var like: [c.CBM_SZ_512]u8 = undefined;
    if (!archPathPrepare(path, &norm, &like)) return cbm_store_count_edges(s, project);
    if (storeIsZovaWorkspace(s)) {
        var scope_ptr: [*c]i64 = null;
        var scope_count: c_int = 0;
        defer c.free(scope_ptr);
        if (storeScopeNodeIds(s, project, &norm, &like, &scope_ptr, &scope_count) != OK or
            storeZovaEdges(s) != OK)
        {
            return ERR;
        }
        const scope = span(scope_ptr, scope_count);
        var n: c_int = 0;
        for (s.zovaEdges()) |*e| {
            n += @intFromBool(storeIdIn(scope, e.source_key) and storeIdIn(scope, e.target_key));
        }
        return n;
    }
    return scopedCount(s, "SELECT COUNT(*) FROM edges e WHERE e.project = ?1 " ++
        "AND EXISTS (SELECT 1 FROM nodes ns WHERE ns.id = e.source_id AND ns.project = ?1 " ++
        "AND (ns.file_path = ?2 OR ns.file_path LIKE ?3)) " ++
        "AND EXISTS (SELECT 1 FROM nodes nt WHERE nt.id = e.target_id AND nt.project = ?1 " ++
        "AND (nt.file_path = ?2 OR nt.file_path LIKE ?3));", project, &norm, &like);
}

/// Steps stmt to completion, reading (name, COUNT(*)) rows into a malloc'd
/// array of cbm_label_count_t or cbm_type_count_t, and finalizes it.
/// CBM_NOT_FOUND when an allocation fails (nothing returned); CBM_STORE_ERR
/// when the scan aborts (the rows read so far are returned).
fn schemaScanCounts(
    comptime T: type,
    comptime name: []const u8,
    s: *Store,
    stmt: ?*Stmt,
    out: *[*c]T,
    out_count: *c_int,
) c_int {
    var cap: usize = ST_INIT_CAP_8;
    var n: usize = 0;
    var arr: [*c]T = interop.mallocN(T, cap);
    if (arr == null) {
        _ = c.sqlite3_finalize(stmt);
        return c.CBM_NOT_FOUND;
    }
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        if (n >= cap) {
            const grown = interop.reallocN(T, arr, cap * ST_GROWTH);
            if (grown == null) {
                for (arr[0..n]) |*item| interop.strFree(&@field(item, name));
                c.free(arr);
                _ = c.sqlite3_finalize(stmt);
                return c.CBM_NOT_FOUND;
            }
            arr = grown;
            cap *= ST_GROWTH;
        }
        var item = std.mem.zeroes(T);
        @field(item, name) = columnDup(stmt, 0);
        item.count = c.sqlite3_column_int(stmt, 1);
        arr[n] = item;
        n += 1;
    }
    if (rc != c.SQLITE_DONE) storeSetErrorSqlite(s, "row scan aborted");
    _ = c.sqlite3_finalize(stmt);
    out.* = arr;
    out_count.* = @intCast(n);
    return if (rc != c.SQLITE_DONE) ERR else OK;
}

const node_base_cols = [_]CStr{ "name", "qualified_name", "file_path", "start_line", "end_line" };
const edge_base_cols = [_]CStr{ "source_id", "target_id" };

/// with_props=false skips the per-label/per-type JSON property-key discovery:
/// those json_each() scans walk EVERY row of each label/type (minutes-scale on
/// multi-million-node graphs) and get_architecture only needs the counts.
fn getSchemaImpl(s_c: ?*Store, project_c: CStr, out: *SchemaInfo, with_props: bool) c_int {
    out.* = std.mem.zeroes(SchemaInfo);
    const s = s_c orelse return c.CBM_NOT_FOUND;
    if (s.db == null) return c.CBM_NOT_FOUND;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;

    // Node labels
    {
        const stmt = prepareOne(s.db, "SELECT label, COUNT(*) FROM nodes WHERE project = ?1 GROUP BY label " ++
            "ORDER BY COUNT(*) DESC;") orelse return c.CBM_NOT_FOUND;
        _ = bindText(stmt, 1, project);
        const rc = schemaScanCounts(c.cbm_label_count_t, "label", s, stmt, &out.node_labels, &out.node_label_count);
        if (rc != OK) return rc;
    }

    // Node label property keys: base columns + distinct JSON property keys per label
    if (with_props) {
        const prop_sql = "SELECT DISTINCT je.key " ++
            "FROM nodes, json_each(nodes.properties) AS je " ++
            "WHERE nodes.project = ?1 AND nodes.label = ?2 " ++
            "  AND nodes.properties != '{}' " ++
            "ORDER BY je.key " ++
            "LIMIT 50;";

        for (span(out.node_labels, out.node_label_count)) |*label| {
            schemaDiscoverProps(s.db, prop_sql, project, label.label, &node_base_cols, &label.properties, &label.property_count);
        }
    }

    // Edge types
    if (storeIsZovaWorkspace(s)) {
        if (storeZovaEdgeTypeCounts(s, false, &.{}, &out.edge_types, &out.edge_type_count) != OK) {
            cbm_store_schema_free(out);
            return ERR;
        }
    } else {
        const stmt = prepareOne(s.db, "SELECT type, COUNT(*) FROM edges WHERE project = ?1 GROUP BY type ORDER " ++
            "BY COUNT(*) DESC;") orelse {
            cbm_store_schema_free(out);
            return c.CBM_NOT_FOUND;
        };
        _ = bindText(stmt, 1, project);
        const rc = schemaScanCounts(c.cbm_type_count_t, "type", s, stmt, &out.edge_types, &out.edge_type_count);
        if (rc == c.CBM_NOT_FOUND) cbm_store_schema_free(out);
        if (rc != OK) return rc;
    }

    // Edge type property keys: base columns + distinct JSON property keys per type
    if (with_props and storeIsZovaWorkspace(s)) {
        if (storeZovaEdgePropertyKeys(s, out, &edge_base_cols) != OK) {
            cbm_store_schema_free(out);
            return ERR;
        }
    } else if (with_props) {
        const prop_sql = "SELECT DISTINCT je.key " ++
            "FROM edges, json_each(edges.properties) AS je " ++
            "WHERE edges.project = ?1 AND edges.type = ?2 " ++
            "  AND edges.properties != '{}' " ++
            "ORDER BY je.key " ++
            "LIMIT 50;";

        for (span(out.edge_types, out.edge_type_count)) |*edge_type| {
            schemaDiscoverProps(s.db, prop_sql, project, edge_type.type, &edge_base_cols, &edge_type.properties, &edge_type.property_count);
        }
    }

    return OK;
}

pub export fn cbm_store_get_schema(s: ?*Store, project: CStr, out: *SchemaInfo) c_int {
    return getSchemaImpl(s, project, out, true);
}

pub export fn cbm_store_get_schema_counts(s: ?*Store, project: CStr, out: *SchemaInfo) c_int {
    return getSchemaImpl(s, project, out, false);
}

pub export fn cbm_store_get_schema_counts_scoped(s_c: ?*Store, project_c: CStr, path: CStr, out: *SchemaInfo) c_int {
    out.* = std.mem.zeroes(SchemaInfo);
    const s = s_c orelse return c.CBM_NOT_FOUND;
    if (s.db == null) return c.CBM_NOT_FOUND;
    var project = project_c;
    if (storeLegacyScope(s, &project) != OK) return ERR;
    var norm: [c.CBM_SZ_512]u8 = undefined;
    var like: [c.CBM_SZ_512]u8 = undefined;
    if (!archPathPrepare(path, &norm, &like)) return getSchemaImpl(s, project, out, false);

    {
        const stmt = prepareOne(s.db, "SELECT label, COUNT(*) FROM nodes WHERE project = ?1" ++ arch_path_scope_sql ++
            " GROUP BY label ORDER BY COUNT(*) DESC;") orelse return c.CBM_NOT_FOUND;
        _ = bindText(stmt, 1, project);
        archBindPathScope(stmt, 2, 3, &norm, &like);
        const rc = schemaScanCounts(c.cbm_label_count_t, "label", s, stmt, &out.node_labels, &out.node_label_count);
        if (rc != OK) return rc;
    }

    if (storeIsZovaWorkspace(s)) {
        var scope: [*c]i64 = null;
        var scope_count: c_int = 0;
        var rc = storeScopeNodeIds(s, project, &norm, &like, &scope, &scope_count);
        if (rc == OK) {
            rc = storeZovaEdgeTypeCounts(s, true, span(scope, scope_count), &out.edge_types, &out.edge_type_count);
        }
        c.free(scope);
        if (rc != OK) {
            cbm_store_schema_free(out);
            return ERR;
        }
    } else {
        const stmt = prepareOne(s.db, "SELECT e.type, COUNT(*) FROM edges e WHERE e.project = ?1 " ++
            "AND EXISTS (SELECT 1 FROM nodes ns WHERE ns.id = e.source_id AND ns.project = ?1 " ++
            "AND (ns.file_path = ?2 OR ns.file_path LIKE ?3)) " ++
            "AND EXISTS (SELECT 1 FROM nodes nt WHERE nt.id = e.target_id AND nt.project = ?1 " ++
            "AND (nt.file_path = ?2 OR nt.file_path LIKE ?3)) " ++
            "GROUP BY e.type ORDER BY COUNT(*) DESC;") orelse {
            cbm_store_schema_free(out);
            return c.CBM_NOT_FOUND;
        };
        _ = bindText(stmt, 1, project);
        archBindPathScope(stmt, 2, 3, &norm, &like);
        const rc = schemaScanCounts(c.cbm_type_count_t, "type", s, stmt, &out.edge_types, &out.edge_type_count);
        if (rc == c.CBM_NOT_FOUND) cbm_store_schema_free(out);
        if (rc != OK) return rc;
    }

    return OK;
}

/// Frees an array of malloc'd strings held through const pointers.
fn freeConstStrings(items: [*c]CStr, count: c_int) void {
    for (span(items, count)) |*item| interop.strFree(item);
    c.free(@ptrCast(items));
}

pub export fn cbm_store_schema_free(out_c: ?*SchemaInfo) void {
    const out = out_c orelse return;
    for (span(out.node_labels, out.node_label_count)) |*label| {
        interop.strFree(&label.label);
        freeStrings(label.properties, label.property_count);
    }
    c.free(out.node_labels);

    for (span(out.edge_types, out.edge_type_count)) |*edge_type| {
        interop.strFree(&edge_type.type);
        freeStrings(edge_type.properties, edge_type.property_count);
    }
    c.free(out.edge_types);

    freeConstStrings(out.rel_patterns, out.rel_pattern_count);
    freeConstStrings(out.sample_func_names, out.sample_func_count);
    freeConstStrings(out.sample_class_names, out.sample_class_count);
    freeConstStrings(out.sample_qns, out.sample_qn_count);

    out.* = std.mem.zeroes(SchemaInfo);
}

// ── Architecture helpers ─────────────────────────────────────────

const ArchInfo = c.cbm_architecture_info_t;

/// A malloc'd NUL-terminated copy of text; NULL on OOM.
fn dupSlice(text: []const u8) [*c]u8 {
    const copy: [*c]u8 = interop.mallocN(u8, text.len + 1);
    if (copy != null) {
        @memcpy(copy[0..text.len], text);
        copy[text.len] = 0;
    }
    return copy;
}

/// Copies a QN segment into buf; null when it is empty or does not fit.
fn qnSegment(buf: *[c.CBM_SZ_256]u8, segment: []const u8) ?CStr {
    if (segment.len == 0 or segment.len >= buf.len) return null;
    @memcpy(buf[0..segment.len], segment);
    buf[segment.len] = 0;
    return buf;
}

/// Extract sub-package from QN: project.dir1.dir2.sym → dir1 (4+ parts → [2], else [1])
pub export fn cbm_qn_to_package(qn_c: CStr) CStr {
    if (qn_c == null or qn_c[0] == 0) return "";
    const static = struct {
        threadlocal var buf: [c.CBM_SZ_256]u8 = undefined;
    };
    const qn = std.mem.span(qn_c);
    // Find dots and extract segment
    var dots: [ST_QN_MAX_DOTS]usize = undefined;
    var ndots: usize = 0;
    for (qn, 0..) |ch, i| {
        if (ndots >= ST_QN_MAX_DOTS) break;
        if (ch == '.') {
            dots[ndots] = i;
            ndots += 1;
        }
    }
    // 4+ segments: return segment[2]
    if (ndots >= ST_QN_MIN_DOTS) {
        if (qnSegment(&static.buf, qn[dots[1] + 1 .. dots[2]])) |segment| return segment;
    }
    // 2+ segments: return segment[1]
    if (ndots >= 1) {
        const end = if (ndots >= 2) dots[1] else qn.len;
        if (qnSegment(&static.buf, qn[dots[0] + 1 .. end])) |segment| return segment;
    }
    return "";
}

/// Extract top-level package from QN: project.dir1.rest → dir1 (segment[1])
pub export fn cbm_qn_to_top_package(qn_c: CStr) CStr {
    if (qn_c == null or qn_c[0] == 0) return "";
    const static = struct {
        threadlocal var buf: [c.CBM_SZ_256]u8 = undefined;
    };
    const qn = std.mem.span(qn_c);
    const first_dot = std.mem.findScalar(u8, qn, '.') orelse return "";
    const start = first_dot + 1;
    const end = std.mem.findScalarPos(u8, qn, start, '.') orelse qn.len;
    return qnSegment(&static.buf, qn[start..end]) orelse "";
}

pub export fn cbm_is_test_file_path(fp: CStr) bool {
    if (fp == null or fp[0] == 0) return false;
    return c.strstr(fp, "test") != null;
}

/// File extension → language name mapping (table-driven)
const ExtLang = struct { ext: []const u8, lang: [:0]const u8 };

const ext_lang_table = [_]ExtLang{
    .{ .ext = ".py", .lang = "Python" },     .{ .ext = ".go", .lang = "Go" },
    .{ .ext = ".js", .lang = "JavaScript" }, .{ .ext = ".jsx", .lang = "JavaScript" },
    .{ .ext = ".ts", .lang = "TypeScript" }, .{ .ext = ".tsx", .lang = "TypeScript" },
    .{ .ext = ".rs", .lang = "Rust" },       .{ .ext = ".java", .lang = "Java" },
    .{ .ext = ".cpp", .lang = "C++" },       .{ .ext = ".cc", .lang = "C++" },
    .{ .ext = ".cxx", .lang = "C++" },       .{ .ext = ".c", .lang = "C" },
    .{ .ext = ".h", .lang = "C" },           .{ .ext = ".cs", .lang = "C#" },
    .{ .ext = ".php", .lang = "PHP" },       .{ .ext = ".lua", .lang = "Lua" },
    .{ .ext = ".scala", .lang = "Scala" },   .{ .ext = ".kt", .lang = "Kotlin" },
    .{ .ext = ".rb", .lang = "Ruby" },       .{ .ext = ".sh", .lang = "Bash" },
    .{ .ext = ".bash", .lang = "Bash" },     .{ .ext = ".zig", .lang = "Zig" },
    .{ .ext = ".ex", .lang = "Elixir" },     .{ .ext = ".exs", .lang = "Elixir" },
    .{ .ext = ".hs", .lang = "Haskell" },    .{ .ext = ".ml", .lang = "OCaml" },
    .{ .ext = ".mli", .lang = "OCaml" },     .{ .ext = ".html", .lang = "HTML" },
    .{ .ext = ".css", .lang = "CSS" },       .{ .ext = ".yaml", .lang = "YAML" },
    .{ .ext = ".yml", .lang = "YAML" },      .{ .ext = ".toml", .lang = "TOML" },
    .{ .ext = ".hcl", .lang = "HCL" },       .{ .ext = ".tf", .lang = "HCL" },
    .{ .ext = ".sql", .lang = "SQL" },       .{ .ext = ".erl", .lang = "Erlang" },
    .{ .ext = ".swift", .lang = "Swift" },   .{ .ext = ".dart", .lang = "Dart" },
    .{ .ext = ".groovy", .lang = "Groovy" }, .{ .ext = ".pl", .lang = "Perl" },
    .{ .ext = ".r", .lang = "R" },           .{ .ext = ".scss", .lang = "SCSS" },
    .{ .ext = ".vue", .lang = "Vue" },       .{ .ext = ".svelte", .lang = "Svelte" },
};

fn extToLang(ext_opt: ?[]const u8) ?[:0]const u8 {
    const ext = ext_opt orelse return null;
    for (&ext_lang_table) |*entry| {
        if (std.mem.eql(u8, ext, entry.ext)) return entry.lang;
    }
    return null;
}

/// Get lowercase file extension from path into buf, dot included. Null if
/// none or too long.
fn fileExtLower(path: CStr, buf: *[c.CBM_SZ_16]u8) ?[]const u8 {
    const dot = std.mem.span(c.cbm_path_last_dot(path));
    if (dot.len == 0 or dot.len >= buf.len) return null;
    for (dot, buf[0..dot.len]) |ch, *lower| lower.* = std.ascii.toLower(ch);
    return buf[0..dot.len];
}

/// A path scope prepared for the architecture queries.
const ArchScope = struct {
    norm: [c.CBM_SZ_512]u8,
    like: [c.CBM_SZ_512]u8,
    scoped: bool,

    fn init(path: CStr) ArchScope {
        var scope: ArchScope = undefined;
        scope.scoped = archPathPrepare(path, &scope.norm, &scope.like);
        return scope;
    }

    /// Prepares sql — scoped_sql under a path scope — and binds the project
    /// as ?1 and the scope as ?2 and ?3. Null when prepare fails.
    fn query(scope: *const ArchScope, db: ?*c.sqlite3, sql: CStr, scoped_sql: CStr, project: CStr) ?*Stmt {
        const stmt = prepareOne(db, if (scope.scoped) scoped_sql else sql) orelse return null;
        _ = bindText(stmt, 1, project);
        if (scope.scoped) archBindPathScope(stmt, 2, 3, &scope.norm, &scope.like);
        return stmt;
    }
};

// ── Architecture aspect implementations ─────────────────────────

fn archLanguages(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    const base = "SELECT file_path FROM nodes WHERE project=?1 AND label='File'";
    const stmt = scope.query(s.db, base, base ++ arch_path_scope_sql, project) orelse {
        storeSetErrorSqlite(s, "arch_languages");
        return ERR;
    };

    // Count per language using a simple parallel array
    var lang_names: [c.CBM_SZ_64][:0]const u8 = undefined;
    var lang_counts: [c.CBM_SZ_64]c_int = undefined;
    var nlang: usize = 0;

    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        var ext_buf: [c.CBM_SZ_16]u8 = undefined;
        const lang = extToLang(fileExtLower(columnText(stmt, 0), &ext_buf)) orelse continue;
        const found = for (lang_names[0..nlang], 0..) |name, i| {
            if (std.mem.eql(u8, name, lang)) break i;
        } else null;
        if (found) |i| {
            lang_counts[i] += 1;
        } else if (nlang < lang_names.len) {
            lang_names[nlang] = lang;
            lang_counts[nlang] = 1;
            nlang += 1;
        }
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);

    // Sort by count descending (simple insertion sort)
    for (1..@max(nlang, 1)) |i| {
        var j = i;
        while (j > 0 and lang_counts[j] > lang_counts[j - 1]) : (j -= 1) {
            std.mem.swap(c_int, &lang_counts[j], &lang_counts[j - 1]);
            std.mem.swap([:0]const u8, &lang_names[j], &lang_names[j - 1]);
        }
    }
    if (nlang > c.CBM_DECIMAL_BASE) nlang = ST_MAX_LANG;

    out.languages = if (nlang > 0) interop.callocN(c.cbm_language_count_t, nlang) else null;
    out.language_count = @intCast(nlang);
    for (0..nlang) |i| {
        out.languages[i] = .{ .language = interop.heapStrdup(lang_names[i].ptr), .file_count = lang_counts[i] };
    }
    return OK;
}

fn archEntryPoints(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    const base = "SELECT name, qualified_name, file_path FROM nodes " ++
        "WHERE project=?1 AND json_extract(properties, '$.is_entry_point') = 1 " ++
        "AND (json_extract(properties, '$.is_test') IS NULL OR " ++
        "json_extract(properties, '$.is_test') != 1) " ++
        "AND file_path NOT LIKE '%test%'";
    const stmt = scope.query(s.db, base ++ " LIMIT 20", base ++ arch_path_scope_sql ++ " LIMIT 20", project) orelse {
        storeSetErrorSqlite(s, "arch_entry_points");
        return ERR;
    };

    var arr = Rows(c.cbm_entry_point_t).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        arr.add().* = .{
            .name = columnDup(stmt, 0),
            .qualified_name = columnDup(stmt, 1),
            .file = columnDup(stmt, 2),
        };
    }
    if (rc != c.SQLITE_DONE) storeSetErrorSqlite(s, "row scan aborted");
    _ = c.sqlite3_finalize(stmt);
    out.entry_points = arr.items;
    out.entry_point_count = arr.len;
    return if (rc != c.SQLITE_DONE) ERR else OK;
}

/// Extract a JSON string value from a simple JSON object by key name.
fn extractJsonStringProp(json_c: CStr, key: []const u8) [*c]u8 {
    if (json_c == null) return null;
    const json = std.mem.span(json_c);
    const at = std.mem.find(u8, json, key) orelse return null;
    const open = std.mem.findScalarPos(u8, json, at + key.len, '"') orelse return null;
    const close = std.mem.findScalarPos(u8, json, open + 1, '"') orelse return null;
    const value = json[open + 1 .. close];
    if (value.len >= c.CBM_SZ_256) return null;
    return dupSlice(value);
}

fn archRoutes(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    const base = "SELECT name, properties, COALESCE(file_path, '') FROM nodes " ++
        "WHERE project=?1 AND label='Route' " ++
        "AND (json_extract(properties, '$.is_test') IS NULL OR " ++
        "json_extract(properties, '$.is_test') != 1)";
    const stmt = scope.query(s.db, base ++ " LIMIT 20", base ++ arch_path_scope_sql ++ " LIMIT 20", project) orelse {
        storeSetErrorSqlite(s, "arch_routes");
        return ERR;
    };

    var arr = Rows(c.cbm_route_info_t).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const name = columnText(stmt, 0);
        const props = columnText(stmt, 1);
        const fp = columnText(stmt, 2);
        if (cbm_is_test_file_path(fp)) continue;

        const route = arr.add();
        route.* = .{
            .method = interop.heapStrdup(""),
            .path = interop.heapStrdup(name),
            .handler = interop.heapStrdup(""),
        };

        var val = extractJsonStringProp(props, "\"method\"");
        if (val != null) {
            interop.strFree(&route.method);
            route.method = val;
        }
        val = extractJsonStringProp(props, "\"path\"");
        if (val != null) {
            interop.strFree(&route.path);
            route.path = val;
        }
        val = extractJsonStringProp(props, "\"handler\"");
        if (val != null) {
            interop.strFree(&route.handler);
            route.handler = val;
        }
    }
    if (rc != c.SQLITE_DONE) storeSetErrorSqlite(s, "row scan aborted");
    _ = c.sqlite3_finalize(stmt);
    out.routes = arr.items;
    out.route_count = arr.len;
    return if (rc != c.SQLITE_DONE) ERR else OK;
}

const StoreIdPair = extern struct {
    source_id: i64,
    target_id: i64,
};

/// The project's CALLS edges as (source, target) node ids: from the Zova edge
/// list for a workspace store, otherwise from the edges table.
fn storeCallsPairs(s: *Store, project: CStr, out: *[*c]StoreIdPair, out_count: *c_int) c_int {
    out.* = null;
    out_count.* = 0;
    var cap: usize = 0;
    var n: usize = 0;
    var pairs: [*c]StoreIdPair = null;
    if (storeIsZovaWorkspace(s)) {
        if (storeZovaEdges(s) != OK) return ERR;
        const t = storeZovaEdgeTypeFind(s, "CALLS");
        if (t >= 0) {
            for (s.zovaEdges()) |*edge| cap += @intFromBool(edge.type == t);
        }
        if (cap != 0) {
            pairs = interop.mallocN(StoreIdPair, cap);
            if (pairs == null) return ERR;
            for (s.zovaEdges()) |*edge| {
                if (edge.type != t) continue;
                pairs[n] = .{ .source_id = edge.source_key, .target_id = edge.target_key };
                n += 1;
            }
        }
        out.* = pairs;
        out_count.* = @intCast(n);
        return OK;
    }
    var stmt: ?*Stmt = null;
    if (prepare(s.db, "SELECT source_id, target_id FROM edges WHERE project=?1 " ++
        "AND type='CALLS'", &stmt) != c.SQLITE_OK)
    {
        storeSetErrorSqlite(s, "calls_pairs");
        return ERR;
    }
    _ = bindText(stmt, 1, project);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        if (n == cap) {
            cap = if (cap != 0) cap * ST_GROWTH else c.CBM_SZ_256;
            const grown = interop.reallocN(StoreIdPair, pairs, cap);
            if (grown == null) {
                rc = c.SQLITE_NOMEM;
                break;
            }
            pairs = grown;
        }
        pairs[n] = .{ .source_id = c.sqlite3_column_int64(stmt, 0), .target_id = c.sqlite3_column_int64(stmt, 1) };
        n += 1;
    }
    _ = c.sqlite3_finalize(stmt);
    if (rc != c.SQLITE_DONE) {
        c.free(pairs);
        storeSetError(s, "calls_pairs");
        return ERR;
    }
    out.* = pairs;
    out_count.* = @intCast(n);
    return OK;
}

/// Hotspot candidates: non-test functions and methods.
const ARCH_HOTSPOT_FILTER =
    "n.project=?1 AND n.label IN ('Function', 'Method') " ++
    "AND (json_extract(n.properties, '$.is_test') IS NULL OR " ++
    "json_extract(n.properties, '$.is_test') != 1) " ++
    "AND n.file_path NOT LIKE '%test%'";

/// Hotspots of a Zova workspace store: fan-in counted from the CALLS edges of
/// the edge list, ties by node id, top 10.
fn archHotspotsZova(s: *Store, project: CStr, scope: *const ArchScope, out: *ArchInfo) c_int {
    var calls: [*c]StoreIdPair = null;
    var call_count: c_int = 0;
    if (storeCallsPairs(s, project, &calls, &call_count) != OK) return ERR;
    const targets_ptr: [*c]i64 = interop.mallocN(i64, @as(usize, @intCast(call_count)) + 1);
    if (targets_ptr == null) {
        c.free(calls);
        return ERR;
    }
    defer c.free(targets_ptr);
    const targets = targets_ptr[0..@intCast(call_count)];
    for (targets, span(calls, call_count)) |*target, *call| target.* = call.target_id;
    c.free(calls);
    c.qsort(targets_ptr, targets.len, @sizeOf(i64), storeIdCompare);

    const sql = "SELECT n.id, n.name, n.qualified_name FROM nodes n WHERE " ++ ARCH_HOTSPOT_FILTER;
    const stmt = scope.query(
        s.db,
        sql ++ " ORDER BY n.id",
        sql ++ " AND (n.file_path = ?2 OR n.file_path LIKE ?3) ORDER BY n.id",
        project,
    ) orelse {
        storeSetErrorSqlite(s, "arch_hotspots");
        return ERR;
    };
    const HOTSPOT_TOP = 10;
    const arr_ptr: [*c]c.cbm_hotspot_t = interop.callocN(c.cbm_hotspot_t, HOTSPOT_TOP);
    var n: usize = 0;
    var t: usize = 0;
    var rc: c_int = c.SQLITE_DONE;
    if (arr_ptr != null) {
        const arr = arr_ptr[0..HOTSPOT_TOP];
        rc = c.sqlite3_step(stmt);
        while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
            const id = c.sqlite3_column_int64(stmt, 0);
            while (t < targets.len and targets[t] < id) t += 1;
            var fan_in: c_int = 0;
            while (t < targets.len and targets[t] == id) {
                fan_in += 1;
                t += 1;
            }
            // Rows come in id order, so an equal fan-in never displaces.
            if (fan_in == 0 or (n == HOTSPOT_TOP and fan_in <= arr[n - 1].fan_in)) continue;
            var at = n - @intFromBool(n == HOTSPOT_TOP);
            if (n < HOTSPOT_TOP) n += 1;
            interop.strFree(&arr[at].name);
            interop.strFree(&arr[at].qualified_name);
            while (at > 0 and arr[at - 1].fan_in < fan_in) : (at -= 1) arr[at] = arr[at - 1];
            arr[at] = .{
                .name = columnDup(stmt, 1),
                .qualified_name = columnDup(stmt, 2),
                .fan_in = fan_in,
            };
        }
    }
    _ = c.sqlite3_finalize(stmt);
    if (arr_ptr == null or rc != c.SQLITE_DONE) {
        for (span(arr_ptr, @intCast(n))) |*hotspot| {
            interop.strFree(&hotspot.name);
            interop.strFree(&hotspot.qualified_name);
        }
        c.free(arr_ptr);
        storeSetError(s, "arch_hotspots");
        return ERR;
    }
    out.hotspots = arr_ptr;
    out.hotspot_count = @intCast(n);
    return OK;
}

fn archHotspots(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    if (storeIsZovaWorkspace(s)) return archHotspotsZova(s, project, &scope, out);
    const base = "SELECT n.name, n.qualified_name, COUNT(*) as fan_in " ++
        "FROM nodes n JOIN edges e ON e.target_id = n.id AND e.type = 'CALLS' " ++
        "WHERE " ++ ARCH_HOTSPOT_FILTER;
    const stmt = scope.query(
        s.db,
        base ++ " GROUP BY n.id ORDER BY fan_in DESC LIMIT 10",
        base ++ " AND (n.file_path = ?2 OR n.file_path LIKE ?3) " ++
            "GROUP BY n.id ORDER BY fan_in DESC LIMIT 10",
        project,
    ) orelse {
        storeSetErrorSqlite(s, "arch_hotspots");
        return ERR;
    };

    var arr = Rows(c.cbm_hotspot_t).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        arr.add().* = .{
            .name = columnDup(stmt, 0),
            .qualified_name = columnDup(stmt, 1),
            .fan_in = c.sqlite3_column_int(stmt, 2),
        };
    }
    if (rc != c.SQLITE_DONE) storeSetErrorSqlite(s, "row scan aborted");
    _ = c.sqlite3_finalize(stmt);
    out.hotspots = arr.items;
    out.hotspot_count = arr.len;
    return if (rc != c.SQLITE_DONE) ERR else OK;
}

/// Look up package name for a node ID in the parallel arrays. nids must be
/// sorted ascending (the node query orders by id) — a linear scan here is
/// O(E×N) across the edge loop and spun for >10 minutes on Linux-kernel-sized
/// graphs (~1.4M defs × ~1.4M CALLS edges).
fn lookupPkg(nids: []const i64, npkgs: []const [*c]u8, id: i64) CStr {
    var lo: usize = 0;
    var hi: usize = nids.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        if (nids[mid] == id) return npkgs[mid];
        if (nids[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return null;
}

/// Cross-package boundaries accumulated as parallel arrays.
const BoundaryAccum = struct {
    froms: [c.CBM_SZ_32][*c]u8 = undefined,
    tos: [c.CBM_SZ_32][*c]u8 = undefined,
    counts: [c.CBM_SZ_32]c_int = undefined,
    n: usize = 0,

    /// Accumulate a cross-package boundary into parallel arrays.
    fn accum(b: *BoundaryAccum, src_pkg: CStr, tgt_pkg: CStr) void {
        for (b.froms[0..b.n], b.tos[0..b.n], b.counts[0..b.n]) |from, to, *count| {
            if (streq(from, src_pkg) and streq(to, tgt_pkg)) {
                count.* += 1;
                return;
            }
        }
        if (b.n < b.counts.len) {
            b.froms[b.n] = interop.heapStrdup(src_pkg);
            b.tos[b.n] = interop.heapStrdup(tgt_pkg);
            b.counts[b.n] = 1;
            b.n += 1;
        }
    }
};

fn archBoundaries(s: *Store, project: CStr, path: CStr, out_arr: *[*c]c.cbm_cross_pkg_boundary_t, out_count: *c_int) c_int {
    const scope = ArchScope.init(path);
    const nbase = "SELECT id, qualified_name, file_path FROM nodes WHERE project=?1 AND label IN " ++
        "('Function','Method','Class')";
    const nstmt = scope.query(s.db, nbase ++ " ORDER BY id", nbase ++ arch_path_scope_sql ++ " ORDER BY id", project) orelse {
        storeSetErrorSqlite(s, "arch_boundaries_nodes");
        return ERR;
    };

    var nids = Rows(i64).init(c.CBM_SZ_256);
    var npkgs = Rows([*c]u8).init(c.CBM_SZ_256);
    defer {
        freeStrings(npkgs.items, npkgs.len);
        c.free(nids.items);
    }

    var rc = c.sqlite3_step(nstmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(nstmt)) {
        nids.add().* = c.sqlite3_column_int64(nstmt, 0);
        npkgs.add().* = interop.heapStrdup(cbm_qn_to_package(columnText(nstmt, 1)));
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(nstmt);
        return ERR;
    }
    _ = c.sqlite3_finalize(nstmt);

    // Scan edges, count cross-package calls
    var calls: [*c]StoreIdPair = null;
    var call_count: c_int = 0;
    if (storeCallsPairs(s, project, &calls, &call_count) != OK) return ERR;

    var b: BoundaryAccum = .{};
    for (span(calls, call_count)) |*call| {
        const src_pkg = lookupPkg(nids.slice(), npkgs.slice(), call.source_id);
        const tgt_pkg = lookupPkg(nids.slice(), npkgs.slice(), call.target_id);
        if (src_pkg == null or tgt_pkg == null or src_pkg[0] == 0 or tgt_pkg[0] == 0 or streq(src_pkg, tgt_pkg)) {
            continue;
        }
        b.accum(src_pkg, tgt_pkg);
    }
    c.free(calls);

    // Sort by count descending
    for (1..@max(b.n, 1)) |i| {
        var j = i;
        while (j > 0 and b.counts[j] > b.counts[j - 1]) : (j -= 1) {
            std.mem.swap(c_int, &b.counts[j], &b.counts[j - 1]);
            std.mem.swap([*c]u8, &b.froms[j], &b.froms[j - 1]);
            std.mem.swap([*c]u8, &b.tos[j], &b.tos[j - 1]);
        }
    }
    if (b.n > c.CBM_DECIMAL_BASE) {
        for (b.froms[ST_MAX_ITERATIONS..b.n], b.tos[ST_MAX_ITERATIONS..b.n]) |from, to| {
            c.free(from);
            c.free(to);
        }
        b.n = ST_MAX_ITERATIONS;
    }

    const result: [*c]c.cbm_cross_pkg_boundary_t = if (b.n > 0) interop.callocN(c.cbm_cross_pkg_boundary_t, b.n) else null;
    for (0..b.n) |i| {
        result[i] = .{ .from = b.froms[i], .to = b.tos[i], .call_count = b.counts[i] };
    }
    out_arr.* = result;
    out_count.* = @intCast(b.n);
    return OK;
}

const MAX_PREVIEW_NAMES = 15;

/// Fallback: derive packages from QN segments when no Package nodes exist.
fn archPackagesFromQn(s: *Store, project: CStr, path: CStr, out_arr: *[*c]c.cbm_package_summary_t, out_count: *c_int) c_int {
    const scope = ArchScope.init(path);
    const qbase = "SELECT qualified_name FROM nodes WHERE project=?1 AND label IN " ++
        "('Function','Method','Class')";
    const stmt = scope.query(s.db, qbase, qbase ++ arch_path_scope_sql, project) orelse {
        storeSetErrorSqlite(s, "arch_packages_qn");
        return ERR;
    };

    var pnames: [c.CBM_SZ_64][*c]u8 = undefined;
    var pcounts: [c.CBM_SZ_64]c_int = undefined;
    var np: usize = 0;
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const pkg = cbm_qn_to_package(columnText(stmt, 0));
        if (pkg[0] == 0) continue;
        const found = for (pnames[0..np], 0..) |name, i| {
            if (streq(name, pkg)) break i;
        } else null;
        if (found) |i| {
            pcounts[i] += 1;
        } else if (np < pnames.len) {
            pnames[np] = interop.heapStrdup(pkg);
            pcounts[np] = 1;
            np += 1;
        }
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        for (pnames[0..np]) |name| c.free(name);
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);

    // Sort by count desc
    for (1..@max(np, 1)) |i| {
        var j = i;
        while (j > 0 and pcounts[j] > pcounts[j - 1]) : (j -= 1) {
            std.mem.swap(c_int, &pcounts[j], &pcounts[j - 1]);
            std.mem.swap([*c]u8, &pnames[j], &pnames[j - 1]);
        }
    }
    if (np > MAX_PREVIEW_NAMES) {
        for (pnames[MAX_PREVIEW_NAMES..np]) |name| c.free(name);
        np = MAX_PREVIEW_NAMES;
    }

    const arr: [*c]c.cbm_package_summary_t = if (np > 0) interop.callocN(c.cbm_package_summary_t, np) else null;
    for (0..np) |i| arr[i] = .{ .name = pnames[i], .node_count = pcounts[i] };
    out_arr.* = arr;
    out_count.* = @intCast(np);
    return OK;
}

fn archPackages(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    const base = "SELECT n.name, COUNT(*) as cnt FROM nodes n " ++
        "WHERE n.project=?1 AND n.label='Package'";
    const stmt = scope.query(
        s.db,
        base ++ " GROUP BY n.name ORDER BY cnt DESC LIMIT 15",
        base ++ " AND (n.file_path = ?2 OR n.file_path LIKE ?3) " ++
            "GROUP BY n.name ORDER BY cnt DESC LIMIT 15",
        project,
    ) orelse {
        storeSetErrorSqlite(s, "arch_packages");
        return ERR;
    };

    var rows = Rows(c.cbm_package_summary_t).init(ST_INIT_CAP_16);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        rows.add().* = .{ .name = columnDup(stmt, 0), .node_count = c.sqlite3_column_int(stmt, 1) };
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        out.packages = rows.items;
        out.package_count = rows.len;
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);

    // Fallback: group by QN segment if no Package nodes
    var arr = rows.items;
    var n = rows.len;
    if (n == 0) {
        c.free(arr);
        const qn_rc = archPackagesFromQn(s, project, path, &arr, &n);
        if (qn_rc != OK) return qn_rc;
    }

    out.packages = arr;
    out.package_count = n;
    return OK;
}

const Layer = struct { layer: CStr, reason: CStr };

/// Classifies a package by its fan-in/out; a computed reason is formatted
/// into reason_buf.
fn classifyLayer(in: c_int, out_deg: c_int, has_routes: bool, has_entry_points: bool, reason_buf: *[c.CBM_SZ_128]u8) Layer {
    if (has_entry_points and out_deg > 0 and in == 0) {
        return .{ .layer = "entry", .reason = "has entry points, only outbound calls" };
    }
    if (has_routes) return .{ .layer = "api", .reason = "has HTTP route definitions" };
    if (in > out_deg and in > ST_MIN_INDEGREE) {
        _ = interop.bprint(reason_buf, "high fan-in ({d} in, {d} out)", .{ in, out_deg });
        return .{ .layer = "core", .reason = reason_buf };
    }
    if (out_deg == 0 and in > 0) return .{ .layer = "leaf", .reason = "only inbound calls, no outbound" };
    if (in == 0 and out_deg > 0) return .{ .layer = "entry", .reason = "only outbound calls" };
    _ = interop.bprint(reason_buf, "fan-in={d}, fan-out={d}", .{ in, out_deg });
    return .{ .layer = "internal", .reason = reason_buf };
}

/// Find or insert a package name, returning its index. Returns null if full.
fn findOrAddPkg(all_pkgs: [][*c]u8, npkgs: *usize, pkg: CStr) ?usize {
    for (all_pkgs[0..npkgs.*], 0..) |name, j| {
        if (streq(name, pkg)) return j;
    }
    if (npkgs.* < all_pkgs.len) {
        const idx = npkgs.*;
        all_pkgs[idx] = interop.heapStrdup(pkg);
        npkgs.* += 1;
        return idx;
    }
    return null;
}

/// Check if a package name appears in an array.
fn pkgInList(pkg: CStr, list: []const [*c]u8) bool {
    for (list) |name| {
        if (streq(pkg, name)) return true;
    }
    return false;
}

/// Collect package names from nodes matching a SQL query (must use ?1 = project).
/// Returns the names collected into pkgs; none when the query cannot be prepared.
fn collectPkgNames(s: *Store, comptime sql: [:0]const u8, project: CStr, path: CStr, pkgs: [][*c]u8) [][*c]u8 {
    const scope = ArchScope.init(path);
    const stmt = scope.query(s.db, sql, sql ++ arch_path_scope_sql, project) orelse return pkgs[0..0];
    var count: usize = 0;
    while (c.sqlite3_step(stmt) == c.SQLITE_ROW and count < pkgs.len) {
        pkgs[count] = interop.heapStrdup(cbm_qn_to_package(columnText(stmt, 0)));
        count += 1;
    }
    _ = c.sqlite3_finalize(stmt);
    return pkgs[0..count];
}

fn archLayers(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    // Get boundaries for fan analysis
    var boundaries_ptr: [*c]c.cbm_cross_pkg_boundary_t = null;
    var bcount: c_int = 0;
    const rc = archBoundaries(s, project, path, &boundaries_ptr, &bcount);
    if (rc != OK) return rc;
    const boundaries = span(boundaries_ptr, bcount);

    // Collect route and entry point packages
    var route_buf: [c.CBM_SZ_32][*c]u8 = undefined;
    const route_pkgs = collectPkgNames(s, "SELECT qualified_name FROM nodes WHERE project=?1 AND label='Route'", project, path, &route_buf);

    var entry_buf: [c.CBM_SZ_32][*c]u8 = undefined;
    const entry_pkgs = collectPkgNames(s, "SELECT qualified_name FROM nodes WHERE project=?1 AND " ++
        "json_extract(properties, '$.is_entry_point') = 1", project, path, &entry_buf);

    // Compute fan-in/out per package
    var all_pkgs: [ST_MAX_PKGS][*c]u8 = undefined;
    var fan_in = std.mem.zeroes([ST_MAX_PKGS]c_int);
    var fan_out = std.mem.zeroes([ST_MAX_PKGS]c_int);
    var npkgs: usize = 0;

    for (boundaries) |*boundary| {
        if (findOrAddPkg(&all_pkgs, &npkgs, boundary.from)) |fi| fan_out[fi] += boundary.call_count;
        if (findOrAddPkg(&all_pkgs, &npkgs, boundary.to)) |ti| fan_in[ti] += boundary.call_count;
    }

    // Also include route/entry packages
    for (route_pkgs) |pkg| _ = findOrAddPkg(&all_pkgs, &npkgs, pkg);
    for (entry_pkgs) |pkg| _ = findOrAddPkg(&all_pkgs, &npkgs, pkg);

    // Classify each package
    out.layers = if (npkgs > 0) interop.callocN(c.cbm_package_layer_t, npkgs) else null;
    out.layer_count = @intCast(npkgs);
    for (0..npkgs) |i| {
        const has_route = pkgInList(all_pkgs[i], route_pkgs);
        const has_entry = pkgInList(all_pkgs[i], entry_pkgs);
        var reason_buf: [c.CBM_SZ_128]u8 = undefined;
        const layer = classifyLayer(fan_in[i], fan_out[i], has_route, has_entry, &reason_buf);
        out.layers[i] = .{
            .name = all_pkgs[i], // transfer ownership
            .layer = interop.heapStrdup(layer.layer),
            .reason = interop.heapStrdup(layer.reason),
        };
    }

    // Sort layers by name
    const layers = span(out.layers, out.layer_count);
    for (1..@max(layers.len, 1)) |i| {
        var j = i;
        while (j > 0 and c.strcmp(layers[j].name, layers[j - 1].name) < 0) : (j -= 1) {
            std.mem.swap(c.cbm_package_layer_t, &layers[j], &layers[j - 1]);
        }
    }

    // Cleanup
    for (boundaries) |*boundary| {
        interop.strFree(&boundary.from);
        interop.strFree(&boundary.to);
    }
    c.free(boundaries_ptr);
    for (route_pkgs) |pkg| c.free(pkg);
    for (entry_pkgs) |pkg| c.free(pkg);

    return OK;
}

/// One directory of the file tree and the names of its direct children.
const ArchDir = struct {
    path: [*c]u8,
    children: [*c][*c]u8 = null,
    child_count: c_int = 0,
    child_cap: c_int = 0,

    /// Add a child to a dir entry if not already present.
    fn addChild(dir: *ArchDir, child: []const u8) void {
        for (span(dir.children, dir.child_count)) |existing| {
            if (std.mem.eql(u8, std.mem.span(existing), child)) return;
        }
        if (dir.child_count >= dir.child_cap) {
            dir.child_cap = if (dir.child_cap != 0) dir.child_cap * 2 else ST_INIT_CAP_4;
            dir.children = interop.reallocN([*c]u8, dir.children, @intCast(dir.child_cap));
        }
        dir.children[@intCast(dir.child_count)] = dupSlice(child);
        dir.child_count += 1;
    }
};

/// The directories of the file tree; at most CBM_SZ_64 are tracked.
const ArchDirs = struct {
    items: [c.CBM_SZ_64]ArchDir = undefined,
    len: usize = 0,

    fn slice(dirs: *ArchDirs) []ArchDir {
        return dirs.items[0..dirs.len];
    }

    /// Find or create a directory entry by path. Null if full.
    fn findOrCreate(dirs: *ArchDirs, path: []const u8) ?*ArchDir {
        for (dirs.slice()) |*dir| {
            if (std.mem.eql(u8, std.mem.span(dir.path), path)) return dir;
        }
        if (dirs.len < dirs.items.len) {
            const dir = &dirs.items[dirs.len];
            dir.* = .{ .path = dupSlice(path) };
            dirs.len += 1;
            return dir;
        }
        return null;
    }
};

/// Create a file tree entry by checking if a path is a file and counting dir children.
fn makeTreeEntry(path: []const u8, files: []const [*c]u8, dirs: []const ArchDir) c.cbm_file_tree_entry_t {
    var e: c.cbm_file_tree_entry_t = .{ .path = dupSlice(path) };
    const is_file = for (files) |file| {
        if (std.mem.eql(u8, std.mem.span(file), path)) break true;
    } else false;
    e.type = interop.heapStrdup(if (is_file) "file" else "dir");
    for (dirs) |*dir| {
        if (std.mem.eql(u8, std.mem.span(dir.path), path)) {
            e.children = dir.child_count;
            break;
        }
    }
    return e;
}

/// Split a path by '/' into parts. Returns number of parts; when parts is
/// full, the last part holds the rest of the path.
fn splitPathParts(fp: []const u8, parts: [][]const u8) usize {
    var nparts: usize = 0;
    var start: usize = 0;
    for (fp, 0..) |ch, i| {
        if (nparts + 1 >= parts.len) break;
        if (ch == '/') {
            parts[nparts] = fp[start..i];
            nparts += 1;
            start = i + 1;
        }
    }
    parts[nparts] = fp[start..];
    return nparts + 1;
}

/// Register dir hierarchy for one file path.
fn archRegisterFileDirs(fp_c: CStr, dirs: *ArchDirs) void {
    const full = std.mem.span(fp_c);
    const fp = full[0..@min(full.len, c.CBM_SZ_512 - 1)];
    var parts_buf: [ST_SEARCH_MAX_BINDS][]const u8 = undefined;
    const parts = parts_buf[0..splitPathParts(fp, &parts_buf)];

    if (dirs.findOrCreate("")) |root| root.addChild(parts[0]);

    var depth: usize = 0;
    while (depth + 1 < parts.len and depth < ST_MAX_PATH_DEPTH) : (depth += 1) {
        // parts[0..depth] joined by '/' is the path up to the end of parts[depth].
        const dir_end = @intFromPtr(parts[depth].ptr) - @intFromPtr(fp.ptr) + parts[depth].len;
        if (dirs.findOrCreate(fp[0..dir_end])) |dir| dir.addChild(parts[depth + 1]);
    }
}

/// Collect tree entries from dir arrays.
fn archCollectEntries(dirs: []const ArchDir, files: []const [*c]u8, entries_out: *[*c]c.cbm_file_tree_entry_t, en_out: *c_int) void {
    var entries = Rows(c.cbm_file_tree_entry_t).init(c.CBM_SZ_64);

    // Root children
    for (dirs) |*dir| {
        if (dir.path[0] != 0) continue;
        for (span(dir.children, dir.child_count)) |child| {
            entries.add().* = makeTreeEntry(std.mem.span(child), files, dirs);
        }
    }

    // Non-root dir children (depth < 3)
    for (dirs) |*dir| {
        const dir_path = std.mem.span(dir.path);
        if (dir_path.len == 0 or std.mem.count(u8, dir_path, "/") >= ST_MAX_PATH_DEPTH) continue;
        for (span(dir.children, dir.child_count)) |child| {
            var path: [c.CBM_SZ_512]u8 = undefined;
            _ = interop.bprint(&path, "{s}/{s}", .{ dir_path, std.mem.span(child) });
            entries.add().* = makeTreeEntry(std.mem.sliceTo(&path, 0), files, dirs);
        }
    }

    // Sort by path
    const sorted = entries.slice();
    for (1..@max(sorted.len, 1)) |i| {
        var j = i;
        while (j > 0 and c.strcmp(sorted[j].path, sorted[j - 1].path) < 0) : (j -= 1) {
            std.mem.swap(c.cbm_file_tree_entry_t, &sorted[j], &sorted[j - 1]);
        }
    }

    entries_out.* = entries.items;
    en_out.* = entries.len;
}

/// Free dir arrays.
fn archFreeDirs(dirs: []const ArchDir, files: [*c][*c]u8, file_count: c_int) void {
    for (dirs) |*dir| {
        c.free(dir.path);
        freeStrings(dir.children, dir.child_count);
    }
    freeStrings(files, file_count);
}

fn archFileTree(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    const base = "SELECT file_path FROM nodes WHERE project=?1 AND label='File'";
    const stmt = scope.query(s.db, base, base ++ arch_path_scope_sql, project) orelse {
        storeSetErrorSqlite(s, "arch_file_tree");
        return ERR;
    };

    var files = Rows([*c]u8).init(c.CBM_SZ_32);
    var dirs: ArchDirs = .{};
    defer archFreeDirs(dirs.slice(), files.items, files.len);

    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        const fp = columnText(stmt, 0);
        if (fp == null) continue;
        files.add().* = interop.heapStrdup(fp);
        archRegisterFileDirs(fp, &dirs);
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);

    archCollectEntries(dirs.slice(), files.slice(), &out.file_tree, &out.file_tree_count);
    return OK;
}

// ── Louvain community detection ─────────────────────────────────

/// A malloc'd slice of n uninitialized T; null on OOM.
fn mallocSlice(comptime T: type, n: usize) ?[]T {
    const items: ?[*]T = interop.mallocN(T, n);
    return (items orelse return null)[0..n];
}

/// Find the index of a node ID in the nodes array.
fn louvainNodeIndex(nodes: []const i64, id: i64) ?u32 {
    for (nodes, 0..) |node, i| {
        if (node == id) return @intCast(i);
    }
    return null;
}

/// Deduplicated, normalized (si < di) undirected edge weights.
const LouvainWeights = struct {
    si: []u32,
    di: []u32,
    w: []f64,
    n: usize = 0,

    fn deinit(weights: *const LouvainWeights) void {
        c.free(weights.si.ptr);
        c.free(weights.di.ptr);
        c.free(weights.w.ptr);
    }
};

/// Build deduplicated, normalized edge weight arrays from raw edges. Null on OOM.
fn louvainBuildWeights(nodes: []const i64, edges: []const c.cbm_louvain_edge_t) ?LouvainWeights {
    const wcap = @max(edges.len, 1);
    const wsi = mallocSlice(u32, wcap) orelse return null;
    const wdi = mallocSlice(u32, wcap) orelse {
        c.free(wsi.ptr);
        return null;
    };
    const ww = mallocSlice(f64, wcap) orelse {
        c.free(wsi.ptr);
        c.free(wdi.ptr);
        return null;
    };
    var weights: LouvainWeights = .{ .si = wsi, .di = wdi, .w = ww };

    for (edges) |*edge| {
        var si = louvainNodeIndex(nodes, edge.src) orelse continue;
        var di = louvainNodeIndex(nodes, edge.dst) orelse continue;
        if (si == di) continue;
        if (si > di) std.mem.swap(u32, &si, &di);
        const found = for (wsi[0..weights.n], wdi[0..weights.n], 0..) |a, b, i| {
            if (a == si and b == di) break i;
        } else null;
        if (found) |i| {
            ww[i] += 1.0;
        } else {
            wsi[weights.n] = si;
            wdi[weights.n] = di;
            ww[weights.n] = 1.0;
            weights.n += 1;
        }
    }
    return weights;
}

const LEIDEN_MAX_LEVELS = 64;
const LEIDEN_MOVE_PASS_CAP = 100;
/// "No label yet" in the relabel map and the aggregate seeds.
const leiden_none = std.math.maxInt(u32);

/// Weighted undirected graph in CSR form. Each undirected edge is stored as
/// two directed entries; k[i] is the weighted degree of node i. Self-loops are
/// never materialised — an aggregate node's intra-community weight is folded
/// into k[i] (which is preserved across levels), while nbr/w hold only
/// inter-community edges. The modularity gain therefore uses k[] for the
/// null-model term and nbr/w for connectivity, so intra weight affects the
/// degree but is never mistaken for an edge to another community.
const LeidenGraph = struct {
    n: usize,
    off: []u32, // CSR offsets, length n + 1
    nbr: []u32, // neighbour indices, length off[n]
    w: []f64, // edge weights aligned with nbr
    k: []f64, // weighted degree per node

    fn deinit(g: *const LeidenGraph) void {
        c.free(g.off.ptr);
        c.free(g.nbr.ptr);
        c.free(g.w.ptr);
        c.free(g.k.ptr);
    }
};

const Oom = error{OutOfMemory};

/// Build a CSR graph from a deduplicated undirected edge list.
fn lgBuild(n: usize, wsi: []const u32, wdi: []const u32, ww: []const f64) Oom!LeidenGraph {
    const off = callocSlice(u32, n + 1) orelse return error.OutOfMemory;
    errdefer c.free(off.ptr);
    const k = callocSlice(f64, n) orelse return error.OutOfMemory;
    errdefer c.free(k.ptr);
    const fill = mallocSlice(u32, n) orelse return error.OutOfMemory;
    defer c.free(fill.ptr);

    for (wsi, wdi) |a, b| {
        off[a + 1] += 1;
        off[b + 1] += 1;
    }
    for (0..n) |i| off[i + 1] += off[i];
    const total = @max(off[n], 1);
    const nbr = mallocSlice(u32, total) orelse return error.OutOfMemory;
    errdefer c.free(nbr.ptr);
    const w = mallocSlice(f64, total) orelse return error.OutOfMemory;

    @memcpy(fill, off[0..n]);
    for (wsi, wdi, ww) |a, b, we| {
        nbr[fill[a]] = b;
        w[fill[a]] = we;
        fill[a] += 1;
        nbr[fill[b]] = a;
        w[fill[b]] = we;
        fill[b] += 1;
        k[a] += we;
        k[b] += we;
    }
    return .{ .n = n, .off = off, .nbr = nbr, .w = w, .k = k };
}

/// Local-moving phase: greedily move each node to the neighbouring community
/// with the highest modularity gain, using a work queue seeded with every node
/// and re-queueing only the neighbours of a node that actually moved. Mutates
/// comm[] in place.
fn leidenMove(g: *const LeidenGraph, comm: []u32, gamma: f64, twom: f64) void {
    const n = g.n;
    const stot = callocSlice(f64, n) orelse return;
    defer c.free(stot.ptr);
    const acc = callocSlice(f64, n) orelse return;
    defer c.free(acc.ptr);
    const queue = mallocSlice(u32, n) orelse return;
    defer c.free(queue.ptr);
    const dirty = mallocSlice(u32, n) orelse return;
    defer c.free(dirty.ptr);
    const inq = callocSlice(bool, n) orelse return;
    defer c.free(inq.ptr);

    for (0..n) |i| {
        stot[comm[i]] += g.k[i];
        queue[i] = @intCast(i);
        inq[i] = true;
    }
    var qhead: usize = 0;
    var qcount: usize = n;
    var cap: i64 = @as(i64, @intCast(n)) * LEIDEN_MOVE_PASS_CAP + LEIDEN_MAX_LEVELS;
    while (qcount > 0 and cap > 0) : (cap -= 1) {
        const v = queue[qhead];
        qhead = (qhead + 1) % n;
        qcount -= 1;
        inq[v] = false;
        const cv = comm[v];
        const lo = g.off[v];
        const hi = g.off[v + 1];
        var ndirty: usize = 0;
        for (g.nbr[lo..hi], g.w[lo..hi]) |u, weight| {
            if (u == v) continue;
            const cu = comm[u];
            if (acc[cu] == 0.0) {
                dirty[ndirty] = cu;
                ndirty += 1;
            }
            acc[cu] += weight;
        }
        stot[cv] -= g.k[v];
        const kv = g.k[v];
        var best_c = cv;
        var best_gain = acc[cv] - gamma * kv * stot[cv] / twom;
        for (dirty[0..ndirty]) |cand| {
            const gain = acc[cand] - gamma * kv * stot[cand] / twom;
            if (gain > best_gain) {
                best_gain = gain;
                best_c = cand;
            }
        }
        stot[best_c] += kv;
        comm[v] = best_c;
        if (best_c != cv) {
            for (g.nbr[lo..hi]) |u| {
                if (comm[u] != best_c and !inq[u] and qcount < n) {
                    queue[(qhead + qcount) % n] = u;
                    qcount += 1;
                    inq[u] = true;
                }
            }
        }
        for (dirty[0..ndirty]) |cand| acc[cand] = 0.0;
    }
}

/// Compact community labels in comm[] to the dense range [0, returned count).
fn leidenRelabel(comm: []u32) usize {
    const map = mallocSlice(u32, comm.len) orelse return comm.len;
    defer c.free(map.ptr);
    @memset(map, leiden_none);
    var next: u32 = 0;
    for (comm) |*label| {
        if (map[label.*] == leiden_none) {
            map[label.*] = next;
            next += 1;
        }
        label.* = map[label.*];
    }
    return next;
}

/// Refinement phase: within each move-phase community, merge singleton nodes
/// into the best connected sub-community (positive modularity gain, edge must
/// exist). This re-derives communities bottom-up so each one is guaranteed
/// internally connected — the defect single-level Louvain suffers from, which
/// fragments the graph into hundreds of tiny noisy clusters. Writes
/// sub-community labels into refined[] and returns their count.
fn leidenRefine(g: *const LeidenGraph, comm: []const u32, gamma: f64, twom: f64, refined: []u32) usize {
    const n = g.n;
    for (refined, 0..) |*label, i| label.* = @intCast(i);
    const stot = callocSlice(f64, n) orelse return leidenRelabel(refined);
    defer c.free(stot.ptr);
    const acc = callocSlice(f64, n) orelse return leidenRelabel(refined);
    defer c.free(acc.ptr);
    const rsize = mallocSlice(u32, n) orelse return leidenRelabel(refined);
    defer c.free(rsize.ptr);
    const dirty = mallocSlice(u32, n) orelse return leidenRelabel(refined);
    defer c.free(dirty.ptr);

    @memcpy(stot, g.k[0..n]);
    @memset(rsize, 1);
    for (0..n) |v| {
        if (rsize[refined[v]] != 1) continue; // only singletons merge, per the refinement rule
        const cv = comm[v];
        const lo = g.off[v];
        const hi = g.off[v + 1];
        var ndirty: usize = 0;
        for (g.nbr[lo..hi], g.w[lo..hi]) |u, weight| {
            if (u == v or comm[u] != cv) continue; // stay within the move-phase community
            const ru = refined[u];
            if (acc[ru] == 0.0) {
                dirty[ndirty] = ru;
                ndirty += 1;
            }
            acc[ru] += weight;
        }
        const rv = refined[v];
        const kv = g.k[v];
        stot[rv] -= kv;
        var best_r = rv;
        var best_gain: f64 = 0.0;
        for (dirty[0..ndirty]) |r| {
            if (r == rv) continue;
            const gain = acc[r] - gamma * kv * stot[r] / twom;
            if (gain > best_gain) {
                best_gain = gain;
                best_r = r;
            }
        }
        if (best_r != rv) {
            refined[v] = best_r;
            stot[best_r] += kv;
            rsize[best_r] += 1;
            rsize[rv] -= 1;
        } else {
            stot[rv] += kv;
        }
        for (dirty[0..ndirty]) |r| acc[r] = 0.0;
    }
    return leidenRelabel(refined);
}

/// Aggregation phase: collapse each refined sub-community into a single node.
/// Returns the coarser graph and records, for each aggregate node, the
/// move-phase community it belonged to in seed[] so the next level starts from
/// the coarse structure rather than from singletons.
fn leidenAggregate(g: *const LeidenGraph, refined: []const u32, r_count: usize, comm: []const u32, seed: []u32) Oom!LeidenGraph {
    const n = g.n;
    const k2 = callocSlice(f64, r_count) orelse return error.OutOfMemory;
    errdefer c.free(k2.ptr);
    const gcount = callocSlice(u32, r_count) orelse return error.OutOfMemory;
    defer c.free(gcount.ptr);
    const gstart = mallocSlice(u32, r_count + 1) orelse return error.OutOfMemory;
    defer c.free(gstart.ptr);
    const members = mallocSlice(u32, n) orelse return error.OutOfMemory;
    defer c.free(members.ptr);
    const fill = mallocSlice(u32, r_count) orelse return error.OutOfMemory;
    defer c.free(fill.ptr);
    const acc = callocSlice(f64, r_count) orelse return error.OutOfMemory;
    defer c.free(acc.ptr);
    const dirty = mallocSlice(u32, r_count) orelse return error.OutOfMemory;
    defer c.free(dirty.ptr);
    const off2 = mallocSlice(u32, r_count + 1) orelse return error.OutOfMemory;
    errdefer c.free(off2.ptr);

    @memset(seed[0..r_count], leiden_none);
    for (0..n) |i| {
        const r = refined[i];
        k2[r] += g.k[i];
        gcount[r] += 1;
        if (seed[r] == leiden_none) seed[r] = comm[i];
    }
    gstart[0] = 0;
    for (0..r_count) |r| {
        gstart[r + 1] = gstart[r] + gcount[r];
        fill[r] = gstart[r];
    }
    for (0..n) |i| {
        members[fill[refined[i]]] = @intCast(i);
        fill[refined[i]] += 1;
    }
    // Pass 1: count distinct inter-community neighbours per aggregate node.
    off2[0] = 0;
    for (0..r_count) |r| {
        var nd: u32 = 0;
        for (members[gstart[r]..gstart[r + 1]]) |i| {
            for (g.nbr[g.off[i]..g.off[i + 1]]) |neighbor| {
                const rb = refined[neighbor];
                if (rb == r or acc[rb] != 0.0) continue;
                acc[rb] = 1.0;
                dirty[nd] = rb;
                nd += 1;
            }
        }
        off2[r + 1] = off2[r] + nd;
        for (dirty[0..nd]) |rb| acc[rb] = 0.0;
    }
    const total = @max(off2[r_count], 1);
    const nbr2 = mallocSlice(u32, total) orelse return error.OutOfMemory;
    errdefer c.free(nbr2.ptr);
    const w2 = mallocSlice(f64, total) orelse return error.OutOfMemory;
    // Pass 2: accumulate inter-community edge weights.
    for (0..r_count) |r| {
        var nd: usize = 0;
        for (members[gstart[r]..gstart[r + 1]]) |i| {
            const lo = g.off[i];
            const hi = g.off[i + 1];
            for (g.nbr[lo..hi], g.w[lo..hi]) |neighbor, weight| {
                const rb = refined[neighbor];
                if (rb == r) continue;
                if (acc[rb] == 0.0) {
                    dirty[nd] = rb;
                    nd += 1;
                }
                acc[rb] += weight;
            }
        }
        const base = off2[r];
        for (dirty[0..nd], 0..) |rb, d| {
            nbr2[base + d] = rb;
            w2[base + d] = acc[rb];
            acc[rb] = 0.0;
        }
    }
    return .{ .n = r_count, .off = off2, .nbr = nbr2, .w = w2, .k = k2 };
}

pub export fn cbm_leiden(
    nodes_c: [*c]const i64,
    node_count: c_int,
    edges_c: [*c]const c.cbm_louvain_edge_t,
    edge_count: c_int,
    resolution: f64,
    out: *[*c]c.cbm_louvain_result_t,
    out_count: *c_int,
) c_int {
    if (node_count <= 0) {
        out.* = null;
        out_count.* = 0;
        return OK;
    }
    const n: usize = @intCast(node_count);
    const nodes = nodes_c[0..n];
    const gamma = if (resolution > 0.0) resolution else 1.0;

    const result = mallocSlice(c.cbm_louvain_result_t, n) orelse return c.CBM_NOT_FOUND;
    for (result, nodes, 0..) |*row, node, i| row.* = .{ .node_id = node, .community = @intCast(i) };
    // Every return below hands back result; until the levels run, every node
    // is its own community.
    out.* = result.ptr;
    out_count.* = node_count;

    // Build deduplicated undirected edge weights, then a CSR graph.
    const weights = louvainBuildWeights(nodes, span(edges_c, edge_count)) orelse return OK;
    const built: ?LeidenGraph = if (weights.n > 0)
        lgBuild(n, weights.si[0..weights.n], weights.di[0..weights.n], weights.w[0..weights.n]) catch null
    else
        null;
    weights.deinit();
    // No edges (or allocation failure): every node is its own community.
    var g = built orelse return OK;
    defer g.deinit();

    var twom: f64 = 0.0;
    for (g.k) |degree| twom += degree;
    if (twom <= 0.0) return OK;

    const orig = mallocSlice(u32, n) orelse return OK; // original node -> current graph node
    defer c.free(orig.ptr);
    var comm = mallocSlice(u32, n) orelse return OK;
    defer c.free(comm.ptr);
    for (orig, comm, 0..) |*o, *label, i| {
        o.* = @intCast(i);
        label.* = @intCast(i);
    }

    for (0..LEIDEN_MAX_LEVELS) |_| {
        leidenMove(&g, comm, gamma, twom);
        const c_count = leidenRelabel(comm);
        if (c_count >= g.n) break; // every node already isolated — nothing to coarsen
        const refined = mallocSlice(u32, g.n) orelse break;
        defer c.free(refined.ptr);
        const r_count = leidenRefine(&g, comm, gamma, twom, refined);
        if (r_count >= g.n) break; // refinement cannot reduce the graph further
        for (orig) |*o| o.* = refined[o.*];
        const seed = mallocSlice(u32, r_count) orelse break;
        const g2 = leidenAggregate(&g, refined, r_count, comm, seed) catch {
            c.free(seed.ptr);
            break;
        };
        g.deinit();
        g = g2;
        c.free(comm.ptr);
        comm = seed;
    }

    for (result, orig) |*row, o| row.community = @intCast(comm[o]);
    return OK;
}

pub export fn cbm_louvain(
    nodes: [*c]const i64,
    node_count: c_int,
    edges: [*c]const c.cbm_louvain_edge_t,
    edge_count: c_int,
    out: *[*c]c.cbm_louvain_result_t,
    out_count: *c_int,
) c_int {
    return cbm_leiden(nodes, node_count, edges, edge_count, 1.0, out, out_count);
}

// ── Architecture: community clusters via Leiden ─────────────────

const CBM_CLUSTER_TOP_N = 12; // report at most this many clusters
const CBM_CLUSTER_MAX_TOPNODES = 5; // representative node names per cluster
const CBM_CLUSTER_MAX_PKGS = 5; // packages listed per cluster
const CBM_CLUSTER_MIN_MEMBERS = 2; // skip singletons
const CBM_CLUSTER_NODE_CAP = 8000; // bound the work for very large graphs

/// Index of node id within the id-sorted ids[].
fn clusterIdIndex(ids: []const i64, id: i64) ?u32 {
    var lo: usize = 0;
    var hi: usize = ids.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        if (ids[mid] == id) return @intCast(mid);
        if (ids[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return null;
}

/// A distinct package list (with a per-package count).
const ClusterPkgs = struct {
    names: [CBM_CLUSTER_MAX_PKGS][*c]u8 = undefined,
    counts: [CBM_CLUSTER_MAX_PKGS]c_int = undefined,
    len: usize = 0,

    /// Append `pkg` to the list, or count it when already present.
    fn add(pkgs: *ClusterPkgs, pkg: CStr) void {
        if (pkg == null or pkg[0] == 0) return;
        for (pkgs.names[0..pkgs.len], pkgs.counts[0..pkgs.len]) |name, *count| {
            if (streq(name, pkg)) {
                count.* += 1;
                return;
            }
        }
        if (pkgs.len < pkgs.names.len) {
            pkgs.names[pkgs.len] = interop.heapStrdup(pkg);
            pkgs.counts[pkgs.len] = 1;
            pkgs.len += 1;
        }
    }
};

/// Build the cluster_info for one community into ci.
fn clusterBuildOne(
    ci: *c.cbm_cluster_info_t,
    community: u32,
    comm: []const u32,
    degree: []const c_int,
    names: []const [*c]u8,
    qns: []const [*c]u8,
    members: c_int,
    cohesion: f64,
) void {
    ci.* = .{ .id = @intCast(community), .members = members, .cohesion = cohesion };

    // Top nodes by degree.
    var top_idx: [CBM_CLUSTER_MAX_TOPNODES]usize = undefined;
    var top_deg: [CBM_CLUSTER_MAX_TOPNODES]c_int = undefined;
    var tn: usize = 0;
    for (comm, degree, 0..) |label, d, i| {
        if (label != community) continue;
        var pos = tn;
        while (pos > 0 and top_deg[pos - 1] < d) pos -= 1;
        if (pos < CBM_CLUSTER_MAX_TOPNODES) {
            var k = if (tn < CBM_CLUSTER_MAX_TOPNODES) tn else CBM_CLUSTER_MAX_TOPNODES - 1;
            while (k > pos) : (k -= 1) {
                top_idx[k] = top_idx[k - 1];
                top_deg[k] = top_deg[k - 1];
            }
            top_idx[pos] = i;
            top_deg[pos] = d;
            if (tn < CBM_CLUSTER_MAX_TOPNODES) tn += 1;
        }
    }
    if (tn > 0) {
        if (mallocSlice(CStr, tn)) |top_nodes| {
            for (top_nodes, top_idx[0..tn]) |*top, idx| top.* = interop.heapStrdup(names[idx]);
            ci.top_nodes = top_nodes.ptr;
            ci.top_node_count = @intCast(tn);
        }
    }

    // Distinct packages (+ dominant one as the label).
    var pkgs: ClusterPkgs = .{};
    for (comm, qns) |label, qn| {
        if (label == community) pkgs.add(cbm_qn_to_top_package(qn));
    }
    if (pkgs.len > 0) {
        var best: usize = 0;
        for (pkgs.counts[0..pkgs.len], 0..) |count, i| {
            if (count > pkgs.counts[best]) best = i;
        }
        ci.label = interop.heapStrdup(pkgs.names[best]);
        // The package names move into the cluster.
        if (mallocSlice(CStr, pkgs.len)) |packages| {
            for (packages, pkgs.names[0..pkgs.len]) |*package, name| package.* = name;
            ci.packages = packages.ptr;
            ci.package_count = @intCast(pkgs.len);
        } else {
            for (pkgs.names[0..pkgs.len]) |name| c.free(name);
        }
    }
    if (ci.label == null) {
        ci.label = interop.heapStrdup(if (ci.top_node_count > 0) ci.top_nodes[0] else "cluster");
    }

    if (mallocSlice(CStr, 1)) |edge_types| {
        edge_types[0] = interop.heapStrdup("CALLS");
        ci.edge_types = edge_types.ptr;
        ci.edge_type_count = 1;
    }
}

/// Comparator for sorting community indices by descending member count.
const ClusterRank = extern struct {
    comm: c_int,
    members: c_int,
};

fn clusterRankCmp(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const ca: *const ClusterRank = @ptrCast(@alignCast(a));
    const cb: *const ClusterRank = @ptrCast(@alignCast(b));
    return cb.members - ca.members;
}

fn archClusters(s: *Store, project: CStr, path: CStr, out: *ArchInfo) c_int {
    const scope = ArchScope.init(path);
    const nbase = "SELECT id, name, qualified_name, file_path FROM nodes " ++
        "WHERE project=?1 AND label IN ('Function','Method','Class')";
    const st = scope.query(
        s.db,
        nbase ++ " ORDER BY id LIMIT ?2",
        nbase ++ arch_path_scope_sql ++ " ORDER BY id LIMIT ?4",
        project,
    ) orelse return OK; // clusters are best-effort
    _ = c.sqlite3_bind_int(st, if (scope.scoped) 4 else 2, CBM_CLUSTER_NODE_CAP);

    var ids = Rows(i64).init(ST_INIT_CAP_8);
    var names = Rows([*c]u8).init(ST_INIT_CAP_8);
    var qns = Rows([*c]u8).init(ST_INIT_CAP_8);
    defer {
        freeStrings(names.items, names.len);
        freeStrings(qns.items, qns.len);
        c.free(ids.items);
    }
    var rc = c.sqlite3_step(st);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(st)) {
        ids.add().* = c.sqlite3_column_int64(st, 0);
        names.add().* = columnDup(st, 1);
        qns.add().* = columnDup(st, 2);
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(st);
        return ERR;
    }
    _ = c.sqlite3_finalize(st);
    const n: usize = @intCast(ids.len);
    if (n < CBM_CLUSTER_MIN_MEMBERS) return OK;

    // 2. Load CALLS edges with both endpoints in the node set (store indices).
    var edges = Rows(c.cbm_louvain_edge_t).init(ids.len);
    var esrc = Rows(u32).init(ids.len);
    var edst = Rows(u32).init(ids.len);
    defer {
        c.free(edges.items);
        c.free(esrc.items);
        c.free(edst.items);
    }
    const degree = callocSlice(c_int, n) orelse return OK;
    defer c.free(degree.ptr);
    var calls: [*c]StoreIdPair = null;
    var call_count: c_int = 0;
    if (storeCallsPairs(s, project, &calls, &call_count) == OK) {
        for (span(calls, call_count)) |*call| {
            const si = clusterIdIndex(ids.slice(), call.source_id) orelse continue;
            const ti = clusterIdIndex(ids.slice(), call.target_id) orelse continue;
            if (si == ti) continue;
            edges.add().* = .{ .src = ids.slice()[si], .dst = ids.slice()[ti] };
            esrc.add().* = si;
            edst.add().* = ti;
            degree[si] += 1;
            degree[ti] += 1;
        }
        c.free(calls);
    }

    // 3. Community detection.
    var res: [*c]c.cbm_louvain_result_t = null;
    var rn: c_int = 0;
    const leiden_rc = cbm_leiden(ids.items, ids.len, edges.items, edges.len, 1.0, &res, &rn);
    defer c.free(res);
    if (leiden_rc != OK or res == null or rn != ids.len) return OK;
    const comm = mallocSlice(u32, n) orelse return OK;
    defer c.free(comm.ptr);
    var community_count: usize = 0;
    for (comm, res[0..n]) |*label, *row| {
        label.* = @intCast(row.community);
        community_count = @max(community_count, @as(usize, label.*) + 1);
    }

    // 4. Members + cohesion (internal vs boundary edges) per community.
    const members = callocSlice(c_int, community_count) orelse return OK;
    defer c.free(members.ptr);
    const internal = callocSlice(c_int, community_count) orelse return OK;
    defer c.free(internal.ptr);
    const boundary = callocSlice(c_int, community_count) orelse return OK;
    defer c.free(boundary.ptr);
    for (comm) |label| members[label] += 1;
    for (esrc.slice(), edst.slice()) |si, ti| {
        const cs = comm[si];
        const cd = comm[ti];
        if (cs == cd) {
            internal[cs] += 1;
        } else {
            boundary[cs] += 1;
            boundary[cd] += 1;
        }
    }

    // 5. Rank communities by size, take the top N non-singletons.
    const rank = mallocSlice(ClusterRank, community_count) orelse return OK;
    defer c.free(rank.ptr);
    for (rank, members, 0..) |*entry, member_count, i| entry.* = .{ .comm = @intCast(i), .members = member_count };
    if (community_count > 1) c.qsort(rank.ptr, community_count, @sizeOf(ClusterRank), clusterRankCmp);

    const clusters = mallocSlice(c.cbm_cluster_info_t, CBM_CLUSTER_TOP_N) orelse return OK;
    var cc: usize = 0;
    for (rank) |*entry| {
        if (cc >= CBM_CLUSTER_TOP_N) break;
        const community: u32 = @intCast(entry.comm);
        if (members[community] < CBM_CLUSTER_MIN_MEMBERS) break; // sorted desc — the rest are singletons too
        const denom: f64 = @floatFromInt(internal[community] + boundary[community]);
        const cohesion = if (denom > 0) @as(f64, @floatFromInt(internal[community])) / denom else 0.0;
        clusterBuildOne(&clusters[cc], community, comm, degree, names.slice(), qns.slice(), members[community], cohesion);
        cc += 1;
    }
    out.clusters = clusters.ptr;
    out.cluster_count = @intCast(cc);
    return OK;
}

// ── GetArchitecture dispatch ────────────────────────────────────

/// "overview" = compact architecture summary: every aspect EXCEPT the large
/// per-file listing (file_tree), which alone dominates the payload on real
/// repos and can push the MCP response past the output cap. Declared in
/// store.h and shared with aspect_wanted in src/mcp/mcp.zig so the store-side
/// DB gate and the MCP-side serialization gate cannot drift.
pub export fn cbm_store_arch_aspect_in_overview(name: CStr) bool {
    return !streq(name, "file_tree");
}

fn wantAspect(aspects: [*c]const CStr, aspect_count: c_int, name: CStr) bool {
    if (aspects == null or aspect_count == 0) return true;
    for (span(aspects, aspect_count)) |aspect| {
        if (streq(aspect, "all")) return true;
        if (streq(aspect, "overview") and cbm_store_arch_aspect_in_overview(name)) return true;
        if (streq(aspect, name)) return true;
    }
    return false;
}

pub export fn cbm_store_get_architecture(
    s_c: ?*Store,
    project_c: CStr,
    path: CStr,
    aspects: [*c]CStr,
    aspect_count: c_int,
    out: *ArchInfo,
) c_int {
    out.* = std.mem.zeroes(ArchInfo);
    var project = project_c;
    var rc = storeLegacyScope(s_c, &project);
    if (rc != OK) return rc;
    const s = s_c orelse return ERR;

    if (wantAspect(aspects, aspect_count, "languages")) {
        rc = archLanguages(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "packages")) {
        rc = archPackages(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "entry_points")) {
        rc = archEntryPoints(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "routes")) {
        rc = archRoutes(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "hotspots")) {
        rc = archHotspots(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "boundaries")) {
        rc = archBoundaries(s, project, path, &out.boundaries, &out.boundary_count);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "layers")) {
        rc = archLayers(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "file_tree")) {
        rc = archFileTree(s, project, path, out);
        if (rc != OK) return rc;
    }
    if (wantAspect(aspects, aspect_count, "clusters")) {
        rc = archClusters(s, project, path, out);
        if (rc != OK) return rc;
    }

    return OK;
}

pub export fn cbm_store_architecture_free(out_c: ?*ArchInfo) void {
    const out = out_c orelse return;
    for (span(out.languages, out.language_count)) |*language| interop.strFree(&language.language);
    c.free(out.languages);
    for (span(out.packages, out.package_count)) |*package| interop.strFree(&package.name);
    c.free(out.packages);
    for (span(out.entry_points, out.entry_point_count)) |*entry| {
        interop.strFree(&entry.name);
        interop.strFree(&entry.qualified_name);
        interop.strFree(&entry.file);
    }
    c.free(out.entry_points);
    for (span(out.routes, out.route_count)) |*route| {
        interop.strFree(&route.method);
        interop.strFree(&route.path);
        interop.strFree(&route.handler);
    }
    c.free(out.routes);
    for (span(out.hotspots, out.hotspot_count)) |*hotspot| {
        interop.strFree(&hotspot.name);
        interop.strFree(&hotspot.qualified_name);
    }
    c.free(out.hotspots);
    for (span(out.boundaries, out.boundary_count)) |*boundary| {
        interop.strFree(&boundary.from);
        interop.strFree(&boundary.to);
    }
    c.free(out.boundaries);
    for (span(out.services, out.service_count)) |*service| {
        interop.strFree(&service.from);
        interop.strFree(&service.to);
        interop.strFree(&service.type);
    }
    c.free(out.services);
    for (span(out.layers, out.layer_count)) |*layer| {
        interop.strFree(&layer.name);
        interop.strFree(&layer.layer);
        interop.strFree(&layer.reason);
    }
    c.free(out.layers);
    for (span(out.clusters, out.cluster_count)) |*cluster| {
        interop.strFree(&cluster.label);
        freeConstStrings(cluster.top_nodes, cluster.top_node_count);
        freeConstStrings(cluster.packages, cluster.package_count);
        freeConstStrings(cluster.edge_types, cluster.edge_type_count);
    }
    c.free(out.clusters);
    for (span(out.file_tree, out.file_tree_count)) |*entry| {
        interop.strFree(&entry.path);
        interop.strFree(&entry.type);
    }
    c.free(out.file_tree);
    out.* = std.mem.zeroes(ArchInfo);
}

// ── Project summaries ───────────────────────────────────────────

pub export fn cbm_store_project_summary_export(s_c: ?*Store, project: CStr, out_c: ?*c.cbm_project_summary_export_t) c_int {
    const s = s_c orelse return ERR;
    const out = out_c orelse return ERR;
    if (project == null) return ERR;
    out.* = .{};
    const stmt = prepareOne(s.db, "SELECT summary, source_hash, created_at, updated_at " ++
        "FROM project_summaries WHERE project=?1") orelse {
        storeSetErrorSqlite(s, "project_summary_export_prepare");
        return ERR;
    };
    _ = bindText(stmt, 1, project);
    const step = c.sqlite3_step(stmt);
    if (step == c.SQLITE_DONE) {
        _ = c.sqlite3_finalize(stmt);
        return NOT_FOUND;
    }
    if (step != c.SQLITE_ROW) {
        _ = c.sqlite3_finalize(stmt);
        storeSetErrorSqlite(s, "project_summary_export_step");
        return ERR;
    }
    out.* = .{
        .summary = columnDup(stmt, 0),
        .source_hash = columnDup(stmt, 1),
        .created_at = columnDup(stmt, 2),
        .updated_at = columnDup(stmt, 3),
    };
    _ = c.sqlite3_finalize(stmt);
    if (out.summary == null or out.source_hash == null or out.created_at == null or out.updated_at == null) {
        cbm_store_project_summary_export_free(out);
        storeSetError(s, "project summary export allocation failed");
        return ERR;
    }
    return OK;
}

pub export fn cbm_store_project_summary_export_free(row_c: ?*c.cbm_project_summary_export_t) void {
    const row = row_c orelse return;
    c.free(row.summary);
    c.free(row.source_hash);
    c.free(row.created_at);
    c.free(row.updated_at);
    row.* = .{};
}

// ── Architecture doc discovery ──────────────────────────────────

pub export fn cbm_store_find_architecture_docs(s_c: ?*Store, project: CStr, out: *[*c][*c]u8, count: *c_int) c_int {
    const s = s_c orelse return ERR;
    const stmt = prepareOne(s.db, "SELECT file_path FROM nodes WHERE project=?1 AND label='File' " ++
        "AND (file_path LIKE '%ARCHITECTURE.md' OR file_path LIKE '%ADR.md' " ++
        "OR file_path LIKE '%DECISIONS.md' OR file_path LIKE 'docs/adr/%' " ++
        "OR file_path LIKE 'doc/adr/%' OR file_path LIKE 'adr/%') " ++
        "ORDER BY file_path LIMIT 20") orelse {
        storeSetErrorSqlite(s, "find_arch_docs");
        return ERR;
    };
    _ = bindText(stmt, 1, project);

    var arr = Rows([*c]u8).init(ST_INIT_CAP_8);
    var rc = c.sqlite3_step(stmt);
    while (rc == c.SQLITE_ROW) : (rc = c.sqlite3_step(stmt)) {
        arr.add().* = columnDup(stmt, 0);
    }
    if (rc != c.SQLITE_DONE) {
        storeSetErrorSqlite(s, "row scan aborted");
        _ = c.sqlite3_finalize(stmt);
        freeStrings(arr.items, arr.len);
        out.* = null;
        count.* = 0;
        return ERR;
    }
    _ = c.sqlite3_finalize(stmt);
    out.* = arr.items;
    count.* = arr.len;
    return OK;
}

// ── Memory management ───────────────────────────────────────────

pub export fn cbm_node_free_fields(n: *c.cbm_node_t) void {
    interop.strFree(&n.project);
    interop.strFree(&n.label);
    interop.strFree(&n.name);
    interop.strFree(&n.qualified_name);
    interop.strFree(&n.file_path);
    interop.strFree(&n.properties_json);
}

pub export fn cbm_store_free_nodes(nodes: [*c]c.cbm_node_t, count: c_int) void {
    if (nodes == null) return;
    for (span(nodes, count)) |*node| cbm_node_free_fields(node);
    c.free(nodes);
}

pub export fn cbm_store_free_edges(edges: [*c]c.cbm_edge_t, count: c_int) void {
    if (edges == null) return;
    for (span(edges, count)) |*edge| {
        interop.strFree(&edge.project);
        interop.strFree(&edge.type);
        interop.strFree(&edge.properties_json);
    }
    c.free(edges);
}

pub export fn cbm_project_free_fields(p: *c.cbm_project_t) void {
    interop.strFree(&p.name);
    interop.strFree(&p.indexed_at);
    interop.strFree(&p.root_path);
}

pub export fn cbm_store_free_projects(projects: [*c]c.cbm_project_t, count: c_int) void {
    if (projects == null) return;
    for (span(projects, count)) |*project| cbm_project_free_fields(project);
    c.free(projects);
}

pub export fn cbm_store_free_file_hashes(hashes: [*c]c.cbm_file_hash_t, count: c_int) void {
    if (hashes == null) return;
    for (span(hashes, count)) |*hash| {
        interop.strFree(&hash.project);
        interop.strFree(&hash.rel_path);
        interop.strFree(&hash.sha256);
    }
    c.free(hashes);
}

// ── Vector search ───────────────────────────────────────────────

const VectorResult = c.cbm_vector_result_t;
const VectorMetrics = c.cbm_vector_search_metrics_t;
const VectorSession = c.cbm_zova_vector_session_t;

pub export fn cbm_store_count_vectors(s_c: ?*Store, project: CStr) c_int {
    const s = s_c orelse return 0;
    if (project == null) return 0;
    const stmt = prepareOne(s.db, "SELECT count(*) FROM node_vectors WHERE project = ?1") orelse return 0;
    _ = c.sqlite3_bind_text(stmt, 1, project, -1, null);
    var count: c_int = 0;
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) count = c.sqlite3_column_int(stmt, 0);
    _ = c.sqlite3_finalize(stmt);
    return count;
}

fn vectorResultFreeFields(result: *const VectorResult) void {
    c.free(result.project);
    c.free(result.name);
    c.free(result.qualified_name);
    c.free(result.file_path);
    c.free(result.label);
}

pub export fn cbm_store_free_vector_results(results: [*c]VectorResult, count: c_int) void {
    if (results == null) return;
    for (span(results, count)) |*result| vectorResultFreeFields(result);
    c.free(results);
}

// Per-keyword scoring: score each keyword independently against each node
// vector, then combine using min(cosine_k) across keywords.  This ensures
// ALL keywords must be relevant, not just the average.
const VS_VEC_DIM = 768;
const VS_SPARSE_NNZE = 8;
const VS_RI_SEED = 0x52494E44;
const VS_MAX_KW = 32;
const VS_STR_BUF = 16;

/// One int8-quantized query or node vector.
const KwVec = [VS_VEC_DIM]i8;
const FloatVec = [VS_VEC_DIM]f32;

/// De-quantizes an int8 vector into out.
fn vsDequantize(values: *const KwVec, out: *FloatVec) void {
    for (out, values) |*value, quantized| value.* = @as(f32, @floatFromInt(quantized)) / CBM_STORE_INT8_MAX;
}

fn vsWorkspaceVectorState(s_c: ?*Store, project: CStr, model: []u8, out_dimensions: *c_int, out_generation: *i64) c_int {
    const s = s_c orelse return ERR;
    if (!s.zova_workspace_query or project == null or model.len == 0) return ERR;
    const stmt = prepareOne(s.db, "SELECT s.model_fingerprint,s.vector_dimensions,s.generation " ++
        "FROM cbm_workspace_index_state_v1 s INNER JOIN cbm_projects_v1 p " ++
        "ON p.workspace_key=s.workspace_key JOIN cbm_workspace_registry r " ++
        "ON r.workspace_key=s.workspace_key WHERE r.workspace_id=?1 AND p.project=?2 " ++
        "AND s.generation>0") orelse return ERR;
    _ = bindText(stmt, 1, &s.zova_graph_workspace_id);
    _ = bindText(stmt, 2, project);
    var rc: c_int = NOT_FOUND;
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const stored_model = columnText(stmt, 0);
        const dimensions = c.sqlite3_column_int(stmt, 1);
        const generation = c.sqlite3_column_int64(stmt, 2);
        if (stored_model != null and stored_model[0] != 0 and dimensions > 0 and generation > 0) {
            const text = std.mem.span(stored_model);
            if (text.len < model.len) {
                @memcpy(model[0..text.len], text);
                model[text.len] = 0;
                out_dimensions.* = dimensions;
                out_generation.* = generation;
                rc = OK;
            }
        }
    }
    _ = c.sqlite3_finalize(stmt);
    return rc;
}

fn vsLoadWorkspaceNativeTokenVector(
    s: *Store,
    project: CStr,
    token: CStr,
    out: *FloatVec,
    out_found: *bool,
    out_generation_mismatch: *bool,
) c_int {
    var model: [c.CBM_SZ_256]u8 = undefined;
    var dimensions: c_int = 0;
    var generation: i64 = 0;
    out_found.* = false;
    out_generation_mismatch.* = false;
    const state_rc = vsWorkspaceVectorState(s, project, &model, &dimensions, &generation);
    if (state_rc != OK or dimensions != VS_VEC_DIM) return ERR;
    const session = vsGetZovaVectorSession(s, s.db_path);
    var session_generation: i64 = 0;
    if (session == null or c.cbm_zova_vector_session_generation(session, &session_generation) != 0 or
        session_generation != generation)
    {
        out_generation_mismatch.* = true;
        return ERR;
    }
    var values: KwVec = undefined;
    var found = false;
    if (c.cbm_zova_vector_session_get_workspace_token_i8(
        session,
        &s.zova_graph_workspace_id,
        &model,
        dimensions,
        token,
        &values,
        values.len,
        &found,
    ) != 0) return ERR;
    if (found) vsDequantize(&values, out);
    out_found.* = found;
    return OK;
}

/// Try to look up an enriched int8 vector for `token` in the token_vectors
/// table.  On success, writes the de-quantized float representation to
/// `out` and returns true.
fn vsLoadEnrichedVector(s: *Store, project: CStr, token: CStr, out: *FloatVec) bool {
    const tv_stmt = prepareOne(s.db, "SELECT vector, idf FROM token_vectors" ++
        " WHERE project = ?1 AND token = ?2 LIMIT 1") orelse return false;
    var found = false;
    _ = c.sqlite3_bind_text(tv_stmt, 1, project, -1, null);
    _ = c.sqlite3_bind_text(tv_stmt, 2, token, -1, null);
    if (c.sqlite3_step(tv_stmt) == c.SQLITE_ROW) {
        const vec: ?*const KwVec = @ptrCast(c.sqlite3_value_blob(c.sqlite3_column_value(tv_stmt, 0)));
        const vec_len = c.sqlite3_column_bytes(tv_stmt, 0);
        if (vec != null and vec_len == VS_VEC_DIM) {
            vsDequantize(vec.?, out);
            found = true;
        }
    }
    _ = c.sqlite3_finalize(tv_stmt);
    return found;
}

/// Sparse-random-index fallback for tokens not present in the enriched table.
/// std.hash.XxHash3 gives the values of the vendored XXH3_64bits.
fn vsFillSparseRandom(token: CStr, out: *FloatVec) void {
    const seed = std.hash.XxHash3.hash(0, std.mem.span(token));
    for (0..VS_SPARSE_NNZE) |n| {
        const i: c_int = @intCast(n);
        const h = std.hash.XxHash3.hash(seed +% VS_RI_SEED, std.mem.asBytes(&i));
        const pos: usize = @intCast(h % VS_VEC_DIM);
        out[pos] += if (h & 1 != 0) 1.0 else -1.0;
    }
}

/// Clamp one float into the int8 representable range.
fn vsClampInt8(v: f32) i8 {
    if (v > CBM_STORE_INT8_MAX) return 127;
    if (v < -CBM_STORE_INT8_MAX) return -127;
    return @intFromFloat(v);
}

/// Normalize + int8 quantize a float vector.  Returns false if the vector is
/// effectively zero (magnitude below epsilon), in which case `dst` is left in
/// an indeterminate state and the caller should skip this keyword.
fn vsNormalizeAndQuantize(src: *const FloatVec, dst: *KwVec) bool {
    var mag: f32 = 0.0;
    for (src) |value| mag += value * value;
    mag = @sqrt(mag);
    if (mag < CBM_STORE_DENOM_EPS_F) return false;
    const inv = 1.0 / mag;
    for (dst, src) |*quantized, value| quantized.* = vsClampInt8(value * inv * CBM_STORE_INT8_MAX);
    return true;
}

/// Build int8 query vectors for each keyword into kw_vecs and stores how many
/// were built (may be less than the keyword count when some keywords produce
/// zero-magnitude vectors).
fn vsBuildKeywordVectors(
    s: *Store,
    project: CStr,
    keywords: []const CStr,
    kw_vecs: *[VS_MAX_KW]KwVec,
    out_actual_kw: *usize,
    metrics: ?*VectorMetrics,
) c_int {
    var actual_kw: usize = 0;
    for (keywords) |keyword| {
        if (actual_kw >= VS_MAX_KW) break;
        if (keyword == null or keyword[0] == 0) continue;
        var kw_f = std.mem.zeroes(FloatVec);
        var found = false;
        if (s.zova_workspace_query) {
            var generation_mismatch = false;
            if (vsLoadWorkspaceNativeTokenVector(s, project, keyword, &kw_f, &found, &generation_mismatch) != OK) {
                if (metrics) |m| {
                    if (generation_mismatch) m.generation_mismatch = true;
                }
                return ERR;
            }
        } else {
            found = vsLoadEnrichedVector(s, project, keyword, &kw_f);
        }
        if (!found) vsFillSparseRandom(keyword, &kw_f);
        if (vsNormalizeAndQuantize(&kw_f, &kw_vecs[actual_kw])) actual_kw += 1;
    }
    out_actual_kw.* = actual_kw;
    return OK;
}

/// Compute the per-keyword min cosine score between a node's int8 vector and
/// each of the query vectors.  Returns 0.0 if the node vector is unavailable
/// or mis-sized.
fn vsMinCosineScore(node_vec_opt: ?*const KwVec, node_vec_len: c_int, kw_vecs: []const KwVec) f64 {
    const node_vec = node_vec_opt orelse return 0.0;
    if (node_vec_len != VS_VEC_DIM) return 0.0;
    var min_score: f64 = 1.0;
    for (kw_vecs) |*kw_vec| {
        var dot: i32 = 0;
        var ma: i32 = 0;
        var mb: i32 = 0;
        for (kw_vec, node_vec) |a, b| {
            dot += @as(i32, a) * @as(i32, b);
            ma += @as(i32, a) * @as(i32, a);
            mb += @as(i32, b) * @as(i32, b);
        }
        const denom = @sqrt(@as(f64, @floatFromInt(ma))) * @sqrt(@as(f64, @floatFromInt(mb)));
        const cos_k = if (denom > CBM_STORE_DENOM_EPS_D) @as(f64, @floatFromInt(dot)) / denom else 0.0;
        if (cos_k < min_score) min_score = cos_k;
    }
    return min_score;
}

/// strdup of text, or of "" for NULL.
fn dupOrEmpty(text: CStr) [*c]u8 {
    return c.strdup(if (text != null) text else "");
}

/// Append one candidate row read from the scan statement into the result
/// vector.  Grows the results array geometrically on demand.  Returns false
/// on allocation failure.
fn vsAppendResult(results: *[*c]VectorResult, count: *c_int, cap: *c_int, stmt: ?*Stmt, kw_vecs: []const KwVec) bool {
    if (count.* >= cap.*) {
        const nc = if (cap.* < c.CBM_SZ_16) c.CBM_SZ_16 else cap.* * 2;
        const grown = interop.reallocN(VectorResult, results.*, @intCast(nc));
        if (grown == null) return false;
        results.* = grown;
        cap.* = nc;
    }
    const idx: usize = @intCast(count.*);
    count.* += 1;
    const node_vec: ?*const KwVec = @ptrCast(c.sqlite3_column_blob(stmt, 6));
    const node_vec_len = c.sqlite3_column_bytes(stmt, 6);
    results.*[idx] = .{
        .node_id = c.sqlite3_column_int64(stmt, 0),
        .name = dupOrEmpty(columnText(stmt, 1)),
        .qualified_name = dupOrEmpty(columnText(stmt, 2)),
        .file_path = dupOrEmpty(columnText(stmt, 3)),
        .label = dupOrEmpty(columnText(stmt, 4)),
        .score = vsMinCosineScore(node_vec, node_vec_len, kw_vecs),
    };
    return true;
}

fn vsSortAndTrim(results_c: [*c]VectorResult, count: *c_int, limit: c_int) void {
    const results = span(results_c, count.*);
    for (0..results.len) |i| {
        for (i + 1..results.len) |j| {
            const higher_score = results[j].score > results[i].score;
            const stable_tie = results[j].score == results[i].score and
                c.strcmp(safeStr(results[j].qualified_name), safeStr(results[i].qualified_name)) < 0;
            if (higher_score or stable_tie) std.mem.swap(VectorResult, &results[i], &results[j]);
        }
    }

    const final_limit: usize = @intCast(if (limit > 0) limit else c.CBM_SZ_16);
    if (results.len > final_limit) {
        for (results[final_limit..]) |*result| vectorResultFreeFields(result);
        count.* = @intCast(final_limit);
    }
}

fn vsElapsedMs(start: *const c.struct_timespec, end: *const c.struct_timespec) f64 {
    return (@as(f64, @floatFromInt(end.tv_sec - start.tv_sec)) * 1000.0) +
        (@as(f64, @floatFromInt(end.tv_nsec - start.tv_nsec)) / 1000000.0);
}

fn vsRecordElapsed(start: *const c.struct_timespec, out_ms: *f64) void {
    var end: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &end);
    out_ms.* = vsElapsedMs(start, &end);
}

fn storeCloseZovaVectorSession(s: *Store) void {
    c.cbm_zova_vector_session_close(s.zova_vector_session);
    s.zova_vector_session = null;
    c.free(s.zova_vector_session_path);
    s.zova_vector_session_path = null;
    s.zova_vector_session_stat_valid = false;
}

/// st_mtime, which POSIX defines as a macro over a timespec field.
fn statMtime(st: *const c.struct_stat) i64 {
    if (builtin.os.tag.isDarwin()) return st.st_mtimespec.tv_sec;
    if (builtin.os.tag == .windows) return st.st_mtime;
    return st.st_mtim.tv_sec;
}

fn vsZovaSessionMatches(s: *const Store, zova_path: CStr, st: *const c.struct_stat) bool {
    return s.zova_vector_session != null and s.zova_vector_session_path != null and
        s.zova_vector_session_stat_valid and streq(s.zova_vector_session_path, zova_path) and
        s.zova_vector_session_stat.st_dev == st.st_dev and
        s.zova_vector_session_stat.st_ino == st.st_ino and
        s.zova_vector_session_stat.st_size == st.st_size and
        statMtime(&s.zova_vector_session_stat) == statMtime(st);
}

fn vsGetZovaVectorSession(s: *Store, zova_path: CStr) ?*VectorSession {
    var st: c.struct_stat = undefined;
    if (zova_path == null or c.stat(zova_path, &st) != 0) return null;
    if (vsZovaSessionMatches(s, zova_path, &st)) return s.zova_vector_session;
    storeCloseZovaVectorSession(s);
    const session = c.cbm_zova_vector_session_open(zova_path);
    const path_copy: [*c]u8 = if (session != null) interop.heapStrdup(zova_path) else null;
    if (session == null or path_copy == null) {
        c.cbm_zova_vector_session_close(session);
        return null;
    }
    s.zova_vector_session = session;
    s.zova_vector_session_path = path_copy;
    s.zova_vector_session_stat = st;
    s.zova_vector_session_stat_valid = true;
    return session;
}

fn storeCloseZovaGraphSession(s: *Store) void {
    c.cbm_zova_graph_session_close(s.zova_graph_session);
    s.zova_graph_session = null;
}

fn storeClearZovaGraphWorkspace(s: *Store) void {
    c.free(s.zova_graph_workspace_root);
    s.zova_graph_workspace_root = null;
    s.zova_graph_workspace_id[0] = 0;
}

/// The positive node key a vector hit names; null when it is not one.
fn vsHitKey(vector_id: [*c]const u8) ?i64 {
    if (vector_id == null) return null;
    var end: [*c]u8 = null;
    const key = c.strtoll(vector_id, &end, 10);
    if (end == vector_id or end[0] != 0 or key <= 0) return null;
    return key;
}

/// True for the node labels vector search reports.
fn vsIsCodeLabel(label: CStr) bool {
    return label != null and (streq(label, "Function") or streq(label, "Method") or streq(label, "Class"));
}

const vs_collection_max = c.CBM_ZOVA_WORKSPACE_ID_MAX + 384;

fn vsTryZovaWorkspaceNativeVectorSearch(
    s: *Store,
    project: CStr,
    kw_vecs: []const KwVec,
    limit: c_int,
    out: *[*c]VectorResult,
    out_count: *c_int,
    metrics: ?*VectorMetrics,
) c_int {
    if (!s.zova_workspace_query) return NOT_FOUND;

    var model: [c.CBM_SZ_256]u8 = undefined;
    var vector_dimensions: c_int = 0;
    var generation: i64 = 0;
    if (vsWorkspaceVectorState(s, project, &model, &vector_dimensions, &generation) != OK or
        vector_dimensions != VS_VEC_DIM)
    {
        return ERR;
    }
    const session = vsGetZovaVectorSession(s, s.db_path);
    var session_generation: i64 = 0;
    if (session == null or c.cbm_zova_vector_session_generation(session, &session_generation) != 0 or
        session_generation != generation)
    {
        if (metrics) |m| m.generation_mismatch = true;
        return ERR;
    }
    var collection: [vs_collection_max]u8 = undefined;
    if (c.cbm_zova_workspace_node_vector_collection_name(
        &s.zova_graph_workspace_id,
        &model,
        vector_dimensions,
        &collection,
        collection.len,
    ) != 0) return ERR;

    const requested_limit = if (limit > 0) limit else c.CBM_SZ_16;
    const fetch_limit = requested_limit *| 5;
    var started: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &started);
    var hits: [*c]c.cbm_zova_vector_hit_t = null;
    var hit_count: c_int = 0;
    if (c.cbm_zova_vector_session_search_collection_i8(
        session,
        &collection,
        &kw_vecs[0],
        @intCast(kw_vecs.len),
        vector_dimensions,
        fetch_limit,
        fetch_limit,
        &hits,
        &hit_count,
    ) != 0) return ERR;
    defer c.cbm_zova_vector_hits_free(hits, hit_count);
    if (metrics) |m| {
        m.used_zova = true;
        m.zova_native_multi_query = kw_vecs.len > 1;
        m.zova_fetch_limit = fetch_limit;
        vsRecordElapsed(&started, &m.zova_prefetch_ms);
    }
    if (hit_count == 0) return OK;

    if (s.zova_repository == null) return ERR;
    const hit_n: usize = @intCast(hit_count);
    const node_keys = callocSlice(i64, hit_n) orelse return ERR;
    defer c.free(node_keys.ptr);
    for (node_keys, hits[0..hit_n]) |*node_key, *hit| {
        node_key.* = vsHitKey(hit.vector_id) orelse return ERR;
    }
    const results = callocSlice(VectorResult, hit_n) orelse return ERR;
    var nodes: [*c]c.cbm_node_t = null;
    if (c.cbm_zova_repository_find_nodes_by_keys(
        s.zova_repository,
        &s.zova_graph_workspace_id,
        node_keys.ptr,
        hit_count,
        &nodes,
    ) != OK) {
        c.free(results.ptr);
        return ERR;
    }
    var count: c_int = 0;
    var rc: c_int = OK;
    for (nodes[0..hit_n], hits[0..hit_n]) |*node, *hit| {
        if (!vsIsCodeLabel(node.label)) {
            cbm_node_free_fields(node);
            continue;
        }
        const item = &results[@intCast(count)];
        count += 1;
        // The node's strings move into the result.
        item.* = .{
            .node_id = node.id,
            .name = @constCast(node.name),
            .qualified_name = @constCast(node.qualified_name),
            .file_path = @constCast(node.file_path),
            .label = @constCast(node.label),
            .score = hit.score,
        };
        node.name = null;
        node.qualified_name = null;
        node.file_path = null;
        node.label = null;
        cbm_node_free_fields(node);
        if (item.name == null or item.qualified_name == null or item.file_path == null or item.label == null) {
            rc = ERR;
            break;
        }
    }
    cbm_store_free_nodes(nodes, hit_count);
    if (rc != OK) {
        cbm_store_free_vector_results(results.ptr, count);
        return ERR;
    }
    vsSortAndTrim(results.ptr, &count, limit);
    out.* = results.ptr;
    out_count.* = count;
    return OK;
}

fn vsTryZovaVectorSearch(
    s: *Store,
    project: CStr,
    kw_vecs: []const KwVec,
    limit: c_int,
    out: *[*c]VectorResult,
    out_count: *c_int,
    metrics: ?*VectorMetrics,
) c_int {
    if (!c.cbm_zova_build_enabled() or s.db_path == null or !s.zova_workspace_query) return NOT_FOUND;
    return vsTryZovaWorkspaceNativeVectorSearch(s, project, kw_vecs, limit, out, out_count, metrics);
}

pub export fn cbm_store_vector_search_ex(
    s_c: ?*Store,
    project: CStr,
    keywords: [*c]CStr,
    keyword_count: c_int,
    limit: c_int,
    out: *[*c]VectorResult,
    out_count: *c_int,
    metrics: ?*VectorMetrics,
) c_int {
    if (metrics) |m| m.* = std.mem.zeroes(VectorMetrics);
    out.* = null;
    out_count.* = 0;
    const s = s_c orelse return ERR;
    if (project == null or keywords == null or keyword_count <= 0) return ERR;

    var stage_start: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &stage_start);
    var kw_storage: [VS_MAX_KW]KwVec = undefined;
    var actual_kw: usize = 0;
    if (vsBuildKeywordVectors(s, project, span(keywords, keyword_count), &kw_storage, &actual_kw, metrics) != OK) {
        return ERR;
    }
    if (metrics) |m| vsRecordElapsed(&stage_start, &m.query_vector_build_ms);
    if (actual_kw == 0) return OK;
    const kw_vecs = kw_storage[0..actual_kw];

    const zova_rc = vsTryZovaVectorSearch(s, project, kw_vecs, limit, out, out_count, metrics);
    if (zova_rc == OK) return OK;
    if (zova_rc == ERR) return ERR;
    if (s.zova_workspace_query) return ERR;

    // Scan all node vectors, compute per-keyword cosine, take min.
    // We use the FIRST keyword as the SQL sort (for top-K pre-filter),
    // then re-score with min across all keywords in the append helper.
    var stmt: ?*Stmt = null;
    if (prepare(s.db, "SELECT n.id,n.name,n.qualified_name,n.file_path,n.label," ++
        "cbm_cosine_i8(v.vector,?1) AS score,v.vector FROM node_vectors v " ++
        "INNER JOIN nodes n ON n.id=v.node_id WHERE v.project=?2 " ++
        "AND n.label IN ('Function','Method','Class') ORDER BY score DESC LIMIT ?3", &stmt) != c.SQLITE_OK)
    {
        _ = c.fprintf(interop.stderr(), "vector_search: %s\n", c.sqlite3_errmsg(s.db));
        return ERR;
    }

    // Use first keyword for SQL pre-filter, fetch more candidates for re-ranking
    const fetch_limit = (if (limit > 0) limit else c.CBM_SZ_16) *| 5;
    _ = c.sqlite3_bind_blob(stmt, 1, &kw_vecs[0], VS_VEC_DIM, null);
    _ = c.sqlite3_bind_text(stmt, 2, project, -1, null);
    _ = c.sqlite3_bind_int(stmt, 3, fetch_limit);

    {
        var kw_buf: [VS_STR_BUF]u8 = undefined;
        var fl_buf: [VS_STR_BUF]u8 = undefined;
        _ = interop.bprint(&kw_buf, "{d}", .{actual_kw});
        _ = interop.bprint(&fl_buf, "{d}", .{fetch_limit});
        interop.logInfo("vector_search.exec", .{ "kw_count", @as(CStr, &kw_buf), "fetch_limit", @as(CStr, &fl_buf), "project", project });
    }

    var results: [*c]VectorResult = null;
    var count: c_int = 0;
    var cap: c_int = 0;
    var step_rc = c.sqlite3_step(stmt);
    while (step_rc == c.SQLITE_ROW) : (step_rc = c.sqlite3_step(stmt)) {
        if (!vsAppendResult(&results, &count, &cap, stmt, kw_vecs)) break;
    }

    if (step_rc != c.SQLITE_DONE) {
        var rc_buf: [VS_STR_BUF]u8 = undefined;
        _ = interop.bprint(&rc_buf, "{d}", .{step_rc});
        interop.logWarn("vector_search.step_error", .{ "rc", @as(CStr, &rc_buf), "msg", c.sqlite3_errmsg(s.db) });
    }
    {
        var cnt_buf: [VS_STR_BUF]u8 = undefined;
        _ = interop.bprint(&cnt_buf, "{d}", .{count});
        interop.logInfo("vector_search.done", .{ "candidates", @as(CStr, &cnt_buf) });
    }
    _ = c.sqlite3_finalize(stmt);

    vsSortAndTrim(results, &count, limit);

    out.* = results;
    out_count.* = count;
    return OK;
}

pub export fn cbm_store_vector_search(
    s: ?*Store,
    project: CStr,
    keywords: [*c]CStr,
    keyword_count: c_int,
    limit: c_int,
    out: *[*c]VectorResult,
    out_count: *c_int,
) c_int {
    return cbm_store_vector_search_ex(s, project, keywords, keyword_count, limit, out, out_count, null);
}

fn multiVectorResultCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    const a: *const VectorResult = @ptrCast(@alignCast(left));
    const b: *const VectorResult = @ptrCast(@alignCast(right));
    if (a.score > b.score) return -1;
    if (a.score < b.score) return 1;
    const by_project = c.strcmp(a.project, b.project);
    if (by_project != 0) return by_project;
    const by_qn = c.strcmp(a.qualified_name, b.qualified_name);
    if (by_qn != 0) return by_qn;
    return order(a.node_id, b.node_id);
}

/// The results of every workspace searched so far.
const CombinedResults = struct {
    items: [*c]VectorResult = null,
    count: c_int = 0,
    capacity: c_int = 0,
};

/// Searches one workspace and appends its hits, hydrated from the store, to
/// combined.
fn multiVectorSearchWorkspace(
    session: ?*VectorSession,
    hydrate: ?*Stmt,
    workspace: *const c.cbm_zova_vector_workspace_t,
    keywords: []const CStr,
    needed: c_int,
    combined: *CombinedResults,
) c_int {
    if (workspace.workspace_id == null or workspace.selector == null or workspace.model_fingerprint == null or
        workspace.vector_dimensions != VS_VEC_DIM)
    {
        return ERR;
    }
    var query_vectors: [VS_MAX_KW]KwVec = undefined;
    var query_count: usize = 0;
    for (keywords) |keyword| {
        if (query_count >= VS_MAX_KW) break;
        if (keyword == null or keyword[0] == 0) continue;
        var token_vector: KwVec = undefined;
        var found = false;
        var values = std.mem.zeroes(FloatVec);
        if (c.cbm_zova_vector_session_get_workspace_token_i8(
            session,
            workspace.workspace_id,
            workspace.model_fingerprint,
            workspace.vector_dimensions,
            keyword,
            &token_vector,
            token_vector.len,
            &found,
        ) != 0) return ERR;
        if (found) {
            vsDequantize(&token_vector, &values);
        } else {
            vsFillSparseRandom(keyword, &values);
        }
        if (vsNormalizeAndQuantize(&values, &query_vectors[query_count])) query_count += 1;
    }
    if (query_count == 0) return OK;

    var collection: [vs_collection_max]u8 = undefined;
    if (c.cbm_zova_workspace_node_vector_collection_name(
        workspace.workspace_id,
        workspace.model_fingerprint,
        workspace.vector_dimensions,
        &collection,
        collection.len,
    ) != 0) return ERR;
    const prefilter = if (query_count > 1 and needed <= @divTrunc(std.math.maxInt(c_int), 5)) needed * 5 else needed;
    var hits: [*c]c.cbm_zova_vector_hit_t = null;
    var hit_count: c_int = 0;
    if (c.cbm_zova_vector_session_search_collection_i8(
        session,
        &collection,
        &query_vectors[0],
        @intCast(query_count),
        VS_VEC_DIM,
        prefilter,
        needed,
        &hits,
        &hit_count,
    ) != 0) return ERR;
    defer c.cbm_zova_vector_hits_free(hits, hit_count);

    for (span(hits, hit_count)) |*hit| {
        const key = vsHitKey(hit.vector_id) orelse return ERR;
        if (c.sqlite3_reset(hydrate) != c.SQLITE_OK or
            c.sqlite3_clear_bindings(hydrate) != c.SQLITE_OK or
            c.sqlite3_bind_int64(hydrate, 1, workspace.workspace_key) != c.SQLITE_OK or
            c.sqlite3_bind_int64(hydrate, 2, key) != c.SQLITE_OK)
        {
            return ERR;
        }
        if (c.sqlite3_step(hydrate) != c.SQLITE_ROW) return ERR;
        const label = columnText(hydrate, 0);
        if (!vsIsCodeLabel(label)) continue;
        if (combined.count == combined.capacity) {
            if (combined.capacity > @divTrunc(std.math.maxInt(c_int), 2)) return ERR;
            const next = if (combined.capacity != 0) combined.capacity * 2 else c.CBM_SZ_16;
            const grown = interop.reallocN(VectorResult, combined.items, @intCast(next));
            if (grown == null) return ERR;
            combined.items = grown;
            combined.capacity = next;
        }
        const result: VectorResult = .{
            .node_id = key,
            .project = c.strdup(workspace.selector),
            .label = c.strdup(label),
            .name = dupOrEmpty(columnText(hydrate, 1)),
            .qualified_name = dupOrEmpty(columnText(hydrate, 2)),
            .file_path = dupOrEmpty(columnText(hydrate, 3)),
            .score = hit.score,
        };
        if (result.project == null or result.label == null or result.name == null or
            result.qualified_name == null or result.file_path == null)
        {
            vectorResultFreeFields(&result);
            return ERR;
        }
        combined.items[@intCast(combined.count)] = result;
        combined.count += 1;
    }
    return OK;
}

pub export fn cbm_store_zova_multi_vector_search(
    db_path: CStr,
    workspaces: [*c]const c.cbm_zova_vector_workspace_t,
    workspace_count: c_int,
    keywords: [*c]CStr,
    keyword_count: c_int,
    limit: c_int,
    offset: c_int,
    out_c: ?*[*c]VectorResult,
    out_count_c: ?*c_int,
) c_int {
    const out = out_c orelse return ERR;
    const out_count = out_count_c orelse return ERR;
    out.* = null;
    out_count.* = 0;
    if (db_path == null or workspaces == null or workspace_count <= 0 or keywords == null or keyword_count <= 0 or
        limit <= 0 or offset < 0 or offset > std.math.maxInt(c_int) - limit)
    {
        return ERR;
    }
    const needed = offset + limit;
    const store = cbm_store_open_path_query(@ptrCast(db_path));
    const session = c.cbm_zova_vector_session_open(db_path);
    // Deferred in reverse: the statement, then the store, then the session.
    defer c.cbm_zova_vector_session_close(session);
    defer cbm_store_close(store);
    const s = store orelse return ERR;
    if (session == null) return ERR;
    const hydrate = prepareOne(s.db, "SELECT n.label,n.name,n.qualified_name,f.file_path FROM cbm_nodes_v1 n " ++
        "JOIN cbm_files_v1 f ON f.file_key=n.file_key WHERE n.workspace_key=?1 " ++
        "AND n.zova_node_key=?2") orelse return ERR;
    defer _ = c.sqlite3_finalize(hydrate);

    var combined: CombinedResults = .{};
    for (span(workspaces, workspace_count)) |*workspace| {
        const rc = multiVectorSearchWorkspace(session, hydrate, workspace, span(keywords, keyword_count), needed, &combined);
        if (rc != OK) {
            cbm_store_free_vector_results(combined.items, combined.count);
            return rc;
        }
    }
    if (combined.count > 1) {
        c.qsort(combined.items, @intCast(combined.count), @sizeOf(VectorResult), multiVectorResultCompare);
    }
    const start: usize = @intCast(@min(offset, combined.count));
    const end: usize = @intCast(@min(offset + limit, combined.count));
    const page_count = end - start;
    var page: [*c]VectorResult = null;
    if (page_count != 0) {
        page = interop.callocN(VectorResult, page_count);
        if (page == null) {
            cbm_store_free_vector_results(combined.items, combined.count);
            return ERR;
        }
        // The page takes the strings of its rows.
        for (page[0..page_count], combined.items[start..end]) |*row, *source| {
            row.* = source.*;
            source.* = std.mem.zeroes(VectorResult);
        }
    }
    cbm_store_free_vector_results(combined.items, combined.count);
    out.* = page;
    out_count.* = @intCast(page_count);
    return OK;
}
