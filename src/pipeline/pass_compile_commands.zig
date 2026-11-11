//! pass_compile_commands.zig — compile_commands.json parsing helpers.
//!
//! Parses compile_commands.json to extract per-file include paths, defines,
//! and C/C++ standard flags.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const CompileFlags = c.cbm_compile_flags_t;

const CC_FLAG_IDX = 1;
const CC_FLAG_SKIP = 2;

/// Emit the current token if non-empty. Returns updated count.
fn emitToken(current: *[c.CBM_SZ_4K]u8, clen: *usize, out: [*]MutStr, count_in: c_int, max_out: c_int) c_int {
    var count = count_in;
    if (clen.* > 0 and count < max_out) {
        current[clen.*] = 0;
        out[ix(count)] = c.strdup(current);
        count += 1;
        clen.* = 0;
    }
    return count;
}

pub export fn cbm_split_command(cmd: CStr, out_c: [*c]MutStr, max_out: c_int) c_int {
    if (cmd == null or out_c == null or max_out <= 0) return 0;
    const out: [*]MutStr = out_c;

    var count: c_int = 0;
    var current: [c.CBM_SZ_4K]u8 = undefined;
    var clen: usize = 0;
    var in_quote: u8 = 0;

    var i: usize = 0;
    while (cmd[i] != 0) : (i += 1) {
        const ch = cmd[i];
        if (in_quote != 0) {
            if (ch == in_quote) {
                in_quote = 0;
            } else if (clen < current.len - 1) {
                current[clen] = ch;
                clen += 1;
            }
        } else if (ch == '"' or ch == '\'') {
            in_quote = ch;
        } else if (ch == ' ' or ch == '\t') {
            count = emitToken(&current, &clen, out, count, max_out);
        } else if (clen < current.len - 1) {
            current[clen] = ch;
            clen += 1;
        }
    }
    return emitToken(&current, &clen, out, count, max_out);
}

/// Resolve a path: if relative, join with directory.
fn resolvePath(path: CStr, directory: CStr) MutStr {
    if (path == null) return null;

    // Absolute path
    if (path[0] == '/') return c.strdup(path);

    // Relative — join with directory
    if (directory != null and directory[0] != 0) {
        var buf: [c.CBM_SZ_4K]u8 = undefined;
        _ = c.snprintf(&buf, buf.len, "%s/%s", directory, path);
        return c.strdup(&buf);
    }

    return c.strdup(path);
}

/// Try to consume a -I or -isystem include path flag. Returns true if consumed.
fn tryIncludeFlag(f: *CompileFlags, args: [*]const CStr, argc: c_int, i: *c_int, directory: CStr) bool {
    const arg = args[ix(i.*)];
    if (arg[0] == '-' and arg[CC_FLAG_IDX] == 'I') {
        var path: CStr = arg + CC_FLAG_SKIP;
        if (path[0] == 0 and i.* + 1 < argc) {
            i.* += 1;
            path = args[ix(i.*)];
        }
        if (path != null and path[0] != 0) {
            f.include_paths[ix(f.include_count)] = resolvePath(path, directory);
            f.include_count += 1;
        }
        return true;
    }
    if (util.streq(arg, "-isystem") and i.* + 1 < argc) {
        i.* += 1;
        f.include_paths[ix(f.include_count)] = resolvePath(args[ix(i.*)], directory);
        f.include_count += 1;
        return true;
    }
    return false;
}

/// Try to consume a -D define flag. Returns true if consumed.
fn tryDefineFlag(f: *CompileFlags, args: [*]const CStr, argc: c_int, i: *c_int) bool {
    const arg = args[ix(i.*)];
    if (arg[0] != '-' or arg[CC_FLAG_IDX] != 'D') return false;
    var define: CStr = arg + CC_FLAG_SKIP;
    if (define[0] == 0 and i.* + 1 < argc) {
        i.* += 1;
        define = args[ix(i.*)];
    }
    if (define != null and define[0] != 0) {
        f.defines[ix(f.define_count)] = c.strdup(define);
        f.define_count += 1;
    }
    return true;
}

pub export fn cbm_extract_flags(args: [*c]CStr, argc: c_int, directory: CStr) ?*CompileFlags {
    const f: *CompileFlags = @as(?*CompileFlags, interop.callocN(CompileFlags, 1)) orelse return null;
    // (size_t)argc, as the C calloc(argc, ...) converted it.
    const slots: usize = @bitCast(@as(isize, argc));
    f.include_paths = interop.callocN(MutStr, slots);
    f.defines = interop.callocN(MutStr, slots);

    var i: c_int = 0;
    while (i < argc) : (i += 1) {
        if (tryIncludeFlag(f, args, argc, &i, directory)) continue;
        if (tryDefineFlag(f, args, argc, &i)) continue;
        if (c.strncmp(args[ix(i)], "-std=", "-std=".len) == 0) {
            _ = c.snprintf(&f.standard, f.standard.len, "%s", args[ix(i)] + 5);
        }
    }
    return f;
}

pub export fn cbm_compile_flags_free(f_c: ?*CompileFlags) void {
    const f = f_c orelse return;
    var i: usize = 0;
    while (i < f.include_count) : (i += 1) c.free(f.include_paths[i]);
    c.free(@ptrCast(f.include_paths));
    i = 0;
    while (i < f.define_count) : (i += 1) c.free(f.defines[i]);
    c.free(@ptrCast(f.defines));
    c.free(f);
}

/// Extract compiler flag args from either "arguments" array or "command" string.
fn extractFlagArgs(
    args_val: ?*c.yyjson_val,
    cmd_val: ?*c.yyjson_val,
    flag_args: *[c.CBM_SZ_256]CStr,
    split_args: *[c.CBM_SZ_256]MutStr,
) c_int {
    @setEvalBranchQuota(10000);
    const max_args: c_int = c.CBM_SZ_256;
    if (args_val != null and c.yyjson_is_arr(args_val)) {
        var n: c_int = 0;
        var aiter: c.yyjson_arr_iter = undefined;
        _ = c.yyjson_arr_iter_init(args_val, &aiter);
        while (true) {
            const a = c.yyjson_arr_iter_next(&aiter);
            if (a == null or n >= max_args) break;
            const s: CStr = c.yyjson_get_str(a);
            if (s != null) {
                flag_args[ix(n)] = s;
                n += 1;
            }
        }
        return n;
    }
    if (cmd_val != null and c.yyjson_is_str(cmd_val)) {
        const n = cbm_split_command(c.yyjson_get_str(cmd_val), split_args, max_args);
        for (0..ix(n)) |j| flag_args[j] = split_args[j];
        return n;
    }
    return 0;
}

/// Process a single compile_commands.json entry. Returns true if added.
fn processCompileEntry(entry: ?*c.yyjson_val, repo_path: CStr, out_path: *MutStr, out_flag: *?*CompileFlags) bool {
    @setEvalBranchQuota(10000);
    const dir_val = c.yyjson_obj_get(entry, "directory");
    const file_val = c.yyjson_obj_get(entry, "file");
    const cmd_val = c.yyjson_obj_get(entry, "command");
    const args_val = c.yyjson_obj_get(entry, "arguments");

    if (file_val == null) return false;
    const directory: CStr = if (dir_val != null) c.yyjson_get_str(dir_val) else "";
    const file_path: CStr = c.yyjson_get_str(file_val);
    if (file_path == null) return false;

    var split_args: [c.CBM_SZ_256]MutStr = @splat(null);
    var flag_args: [c.CBM_SZ_256]CStr = undefined;
    const flag_argc = extractFlagArgs(args_val, cmd_val, &flag_args, &split_args);

    if (flag_argc == 0) return false;

    const f = cbm_extract_flags(&flag_args, flag_argc, directory);

    if (cmd_val != null and c.yyjson_is_str(cmd_val)) {
        for (0..ix(flag_argc)) |j| c.free(split_args[j]);
    }

    if (f == null) return false;

    var abs_path: [c.CBM_SZ_4K]u8 = undefined;
    if (file_path[0] != '/' and directory != null and directory[0] != 0) {
        _ = c.snprintf(&abs_path, abs_path.len, "%s/%s", directory, file_path);
    } else {
        _ = c.snprintf(&abs_path, abs_path.len, "%s", file_path);
    }

    const repo_len = c.strlen(repo_path);
    if (c.strncmp(&abs_path, repo_path, repo_len) != 0 or abs_path[repo_len] != '/') {
        cbm_compile_flags_free(f);
        return false;
    }

    out_path.* = c.strdup(@as(CStr, &abs_path) + repo_len + 1);
    out_flag.* = f;
    return true;
}

pub export fn cbm_parse_compile_commands(
    json_data: CStr,
    repo_path: CStr,
    out_paths: ?*[*c]MutStr,
    out_flags: ?*[*c]?*CompileFlags,
) c_int {
    @setEvalBranchQuota(10000);
    if (json_data == null or repo_path == null) return c.CBM_NOT_FOUND;
    const paths_out = out_paths orelse return c.CBM_NOT_FOUND;
    const flags_out = out_flags orelse return c.CBM_NOT_FOUND;
    paths_out.* = null;
    flags_out.* = null;

    const doc = c.yyjson_read(json_data, c.strlen(json_data), 0);
    if (doc == null) return c.CBM_NOT_FOUND;

    const root = c.yyjson_doc_get_root(doc);
    if (!c.yyjson_is_arr(root)) {
        c.yyjson_doc_free(doc);
        return c.CBM_NOT_FOUND;
    }

    const arr_len: c_int = @truncate(@as(isize, @bitCast(c.yyjson_arr_size(root))));
    if (arr_len == 0) {
        c.yyjson_doc_free(doc);
        return 0;
    }

    const slots: usize = @bitCast(@as(isize, arr_len));
    const paths = interop.callocN(MutStr, slots);
    const flags = interop.callocN(?*CompileFlags, slots);
    var count: usize = 0;

    var iter: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(root, &iter);

    while (true) {
        const entry = c.yyjson_arr_iter_next(&iter);
        if (entry == null) break;
        var p: MutStr = null;
        var f: ?*CompileFlags = null;
        if (processCompileEntry(entry, repo_path, &p, &f)) {
            paths[count] = p;
            flags[count] = f;
            count += 1;
        }
    }

    c.yyjson_doc_free(doc);
    paths_out.* = paths;
    flags_out.* = flags;
    return @intCast(count);
}
