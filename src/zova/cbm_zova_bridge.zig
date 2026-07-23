const std = @import("std");
const zova = @import("zova");
const sqlite = zova.sqlite;
const c = sqlite.c;

const compat = @cImport({
    @cInclude("foundation/compat_regex.h");
});

const HookError = sqlite.Error || error{
    ExtensionExists,
    ExtensionIncompatible,
    ExtensionInvalid,
    ExtensionNotFound,
    ExtensionUnavailable,
    OutOfMemory,
};
const camel_split_buf = 2048;
const camel_buf_guard = 2;
const denom_eps = 1e-10;

const manifest = zova.ExtensionManifest{
    .name = "codebase_memory",
    .version = "0.1.0",
    .storage_prefix = "_zova_ext_codebase_memory_",
    .zova_abi_min = "0.21.0",
    .capabilities = "sql,codebase-memory",
    .manifest_json = "{\"extension\":\"codebase_memory\",\"version\":\"0.1.0\"}",
};

const extension = zova.Extension{
    .manifest = manifest,
    .install = install,
    .check = check,
    .drop = drop,
    .register_sql = registerSql,
};

const registry = zova.ExtensionRegistry.init(&.{extension});

export fn cbm_zova_bridge_build_enabled() c_int {
    return 1;
}

export fn cbm_zova_bridge_install_extension(zova_path: [*:0]const u8) c_int {
    var db = zova.Database.openWithExtensions(std.mem.span(zova_path), registry) catch return -1;
    defer db.deinit();

    db.installExtension("codebase_memory") catch |err| switch (err) {
        error.ExtensionExists => {},
        else => return -1,
    };
    return 0;
}

export fn cbm_zova_bridge_cosine_smoke(
    zova_path: [*:0]const u8,
    a: [*]const i8,
    b: [*]const i8,
    len: usize,
    out_score: ?*f64,
) c_int {
    const out = out_score orelse return -1;
    var db = openBridgeDb(zova_path) catch return -1;
    defer db.deinit();

    var stmt = db.sqlite_db.prepare("select cbm_cosine_i8(?1, ?2)") catch return -1;
    defer stmt.deinit();
    stmt.bindBlob(1, std.mem.sliceAsBytes(a[0..len])) catch return -1;
    stmt.bindBlob(2, std.mem.sliceAsBytes(b[0..len])) catch return -1;
    if ((stmt.step() catch return -1) != .row) return -1;
    out.* = stmt.columnDouble(0);
    return 0;
}

export fn cbm_zova_bridge_camel_smoke(
    zova_path: [*:0]const u8,
    input: [*:0]const u8,
    out: [*]u8,
    out_len: usize,
) c_int {
    if (out_len == 0) return -1;
    var db = openBridgeDb(zova_path) catch return -1;
    defer db.deinit();

    var stmt = db.sqlite_db.prepare("select cbm_camel_split(?1)") catch return -1;
    defer stmt.deinit();
    stmt.bindText(1, std.mem.span(input)) catch return -1;
    if ((stmt.step() catch return -1) != .row) return -1;
    return copyOut(stmt.columnText(0), out, out_len);
}

export fn cbm_zova_bridge_regex_smoke(
    zova_path: [*:0]const u8,
    pattern: [*:0]const u8,
    text: [*:0]const u8,
    case_insensitive: c_int,
    out_match: ?*c_int,
) c_int {
    const out = out_match orelse return -1;
    var db = openBridgeDb(zova_path) catch return -1;
    defer db.deinit();

    const sql = if (case_insensitive != 0) "select iregexp(?1, ?2)" else "select regexp(?1, ?2)";
    var stmt = db.sqlite_db.prepare(sql) catch return -1;
    defer stmt.deinit();
    stmt.bindText(1, std.mem.span(pattern)) catch return -1;
    stmt.bindText(2, std.mem.span(text)) catch return -1;
    if ((stmt.step() catch return -1) != .row) return -1;
    out.* = @intCast(stmt.columnInt64(0));
    return 0;
}

export fn cbm_zova_bridge_invalid_regex_smoke(
    zova_path: [*:0]const u8,
    out_error: [*]u8,
    out_error_len: usize,
) c_int {
    if (out_error_len == 0) return -1;
    var db = openBridgeDb(zova_path) catch return -1;
    defer db.deinit();

    var stmt = db.sqlite_db.prepare("select regexp(?1, ?2)") catch return -1;
    defer stmt.deinit();
    stmt.bindText(1, "[") catch return -1;
    stmt.bindText(2, "text") catch return -1;
    _ = stmt.step() catch {
        _ = copyOut(db.sqlite_db.errorMessage(), out_error, out_error_len);
        return 0;
    };
    return -1;
}

fn openBridgeDb(path: [*:0]const u8) !zova.Database {
    var db = try zova.Database.openWithExtensions(std.mem.span(path), registry);
    errdefer db.deinit();
    try db.checkExtension("codebase_memory");
    return db;
}

fn install(db: *sqlite.Database, _: zova.ExtensionManifest) HookError!void {
    try db.exec(
        \\create table _zova_ext_codebase_memory_meta (
        \\  key text primary key,
        \\  value text not null
        \\)
    );
    try db.exec("insert into _zova_ext_codebase_memory_meta (key, value) values ('installed', '0.1.0')");
}

fn check(db: *sqlite.Database, _: zova.ExtensionManifest) HookError!void {
    var stmt = try db.prepare("select value from _zova_ext_codebase_memory_meta where key = 'installed'");
    defer stmt.deinit();
    if ((try stmt.step()) != .row) return error.ExtensionInvalid;
    if (!std.mem.eql(u8, stmt.columnText(0), "0.1.0")) return error.ExtensionInvalid;
}

fn drop(db: *sqlite.Database, _: zova.ExtensionManifest) HookError!void {
    try db.exec("drop table _zova_ext_codebase_memory_meta");
}

fn registerSql(db: *sqlite.Database, _: zova.ExtensionManifest) HookError!void {
    try createFunction(db, "cbm_cosine_i8", 2, cosineFunc);
    try createFunction(db, "cbm_camel_split", 1, camelSplitFunc);
    try createFunction(db, "regexp", 2, regexpFunc);
    try createFunction(db, "iregexp", 2, iregexpFunc);
}

fn createFunction(
    db: *sqlite.Database,
    name: [*:0]const u8,
    argc: c_int,
    func: ?*const fn (?*c.sqlite3_context, c_int, [*c]?*c.sqlite3_value) callconv(.c) void,
) sqlite.Error!void {
    const rc = c.sqlite3_create_function_v2(
        db.handle,
        name,
        argc,
        c.SQLITE_UTF8 | c.SQLITE_DETERMINISTIC,
        null,
        func,
        null,
        null,
        null,
    );
    if (rc != c.SQLITE_OK) return error.SqliteError;
}

fn cosineFunc(ctx: ?*c.sqlite3_context, argc: c_int, argv: [*c]?*c.sqlite3_value) callconv(.c) void {
    _ = argc;
    const context = ctx orelse return;
    const a_value = argv[0] orelse {
        c.sqlite3_result_double(context, 0.0);
        return;
    };
    const b_value = argv[1] orelse {
        c.sqlite3_result_double(context, 0.0);
        return;
    };
    if (c.sqlite3_value_type(a_value) != c.SQLITE_BLOB or c.sqlite3_value_type(b_value) != c.SQLITE_BLOB) {
        c.sqlite3_result_double(context, 0.0);
        return;
    }
    const len_a = c.sqlite3_value_bytes(a_value);
    const len_b = c.sqlite3_value_bytes(b_value);
    if (len_a != len_b or len_a == 0) {
        c.sqlite3_result_double(context, 0.0);
        return;
    }
    const a_blob = c.sqlite3_value_blob(a_value) orelse {
        c.sqlite3_result_double(context, 0.0);
        return;
    };
    const b_blob = c.sqlite3_value_blob(b_value) orelse {
        c.sqlite3_result_double(context, 0.0);
        return;
    };
    const a: [*]const i8 = @ptrCast(@alignCast(a_blob));
    const b: [*]const i8 = @ptrCast(@alignCast(b_blob));
    var dot: i32 = 0;
    var mag_a: i32 = 0;
    var mag_b: i32 = 0;
    for (0..@intCast(len_a)) |i| {
        const av: i32 = a[i];
        const bv: i32 = b[i];
        dot += av * bv;
        mag_a += av * av;
        mag_b += bv * bv;
    }
    const denom = std.math.sqrt(@as(f64, @floatFromInt(mag_a))) * std.math.sqrt(@as(f64, @floatFromInt(mag_b)));
    c.sqlite3_result_double(context, if (denom > denom_eps) @as(f64, @floatFromInt(dot)) / denom else 0.0);
}

fn camelSplitFunc(ctx: ?*c.sqlite3_context, argc: c_int, argv: [*c]?*c.sqlite3_value) callconv(.c) void {
    _ = argc;
    const context = ctx orelse return;
    const value = argv[0] orelse {
        resultText(context, "");
        return;
    };
    const raw = c.sqlite3_value_text(value) orelse {
        resultText(context, "");
        return;
    };
    const input = std.mem.span(raw);
    if (input.len == 0) {
        resultText(context, "");
        return;
    }
    var buf: [camel_split_buf]u8 = undefined;
    if (input.len + 1 >= buf.len) {
        resultText(context, input);
        return;
    }
    @memcpy(buf[0..input.len], input);
    var len = input.len;
    buf[len] = ' ';
    len += 1;
    var i: usize = 0;
    while (i < input.len and len < buf.len - camel_buf_guard) : (i += 1) {
        if (camelShouldSplit(input, i)) {
            buf[len] = ' ';
            len += 1;
        }
        buf[len] = input[i];
        len += 1;
    }
    resultText(context, buf[0..len]);
}

fn regexpFunc(ctx: ?*c.sqlite3_context, argc: c_int, argv: [*c]?*c.sqlite3_value) callconv(.c) void {
    regexFunc(ctx, argc, argv, false);
}

fn iregexpFunc(ctx: ?*c.sqlite3_context, argc: c_int, argv: [*c]?*c.sqlite3_value) callconv(.c) void {
    regexFunc(ctx, argc, argv, true);
}

fn regexFunc(ctx: ?*c.sqlite3_context, argc: c_int, argv: [*c]?*c.sqlite3_value, case_insensitive: bool) void {
    _ = argc;
    const context = ctx orelse return;
    const pattern_value = argv[0] orelse {
        c.sqlite3_result_int(context, 0);
        return;
    };
    const text_value = argv[1] orelse {
        c.sqlite3_result_int(context, 0);
        return;
    };
    const pattern_raw = c.sqlite3_value_text(pattern_value) orelse {
        c.sqlite3_result_int(context, 0);
        return;
    };
    const text_raw = c.sqlite3_value_text(text_value) orelse {
        c.sqlite3_result_int(context, 0);
        return;
    };
    var re: compat.cbm_regex_t = undefined;
    const flags: c_int = compat.CBM_REG_EXTENDED | compat.CBM_REG_NOSUB |
        if (case_insensitive) compat.CBM_REG_ICASE else 0;
    if (compat.cbm_regcomp(&re, @ptrCast(pattern_raw), flags) != compat.CBM_REG_OK) {
        c.sqlite3_result_error(context, "invalid regex", -1);
        return;
    }
    defer compat.cbm_regfree(&re);
    c.sqlite3_result_int(
        context,
        if (compat.cbm_regexec(&re, @ptrCast(text_raw), 0, null, 0) == compat.CBM_REG_OK) 1 else 0,
    );
}

fn camelShouldSplit(input: []const u8, i: usize) bool {
    if (i == 0) return false;
    const curr = input[i];
    const prev = input[i - 1];
    const next = if (i + 1 < input.len) input[i + 1] else 0;
    if (curr >= 'A' and curr <= 'Z' and prev >= 'a' and prev <= 'z') return true;
    if (curr >= 'A' and curr <= 'Z' and prev >= 'A' and prev <= 'Z' and next >= 'a' and next <= 'z') return true;
    return false;
}

fn resultText(ctx: *c.sqlite3_context, value: []const u8) void {
    const raw = c.sqlite3_malloc64(@intCast(value.len + 1)) orelse {
        c.sqlite3_result_error_nomem(ctx);
        return;
    };
    const bytes: [*]u8 = @ptrCast(raw);
    if (value.len > 0) {
        @memcpy(bytes[0..value.len], value);
    }
    bytes[value.len] = 0;
    c.sqlite3_result_text(ctx, @ptrCast(bytes), @intCast(value.len), c.sqlite3_free);
}

fn copyOut(value: []const u8, out: [*]u8, out_len: usize) c_int {
    if (out_len == 0) return -1;
    const n = @min(value.len, out_len - 1);
    if (n > 0) {
        @memcpy(out[0..n], value[0..n]);
    }
    out[n] = 0;
    return if (value.len < out_len) 0 else -1;
}
