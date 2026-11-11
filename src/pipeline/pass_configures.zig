//! pass_configures.zig — Config key and env var helpers.
//!
//! Pure helper functions for detecting environment variable names,
//! normalizing config keys, and identifying config file extensions.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;

pub export fn cbm_is_env_var_name(s: CStr) bool {
    if (s == null) return false;
    const len = c.strlen(s);
    if (len < 2) return false;

    var has_upper = false;
    for (s[0..len]) |ch| {
        if (ch >= 'A' and ch <= 'Z') {
            has_upper = true;
        } else if (ch == '_' or (ch >= '0' and ch <= '9')) {
            // ok
        } else {
            return false;
        }
    }
    return has_upper;
}

/// Emit a camelCase-split word (lowercased) into the output buffer.
/// Prepends '_' if out_pos > 0. Updates out_pos and token_count.
fn emitCamelWords(part: []const u8, norm_out: [*]u8, norm_sz: usize, out_pos: *usize, token_count: *c_int) void {
    var start: usize = 0;
    var i: usize = 1;
    while (i < part.len) : (i += 1) {
        if (part[i] >= 'A' and part[i] <= 'Z' and part[i - 1] >= 'a' and part[i - 1] <= 'z') {
            if (out_pos.* > 0 and out_pos.* < norm_sz - 1) {
                norm_out[out_pos.*] = '_';
                out_pos.* += 1;
            }
            var j = start;
            while (j < i and out_pos.* < norm_sz - 1) : (j += 1) {
                norm_out[out_pos.*] = std.ascii.toLower(part[j]);
                out_pos.* += 1;
            }
            token_count.* += 1;
            start = i;
        }
    }
    // Emit remaining
    if (out_pos.* > 0 and out_pos.* < norm_sz - 1) {
        norm_out[out_pos.*] = '_';
        out_pos.* += 1;
    }
    var j = start;
    while (j < part.len and out_pos.* < norm_sz - 1) : (j += 1) {
        norm_out[out_pos.*] = std.ascii.toLower(part[j]);
        out_pos.* += 1;
    }
    token_count.* += 1;
}

pub export fn cbm_normalize_config_key(key: CStr, norm_out: MutStr, norm_sz: usize) c_int {
    if (key == null or norm_out == null or norm_sz == 0) return 0;
    norm_out[0] = 0;

    var buf: [c.CBM_SZ_512]u8 = undefined;
    var klen = c.strlen(key);
    if (klen >= buf.len) klen = buf.len - 1;
    @memcpy(buf[0..klen], key[0..klen]);
    buf[klen] = 0;

    // Replace delimiters with spaces
    for (buf[0..klen]) |*ch| {
        if (ch.* == '_' or ch.* == '-' or ch.* == '.') ch.* = ' ';
    }

    var token_count: c_int = 0;
    var out_pos: usize = 0;

    var saveptr: MutStr = null;
    var part: MutStr = c.strtok_r(&buf, " ", &saveptr);
    while (part != null) {
        emitCamelWords(part[0..c.strlen(part)], norm_out, norm_sz, &out_pos, &token_count);
        part = c.strtok_r(null, " ", &saveptr);
    }

    norm_out[out_pos] = 0;
    return token_count;
}

pub export fn cbm_has_config_extension(path: CStr) bool {
    if (path == null) return false;

    // Find last dot
    const dot: CStr = c.strrchr(path, '.');
    var basename: CStr = c.strrchr(path, '/');
    if (basename == null) {
        basename = path;
    } else {
        basename += 1;
    }

    // Special case: .env files
    if (util.streq(basename, ".env")) return true;

    if (dot == null) return false;

    const exts = [_][*:0]const u8{
        ".toml", ".ini", ".yaml", ".yml", ".cfg", ".properties", ".json", ".xml", ".conf", ".env",
    };
    for (exts) |ext| {
        if (util.streq(dot, ext)) return true;
    }
    return false;
}
