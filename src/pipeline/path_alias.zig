//! path_alias.zig — Resolve build-tool path aliases.
//!
//! Implements src/pipeline/path_alias.h. Builds a directory-scoped collection
//! of alias maps from per-language config files (currently tsconfig.json /
//! jsconfig.json) so the import resolver can turn "@/lib/auth"-style imports
//! into repo-relative paths.
//!
//! Design notes:
//!   - Public types and functions are language-agnostic. Adding a Vite /
//!     Webpack / Python loader means writing a new load*File() helper
//!     and registering it in findAliasFiles. The resolver, the
//!     collection, and the pipeline integration do not change.
//!   - Sorting uses libc qsort, as the C code did, so entries of equal
//!     specificity keep the order they had before the port.
//!   - The repo walk caps recursion depth and total file count and emits
//!     a warning when either cap fires, so silent truncation on
//!     pathological monorepos shows up in the index log.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = [*c]const u8;
const MutStr = [*c]u8;
const Alias = c.cbm_path_alias_t;
const AliasMap = c.cbm_path_alias_map_t;
const Scope = c.cbm_path_alias_scope_t;
const Collection = c.cbm_path_alias_collection_t;

/// Resource ceilings. Chosen to comfortably cover real-world monorepos
/// (Next.js Skyline, large nx workspaces) while bounding worst-case
/// memory and walk time. Cap hits are logged.
const MAX_ENTRIES = 256; // per single config file
const MAX_FILES = 256; // config files per repo walk
const MAX_FILE_BYTES = 64 * 1024;
const MAX_DEPTH = 32; // directory recursion depth

// ── Helpers ─────────────────────────────────────────────────────────

/// Strip .ts/.tsx/.js/.jsx in place. Returns its argument.
fn stripResolvedExt(path: MutStr) MutStr {
    if (path == null) return path;
    const len = c.strlen(path);
    if (len > 3 and path[len - 3] == '.' and (path[len - 2] == 't' or path[len - 2] == 'j') and
        path[len - 1] == 's')
    {
        path[len - 3] = 0;
        return path;
    }
    if (len > 4 and path[len - 4] == '.' and (path[len - 3] == 't' or path[len - 3] == 'j') and
        path[len - 2] == 's' and path[len - 1] == 'x')
    {
        path[len - 4] = 0;
    }
    return path;
}

/// Join dir_prefix with target, collapsing "." and ".." segments so aliases
/// that climb out of their tsconfig's directory (the common monorepo
/// pattern: a tsconfig at apps/web/tsconfig.json pointing an alias at a
/// wildcard target like "../../packages/shared/src/" + wildcard) resolve
/// to a real repo-relative path. Naive concatenation left literal ".."
/// components in the target, which never match a module's FQN since
/// cbm_pipeline_fqn_module tokenizes on '/' without collapsing them
/// (#730). A trailing '/' on target (the usual case right before a
/// wildcard) is preserved so the caller's later wildcard-substring
/// concat still lines up. Returns heap-allocated
/// repo-relative target.
fn resolveTargetRelative(dir_prefix: CStr, target: CStr) MutStr {
    if (target == null) return null;
    const dp_len: usize = if (dir_prefix != null and dir_prefix[0] != 0) c.strlen(dir_prefix) else 0;
    const t_len = c.strlen(target);
    const buf: MutStr = @ptrCast(c.malloc(dp_len + t_len + 2));
    if (buf == null) return null;
    buf[0] = 0;
    if (dp_len > 0) {
        @memcpy(buf[0..dp_len], dir_prefix[0..dp_len]);
        buf[dp_len] = 0;
    }

    const trailing_slash = t_len > 0 and target[t_len - 1] == '/';

    var p: [*]const u8 = target;
    while (p[0] != 0) {
        while (p[0] == '/') p += 1;
        if (p[0] == 0) break;
        const seg_start = p;
        while (p[0] != 0 and p[0] != '/') p += 1;
        const seg_len = @intFromPtr(p) - @intFromPtr(seg_start);
        if (seg_len == 1 and seg_start[0] == '.') continue;
        if (seg_len == 2 and seg_start[0] == '.' and seg_start[1] == '.') {
            const last: MutStr = c.strrchr(buf, '/');
            if (last != null) {
                last[0] = 0;
            } else {
                buf[0] = 0;
            }
            continue;
        }
        var cur = c.strlen(buf);
        if (cur > 0) {
            buf[cur] = '/';
            cur += 1;
        }
        @memcpy(buf[cur..][0..seg_len], seg_start[0..seg_len]);
        buf[cur + seg_len] = 0;
    }

    if (trailing_slash) {
        const cur = c.strlen(buf);
        buf[cur] = '/';
        buf[cur + 1] = 0;
    }
    return buf;
}

/// qsort comparator: alias entries by alias_prefix length, descending.
fn cmpAliasEntryBySpecificity(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const ea: *const Alias = @ptrCast(@alignCast(a));
    const eb: *const Alias = @ptrCast(@alignCast(b));
    const la = c.strlen(ea.alias_prefix);
    const lb = c.strlen(eb.alias_prefix);
    if (lb > la) return 1;
    if (lb < la) return -1;
    return 0;
}

/// qsort comparator: scopes by dir_prefix length, descending.
fn cmpScopeBySpecificity(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const sa: *const Scope = @ptrCast(@alignCast(a));
    const sb: *const Scope = @ptrCast(@alignCast(b));
    const la = c.strlen(sa.dir_prefix);
    const lb = c.strlen(sb.dir_prefix);
    if (lb > la) return 1;
    if (lb < la) return -1;
    return 0;
}

// ── tsconfig.json / jsconfig.json loader ────────────────────────────

/// Parse compilerOptions.paths and compilerOptions.baseUrl into an alias map.
/// dir_prefix is the directory of the config file relative to the repo root
/// (e.g. "apps/manager", or "" for repo root). Returns NULL if the file is
/// missing, malformed, or has neither a usable paths block nor a baseUrl.
fn loadTsconfigFile(abs_path: CStr, dir_prefix: CStr) ?*AliasMap {
    @setEvalBranchQuota(10000);
    const f = c.cbm_fopen(abs_path, "r") orelse return null;
    _ = c.fseek(f, 0, c.SEEK_END);
    const len = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    if (len <= 0 or len > MAX_FILE_BYTES) {
        _ = c.fclose(f);
        return null;
    }
    const buf: MutStr = @ptrCast(c.malloc(@as(usize, @intCast(len)) + 1));
    if (buf == null) {
        _ = c.fclose(f);
        return null;
    }
    const nread = c.fread(buf, 1, @intCast(len), f);
    _ = c.fclose(f);
    buf[nread] = 0;

    const flg: c.yyjson_read_flag = c.YYJSON_READ_ALLOW_COMMENTS | c.YYJSON_READ_ALLOW_TRAILING_COMMAS;
    const doc = c.yyjson_read(buf, nread, flg);
    c.free(buf);
    if (doc == null) return null;
    const root = c.yyjson_doc_get_root(doc);
    const compiler_opts = c.yyjson_obj_get(root, "compilerOptions");
    if (compiler_opts == null) {
        c.yyjson_doc_free(doc);
        return null;
    }
    const base_url_val = c.yyjson_obj_get(compiler_opts, "baseUrl");
    const base_url_str: CStr = if (base_url_val != null) c.yyjson_get_str(base_url_val) else null;
    const paths_obj = c.yyjson_obj_get(compiler_opts, "paths");
    if (paths_obj == null and base_url_str == null) {
        c.yyjson_doc_free(doc);
        return null;
    }

    const map: *AliasMap = @as(?*AliasMap, interop.callocN(AliasMap, 1)) orelse {
        c.yyjson_doc_free(doc);
        return null;
    };

    if (base_url_str != null and base_url_str[0] != 0 and c.strcmp(base_url_str, ".") != 0) {
        map.base_url = resolveTargetRelative(dir_prefix, base_url_str);
    } else if (base_url_str != null and c.strcmp(base_url_str, ".") == 0 and dir_prefix != null and
        dir_prefix[0] != 0)
    {
        map.base_url = c.strdup(dir_prefix);
    }

    if (paths_obj != null and c.yyjson_is_obj(paths_obj)) {
        const obj_size = c.yyjson_obj_size(paths_obj);
        const capped = obj_size > MAX_ENTRIES;
        const capacity: c_int = @intCast(if (capped) MAX_ENTRIES else obj_size);
        if (capacity > 0) {
            map.entries = interop.callocN(Alias, @intCast(capacity));
            if (map.entries == null) {
                c.free(map.base_url);
                c.free(map);
                c.yyjson_doc_free(doc);
                return null;
            }
            var iter = c.yyjson_obj_iter_with(paths_obj);
            while (true) {
                const key = c.yyjson_obj_iter_next(&iter);
                if (key == null or map.count >= capacity) break;
                const val = c.yyjson_obj_iter_get_val(key);
                const alias_pattern: CStr = c.yyjson_get_str(key);
                if (alias_pattern == null or !c.yyjson_is_arr(val) or c.yyjson_arr_size(val) == 0) {
                    continue;
                }
                const target_pattern: CStr = c.yyjson_get_str(c.yyjson_arr_get_first(val));
                if (target_pattern == null) continue;
                const entry = util.at(map.entries, @intCast(map.count));
                const star: CStr = c.strchr(alias_pattern, '*');
                if (star != null) {
                    entry.has_wildcard = true;
                    entry.alias_prefix = c.cbm_strndup(alias_pattern, @intFromPtr(star) - @intFromPtr(alias_pattern));
                    entry.alias_suffix = c.strdup(star + 1);
                } else {
                    entry.has_wildcard = false;
                    entry.alias_prefix = c.strdup(alias_pattern);
                    entry.alias_suffix = c.strdup("");
                }
                const tstar: CStr = c.strchr(target_pattern, '*');
                if (tstar != null) {
                    const pre: MutStr = c.cbm_strndup(target_pattern, @intFromPtr(tstar) - @intFromPtr(target_pattern));
                    entry.target_prefix = resolveTargetRelative(dir_prefix, pre);
                    c.free(pre);
                    entry.target_suffix = c.strdup(tstar + 1);
                } else {
                    entry.target_prefix = resolveTargetRelative(dir_prefix, target_pattern);
                    entry.target_suffix = c.strdup("");
                }
                map.count += 1;
            }
            if (capped) {
                interop.logWarn("path_alias.entries.cap_hit", .{
                    @as(CStr, "config"), abs_path, @as(CStr, "kept"), @as(CStr, "256_of_more"),
                });
            }
            if (map.count > 1) {
                c.qsort(map.entries, @intCast(map.count), @sizeOf(Alias), cmpAliasEntryBySpecificity);
            }
        }
    }

    c.yyjson_doc_free(doc);
    return map;
}

// ── Public API ──────────────────────────────────────────────────────

pub export fn cbm_path_alias_collection_free(coll_c: ?*Collection) void {
    const coll = coll_c orelse return;
    var i: usize = 0;
    while (i < coll.count) : (i += 1) {
        c.free(coll.scopes[i].dir_prefix);
        if (coll.scopes[i].map) |map| {
            var j: usize = 0;
            while (j < map.*.count) : (j += 1) {
                c.free(map.*.entries[j].alias_prefix);
                c.free(map.*.entries[j].alias_suffix);
                c.free(map.*.entries[j].target_prefix);
                c.free(map.*.entries[j].target_suffix);
            }
            c.free(map.*.entries);
            c.free(map.*.base_url);
            c.free(map);
        }
    }
    c.free(coll.scopes);
    c.free(coll);
}

pub export fn cbm_path_alias_resolve(map_c: ?*const AliasMap, module_path: CStr) MutStr {
    const map = map_c orelse return null;
    if (module_path == null) return null;
    const mod_len = c.strlen(module_path);

    var i: usize = 0;
    while (i < map.count) : (i += 1) {
        const e = util.at(map.entries, i);

        if (e.has_wildcard) {
            const prefix_len = c.strlen(e.alias_prefix);
            const suffix_len = c.strlen(e.alias_suffix);
            if (mod_len < prefix_len + suffix_len) continue;
            if (c.strncmp(module_path, e.alias_prefix, prefix_len) != 0) continue;
            if (suffix_len > 0 and
                c.strcmp(module_path + mod_len - suffix_len, e.alias_suffix) != 0)
            {
                continue;
            }
            const wild_len = mod_len - prefix_len - suffix_len;
            const wild_start = module_path + prefix_len;
            const tp_len = c.strlen(e.target_prefix);
            const ts_len = c.strlen(e.target_suffix);
            const result: MutStr = @ptrCast(c.malloc(tp_len + wild_len + ts_len + 1));
            if (result == null) return null;
            @memcpy(result[0..tp_len], e.target_prefix[0..tp_len]);
            @memcpy(result[tp_len..][0..wild_len], wild_start[0..wild_len]);
            @memcpy(result[tp_len + wild_len ..][0..ts_len], e.target_suffix[0..ts_len]);
            result[tp_len + wild_len + ts_len] = 0;
            return stripResolvedExt(result);
        }

        if (c.strcmp(module_path, e.alias_prefix) == 0) {
            return stripResolvedExt(c.strdup(e.target_prefix));
        }
    }

    // baseUrl fallback. Apply only to non-relative imports that look
    // sub-path-ish (contain '/' but don't start with '.' or '@'); skips
    // obvious package names like "react" or "lodash".
    if (map.base_url != null and module_path[0] != '.' and module_path[0] != '@' and
        c.strchr(module_path, '/') != null)
    {
        const bu_len = c.strlen(map.base_url);
        const need = bu_len + 1 + mod_len + 1;
        const result: MutStr = @ptrCast(c.malloc(need));
        if (result == null) return null;
        _ = c.snprintf(result, need, "%s/%s", map.base_url, module_path);
        return stripResolvedExt(result);
    }
    return null;
}

// ── Repo walk ───────────────────────────────────────────────────────

const AliasConfigHit = extern struct {
    abs: [c.CBM_SZ_512]u8,
    rel: [c.CBM_SZ_256]u8,
};

const TS_CONFIG_NAMES = [_][*:0]const u8{ "tsconfig.json", "jsconfig.json" };

// NOLINT(misc-no-recursion) in the C original: bounded by MAX_DEPTH.
fn findAliasFiles(
    abs_dir: CStr,
    rel_dir: CStr,
    out: [*]AliasConfigHit,
    count: *c_int,
    max_count: c_int,
    depth: c_int,
    excluded_dirs: [*c][*c]u8,
    excluded_count: c_int,
) void {
    if (count.* >= max_count or depth > MAX_DEPTH) return;
    const d = c.cbm_opendir(abs_dir) orelse return;

    // One config file per directory: prefer tsconfig.json over jsconfig.json.
    for (TS_CONFIG_NAMES) |config_name| {
        if (count.* >= max_count) break;
        var check: [c.CBM_SZ_512]u8 = undefined;
        _ = c.snprintf(&check, check.len, "%s/%s", abs_dir, config_name);
        if (c.cbm_fopen(&check, "r")) |f| {
            _ = c.fclose(f);
            const hit = &out[@intCast(count.*)];
            _ = c.snprintf(&hit.abs, hit.abs.len, "%s", @as(CStr, &check));
            _ = c.snprintf(&hit.rel, hit.rel.len, "%s", rel_dir);
            count.* += 1;
            break;
        }
    }

    while (true) {
        const ent = c.cbm_readdir(d) orelse break;
        if (count.* >= max_count) break;
        if (!ent.*.is_dir) continue;
        const name: CStr = &ent.*.name;
        if (name[0] == '.' or c.strcmp(name, "node_modules") == 0 or c.strcmp(name, "dist") == 0 or
            c.strcmp(name, "build") == 0 or c.strcmp(name, ".next") == 0 or
            c.strcmp(name, "coverage") == 0 or c.strcmp(name, "target") == 0 // Rust
        ) {
            continue;
        }
        var child_abs: [c.CBM_SZ_512]u8 = undefined;
        var child_rel: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&child_abs, child_abs.len, "%s/%s", abs_dir, name);
        if (rel_dir[0] == 0) {
            _ = c.snprintf(&child_rel, child_rel.len, "%s", name);
        } else {
            _ = c.snprintf(&child_rel, child_rel.len, "%s/%s", rel_dir, name);
        }
        if (c.cbm_pipeline_relpath_is_excluded(&child_rel, excluded_dirs, excluded_count) != 0) continue;
        findAliasFiles(&child_abs, &child_rel, out, count, max_count, depth + 1, excluded_dirs, excluded_count);
    }
    _ = c.cbm_closedir(d);
}

pub export fn cbm_load_path_aliases_excluded(
    repo_path: CStr,
    excluded_dirs: [*c][*c]u8,
    excluded_count: c_int,
) ?*Collection {
    if (repo_path == null) return null;
    const hits: [*]AliasConfigHit = @as(?[*]AliasConfigHit, interop.callocN(AliasConfigHit, MAX_FILES)) orelse return null;
    var count: c_int = 0;
    findAliasFiles(repo_path, "", hits, &count, MAX_FILES, 0, excluded_dirs, excluded_count);
    if (count >= MAX_FILES) {
        interop.logWarn("path_alias.files.cap_hit", .{
            @as(CStr, "repo"), repo_path, @as(CStr, "kept"), @as(CStr, "256_of_more"),
        });
    }
    if (count == 0) {
        c.free(hits);
        return null;
    }

    const coll: *Collection = @as(?*Collection, interop.callocN(Collection, 1)) orelse {
        c.free(hits);
        return null;
    };
    coll.scopes = interop.callocN(Scope, @intCast(count));
    if (coll.scopes == null) {
        c.free(coll);
        c.free(hits);
        return null;
    }

    for (hits[0..@intCast(count)]) |*hit| {
        const map = loadTsconfigFile(&hit.abs, &hit.rel) orelse continue;
        const scope = util.at(coll.scopes, @intCast(coll.count));
        scope.dir_prefix = c.strdup(&hit.rel);
        scope.map = map;
        coll.count += 1;
    }
    c.free(hits);

    if (coll.count == 0) {
        c.free(coll.scopes);
        c.free(coll);
        return null;
    }

    if (coll.count > 1) {
        c.qsort(coll.scopes, @intCast(coll.count), @sizeOf(Scope), cmpScopeBySpecificity);
    }
    return coll;
}

pub export fn cbm_load_path_aliases(repo_path: CStr) ?*Collection {
    return cbm_load_path_aliases_excluded(repo_path, null, 0);
}

pub export fn cbm_path_alias_find_for_file(coll_c: ?*const Collection, rel_path: CStr) ?*const AliasMap {
    const coll = coll_c orelse return null;
    if (rel_path == null) return null;
    var i: usize = 0;
    while (i < coll.count) : (i += 1) {
        const prefix: CStr = coll.scopes[i].dir_prefix;
        const plen = c.strlen(prefix);
        if (plen == 0) return coll.scopes[i].map;
        if (c.strncmp(rel_path, prefix, plen) == 0 and
            (rel_path[plen] == '/' or rel_path[plen] == 0))
        {
            return coll.scopes[i].map;
        }
    }
    return null;
}
