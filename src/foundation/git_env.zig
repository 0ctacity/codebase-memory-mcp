//! git_env.zig — child environment for cbm's git spawns.
//!
//! Implements src/foundation/git_env.h.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");

const is_windows = builtin.os.tag == .windows;

/// `git rev-parse --local-env-vars` (git 2.50).
pub export const cbm_git_repo_env_vars = [c.CBM_GIT_REPO_ENV_VAR_COUNT][*:0]const u8{
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_CONFIG",
    "GIT_CONFIG_PARAMETERS",
    "GIT_CONFIG_COUNT",
    "GIT_OBJECT_DIRECTORY",
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_IMPLICIT_WORK_TREE",
    "GIT_GRAFT_FILE",
    "GIT_INDEX_FILE",
    "GIT_NO_REPLACE_OBJECTS",
    "GIT_REPLACE_REF_BASE",
    "GIT_PREFIX",
    "GIT_SHALLOW_FILE",
    "GIT_COMMON_DIR",
};

fn gitEnvFold(ch: u8) u8 {
    if (is_windows) return if (ch >= 'a' and ch <= 'z') ch - 'a' + 'A' else ch;
    return ch;
}

/// Does `name` equal `variable`?
fn gitEnvNameEquals(name: []const u8, variable: []const u8) bool {
    if (name.len != variable.len) return false;
    for (name, variable) |n, v| {
        if (gitEnvFold(n) != v) return false;
    }
    return true;
}

fn gitEnvNameIsRepoLocal(name: []const u8) bool {
    for (cbm_git_repo_env_vars) |variable| {
        if (gitEnvNameEquals(name, std.mem.span(variable))) return true;
    }
    return false;
}

pub export fn cbm_git_env_entry_is_repo_local(entry: ?[*:0]const u8) bool {
    const e = std.mem.span(entry orelse return false);
    const name_len = std.mem.indexOfScalar(u8, e, '=') orelse e.len;
    return gitEnvNameIsRepoLocal(e[0..name_len]);
}

pub const windows = if (is_windows) struct {
    const GIT_ENV_ASCII_MAX = 0x7F;

    /// Is a wide "NAME=value" entry repository-local? The names we match are
    /// ASCII, so any non-ASCII code unit is a mismatch. Windows keeps per-drive
    /// working directories as "=C:=C:\\dir"; the separator search starts at index 1
    /// so those entries are kept.
    fn gitEnvWideEntryIsRepoLocal(entry: []const c.wchar_t) bool {
        var name: [64]u8 = undefined;
        var n: usize = 0;
        for (entry, 0..) |unit, i| {
            if (i != 0 and unit == '=') break;
            if (unit > GIT_ENV_ASCII_MAX or n + 1 >= name.len) return false;
            name[n] = @truncate(unit);
            n += 1;
        }
        return gitEnvNameIsRepoLocal(name[0..n]);
    }

    pub export fn cbm_git_child_env_block() ?[*:0]c.wchar_t {
        const current: [*:0]const c.wchar_t = @ptrCast(c.GetEnvironmentStringsW() orelse return null);

        var kept: usize = 0;
        var e = current;
        while (e[0] != 0) : (e += std.mem.len(e) + 1) {
            if (!gitEnvWideEntryIsRepoLocal(std.mem.span(e))) kept += std.mem.len(e) + 1;
        }

        // 2: a block that keeps nothing still needs its double terminator.
        const block: [*]c.wchar_t = @ptrCast(@alignCast(c.malloc((kept + 2) * @sizeOf(c.wchar_t)) orelse {
            _ = c.FreeEnvironmentStringsW(@constCast(current));
            return null;
        }));

        var pos: usize = 0;
        e = current;
        while (e[0] != 0) : (e += std.mem.len(e) + 1) {
            if (!gitEnvWideEntryIsRepoLocal(std.mem.span(e))) {
                const len = std.mem.len(e) + 1;
                @memcpy(block[pos .. pos + len], e[0..len]);
                pos += len;
            }
        }
        block[pos] = 0;
        if (pos == 0) block[1] = 0;

        _ = c.FreeEnvironmentStringsW(@constCast(current));
        return @ptrCast(block);
    }
} else struct {};

pub const posix = if (!is_windows) struct {
    extern var environ: ?[*:null]?[*:0]u8;

    pub export fn cbm_git_child_envp() ?[*:null]?[*:0]u8 {
        var count: usize = 0;
        var bytes: usize = 0;
        if (environ) |env| {
            var i: usize = 0;
            while (env[i]) |entry| : (i += 1) {
                if (!cbm_git_env_entry_is_repo_local(entry)) {
                    count += 1;
                    bytes += std.mem.len(entry) + 1;
                }
            }
        }

        const table = (count + 1) * @sizeOf(?[*:0]u8);
        const raw: [*]u8 = @ptrCast(c.malloc(table + bytes) orelse return null);
        const envp: [*]?[*:0]u8 = @ptrCast(@alignCast(raw));
        var strings = raw + table;
        var slot: usize = 0;
        if (environ) |env| {
            var i: usize = 0;
            while (env[i]) |entry| : (i += 1) {
                if (slot >= count) break;
                if (!cbm_git_env_entry_is_repo_local(entry)) {
                    const len = std.mem.len(entry) + 1;
                    @memcpy(strings[0..len], entry[0..len]);
                    envp[slot] = @ptrCast(strings);
                    slot += 1;
                    strings += len;
                }
            }
        }
        envp[slot] = null;
        return @ptrCast(envp);
    }
} else struct {};

pub export fn cbm_git_child_env_free(env: ?*anyopaque) void {
    c.free(env);
}
